#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/cached_resource.hpp"
#include "umbra/http_types.hpp"
#include "umbra/road_graph.hpp"
#include "umbra/solar.hpp"
#include "umbra/weather.hpp"

namespace umbra {

/// Traffic data could not be fetched or parsed.
class TrafficError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Current flow on one stretch of road, as reported by the traffic provider.
struct TrafficFlow {
  std::vector<LatLng> shape;   ///< polyline of the stretch
  double speed_mps = 0.0;      ///< current speed (capped at the speed limit)
  double free_flow_mps = 0.0;  ///< typical speed without congestion
  double jam_factor = 0.0;     ///< 0 = free flow ... 10 = road closed
  double confidence = 0.0;     ///< > 0.7 means real-time measurements
};

struct TrafficSnapshot {
  std::string source_updated;  ///< provider timestamp (RFC 3339)
  std::vector<TrafficFlow> flows;
};

/// Parse a HERE Traffic API v7 `/flow` response requested with
/// `locationReferencing=shape`. Throws TrafficError.
[[nodiscard]] TrafficSnapshot parse_here_flow(std::string_view json_text);

/// HERE Traffic API v7 flow URL for a bounding box.
[[nodiscard]] std::string here_flow_url(std::string_view base_url, const BoundingBox& box, std::string_view api_key);

/// Live traffic mapped onto the road graph.
struct TrafficMatch {
  /// Per edge id: current speed / free-flow speed in (0, 1]. 1 where no data matched.
  std::vector<float> speed_factor;
  std::size_t matched_edges = 0;
  std::string source_updated;
};

/**
 * Map provider road stretches onto graph edges: an edge takes the flow of the
 * nearest stretch whose polyline passes within `max_distance_m` of the edge's
 * midpoint, running in the same direction (within `max_bearing_deg`), so the
 * two carriageways of a divided road get their own speeds. Stretches are
 * bucketed in a uniform grid, so matching is O(E · stretches per cell).
 */
[[nodiscard]] TrafficMatch match_traffic(const RoadGraph& graph, const TrafficSnapshot& snapshot,
                                         double max_distance_m = 15.0, double max_bearing_deg = 30.0);

struct TrafficServiceOptions {
  std::string base_url = "https://data.traffic.hereapi.com/v7/flow";
  /// Flow refreshes every 15 min: about 2,900 requests a month for the whole map.
  CachePolicy cache{std::chrono::minutes(15), std::chrono::minutes(2), std::chrono::minutes(45)};
};

/// Live traffic for the whole map, fetched and matched on demand and cached.
class TrafficService {
 public:
  using Clock = CachedResource<TrafficMatch>::Clock;
  using NowFn = CachedResource<TrafficMatch>::NowFn;

  /// `graph` must outlive the service. `api_key` is the provider's key.
  TrafficService(const RoadGraph& graph, std::string api_key, HttpGet get, TrafficServiceOptions options = {},
                 NowFn now = Clock::now);

  /// Current per-edge speed factors, or null if traffic is unavailable.
  [[nodiscard]] std::shared_ptr<const TrafficMatch> current() const { return cache_.get(); }

 private:
  CachedResource<TrafficMatch> cache_;
};

/// Steer drives away from roads that head into a low sun.
struct GlareAvoidance {
  bool enabled = false;
  SunPosition sun;
  double max_elevation_deg = kGlareMaxElevationDeg;  ///< kGlareMaxElevationLowSeatDeg if the visor doesn't help
};

/// A minute driven into the sun costs this many minutes when avoiding glare.
inline constexpr double kGlarePenalty = 3.0;

/// Everything that affects a drive.
struct DriveConditions {
  std::shared_ptr<const TrafficMatch> traffic;  ///< null = free flow
  WeatherSlowdown weather;
  GlareAvoidance glare;
};

/// Roads at or above this free-flow speed count as freeways (about 50 mph).
inline constexpr double kFreewaySpeedMps = 22.0;

/// Travel time of an edge under `conditions` (>= its free-flow time, so the A*
/// time heuristic stays admissible).
[[nodiscard]] double conditioned_duration_s(EdgeId id, const Edge& edge, const DriveConditions& conditions);

/// Is this edge driven straight into the sun under `glare` (its sun position and threshold)?
[[nodiscard]] bool edge_into_sun(const RoadGraph& graph, const Edge& edge, const GlareAvoidance& glare);

/**
 * Cost the fastest-route search minimises: the conditioned travel time, times
 * kGlarePenalty on edges into the sun when glare avoidance is on. Never less
 * than the free-flow time, so the A* heuristic stays admissible.
 */
[[nodiscard]] double drive_cost_s(const RoadGraph& graph, EdgeId id, const Edge& edge,
                                  const DriveConditions& conditions);

}  // namespace umbra
