#include "server/trip_queries.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "umbra/traffic.hpp"
#include "umbra/weather.hpp"

namespace umbra::server {
namespace {

const char* to_string(WalkComfort c) {
  switch (c) {
    case WalkComfort::kPleasant: return "pleasant";
    case WalkComfort::kFair: return "fair";
    case WalkComfort::kCaution: return "caution";
    case WalkComfort::kDangerous: return "dangerous";
  }
  return "fair";
}

/// Forecast sample at `time`, if live weather is on and available.
std::optional<WeatherSample> weather_at(const ApiContext& ctx, std::int64_t time) {
  if (ctx.weather == nullptr) return std::nullopt;
  const auto forecast = ctx.weather->forecast();
  return forecast ? forecast->at(time) : std::nullopt;
}

json weather_json(const WeatherSample& w, const SunPosition& sun) {
  const ComfortAssessment comfort = assess_walk_comfort(w, sun);
  return {{"time", w.time},
          {"temperatureC", round_to(w.temperature_c, 1)},
          {"feelsLikeC", round_to(w.apparent_temperature_c, 1)},
          {"cloudCoverPct", round_to(w.cloud_cover_pct, 0)},
          {"precipitationMm", round_to(w.precipitation_mm, 1)},
          {"windKmh", round_to(w.wind_speed_kmh, 1)},
          {"uvIndex", round_to(w.uv_index, 1)},
          {"directNormalWm2", round_to(w.direct_normal_wm2, 0)},
          {"code", w.weather_code},
          {"description", std::string(describe_weather_code(w.weather_code))},
          {"sunStrength", round_to(direct_sun_strength(w, sun), 2)},
          {"comfort", {{"level", to_string(comfort.level)},
                       {"headline", comfort.headline},
                       {"advice", comfort.advice}}},
          {"source", "Open-Meteo"}};
}

/// Label the route's ends with names the user picked, instead of the nearest street.
void apply_names(Route& route, const std::string& from_name, const std::string& to_name) {
  if (!from_name.empty()) route.origin.label = from_name;
  if (!to_name.empty()) {
    route.destination.label = to_name;
    if (!route.steps.empty() && route.steps.back().maneuver == "arrive") {
      route.steps.back().instruction = "Arrive at " + to_name;
    }
  }
}

json walk_route_json(const WalkRoute& walk, bool with_steps) {
  const SearchResult& s = walk.route.search;
  json runs = json::array();
  for (const ShadeRun& run : walk.runs) runs.push_back({{"shaded", run.shaded}, {"path", path_json(run.points)}});
  const double length = std::max(s.length_m, 1e-9);
  json out = {{"summary", {{"distanceM", round_to(s.length_m, 1)},
                           {"durationS", round_to(s.duration_s, 1)},
                           {"shadedM", round_to(walk.shaded_length_m, 1)},
                           {"shadePct", round_to(100.0 * walk.shade_fraction(), 1)},
                           {"litPct", round_to(100.0 * walk.lit_length_m / length, 1)},
                           {"unlitPct", round_to(100.0 * walk.unlit_length_m / length, 1)},
                           {"segments", s.edges.size()},
                           {"nodesSettled", s.settled_count},
                           {"computeMs", millis(s.elapsed)}}},
              {"geometry", path_json(walk.route.geometry)},
              {"runs", runs}};
  if (with_steps) {
    json steps = json::array();
    for (const RouteStep& step : walk.route.steps) steps.push_back(to_json(step));
    out["steps"] = steps;
  }
  return out;
}

const WalkPlanner& require_walk(const ApiContext& ctx) {
  if (ctx.walk == nullptr) throw RoutingError("Walking data is not loaded on this server");
  return *ctx.walk;
}

}  // namespace

// ------------------------------------------------------------------- search

json search_query(const ApiContext& ctx, const std::string& query, std::size_t limit) {
  if (query.size() > 200) throw BadRequest("'q' is too long");
  if (limit < 1 || limit > 20) throw BadRequest("'limit' must be 1-20");
  json results = json::array();
  for (const SearchEntry& entry : ctx.drive.search(query, limit)) {
    json item = {{"kind", to_string(entry.kind)},
                 {"name", entry.name},
                 {"subtitle", entry.subtitle},
                 {"position", to_json(entry.position)}};
    if (entry.kind == EntryKind::kBuilding) {
      item["code"] = entry.code;
      item["accessPoints"] = path_json(entry.access_points);
      item["entrancesMapped"] = entry.entrances_mapped;
    }
    results.push_back(std::move(item));
  }
  return {{"query", query}, {"results", results}};
}

// -------------------------------------------------------------------- drive

json drive_query(const ApiContext& ctx, const DriveQuery& q) {
  const RouteService& service = ctx.drive;
  const RoadGraph& graph = service.graph();
  const std::int64_t now = now_unix();
  const std::int64_t time = q.unix_time > 0 ? q.unix_time : now;
  const SunPosition sun = sun_position(graph.bounds().center(), time);

  // Conditions: live traffic per edge (only for departures within 15 minutes:
  // there is no historical traffic), forecast weather and the sun.
  DriveConditions conditions;
  constexpr std::int64_t kLiveTrafficWindowS = 15 * 60;
  if (q.live_traffic && ctx.traffic != nullptr && std::llabs(time - now) <= kLiveTrafficWindowS) {
    conditions.traffic = ctx.traffic->current();
  }
  const std::optional<WeatherSample> weather = q.live_weather ? weather_at(ctx, time) : std::nullopt;
  if (weather) conditions.weather = drive_weather_slowdown(*weather);
  conditions.glare = {q.avoid_glare, sun, q.glare_max_elevation_deg};

  const std::vector<SnappedPoint> origins = service.snap_all(q.from.points);
  const std::vector<SnappedPoint> destinations = service.snap_all(q.to.points);
  const std::vector<NodeId> sources = RouteService::node_ids(origins);
  const std::vector<NodeId> targets = RouteService::node_ids(destinations);

  const auto make_cost = [&](const DriveConditions& c) {
    return q.metric == Metric::kTime
               ? EdgeCostFn([&graph, &c](EdgeId id, const Edge& e) { return drive_cost_s(graph, id, e, c); })
               : EdgeCostFn([&graph, &c](EdgeId, const Edge& e) {
                   return c.glare.enabled && edge_into_sun(graph, e, c.glare) ? e.length_m * kGlarePenalty : e.length_m;
                 });
  };
  const EdgeCostFn cost = make_cost(conditions);
  // Conditions only slow edges down, so the free-flow heuristic stays admissible.
  const double h_per_meter = q.metric == Metric::kTime ? 1.0 / graph.max_speed_mps() : 1.0;

  SearchOptions options;
  options.algorithm = q.algorithm;
  options.record_settled = q.explored;
  Route route = service.make_route(origins, destinations,
                                   find_shortest_path_between(graph, sources, targets, cost, h_per_meter, options));
  apply_names(route, q.from.name, q.to.name);
  const SearchResult& search = route.search;

  // The other algorithm on the same endpoints, for a side-by-side comparison.
  SearchOptions other = options;
  other.algorithm = q.algorithm == Algorithm::kDijkstra ? Algorithm::kAStar : Algorithm::kDijkstra;
  other.record_settled = false;
  const SearchResult alt = find_shortest_path_between(graph, sources, targets, cost, h_per_meter, other);
  auto stats = [](const SearchResult& r) {
    return json{{"nodesSettled", r.settled_count}, {"computeMs", millis(r.elapsed)}};
  };
  json comparison;
  comparison[to_string(q.algorithm)] = stats(search);
  comparison[to_string(other.algorithm)] = stats(alt);

  // Time driven into the sun on a path (edge by edge, at the chosen threshold).
  GlareAvoidance glare_check = conditions.glare;
  glare_check.enabled = true;
  auto times = [&](const SearchResult& r) {
    std::pair<double, double> out{0.0, 0.0};  // total, into the sun
    for (EdgeId id : r.edges) {
      const double t = conditioned_duration_s(id, graph.edge(id), conditions);
      out.first += t;
      if (edge_into_sun(graph, graph.edge(id), glare_check)) out.second += t;
    }
    return out;
  };
  const auto [duration, glare_s] = times(search);

  // With glare avoidance on, also report what the plain fastest route would have been.
  json avoided = nullptr;
  if (q.avoid_glare) {
    DriveConditions plain = conditions;
    plain.glare.enabled = false;
    SearchOptions quiet = other;
    quiet.algorithm = q.algorithm;
    const auto [plain_duration, plain_glare] =
        times(find_shortest_path_between(graph, sources, targets, make_cost(plain), h_per_meter, quiet));
    avoided = {{"fastestDurationS", round_to(plain_duration, 1)}, {"fastestGlareS", round_to(plain_glare, 1)}};
  }

  // Flag directions legs that head into the sun (straight-line heading of each leg).
  json steps = json::array();
  for (std::size_t i = 0; i < route.steps.size(); ++i) {
    json step = to_json(route.steps[i]);
    if (i + 1 < route.steps.size() && route.steps[i].length_m > 0) {
      const double heading = bearing_deg(route.steps[i].location, route.steps[i + 1].location);
      step["glare"] = sun_glare(heading, sun, q.glare_max_elevation_deg);
    }
    steps.push_back(std::move(step));
  }

  json traffic = nullptr;
  if (conditions.traffic) {
    traffic = {{"updated", conditions.traffic->source_updated},
               {"matchedEdges", conditions.traffic->matched_edges},
               {"source", "HERE"}};
  }
  std::size_t stride = 1;
  const json explored = explored_json(graph, search, stride);
  return {
      {"found", search.found},
      {"mode", "drive"},
      {"metric", to_string(q.metric)},
      {"algorithm", to_string(q.algorithm)},
      {"time", time},
      {"origin", to_json(route.origin)},
      {"destination", to_json(route.destination)},
      {"summary", {{"distanceM", round_to(search.length_m, 1)},
                   {"durationS", round_to(duration, 1)},
                   {"freeFlowDurationS", round_to(search.duration_s, 1)},
                   {"segments", search.edges.size()},
                   {"nodesSettled", search.settled_count},
                   {"totalNodes", graph.node_count()},
                   {"computeMs", millis(search.elapsed)}}},
      {"conditions", {{"traffic", traffic},
                      {"weather", weather ? json{{"slowdown", conditions.weather.reason},
                                                 {"freewayFactor", round_to(conditions.weather.freeway, 3)},
                                                 {"streetFactor", round_to(conditions.weather.street, 3)},
                                                 {"description", std::string(describe_weather_code(weather->weather_code))}}
                                          : json(nullptr)},
                      {"sun", to_json(sun)},
                      {"glare", glare_s > 0},
                      {"glareS", round_to(glare_s, 1)},
                      {"avoidGlare", q.avoid_glare},
                      {"glareMaxElevationDeg", q.glare_max_elevation_deg},
                      {"glareAvoided", avoided}}},
      {"comparison", comparison},
      {"geometry", path_json(route.geometry)},
      {"steps", steps},
      {"explored", explored},
      {"exploredStride", stride},
  };
}

// --------------------------------------------------------------------- walk

json walk_query(const ApiContext& ctx, WalkQuery q) {
  const WalkPlanner& planner = require_walk(ctx);
  const SunPosition sun = planner.sun_at(q.options.unix_time);
  const std::optional<WeatherSample> weather = q.live_weather ? weather_at(ctx, q.options.unix_time) : std::nullopt;
  // Live weather scales the sun penalty: under clouds, shade matters less.
  if (weather) q.options.sun_strength = direct_sun_strength(*weather, sun);

  WalkPlan plan = q.via_water ? planner.plan_via_stop(q.from.points, q.to.points, q.options)
                              : planner.plan(q.from.points, q.to.points, q.options);
  apply_names(plan.preferred.route, q.from.name, q.to.name);
  apply_names(plan.shortest.route, q.from.name, q.to.name);

  const RoadGraph& graph = planner.network().graph();
  std::size_t stride = 1;
  const json explored = explored_json(graph, plan.preferred.route.search, stride);
  json via = nullptr;
  if (plan.via) via = {{"name", plan.via->name}, {"position", to_json(plan.via->position)}};
  return {
      {"found", plan.preferred.route.search.found},
      {"mode", "walk"},
      {"algorithm", to_string(q.options.algorithm)},
      {"preference", to_string(plan.preference)},
      {"time", q.options.unix_time},
      {"sun", to_json(plan.sun)},
      {"sunStrength", round_to(plan.sun_strength, 2)},
      {"weather", weather ? weather_json(*weather, sun) : json(nullptr)},
      {"avoidStairs", q.options.avoid_stairs},
      {"lighting", plan.lighting_applied},
      {"via", via},
      {"origin", to_json(plan.preferred.route.origin)},
      {"destination", to_json(plan.preferred.route.destination)},
      {"totalNodes", graph.node_count()},
      {"preferred", walk_route_json(plan.preferred, true)},
      {"shortest", walk_route_json(plan.shortest, false)},
      {"explored", explored},
      {"exploredStride", stride},
  };
}

// ------------------------------------------------------------------ weather

json weather_query(const ApiContext& ctx, std::int64_t time) {
  if (ctx.weather == nullptr) throw RoutingError("Live weather is disabled on this server");
  const auto sample = weather_at(ctx, time);
  if (!sample) throw RoutingError("No weather forecast is available for that time");
  return weather_json(*sample, sun_position(ctx.drive.graph().bounds().center(), time));
}

json departures_query(const ApiContext& ctx, const WalkQuery& q, std::int64_t start, std::int64_t step_s,
                      int count) {
  const WalkPlanner& planner = require_walk(ctx);
  if (count < 1 || count > 16) throw BadRequest("'count' must be 1-16");
  if (step_s < 300 || step_s > 6 * 3600) throw BadRequest("'step' must be 300-21600 seconds");

  std::vector<std::int64_t> times;
  for (int i = 0; i < count; ++i) times.push_back(start + i * step_s);
  // One forecast lookup per time, shared by the planner and the response.
  const auto forecast = ctx.weather != nullptr ? ctx.weather->forecast() : nullptr;
  auto sample_at = [&](std::int64_t t) { return forecast ? forecast->at(t) : std::nullopt; };
  auto strength_at = [&](std::int64_t t) {
    const auto w = sample_at(t);
    return w ? direct_sun_strength(*w, planner.sun_at(t)) : 1.0;
  };

  json options = json::array();
  for (const DepartureOption& d : planner.compare_departures(q.from.points, q.to.points, times, q.options, strength_at)) {
    const auto w = sample_at(d.unix_time);
    json item = {{"time", d.unix_time},
                 {"found", d.found},
                 {"distanceM", round_to(d.length_m, 1)},
                 {"durationS", round_to(d.duration_s, 1)},
                 {"shadePct", round_to(100.0 * d.shade_fraction, 1)},
                 {"sunUp", d.sun.is_up()},
                 {"sunStrength", round_to(d.sun_strength, 2)},
                 {"feelsLikeC", nullptr},
                 {"comfort", nullptr}};
    if (w) {
      item["feelsLikeC"] = round_to(w->apparent_temperature_c, 1);
      item["comfort"] = to_string(assess_walk_comfort(*w, d.sun).level);
    }
    options.push_back(std::move(item));
  }
  return {{"preference", to_string(q.options.preference)}, {"options", options}};
}

}  // namespace umbra::server
