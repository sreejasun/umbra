#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "umbra/shortest_path.hpp"
#include "test_helpers.hpp"

namespace umbra {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

/// Small network where the fastest and the shortest route differ:
///
///   0 --(1000 m, 30 s freeway)--> 1 --(1000 m, 30 s)--> 3
///   0 --(600 m, 90 s street)----> 2 --(600 m, 90 s)---> 3
RoadGraph freeway_vs_street() {
  RoadGraphBuilder b;
  b.add_node(0, {33.40, -111.95});
  b.add_node(1, {33.41, -111.94});
  b.add_node(2, {33.40, -111.94});
  b.add_node(3, {33.41, -111.93});
  const NameId fwy = b.intern_name("Loop 202");
  const NameId st = b.intern_name("Apache Blvd");
  b.add_edge(0, 1, 1000, 30, fwy);
  b.add_edge(1, 3, 1000, 30, fwy);
  b.add_edge(0, 2, 600, 90, st);
  b.add_edge(2, 3, 600, 90, st);
  return b.build();
}

/// Reference implementation: Bellman-Ford, O(V * E). Slow but obviously correct.
std::vector<double> bellman_ford(const RoadGraph& g, NodeId source, Metric metric) {
  std::vector<double> dist(g.node_count(), kInf);
  dist[source] = 0;
  for (std::size_t round = 0; round + 1 < g.node_count(); ++round) {
    bool changed = false;
    for (NodeId u = 0; u < g.node_count(); ++u) {
      if (dist[u] == kInf) continue;
      for (const Edge& e : g.out_edges(u)) {
        const double c = dist[u] + edge_weight(e, metric);
        if (c < dist[e.target]) {
          dist[e.target] = c;
          changed = true;
        }
      }
    }
    if (!changed) break;
  }
  return dist;
}

TEST(ShortestPath, FastestRouteTakesTheFreeway) {
  const RoadGraph g = freeway_vs_street();
  const auto r = find_shortest_path(g, 0, 3, {Metric::kTime, Algorithm::kDijkstra});
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{0, 1, 3}));
  EXPECT_DOUBLE_EQ(r.cost, 60);
  EXPECT_DOUBLE_EQ(r.duration_s, 60);
  EXPECT_DOUBLE_EQ(r.length_m, 2000);
  ASSERT_EQ(r.edges.size(), 2U);
  EXPECT_EQ(g.edge(r.edges[0]).target, 1U);
}

TEST(ShortestPath, ShortestRouteTakesTheStreet) {
  const RoadGraph g = freeway_vs_street();
  const auto r = find_shortest_path(g, 0, 3, {Metric::kDistance, Algorithm::kDijkstra});
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{0, 2, 3}));
  EXPECT_DOUBLE_EQ(r.cost, 1200);
  EXPECT_DOUBLE_EQ(r.duration_s, 180);
}

TEST(ShortestPath, RespectsOneWayRoads) {
  const RoadGraph g = freeway_vs_street();  // every edge is one-way
  const auto r = find_shortest_path(g, 3, 0);
  EXPECT_FALSE(r.found);
  EXPECT_TRUE(r.nodes.empty());
  EXPECT_TRUE(r.edges.empty());
}

TEST(ShortestPath, SourceEqualsTarget) {
  const RoadGraph g = freeway_vs_street();
  for (auto algo : {Algorithm::kDijkstra, Algorithm::kAStar}) {
    const auto r = find_shortest_path(g, 2, 2, {Metric::kTime, algo});
    ASSERT_TRUE(r.found);
    EXPECT_EQ(r.nodes, (std::vector<NodeId>{2}));
    EXPECT_DOUBLE_EQ(r.cost, 0);
    EXPECT_EQ(r.settled_count, 1U);
  }
}

TEST(ShortestPath, RecordsSettledOrderWhenAsked) {
  const RoadGraph g = freeway_vs_street();
  SearchOptions opt;
  opt.record_settled = true;
  const auto r = find_shortest_path(g, 0, 3, opt);
  EXPECT_EQ(r.settled_order.size(), r.settled_count);
  EXPECT_EQ(r.settled_order.front(), 0U);
  EXPECT_EQ(r.settled_order.back(), 3U);

  const auto quiet = find_shortest_path(g, 0, 3);
  EXPECT_TRUE(quiet.settled_order.empty());
}

TEST(ShortestPath, RejectsInvalidNodeIds) {
  const RoadGraph g = freeway_vs_street();
  EXPECT_THROW((void)find_shortest_path(g, 0, 99), std::out_of_range);
  EXPECT_THROW((void)find_shortest_path(g, 99, 0), std::out_of_range);
}

