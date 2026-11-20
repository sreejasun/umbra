#include "umbra/traffic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace umbra {
namespace {

using json = nlohmann::json;

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kMinFactor = 0.05;  // a reported standstill still lets the route through, slowly

/// Local equirectangular projection to metres around a reference latitude.
struct Projection {
  double m_lat;
  double m_lng;
  explicit Projection(double ref_lat)
      : m_lat(kEarthRadiusM * kDegToRad), m_lng(kEarthRadiusM * kDegToRad * std::cos(ref_lat * kDegToRad)) {}
  [[nodiscard]] std::pair<double, double> operator()(const LatLng& p) const { return {p.lng * m_lng, p.lat * m_lat}; }
};

double point_segment_distance(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax;
  const double dy = by - ay;
  const double len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

}  // namespace

// ------------------------------------------------------------------ parsing

TrafficSnapshot parse_here_flow(std::string_view json_text) {
  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw TrafficError(std::string("Invalid traffic JSON: ") + e.what());
  }
  if (!doc.is_object() || !doc.contains("results") || !doc["results"].is_array()) {
    throw TrafficError("Traffic response has no 'results' array");
  }

  TrafficSnapshot snapshot;
  snapshot.source_updated = doc.value("sourceUpdated", std::string{});
  try {
    for (const json& result : doc["results"]) {
      if (!result.contains("currentFlow") || !result.contains("location")) continue;
      const json& flow = result["currentFlow"];
      const double free_flow = flow.value("freeFlow", 0.0);
      if (free_flow <= 0.0) continue;

      TrafficFlow out;
      out.free_flow_mps = free_flow;
      out.jam_factor = flow.value("jamFactor", 0.0);
      out.speed_mps = flow.value("speed", free_flow);
      out.confidence = flow.value("confidence", 0.0);
      // A closed road (jam factor 10) may report no speed at all.
      if (out.jam_factor >= 10.0) out.speed_mps = 0.0;

      const json& links = result["location"].value("shape", json::object()).value("links", json::array());
      for (const json& link : links) {
        for (const json& p : link.value("points", json::array())) {
          out.shape.push_back({p.at("lat").get<double>(), p.at("lng").get<double>()});
        }
      }
      if (out.shape.size() >= 2) snapshot.flows.push_back(std::move(out));
    }
  } catch (const json::exception& e) {
    throw TrafficError(std::string("Malformed traffic data: ") + e.what());
  }
  return snapshot;
}

std::string here_flow_url(std::string_view base_url, const BoundingBox& box, std::string_view api_key) {
  char bbox[128];
  // HERE expects west,south,east,north.
  std::snprintf(bbox, sizeof bbox, "bbox:%.5f,%.5f,%.5f,%.5f", box.min_lng, box.min_lat, box.max_lng, box.max_lat);
  std::string url(base_url);
  url += "?locationReferencing=shape&in=";
  url += bbox;
  url += "&apiKey=";
  url += api_key;
  return url;
}

// ----------------------------------------------------------------- matching

