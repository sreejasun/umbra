#include "umbra/osm_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "umbra/geometry2d.hpp"

namespace umbra {
namespace {

using json = nlohmann::json;

enum class Flow { kBoth, kForward, kBackward, kSkip };

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string tag(const json& tags, const char* key) {
  auto it = tags.find(key);
  return (it != tags.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

/// Direction(s) a way may be driven, following OSM tagging conventions.
Flow way_flow(const json& tags, const std::string& highway) {
  const std::string oneway = lower(tag(tags, "oneway"));
  if (oneway == "yes" || oneway == "1" || oneway == "true") return Flow::kForward;
  if (oneway == "-1" || oneway == "reverse") return Flow::kBackward;
  if (oneway == "reversible") return Flow::kSkip;  // direction changes by time of day
  if (oneway == "no" || oneway == "false" || oneway == "0" || oneway == "alternating") {
    return Flow::kBoth;
  }

  // Implied one-way roads.
  const std::string junction = tag(tags, "junction");
  if (junction == "roundabout" || junction == "circular") return Flow::kForward;
  if (highway == "motorway" || highway == "motorway_link") return Flow::kForward;
  return Flow::kBoth;
}

bool is_restricted(const json& tags) {
  const std::string access = lower(tag(tags, "access"));
  const std::string motor = lower(tag(tags, "motor_vehicle"));
  auto blocked = [](const std::string& v) { return v == "no" || v == "private"; };
  return blocked(access) || blocked(motor) || tag(tags, "area") == "yes";
}

/// Pedestrian access: an explicit `foot` tag wins over the general `access` tag.
bool walking_allowed(const json& tags) {
  const std::string foot = lower(tag(tags, "foot"));
  if (foot == "no" || foot == "private" || foot == "use_sidepath") return false;
  if (foot == "yes" || foot == "designated" || foot == "permissive") return true;
  const std::string access = lower(tag(tags, "access"));
  return access != "no" && access != "private";
}

/// Roofed or indoor walkways are in shade at any time of day.
bool is_covered(const json& tags, const std::string& highway) {
  const std::string covered = lower(tag(tags, "covered"));
  const std::string tunnel = lower(tag(tags, "tunnel"));
  return covered == "yes" || covered == "arcade" || covered == "colonnade" || tunnel == "yes" ||
         tunnel == "building_passage" || lower(tag(tags, "indoor")) == "yes" || highway == "corridor";
}

/// How one way is used by the chosen travel mode.
struct WayProfile {
  double speed_mps;
  Flow flow;
  std::uint8_t flags;
};

std::optional<WayProfile> drive_profile(const json& tags, const std::string& highway) {
  const auto default_speed = osm::default_speed_kmh(highway);
  if (!default_speed || is_restricted(tags)) return std::nullopt;
  const Flow flow = way_flow(tags, highway);
  if (flow == Flow::kSkip) return std::nullopt;
  const double kmh = osm::parse_maxspeed_kmh(tag(tags, "maxspeed")).value_or(*default_speed);
  return WayProfile{kmh / 3.6, flow, 0};
}

/// Edge flags for pedestrians: shelter, stairs (or wheelchair=no) and street lighting.
std::uint8_t walk_flags(const json& tags, const std::string& highway) {
  std::uint8_t flags = 0;
  if (is_covered(tags, highway)) flags |= kEdgeCovered;
  if (highway == "steps" || lower(tag(tags, "wheelchair")) == "no") flags |= kEdgeStairs;
  const std::string lit = lower(tag(tags, "lit"));
  if (lit == "no" || lit == "disused") {
    flags |= kEdgeUnlit;
  } else if (!lit.empty()) {
    flags |= kEdgeLit;  // yes, 24/7, automatic, sunset-sunrise, limited, ...
  }
  return flags;
}

std::optional<WayProfile> walk_profile(const json& tags, const std::string& highway) {
  const auto kmh = osm::walking_speed_kmh(highway);
  if (!kmh || !walking_allowed(tags)) return std::nullopt;
  // Pedestrians may walk both ways on one-way streets.
  return WayProfile{*kmh / 3.6, Flow::kBoth, walk_flags(tags, highway)};
}

}  // namespace

namespace osm {

std::optional<double> parse_maxspeed_kmh(std::string_view value) {
  // Multiple values ("50;30") -> use the first.
  value = value.substr(0, value.find(';'));
  std::string text = lower(value);

  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) return std::nullopt;
  text = text.substr(first);

  char* end = nullptr;
  const double number = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || !(number > 0) || number > 200) return std::nullopt;

  const std::string unit(end);
  if (unit.find("mph") != std::string::npos) return number * 1.609344;
  if (unit.find("knot") != std::string::npos) return number * 1.852;
  return number;  // km/h is the OSM default unit
}

std::optional<double> default_speed_kmh(std::string_view highway) {
  // Typical free-flow speeds for US urban roads.
  static const std::unordered_map<std::string_view, double> kSpeeds{
      {"motorway", 105},     {"motorway_link", 60},  {"trunk", 85},        {"trunk_link", 55},
      {"primary", 65},       {"primary_link", 50},   {"secondary", 55},    {"secondary_link", 45},
      {"tertiary", 45},      {"tertiary_link", 40},  {"unclassified", 40}, {"residential", 30},
      {"living_street", 10},
  };
  if (auto it = kSpeeds.find(highway); it != kSpeeds.end()) return it->second;
  return std::nullopt;
}

std::optional<double> walking_speed_kmh(std::string_view highway) {
  // Freeways (motorway / trunk) are not walkable; stairs are slower than flat ground.
  static const std::unordered_set<std::string_view> kWalkable{
      "primary",     "primary_link", "secondary", "secondary_link", "tertiary", "tertiary_link",
      "unclassified", "residential", "living_street", "service",     "pedestrian", "footway",
      "path",        "cycleway",     "track",     "corridor"};
  if (highway == "steps") return 2.5;
  if (kWalkable.count(highway)) return 5.0;
  return std::nullopt;
}

std::optional<double> parse_length_m(std::string_view value) {
  std::string text = lower(value);
  char* end = nullptr;
  const double number = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || !(number > 0)) return std::nullopt;
  const std::string unit(end);
  if (unit.find('\'') != std::string::npos || unit.find("ft") != std::string::npos) return number * 0.3048;
  return number;
}

}  // namespace osm

