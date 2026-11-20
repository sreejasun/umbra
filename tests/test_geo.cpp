#include <gtest/gtest.h>

#include "umbra/geo.hpp"

namespace umbra {
namespace {

TEST(Geo, HaversineOfIdenticalPointsIsZero) {
  EXPECT_DOUBLE_EQ(haversine_m({33.42, -111.93}, {33.42, -111.93}), 0.0);
}

TEST(Geo, OneDegreeOfLatitudeIsAbout111Km) {
  EXPECT_NEAR(haversine_m({0, 0}, {1, 0}), 111'195.0, 5.0);
}

TEST(Geo, HaversineIsSymmetric) {
  const LatLng a{33.4255, -111.9400};  // Tempe
  const LatLng b{33.4484, -112.0740};  // Phoenix
  EXPECT_DOUBLE_EQ(haversine_m(a, b), haversine_m(b, a));
  EXPECT_NEAR(haversine_m(a, b), 12'700.0, 300.0);
}

TEST(Geo, BearingCardinalDirections) {
  EXPECT_NEAR(bearing_deg({0, 0}, {1, 0}), 0.0, 1e-9);
  EXPECT_NEAR(bearing_deg({0, 0}, {0, 1}), 90.0, 1e-9);
  EXPECT_NEAR(bearing_deg({0, 0}, {-1, 0}), 180.0, 1e-9);
  EXPECT_NEAR(bearing_deg({0, 0}, {0, -1}), 270.0, 1e-9);
}

TEST(Geo, BearingDeltaWrapsAround) {
  EXPECT_DOUBLE_EQ(bearing_delta_deg(350, 10), 20);    // right across north
  EXPECT_DOUBLE_EQ(bearing_delta_deg(10, 350), -20);   // left across north
  EXPECT_DOUBLE_EQ(bearing_delta_deg(90, 0), -90);     // east -> north = left
  EXPECT_DOUBLE_EQ(bearing_delta_deg(0, 180), 180);    // u-turn
}

}  // namespace
}  // namespace umbra
