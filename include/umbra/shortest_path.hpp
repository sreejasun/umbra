#pragma once

#include <chrono>
#include <functional>
#include <span>
#include <vector>

#include "umbra/road_graph.hpp"

namespace umbra {

/// What a route minimises.
enum class Metric {
  kTime,      ///< fastest route (edge weight = travel time in seconds)
  kDistance,  ///< shortest route (edge weight = length in metres)
};

enum class Algorithm {
  kDijkstra,  ///< uninformed: explores outward in rings of equal cost
  kAStar,     ///< goal-directed: Dijkstra + admissible straight-line heuristic
};

struct SearchOptions {
  Metric metric = Metric::kTime;
  Algorithm algorithm = Algorithm::kDijkstra;
  bool record_settled = false;  ///< keep the order nodes were settled (for visualisation)
};

struct SearchResult {
  bool found = false;
  double cost = 0.0;        ///< in the unit of the chosen metric
  double length_m = 0.0;    ///< total physical length of the path
  double duration_s = 0.0;  ///< total travel time of the path
  std::vector<NodeId> nodes;  ///< source ... target
  std::vector<EdgeId> edges;  ///< edges[i] connects nodes[i] -> nodes[i + 1]
  std::size_t settled_count = 0;  ///< nodes whose final distance was fixed
  std::vector<NodeId> settled_order;  ///< filled only if record_settled
  std::chrono::microseconds elapsed{0};
};

/// Weight of an edge under the given metric.
[[nodiscard]] inline double edge_weight(const Edge& e, Metric metric) noexcept {
  return metric == Metric::kTime ? e.duration_s : e.length_m;
}

/**
 * Point-to-point shortest path with Dijkstra or A*.
 *
 * Both share one implementation: A* is Dijkstra with the heap keyed on
 * `g(v) + h(v)` instead of `g(v)`, where h is the great-circle distance to the
 * target (divided by the network's top speed for the time metric). Edge
 * lengths are sums of great-circle segments, so h never overestimates and is
 * consistent; each node is therefore settled at most once and the result is
 * optimal.
 *
 *   time: O((V + E) log V)   memory: O(V)
 *
 * Throws std::out_of_range if source or target are not valid node ids.
 */
[[nodiscard]] SearchResult find_shortest_path(const RoadGraph& graph, NodeId source, NodeId target,
                                              const SearchOptions& options = {});

/// Cost of traversing one edge; must be >= 0.
using EdgeCostFn = std::function<double(EdgeId, const Edge&)>;

/**
 * Cheapest path from any of `sources` to any of `targets`: for example from a
 * click to whichever entrance of a building is closest by network.
 *
 * Equivalent to adding a virtual super-source joined to every source at cost
 * 0 and stopping at the first target settled. A* uses the distance to the
 * nearest target as its heuristic (still admissible and consistent), so one
 * search costs about the same as a point-to-point query, instead of the
 * |sources| x |targets| queries that trying every pair would need.
 *
 * `result.nodes.front()` / `.back()` tell which source and target were used.
 * Throws std::invalid_argument if either set is empty and std::out_of_range
 * for an invalid node id.
 */
[[nodiscard]] SearchResult find_shortest_path_between(const RoadGraph& graph, std::span<const NodeId> sources,
                                                      std::span<const NodeId> targets,
                                                      const SearchOptions& options = {});

/**
 * Multi-source / multi-target search with a caller-supplied edge cost (e.g.
 * length plus a penalty for walking in the sun). `options.metric` is ignored.
 * For A*, `min_cost_per_meter` must be a lower bound on cost / length over all
 * edges so that `haversine * min_cost_per_meter` never overestimates; pass 0
 * to fall back to Dijkstra behaviour.
 */
[[nodiscard]] SearchResult find_shortest_path_between(const RoadGraph& graph, std::span<const NodeId> sources,
                                                      std::span<const NodeId> targets, const EdgeCostFn& cost,
                                                      double min_cost_per_meter, const SearchOptions& options = {});

/// Single-source Dijkstra: cost from `source` to every node (infinity if unreachable).
[[nodiscard]] std::vector<double> shortest_costs_from(const RoadGraph& graph, NodeId source,
                                                      Metric metric = Metric::kTime);

/// Multi-source Dijkstra with a custom edge cost: cost from the nearest source
/// to every node (infinity if unreachable). Used to pick the best stop on a
/// trip: d(origin, s) + d(s, destination) for every candidate s in two searches.
[[nodiscard]] std::vector<double> shortest_costs_from(const RoadGraph& graph, std::span<const NodeId> sources,
                                                      const EdgeCostFn& cost);

}  // namespace umbra