RoadGraph parse_overpass_json(std::string_view json_text, const OsmLoadOptions& options,
                              OsmLoadStats* stats) {
  OsmLoadStats local_stats;
  OsmLoadStats& s = stats ? *stats : local_stats;
  s = {};

  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw MapLoadError(std::string("Invalid map JSON: ") + e.what());
  }
  if (!doc.is_object() || !doc.contains("elements") || !doc["elements"].is_array()) {
    throw MapLoadError("Map JSON must be an Overpass response with an 'elements' array");
  }

  try {
    const json& elements = doc["elements"];

    // Pass 1: coordinates of every OSM node.
    std::unordered_map<std::int64_t, LatLng> coords;
    coords.reserve(elements.size());
    for (const json& el : elements) {
      if (tag(el, "type") == "node") {
        coords.emplace(el.at("id").get<std::int64_t>(),
                       LatLng{el.at("lat").get<double>(), el.at("lon").get<double>()});
      }
    }

    // Pass 2: turn drivable ways into directed edges.
    RoadGraphBuilder builder;
    std::unordered_map<std::int64_t, NodeId> node_ids;
    auto graph_node = [&](std::int64_t osm_id) -> std::optional<NodeId> {
      if (auto it = node_ids.find(osm_id); it != node_ids.end()) return it->second;
      auto c = coords.find(osm_id);
      if (c == coords.end()) return std::nullopt;  // referenced node missing from extract
      const NodeId id = builder.add_node(osm_id, c->second);
      node_ids.emplace(osm_id, id);
      return id;
    };

    for (const json& el : elements) {
      if (tag(el, "type") != "way") continue;
      ++s.ways_seen;

      static const json kNoTags = json::object();
      const json& tags = el.contains("tags") ? el["tags"] : kNoTags;
      const std::string highway = tag(tags, "highway");
      const std::optional<WayProfile> profile =
          options.mode == TravelMode::kWalk ? walk_profile(tags, highway) : drive_profile(tags, highway);
      if (!profile) continue;
      const Flow flow = profile->flow;
      const double speed_mps = profile->speed_mps;

      std::string name = tag(tags, "name");
      if (name.empty()) name = tag(tags, "ref");
      const NameId name_id = builder.intern_name(name);

      // Each consecutive pair of nodes along the way becomes one segment.
      std::optional<NodeId> prev;
      LatLng prev_pos;
      bool used = false;
      for (const json& ref : el.at("nodes")) {
        const auto osm_id = ref.get<std::int64_t>();
        const std::optional<NodeId> cur = graph_node(osm_id);
        if (!cur) {
          prev.reset();  // gap in the extract: do not bridge it
          continue;
        }
        const LatLng pos = coords.at(osm_id);
        if (prev && *prev != *cur) {
          const double length = haversine_m(prev_pos, pos);
          const double duration = length / speed_mps;
          if (flow != Flow::kBackward) builder.add_edge(*prev, *cur, length, duration, name_id, profile->flags);
          if (flow != Flow::kForward) builder.add_edge(*cur, *prev, length, duration, name_id, profile->flags);
          used = true;
        }
        prev = cur;
        prev_pos = pos;
      }
      if (used) ++s.ways_used;
    }

    RoadGraph graph = builder.build();
    s.nodes_before_pruning = graph.node_count();
    if (options.keep_largest_component) graph = graph.largest_strongly_connected_component();
    return graph;
  } catch (const json::exception& e) {
    throw MapLoadError(std::string("Malformed map data: ") + e.what());
  }
}

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw MapLoadError("Cannot open map file: " + path.string());
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/// "fast_food" -> "Fast food"
std::string humanize(std::string value) {
  std::replace(value.begin(), value.end(), '_', ' ');
  if (!value.empty()) value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
  return value;
}

