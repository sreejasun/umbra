#include "umbra/weather.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <utility>

#include <nlohmann/json.hpp>

namespace umbra {
namespace {

using json = nlohmann::json;

constexpr std::int64_t kHour = 3600;

/// Hourly radiation and precipitation are means / sums over the *preceding*
/// hour, so they describe the middle of that hour.
constexpr std::int64_t kPrecedingHourShift = kHour / 2;

double number_or(const json& array, std::size_t i, double fallback) {
  if (i >= array.size() || !array[i].is_number()) return fallback;
  return array[i].get<double>();
}

/// Linear interpolation of one field at `t`, where sample i describes time(i) - shift.
template <typename Field>
double interpolate(const std::vector<WeatherSample>& hourly, std::int64_t t, std::int64_t shift, Field field) {
  const auto at = [&](std::size_t i) { return hourly[i].time - shift; };
  if (t <= at(0)) return field(hourly.front());
  if (t >= at(hourly.size() - 1)) return field(hourly.back());
  const auto upper = std::partition_point(hourly.begin(), hourly.end(),
                                          [&](const WeatherSample& s) { return s.time - shift <= t; });
  const auto hi = static_cast<std::size_t>(upper - hourly.begin());
  const std::size_t lo = hi - 1;
  const double f = static_cast<double>(t - at(lo)) / static_cast<double>(at(hi) - at(lo));
  return field(hourly[lo]) + (field(hourly[hi]) - field(hourly[lo])) * f;
}

bool is_rain(int code) noexcept { return (code >= 61 && code <= 67) || (code >= 80 && code <= 82); }
bool is_snow(int code) noexcept { return (code >= 71 && code <= 77) || code == 85 || code == 86; }
bool is_thunder(int code) noexcept { return code >= 95 && code <= 99; }

}  // namespace

// ------------------------------------------------------------------ forecast

WeatherForecast::WeatherForecast(std::vector<WeatherSample> hourly) : hourly_(std::move(hourly)) {
  std::sort(hourly_.begin(), hourly_.end(),
            [](const WeatherSample& a, const WeatherSample& b) { return a.time < b.time; });
}

std::optional<WeatherSample> WeatherForecast::at(std::int64_t t) const {
  if (hourly_.empty() || t < hourly_.front().time - kHour || t > hourly_.back().time + kHour) {
    return std::nullopt;
  }
  const auto lerp = [&](auto member, std::int64_t shift) {
    return interpolate(hourly_, t, shift, [member](const WeatherSample& s) { return s.*member; });
  };

  WeatherSample out;
  out.time = t;
  out.temperature_c = lerp(&WeatherSample::temperature_c, 0);
  out.apparent_temperature_c = lerp(&WeatherSample::apparent_temperature_c, 0);
  out.cloud_cover_pct = lerp(&WeatherSample::cloud_cover_pct, 0);
  out.wind_speed_kmh = lerp(&WeatherSample::wind_speed_kmh, 0);
  out.uv_index = lerp(&WeatherSample::uv_index, 0);
  out.precipitation_mm = lerp(&WeatherSample::precipitation_mm, kPrecedingHourShift);
  out.direct_normal_wm2 = lerp(&WeatherSample::direct_normal_wm2, kPrecedingHourShift);

  // A weather code cannot be interpolated: take the nearer of the two surrounding hours.
  auto after = std::partition_point(hourly_.begin(), hourly_.end(),
                                    [t](const WeatherSample& s) { return s.time < t; });
  if (after == hourly_.end()) --after;
  if (after != hourly_.begin() && t - std::prev(after)->time <= after->time - t) --after;
  out.weather_code = after->weather_code;
  return out;
}

WeatherForecast parse_open_meteo(std::string_view json_text) {
  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw WeatherError(std::string("Invalid weather JSON: ") + e.what());
  }
  if (!doc.is_object()) throw WeatherError("Weather response must be a JSON object");
  if (doc.contains("error") && doc["error"].is_boolean() && doc["error"].get<bool>()) {
    throw WeatherError("Weather service error: " + doc.value("reason", std::string("unknown")));
  }
  if (!doc.contains("hourly") || !doc["hourly"].is_object()) throw WeatherError("Weather response has no 'hourly' data");