TEST(ShortestPath, PathCostEqualsSumOfEdgeWeights) {
  const RoadGraph g = test::random_road_graph(400, 7);
  std::mt19937 rng(3);
  std::uniform_int_distribution<NodeId> pick(0, static_cast<NodeId>(g.node_count() - 1));
  for (int i = 0; i < 50; ++i) {
    const auto r = find_shortest_path(g, pick(rng), pick(rng), {Metric::kDistance});
    if (!r.found) continue;
    double sum = 0;
    for (std::size_t k = 0; k < r.edges.size(); ++k) {
      const Edge& e = g.edge(r.edges[k]);
      EXPECT_EQ(e.source, r.nodes[k]);
      EXPECT_EQ(e.target, r.nodes[k + 1]);
      sum += e.length_m;
    }
    EXPECT_NEAR(sum, r.cost, 1e-6);
  }
}

// Dijkstra must agree with Bellman-Ford on every reachable node.
TEST(ShortestPath, DijkstraMatchesBellmanFordOnRandomGraphs) {
  for (unsigned seed = 1; seed <= 5; ++seed) {
    const RoadGraph g = test::random_road_graph(150, seed);
    for (Metric metric : {Metric::kTime, Metric::kDistance}) {
      const auto expected = bellman_ford(g, 0, metric);
      const auto actual = shortest_costs_from(g, 0, metric);
      ASSERT_EQ(expected.size(), actual.size());
      for (std::size_t v = 0; v < expected.size(); ++v) {
        if (expected[v] == kInf) {
          EXPECT_EQ(actual[v], kInf);
        } else {
          EXPECT_NEAR(actual[v], expected[v], 1e-6) << "seed " << seed << " node " << v;
        }
      }
    }
  }
}

// A* must return the same optimal cost as Dijkstra while settling no more nodes.
TEST(ShortestPath, AStarIsOptimalAndExploresLess) {
  const RoadGraph g = test::random_road_graph(800, 11);
  std::mt19937 rng(5);
  std::uniform_int_distribution<NodeId> pick(0, static_cast<NodeId>(g.node_count() - 1));
  std::size_t dijkstra_settled = 0;
  std::size_t astar_settled = 0;

  for (int i = 0; i < 200; ++i) {
    const NodeId s = pick(rng);
    const NodeId t = pick(rng);
    for (Metric metric : {Metric::kTime, Metric::kDistance}) {
      const auto d = find_shortest_path(g, s, t, {metric, Algorithm::kDijkstra});
      const auto a = find_shortest_path(g, s, t, {metric, Algorithm::kAStar});
      ASSERT_EQ(d.found, a.found);
      if (!d.found) continue;
      EXPECT_NEAR(a.cost, d.cost, 1e-6 * std::max(1.0, d.cost));
      dijkstra_settled += d.settled_count;
      astar_settled += a.settled_count;
    }
  }
  EXPECT_LT(astar_settled, dijkstra_settled);
}

// ------------------------------------------------------- sets of endpoints

TEST(ShortestPathBetween, PicksTheCheapestTarget) {
  const RoadGraph g = freeway_vs_street();
  const std::vector<NodeId> sources{0};
  const std::vector<NodeId> targets{3, 2};
  const auto r = find_shortest_path_between(g, sources, targets, {Metric::kDistance, Algorithm::kDijkstra});
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.nodes.back(), 2U);  // 600 m beats 1,200 m
  EXPECT_DOUBLE_EQ(r.cost, 600.0);
}

TEST(ShortestPathBetween, SourceInsideTheTargetsIsFree) {
  const RoadGraph g = freeway_vs_street();
  const std::vector<NodeId> sources{1, 2};
  const std::vector<NodeId> targets{3, 2};
  const auto r = find_shortest_path_between(g, sources, targets);
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{2}));
  EXPECT_DOUBLE_EQ(r.cost, 0.0);
}

TEST(ShortestPathBetween, RejectsEmptySets) {
  const RoadGraph g = freeway_vs_street();
  const std::vector<NodeId> some{0};
  const std::vector<NodeId> none;
  EXPECT_THROW((void)find_shortest_path_between(g, none, some), std::invalid_argument);
  EXPECT_THROW((void)find_shortest_path_between(g, some, none), std::invalid_argument);
  const std::vector<NodeId> bad{99};
  EXPECT_THROW((void)find_shortest_path_between(g, some, bad), std::out_of_range);
}

// ----------------------------------------------------------- alternatives

EdgeCostFn by_length() {
  return [](EdgeId, const Edge& e) { return e.length_m; };
}

TEST(AlternativePaths, FindsTheOtherRoadFirstBest) {
  const RoadGraph g = freeway_vs_street();  // 0->1->3 (2,000 m) and 0->2->3 (1,200 m)
  const std::vector<NodeId> from{0};
  const std::vector<NodeId> to{3};
  AlternativeOptions limits;
  limits.max_stretch = 2.0;
  const auto routes = find_alternative_paths(g, from, to, by_length(), 1.0, {}, limits);
  ASSERT_EQ(routes.size(), 2U);
  EXPECT_EQ(routes[0].nodes, (std::vector<NodeId>{0, 2, 3}));
  EXPECT_EQ(routes[1].nodes, (std::vector<NodeId>{0, 1, 3}));
  EXPECT_DOUBLE_EQ(routes[1].cost, 2000.0) << "true cost, without the penalty";
}