/// Category label and ranking importance for a tagged feature, or nullopt to skip it.
std::optional<std::pair<std::string, double>> classify_place(const json& tags) {
  static const std::unordered_set<std::string> kSkippedAmenities{
      "parking", "parking_space", "parking_entrance", "bicycle_parking", "motorcycle_parking",
      "bench", "waste_basket", "waste_disposal", "recycling", "vending_machine", "drinking_water",
      "toilets", "shelter", "atm", "charging_station", "post_box", "telephone", "fountain",
      "bicycle_rental", "grit_bin", "clock", "letter_box", "loading_dock", "bbq", "smoking_area"};
  static const std::unordered_map<std::string, std::string> kLabels{
      {"tram_stop", "Light rail stop"}, {"halt", "Station"}, {"mall", "Shopping center"},
      {"university", "University"}, {"college", "College"}};

  auto label = [&](const std::string& v) {
    auto it = kLabels.find(v);
    return it != kLabels.end() ? it->second : humanize(v);
  };

  if (const std::string v = tag(tags, "amenity"); !v.empty()) {
    if (kSkippedAmenities.count(v)) return std::nullopt;
    if (v == "university" || v == "college") return std::pair{label(v), 3.0};
    if (v == "hospital") return std::pair{label(v), 1.5};
    if (v == "library" || v == "townhall" || v == "marketplace") return std::pair{label(v), 1.0};
    return std::pair{label(v), 0.5};
  }
  if (const std::string v = tag(tags, "leisure"); !v.empty()) {
    return std::pair{label(v), v == "stadium" ? 1.5 : 1.0};
  }
  if (const std::string v = tag(tags, "railway"); !v.empty()) return std::pair{label(v), 1.0};
  if (const std::string v = tag(tags, "shop"); !v.empty()) {
    return std::pair{label(v), v == "mall" ? 2.0 : 0.5};
  }
  if (const std::string v = tag(tags, "tourism"); !v.empty()) {
    const bool landmark = v == "museum" || v == "attraction" || v == "hotel";
    return std::pair{label(v), landmark ? 1.0 : 0.5};
  }
  if (const std::string v = tag(tags, "building"); !v.empty()) {
    if (v == "university" || v == "college") return std::pair{std::string("Campus building"), 0.6};
    return std::pair{label(v), v == "stadium" ? 1.5 : 0.8};
  }
  return std::nullopt;
}

}  // namespace

