#pragma once

#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "umbra/osm_loader.hpp"
#include "umbra/road_graph.hpp"
#include "umbra/search_index.hpp"
#include "umbra/shortest_path.hpp"
#include "umbra/spatial_index.hpp"

namespace umbra {

/// A user-facing routing failure (e.g. a click far outside the map).
class RoutingError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct SnappedPoint {
  NodeId node = kInvalidNode;
  LatLng position;
  double snap_distance_m = 0.0;  ///< from the requested point to the node
  std::string label;             ///< e.g. "Market Street & 5th Street"
};

/// One turn-by-turn instruction.
struct RouteStep {
  std::string instruction;  ///< e.g. "Turn left onto Market Street"
  std::string maneuver;     ///< depart | straight | slight-left | left | sharp-left | ... | uturn | arrive
  std::string street;
  double length_m = 0.0;
  double duration_s = 0.0;
  LatLng location;  ///< where the manoeuvre happens
};

struct Route {
  SnappedPoint origin;
  SnappedPoint destination;
  SearchResult search;
  std::vector<LatLng> geometry;
  std::vector<RouteStep> steps;
};

struct RouteServiceOptions {
  double max_snap_distance_m = 750.0;
  std::string unnamed_road = "an unnamed road";  ///< used in directions, e.g. "a walkway"
  std::string unnamed_place = "Unnamed road";    ///< label for a point on unnamed ways, e.g. "Walkway"
};

/// Application facade: snapping, searching and directions on one graph.
/// Thread-safe for concurrent reads (all state is immutable after construction).
class RouteService {
 public:
  /// `places` are optional points of interest for the search box; every named
  /// street and intersection of the graph is always searchable.
  explicit RouteService(RoadGraph graph, std::vector<Place> places = {}, RouteServiceOptions options = {});

  /// Assemble geometry and turn-by-turn steps for a search result between two snapped points.
  [[nodiscard]] Route make_route(SnappedPoint origin, SnappedPoint destination, SearchResult search) const;

  [[nodiscard]] const RoadGraph& graph() const noexcept { return graph_; }

  /// Autocomplete search over places, buildings, streets and intersections. A
  /// query that is exactly a building code ("MU", "coor") lists that building first.
  [[nodiscard]] std::vector<SearchEntry> search(std::string_view query, std::size_t limit = 8) const;

  [[nodiscard]] std::size_t searchable_count() const noexcept { return search_index_.size(); }

  /// Snap a coordinate to the nearest node. Throws RoutingError if it is too far.
  [[nodiscard]] SnappedPoint snap(const LatLng& point) const;

  /// Snap every point that is close enough to the network; points that are too
  /// far are skipped. Throws RoutingError (for the first point) if none snap.
  [[nodiscard]] std::vector<SnappedPoint> snap_all(std::span<const LatLng> points) const;

  /// Compute a route between two coordinates.
  [[nodiscard]] Route route(const LatLng& from, const LatLng& to,
                            const SearchOptions& options = {}) const;

  /// Like make_route(), for a search between sets of snapped points: the
  /// endpoints the search actually used become the route's origin and destination.
  [[nodiscard]] Route make_route(std::span<const SnappedPoint> origins, std::span<const SnappedPoint> destinations,
                                 SearchResult search) const;

  /// Node ids of snapped points, in order.
  [[nodiscard]] static std::vector<NodeId> node_ids(std::span<const SnappedPoint> points);

  /// Human-readable name for a node, built from the streets that meet there.
  [[nodiscard]] std::string intersection_label(NodeId node) const;

 private:
  [[nodiscard]] std::vector<RouteStep> build_steps(const SearchResult& result) const;
  [[nodiscard]] std::string street_name(NameId id) const;
  void build_search_index(std::vector<Place> places);

  RoadGraph graph_;
  SpatialIndex index_;
  double max_snap_distance_m_;
  std::string unnamed_road_;
  std::string unnamed_place_;
  std::vector<std::vector<NameId>> incident_names_;  // per node, distinct street names
  SearchIndex search_index_;
  std::unordered_map<std::string, std::uint32_t> building_codes_;  // lower-case code -> entry id
};

}  // namespace umbra
