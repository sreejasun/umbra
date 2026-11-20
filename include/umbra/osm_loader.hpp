#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/road_graph.hpp"
#include "umbra/shade_map.hpp"

namespace umbra {

/// Thrown when map data cannot be read or is malformed.
class MapLoadError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Which network to build from the extract.
enum class TravelMode {
  kDrive,  ///< cars: road classes, one-way rules, speed limits
  kWalk,   ///< pedestrians: sidewalks, footways, paths, streets; both directions, 5 km/h
};

struct OsmLoadOptions {
  TravelMode mode = TravelMode::kDrive;
  /// Drop nodes that cannot reach (or be reached from) the main network.
  bool keep_largest_component = true;
};

struct OsmLoadStats {
  std::size_t ways_seen = 0;
  std::size_t ways_used = 0;
  std::size_t nodes_before_pruning = 0;
};

/// Build a directed road graph from Overpass API JSON (`[out:json]`).
///
/// - Only drivable `highway=*` classes are used; `access=no|private` and
///   `area=yes` ways are skipped.
/// - `oneway=yes|1|true|-1`, roundabouts and motorways become one-way edges.
/// - Travel time uses `maxspeed` (km/h or mph) or a per-class default speed.
[[nodiscard]] RoadGraph parse_overpass_json(std::string_view json_text,
                                            const OsmLoadOptions& options = {},
                                            OsmLoadStats* stats = nullptr);

[[nodiscard]] RoadGraph load_overpass_file(const std::filesystem::path& path,
                                           const OsmLoadOptions& options = {},
                                           OsmLoadStats* stats = nullptr);

/// A named point of interest (campus, shop, park, station, ...).
struct Place {
  std::string name;
  std::string category;  ///< human-readable, e.g. "Fast food", "Light rail stop"
  std::string keywords;  ///< alternative names (short_name, alt_name), e.g. "ASU"
  LatLng position;
  double importance = 0.5;  ///< search ranking boost
  std::string code;         ///< official building code, e.g. "COOR" (campus buildings)
  /// Where a trip to or from this place may start or end: mapped entrances, or
  /// points on the building outline when no entrance is mapped. Empty means
  /// `position`. Routing picks the one with the cheapest path.
  std::vector<LatLng> access_points;
  bool entrances_mapped = false;  ///< true if access_points are real, mapped entrances
};

/// Parse named places from Overpass JSON (`out center tags`). Utility features
/// such as parking lots or benches are skipped, and duplicates of the same name
/// within 200 m are merged.
[[nodiscard]] std::vector<Place> parse_overpass_places(std::string_view json_text);

[[nodiscard]] std::vector<Place> load_places_file(const std::filesystem::path& path);

/// Parse campus buildings from Overpass JSON (`scripts/fetch_campus.sh`):
/// named buildings inside `amenity=university|college` grounds, with their
/// `ref` code, campus name and entrances (`entrance=yes|main|secondary` nodes
/// on the outline). Buildings without a mapped entrance get up to
/// `kMaxOutlineAccessPoints` evenly spaced outline points instead.
[[nodiscard]] std::vector<Place> parse_campus_buildings(std::string_view json_text);

[[nodiscard]] std::vector<Place> load_campus_file(const std::filesystem::path& path);

/// Parse drinking-water points (`scripts/fetch_water.sh`): fountains, bottle
/// fillers and taps, skipping private ones. Unnamed points are called
/// "Water fountain" or "Bottle filler".
[[nodiscard]] std::vector<Place> parse_water_points(std::string_view json_text);

[[nodiscard]] std::vector<Place> load_water_file(const std::filesystem::path& path);

inline constexpr std::size_t kMaxOutlineAccessPoints = 12;

/// Combine place lists, dropping any `others` entry with the same name
/// (case-insensitive) as a `preferred` entry within 200 m.
[[nodiscard]] std::vector<Place> merge_places(std::vector<Place> preferred, std::vector<Place> others);

/// Parse buildings (with estimated heights) and trees from Overpass JSON (`out geom`).
[[nodiscard]] ShadeCasters parse_shade_casters(std::string_view json_text);

[[nodiscard]] ShadeCasters load_shade_file(const std::filesystem::path& path);

namespace osm {

/// Walking speed for a `highway` class in km/h, or nullopt if pedestrians cannot use it.
[[nodiscard]] std::optional<double> walking_speed_kmh(std::string_view highway);

/// Parse a length such as "12", "12 m", "40 ft" or "40'" to metres.
[[nodiscard]] std::optional<double> parse_length_m(std::string_view value);

/// Parse an OSM `maxspeed` value ("25 mph", "50", "50 km/h") to km/h.
[[nodiscard]] std::optional<double> parse_maxspeed_kmh(std::string_view value);

/// Default free-flow speed for a `highway` class in km/h, or nullopt if the
/// class is not drivable.
[[nodiscard]] std::optional<double> default_speed_kmh(std::string_view highway);

}  // namespace osm
}  // namespace umbra
