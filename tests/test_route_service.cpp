#include <gtest/gtest.h>

#include "umbra/osm_loader.hpp"
#include "umbra/route_service.hpp"

namespace umbra {
namespace {

class RouteServiceTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    service_ = new RouteService(
        load_overpass_file(std::string(UMBRA_TEST_DATA_DIR) + "/mini_city.osm.json"));
  }
  static void TearDownTestSuite() {
    delete service_;
    service_ = nullptr;
  }
  static RouteService* service_;
};

RouteService* RouteServiceTest::service_ = nullptr;

TEST_F(RouteServiceTest, SnapsToNearestIntersection) {
  const SnappedPoint p = service_->snap({33.40005, -111.93905});  // just off node 2
  EXPECT_EQ(service_->graph().node(p.node).osm_id, 2);
  EXPECT_LT(p.snap_distance_m, 10.0);
  EXPECT_NE(p.label.find("Main Street"), std::string::npos);
  EXPECT_NE(p.label.find("Second Avenue"), std::string::npos);
}

TEST_F(RouteServiceTest, RejectsPointsOutsideCoverage) {
  EXPECT_THROW((void)service_->snap({40.0, -100.0}), RoutingError);
  EXPECT_THROW((void)service_->snap({123.0, 0.0}), RoutingError);
}

TEST_F(RouteServiceTest, ProducesTurnByTurnDirections) {
  // Node 1 -> node 6: east on Main Street, then left (north) on Third Avenue,
  // because Oak Street only runs westbound.
  const Route r = service_->route({33.4000, -111.9400}, {33.4010, -111.9380});
  ASSERT_TRUE(r.search.found);
  ASSERT_EQ(r.steps.size(), 3U);

  EXPECT_EQ(r.steps[0].maneuver, "depart");
  EXPECT_EQ(r.steps[0].instruction, "Head east on Main Street");
  EXPECT_NEAR(r.steps[0].length_m, 186.0, 3.0);

  EXPECT_EQ(r.steps[1].maneuver, "left");
  EXPECT_EQ(r.steps[1].instruction, "Turn left onto Third Avenue");

  EXPECT_EQ(r.steps[2].maneuver, "arrive");
  EXPECT_EQ(r.geometry.size(), r.search.nodes.size());
}

TEST(RouteServiceSteps, FoldsShortJogBackIntoTheSameStreet) {
  // Rio Salado -> 11 m on Smith Rd -> Rio Salado -> left onto Main St.
  RoadGraphBuilder b;
  const NodeId a = b.add_node(1, {33.4000, -111.950});
  const NodeId bb = b.add_node(2, {33.4000, -111.948});
  const NodeId c = b.add_node(3, {33.4001, -111.948});
  const NodeId d = b.add_node(4, {33.4001, -111.947});
  const NodeId e = b.add_node(5, {33.4011, -111.947});
  const NameId rio = b.intern_name("Rio Salado Parkway");
  const NameId smith = b.intern_name("Smith Road");
  const NameId main = b.intern_name("Main Street");
  b.add_edge(a, bb, 186, 18, rio);
  b.add_edge(bb, c, 11, 1, smith);
  b.add_edge(c, d, 93, 9, rio);
  b.add_edge(d, e, 111, 11, main);
  const RouteService service(b.build());

  const Route r = service.route({33.4000, -111.950}, {33.4011, -111.947});
  ASSERT_TRUE(r.search.found);
  ASSERT_EQ(r.steps.size(), 3U);
  EXPECT_EQ(r.steps[0].instruction, "Head east on Rio Salado Parkway");
  EXPECT_DOUBLE_EQ(r.steps[0].length_m, 186 + 11 + 93);
  EXPECT_EQ(r.steps[1].instruction, "Turn left onto Main Street");
  EXPECT_EQ(r.steps[2].maneuver, "arrive");
}

TEST_F(RouteServiceTest, RouteToSamePointIsTrivial) {
  const Route r = service_->route({33.4, -111.94}, {33.4, -111.94});
  ASSERT_TRUE(r.search.found);
  EXPECT_DOUBLE_EQ(r.search.length_m, 0.0);
  ASSERT_EQ(r.steps.size(), 1U);
  EXPECT_EQ(r.steps[0].maneuver, "arrive");
}

// ------------------------------------------------- buildings and endpoints

TEST_F(RouteServiceTest, RoutesToTheBestOfSeveralEndpoints) {
  // From node 1, node 2 (186 m east) is much closer than node 6 (one block
  // east and north), so the route ends there.
  const std::vector<LatLng> from{{33.4000, -111.9400}};
  const std::vector<LatLng> to{{33.4010, -111.9380}, {33.4000, -111.9390}};
  const auto origins = service_->snap_all(from);
  const auto destinations = service_->snap_all(to);
  const Route r = service_->make_route(
      origins, destinations,
      find_shortest_path_between(service_->graph(), RouteService::node_ids(origins),
                                 RouteService::node_ids(destinations), {Metric::kDistance, Algorithm::kAStar}));
  ASSERT_TRUE(r.search.found);
  EXPECT_EQ(service_->graph().node(r.destination.node).osm_id, 2);
  EXPECT_EQ(r.steps.back().maneuver, "arrive");
}

TEST_F(RouteServiceTest, SkipsEndpointsThatAreTooFar) {
  const std::vector<LatLng> points{{40.0, -100.0}, {33.40005, -111.93905}};
  const auto snapped = service_->snap_all(points);
  ASSERT_EQ(snapped.size(), 1U);
  EXPECT_EQ(service_->graph().node(snapped[0].node).osm_id, 2);

  const std::vector<LatLng> all_far{{40.0, -100.0}};
  EXPECT_THROW((void)service_->snap_all(all_far), RoutingError);
}

TEST(RouteServiceBuildings, SearchesBuildingsByNameAndCode) {
  const std::string dir = UMBRA_TEST_DATA_DIR;
  RouteService service(load_overpass_file(dir + "/mini_city.osm.json"),
                       merge_places(load_campus_file(dir + "/mini_campus.json"),
                                    load_places_file(dir + "/mini_places.json")));

  const auto by_name = service.search("science", 5);
  ASSERT_FALSE(by_name.empty());
  EXPECT_EQ(by_name[0].kind, EntryKind::kBuilding);
  EXPECT_EQ(by_name[0].name, "Science Hall");
  EXPECT_NE(by_name[0].subtitle.find("SCI"), std::string::npos);
  EXPECT_EQ(by_name[0].access_points.size(), 1U);

  // An exact building code ranks that building first, case-insensitively.
  const auto by_code = service.search(" lib ", 5);
  ASSERT_FALSE(by_code.empty());
  EXPECT_EQ(by_code[0].name, "Library");

  // Campus short names work as keywords ("DSU library").
  const auto with_campus = service.search("dsu library", 5);
  ASSERT_FALSE(with_campus.empty());
  EXPECT_EQ(with_campus[0].name, "Library");
}

}  // namespace
}  // namespace umbra
