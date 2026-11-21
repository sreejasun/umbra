#include "umbra/shortest_path.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

#include "umbra/indexed_min_heap.hpp"

namespace umbra {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

void validate(const RoadGraph& graph, NodeId node, const char* what) {
  if (node >= graph.node_count()) {
    throw std::out_of_range(std::string("find_shortest_path: invalid ") + what + " node id");
  }
}

/**
 * Shared Dijkstra / A* loop from a set of sources to a set of targets.
 *
 * All sources start at cost 0 (as if joined to a virtual super-source), and the
 * search stops at the first target it settles, which is the cheapest
 * source-target pair. A* is Dijkstra with the heap keyed on g(v) + h(v), where
 * h is the great-circle distance to the nearest target times `h_per_meter`.
 * The minimum of consistent heuristics is consistent, so each node is still
 * settled at most once and the result is optimal. With h = 0 this is plain
 * Dijkstra.
 */
template <typename Cost>
SearchResult search(const RoadGraph& graph, std::span<const NodeId> sources, std::span<const NodeId> targets,
                    Cost&& cost, double h_per_meter, const SearchOptions& options) {
  if (sources.empty()) throw std::invalid_argument("find_shortest_path: no source nodes");
  if (targets.empty()) throw std::invalid_argument("find_shortest_path: no target nodes");
  for (NodeId s : sources) validate(graph, s, "source");
  for (NodeId t : targets) validate(graph, t, "target");

  const auto started = std::chrono::steady_clock::now();
  const std::size_t n = graph.node_count();
  const auto nodes = graph.nodes();

  std::vector<bool> is_target(n, false);
  std::vector<LatLng> goals;
  for (NodeId t : targets) {
    if (!is_target[t]) goals.push_back(nodes[t].position);
    is_target[t] = true;
  }

  const bool use_heuristic = options.algorithm == Algorithm::kAStar && h_per_meter > 0;
  auto heuristic = [&](NodeId v) {
    if (!use_heuristic) return 0.0;
    double nearest = kInfinity;
    for (const LatLng& goal : goals) nearest = std::min(nearest, haversine_m(nodes[v].position, goal));
    return nearest * h_per_meter;
  };

  std::vector<double> best(n, kInfinity);            // g(v): best known cost from the nearest source
  std::vector<EdgeId> parent_edge(n, kInvalidEdge);  // edge used to reach v (none for sources)
  std::vector<bool> settled(n, false);
  IndexedMinHeap<double> frontier(n);

  SearchResult result;
  for (NodeId s : sources) {
    if (best[s] == 0.0) continue;  // duplicate source
    best[s] = 0.0;
    frontier.push(s, heuristic(s));
  }

  NodeId reached = kInvalidNode;
  while (!frontier.empty()) {
    const NodeId u = frontier.pop().first;
    settled[u] = true;
    ++result.settled_count;
    if (options.record_settled) result.settled_order.push_back(u);
    if (is_target[u]) {
      result.found = true;
      reached = u;
      break;
    }

    EdgeId edge_id = graph.first_edge(u);
    for (const Edge& e : graph.out_edges(u)) {
      const EdgeId id = edge_id++;
      const NodeId v = e.target;
      if (settled[v]) continue;

      const double candidate = best[u] + cost(id, e);
      if (candidate < best[v]) {  // relaxation
        best[v] = candidate;
        parent_edge[v] = id;
        frontier.push_or_decrease(v, candidate + heuristic(v));
      }
    }
  }

  if (result.found) {
    // Sources have no parent edge (their cost is 0 and edge costs are >= 0).
    NodeId v = reached;
    while (parent_edge[v] != kInvalidEdge) {
      const EdgeId id = parent_edge[v];
      result.edges.push_back(id);
      v = graph.edge(id).source;
    }
    std::reverse(result.edges.begin(), result.edges.end());

    result.nodes.reserve(result.edges.size() + 1);
    result.nodes.push_back(v);
    for (EdgeId id : result.edges) {
      const Edge& e = graph.edge(id);
      result.nodes.push_back(e.target);
      result.length_m += e.length_m;
      result.duration_s += e.duration_s;
    }
    result.cost = best[reached];
  }

  result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started);
  return result;
}

