#include "umbra/shade_map.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace umbra {
namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

/// Unit vector pointing from the ground towards the sun (x = east, y = north).
geom::Vec2 toward_sun(const SunPosition& sun) {
  return {std::sin(sun.azimuth_deg * kDeg), std::cos(sun.azimuth_deg * kDeg)};
}

bool sun_blocked_everywhere(const SunPosition& sun) {
  return sun.elevation_deg < ShadeMap::kMinSunElevationDeg;
}

}  // namespace

double ShadeMap::estimate_height_m(const std::string& type, double levels, double area_m2) {
  constexpr double kMetersPerLevel = 3.3;
  if (levels > 0) return levels * kMetersPerLevel + 1.0;  // + parapet / roof
  if (type == "carport" || type == "roof" || type == "canopy") return 3.5;
  if (type == "shed" || type == "garage" || type == "garages") return 3.0;
  if (type == "house" || type == "detached" || type == "residential" || type == "bungalow") return 4.5;
  if (type == "apartments" || type == "dormitory") return 10.0;
  if (type == "university" || type == "college" || type == "office" || type == "hospital") return 12.0;
  if (type == "parking") return 9.0;
  // Generic "building=yes": larger footprints tend to be taller.
  if (area_m2 < 150) return 4.0;
  if (area_m2 < 2000) return 6.0;
  return 9.0;
}

ShadeMap::ShadeMap(ShadeCasters casters, double cell_size_m) : cell_size_m_(cell_size_m) {
  if (!(cell_size_m > 0)) throw std::invalid_argument("ShadeMap: cell size must be positive");
  if (casters.buildings.empty() && casters.trees.empty()) return;

  // Projection origin: first available coordinate (the map spans only a few km).
  origin_ = !casters.buildings.empty() && !casters.buildings.front().ring.empty()
                ? casters.buildings.front().ring.front()
                : casters.trees.front().position;
  m_per_deg_lat_ = kEarthRadiusM * kDeg;
  m_per_deg_lng_ = m_per_deg_lat_ * std::cos(origin_.lat * kDeg);

  constexpr double kInf = std::numeric_limits<double>::infinity();
  geom::Vec2 lo{kInf, kInf};
  geom::Vec2 hi{-kInf, -kInf};
  auto extend = [&](geom::Vec2 p) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
  };

  for (const BuildingFootprint& b : casters.buildings) {
    if (b.ring.size() < 3 || !(b.height_m > 0)) continue;
    Building local{{}, b.height_m, {kInf, kInf}, {-kInf, -kInf}};
    for (const LatLng& p : b.ring) {
      const geom::Vec2 v = to_local(p);
      local.ring.push_back(v);
      local.min = {std::min(local.min.x, v.x), std::min(local.min.y, v.y)};
      local.max = {std::max(local.max.x, v.x), std::max(local.max.y, v.y)};
      extend(v);
    }
    max_building_height_m_ = std::max(max_building_height_m_, b.height_m);
    buildings_.push_back(std::move(local));
  }
  for (const Tree& t : casters.trees) {
    const geom::Vec2 c = to_local(t.position);
    trees_.push_back({c, t.crown_radius_m, t.height_m});
    extend(c);
  }
  if (buildings_.empty() && trees_.empty()) return;

  grid_min_ = lo;
  cols_ = static_cast<long>((hi.x - lo.x) / cell_size_m_) + 1;
  rows_ = static_cast<long>((hi.y - lo.y) / cell_size_m_) + 1;
  building_cells_.resize(static_cast<std::size_t>(cols_ * rows_));
  tree_cells_.resize(static_cast<std::size_t>(cols_ * rows_));

  auto cell_of = [this](double v, double origin) { return static_cast<long>((v - origin) / cell_size_m_); };
  for (std::uint32_t id = 0; id < buildings_.size(); ++id) {
    const Building& b = buildings_[id];
    for (long cy = cell_of(b.min.y, grid_min_.y); cy <= cell_of(b.max.y, grid_min_.y); ++cy) {
      for (long cx = cell_of(b.min.x, grid_min_.x); cx <= cell_of(b.max.x, grid_min_.x); ++cx) {
        building_cells_[static_cast<std::size_t>(cell_index(cx, cy))].push_back(id);
      }
    }
  }
  for (std::uint32_t id = 0; id < trees_.size(); ++id) {
    const geom::Vec2 c = trees_[id].center;
    tree_cells_[static_cast<std::size_t>(cell_index(cell_of(c.x, grid_min_.x), cell_of(c.y, grid_min_.y)))]
        .push_back(id);
  }
}

