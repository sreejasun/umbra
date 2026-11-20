#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/cached_resource.hpp"
#include "umbra/geo.hpp"
#include "umbra/http_types.hpp"
#include "umbra/solar.hpp"

namespace umbra {

/// Weather data could not be fetched or parsed.
class WeatherError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Weather at one instant (one hourly forecast step, or an interpolation between two).
struct WeatherSample {
  std::int64_t time = 0;               ///< UTC seconds
  double temperature_c = 0.0;          ///< air temperature at 2 m
  double apparent_temperature_c = 0.0; ///< "feels like": temperature with humidity, wind and sun
  double cloud_cover_pct = 0.0;        ///< total cloud cover, 0-100
  double precipitation_mm = 0.0;       ///< rain/snow in the preceding hour
  double wind_speed_kmh = 0.0;         ///< at 10 m
  double uv_index = 0.0;
  double direct_normal_wm2 = 0.0;      ///< direct normal irradiance (the beam that casts shadows)
  int weather_code = 0;                ///< WMO weather interpretation code
};

/**
 * An hourly forecast, sorted by time.
 *
 * `at(t)` interpolates linearly between the two hours around `t` (the weather
 * code comes from the nearer hour). Times more than an hour outside the
 * forecast return nullopt rather than a guess.
 */
class WeatherForecast {
 public:
  WeatherForecast() = default;
  explicit WeatherForecast(std::vector<WeatherSample> hourly);

  [[nodiscard]] std::optional<WeatherSample> at(std::int64_t unix_time) const;
  [[nodiscard]] bool empty() const noexcept { return hourly_.empty(); }
  [[nodiscard]] const std::vector<WeatherSample>& hourly() const noexcept { return hourly_; }

 private:
  std::vector<WeatherSample> hourly_;
};

/// Parse an Open-Meteo `/v1/forecast` response requested with `timeformat=unixtime`
/// and the hourly variables listed in `open_meteo_url()`. Throws WeatherError.
[[nodiscard]] WeatherForecast parse_open_meteo(std::string_view json_text);

/// Forecast URL for `where`: hourly data from yesterday to 7 days ahead.
[[nodiscard]] std::string open_meteo_url(std::string_view base_url, const LatLng& where);

/**
 * Clear-sky direct normal irradiance (W/m^2) for a sun elevation, from the
 * Meinel & Meinel (1976) model: 1353 * 0.7^(AM^0.678), AM = 1 / sin(elevation).
 * Returns 0 when the sun is below the horizon.
 */
[[nodiscard]] double clear_sky_direct_normal(double sun_elevation_deg) noexcept;

/**
 * How strong direct sunshine is compared with a cloudless sky, in [0, 1]:
 * forecast direct irradiance / clear-sky direct irradiance. 1 means crisp
 * shadows; 0 means overcast (no shadows, so shade does not matter).
 */
[[nodiscard]] double direct_sun_strength(const WeatherSample& weather, const SunPosition& sun) noexcept;

/// Short description of a WMO weather code, e.g. "Partly cloudy", "Thunderstorm".
[[nodiscard]] std::string_view describe_weather_code(int code) noexcept;

/// Speed reductions from weather, following the FHWA Road Weather Management
/// Program's published ranges (midpoints): see drive_weather_slowdown().
struct WeatherSlowdown {
  double freeway = 1.0;  ///< multiply free-flow speed on freeways by this
  double street = 1.0;   ///< ... and on arterials and local streets by this
  std::string reason;    ///< e.g. "Heavy rain", empty if none
};

/**
 * Weather speed factors. Freeways (FHWA, free-flow speed): light rain -2..13 %,
 * heavy rain -6..17 %, snow -5..64 %, low visibility -10..12 %. Arterials:
 * wet pavement -10..25 %, snow -30..40 %. Midpoints are used.
 */
[[nodiscard]] WeatherSlowdown drive_weather_slowdown(const WeatherSample& weather);

/// Is now a good time to walk?
enum class WalkComfort : std::uint8_t {
  kPleasant,   ///< go for it
  kFair,       ///< fine, with a small caveat (warm, chilly, strong UV)
  kCaution,    ///< uncomfortable: very hot, rain, cold or windy
  kDangerous,  ///< dangerous heat or thunderstorms
};

struct ComfortAssessment {
  WalkComfort level = WalkComfort::kPleasant;
  std::string headline;  ///< e.g. "Very hot"
  std::string advice;    ///< e.g. "Stay in the shade and carry water."
};

/**
 * Rate walking conditions. Heat bands follow the US National Weather Service
 * heat-index categories applied to the "feels like" temperature: caution from
 * 80 F (26.7 C), extreme caution from 90 F (32.2 C), danger from 103 F (39.4 C).
 */
[[nodiscard]] ComfortAssessment assess_walk_comfort(const WeatherSample& weather, const SunPosition& sun);

struct WeatherServiceOptions {
  std::string base_url = "https://api.open-meteo.com/v1/forecast";
  CachePolicy cache;  ///< refresh every 10 min, back off 60 s, serve up to 3 h old
};

/**
 * Live forecast for one location (the map's centre), fetched on demand and
 * cached (see CachedResource). The HTTP client is injected, so the core library
 * has no network code and tests use a fake.
 */
class WeatherService {
 public:
  using Clock = CachedResource<WeatherForecast>::Clock;
  using NowFn = CachedResource<WeatherForecast>::NowFn;

  WeatherService(LatLng location, HttpGet get, WeatherServiceOptions options = {}, NowFn now = Clock::now);

  /// Current forecast, or null if none could be fetched recently.
  [[nodiscard]] std::shared_ptr<const WeatherForecast> forecast() const { return cache_.get(); }

  [[nodiscard]] const LatLng& location() const noexcept { return location_; }
  [[nodiscard]] const std::string& url() const noexcept { return url_; }

 private:
  LatLng location_;
  std::string url_;
  CachedResource<WeatherForecast> cache_;
};

}  // namespace umbra