TrafficMatch match_traffic(const RoadGraph& graph, const TrafficSnapshot& snapshot, double max_distance_m,
                           double max_bearing_deg) {
  TrafficMatch match;
  match.source_updated = snapshot.source_updated;
  match.speed_factor.assign(graph.edge_count(), 1.0F);
  if (snapshot.flows.empty() || graph.edge_count() == 0) return match;

  const Projection project(graph.bounds().center().lat);
  const double cell = std::max(2.0 * max_distance_m, 40.0);
  auto cell_of = [cell](double x, double y) {
    return std::pair{static_cast<std::int64_t>(std::floor(x / cell)), static_cast<std::int64_t>(std::floor(y / cell))};
  };
  auto key = [](std::int64_t cx, std::int64_t cy) { return (cx << 32) ^ (cy & 0xffffffff); };

  // Grid of provider segments: (flow index, segment index), registered in
  // every cell their bounding box touches.
  std::unordered_map<std::int64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> grid;
  for (std::uint32_t f = 0; f < snapshot.flows.size(); ++f) {
    const auto& shape = snapshot.flows[f].shape;
    for (std::uint32_t s = 0; s + 1 < shape.size(); ++s) {
      const auto [ax, ay] = project(shape[s]);
      const auto [bx, by] = project(shape[s + 1]);
      const auto [c0x, c0y] = cell_of(std::min(ax, bx), std::min(ay, by));
      const auto [c1x, c1y] = cell_of(std::max(ax, bx), std::max(ay, by));
      for (auto cx = c0x; cx <= c1x; ++cx) {
        for (auto cy = c0y; cy <= c1y; ++cy) grid[key(cx, cy)].push_back({f, s});
      }
    }
  }

  for (EdgeId id = 0; id < graph.edge_count(); ++id) {
    const Edge& e = graph.edge(id);
    const LatLng a = graph.node(e.source).position;
    const LatLng b = graph.node(e.target).position;
    const LatLng mid{(a.lat + b.lat) / 2, (a.lng + b.lng) / 2};
    const double heading = bearing_deg(a, b);
    const auto [mx, my] = project(mid);
    const auto [cx, cy] = cell_of(mx, my);

    double best = max_distance_m;
    const TrafficFlow* best_flow = nullptr;
    for (auto dx = -1; dx <= 1; ++dx) {
      for (auto dy = -1; dy <= 1; ++dy) {
        const auto it = grid.find(key(cx + dx, cy + dy));
        if (it == grid.end()) continue;
        for (const auto& [f, s] : it->second) {
          const auto& shape = snapshot.flows[f].shape;
          const double segment_heading = bearing_deg(shape[s], shape[s + 1]);
          if (std::abs(bearing_delta_deg(heading, segment_heading)) > max_bearing_deg) continue;
          const auto [ax, ay] = project(shape[s]);
          const auto [bx, by] = project(shape[s + 1]);
          const double d = point_segment_distance(mx, my, ax, ay, bx, by);
          if (d <= best) {
            best = d;
            best_flow = &snapshot.flows[f];
          }
        }
      }
    }
    if (best_flow == nullptr) continue;
    const double factor = std::clamp(best_flow->speed_mps / best_flow->free_flow_mps, kMinFactor, 1.0);
    match.speed_factor[id] = static_cast<float>(factor);
    ++match.matched_edges;
  }
  return match;
}

TrafficService::TrafficService(const RoadGraph& graph, std::string api_key, HttpGet get,
                               TrafficServiceOptions options, NowFn now)
    : cache_(
          [&graph, get = std::move(get), url = here_flow_url(options.base_url, graph.bounds(), api_key)] {
            if (!get) throw TrafficError("TrafficService: no HTTP client");
            return match_traffic(graph, parse_here_flow(get(url)));
          },
          options.cache, std::move(now)) {}

// ---------------------------------------------------------------- conditions

double conditioned_duration_s(EdgeId id, const Edge& edge, const DriveConditions& conditions) {
  double factor = 1.0;
  if (conditions.traffic && id < conditions.traffic->speed_factor.size()) {
    factor *= conditions.traffic->speed_factor[id];
  }
  const bool freeway = edge.duration_s > 0 && edge.length_m / edge.duration_s >= kFreewaySpeedMps;
  factor *= freeway ? conditions.weather.freeway : conditions.weather.street;
  return edge.duration_s / std::max(factor, kMinFactor);
}

bool edge_into_sun(const RoadGraph& graph, const Edge& edge, const GlareAvoidance& glare) {
  const double heading = bearing_deg(graph.node(edge.source).position, graph.node(edge.target).position);
  return sun_glare(heading, glare.sun, glare.max_elevation_deg);
}

double drive_cost_s(const RoadGraph& graph, EdgeId id, const Edge& edge, const DriveConditions& conditions) {
  const double time = conditioned_duration_s(id, edge, conditions);
  return conditions.glare.enabled && edge_into_sun(graph, edge, conditions.glare) ? time * kGlarePenalty : time;
}

}  // namespace umbra
