#include "umbra/route_service.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace umbra {
namespace {

constexpr std::uint32_t kAmbiguousCode = std::numeric_limits<std::uint32_t>::max();

const char* cardinal(double bearing) {
  static constexpr const char* kNames[] = {"north", "northeast", "east", "southeast",
                                           "south", "southwest", "west", "northwest"};
  const auto index = static_cast<int>(std::lround(bearing / 45.0)) % 8;
  return kNames[index];
}

struct Maneuver {
  const char* type;
  const char* verb;
};

/// Classify a heading change into a driving manoeuvre. Positive = right.
Maneuver classify_turn(double delta) {
  const double a = std::abs(delta);
  const bool right = delta > 0;
  if (a < 20) return {"straight", "Continue onto"};
  if (a < 60) return right ? Maneuver{"slight-right", "Bear right onto"}
                           : Maneuver{"slight-left", "Bear left onto"};
  if (a < 135) return right ? Maneuver{"right", "Turn right onto"} : Maneuver{"left", "Turn left onto"};
  if (a < 170) return right ? Maneuver{"sharp-right", "Make a sharp right onto"}
                            : Maneuver{"sharp-left", "Make a sharp left onto"};
  return {"uturn", "Make a U-turn onto"};
}

}  // namespace

RouteService::RouteService(RoadGraph graph, std::vector<Place> places, RouteServiceOptions options)
    : graph_(std::move(graph)),
      index_(graph_),
      max_snap_distance_m_(options.max_snap_distance_m),
      unnamed_road_(std::move(options.unnamed_road)),
      unnamed_place_(std::move(options.unnamed_place)) {
  if (graph_.empty()) throw std::invalid_argument("RouteService: road graph is empty");

  // Distinct street names touching each node, used for intersection labels.
  incident_names_.resize(graph_.node_count());
  auto add = [this](NodeId v, NameId name) {
    if (name == 0) return;
    auto& names = incident_names_[v];
    if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
  };
  for (NodeId u = 0; u < graph_.node_count(); ++u) {
    for (const Edge& e : graph_.out_edges(u)) {
      add(u, e.name);
      add(e.target, e.name);
    }
  }

  build_search_index(std::move(places));
}