std::vector<Place> parse_overpass_places(std::string_view json_text) {
  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw MapLoadError(std::string("Invalid places JSON: ") + e.what());
  }
  if (!doc.is_object() || !doc.contains("elements") || !doc["elements"].is_array()) {
    throw MapLoadError("Places JSON must be an Overpass response with an 'elements' array");
  }

  std::vector<Place> places;
  std::unordered_map<std::string, std::vector<std::size_t>> by_name;  // lower-case name -> indices
  try {
    for (const json& el : doc["elements"]) {
      if (!el.contains("tags")) continue;
      const json& tags = el["tags"];
      const std::string name = tag(tags, "name");
      if (name.empty()) continue;
      const auto category = classify_place(tags);
      if (!category) continue;

      // Nodes carry lat/lon; ways and relations carry a "center" (from `out center`).
      LatLng position;
      if (el.contains("lat") && el.contains("lon")) {
        position = {el["lat"].get<double>(), el["lon"].get<double>()};
      } else if (el.contains("center")) {
        position = {el["center"].at("lat").get<double>(), el["center"].at("lon").get<double>()};
      } else {
        continue;
      }

      std::string keywords = tag(tags, "short_name");
      if (const std::string alt = tag(tags, "alt_name"); !alt.empty()) keywords += " " + alt;

      // Features notable enough to have a Wikipedia/Wikidata entry are usually landmarks.
      double importance = category->second;
      if (!tag(tags, "wikidata").empty() || !tag(tags, "wikipedia").empty()) importance += 1.0;
      // A feature mapped as an area (campus, mall) usually outranks a single point.
      if (tag(el, "type") != "node") importance += 0.5;

      Place place;
      place.name = name;
      place.category = category->first;
      place.keywords = keywords;
      place.position = position;
      place.importance = importance;

      // Merge duplicates: the same name mapped as both a node and a building, etc.
      constexpr double kMergeRadiusM = 200.0;
      auto& same_name = by_name[lower(name)];
      auto duplicate = std::find_if(same_name.begin(), same_name.end(), [&](std::size_t i) {
        return haversine_m(places[i].position, position) < kMergeRadiusM;
      });
      if (duplicate == same_name.end()) {
        same_name.push_back(places.size());
        places.push_back(std::move(place));
      } else if (place.importance > places[*duplicate].importance) {
        places[*duplicate] = std::move(place);
      }
    }
  } catch (const json::exception& e) {
    throw MapLoadError(std::string("Malformed places data: ") + e.what());
  }
  return places;
}

std::vector<Place> load_places_file(const std::filesystem::path& path) {
  return parse_overpass_places(read_file(path));
}

// ----------------------------------------------------------------------------
// Campus buildings
// ----------------------------------------------------------------------------

