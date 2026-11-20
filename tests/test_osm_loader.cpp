#include <gtest/gtest.h>

#include <set>

#include "umbra/osm_loader.hpp"

namespace umbra {
namespace {

const std::string kFixture = std::string(UMBRA_TEST_DATA_DIR) + "/mini_city.osm.json";

std::set<std::int64_t> osm_ids(const RoadGraph& g) {
  std::set<std::int64_t> ids;
  for (const Node& n : g.nodes()) ids.insert(n.osm_id);
  return ids;
}

const Edge* find_edge(const RoadGraph& g, std::int64_t from, std::int64_t to) {
  for (NodeId u = 0; u < g.node_count(); ++u) {
    if (g.node(u).osm_id != from) continue;
    for (const Edge& e : g.out_edges(u)) {
      if (g.node(e.target).osm_id == to) return &e;
    }
  }
  return nullptr;
}

TEST(OsmMaxspeed, ParsesUnits) {
  EXPECT_NEAR(*osm::parse_maxspeed_kmh("25 mph"), 40.2336, 1e-4);
  EXPECT_DOUBLE_EQ(*osm::parse_maxspeed_kmh("50"), 50.0);
  EXPECT_DOUBLE_EQ(*osm::parse_maxspeed_kmh("60 km/h"), 60.0);
  EXPECT_DOUBLE_EQ(*osm::parse_maxspeed_kmh("50;30"), 50.0);
  EXPECT_NEAR(*osm::parse_maxspeed_kmh("10 knots"), 18.52, 1e-9);
}

TEST(OsmMaxspeed, RejectsNonNumericValues) {
  EXPECT_FALSE(osm::parse_maxspeed_kmh("").has_value());
  EXPECT_FALSE(osm::parse_maxspeed_kmh("none").has_value());
  EXPECT_FALSE(osm::parse_maxspeed_kmh("signals").has_value());
  EXPECT_FALSE(osm::parse_maxspeed_kmh("0").has_value());
  EXPECT_FALSE(osm::parse_maxspeed_kmh("-5").has_value());
}

TEST(OsmDefaults, OnlyDrivableClassesHaveSpeeds) {
  EXPECT_TRUE(osm::default_speed_kmh("residential").has_value());
  EXPECT_TRUE(osm::default_speed_kmh("motorway").has_value());
  EXPECT_FALSE(osm::default_speed_kmh("footway").has_value());
  EXPECT_FALSE(osm::default_speed_kmh("cycleway").has_value());
  EXPECT_GT(*osm::default_speed_kmh("motorway"), *osm::default_speed_kmh("residential"));
}

TEST(OsmLoader, LoadsDrivableNetworkAndPrunesIsland) {
  OsmLoadStats stats;
  const RoadGraph g = load_overpass_file(kFixture, {}, &stats);

  // Nodes 1-6 form the main network; 7 (private), 999 (missing) never enter the
  // graph, and 8-9 (island) are removed by the strongly-connected-component pass.
  EXPECT_EQ(osm_ids(g), (std::set<std::int64_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(stats.nodes_before_pruning, 8U);
  EXPECT_EQ(stats.ways_seen, 10U);
  EXPECT_EQ(stats.ways_used, 6U);  // 5 main ways + island
}

TEST(OsmLoader, KeepsIslandWhenPruningDisabled) {
  OsmLoadOptions options;
  options.keep_largest_component = false;
  const RoadGraph g = load_overpass_file(kFixture, options);
  EXPECT_EQ(g.node_count(), 8U);
}

TEST(OsmLoader, HonoursOneWayDirection) {
  const RoadGraph g = load_overpass_file(kFixture);
  // Oak Street is oneway=-1: drivable only 6 -> 5 -> 4.
  EXPECT_NE(find_edge(g, 6, 5), nullptr);
  EXPECT_NE(find_edge(g, 5, 4), nullptr);
  EXPECT_EQ(find_edge(g, 4, 5), nullptr);
  EXPECT_EQ(find_edge(g, 5, 6), nullptr);
  // Main Street is two-way.
  EXPECT_NE(find_edge(g, 1, 2), nullptr);
  EXPECT_NE(find_edge(g, 2, 1), nullptr);
}

TEST(OsmLoader, UsesMaxspeedForTravelTime) {
  const RoadGraph g = load_overpass_file(kFixture);
  const Edge* main = find_edge(g, 1, 2);  // 25 mph
  const Edge* third = find_edge(g, 3, 6);  // 40 km/h
  ASSERT_NE(main, nullptr);
  ASSERT_NE(third, nullptr);
  EXPECT_NEAR(main->length_m / main->duration_s * 3.6, 40.2336, 1e-6);
  EXPECT_NEAR(third->length_m / third->duration_s * 3.6, 40.0, 1e-6);
  EXPECT_EQ(g.name(main->name), "Main Street");
  EXPECT_NEAR(main->length_m, 93.0, 1.5);  // 0.001 deg of longitude at 33.4 N
}

TEST(OsmLoader, RejectsMalformedInput) {
  EXPECT_THROW((void)parse_overpass_json("not json"), MapLoadError);
  EXPECT_THROW((void)parse_overpass_json(R"({"foo": 1})"), MapLoadError);
  EXPECT_THROW((void)parse_overpass_json(R"({"elements": [{"type": "node", "id": 1}]})"),
               MapLoadError);
  EXPECT_THROW((void)load_overpass_file("/definitely/not/here.json"), MapLoadError);
}

TEST(OsmLoader, EmptyExtractGivesEmptyGraph) {
  EXPECT_TRUE(parse_overpass_json(R"({"elements": []})").empty());
}

// ------------------------------------------------------- campus buildings

const Place* find_place(const std::vector<Place>& places, const std::string& name) {
  for (const Place& p : places) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

TEST(CampusLoader, KeepsNamedBuildingsInsideCampus) {
  const auto buildings = load_campus_file(std::string(UMBRA_TEST_DATA_DIR) + "/mini_campus.json");
  // Science Hall (mapped twice, listed once), Old Gym and the Library; not the shop off campus.
  EXPECT_EQ(buildings.size(), 3U);
  EXPECT_EQ(find_place(buildings, "Corner Shop"), nullptr);

  const Place* science = find_place(buildings, "Science Hall");
  ASSERT_NE(science, nullptr);
  EXPECT_EQ(science->code, "SCI");
  EXPECT_EQ(science->category, "DSU building");
  EXPECT_NE(science->keywords.find("SCI"), std::string::npos);
  EXPECT_NE(science->keywords.find("Sci Hall"), std::string::npos);
  EXPECT_NE(science->keywords.find("DSU"), std::string::npos);
  EXPECT_NEAR(science->position.lat, 33.4005, 1e-6);
  EXPECT_NEAR(science->position.lng, -111.9395, 1e-6);
}

TEST(CampusLoader, UsesMappedEntrances) {
  const auto buildings = load_campus_file(std::string(UMBRA_TEST_DATA_DIR) + "/mini_campus.json");
  const Place* science = find_place(buildings, "Science Hall");
  ASSERT_NE(science, nullptr);
  // The main entrance counts; the service (staff/delivery) door does not.
  EXPECT_TRUE(science->entrances_mapped);
  ASSERT_EQ(science->access_points.size(), 1U);
  EXPECT_DOUBLE_EQ(science->access_points[0].lat, 33.4003);
  EXPECT_DOUBLE_EQ(science->access_points[0].lng, -111.9397);

  // Multipolygon buildings find entrances on their outer ring too.
  const Place* library = find_place(buildings, "Library");
  ASSERT_NE(library, nullptr);
  EXPECT_TRUE(library->entrances_mapped);
  ASSERT_EQ(library->access_points.size(), 1U);
  EXPECT_DOUBLE_EQ(library->access_points[0].lat, 33.4001);
}

TEST(CampusLoader, FallsBackToPointsAroundTheOutline) {
  const auto buildings = load_campus_file(std::string(UMBRA_TEST_DATA_DIR) + "/mini_campus.json");
  const Place* gym = find_place(buildings, "Old Gym");
  ASSERT_NE(gym, nullptr);
  EXPECT_FALSE(gym->entrances_mapped);
  EXPECT_EQ(gym->access_points.size(), kMaxOutlineAccessPoints);
  for (const LatLng& p : gym->access_points) {
    // On the outline: on one of the four sides of the 0.0004-degree square.
    const bool on_lat_side = std::abs(p.lat - 33.4008) < 1e-9 || std::abs(p.lat - 33.4012) < 1e-9;
    const bool on_lng_side = std::abs(p.lng + 111.9385) < 1e-9 || std::abs(p.lng + 111.9381) < 1e-9;
    EXPECT_TRUE(on_lat_side || on_lng_side) << p.lat << "," << p.lng;
  }
}

TEST(CampusLoader, RejectsMalformedInput) {
  EXPECT_THROW((void)parse_campus_buildings("{"), MapLoadError);
  EXPECT_THROW((void)parse_campus_buildings(R"({"elements": 5})"), MapLoadError);
  EXPECT_TRUE(parse_campus_buildings(R"({"elements": []})").empty());
}

TEST(MergePlaces, CampusEntriesReplaceSameNamedPlaces) {
  Place building;
  building.name = "Science Hall";
  building.position = {33.4005, -111.9395};
  building.code = "SCI";
  Place duplicate;
  duplicate.name = "science hall";
  duplicate.position = {33.4006, -111.9395};
  Place far_namesake = duplicate;
  far_namesake.position = {33.43, -111.90};
  Place other;
  other.name = "Cactus Coffee";
  other.position = {33.4005, -111.9395};

  const auto merged = merge_places({building}, {duplicate, far_namesake, other});
  ASSERT_EQ(merged.size(), 3U);
  EXPECT_EQ(merged[0].code, "SCI");
  EXPECT_EQ(merged[1].position.lat, 33.43);
  EXPECT_EQ(merged[2].name, "Cactus Coffee");
}

}  // namespace
}  // namespace umbra
