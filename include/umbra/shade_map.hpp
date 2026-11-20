#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "umbra/geo.hpp"
#include "umbra/geometry2d.hpp"
#include "umbra/solar.hpp"

namespace umbra {

/// A building (or shade roof / carport) footprint extruded to a height.
struct BuildingFootprint {
  std::vector<LatLng> ring;  ///< outer boundary
  double height_m = 6.0;
};

/// A tree, modelled as a disc-shaped crown.
struct Tree {
  LatLng position;
  double crown_radius_m = 3.5;
  double height_m = 8.0;
};

struct ShadeCasters {
  std::vector<BuildingFootprint> buildings;
  std::vector<Tree> trees;
};

/**
 * Answers "is this point in shade at this moment?" from building and tree geometry.
 *
 * Buildings are vertical prisms. A ground point P is shaded by a building of
 * height h iff the ray from P towards the sun enters the footprint while still
 * below h, i.e. iff the horizontal segment
 *
 *     P  ->  P + d * h / tan(elevation)          (d = unit vector towards the sun)
 *
 * intersects the footprint polygon. This is exact for any (also non-convex)
 * footprint. Trees cast a disc-shaped shadow displaced away from the sun.
 *
 * Candidate buildings are found with a uniform grid: the cells crossed by the
 * segment are enumerated with a 2-D DDA (Amanatides & Woo), so a query only
 * tests buildings that lie in the sun's direction.
 */
class ShadeMap {
 public:
  /// Below this elevation the sun is effectively blocked everywhere (dusk, dawn, night).
  static constexpr double kMinSunElevationDeg = 2.0;
  /// Longest shadow considered, which bounds the work per query at low sun.
  static constexpr double kMaxShadowLengthM = 250.0;
  /// Spacing of the sample points used to measure a segment's sun exposure.
  static constexpr double kSampleSpacingM = 5.0;

  explicit ShadeMap(ShadeCasters casters, double cell_size_m = 40.0);

  [[nodiscard]] std::size_t building_count() const noexcept { return buildings_.size(); }
  [[nodiscard]] std::size_t tree_count() const noexcept { return trees_.size(); }

  /// Whether direct sunlight is blocked at `point`.
  [[nodiscard]] bool is_shaded(const LatLng& point, const SunPosition& sun) const;

  /// Fraction [0, 1] of the straight segment a-b that is in direct sun.
  [[nodiscard]] double sun_exposure(const LatLng& a, const LatLng& b, const SunPosition& sun) const;

  /// Footprints and shadow outlines of buildings inside `view`, for drawing.
  struct ShadowShape {
    std::vector<LatLng> footprint;
    std::vector<LatLng> shadow;  ///< convex hull of the footprint and its projection; empty at night
  };
  [[nodiscard]] std::vector<ShadowShape> shadows(const BoundingBox& view, const SunPosition& sun,
                                                 std::size_t limit) const;

  /// Height guess for an untagged building: by type, then by floor count, then by footprint area.
  [[nodiscard]] static double estimate_height_m(const std::string& building_type, double levels,
                                                double footprint_area_m2);

 private:
  struct Building {
    std::vector<geom::Vec2> ring;
    double height_m;
    geom::Vec2 min;
    geom::Vec2 max;
  };
  struct LocalTree {
    geom::Vec2 center;
    double radius_m;
    double height_m;
  };

  [[nodiscard]] geom::Vec2 to_local(const LatLng& p) const noexcept;
  [[nodiscard]] LatLng to_geo(geom::Vec2 v) const noexcept;
  [[nodiscard]] bool is_shaded_local(geom::Vec2 p, const SunPosition& sun) const;
  [[nodiscard]] long cell_index(long cx, long cy) const noexcept;

  template <typename Visit>
  void for_each_cell_on_segment(geom::Vec2 a, geom::Vec2 b, Visit&& visit) const;

  std::vector<Building> buildings_;
  std::vector<LocalTree> trees_;
  double max_building_height_m_ = 0.0;

  // Local equirectangular projection.
  LatLng origin_;
  double m_per_deg_lat_ = 0.0;
  double m_per_deg_lng_ = 0.0;

  // Uniform grid (cell -> ids), stored as vectors of ids per cell.
  double cell_size_m_;
  geom::Vec2 grid_min_;
  long cols_ = 0;
  long rows_ = 0;
  std::vector<std::vector<std::uint32_t>> building_cells_;
  std::vector<std::vector<std::uint32_t>> tree_cells_;
};

}  // namespace umbra