TEST(AlternativePaths, DropsRoutesThatAreTooLong) {
  const RoadGraph g = freeway_vs_street();
  const std::vector<NodeId> from{0};
  const std::vector<NodeId> to{3};
  const auto routes = find_alternative_paths(g, from, to, by_length(), 1.0);  // 2,000 > 1.35 x 1,200
  ASSERT_EQ(routes.size(), 1U);
  AlternativeOptions one;
  one.max_routes = 1;
  EXPECT_EQ(find_alternative_paths(g, from, to, by_length(), 1.0, {}, one).size(), 1U);
}

// On random graphs: the first route is optimal, others are distinct, within the
// stretch limit and share at most the allowed length with earlier ones.
TEST(AlternativePaths, RespectsTheLimitsOnRandomGraphs) {
  const RoadGraph g = test::random_road_graph(500, 31, 0.0);
  std::mt19937 rng(17);
  std::uniform_int_distribution<NodeId> pick(0, static_cast<NodeId>(g.node_count() - 1));
  const AlternativeOptions limits;
  for (int i = 0; i < 40; ++i) {
    const std::vector<NodeId> from{pick(rng)};
    const std::vector<NodeId> to{pick(rng)};
    const auto routes = find_alternative_paths(g, from, to, by_length(), 1.0, {Metric::kDistance, Algorithm::kAStar});
    const auto best = find_shortest_path(g, from[0], to[0], {Metric::kDistance});
    ASSERT_EQ(routes.empty(), !best.found);
    if (routes.empty()) continue;
    EXPECT_NEAR(routes[0].cost, best.cost, 1e-6 * std::max(1.0, best.cost));
    std::vector<bool> used(g.edge_count(), false);
    for (std::size_t k = 0; k < routes.size(); ++k) {
      EXPECT_LE(routes[k].cost, routes[0].cost * limits.max_stretch + 1e-6);
      double shared = 0;
      for (EdgeId id : routes[k].edges) shared += used[id] ? g.edge(id).length_m : 0.0;
      if (k > 0 && routes[k].length_m > 0) EXPECT_LE(shared / routes[k].length_m, limits.max_shared + 1e-9);
      for (EdgeId id : routes[k].edges) used[id] = true;
    }
  }
}

TEST(ShortestCostsFrom, MultiSourceCustomCost) {
  const RoadGraph g = freeway_vs_street();
  const std::vector<NodeId> sources{1, 2};
  const auto costs = shortest_costs_from(g, sources, [](EdgeId, const Edge& e) { return e.length_m; });
  EXPECT_DOUBLE_EQ(costs[1], 0.0);
  EXPECT_DOUBLE_EQ(costs[2], 0.0);
  EXPECT_DOUBLE_EQ(costs[3], 600.0);  // nearest source: 2
  EXPECT_EQ(costs[0], kInf);          // nothing leads back to 0
}

// One search over sets must equal the best of all |S| x |T| point-to-point
// searches, for Dijkstra and for A* with the nearest-target heuristic.
TEST(ShortestPathBetween, MatchesTheBestPairOnRandomGraphs) {
  const RoadGraph g = test::random_road_graph(400, 21);
  std::mt19937 rng(9);
  std::uniform_int_distribution<NodeId> pick(0, static_cast<NodeId>(g.node_count() - 1));
  std::uniform_int_distribution<int> count(1, 4);

  for (int i = 0; i < 60; ++i) {
    std::vector<NodeId> sources(static_cast<std::size_t>(count(rng)));
    std::vector<NodeId> targets(static_cast<std::size_t>(count(rng)));
    for (NodeId& s : sources) s = pick(rng);
    for (NodeId& t : targets) t = pick(rng);

    for (Metric metric : {Metric::kTime, Metric::kDistance}) {
      double best = kInf;
      for (NodeId s : sources) {
        for (NodeId t : targets) {
          const auto r = find_shortest_path(g, s, t, {metric, Algorithm::kDijkstra});
          if (r.found) best = std::min(best, r.cost);
        }
      }
      for (Algorithm algorithm : {Algorithm::kDijkstra, Algorithm::kAStar}) {
        const auto r = find_shortest_path_between(g, sources, targets, {metric, algorithm});
        ASSERT_EQ(r.found, best < kInf);
        if (!r.found) continue;
        EXPECT_NEAR(r.cost, best, 1e-6 * std::max(1.0, best));
        EXPECT_NE(std::find(sources.begin(), sources.end(), r.nodes.front()), sources.end());
        EXPECT_NE(std::find(targets.begin(), targets.end(), r.nodes.back()), targets.end());
      }
    }
  }
}

}  // namespace
}  // namespace umbra
