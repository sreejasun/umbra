#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "umbra/geo.hpp"

namespace umbra {

using NodeId = std::uint32_t;
using EdgeId = std::uint32_t;
using NameId = std::uint32_t;

inline constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();
inline constexpr EdgeId kInvalidEdge = std::numeric_limits<EdgeId>::max();

/// An intersection or shape point of a road.
struct Node {
  std::int64_t osm_id = 0;
  LatLng position;
};

/// Edge attribute bits.
inline constexpr std::uint8_t kEdgeCovered = 1U << 0;   ///< roofed walkway, arcade or passage
inline constexpr std::uint8_t kEdgeStairs = 1U << 1;    ///< steps, or tagged wheelchair=no
inline constexpr std::uint8_t kEdgeLit = 1U << 2;       ///< street lighting (lit=yes)
inline constexpr std::uint8_t kEdgeUnlit = 1U << 3;     ///< explicitly unlit (lit=no)

/// A directed road segment. Two-way roads are stored as two edges.
struct Edge {
  NodeId source = kInvalidNode;
  NodeId target = kInvalidNode;
  double length_m = 0.0;    ///< physical length
  double duration_s = 0.0;  ///< free-flow travel time at the posted / default speed
  NameId name = 0;          ///< index into the street-name table ("" = unnamed)
  std::uint8_t flags = 0;   ///< kEdge* bits

  [[nodiscard]] bool covered() const noexcept { return (flags & kEdgeCovered) != 0; }
  [[nodiscard]] bool has(std::uint8_t flag) const noexcept { return (flags & flag) != 0; }
};

/**
 * Immutable directed road network in Compressed Sparse Row (CSR) layout.
 *
 * All outgoing edges of node `v` are stored contiguously in
 * `edges_[offsets_[v] .. offsets_[v + 1])`. Compared with a vector-of-vectors
 * adjacency list this uses one allocation, has no per-node overhead and is
 * cache-friendly when a search scans neighbours.
 *
 *   memory: O(V + E)   out_edges(v): O(1) to obtain, O(deg v) to scan
 */
class RoadGraph {
 public:
  RoadGraph() = default;

  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
  [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

  [[nodiscard]] const Node& node(NodeId id) const { return nodes_.at(id); }
  [[nodiscard]] const Edge& edge(EdgeId id) const { return edges_.at(id); }
  [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }

  /// Outgoing edges of `id`. Edge ids are `first_edge(id) + index`.
  [[nodiscard]] std::span<const Edge> out_edges(NodeId id) const noexcept {
    return {edges_.data() + offsets_[id], edges_.data() + offsets_[id + 1]};
  }
  [[nodiscard]] EdgeId first_edge(NodeId id) const noexcept { return offsets_[id]; }

  [[nodiscard]] const std::string& name(NameId id) const { return names_.at(id); }

  /// Fastest speed on any edge (m/s); used to keep the A* time heuristic admissible.
  [[nodiscard]] double max_speed_mps() const noexcept { return max_speed_mps_; }
  [[nodiscard]] const BoundingBox& bounds() const noexcept { return bounds_; }

  /// Return the subgraph induced by the largest strongly connected component
  /// (Kosaraju's algorithm, iterative). Guarantees every node can reach every
  /// other node, so a snapped start/end pair always has a route.
  [[nodiscard]] RoadGraph largest_strongly_connected_component() const;

 private:
  friend class RoadGraphBuilder;

  /// Build CSR offsets from `edges` (any order) and compute bounds / max speed.
  static RoadGraph assemble(std::vector<Node> nodes, std::vector<Edge> edges,
                            std::vector<std::string> names);

  std::vector<Node> nodes_;
  std::vector<EdgeId> offsets_{0};  // size V + 1
  std::vector<Edge> edges_;         // sorted by source
  std::vector<std::string> names_{""};
  double max_speed_mps_ = 0.0;
  BoundingBox bounds_;
};

/// Incrementally collects nodes and edges, then freezes them into a RoadGraph.
class RoadGraphBuilder {
 public:
  NodeId add_node(std::int64_t osm_id, LatLng position);
  NameId intern_name(std::string_view name);

  /// Add a directed edge. Throws std::invalid_argument on bad ids or weights.
  void add_edge(NodeId source, NodeId target, double length_m, double duration_s, NameId name = 0,
                std::uint8_t flags = 0);

  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }

  /// Build the CSR graph. The builder is left empty.
  [[nodiscard]] RoadGraph build();

 private:
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<std::string> names_{""};
  std::unordered_map<std::string, NameId> name_index_{{"", 0}};
};

}  // namespace umbra