  const json& h = doc["hourly"];
  const auto column = [&h](const char* key) -> const json& {
    static const json kEmpty = json::array();
    return h.contains(key) && h[key].is_array() ? h[key] : kEmpty;
  };
  const json& times = column("time");
  if (times.empty()) throw WeatherError("Weather response has no hourly 'time' values (use timeformat=unixtime)");

  const json& temperature = column("temperature_2m");
  const json& apparent = column("apparent_temperature");
  const json& clouds = column("cloud_cover");
  const json& precipitation = column("precipitation");
  const json& wind = column("wind_speed_10m");
  const json& uv = column("uv_index");
  const json& direct = column("direct_normal_irradiance");
  const json& code = column("weather_code");

  std::vector<WeatherSample> hourly;
  hourly.reserve(times.size());
  for (std::size_t i = 0; i < times.size(); ++i) {
    if (!times[i].is_number_integer()) throw WeatherError("Hourly 'time' must be Unix seconds (timeformat=unixtime)");
    // Hours without a temperature are missing data, not "0 degrees".
    if (i >= temperature.size() || !temperature[i].is_number()) continue;
    WeatherSample s;
    s.time = times[i].get<std::int64_t>();
    s.temperature_c = temperature[i].get<double>();
    s.apparent_temperature_c = number_or(apparent, i, s.temperature_c);
    s.cloud_cover_pct = std::clamp(number_or(clouds, i, 0.0), 0.0, 100.0);
    s.precipitation_mm = std::max(0.0, number_or(precipitation, i, 0.0));
    s.wind_speed_kmh = std::max(0.0, number_or(wind, i, 0.0));
    s.uv_index = std::max(0.0, number_or(uv, i, 0.0));
    s.direct_normal_wm2 = std::max(0.0, number_or(direct, i, 0.0));
    s.weather_code = static_cast<int>(number_or(code, i, 0.0));
    hourly.push_back(s);
  }
  if (hourly.empty()) throw WeatherError("Weather response has no usable hourly data");
  return WeatherForecast(std::move(hourly));
}

std::string open_meteo_url(std::string_view base_url, const LatLng& where) {
  char coords[96];
  std::snprintf(coords, sizeof coords, "latitude=%.4f&longitude=%.4f", where.lat, where.lng);
  std::string url(base_url);
  url += url.find('?') == std::string::npos ? '?' : '&';
  url += coords;
  url +=
      "&hourly=temperature_2m,apparent_temperature,cloud_cover,precipitation,weather_code,"
      "wind_speed_10m,uv_index,direct_normal_irradiance"
      "&timeformat=unixtime&timezone=GMT&past_days=1&forecast_days=7";
  return url;
}

// ------------------------------------------------------------------ sunshine

double clear_sky_direct_normal(double sun_elevation_deg) noexcept {
  if (sun_elevation_deg <= 0.0) return 0.0;
  const double air_mass = 1.0 / std::sin(sun_elevation_deg * std::numbers::pi / 180.0);
  return 1353.0 * std::pow(0.7, std::pow(air_mass, 0.678));
}

double direct_sun_strength(const WeatherSample& weather, const SunPosition& sun) noexcept {
  const double clear = clear_sky_direct_normal(sun.elevation_deg);
  if (clear <= 0.0) return 0.0;
  return std::clamp(weather.direct_normal_wm2 / clear, 0.0, 1.0);
}

std::string_view describe_weather_code(int code) noexcept {
  switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45:
    case 48: return "Fog";
    case 51:
    case 53:
    case 55: return "Drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Freezing rain";
    case 71:
    case 73:
    case 75:
    case 77: return "Snow";
    case 80:
    case 81: return "Rain showers";
    case 82: return "Violent rain showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96:
    case 99: return "Thunderstorm with hail";
    default: return "Unknown";
  }
}