geom::Vec2 ShadeMap::to_local(const LatLng& p) const noexcept {
  return {(p.lng - origin_.lng) * m_per_deg_lng_, (p.lat - origin_.lat) * m_per_deg_lat_};
}

LatLng ShadeMap::to_geo(geom::Vec2 v) const noexcept {
  return {origin_.lat + v.y / m_per_deg_lat_, origin_.lng + v.x / m_per_deg_lng_};
}

long ShadeMap::cell_index(long cx, long cy) const noexcept { return cy * cols_ + cx; }

// Visit every grid cell crossed by segment a-b (Amanatides & Woo voxel traversal).
// Cells outside the grid are skipped, but the walk continues through them.
template <typename Visit>
void ShadeMap::for_each_cell_on_segment(geom::Vec2 a, geom::Vec2 b, Visit&& visit) const {
  const double x0 = (a.x - grid_min_.x) / cell_size_m_;
  const double y0 = (a.y - grid_min_.y) / cell_size_m_;
  const double x1 = (b.x - grid_min_.x) / cell_size_m_;
  const double y1 = (b.y - grid_min_.y) / cell_size_m_;

  long cx = static_cast<long>(std::floor(x0));
  long cy = static_cast<long>(std::floor(y0));
  const long end_x = static_cast<long>(std::floor(x1));
  const long end_y = static_cast<long>(std::floor(y1));

  const double dx = x1 - x0;
  const double dy = y1 - y0;
  const long step_x = dx > 0 ? 1 : -1;
  const long step_y = dy > 0 ? 1 : -1;
  constexpr double kInf = std::numeric_limits<double>::infinity();
  double t_max_x = dx != 0 ? (dx > 0 ? (static_cast<double>(cx) + 1 - x0) : (x0 - static_cast<double>(cx))) / std::abs(dx) : kInf;
  double t_max_y = dy != 0 ? (dy > 0 ? (static_cast<double>(cy) + 1 - y0) : (y0 - static_cast<double>(cy))) / std::abs(dy) : kInf;
  const double t_delta_x = dx != 0 ? 1.0 / std::abs(dx) : kInf;
  const double t_delta_y = dy != 0 ? 1.0 / std::abs(dy) : kInf;

  long remaining = std::abs(end_x - cx) + std::abs(end_y - cy) + 1;
  while (remaining-- > 0) {
    if (cx >= 0 && cy >= 0 && cx < cols_ && cy < rows_) visit(cell_index(cx, cy));
    if (cx == end_x && cy == end_y) break;
    if (t_max_x < t_max_y) {
      cx += step_x;
      t_max_x += t_delta_x;
    } else {
      cy += step_y;
      t_max_y += t_delta_y;
    }
  }
}

bool ShadeMap::is_shaded(const LatLng& point, const SunPosition& sun) const {
  if (sun_blocked_everywhere(sun)) return true;
  if (buildings_.empty() && trees_.empty()) return false;
  return is_shaded_local(to_local(point), sun);
}

bool ShadeMap::is_shaded_local(geom::Vec2 p, const SunPosition& sun) const {
  const geom::Vec2 dir = toward_sun(sun);
  const double tan_elevation = std::tan(sun.elevation_deg * kDeg);

  // Buildings: does the ray towards the sun pass through a footprint below roof height?
  if (!buildings_.empty()) {
    const double reach = std::min(max_building_height_m_ / tan_elevation, kMaxShadowLengthM);
    std::vector<std::uint32_t> tested;
    bool shaded = false;
    for_each_cell_on_segment(p, p + dir * reach, [&](long cell) {
      if (shaded) return;
      for (std::uint32_t id : building_cells_[static_cast<std::size_t>(cell)]) {
        if (std::find(tested.begin(), tested.end(), id) != tested.end()) continue;
        tested.push_back(id);
        const Building& b = buildings_[id];
        const geom::Vec2 end = p + dir * std::min(b.height_m / tan_elevation, kMaxShadowLengthM);
        // Cheap bounding-box rejection before the exact polygon test.
        if (std::max(p.x, end.x) < b.min.x || std::min(p.x, end.x) > b.max.x ||
            std::max(p.y, end.y) < b.min.y || std::min(p.y, end.y) > b.max.y) {
          continue;
        }
        if (geom::segment_hits_polygon(p, end, b.ring)) {
          shaded = true;
          return;
        }
      }
    });
    if (shaded) return true;
  }

  // Trees: the crown's shadow is a disc displaced away from the sun. Equivalently,
  // P is shaded if the point P + d * (crown height / tan e) lies within a crown.
  if (!trees_.empty()) {
    const auto cell_of = [this](double v, double origin) {
      return static_cast<long>(std::floor((v - origin) / cell_size_m_));
    };
    // Crown centre at ~65% of tree height; heights vary, so probe per tree.
    // Search the strip between P and the farthest probe point (tallest trees ~18 m).
    const double max_offset = std::min(12.0 / tan_elevation, kMaxShadowLengthM);
    const geom::Vec2 far = p + dir * max_offset;
    const long x_lo = cell_of(std::min(p.x, far.x) - 10, grid_min_.x);
    const long x_hi = cell_of(std::max(p.x, far.x) + 10, grid_min_.x);
    const long y_lo = cell_of(std::min(p.y, far.y) - 10, grid_min_.y);
    const long y_hi = cell_of(std::max(p.y, far.y) + 10, grid_min_.y);
    for (long cy = std::max(0L, y_lo); cy <= std::min(rows_ - 1, y_hi); ++cy) {
      for (long cx = std::max(0L, x_lo); cx <= std::min(cols_ - 1, x_hi); ++cx) {
        for (std::uint32_t id : tree_cells_[static_cast<std::size_t>(cell_index(cx, cy))]) {
          const LocalTree& t = trees_[id];
          const double offset = std::min(0.65 * t.height_m / tan_elevation, kMaxShadowLengthM);
          const geom::Vec2 probe = p + dir * offset;
          const double ddx = probe.x - t.center.x;
          const double ddy = probe.y - t.center.y;
          if (ddx * ddx + ddy * ddy <= t.radius_m * t.radius_m) return true;
        }
      }
    }
  }
  return false;
}

