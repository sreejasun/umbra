#include "umbra/solar.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace umbra {
namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

double sin_d(double deg) { return std::sin(deg * kDeg); }
double cos_d(double deg) { return std::cos(deg * kDeg); }
double tan_d(double deg) { return std::tan(deg * kDeg); }
double acos_d(double x) { return std::acos(std::clamp(x, -1.0, 1.0)) / kDeg; }
double asin_d(double x) { return std::asin(std::clamp(x, -1.0, 1.0)) / kDeg; }
double wrap360(double deg) { return deg - 360.0 * std::floor(deg / 360.0); }

/// Atmospheric refraction correction in degrees (NOAA approximation).
double refraction(double elevation) {
  if (elevation > 85.0) return 0.0;
  const double te = tan_d(elevation);
  double arcsec;
  if (elevation > 5.0) {
    arcsec = 58.1 / te - 0.07 / (te * te * te) + 0.000086 / std::pow(te, 5);
  } else if (elevation > -0.575) {
    arcsec = 1735.0 + elevation * (-518.2 + elevation * (103.4 + elevation * (-12.79 + elevation * 0.711)));
  } else {
    arcsec = -20.774 / te;
  }
  return arcsec / 3600.0;
}

}  // namespace

SunPosition sun_position(const LatLng& where, std::int64_t unix_seconds) noexcept {
  const double julian_day = static_cast<double>(unix_seconds) / 86400.0 + 2440587.5;
  const double t = (julian_day - 2451545.0) / 36525.0;  // Julian centuries since J2000.0

  // Sun's orbit.
  const double mean_longitude = wrap360(280.46646 + t * (36000.76983 + t * 0.0003032));
  const double mean_anomaly = 357.52911 + t * (35999.05029 - 0.0001537 * t);
  const double eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const double equation_of_center = sin_d(mean_anomaly) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                                    sin_d(2 * mean_anomaly) * (0.019993 - 0.000101 * t) +
                                    sin_d(3 * mean_anomaly) * 0.000289;
  const double true_longitude = mean_longitude + equation_of_center;
  const double omega = 125.04 - 1934.136 * t;
  const double apparent_longitude = true_longitude - 0.00569 - 0.00478 * sin_d(omega);

  // Earth's axial tilt and the sun's declination.
  const double mean_obliquity =
      23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
  const double obliquity = mean_obliquity + 0.00256 * cos_d(omega);
  const double declination = asin_d(sin_d(obliquity) * sin_d(apparent_longitude));

  // Equation of time (minutes): difference between solar and clock time.
  const double y = tan_d(obliquity / 2) * tan_d(obliquity / 2);
  const double equation_of_time =
      4.0 / kDeg *
      (y * sin_d(2 * mean_longitude) - 2 * eccentricity * sin_d(mean_anomaly) +
       4 * eccentricity * y * sin_d(mean_anomaly) * cos_d(2 * mean_longitude) -
       0.5 * y * y * sin_d(4 * mean_longitude) - 1.25 * eccentricity * eccentricity * sin_d(2 * mean_anomaly));

  // Local hour angle.
  const double seconds_of_day = static_cast<double>(((unix_seconds % 86400) + 86400) % 86400);
  const double true_solar_minutes = wrap360((seconds_of_day / 60.0 + equation_of_time + 4.0 * where.lng) / 4.0) * 4.0;
  const double hour_angle = true_solar_minutes / 4.0 < 0 ? true_solar_minutes / 4.0 + 180.0
                                                         : true_solar_minutes / 4.0 - 180.0;

  // Horizontal coordinates.
  const double zenith = acos_d(sin_d(where.lat) * sin_d(declination) +
                               cos_d(where.lat) * cos_d(declination) * cos_d(hour_angle));
  const double elevation = 90.0 - zenith;

  const double denominator = cos_d(where.lat) * sin_d(zenith);
  double azimuth = 180.0;
  if (std::abs(denominator) > 1e-12) {
    const double a = acos_d((sin_d(where.lat) * cos_d(zenith) - sin_d(declination)) / denominator);
    azimuth = hour_angle > 0 ? wrap360(a + 180.0) : wrap360(540.0 - a);
  }

  return {azimuth, elevation + refraction(elevation)};
}

bool sun_glare(double heading_deg, const SunPosition& sun, double max_elevation_deg) noexcept {
  if (!sun.is_up() || sun.elevation_deg > max_elevation_deg) return false;
  return std::abs(bearing_delta_deg(heading_deg, sun.azimuth_deg)) <= kGlareMaxAzimuthOffsetDeg;
}

}  // namespace umbra
