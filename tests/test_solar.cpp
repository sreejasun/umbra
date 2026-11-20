#include <gtest/gtest.h>

#include <cstdint>

#include "umbra/solar.hpp"

namespace umbra {
namespace {

const LatLng kTempe{33.4255, -111.9400};
constexpr std::int64_t kJune21_2026_0000Z = 1782000000;
constexpr std::int64_t kDec21_2026_1930Z = 1797881400;  // ~ solar noon in Tempe

/// Highest sun of the UTC day starting at `day_start`, scanning minute by minute.
SunPosition solar_noon(std::int64_t day_start) {
  SunPosition best{0, -90};
  for (std::int64_t t = day_start; t < day_start + 86400; t += 60) {
    const SunPosition s = sun_position(kTempe, t);
    if (s.elevation_deg > best.elevation_deg) best = s;
  }
  return best;
}

TEST(Solar, SummerSolsticeNoonIsHighInTheSouth) {
  // Max elevation = 90 - (latitude - declination) = 90 - (33.43 - 23.44) ~= 80.0 deg.
  const SunPosition noon = solar_noon(kJune21_2026_0000Z);
  EXPECT_NEAR(noon.elevation_deg, 80.0, 0.3);
  EXPECT_NEAR(noon.azimuth_deg, 180.0, 3.0);
}

TEST(Solar, WinterSolsticeNoonIsLow) {
  // 90 - (33.43 + 23.44) ~= 33.1 deg.
  const SunPosition noon = sun_position(kTempe, kDec21_2026_1930Z);
  EXPECT_NEAR(noon.elevation_deg, 33.1, 0.5);
  EXPECT_NEAR(noon.azimuth_deg, 180.0, 2.0);
}

TEST(Solar, MorningSunIsEastAndAfternoonSunIsWest) {
  const std::int64_t morning = kJune21_2026_0000Z + 15 * 3600;  // 08:00 MST
  const std::int64_t evening = kJune21_2026_0000Z + 24 * 3600;  // 17:00 MST
  const SunPosition am = sun_position(kTempe, morning);
  const SunPosition pm = sun_position(kTempe, evening);
  EXPECT_TRUE(am.is_up());
  EXPECT_TRUE(pm.is_up());
  EXPECT_GT(am.azimuth_deg, 45.0);
  EXPECT_LT(am.azimuth_deg, 135.0);
  EXPECT_GT(pm.azimuth_deg, 225.0);
  EXPECT_LT(pm.azimuth_deg, 315.0);
}

TEST(Solar, SunIsDownAtMidnight) {
  const SunPosition midnight = sun_position(kTempe, kJune21_2026_0000Z + 31 * 3600);  // 00:00 MST
  EXPECT_FALSE(midnight.is_up());
  EXPECT_LT(midnight.elevation_deg, -20.0);
}

TEST(Solar, AzimuthStaysInRange) {
  for (std::int64_t t = kJune21_2026_0000Z; t < kJune21_2026_0000Z + 86400; t += 1800) {
    const SunPosition s = sun_position(kTempe, t);
    EXPECT_GE(s.azimuth_deg, 0.0);
    EXPECT_LT(s.azimuth_deg, 360.0);
    EXPECT_GE(s.elevation_deg, -90.0);
    EXPECT_LE(s.elevation_deg, 90.0);
  }
}

}  // namespace
}  // namespace umbra
