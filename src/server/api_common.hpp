#pragma once

// Shared pieces of the JSON API: error types, parameter parsing and JSON
// serialisation of core types. Used by the HTTP handlers and the assistant's tools.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "umbra/route_service.hpp"
#include "umbra/solar.hpp"
#include "umbra/walk_planner.hpp"

namespace httplib {
struct Request;
struct Response;
}  // namespace httplib

namespace umbra::server {

using json = nlohmann::json;

/// The client sent invalid parameters (HTTP 400).
class BadRequest : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The client must sign in first (HTTP 401).
class Unauthorized : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The client is over a rate limit (HTTP 429).
class TooManyRequests : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::size_t kMaxEndpointPoints = 32;
inline constexpr std::size_t kMaxNameLength = 120;

// ----------------------------------------------------------------- parsing

[[nodiscard]] std::optional<double> parse_double(const std::string& text);

/// One or more "lat,lng" points separated by ';' (at most kMaxEndpointPoints).
[[nodiscard]] std::vector<LatLng> parse_points(const std::string& raw, const std::string& what);

/// Unix seconds within 1970..2200 (the solar formulas' valid range).
[[nodiscard]] std::int64_t parse_unix_time(const std::string& raw);

[[nodiscard]] std::int64_t now_unix();
[[nodiscard]] Algorithm parse_algorithm(const std::string& value);
[[nodiscard]] Metric parse_metric(const std::string& value);
[[nodiscard]] ShadePreference parse_shade(const std::string& value);

/// Optional display name for a trip end, at most kMaxNameLength bytes.
[[nodiscard]] std::string checked_name(const std::string& value, const std::string& what);

/// Query-string helpers: the value of `key`, or `fallback` when absent.
[[nodiscard]] std::string param(const httplib::Request& req, const char* key, const std::string& fallback = "");
[[nodiscard]] bool flag_param(const httplib::Request& req, const char* key, bool fallback);

// ------------------------------------------------------------ serialisation

/// Round to `digits` decimals so JSON prints e.g. 104.6, not 104.60000000000001.
[[nodiscard]] double round_to(double value, int digits);
[[nodiscard]] double millis(std::chrono::microseconds us);

[[nodiscard]] json to_json(const LatLng& p);
[[nodiscard]] json to_json(const SnappedPoint& p);
[[nodiscard]] json to_json(const RouteStep& s);
[[nodiscard]] json to_json(const SunPosition& sun);
[[nodiscard]] json path_json(const std::vector<LatLng>& points);

[[nodiscard]] const char* to_string(Metric m);
[[nodiscard]] const char* to_string(Algorithm a);
[[nodiscard]] const char* to_string(EntryKind k);
[[nodiscard]] const char* to_string(ShadePreference p);

/// Settled nodes for the search animation, thinned to at most 8,000 points.
[[nodiscard]] json explored_json(const RoadGraph& graph, const SearchResult& search, std::size_t& stride);

// ---------------------------------------------------------------- responses

void send_json(httplib::Response& res, const json& body, int status = 200);
void send_error(httplib::Response& res, int status, const std::string& message);

using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

/// Wrap a handler so domain errors become clean JSON responses
/// (400 BadRequest, 401 Unauthorized, 422 RoutingError / AccountError, 429 TooManyRequests).
[[nodiscard]] Handler guarded(Handler handler);

}  // namespace umbra::server
