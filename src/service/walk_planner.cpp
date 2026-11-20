#include "umbra/walk_planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace umbra {

double sun_penalty(ShadePreference preference) noexcept {
  switch (preference) {
    case ShadePreference::kShortest: return 0.0;
    case ShadePreference::kBalanced: return 1.5;
    case ShadePreference::kMaxShade: return 5.0;
  }
  return 0.0;
}

WalkPlanner::WalkPlanner(const RouteService& network, std::shared_ptr<const ShadeMap> shade,
                         std::vector<WalkStop> stops)
    : network_(network), shade_(std::move(shade)) {
  if (!shade_) throw std::invalid_argument("WalkPlanner: shade map is required");
  for (WalkStop& stop : stops) {
    try {
      const SnappedPoint p = network_.snap(stop.position);
      if (p.snap_distance_m > kMaxStopDistanceM) continue;  // not reachable on foot from a mapped path
      stop_nodes_.push_back(p.node);
      stops_.push_back(std::move(stop));
    } catch (const RoutingError&) {
      // off the map
    }
  }
}

SunPosition WalkPlanner::sun_at(std::int64_t unix_time) const {
  return sun_position(network_.graph().bounds().center(), unix_time);
}

/// Edge costs for one request. `preferred` adds the sun (day) or lighting
/// (night) preference; `shortest` is plain length. Both respect avoid_stairs.
struct WalkPlanner::Costs {
  EdgeCostFn preferred;
  EdgeCostFn shortest;
  bool same = false;  ///< preferred == shortest: one search is enough
  bool lighting = false;
};

WalkPlanner::Costs WalkPlanner::make_costs(const WalkOptions& options, const SunPosition& sun,
                                           double sun_strength) const {
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  const bool avoid_stairs = options.avoid_stairs;
  Costs costs;
  costs.shortest = [avoid_stairs](EdgeId, const Edge& e) {
    return avoid_stairs && e.has(kEdgeStairs) ? kInfinity : e.length_m;
  };

  costs.lighting = !sun.is_up() && options.prefer_lit;
  const double penalty = sun.is_up() ? sun_penalty(options.preference) * sun_strength : 0.0;
  costs.same = penalty == 0.0 && !costs.lighting;
  if (costs.same) {
    costs.preferred = costs.shortest;
    return costs;
  }

  // Sun exposure of each directed edge, memoised for this request and computed on first use.
  auto exposure = std::make_shared<std::vector<float>>(network_.graph().edge_count(), -1.0F);
  const RoadGraph& graph = network_.graph();
  const ShadeMap& shade = *shade_;
  const bool lighting = costs.lighting;
  costs.preferred = [=, &graph, &shade](EdgeId id, const Edge& e) {
    if (avoid_stairs && e.has(kEdgeStairs)) return kInfinity;
    double factor = 1.0;
    if (lighting) {
      factor = e.has(kEdgeLit) ? 1.0 : e.has(kEdgeUnlit) ? kUnlitPenalty : kUnknownLightingPenalty;
    }
    if (penalty > 0.0) {
      float& x = (*exposure)[id];
      if (x < 0) {
        x = e.covered() ? 0.0F
                        : static_cast<float>(shade.sun_exposure(graph.node(e.source).position,
                                                                graph.node(e.target).position, sun));
      }
      factor *= 1.0 + penalty * static_cast<double>(x);
    }
    return e.length_m * factor;
  };
  return costs;
}

WalkPlan WalkPlanner::plan(const LatLng& from, const LatLng& to, const WalkOptions& options) const {
  return plan(std::span(&from, 1), std::span(&to, 1), options);
}

WalkPlan WalkPlanner::plan(std::span<const LatLng> from, std::span<const LatLng> to,
                           const WalkOptions& options) const {
  const RoadGraph& graph = network_.graph();
  const std::vector<SnappedPoint> origins = network_.snap_all(from);
  const std::vector<SnappedPoint> destinations = network_.snap_all(to);
  const std::vector<NodeId> sources = RouteService::node_ids(origins);
  const std::vector<NodeId> targets = RouteService::node_ids(destinations);

  WalkPlan plan;
  plan.sun = sun_at(options.unix_time);
  plan.preference = options.preference;
  plan.sun_strength = std::isfinite(options.sun_strength) ? std::clamp(options.sun_strength, 0.0, 1.0) : 1.0;
  const Costs costs = make_costs(options, plan.sun, plan.sun_strength);
  plan.lighting_applied = costs.lighting;

  SearchOptions search_options;
  search_options.algorithm = options.algorithm;
  search_options.record_settled = options.record_settled;
  SearchResult preferred = find_shortest_path_between(graph, sources, targets, costs.preferred, 1.0, search_options);
  plan.preferred = describe(network_.make_route(origins, destinations, std::move(preferred)), plan.sun);
  if (costs.same) {
    plan.shortest = plan.preferred;
    plan.shortest.route.search.settled_order.clear();
    return plan;
  }
  search_options.record_settled = false;
  SearchResult shortest = find_shortest_path_between(graph, sources, targets, costs.shortest, 1.0, search_options);
  plan.shortest = describe(network_.make_route(origins, destinations, std::move(shortest)), plan.sun);
  return plan;
}

