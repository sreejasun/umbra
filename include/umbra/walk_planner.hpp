#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <memory>
#include <span>
#include <vector>

#include "umbra/route_service.hpp"
#include "umbra/shade_map.hpp"
#include "umbra/solar.hpp"

namespace umbra {

/// How strongly the walker wants to avoid direct sun.
enum class ShadePreference {
  kShortest,  ///< ignore the sun
  kBalanced,  ///< a metre in the sun costs 2.5 metres
  kMaxShade,  ///< a metre in the sun costs 6 metres
};

/// Extra cost per metre walked in direct sun, on top of the metre itself.
[[nodiscard]] double sun_penalty(ShadePreference preference) noexcept;

/// A stretch of route that is entirely shaded or entirely sunny (for drawing).
struct ShadeRun {
  bool shaded = false;
  std::vector<LatLng> points;
};

struct WalkRoute {
  Route route;
  double shaded_length_m = 0.0;
  double lit_length_m = 0.0;    ///< on ways tagged with street lighting
  double unlit_length_m = 0.0;  ///< on ways tagged lit=no
  std::vector<ShadeRun> runs;

  [[nodiscard]] double shade_fraction() const noexcept {
    return route.search.length_m > 0 ? shaded_length_m / route.search.length_m : 1.0;
  }
};

/// A place to stop on the way, e.g. a drinking-water fountain.
struct WalkStop {
  std::string name;
  LatLng position;
};

/// A stop farther than this from every walkway is ignored.
inline constexpr double kMaxStopDistanceM = 60.0;

/// Light preference after dark: a metre on an unlit way costs 3 m, on a way
/// with unknown lighting 1.25 m, on a lit way 1 m.
inline constexpr double kUnlitPenalty = 3.0;
inline constexpr double kUnknownLightingPenalty = 1.25;

struct WalkOptions {
  std::int64_t unix_time = 0;  ///< when the walk happens (UTC seconds)
  ShadePreference preference = ShadePreference::kBalanced;
  Algorithm algorithm = Algorithm::kDijkstra;
  bool record_settled = false;
  /// Strength of direct sunshine in [0, 1] (1 = cloudless, 0 = overcast), e.g.
  /// from direct_sun_strength() on live weather. Scales the sun penalty.
  double sun_strength = 1.0;
  bool avoid_stairs = false;  ///< step-free route (wheelchair, stroller): never use stairs
  bool prefer_lit = true;     ///< after dark, prefer lit ways
};

struct WalkPlan {
  SunPosition sun;
  ShadePreference preference = ShadePreference::kBalanced;
  double sun_strength = 1.0;  ///< the value used (clamped to [0, 1])
  bool lighting_applied = false;  ///< the lit-way preference was used (night)
  WalkRoute preferred;  ///< the route optimised for the chosen preferences
  WalkRoute shortest;   ///< the plain shortest walk, for comparison
  std::optional<WalkStop> via;  ///< set by plan_via_stop()
};

/// One candidate departure time, for "best time to leave".
struct DepartureOption {
  std::int64_t unix_time = 0;
  SunPosition sun;
  double sun_strength = 1.0;
  bool found = false;
  double length_m = 0.0;
  double duration_s = 0.0;
  double shade_fraction = 0.0;
};

/**
 * Shade-aware pedestrian routing.
 *
 * Edge cost = length * (1 + penalty * sun_strength * sun_exposure) by day,
 * where sun_exposure in [0, 1] is the share of the segment in direct sun at
 * the requested time (covered walkways are always 0) and sun_strength in
 * [0, 1] says how strong the sunshine is (clouds weaken it; overcast removes
 * shadows). After dark the lighting penalty applies instead. Stairs are
 * impassable with `avoid_stairs`. Exposure is computed lazily, only for edges
 * the search actually relaxes. Because cost >= length, the straight-line
 * distance remains an admissible A* heuristic.
 */
class WalkPlanner {
 public:
  /// `stops` are optional places to stop on the way (drinking water); ones more
  /// than kMaxStopDistanceM from the walking network are ignored.
  WalkPlanner(const RouteService& network, std::shared_ptr<const ShadeMap> shade, std::vector<WalkStop> stops = {});

  [[nodiscard]] WalkPlan plan(const LatLng& from, const LatLng& to, const WalkOptions& options) const;

  /// Walk from any of `from` to any of `to` (e.g. to the best entrance of a building).
  [[nodiscard]] WalkPlan plan(std::span<const LatLng> from, std::span<const LatLng> to,
                              const WalkOptions& options) const;

  /**
   * The same walk with one stop: the stop s minimising cost(from, s) +
   * cost(s, to) under the walk's own cost, found with two multi-source
   * Dijkstra runs (one from the origins, one from the destinations; the walking
   * network is symmetric) instead of one route per candidate.
   * Throws RoutingError if there are no stops or none is reachable.
   */
  [[nodiscard]] WalkPlan plan_via_stop(std::span<const LatLng> from, std::span<const LatLng> to,
                                       const WalkOptions& options) const;

  /// The preferred walk at each of `times`; `sun_strength_at` gives the live
  /// sun strength for a time (1 if unknown).
  [[nodiscard]] std::vector<DepartureOption> compare_departures(
      std::span<const LatLng> from, std::span<const LatLng> to, std::span<const std::int64_t> times,
      const WalkOptions& options, const std::function<double(std::int64_t)>& sun_strength_at) const;

  /// Sun position over the centre of the map at `unix_time`.
  [[nodiscard]] SunPosition sun_at(std::int64_t unix_time) const;

  [[nodiscard]] const RouteService& network() const noexcept { return network_; }
  [[nodiscard]] const ShadeMap& shade_map() const noexcept { return *shade_; }
  [[nodiscard]] std::size_t stop_count() const noexcept { return stops_.size(); }

 private:
  struct Costs;
  [[nodiscard]] Costs make_costs(const WalkOptions& options, const SunPosition& sun, double sun_strength) const;
  [[nodiscard]] WalkRoute describe(Route route, const SunPosition& sun) const;

  const RouteService& network_;
  std::shared_ptr<const ShadeMap> shade_;
  std::vector<WalkStop> stops_;    // only stops that snapped
  std::vector<NodeId> stop_nodes_;  // parallel to stops_
};

}  // namespace umbra
