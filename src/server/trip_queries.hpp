#pragma once

// Trip queries shared by the HTTP API and the trip assistant's tools: each
// takes parsed parameters and returns the JSON the API sends.

#include <cstdint>
#include <string>
#include <vector>

#include "server/api_common.hpp"

namespace umbra {
class TrafficService;
class UserStore;
class WeatherService;
}  // namespace umbra

namespace umbra::server {

class TripAssistant;

struct RegionInfo {
  std::string name;
  std::string timezone;  ///< IANA name, e.g. "America/Phoenix" (used by the UI's time picker)
};

/// Everything the API needs. Pointers are null when a feature is disabled.
/// Referenced objects must outlive the server.
struct ApiContext {
  const RouteService& drive;
  const WalkPlanner* walk = nullptr;
  const WeatherService* weather = nullptr;
  const TrafficService* traffic = nullptr;
  UserStore* users = nullptr;
  const TripAssistant* assistant = nullptr;
  RegionInfo region;
  bool https = false;        ///< mark session cookies Secure
  bool trust_proxy = false;  ///< client IP from X-Forwarded-For
};

/// One end of a trip: a point or several alternatives (a building's entrances).
struct TripEnd {
  std::vector<LatLng> points;
  std::string name;  ///< optional label, e.g. "Hayden Library"
};

struct DriveQuery {
  TripEnd from;
  TripEnd to;
  Metric metric = Metric::kTime;
  Algorithm algorithm = Algorithm::kDijkstra;
  bool explored = true;
  bool live_traffic = true;
  bool live_weather = true;
  std::int64_t unix_time = 0;  ///< departure; 0 = now
  bool avoid_glare = false;    ///< steer away from roads into a low sun
  double glare_max_elevation_deg = kGlareMaxElevationDeg;
};

struct WalkQuery {
  TripEnd from;
  TripEnd to;
  WalkOptions options;
  bool live_weather = true;
  bool via_water = false;
};

[[nodiscard]] json search_query(const ApiContext& ctx, const std::string& query, std::size_t limit);
[[nodiscard]] json drive_query(const ApiContext& ctx, const DriveQuery& query);
[[nodiscard]] json walk_query(const ApiContext& ctx, WalkQuery query);

/// Weather at `time` with sun strength and walk comfort; throws RoutingError if unavailable.
[[nodiscard]] json weather_query(const ApiContext& ctx, std::int64_t time);

/// The walk at `count` departure times from `start`, `step_s` apart (count <= 16).
[[nodiscard]] json departures_query(const ApiContext& ctx, const WalkQuery& query, std::int64_t start,
                                    std::int64_t step_s, int count);

}  // namespace umbra::server
