#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <random>

#include "umbra/spatial_index.hpp"
#include "test_helpers.hpp"

namespace umbra {
namespace {

TEST(SpatialIndex, EmptyGraphHasNoNearest) {
  const RoadGraph empty;
  const SpatialIndex index(empty);
  EXPECT_FALSE(index.nearest({33.4, -111.9}).has_value());
}

TEST(SpatialIndex, ExactHitHasZeroDistance) {
  const RoadGraph g = test::random_road_graph(50, 2);
  const SpatialIndex index(g);
  const auto m = index.nearest(g.node(17).position);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->node, 17U);
  EXPECT_NEAR(m->distance_m, 0.0, 1e-6);
}

// Grid search must return the same node as a brute-force scan, for queries
// both inside and well outside the graph's bounding box.
TEST(SpatialIndex, MatchesBruteForce) {
  const RoadGraph g = test::random_road_graph(2000, 9);
  for (double cell : {25.0, 100.0, 400.0}) {
    const SpatialIndex index(g, cell);
    std::mt19937 rng(77);
    std::uniform_real_distribution<double> lat(33.38, 33.44);
    std::uniform_real_distribution<double> lng(-111.97, -111.91);

    for (int i = 0; i < 500; ++i) {
      const LatLng q{lat(rng), lng(rng)};
      const auto m = index.nearest(q);
      ASSERT_TRUE(m.has_value());

      double best = std::numeric_limits<double>::infinity();
      for (const Node& n : g.nodes()) best = std::min(best, haversine_m(q, n.position));

      // Projection error over a few km is far below a metre.
      EXPECT_NEAR(haversine_m(q, g.node(m->node).position), best, 0.5) << "cell " << cell;
      EXPECT_NEAR(m->distance_m, best, 1.0);
    }
  }
}

TEST(SpatialIndex, RejectsNonPositiveCellSize) {
  const RoadGraph g = test::random_road_graph(10, 1);
  EXPECT_THROW(SpatialIndex(g, 0.0), std::invalid_argument);
}

}  // namespace
}  // namespace umbra
