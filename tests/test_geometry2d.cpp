#include <gtest/gtest.h>

#include <random>

#include "umbra/geometry2d.hpp"

namespace umbra::geom {
namespace {

const std::vector<Vec2> kSquare{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
// An "L" shape (non-convex).
const std::vector<Vec2> kL{{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}};

TEST(Geometry2d, SegmentIntersection) {
  EXPECT_TRUE(segments_intersect({0, 0}, {10, 10}, {0, 10}, {10, 0}));   // X
  EXPECT_FALSE(segments_intersect({0, 0}, {10, 0}, {0, 1}, {10, 1}));    // parallel
  EXPECT_TRUE(segments_intersect({0, 0}, {10, 0}, {10, 0}, {10, 5}));    // touching endpoint
  EXPECT_TRUE(segments_intersect({0, 0}, {10, 0}, {5, 0}, {15, 0}));     // collinear overlap
  EXPECT_FALSE(segments_intersect({0, 0}, {4, 0}, {5, 0}, {15, 0}));     // collinear gap
}

TEST(Geometry2d, PointInPolygonHandlesNonConvexShapes) {
  EXPECT_TRUE(point_in_polygon({5, 5}, kSquare));
  EXPECT_FALSE(point_in_polygon({15, 5}, kSquare));
  EXPECT_TRUE(point_in_polygon({2, 8}, kL));
  EXPECT_FALSE(point_in_polygon({8, 8}, kL));  // in the notch
}

TEST(Geometry2d, SegmentHitsPolygon) {
  EXPECT_TRUE(segment_hits_polygon({-5, 5}, {5, 5}, kSquare));    // enters
  EXPECT_TRUE(segment_hits_polygon({2, 2}, {3, 3}, kSquare));      // fully inside
  EXPECT_FALSE(segment_hits_polygon({-5, -5}, {-1, 20}, kSquare)); // misses
  EXPECT_FALSE(segment_hits_polygon({6, 6}, {9, 9}, kL));          // inside the notch
}

TEST(Geometry2d, ConvexHullOfSquareWithInteriorPoints) {
  std::vector<Vec2> points = kSquare;
  points.push_back({5, 5});
  points.push_back({2, 7});
  points.push_back({10, 5});  // on an edge: dropped
  const auto hull = convex_hull(points);
  ASSERT_EQ(hull.size(), 4U);
  for (std::size_t i = 0; i < hull.size(); ++i) {  // counter-clockwise
    EXPECT_GT(cross(hull[i], hull[(i + 1) % 4], hull[(i + 2) % 4]), 0);
  }
}

TEST(Geometry2d, ConvexHullContainsEveryInputPoint) {
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> coord(-50, 50);
  std::vector<Vec2> points(200);
  for (Vec2& p : points) p = {coord(rng), coord(rng)};
  const auto hull = convex_hull(points);
  for (const Vec2& p : points) {
    for (std::size_t i = 0; i < hull.size(); ++i) {
      EXPECT_GE(cross(hull[i], hull[(i + 1) % hull.size()], p), -1e-9);
    }
  }
}

TEST(Geometry2d, ConvexHullOfDegenerateInput) {
  EXPECT_TRUE(convex_hull({}).empty());
  EXPECT_EQ(convex_hull({{1, 1}, {1, 1}}).size(), 1U);
}

}  // namespace
}  // namespace umbra::geom