namespace {

std::vector<LatLng> read_geometry(const json& geometry) {
  std::vector<LatLng> points;
  points.reserve(geometry.size());
  for (const json& p : geometry) points.push_back({p.at("lat").get<double>(), p.at("lon").get<double>()});
  return points;
}

/// Outer rings of a way (its geometry) or multipolygon relation (its outer members).
std::vector<std::vector<LatLng>> outer_rings(const json& el) {
  std::vector<std::vector<LatLng>> rings;
  if (el.contains("geometry")) {
    rings.push_back(read_geometry(el["geometry"]));
  } else if (el.contains("members")) {
    for (const json& m : el["members"]) {
      if (tag(m, "role") == "outer" && m.contains("geometry")) rings.push_back(read_geometry(m["geometry"]));
    }
  }
  std::erase_if(rings, [](const auto& ring) { return ring.size() < 3; });
  return rings;
}

/// Is `p` inside `ring`? Degrees used as planar x/y: fine for a campus-sized polygon.
bool inside(const LatLng& p, const std::vector<LatLng>& ring) {
  std::vector<geom::Vec2> planar;
  planar.reserve(ring.size());
  for (const LatLng& q : ring) planar.push_back({q.lng, q.lat});
  return geom::point_in_polygon({p.lng, p.lat}, planar);
}

/// Up to `count` points spread evenly (by distance) along a closed ring.
std::vector<LatLng> spread_along(const std::vector<LatLng>& ring, std::size_t count) {
  double perimeter = 0.0;
  for (std::size_t i = 0; i + 1 < ring.size(); ++i) perimeter += haversine_m(ring[i], ring[i + 1]);
  if (perimeter <= 0.0) return {ring.front()};
  std::vector<LatLng> out;
  const double step = perimeter / static_cast<double>(count);
  double next = 0.0;
  double walked = 0.0;
  for (std::size_t i = 0; i + 1 < ring.size() && out.size() < count; ++i) {
    const double len = haversine_m(ring[i], ring[i + 1]);
    while (len > 0 && next <= walked + len && out.size() < count) {
      const double f = (next - walked) / len;
      out.push_back({ring[i].lat + (ring[i + 1].lat - ring[i].lat) * f,
                     ring[i].lng + (ring[i + 1].lng - ring[i].lng) * f});
      next += step;
    }
    walked += len;
  }
  return out;
}

LatLng ring_centroid(const std::vector<LatLng>& ring) {
  LatLng c{0, 0};
  const std::size_t n = ring.size() > 1 && ring.front().lat == ring.back().lat && ring.front().lng == ring.back().lng
                            ? ring.size() - 1
                            : ring.size();
  for (std::size_t i = 0; i < n; ++i) {
    c.lat += ring[i].lat;
    c.lng += ring[i].lng;
  }
  return {c.lat / static_cast<double>(n), c.lng / static_cast<double>(n)};
}

/// Coordinates at OSM's 1e-7 degree precision, packed into one integer key.
std::int64_t coordinate_key(const LatLng& p) {
  const auto lat = static_cast<std::int64_t>(std::llround((p.lat + 90.0) * 1e7));
  const auto lng = static_cast<std::int64_t>(std::llround((p.lng + 180.0) * 1e7));
  return lat * 3'600'000'000LL + lng;
}

bool usable_entrance(const json& tags) {
  const std::string kind = lower(tag(tags, "entrance"));
  const std::string access = lower(tag(tags, "access"));
  if (access == "no" || access == "private") return false;
  return kind == "yes" || kind == "main" || kind == "secondary";
}

}  // namespace

std::vector<Place> parse_campus_buildings(std::string_view json_text) {
  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw MapLoadError(std::string("Invalid campus JSON: ") + e.what());
  }
  if (!doc.is_object() || !doc.contains("elements") || !doc["elements"].is_array()) {
    throw MapLoadError("Campus JSON must be an Overpass response with an 'elements' array");
  }

  struct Campus {
    std::string name;
    std::string short_name;
    std::vector<std::vector<LatLng>> rings;
  };
  std::vector<Campus> campuses;
  std::unordered_map<std::int64_t, LatLng> entrances;  // coordinate_key -> position
  std::vector<const json*> buildings;

  try {
    for (const json& el : doc["elements"]) {
      if (!el.contains("tags")) continue;
      const json& tags = el["tags"];
      const std::string amenity = tag(tags, "amenity");
      if ((amenity == "university" || amenity == "college") && tag(el, "type") != "node") {
        auto rings = outer_rings(el);
        if (!rings.empty()) campuses.push_back({tag(tags, "name"), tag(tags, "short_name"), std::move(rings)});
      }
      if (tag(el, "type") == "node" && usable_entrance(tags)) {
        const LatLng p{el.at("lat").get<double>(), el.at("lon").get<double>()};
        entrances[coordinate_key(p)] = p;
      }
      if (!tag(tags, "building").empty() && !tag(tags, "name").empty() && tag(el, "type") != "node") {
        buildings.push_back(&el);
      }
    }

    std::vector<Place> places;
    std::unordered_set<std::string> seen;  // "name|code": a building mapped twice is listed once
    for (const json* el : buildings) {
      const json& tags = (*el)["tags"];
      const auto rings = outer_rings(*el);
      if (rings.empty()) continue;
      // The largest ring is the main footprint.
      const auto& outline = *std::max_element(rings.begin(), rings.end(),
                                              [](const auto& a, const auto& b) { return a.size() < b.size(); });
      const LatLng centre = ring_centroid(outline);

      const Campus* campus = nullptr;
      for (const Campus& c : campuses) {
        if (std::any_of(c.rings.begin(), c.rings.end(), [&](const auto& ring) { return inside(centre, ring); })) {
          campus = &c;
          break;
        }
      }
      if (campus == nullptr) continue;  // off campus (e.g. a shop across the street)

      Place place;
      place.name = tag(tags, "name");
      place.code = tag(tags, "ref");
      if (!seen.insert(lower(place.name) + "|" + lower(place.code)).second) continue;
      place.position = centre;
      const std::string campus_label = campus->short_name.empty() ? campus->name : campus->short_name;
      place.category = campus_label.empty() ? "Campus building" : campus_label + " building";
      place.keywords = place.code;
      for (const char* key : {"alt_name", "short_name", "old_name"}) {
        if (const std::string v = tag(tags, key); !v.empty()) place.keywords += " " + v;
      }
      place.keywords += " " + campus->short_name;
      place.importance = 0.9 + (place.code.empty() ? 0.0 : 0.3);

      // Entrances: outline vertices that are entrance nodes. Overpass prints a
      // node's coordinates identically everywhere, so an exact match identifies
      // it, for ways and multipolygon members alike.
      for (const auto& ring : rings) {
        for (const LatLng& vertex : ring) {
          const auto it = entrances.find(coordinate_key(vertex));
          if (it == entrances.end()) continue;
          const bool duplicate = std::any_of(place.access_points.begin(), place.access_points.end(),
                                             [&](const LatLng& p) { return coordinate_key(p) == it->first; });
          if (!duplicate) place.access_points.push_back(it->second);
        }
      }
      place.entrances_mapped = !place.access_points.empty();
      if (!place.entrances_mapped) place.access_points = spread_along(outline, kMaxOutlineAccessPoints);
      places.push_back(std::move(place));
    }
    return places;
  } catch (const json::exception& e) {
    throw MapLoadError(std::string("Malformed campus data: ") + e.what());
  }
}

