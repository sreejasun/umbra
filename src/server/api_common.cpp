#include "server/api_common.hpp"

#include <cmath>
#include <cstdlib>

#include <httplib.h>

#include "umbra/shade_map.hpp"
#include "umbra/user_store.hpp"

namespace umbra::server {

namespace {
constexpr std::size_t kMaxExploredPoints = 8000;
}  // namespace

// ------------------------------------------------------------------ parsing

std::optional<double> parse_double(const std::string& text) {
  if (text.empty()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || !std::isfinite(value)) return std::nullopt;
  return value;
}

std::vector<LatLng> parse_points(const std::string& raw, const std::string& what) {
  const std::string format = "'" + what + "' must be 'lat,lng' or 'lat,lng;lat,lng;...'";
  std::vector<LatLng> points;
  std::size_t start = 0;
  while (start <= raw.size()) {
    const std::size_t end = std::min(raw.find(';', start), raw.size());
    const std::string item = raw.substr(start, end - start);
    const auto comma = item.find(',');
    if (comma == std::string::npos) throw BadRequest(format);
    const auto lat = parse_double(item.substr(0, comma));
    const auto lng = parse_double(item.substr(comma + 1));
    if (!lat || !lng) throw BadRequest(format);
    points.push_back({*lat, *lng});
    if (points.size() > kMaxEndpointPoints) {
      throw BadRequest("'" + what + "' accepts at most " + std::to_string(kMaxEndpointPoints) + " points");
    }
    start = end + 1;
  }
  return points;
}

std::int64_t parse_unix_time(const std::string& raw) {
  const auto t = parse_double(raw);
  if (!t || *t < 0 || *t > 7.3e9) throw BadRequest("'time' must be a Unix timestamp in seconds");
  return static_cast<std::int64_t>(*t);
}

std::int64_t now_unix() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Algorithm parse_algorithm(const std::string& v) {
  if (v.empty() || v == "dijkstra") return Algorithm::kDijkstra;
  if (v == "astar") return Algorithm::kAStar;
  throw BadRequest("'algorithm' must be 'dijkstra' or 'astar'");
}

Metric parse_metric(const std::string& v) {
  if (v.empty() || v == "time") return Metric::kTime;
  if (v == "distance") return Metric::kDistance;
  throw BadRequest("'metric' must be 'time' or 'distance'");
}

ShadePreference parse_shade(const std::string& v) {
  if (v.empty() || v == "balanced") return ShadePreference::kBalanced;
  if (v == "shortest") return ShadePreference::kShortest;
  if (v == "max") return ShadePreference::kMaxShade;
  throw BadRequest("'shade' must be 'shortest', 'balanced' or 'max'");
}

std::string checked_name(const std::string& value, const std::string& what) {
  if (value.size() > kMaxNameLength) throw BadRequest("'" + what + "' is too long");
  return value;
}

std::string param(const httplib::Request& req, const char* key, const std::string& fallback) {
  return req.has_param(key) ? req.get_param_value(key) : fallback;
}

bool flag_param(const httplib::Request& req, const char* key, bool fallback) {
  if (!req.has_param(key)) return fallback;
  const std::string v = req.get_param_value(key);
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

// ------------------------------------------------------------ serialisation

double round_to(double value, int digits) {
  // Dividing by an exact power of ten yields the closest double.
  const double scale = std::pow(10.0, digits);
  return std::round(value * scale) / scale;
}

double millis(std::chrono::microseconds us) { return round_to(static_cast<double>(us.count()) / 1000.0, 3); }

json to_json(const LatLng& p) { return json::array({round_to(p.lat, 6), round_to(p.lng, 6)}); }

json to_json(const SnappedPoint& p) {
  return {{"node", p.node},
          {"position", to_json(p.position)},
          {"label", p.label},
          {"snapDistanceM", round_to(p.snap_distance_m, 1)}};
}

json to_json(const RouteStep& s) {
  return {{"instruction", s.instruction},
          {"maneuver", s.maneuver},
          {"street", s.street},
          {"distanceM", round_to(s.length_m, 1)},
          {"durationS", round_to(s.duration_s, 1)},
          {"location", to_json(s.location)}};
}

json to_json(const SunPosition& sun) {
  static constexpr const char* kDirections[] = {"north", "northeast", "east", "southeast",
                                                "south", "southwest", "west", "northwest"};
  const auto index = static_cast<int>(std::lround(sun.azimuth_deg / 45.0)) % 8;
  return {{"azimuthDeg", round_to(sun.azimuth_deg, 1)},
          {"elevationDeg", round_to(sun.elevation_deg, 1)},
          {"up", sun.elevation_deg >= ShadeMap::kMinSunElevationDeg},
          {"direction", kDirections[index]}};
}

json path_json(const std::vector<LatLng>& points) {
  json out = json::array();
  for (const LatLng& p : points) out.push_back(to_json(p));
  return out;
}

const char* to_string(Metric m) { return m == Metric::kTime ? "time" : "distance"; }
const char* to_string(Algorithm a) { return a == Algorithm::kAStar ? "astar" : "dijkstra"; }

const char* to_string(EntryKind k) {
  switch (k) {
    case EntryKind::kPlace: return "place";
    case EntryKind::kBuilding: return "building";
    case EntryKind::kStreet: return "street";
    case EntryKind::kIntersection: return "intersection";
  }
  return "place";
}

const char* to_string(ShadePreference p) {
  switch (p) {
    case ShadePreference::kShortest: return "shortest";
    case ShadePreference::kBalanced: return "balanced";
    case ShadePreference::kMaxShade: return "max";
  }
  return "balanced";
}

json explored_json(const RoadGraph& graph, const SearchResult& search, std::size_t& stride) {
  json explored = json::array();
  const std::size_t total = search.settled_order.size();
  stride = total > kMaxExploredPoints ? (total + kMaxExploredPoints - 1) / kMaxExploredPoints : 1;
  for (std::size_t i = 0; i < total; i += stride) {
    explored.push_back(to_json(graph.node(search.settled_order[i]).position));
  }
  return explored;
}

// ---------------------------------------------------------------- responses

void send_json(httplib::Response& res, const json& body, int status) {
  res.status = status;
  res.set_header("Cache-Control", "no-store");
  res.set_content(body.dump(), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& message) {
  send_json(res, {{"error", message}}, status);
}

Handler guarded(Handler handler) {
  return [handler = std::move(handler)](const httplib::Request& req, httplib::Response& res) {
    try {
      handler(req, res);
    } catch (const BadRequest& e) {
      send_error(res, 400, e.what());
    } catch (const Unauthorized& e) {
      send_error(res, 401, e.what());
    } catch (const TooManyRequests& e) {
      res.set_header("Retry-After", "60");
      send_error(res, 429, e.what());
    } catch (const RoutingError& e) {
      send_error(res, 422, e.what());
    } catch (const AccountError& e) {
      send_error(res, 422, e.what());
    }
  };
}

}  // namespace umbra::server