void RouteService::build_search_index(std::vector<Place> places) {
  // Places: keep those within reach of the road network, described by category
  // and the nearest intersection ("Fast food · near Mill Avenue & 5th Street").
  for (Place& place : places) {
    const auto match = index_.nearest(place.position);
    if (!match || match->distance_m > max_snap_distance_m_) continue;
    const bool building = !place.access_points.empty();
    std::string subtitle = place.category;
    if (!place.code.empty()) subtitle += " · " + place.code;
    subtitle += " · near " + intersection_label(match->node);
    SearchEntry entry{building ? EntryKind::kBuilding : EntryKind::kPlace,
                      std::move(place.name),
                      std::move(subtitle),
                      std::move(place.keywords),
                      place.position,
                      place.importance};
    entry.code = std::move(place.code);
    entry.access_points = std::move(place.access_points);
    entry.entrances_mapped = place.entrances_mapped;
    if (entry.code.size() >= 2) {
      std::string code = entry.code;
      std::transform(code.begin(), code.end(), code.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      const auto id = static_cast<std::uint32_t>(search_index_.size());
      // A code shared by two buildings is ambiguous: leave it to the ranked search.
      if (!building_codes_.emplace(code, id).second) building_codes_[code] = kAmbiguousCode;
    }
    search_index_.add(std::move(entry));
  }

  // Streets: one entry per name, positioned at the street's node closest to its centroid.
  const std::size_t name_count = [this] {
    NameId max_id = 0;
    for (const auto& names : incident_names_) {
      for (NameId n : names) max_id = std::max(max_id, n);
    }
    return static_cast<std::size_t>(max_id) + 1;
  }();
  std::vector<std::vector<NodeId>> nodes_by_name(name_count);
  for (NodeId v = 0; v < graph_.node_count(); ++v) {
    for (NameId n : incident_names_[v]) nodes_by_name[n].push_back(v);
  }
  for (NameId n = 1; n < name_count; ++n) {
    const auto& nodes = nodes_by_name[n];
    if (nodes.empty()) continue;
    LatLng centroid{0, 0};
    for (NodeId v : nodes) {
      centroid.lat += graph_.node(v).position.lat;
      centroid.lng += graph_.node(v).position.lng;
    }
    centroid.lat /= static_cast<double>(nodes.size());
    centroid.lng /= static_cast<double>(nodes.size());
    const NodeId anchor = *std::min_element(nodes.begin(), nodes.end(), [&](NodeId a, NodeId b) {
      return haversine_m(graph_.node(a).position, centroid) < haversine_m(graph_.node(b).position, centroid);
    });
    search_index_.add({EntryKind::kStreet, graph_.name(n), "Street", "", graph_.node(anchor).position, 0.8});
  }

  // Intersections: one entry per distinct pair of crossing street names.
  std::unordered_set<std::uint64_t> seen_pairs;
  for (NodeId v = 0; v < graph_.node_count(); ++v) {
    const auto& names = incident_names_[v];
    for (std::size_t i = 0; i < names.size(); ++i) {
      for (std::size_t j = i + 1; j < names.size(); ++j) {
        NameId a = names[i];
        NameId b = names[j];
        if (graph_.name(b) < graph_.name(a)) std::swap(a, b);
        const std::uint64_t key = (static_cast<std::uint64_t>(a) << 32) | b;
        if (!seen_pairs.insert(key).second) continue;
        search_index_.add({EntryKind::kIntersection, graph_.name(a) + " & " + graph_.name(b),
                           "Intersection", "", graph_.node(v).position, 0.3});
      }
    }
  }

  search_index_.build();
}

std::vector<SearchEntry> RouteService::search(std::string_view query, std::size_t limit) const {
  std::vector<SearchEntry> results;
  if (limit == 0) return results;

  // Exact building code: "MU" means the Memorial Union, ahead of "Music Building".
  std::optional<std::uint32_t> code_hit;
  std::string code;
  for (const char ch : query) {
    if (!std::isspace(static_cast<unsigned char>(ch))) code += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (const auto it = building_codes_.find(code); it != building_codes_.end() && it->second != kAmbiguousCode) {
    code_hit = it->second;
    results.push_back(search_index_.entry(*code_hit));
  }

  for (const SearchHit& hit : search_index_.search(query, limit)) {
    if (results.size() >= limit) break;
    if (code_hit && hit.entry == *code_hit) continue;
    results.push_back(search_index_.entry(hit.entry));
  }
  return results;
}

std::string RouteService::street_name(NameId id) const {
  const std::string& name = graph_.name(id);
  return name.empty() ? unnamed_road_ : name;
}

std::string RouteService::intersection_label(NodeId node) const {
  const auto& names = incident_names_.at(node);
  if (names.size() == 1) return graph_.name(names[0]);
  if (names.size() >= 2) return graph_.name(names[0]) + " & " + graph_.name(names[1]);

  // Unnamed footpath or service road: describe it by the nearest named street,
  // found with a small breadth-first search ("Walkway near East Lemon Street").
  constexpr std::size_t kMaxVisited = 200;
  std::vector<NodeId> queue{node};
  std::unordered_set<NodeId> seen{node};
  for (std::size_t head = 0; head < queue.size() && seen.size() < kMaxVisited; ++head) {
    for (const Edge& e : graph_.out_edges(queue[head])) {
      if (!incident_names_[e.target].empty()) {
        return unnamed_place_ + " near " + graph_.name(incident_names_[e.target].front());
      }
      if (seen.insert(e.target).second) queue.push_back(e.target);
    }
  }
  return unnamed_place_;
}

std::vector<NodeId> RouteService::node_ids(std::span<const SnappedPoint> points) {
  std::vector<NodeId> ids;
  ids.reserve(points.size());
  for (const SnappedPoint& p : points) ids.push_back(p.node);
  return ids;
}

SnappedPoint RouteService::snap(const LatLng& point) const {
  if (!std::isfinite(point.lat) || !std::isfinite(point.lng) || std::abs(point.lat) > 90 ||
      std::abs(point.lng) > 180) {
    throw RoutingError("Coordinates are out of range");
  }
  const auto match = index_.nearest(point);
  if (!match) throw RoutingError("The road network is empty");
  if (match->distance_m > max_snap_distance_m_) {
    std::ostringstream msg;
    msg << "That point is " << std::fixed << std::setprecision(1) << match->distance_m / 1000.0
        << " km from the nearest road in the loaded map area";
    throw RoutingError(msg.str());
  }
  return {match->node, graph_.node(match->node).position, match->distance_m,
          intersection_label(match->node)};
}

Route RouteService::route(const LatLng& from, const LatLng& to, const SearchOptions& options) const {
  SnappedPoint origin = snap(from);
  SnappedPoint destination = snap(to);
  SearchResult search = find_shortest_path(graph_, origin.node, destination.node, options);
  return make_route(std::move(origin), std::move(destination), std::move(search));
}

std::vector<SnappedPoint> RouteService::snap_all(std::span<const LatLng> points) const {
  if (points.empty()) throw RoutingError("No points to route between");
  std::vector<SnappedPoint> snapped;
  std::optional<RoutingError> first_error;
  for (const LatLng& p : points) {
    try {
      SnappedPoint s = snap(p);
      const bool duplicate = std::any_of(snapped.begin(), snapped.end(),
                                         [&](const SnappedPoint& other) { return other.node == s.node; });
      if (!duplicate) snapped.push_back(std::move(s));
    } catch (const RoutingError& e) {
      if (!first_error) first_error = e;
    }
  }
  if (snapped.empty()) throw *first_error;
  return snapped;
}

Route RouteService::make_route(std::span<const SnappedPoint> origins, std::span<const SnappedPoint> destinations,
                               SearchResult search) const {
  if (origins.empty() || destinations.empty()) throw std::invalid_argument("make_route: no endpoints");
  // The search reports which endpoints it used; if nothing was found, fall back to the first ones.
  auto pick = [](std::span<const SnappedPoint> points, std::optional<NodeId> node) {
    if (node) {
      for (const SnappedPoint& p : points) {
        if (p.node == *node) return p;
      }
    }
    return points.front();
  };
  const bool found = search.found && !search.nodes.empty();
  SnappedPoint origin = pick(origins, found ? std::optional(search.nodes.front()) : std::nullopt);
  SnappedPoint destination = pick(destinations, found ? std::optional(search.nodes.back()) : std::nullopt);
  return make_route(std::move(origin), std::move(destination), std::move(search));
}

Route RouteService::make_route(SnappedPoint origin, SnappedPoint destination, SearchResult search) const {
  Route route;
  route.origin = std::move(origin);
  route.destination = std::move(destination);
  route.search = std::move(search);

  route.geometry.reserve(route.search.nodes.size());
  for (NodeId v : route.search.nodes) route.geometry.push_back(graph_.node(v).position);

  route.steps = build_steps(route.search);
  if (route.search.found) {
    route.steps.push_back({"Arrive at " + route.destination.label, "arrive", "", 0.0, 0.0,
                           route.destination.position});
  }
  return route;
}

std::vector<RouteStep> RouteService::build_steps(const SearchResult& result) const {
  std::vector<RouteStep> steps;
  if (!result.found || result.edges.empty()) return steps;

  // 1. Group consecutive edges on the same street into legs.
  struct Leg {
    NameId name;
    EdgeId first;  // first edge of the leg
    EdgeId last;   // last edge of the leg
    double length_m;
    double duration_s;
  };
  std::vector<Leg> legs;
  for (EdgeId id : result.edges) {
    const Edge& e = graph_.edge(id);
    if (!legs.empty() && legs.back().name == e.name) {
      legs.back().last = id;
      legs.back().length_m += e.length_m;
      legs.back().duration_s += e.duration_s;
    } else {
      legs.push_back({e.name, id, id, e.length_m, e.duration_s});
    }
  }

  // 2. Smooth map artefacts: a very short leg sandwiched between two legs of
  //    the same street (e.g. a 10 m jog through a split intersection) is folded
  //    back into that street instead of producing two extra turns.
  constexpr double kShortLegM = 35.0;
  std::vector<Leg> smoothed;
  for (std::size_t i = 0; i < legs.size(); ++i) {
    if (!smoothed.empty() && i + 1 < legs.size() && legs[i].length_m < kShortLegM &&
        smoothed.back().name == legs[i + 1].name) {
      Leg& keep = smoothed.back();
      keep.last = legs[i + 1].last;
      keep.length_m += legs[i].length_m + legs[i + 1].length_m;
      keep.duration_s += legs[i].duration_s + legs[i + 1].duration_s;
      ++i;  // legs[i + 1] consumed
      continue;
    }
    smoothed.push_back(legs[i]);
  }

  // 3. Describe each leg; the turn angle is measured between the last edge of
  //    the previous leg and the first edge of this one.
  auto edge_bearing = [this](EdgeId id) {
    const Edge& e = graph_.edge(id);
    return bearing_deg(graph_.node(e.source).position, graph_.node(e.target).position);
  };

  for (std::size_t i = 0; i < smoothed.size(); ++i) {
    const Leg& leg = smoothed[i];
    RouteStep step;
    step.street = street_name(leg.name);
    step.location = graph_.node(graph_.edge(leg.first).source).position;
    step.length_m = leg.length_m;
    step.duration_s = leg.duration_s;
    if (i == 0) {
      step.maneuver = "depart";
      step.instruction = std::string("Head ") + cardinal(edge_bearing(leg.first)) + " on " + step.street;
    } else {
      const double delta = bearing_delta_deg(edge_bearing(smoothed[i - 1].last), edge_bearing(leg.first));
      const Maneuver m = classify_turn(delta);
      step.maneuver = m.type;
      step.instruction = std::string(m.verb) + " " + step.street;
    }
    steps.push_back(std::move(step));
  }
  return steps;
}

}  // namespace umbra
