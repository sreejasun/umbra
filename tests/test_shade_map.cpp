#include <gtest/gtest.h>

#include <cmath>

#include "umbra/shade_map.hpp"

namespace umbra {
namespace {

// Metres -> degrees near 33.4 N.
constexpr double kLat0 = 33.4000;
constexpr double kLng0 = -111.9400;
constexpr double kMPerDegLat = 111195.0;
const double kMPerDegLng = kMPerDegLat * std::cos(kLat0 * 3.141592653589793 / 180.0);

LatLng at(double east_m, double north_m) {
  return {kLat0 + north_m / kMPerDegLat, kLng0 + east_m / kMPerDegLng};
}

/// A 20 m x 20 m building, 10 m tall, centred on the origin.
ShadeMap one_building() {
  ShadeCasters c;
  c.buildings.push_back({{at(-10, -10), at(10, -10), at(10, 10), at(-10, 10)}, 10.0});
  return ShadeMap(std::move(c));
}

const SunPosition kSunSouth45{180.0, 45.0};  // shadow falls 10 m to the north
const SunPosition kNight{180.0, -10.0};

TEST(ShadeMap, PointBehindBuildingIsShaded) {
  const ShadeMap map = one_building();
  EXPECT_TRUE(map.is_shaded(at(0, 15), kSunSouth45));   // 5 m north of the wall
  EXPECT_FALSE(map.is_shaded(at(0, 25), kSunSouth45));  // beyond the 10 m shadow
  EXPECT_FALSE(map.is_shaded(at(0, -15), kSunSouth45)); // sunny side
  EXPECT_FALSE(map.is_shaded(at(15, 15), kSunSouth45)); // beside the shadow
  EXPECT_TRUE(map.is_shaded(at(0, 0), kSunSouth45));    // inside the building
}

TEST(ShadeMap, LowSunCastsLongerShadows) {
  const ShadeMap map = one_building();
  const SunPosition low{180.0, 15.0};  // 10 m / tan(15 deg) ~= 37 m
  EXPECT_TRUE(map.is_shaded(at(0, 40), low));
  EXPECT_FALSE(map.is_shaded(at(0, 52), low));
}

TEST(ShadeMap, ShadowFollowsTheSun) {
  const ShadeMap map = one_building();
  const SunPosition west{270.0, 45.0};  // afternoon sun from the west: shadow to the east
  EXPECT_TRUE(map.is_shaded(at(15, 0), west));
  EXPECT_FALSE(map.is_shaded(at(-15, 0), west));
}

TEST(ShadeMap, EverythingIsShadedAtNight) {
  const ShadeMap map = one_building();
  EXPECT_TRUE(map.is_shaded(at(500, 500), kNight));
  EXPECT_DOUBLE_EQ(map.sun_exposure(at(0, -50), at(0, -40), kNight), 0.0);
}

TEST(ShadeMap, TreesShadeUnderAndBehindTheirCrown) {
  ShadeCasters c;
  c.trees.push_back({at(0, 0), 3.0, 8.0});
  const ShadeMap map(std::move(c));
  const SunPosition overhead{180.0, 85.0};
  EXPECT_TRUE(map.is_shaded(at(0, 0), overhead));
  EXPECT_FALSE(map.is_shaded(at(10, 0), overhead));
  // Crown at ~5.2 m, sun at 45 deg from the south: shadow centred ~5 m north of the trunk.
  EXPECT_TRUE(map.is_shaded(at(0, 5), kSunSouth45));
  EXPECT_FALSE(map.is_shaded(at(0, -5), kSunSouth45));
}

TEST(ShadeMap, SunExposureOfSegments) {
  const ShadeMap map = one_building();
  // Runs east-west through the shadow 5 m north of the wall: middle 20 m of 60 m shaded.
  EXPECT_NEAR(map.sun_exposure(at(-30, 15), at(30, 15), kSunSouth45), 40.0 / 60.0, 0.1);
  EXPECT_DOUBLE_EQ(map.sun_exposure(at(-30, -30), at(30, -30), kSunSouth45), 1.0);
  EXPECT_DOUBLE_EQ(map.sun_exposure(at(-5, 12), at(5, 12), kSunSouth45), 0.0);
}

TEST(ShadeMap, EmptyMapIsAlwaysSunnyByDay) {
  const ShadeMap map(ShadeCasters{});
  EXPECT_FALSE(map.is_shaded(at(0, 0), kSunSouth45));
  EXPECT_DOUBLE_EQ(map.sun_exposure(at(0, 0), at(10, 0), kSunSouth45), 1.0);
  EXPECT_TRUE(map.shadows({33.3, -112, 33.5, -111.8}, kSunSouth45, 10).empty());
}

TEST(ShadeMap, ShadowOutlinesForDrawing) {
  const ShadeMap map = one_building();
  const auto shapes = map.shadows({kLat0 - 0.01, kLng0 - 0.01, kLat0 + 0.01, kLng0 + 0.01}, kSunSouth45, 10);
  ASSERT_EQ(shapes.size(), 1U);
  EXPECT_EQ(shapes[0].footprint.size(), 4U);
  ASSERT_GE(shapes[0].shadow.size(), 4U);
  double max_lat = -90;
  for (const LatLng& p : shapes[0].shadow) max_lat = std::max(max_lat, p.lat);
  EXPECT_NEAR((max_lat - kLat0) * kMPerDegLat, 20.0, 0.5);  // 10 m wall + 10 m shadow

  EXPECT_TRUE(map.shadows({kLat0 - 0.01, kLng0 - 0.01, kLat0 + 0.01, kLng0 + 0.01}, kNight, 10)[0]
                  .shadow.empty());
  EXPECT_TRUE(map.shadows({34, -110, 34.1, -109.9}, kSunSouth45, 10).empty());  // out of view
}

TEST(ShadeMap, EstimatesBuildingHeights) {
  EXPECT_DOUBLE_EQ(ShadeMap::estimate_height_m("yes", 3, 500), 3 * 3.3 + 1.0);
  EXPECT_DOUBLE_EQ(ShadeMap::estimate_height_m("carport", 0, 50), 3.5);
  EXPECT_DOUBLE_EQ(ShadeMap::estimate_height_m("house", 0, 180), 4.5);
  EXPECT_LT(ShadeMap::estimate_height_m("yes", 0, 100), ShadeMap::estimate_height_m("yes", 0, 5000));
}

}  // namespace
}  // namespace umbra