WalkPlan WalkPlanner::plan_via_stop(std::span<const LatLng> from, std::span<const LatLng> to,
                                    const WalkOptions& options) const {
  if (stops_.empty()) throw RoutingError("No drinking-water stops are loaded on this server");
  const RoadGraph& graph = network_.graph();
  const std::vector<SnappedPoint> origins = network_.snap_all(from);
  const std::vector<SnappedPoint> destinations = network_.snap_all(to);
  const std::vector<NodeId> sources = RouteService::node_ids(origins);
  const std::vector<NodeId> targets = RouteService::node_ids(destinations);

  // The direct plan provides the sun, the comparison route and the cost settings.
  WalkPlan plan = this->plan(from, to, options);
  const Costs costs = make_costs(options, plan.sun, plan.sun_strength);

  const std::vector<double> to_stop = shortest_costs_from(graph, sources, costs.preferred);
  const std::vector<double> from_stop = shortest_costs_from(graph, targets, costs.preferred);
  std::size_t best = stops_.size();
  double best_cost = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < stops_.size(); ++i) {
    const double c = to_stop[stop_nodes_[i]] + from_stop[stop_nodes_[i]];
    if (c < best_cost) {
      best_cost = c;
      best = i;
    }
  }
  if (best == stops_.size()) throw RoutingError("No drinking-water stop can be reached on this walk");

  const NodeId stop = stop_nodes_[best];
  SearchOptions search_options;
  search_options.algorithm = options.algorithm;
  search_options.record_settled = options.record_settled;
  SearchResult first = find_shortest_path_between(graph, sources, std::span(&stop, 1), costs.preferred, 1.0, search_options);
  SearchResult second = find_shortest_path_between(graph, std::span(&stop, 1), targets, costs.preferred, 1.0, search_options);

  // Join the legs into one route, with a waypoint step at the stop.
  SnappedPoint stop_point{stop, graph.node(stop).position, 0.0, stops_[best].name};
  Route leg1 = network_.make_route(origins, std::span(&stop_point, 1), std::move(first));
  Route leg2 = network_.make_route(std::span(&stop_point, 1), destinations, std::move(second));
  Route joined = std::move(leg1);
  if (!joined.steps.empty() && joined.steps.back().maneuver == "arrive") joined.steps.pop_back();
  joined.steps.push_back({"Stop for water at " + stops_[best].name, "waypoint", "", 0.0, 0.0, stop_point.position});
  joined.steps.insert(joined.steps.end(), leg2.steps.begin(), leg2.steps.end());
  joined.destination = leg2.destination;
  if (!leg2.geometry.empty()) joined.geometry.insert(joined.geometry.end(), leg2.geometry.begin() + 1, leg2.geometry.end());
  SearchResult& s = joined.search;
  const SearchResult& t = leg2.search;
  s.found = s.found && t.found;
  s.cost += t.cost;
  s.length_m += t.length_m;
  s.duration_s += t.duration_s;
  s.edges.insert(s.edges.end(), t.edges.begin(), t.edges.end());
  if (!t.nodes.empty()) s.nodes.insert(s.nodes.end(), t.nodes.begin() + 1, t.nodes.end());
  s.settled_count += t.settled_count;
  s.settled_order.insert(s.settled_order.end(), t.settled_order.begin(), t.settled_order.end());
  s.elapsed += t.elapsed;

  plan.preferred = describe(std::move(joined), plan.sun);
  plan.via = stops_[best];
  return plan;
}

std::vector<DepartureOption> WalkPlanner::compare_departures(
    std::span<const LatLng> from, std::span<const LatLng> to, std::span<const std::int64_t> times,
    const WalkOptions& options, const std::function<double(std::int64_t)>& sun_strength_at) const {
  std::vector<DepartureOption> out;
  out.reserve(times.size());
  for (const std::int64_t t : times) {
    WalkOptions at = options;
    at.unix_time = t;
    at.record_settled = false;
    at.sun_strength = sun_strength_at ? sun_strength_at(t) : 1.0;
    const WalkPlan p = plan(from, to, at);
    const SearchResult& r = p.preferred.route.search;
    out.push_back({t, p.sun, p.sun_strength, r.found, r.length_m, r.duration_s, p.preferred.shade_fraction()});
  }
  return out;
}

WalkRoute WalkPlanner::describe(Route route, const SunPosition& sun) const {
  WalkRoute walk;
  const RoadGraph& graph = network_.graph();

  auto append = [&walk](bool shaded, LatLng a, LatLng b) {
    if (walk.runs.empty() || walk.runs.back().shaded != shaded) {
      walk.runs.push_back({shaded, {a}});
    }
    walk.runs.back().points.push_back(b);
  };

  // Split every edge into ~5 m pieces and classify each piece at its midpoint.
  for (EdgeId id : route.search.edges) {
    const Edge& e = graph.edge(id);
    const LatLng a = graph.node(e.source).position;
    const LatLng b = graph.node(e.target).position;
    const auto pieces = std::max<long>(1, static_cast<long>(std::ceil(e.length_m / ShadeMap::kSampleSpacingM)));
    for (long i = 0; i < pieces; ++i) {
      const double t0 = static_cast<double>(i) / static_cast<double>(pieces);
      const double t1 = static_cast<double>(i + 1) / static_cast<double>(pieces);
      const LatLng p0{a.lat + (b.lat - a.lat) * t0, a.lng + (b.lng - a.lng) * t0};
      const LatLng p1{a.lat + (b.lat - a.lat) * t1, a.lng + (b.lng - a.lng) * t1};
      const LatLng mid{(p0.lat + p1.lat) / 2, (p0.lng + p1.lng) / 2};
      const bool shaded = e.covered() || shade_->is_shaded(mid, sun);
      if (shaded) walk.shaded_length_m += e.length_m / static_cast<double>(pieces);
      append(shaded, p0, p1);
    }
    if (e.has(kEdgeLit)) walk.lit_length_m += e.length_m;
    if (e.has(kEdgeUnlit)) walk.unlit_length_m += e.length_m;
  }
  walk.route = std::move(route);
  return walk;
}

}  // namespace umbra
