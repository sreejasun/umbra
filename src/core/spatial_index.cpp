#include "umbra/spatial_index.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace umbra {

SpatialIndex::SpatialIndex(const RoadGraph& graph, double cell_size_m) : cell_size_m_(cell_size_m) {
  if (!(cell_size_m > 0)) throw std::invalid_argument("SpatialIndex: cell size must be positive");
  if (graph.empty()) return;

  origin_lat_ = graph.bounds().center().lat;
  meters_per_deg_lat_ = kEarthRadiusM * std::numbers::pi / 180.0;
  meters_per_deg_lng_ = meters_per_deg_lat_ * std::cos(origin_lat_ * std::numbers::pi / 180.0);

  const auto nodes = graph.nodes();
  projected_.reserve(nodes.size());
  double max_x = -std::numeric_limits<double>::infinity();
  double max_y = max_x;
  min_x_ = std::numeric_limits<double>::infinity();
  min_y_ = min_x_;
  for (const Node& node : nodes) {
    const Point p = project(node.position);
    projected_.push_back(p);
    min_x_ = std::min(min_x_, p.x);
    min_y_ = std::min(min_y_, p.y);
    max_x = std::max(max_x, p.x);
    max_y = std::max(max_y, p.y);
  }

  cols_ = static_cast<long>((max_x - min_x_) / cell_size_m_) + 1;
  rows_ = static_cast<long>((max_y - min_y_) / cell_size_m_) + 1;
  const auto cell_count = static_cast<std::size_t>(cols_ * rows_);

  // Bucket nodes into cells (counting sort -> CSR).
  auto cell_of = [&](const Point& p) {
    return static_cast<std::size_t>(cell_coord(p.y, min_y_, rows_) * cols_ +
                                    cell_coord(p.x, min_x_, cols_));
  };
  cell_offsets_.assign(cell_count + 1, 0);
  for (const Point& p : projected_) ++cell_offsets_[cell_of(p) + 1];
  for (std::size_t i = 0; i < cell_count; ++i) cell_offsets_[i + 1] += cell_offsets_[i];
  cell_nodes_.resize(projected_.size());
  std::vector<std::uint32_t> cursor(cell_offsets_.begin(), cell_offsets_.end() - 1);
  for (NodeId v = 0; v < projected_.size(); ++v) cell_nodes_[cursor[cell_of(projected_[v])]++] = v;
}

SpatialIndex::Point SpatialIndex::project(const LatLng& p) const noexcept {
  return {p.lng * meters_per_deg_lng_, p.lat * meters_per_deg_lat_};
}

long SpatialIndex::cell_coord(double v, double origin, long count) const noexcept {
  const double c = std::floor((v - origin) / cell_size_m_);
  if (c < 0) return 0;
  if (c >= static_cast<double>(count)) return count - 1;
  return static_cast<long>(c);
}

std::optional<SpatialIndex::Match> SpatialIndex::nearest(const LatLng& query) const {
  if (projected_.empty()) return std::nullopt;

  const Point q = project(query);
  const long qx = cell_coord(q.x, min_x_, cols_);
  const long qy = cell_coord(q.y, min_y_, rows_);

  double best_d2 = std::numeric_limits<double>::infinity();
  NodeId best = kInvalidNode;

  auto scan_cell = [&](long cx, long cy) {
    if (cx < 0 || cy < 0 || cx >= cols_ || cy >= rows_) return;
    const auto cell = static_cast<std::size_t>(cy * cols_ + cx);
    for (std::uint32_t i = cell_offsets_[cell]; i < cell_offsets_[cell + 1]; ++i) {
      const NodeId v = cell_nodes_[i];
      const double dx = projected_[v].x - q.x;
      const double dy = projected_[v].y - q.y;
      const double d2 = dx * dx + dy * dy;
      if (d2 < best_d2 || (d2 == best_d2 && v < best)) {
        best_d2 = d2;
        best = v;
      }
    }
  };

  // Every cell in ring r+1 is at least r * cell_size away from the query
  // (also when the query lies outside the grid and was clamped), so once the
  // best match is within that bound no further ring can improve it.
  const long max_ring = std::max(cols_, rows_);
  for (long r = 0; r <= max_ring; ++r) {
    if (r == 0) {
      scan_cell(qx, qy);
    } else {
      for (long dx = -r; dx <= r; ++dx) {
        scan_cell(qx + dx, qy - r);
        scan_cell(qx + dx, qy + r);
      }
      for (long dy = -r + 1; dy <= r - 1; ++dy) {
        scan_cell(qx - r, qy + dy);
        scan_cell(qx + r, qy + dy);
      }
    }
    if (best != kInvalidNode && std::sqrt(best_d2) <= static_cast<double>(r) * cell_size_m_) break;
  }

  return Match{best, std::sqrt(best_d2)};
}

}  // namespace umbra
