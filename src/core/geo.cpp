#include "umbra/geo.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace umbra {
namespace {
constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr double kRadToDeg = 180.0 / std::numbers::pi;
}  // namespace

double haversine_m(const LatLng& a, const LatLng& b) noexcept {
  const double lat1 = a.lat * kDegToRad;
  const double lat2 = b.lat * kDegToRad;
  const double dlat = lat2 - lat1;
  const double dlng = (b.lng - a.lng) * kDegToRad;

  const double s_lat = std::sin(dlat / 2.0);
  const double s_lng = std::sin(dlng / 2.0);
  const double h = s_lat * s_lat + std::cos(lat1) * std::cos(lat2) * s_lng * s_lng;
  return 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(h)));
}

double bearing_deg(const LatLng& from, const LatLng& to) noexcept {
  const double lat1 = from.lat * kDegToRad;
  const double lat2 = to.lat * kDegToRad;
  const double dlng = (to.lng - from.lng) * kDegToRad;

  const double y = std::sin(dlng) * std::cos(lat2);
  const double x = std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dlng);
  const double deg = std::atan2(y, x) * kRadToDeg;
  return std::fmod(deg + 360.0, 360.0);
}

double bearing_delta_deg(double from, double to) noexcept {
  double d = std::fmod(to - from, 360.0);
  if (d > 180.0) d -= 360.0;
  if (d <= -180.0) d += 360.0;
  return d;
}

}  // namespace umbra
