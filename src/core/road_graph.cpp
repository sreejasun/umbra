#include "umbra/road_graph.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace umbra {

// ----------------------------------------------------------------------------
// RoadGraphBuilder
// ----------------------------------------------------------------------------

NodeId RoadGraphBuilder::add_node(std::int64_t osm_id, LatLng position) {
  if (nodes_.size() >= kInvalidNode) throw std::length_error("RoadGraphBuilder: too many nodes");
  nodes_.push_back({osm_id, position});
  return static_cast<NodeId>(nodes_.size() - 1);
}

NameId RoadGraphBuilder::intern_name(std::string_view name) {
  std::string key(name);
  if (auto it = name_index_.find(key); it != name_index_.end()) return it->second;
  const auto id = static_cast<NameId>(names_.size());
  names_.push_back(key);
  name_index_.emplace(std::move(key), id);
  return id;
}

void RoadGraphBuilder::add_edge(NodeId source, NodeId target, double length_m, double duration_s,
                                NameId name, std::uint8_t flags) {
  if (source >= nodes_.size() || target >= nodes_.size()) {
    throw std::invalid_argument("RoadGraphBuilder::add_edge: node id out of range");
  }
  if (source == target) throw std::invalid_argument("RoadGraphBuilder::add_edge: self-loop");
  if (!std::isfinite(length_m) || !std::isfinite(duration_s) || length_m < 0 || duration_s < 0) {
    throw std::invalid_argument("RoadGraphBuilder::add_edge: weights must be finite and >= 0");
  }
  if (name >= names_.size()) throw std::invalid_argument("RoadGraphBuilder::add_edge: bad name id");
  if (edges_.size() >= kInvalidEdge) throw std::length_error("RoadGraphBuilder: too many edges");
  edges_.push_back({source, target, length_m, duration_s, name, flags});
}

RoadGraph RoadGraphBuilder::build() {
  RoadGraph graph = RoadGraph::assemble(std::move(nodes_), std::move(edges_), std::move(names_));
  nodes_.clear();
  edges_.clear();
  names_ = {""};
  name_index_ = {{"", 0}};
  return graph;
}

// ----------------------------------------------------------------------------
// RoadGraph
// ----------------------------------------------------------------------------

RoadGraph RoadGraph::assemble(std::vector<Node> nodes, std::vector<Edge> edges,
                              std::vector<std::string> names) {
  RoadGraph g;
  const std::size_t n = nodes.size();

  // Counting sort of edges by source: O(V + E), stable.
  g.offsets_.assign(n + 1, 0);
  for (const Edge& e : edges) ++g.offsets_[e.source + 1];
  for (std::size_t i = 0; i < n; ++i) g.offsets_[i + 1] += g.offsets_[i];

  g.edges_.resize(edges.size());
  std::vector<EdgeId> cursor(g.offsets_.begin(), g.offsets_.end() - 1);
  for (const Edge& e : edges) g.edges_[cursor[e.source]++] = e;

  for (const Edge& e : g.edges_) {
    if (e.duration_s > 0) g.max_speed_mps_ = std::max(g.max_speed_mps_, e.length_m / e.duration_s);
  }

  if (n > 0) {
    BoundingBox b{nodes[0].position.lat, nodes[0].position.lng, nodes[0].position.lat,
                  nodes[0].position.lng};
    for (const Node& node : nodes) {
      b.min_lat = std::min(b.min_lat, node.position.lat);
      b.max_lat = std::max(b.max_lat, node.position.lat);
      b.min_lng = std::min(b.min_lng, node.position.lng);
      b.max_lng = std::max(b.max_lng, node.position.lng);
    }
    g.bounds_ = b;
  }

  g.nodes_ = std::move(nodes);
  g.names_ = names.empty() ? std::vector<std::string>{""} : std::move(names);
  return g;
}

RoadGraph RoadGraph::largest_strongly_connected_component() const {
  const std::size_t n = nodes_.size();
  if (n == 0) return *this;

  // Reverse adjacency (CSR) for the second pass.
  std::vector<EdgeId> rev_offsets(n + 1, 0);
  for (const Edge& e : edges_) ++rev_offsets[e.target + 1];
  for (std::size_t i = 0; i < n; ++i) rev_offsets[i + 1] += rev_offsets[i];
  std::vector<NodeId> rev_sources(edges_.size());
  {
    std::vector<EdgeId> cursor(rev_offsets.begin(), rev_offsets.end() - 1);
    for (const Edge& e : edges_) rev_sources[cursor[e.target]++] = e.source;
  }

  // Pass 1: iterative DFS on G, recording nodes in order of completion.
  std::vector<NodeId> finish_order;
  finish_order.reserve(n);
  {
    std::vector<bool> visited(n, false);
    std::vector<std::pair<NodeId, EdgeId>> stack;  // (node, next edge to try)
    for (NodeId start = 0; start < n; ++start) {
      if (visited[start]) continue;
      visited[start] = true;
      stack.emplace_back(start, offsets_[start]);
      while (!stack.empty()) {
        auto& [v, next] = stack.back();
        if (next < offsets_[v + 1]) {
          const NodeId w = edges_[next++].target;
          if (!visited[w]) {
            visited[w] = true;
            stack.emplace_back(w, offsets_[w]);  // invalidates v/next; not used again
          }
        } else {
          finish_order.push_back(v);
          stack.pop_back();
        }
      }
    }
  }

  // Pass 2: DFS on reversed G in decreasing finish time; each tree is one SCC.
  constexpr std::uint32_t kUnassigned = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> component(n, kUnassigned);
  std::vector<std::size_t> component_size;
  {
    std::vector<NodeId> stack;
    for (auto it = finish_order.rbegin(); it != finish_order.rend(); ++it) {
      if (component[*it] != kUnassigned) continue;
      const auto c = static_cast<std::uint32_t>(component_size.size());
      component_size.push_back(0);
      component[*it] = c;
      stack.push_back(*it);
      while (!stack.empty()) {
        const NodeId u = stack.back();
        stack.pop_back();
        ++component_size[c];
        for (EdgeId i = rev_offsets[u]; i < rev_offsets[u + 1]; ++i) {
          const NodeId w = rev_sources[i];
          if (component[w] == kUnassigned) {
            component[w] = c;
            stack.push_back(w);
          }
        }
      }
    }
  }

  const auto largest = static_cast<std::uint32_t>(
      std::max_element(component_size.begin(), component_size.end()) - component_size.begin());

  // Induced subgraph with compacted ids (order-preserving).
  std::vector<NodeId> remap(n, kInvalidNode);
  std::vector<Node> nodes;
  nodes.reserve(component_size[largest]);
  for (NodeId v = 0; v < n; ++v) {
    if (component[v] == largest) {
      remap[v] = static_cast<NodeId>(nodes.size());
      nodes.push_back(nodes_[v]);
    }
  }
  std::vector<Edge> edges;
  for (const Edge& e : edges_) {
    if (component[e.source] == largest && component[e.target] == largest) {
      Edge copy = e;
      copy.source = remap[e.source];
      copy.target = remap[e.target];
      edges.push_back(copy);
    }
  }
  return assemble(std::move(nodes), std::move(edges), names_);
}

}  // namespace umbra