double time_heuristic_scale(const RoadGraph& graph, Metric metric) {
  // For time the straight-line distance is divided by the network's top speed,
  // so the estimate is a lower bound on any real travel time.
  if (metric == Metric::kDistance) return 1.0;
  return graph.max_speed_mps() > 0 ? 1.0 / graph.max_speed_mps() : 0.0;
}

}  // namespace

SearchResult find_shortest_path(const RoadGraph& graph, NodeId source, NodeId target,
                                const SearchOptions& options) {
  return find_shortest_path_between(graph, std::span(&source, 1), std::span(&target, 1), options);
}

SearchResult find_shortest_path_between(const RoadGraph& graph, std::span<const NodeId> sources,
                                        std::span<const NodeId> targets, const SearchOptions& options) {
  const Metric metric = options.metric;
  return search(
      graph, sources, targets, [metric](EdgeId, const Edge& e) { return edge_weight(e, metric); },
      time_heuristic_scale(graph, metric), options);
}

SearchResult find_shortest_path_between(const RoadGraph& graph, std::span<const NodeId> sources,
                                        std::span<const NodeId> targets, const EdgeCostFn& cost,
                                        double min_cost_per_meter, const SearchOptions& options) {
  if (!cost) throw std::invalid_argument("find_shortest_path: cost function is empty");
  return search(graph, sources, targets, cost, min_cost_per_meter, options);
}

std::vector<SearchResult> find_alternative_paths(const RoadGraph& graph, std::span<const NodeId> sources,
                                                std::span<const NodeId> targets, const EdgeCostFn& cost,
                                                double min_cost_per_meter, const SearchOptions& options,
                                                const AlternativeOptions& limits) {
  if (!cost) throw std::invalid_argument("find_alternative_paths: cost function is empty");
  std::vector<SearchResult> routes;
  if (limits.max_routes == 0) return routes;

  std::vector<double> factor(graph.edge_count(), 1.0);
  std::vector<bool> used(graph.edge_count(), false);  // edges of kept routes
  const EdgeCostFn penalised = [&](EdgeId id, const Edge& e) { return cost(id, e) * factor[id]; };

  for (std::size_t attempt = 0; attempt <= limits.max_attempts && routes.size() < limits.max_routes; ++attempt) {
    SearchResult r = search(graph, sources, targets, penalised, min_cost_per_meter, options);
    if (!r.found) break;
    double true_cost = 0.0;
    double shared_m = 0.0;
    for (EdgeId id : r.edges) {
      true_cost += cost(id, graph.edge(id));
      if (used[id]) shared_m += graph.edge(id).length_m;
      factor[id] *= limits.penalty;
    }
    r.cost = true_cost;
    if (!routes.empty()) {
      if (true_cost > routes.front().cost * limits.max_stretch) continue;
      if (r.length_m > 0 && shared_m / r.length_m > limits.max_shared) continue;
    }
    for (EdgeId id : r.edges) used[id] = true;
    routes.push_back(std::move(r));
  }
  return routes;
}

std::vector<double> shortest_costs_from(const RoadGraph& graph, NodeId source, Metric metric) {
  return shortest_costs_from(graph, std::span(&source, 1),
                             [metric](EdgeId, const Edge& e) { return edge_weight(e, metric); });
}

std::vector<double> shortest_costs_from(const RoadGraph& graph, std::span<const NodeId> sources,
                                        const EdgeCostFn& cost) {
  if (!cost) throw std::invalid_argument("shortest_costs_from: cost function is empty");
  for (NodeId s : sources) validate(graph, s, "source");
  const std::size_t n = graph.node_count();

  std::vector<double> best(n, kInfinity);
  IndexedMinHeap<double> frontier(n);
  std::vector<bool> settled(n, false);

  for (NodeId s : sources) {
    if (best[s] == 0.0) continue;
    best[s] = 0.0;
    frontier.push(s, 0.0);
  }
  while (!frontier.empty()) {
    const NodeId u = frontier.pop().first;
    settled[u] = true;
    EdgeId edge_id = graph.first_edge(u);
    for (const Edge& e : graph.out_edges(u)) {
      const EdgeId id = edge_id++;
      if (settled[e.target]) continue;
      const double candidate = best[u] + cost(id, e);
      if (candidate < best[e.target]) {
        best[e.target] = candidate;
        frontier.push_or_decrease(e.target, candidate);
      }
    }
  }
  return best;
}

}  // namespace umbra
