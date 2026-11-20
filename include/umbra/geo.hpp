#pragma once

namespace umbra {

/// A WGS-84 coordinate in decimal degrees.
struct LatLng {
  double lat = 0.0;
  double lng = 0.0;
};

/// Mean Earth radius in metres (IUGG).
inline constexpr double kEarthRadiusM = 6'371'008.8;

/// Great-circle distance between two coordinates, in metres.
[[nodiscard]] double haversine_m(const LatLng& a, const LatLng& b) noexcept;

/// Initial compass bearing from `from` to `to`, in degrees within [0, 360).
[[nodiscard]] double bearing_deg(const LatLng& from, const LatLng& to) noexcept;

/// Signed smallest difference `to - from` between two bearings, within (-180, 180].
/// Positive means a clockwise (right-hand) turn.
[[nodiscard]] double bearing_delta_deg(double from, double to) noexcept;

/// Axis-aligned bounding box.
struct BoundingBox {
  double min_lat = 0.0;
  double min_lng = 0.0;
  double max_lat = 0.0;
  double max_lng = 0.0;

  [[nodiscard]] LatLng center() const noexcept {
    return {(min_lat + max_lat) / 2.0, (min_lng + max_lng) / 2.0};
  }
};

}  // namespace umbra
