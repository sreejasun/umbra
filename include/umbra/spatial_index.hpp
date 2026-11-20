#pragma once

#include <optional>
#include <vector>

#include "umbra/road_graph.hpp"

namespace umbra {

/**
 * Uniform-grid spatial index for nearest-node queries ("snap a click to the
 * closest intersection").
 *
 * Coordinates are projected to a local equirectangular plane in metres, which
 * is accurate to well under 0.1% over a city-sized area. Nodes are bucketed
 * into square cells stored in CSR form. A query scans rings of cells outward
 * from the query cell and stops once the next ring cannot contain anything
 * closer than the best match found so far.
 *
 *   build: O(V)   query: O(k) where k = nodes in the cells visited (typically tiny)
 */
class SpatialIndex {
 public:
  struct Match {
    NodeId node = kInvalidNode;
    double distance_m = 0.0;
  };

  explicit SpatialIndex(const RoadGraph& graph, double cell_size_m = 100.0);

  /// Closest node to `query`, or nullopt if the graph is empty.
  [[nodiscard]] std::optional<Match> nearest(const LatLng& query) const;

 private:
  struct Point {
    double x;
    double y;
  };

  [[nodiscard]] Point project(const LatLng& p) const noexcept;
  [[nodiscard]] long cell_coord(double v, double origin, long count) const noexcept;

  double cell_size_m_;
  double origin_lat_ = 0.0;
  double meters_per_deg_lat_ = 0.0;
  double meters_per_deg_lng_ = 0.0;
  double min_x_ = 0.0;
  double min_y_ = 0.0;
  long cols_ = 0;
  long rows_ = 0;
  std::vector<std::uint32_t> cell_offsets_;  // size cols*rows + 1
  std::vector<NodeId> cell_nodes_;
  std::vector<Point> projected_;  // per node
};

}  // namespace umbra
