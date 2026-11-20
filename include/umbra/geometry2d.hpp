#pragma once

// Small planar-geometry toolkit (local metric coordinates: x = east, y = north).

#include <algorithm>
#include <vector>

namespace umbra::geom {

struct Vec2 {
  double x = 0.0;
  double y = 0.0;

  friend Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
  friend Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
  friend Vec2 operator*(Vec2 a, double s) noexcept { return {a.x * s, a.y * s}; }
  friend bool operator==(Vec2 a, Vec2 b) noexcept { return a.x == b.x && a.y == b.y; }
};

/// z-component of the cross product (b - a) x (c - a): > 0 if a, b, c turn counter-clockwise.
[[nodiscard]] inline double cross(Vec2 a, Vec2 b, Vec2 c) noexcept {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/// Do closed segments [p1, p2] and [q1, q2] intersect (including touching)?
[[nodiscard]] inline bool segments_intersect(Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2) noexcept {
  const double d1 = cross(q1, q2, p1);
  const double d2 = cross(q1, q2, p2);
  const double d3 = cross(p1, p2, q1);
  const double d4 = cross(p1, p2, q2);
  if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
    return true;
  }
  auto on_segment = [](Vec2 a, Vec2 b, Vec2 p) {
    return std::min(a.x, b.x) <= p.x && p.x <= std::max(a.x, b.x) && std::min(a.y, b.y) <= p.y &&
           p.y <= std::max(a.y, b.y);
  };
  return (d1 == 0 && on_segment(q1, q2, p1)) || (d2 == 0 && on_segment(q1, q2, p2)) ||
         (d3 == 0 && on_segment(p1, p2, q1)) || (d4 == 0 && on_segment(p1, p2, q2));
}

/// Even-odd ray casting. `ring` may be open or closed (first == last).
[[nodiscard]] inline bool point_in_polygon(Vec2 p, const std::vector<Vec2>& ring) noexcept {
  bool inside = false;
  const std::size_t n = ring.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2 a = ring[i];
    const Vec2 b = ring[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

/// Does segment [a, b] touch the polygon (cross an edge or lie inside it)?
[[nodiscard]] inline bool segment_hits_polygon(Vec2 a, Vec2 b, const std::vector<Vec2>& ring) noexcept {
  if (ring.size() < 3) return false;
  if (point_in_polygon(a, ring)) return true;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    if (segments_intersect(a, b, ring[j], ring[i])) return true;
  }
  return false;
}

/// Convex hull (Andrew's monotone chain), counter-clockwise, O(n log n).
[[nodiscard]] inline std::vector<Vec2> convex_hull(std::vector<Vec2> points) {
  std::sort(points.begin(), points.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  points.erase(std::unique(points.begin(), points.end()), points.end());
  if (points.size() < 3) return points;

  std::vector<Vec2> hull(2 * points.size());
  std::size_t k = 0;
  for (const Vec2& p : points) {  // lower hull
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], p) <= 0) --k;
    hull[k++] = p;
  }
  for (std::size_t i = points.size() - 1, lower = k + 1; i-- > 0;) {  // upper hull
    while (k >= lower && cross(hull[k - 2], hull[k - 1], points[i]) <= 0) --k;
    hull[k++] = points[i];
  }
  hull.resize(k - 1);  // last point equals the first
  return hull;
}

}  // namespace umbra::geom
