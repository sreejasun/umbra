#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <future>
#include <thread>
#include <sstream>
#include <stdexcept>

#include "umbra/weather.hpp"

namespace umbra {
namespace {

constexpr std::int64_t kT0 = 1784127600;  // first hour in open_meteo_sample.json
constexpr std::int64_t kHour = 3600;

std::string read_fixture(const char* name) {
  std::ifstream in(std::string(UMBRA_TEST_DATA_DIR) + "/" + name);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

WeatherSample sample(double feels_c, int code = 0) {
  WeatherSample s;
  s.temperature_c = feels_c;
  s.apparent_temperature_c = feels_c;
  s.weather_code = code;
  return s;
}

const SunPosition kHighSun{180.0, 60.0};
const SunPosition kNight{0.0, -20.0};

// ------------------------------------------------------------------ parsing

TEST(WeatherParse, ReadsHourlyColumnsAndSkipsMissingHours) {
  const WeatherForecast f = parse_open_meteo(read_fixture("open_meteo_sample.json"));
  // The third hour has no temperature, so it is missing data, not 0 degrees.
  ASSERT_EQ(f.hourly().size(), 4U);
  const WeatherSample& first = f.hourly().front();
  EXPECT_EQ(first.time, kT0);
  EXPECT_DOUBLE_EQ(first.temperature_c, 38.0);
  EXPECT_DOUBLE_EQ(first.apparent_temperature_c, 40.0);
  EXPECT_DOUBLE_EQ(first.uv_index, 9.0);
  EXPECT_DOUBLE_EQ(first.direct_normal_wm2, 800.0);
  EXPECT_EQ(f.hourly()[2].weather_code, 95);
}

TEST(WeatherParse, RejectsBadResponses) {
  EXPECT_THROW((void)parse_open_meteo("not json"), WeatherError);
  EXPECT_THROW((void)parse_open_meteo("[]"), WeatherError);
  EXPECT_THROW((void)parse_open_meteo(R"({"error": true, "reason": "Latitude must be in range"})"), WeatherError);
  EXPECT_THROW((void)parse_open_meteo(R"({"hourly": {"time": []}})"), WeatherError);
  // ISO times mean the request forgot timeformat=unixtime.
  EXPECT_THROW((void)parse_open_meteo(R"({"hourly": {"time": ["2026-07-15T12:00"], "temperature_2m": [30]}})"),
               WeatherError);
}

TEST(WeatherParse, UrlAsksForEverythingTheParserReads) {
  const std::string url = open_meteo_url("https://api.open-meteo.com/v1/forecast", {33.4255, -111.94});
  EXPECT_NE(url.find("latitude=33.4255&longitude=-111.9400"), std::string::npos);
  for (const char* field : {"temperature_2m", "apparent_temperature", "cloud_cover", "precipitation",
                            "weather_code", "wind_speed_10m", "uv_index", "direct_normal_irradiance",
                            "timeformat=unixtime"}) {
    EXPECT_NE(url.find(field), std::string::npos) << field;
  }
}

// ------------------------------------------------------------ interpolation

TEST(WeatherForecast, InterpolatesBetweenHours) {
  const WeatherForecast f = parse_open_meteo(read_fixture("open_meteo_sample.json"));
  const auto half = f.at(kT0 + kHour / 2);
  ASSERT_TRUE(half);
  EXPECT_NEAR(half->temperature_c, 39.0, 1e-9);
  EXPECT_NEAR(half->cloud_cover_pct, 10.0, 1e-9);
  // Radiation is an average over the preceding hour, so the value stamped
  // kT0 + 1 h describes kT0 + 30 min exactly.
  EXPECT_NEAR(half->direct_normal_wm2, 700.0, 1e-9);
  // Codes are categories: take the nearest hour instead of averaging.
  EXPECT_EQ(f.at(kT0 + 20 * 60)->weather_code, 0);
  EXPECT_EQ(f.at(kT0 + 40 * 60)->weather_code, 1);
}

TEST(WeatherForecast, RefusesToGuessFarOutsideTheForecast) {
  const WeatherForecast f = parse_open_meteo(read_fixture("open_meteo_sample.json"));
  EXPECT_TRUE(f.at(kT0 - 30 * 60));
  EXPECT_FALSE(f.at(kT0 - 2 * kHour));
  EXPECT_FALSE(f.at(kT0 + 10 * kHour));
  EXPECT_FALSE(WeatherForecast().at(kT0));
}

// ----------------------------------------------------------------- sunshine

TEST(Sunshine, ClearSkyBeamWeakensTowardTheHorizon) {
  EXPECT_DOUBLE_EQ(clear_sky_direct_normal(-1.0), 0.0);
  // Meinel model: ~1,000 W/m^2 overhead, ~830 at 37 degrees, much less near sunrise.
  EXPECT_NEAR(clear_sky_direct_normal(90.0), 947.0, 1.0);
  EXPECT_NEAR(clear_sky_direct_normal(37.0), 830.0, 15.0);
  EXPECT_LT(clear_sky_direct_normal(5.0), clear_sky_direct_normal(20.0));
}

TEST(Sunshine, StrengthIsForecastOverClearSky) {
  WeatherSample w;
  w.direct_normal_wm2 = clear_sky_direct_normal(60.0) / 2;
  EXPECT_NEAR(direct_sun_strength(w, kHighSun), 0.5, 1e-9);
  w.direct_normal_wm2 = 5000.0;  // clamped
  EXPECT_DOUBLE_EQ(direct_sun_strength(w, kHighSun), 1.0);
  w.direct_normal_wm2 = 0.0;  // overcast
  EXPECT_DOUBLE_EQ(direct_sun_strength(w, kHighSun), 0.0);
  w.direct_normal_wm2 = 500.0;  // the sun is down: nothing to avoid
  EXPECT_DOUBLE_EQ(direct_sun_strength(w, kNight), 0.0);
}

// ------------------------------------------------------------------ comfort

TEST(WalkComfortTest, FollowsNwsHeatBands) {
  EXPECT_EQ(assess_walk_comfort(sample(22.0), kNight).level, WalkComfort::kPleasant);
  EXPECT_EQ(assess_walk_comfort(sample(28.0), kNight).level, WalkComfort::kFair);       // >= 80 F
  EXPECT_EQ(assess_walk_comfort(sample(35.0), kNight).level, WalkComfort::kCaution);    // >= 90 F
  EXPECT_EQ(assess_walk_comfort(sample(41.0), kNight).level, WalkComfort::kDangerous);  // >= 103 F
  EXPECT_EQ(assess_walk_comfort(sample(41.0), kNight).headline, "Dangerous heat");
}

TEST(WalkComfortTest, WeatherHazards) {
  EXPECT_EQ(assess_walk_comfort(sample(22.0, 95), kNight).level, WalkComfort::kDangerous);  // thunderstorm
  EXPECT_EQ(assess_walk_comfort(sample(22.0, 63), kNight).headline, "Rain");
  EXPECT_EQ(assess_walk_comfort(sample(2.0), kNight).headline, "Cold");
  EXPECT_EQ(assess_walk_comfort(sample(8.0), kNight).headline, "Chilly");

  WeatherSample windy = sample(22.0);
  windy.wind_speed_kmh = 55.0;
  EXPECT_EQ(assess_walk_comfort(windy, kNight).headline, "Windy");

  WeatherSample bright = sample(22.0);
  bright.uv_index = 10.0;
  EXPECT_EQ(assess_walk_comfort(bright, kHighSun).headline, "Strong UV");
  EXPECT_EQ(assess_walk_comfort(bright, kNight).headline, "Pleasant");  // no UV at night
}

TEST(WalkComfortTest, DescribesWeatherCodes) {
  EXPECT_EQ(describe_weather_code(0), "Clear sky");
  EXPECT_EQ(describe_weather_code(3), "Overcast");
  EXPECT_EQ(describe_weather_code(95), "Thunderstorm");
  EXPECT_EQ(describe_weather_code(1234), "Unknown");
}

// ------------------------------------------------------------------ service

/// Fake HTTP client and clock for WeatherService.
struct FakeUpstream {
  int calls = 0;
  bool fail = false;
  std::string body = read_fixture("open_meteo_sample.json");
  WeatherService::Clock::time_point now{};

  HttpGet client() {
    return [this](const std::string&) {
      ++calls;
      if (fail) throw std::runtime_error("network down");
      return body;
    };
  }
  WeatherService::NowFn clock() {
    return [this] { return now; };
  }
};

TEST(WeatherServiceTest, CachesBetweenRefreshes) {
  FakeUpstream up;
  WeatherService service({33.42, -111.93}, up.client(), {}, up.clock());
  ASSERT_TRUE(service.forecast());
  ASSERT_TRUE(service.forecast());
  EXPECT_EQ(up.calls, 1);

  up.now += std::chrono::minutes(11);  // past refresh_after
  ASSERT_TRUE(service.forecast());
  EXPECT_EQ(up.calls, 2);
}

TEST(WeatherServiceTest, ServesStaleDataThenGivesUp) {
  FakeUpstream up;
  WeatherService service({33.42, -111.93}, up.client(), {}, up.clock());
  ASSERT_TRUE(service.forecast());

  up.fail = true;
  up.now += std::chrono::minutes(30);
  EXPECT_TRUE(service.forecast()) << "a failed refresh keeps the previous forecast";
  const int after_failure = up.calls;
  EXPECT_TRUE(service.forecast());
  EXPECT_EQ(up.calls, after_failure) << "no retry storm while upstream is down";

  up.now += std::chrono::hours(4);  // past max_age
  EXPECT_FALSE(service.forecast());
}

TEST(WeatherServiceTest, RecoversAfterTheBackoff) {
  FakeUpstream up;
  up.fail = true;
  WeatherService service({33.42, -111.93}, up.client(), {}, up.clock());
  EXPECT_FALSE(service.forecast());
  up.fail = false;
  EXPECT_FALSE(service.forecast());  // still backing off
  up.now += std::chrono::seconds(61);
  EXPECT_TRUE(service.forecast());
}

TEST(CachedResourceTest, BackgroundModeNeverWaitsForTheUpstream) {
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> calls = 0;
  CachePolicy policy;
  policy.background = true;
  CachedResource<int> cache(
      [&] {
        ++calls;
        released.wait();  // a slow upstream
        return 42;
      },
      policy);

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(cache.get(), nullptr) << "nothing yet, and no waiting";
  for (int i = 0; i < 200 && calls.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_EQ(cache.get(), nullptr) << "still fetching";
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(1500));
  EXPECT_EQ(calls.load(), 1) << "one fetch at a time";

  release.set_value();
  std::shared_ptr<const int> value;
  for (int i = 0; i < 200 && !value; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    value = cache.get();
  }
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);
}

TEST(WeatherServiceTest, SendsOnlyTheMapCentre) {
  FakeUpstream up;
  std::string requested;
  WeatherService service(
      {33.4, -111.9},
      [&](const std::string& url) {
        requested = url;
        return up.body;
      },
      {}, up.clock());
  (void)service.forecast();
  EXPECT_EQ(requested, service.url());
  EXPECT_NE(requested.find("latitude=33.4000&longitude=-111.9000"), std::string::npos);
}

}  // namespace
}  // namespace umbra
