#pragma once

#include <cstdint>

#include "umbra/geo.hpp"

namespace umbra {

/// Apparent position of the sun in the sky.
struct SunPosition {
  double azimuth_deg = 0.0;    ///< compass direction of the sun, clockwise from north, [0, 360)
  double elevation_deg = 0.0;  ///< angle above the horizon (with atmospheric refraction); < 0 at night

  [[nodiscard]] bool is_up() const noexcept { return elevation_deg > 0.0; }
};

/**
 * Sun position for a place and instant, using the NOAA Solar Calculator
 * algorithm (Meeus, "Astronomical Algorithms"). Accurate to about 0.01 degrees
 * for years 1800-2100, far finer than the building data it is used with.
 *
 * @param where         observer location
 * @param unix_seconds  instant in UTC (seconds since 1970-01-01T00:00:00Z)
 */
[[nodiscard]] SunPosition sun_position(const LatLng& where, std::int64_t unix_seconds) noexcept;

/// Sun glare thresholds: the sun is low and close to the driver's line of sight.
inline constexpr double kGlareMaxElevationDeg = 20.0;
/// For drivers whose eyes sit below the lowered visor (short drivers, low
/// seats), the sun reaches the eyes up to a higher elevation.
inline constexpr double kGlareMaxElevationLowSeatDeg = 35.0;
inline constexpr double kGlareMaxAzimuthOffsetDeg = 25.0;

/**
 * Does a driver heading `heading_deg` (clockwise from north) look into a low
 * sun? True when the sun is up, at most `max_elevation_deg` high and within
 * 25 degrees of the heading: the conditions under which the sun shines
 * through the windshield below the visor.
 */
[[nodiscard]] bool sun_glare(double heading_deg, const SunPosition& sun,
                             double max_elevation_deg = kGlareMaxElevationDeg) noexcept;

}  // namespace umbra