std::vector<Place> load_campus_file(const std::filesystem::path& path) {
  return parse_campus_buildings(read_file(path));
}

std::vector<Place> parse_water_points(std::string_view json_text) {
  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw MapLoadError(std::string("Invalid water JSON: ") + e.what());
  }
  if (!doc.is_object() || !doc.contains("elements") || !doc["elements"].is_array()) {
    throw MapLoadError("Water JSON must be an Overpass response with an 'elements' array");
  }
  std::vector<Place> points;
  try {
    for (const json& el : doc["elements"]) {
      if (!el.contains("tags")) continue;
      const json& tags = el["tags"];
      const std::string access = lower(tag(tags, "access"));
      if (access == "private" || access == "no" || lower(tag(tags, "drinking_water")) == "no") continue;

      Place p;
      if (el.contains("lat") && el.contains("lon")) {
        p.position = {el["lat"].get<double>(), el["lon"].get<double>()};
      } else if (el.contains("center")) {
        p.position = {el["center"].at("lat").get<double>(), el["center"].at("lon").get<double>()};
      } else {
        continue;
      }
      const bool bottle = lower(tag(tags, "bottle")) == "yes";
      p.category = bottle ? "Bottle filler" : "Water fountain";
      p.name = tag(tags, "name");
      if (p.name.empty()) p.name = p.category;
      points.push_back(std::move(p));
    }
  } catch (const json::exception& e) {
    throw MapLoadError(std::string("Malformed water data: ") + e.what());
  }
  return points;
}

std::vector<Place> load_water_file(const std::filesystem::path& path) {
  return parse_water_points(read_file(path));
}

std::vector<Place> merge_places(std::vector<Place> preferred, std::vector<Place> others) {
  constexpr double kSameRadiusM = 200.0;
  const std::size_t keep = preferred.size();
  for (Place& other : others) {
    const std::string name = lower(other.name);
    const bool duplicate = std::any_of(preferred.begin(), preferred.begin() + static_cast<std::ptrdiff_t>(keep),
                                       [&](const Place& p) {
                                         return lower(p.name) == name &&
                                                haversine_m(p.position, other.position) < kSameRadiusM;
                                       });
    if (!duplicate) preferred.push_back(std::move(other));
  }
  return preferred;
}

// ----------------------------------------------------------------------------
// Shade casters: buildings and trees
// ----------------------------------------------------------------------------

namespace {


/// Planar polygon area in m² (shoelace on a local equirectangular projection).
double ring_area_m2(const std::vector<LatLng>& ring) {
  if (ring.size() < 3) return 0.0;
  const double m_lat = kEarthRadiusM * std::numbers::pi / 180.0;
  const double m_lng = m_lat * std::cos(ring.front().lat * std::numbers::pi / 180.0);
  double twice = 0.0;
  for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    twice += (ring[j].lng * m_lng) * (ring[i].lat * m_lat) - (ring[i].lng * m_lng) * (ring[j].lat * m_lat);
  }
  return std::abs(twice) / 2.0;
}