double ShadeMap::sun_exposure(const LatLng& a, const LatLng& b, const SunPosition& sun) const {
  if (sun_blocked_everywhere(sun)) return 0.0;
  if (buildings_.empty() && trees_.empty()) return 1.0;

  const geom::Vec2 la = to_local(a);
  const geom::Vec2 lb = to_local(b);
  const double length = std::hypot(lb.x - la.x, lb.y - la.y);
  const auto samples = std::max<long>(1, static_cast<long>(std::ceil(length / kSampleSpacingM)));

  long sunny = 0;
  for (long i = 0; i < samples; ++i) {
    const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(samples);  // sample midpoints
    if (!is_shaded_local(la + (lb - la) * t, sun)) ++sunny;
  }
  return static_cast<double>(sunny) / static_cast<double>(samples);
}

std::vector<ShadeMap::ShadowShape> ShadeMap::shadows(const BoundingBox& view, const SunPosition& sun,
                                                     std::size_t limit) const {
  std::vector<ShadowShape> shapes;
  if (buildings_.empty()) return shapes;

  const geom::Vec2 lo = to_local({view.min_lat, view.min_lng});
  const geom::Vec2 hi = to_local({view.max_lat, view.max_lng});
  auto clamp_col = [this](double v) {
    return std::clamp(static_cast<long>((v - grid_min_.x) / cell_size_m_), 0L, cols_ - 1);
  };
  auto clamp_row = [this](double v) {
    return std::clamp(static_cast<long>((v - grid_min_.y) / cell_size_m_), 0L, rows_ - 1);
  };

  const bool daylight = !sun_blocked_everywhere(sun);
  const geom::Vec2 away = toward_sun(sun) * -1.0;
  const double tan_elevation = std::tan(sun.elevation_deg * kDeg);

  std::vector<bool> seen(buildings_.size(), false);
  for (long cy = clamp_row(lo.y); cy <= clamp_row(hi.y); ++cy) {
    for (long cx = clamp_col(lo.x); cx <= clamp_col(hi.x); ++cx) {
      for (std::uint32_t id : building_cells_[static_cast<std::size_t>(cell_index(cx, cy))]) {
        if (seen[id]) continue;
        seen[id] = true;
        const Building& b = buildings_[id];
        if (b.max.x < lo.x || b.min.x > hi.x || b.max.y < lo.y || b.min.y > hi.y) continue;

        ShadowShape shape;
        for (geom::Vec2 v : b.ring) shape.footprint.push_back(to_geo(v));
        if (daylight) {
          // A prism's shadow is the union of the footprint swept along the
          // shadow direction; its convex hull is a close, cheap outline.
          const geom::Vec2 offset = away * std::min(b.height_m / tan_elevation, kMaxShadowLengthM);
          std::vector<geom::Vec2> points = b.ring;
          for (geom::Vec2 v : b.ring) points.push_back(v + offset);
          for (geom::Vec2 v : geom::convex_hull(std::move(points))) shape.shadow.push_back(to_geo(v));
        }
        shapes.push_back(std::move(shape));
        if (shapes.size() >= limit) return shapes;
      }
    }
  }
  return shapes;
}

}  // namespace umbra