WeatherSlowdown drive_weather_slowdown(const WeatherSample& w) {
  WeatherSlowdown out;
  auto apply = [&out](double freeway_cut, double street_cut, const char* reason) {
    if (1.0 - freeway_cut < out.freeway || 1.0 - street_cut < out.street) {
      out.freeway = std::min(out.freeway, 1.0 - freeway_cut);
      out.street = std::min(out.street, 1.0 - street_cut);
      out.reason = reason;
    }
  };
  const bool drizzle = w.weather_code >= 51 && w.weather_code <= 57;
  const bool heavy_rain = w.weather_code == 65 || w.weather_code == 82 || w.precipitation_mm >= 7.6;  // AMS: > 7.6 mm/h
  if (is_snow(w.weather_code)) {
    apply(0.345, 0.35, "Snow");
  } else if (heavy_rain) {
    apply(0.115, 0.175, "Heavy rain");
  } else if (is_rain(w.weather_code) || drizzle || w.precipitation_mm >= 0.1) {
    apply(0.075, 0.175, "Rain");  // wet pavement
  }
  if (w.weather_code == 45 || w.weather_code == 48) apply(0.11, 0.11, "Fog");
  return out;
}

ComfortAssessment assess_walk_comfort(const WeatherSample& w, const SunPosition& sun) {
  // NWS heat-index thresholds, converted to Celsius.
  constexpr double kCautionC = 26.7;         // 80 F
  constexpr double kExtremeCautionC = 32.2;  // 90 F
  constexpr double kDangerC = 39.4;          // 103 F
  constexpr double kColdC = 4.4;             // 40 F
  constexpr double kChillyC = 10.0;          // 50 F
  constexpr double kWindyKmh = 40.0;

  const double feels = w.apparent_temperature_c;
  if (is_thunder(w.weather_code)) {
    return {WalkComfort::kDangerous, "Thunderstorms", "Lightning risk: wait it out indoors."};
  }
  if (feels >= kDangerC) {
    return {WalkComfort::kDangerous, "Dangerous heat",
            "Avoid long walks: stay in the shade, carry water and take breaks indoors."};
  }
  if (feels >= kExtremeCautionC) {
    return {WalkComfort::kCaution, "Very hot", "Stick to the shade and carry water."};
  }
  if (is_snow(w.weather_code)) return {WalkComfort::kCaution, "Snow", "Watch for slippery paths."};
  if (is_rain(w.weather_code) || w.precipitation_mm >= 0.5) {
    return {WalkComfort::kCaution, "Rain", "Bring an umbrella; covered walkways help."};
  }
  if (feels < kColdC) return {WalkComfort::kCaution, "Cold", "Dress warmly."};
  if (w.wind_speed_kmh >= kWindyKmh) {
    return {WalkComfort::kCaution, "Windy", "Strong gusts; blowing dust is possible."};
  }
  if (feels >= kCautionC) return {WalkComfort::kFair, "Warm", "Shade makes a noticeable difference."};
  if (feels < kChillyC) return {WalkComfort::kFair, "Chilly", "Bring a jacket; sunny streets feel warmer."};
  if (w.weather_code >= 51 && w.weather_code <= 57) return {WalkComfort::kFair, "Drizzle", "A light jacket helps."};
  if (w.weather_code == 45 || w.weather_code == 48) return {WalkComfort::kFair, "Fog", "Visibility is low at crossings."};
  if (sun.is_up() && w.uv_index >= 8.0) {
    return {WalkComfort::kFair, "Strong UV", "Nice temperature, but wear sunscreen."};
  }
  return {WalkComfort::kPleasant, "Pleasant", "Great weather for a walk."};
}

// ------------------------------------------------------------------ service

WeatherService::WeatherService(LatLng location, HttpGet get, WeatherServiceOptions options, NowFn now)
    : location_(location),
      url_(open_meteo_url(options.base_url, location)),
      cache_(
          [get = std::move(get), url = url_] {
            if (!get) throw WeatherError("WeatherService: no HTTP client");
            return parse_open_meteo(get(url));
          },
          options.cache, std::move(now)) {}

}  // namespace umbra