double building_height(const json& tags, const std::vector<LatLng>& ring) {
  if (auto h = osm::parse_length_m(tag(tags, "height")); h && *h < 400) return *h;
  double levels = 0;
  if (auto l = osm::parse_length_m(tag(tags, "building:levels")); l && *l < 120) levels = *l;
  return ShadeMap::estimate_height_m(lower(tag(tags, "building")), levels, ring_area_m2(ring));
}

Tree make_tree(LatLng position, const json& tags) {
  Tree tree;
  tree.position = position;
  if (auto h = osm::parse_length_m(tag(tags, "height"))) tree.height_m = std::clamp(*h, 3.0, 18.0);
  if (auto d = osm::parse_length_m(tag(tags, "diameter_crown"))) {
    tree.crown_radius_m = std::clamp(*d / 2.0, 1.5, 8.0);
  }
  return tree;
}

}  // namespace

ShadeCasters parse_shade_casters(std::string_view json_text) {
  json doc;
  try {
    doc = json::parse(json_text);
  } catch (const json::parse_error& e) {
    throw MapLoadError(std::string("Invalid shade JSON: ") + e.what());
  }
  if (!doc.is_object() || !doc.contains("elements") || !doc["elements"].is_array()) {
    throw MapLoadError("Shade JSON must be an Overpass response with an 'elements' array");
  }

  ShadeCasters casters;
  try {
    for (const json& el : doc["elements"]) {
      if (!el.contains("tags")) continue;
      const json& tags = el["tags"];
      const std::string type = tag(el, "type");
      const std::string building = lower(tag(tags, "building"));
      const std::string natural = tag(tags, "natural");

      if (!building.empty() && building != "no") {
        if (type == "way" && el.contains("geometry")) {
          std::vector<LatLng> ring = read_geometry(el["geometry"]);
          if (ring.size() >= 3) {
            const double height = building_height(tags, ring);
            casters.buildings.push_back({std::move(ring), height});
          }
        } else if (type == "relation" && el.contains("members")) {
          // Multipolygon buildings: each outer ring is a footprint (courtyards are ignored).
          for (const json& member : el["members"]) {
            if (tag(member, "role") != "outer" || !member.contains("geometry")) continue;
            std::vector<LatLng> ring = read_geometry(member["geometry"]);
            if (ring.size() < 3) continue;
            const double height = building_height(tags, ring);
            casters.buildings.push_back({std::move(ring), height});
          }
        }
      } else if (natural == "tree" && type == "node") {
        casters.trees.push_back(make_tree({el.at("lat").get<double>(), el.at("lon").get<double>()}, tags));
      } else if (natural == "tree_row" && type == "way" && el.contains("geometry")) {
        // Place a tree every 8 m along the row.
        const std::vector<LatLng> line = read_geometry(el["geometry"]);
        constexpr double kSpacingM = 8.0;
        double carry = 0.0;
        for (std::size_t i = 1; i < line.size(); ++i) {
          const double length = haversine_m(line[i - 1], line[i]);
          for (double d = carry; d < length; d += kSpacingM) {
            const double t = d / length;
            casters.trees.push_back(make_tree({line[i - 1].lat + (line[i].lat - line[i - 1].lat) * t,
                                               line[i - 1].lng + (line[i].lng - line[i - 1].lng) * t},
                                              tags));
          }
          carry = std::fmod(carry - length, kSpacingM);
          if (carry < 0) carry += kSpacingM;
        }
      }
    }
  } catch (const json::exception& e) {
    throw MapLoadError(std::string("Malformed shade data: ") + e.what());
  }
  return casters;
}

ShadeCasters load_shade_file(const std::filesystem::path& path) {
  return parse_shade_casters(read_file(path));
}

RoadGraph load_overpass_file(const std::filesystem::path& path, const OsmLoadOptions& options,
                             OsmLoadStats* stats) {
  return parse_overpass_json(read_file(path), options, stats);
}

}  // namespace umbra
