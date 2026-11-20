#include <gtest/gtest.h>

#include <stdexcept>

#include "umbra/road_graph.hpp"

namespace umbra {
namespace {

RoadGraph line_graph() {
  // 0 <-> 1 <-> 2, plus a one-way spur 2 -> 3 (3 is a dead end).
  RoadGraphBuilder b;
  for (int i = 0; i < 4; ++i) b.add_node(100 + i, {33.4, -111.9 + 0.001 * i});
  const NameId main = b.intern_name("Main St");
  b.add_edge(0, 1, 100, 10, main);
  b.add_edge(1, 0, 100, 10, main);
  b.add_edge(1, 2, 100, 5, main);
  b.add_edge(2, 1, 100, 5, main);
  b.add_edge(2, 3, 50, 5);
  return b.build();
}

TEST(RoadGraph, BuildsCompressedSparseRows) {
  const RoadGraph g = line_graph();
  EXPECT_EQ(g.node_count(), 4U);
  EXPECT_EQ(g.edge_count(), 5U);
  EXPECT_EQ(g.out_edges(0).size(), 1U);
  EXPECT_EQ(g.out_edges(1).size(), 2U);
  EXPECT_EQ(g.out_edges(2).size(), 2U);
  EXPECT_TRUE(g.out_edges(3).empty());
  for (NodeId v = 0; v < g.node_count(); ++v) {
    for (const Edge& e : g.out_edges(v)) EXPECT_EQ(e.source, v);
  }
  EXPECT_EQ(g.edge(g.first_edge(1)).source, 1U);
}

TEST(RoadGraph, InternsStreetNames) {
  RoadGraphBuilder b;
  const NameId a = b.intern_name("Mill Ave");
  const NameId again = b.intern_name("Mill Ave");
  const NameId other = b.intern_name("University Dr");
  EXPECT_EQ(a, again);
  EXPECT_NE(a, other);
  EXPECT_EQ(b.intern_name(""), 0U);
}

TEST(RoadGraph, ComputesBoundsAndMaxSpeed) {
  const RoadGraph g = line_graph();
  EXPECT_DOUBLE_EQ(g.bounds().min_lng, -111.9);
  EXPECT_NEAR(g.bounds().max_lng, -111.897, 1e-12);
  EXPECT_DOUBLE_EQ(g.max_speed_mps(), 20.0);  // 100 m in 5 s
}

TEST(RoadGraph, RejectsInvalidEdges) {
  RoadGraphBuilder b;
  b.add_node(1, {0, 0});
  b.add_node(2, {0, 1});
  EXPECT_THROW(b.add_edge(0, 5, 1, 1), std::invalid_argument);
  EXPECT_THROW(b.add_edge(0, 0, 1, 1), std::invalid_argument);
  EXPECT_THROW(b.add_edge(0, 1, -1, 1), std::invalid_argument);
  EXPECT_THROW(b.add_edge(0, 1, 1, std::numeric_limits<double>::infinity()), std::invalid_argument);
  EXPECT_THROW(b.add_edge(0, 1, 1, 1, 42), std::invalid_argument);
}

TEST(RoadGraph, LargestSccDropsDeadEndsAndIslands) {
  RoadGraphBuilder b;
  // Main loop 0 -> 1 -> 2 -> 0, a dead end 2 -> 3, and an island 4 <-> 5.
  for (int i = 0; i < 6; ++i) b.add_node(i, {33.4 + 0.001 * i, -111.9});
  b.add_edge(0, 1, 1, 1);
  b.add_edge(1, 2, 1, 1);
  b.add_edge(2, 0, 1, 1);
  b.add_edge(2, 3, 1, 1);
  b.add_edge(4, 5, 1, 1);
  b.add_edge(5, 4, 1, 1);
  const RoadGraph scc = b.build().largest_strongly_connected_component();

  EXPECT_EQ(scc.node_count(), 3U);
  EXPECT_EQ(scc.edge_count(), 3U);
  for (const Node& n : scc.nodes()) EXPECT_LE(n.osm_id, 2);
}

TEST(RoadGraph, SccOfEmptyGraphIsEmpty) {
  const RoadGraph empty;
  EXPECT_TRUE(empty.largest_strongly_connected_component().empty());
}

}  // namespace
}  // namespace umbra
