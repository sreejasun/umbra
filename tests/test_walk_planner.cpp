#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "umbra/osm_loader.hpp"
#include "umbra/walk_planner.hpp"

namespace umbra {
namespace {

const std::string kDataDir = UMBRA_TEST_DATA_DIR;
constexpr std::int64_t kWinterNoon = 1797881400;   // 2026-12-21 12:30 MST: sun due south, ~33 deg up
constexpr std::int64_t kWinterNight = 1797846000;  // 2026-12-21 02:40 MST

const Edge* find_edge(const RoadGraph& g, std::int64_t from, std::int64_t to) {
  for (NodeId u = 0; u < g.node_count(); ++u) {
    if (g.node(u).osm_id != from) continue;
    for (const Edge& e : g.out_edges(u)) {
      if (g.node(e.target).osm_id == to) return &e;
    }
  }
  return nullptr;
}

// ------------------------------------------------------------- loaders

TEST(WalkLoader, BuildsPedestrianNetwork) {
  OsmLoadOptions options;
  options.mode = TravelMode::kWalk;
  const RoadGraph g = load_overpass_file(kDataDir + "/mini_walk.osm.json", options);

  // Motorway, foot=no and access=private ways are excluded, so nodes 7-8 vanish.
  EXPECT_EQ(g.node_count(), 6U);
  // One-way streets are walkable in both directions.
  EXPECT_NE(find_edge(g, 1, 2), nullptr);
  EXPECT_NE(find_edge(g, 2, 1), nullptr);
  // Walking pace: 5 km/h on flat ground, slower on stairs.
  const Edge* street = find_edge(g, 1, 2);
  EXPECT_NEAR(street->length_m / street->duration_s * 3.6, 5.0, 1e-9);
  const Edge* steps = find_edge(g, 3, 6);
  ASSERT_NE(steps, nullptr);
  EXPECT_NEAR(steps->length_m / steps->duration_s * 3.6, 2.5, 1e-9);
  // Covered walkways are flagged; ordinary ones are not.
  EXPECT_TRUE(find_edge(g, 4, 5)->covered());
  EXPECT_FALSE(find_edge(g, 1, 4)->covered());
}

TEST(WalkLoader, DriveModeIgnoresFootways) {
  const RoadGraph drive = load_overpass_file(kDataDir + "/mini_walk.osm.json", {TravelMode::kDrive, false});
  EXPECT_EQ(find_edge(drive, 1, 4), nullptr);    // footway
  EXPECT_EQ(find_edge(drive, 2, 1), nullptr);    // one-way for cars
  EXPECT_NE(find_edge(drive, 4, 7), nullptr);    // motorway is drivable
}

TEST(ShadeLoader, ParsesBuildingsAndTrees) {
  const ShadeCasters c = load_shade_file(kDataDir + "/mini_shade.json");
  // Way buildings (3) + relation outer ring (1); "building=no" is skipped.
  ASSERT_EQ(c.buildings.size(), 4U);
  EXPECT_DOUBLE_EQ(c.buildings[0].height_m, 24.0);           // explicit height
  EXPECT_DOUBLE_EQ(c.buildings[1].height_m, 3.5);            // carport default
  EXPECT_DOUBLE_EQ(c.buildings[2].height_m, 4 * 3.3 + 1.0);  // from building:levels
  EXPECT_DOUBLE_EQ(c.buildings[3].height_m, 12.0);           // university default
  // One tagged tree + a 40 m tree row planted every 8 m (5 trees).
  ASSERT_EQ(c.trees.size(), 6U);
  EXPECT_DOUBLE_EQ(c.trees[0].height_m, 12.0);
  EXPECT_DOUBLE_EQ(c.trees[0].crown_radius_m, 5.0);
}

TEST(ShadeLoader, ParsesLengths) {
  EXPECT_DOUBLE_EQ(*osm::parse_length_m("12"), 12.0);
  EXPECT_DOUBLE_EQ(*osm::parse_length_m("12 m"), 12.0);
  EXPECT_NEAR(*osm::parse_length_m("40 ft"), 12.192, 1e-9);
  EXPECT_NEAR(*osm::parse_length_m("40'"), 12.192, 1e-9);
  EXPECT_FALSE(osm::parse_length_m("tall").has_value());
  EXPECT_THROW((void)parse_shade_casters("{"), MapLoadError);
}

// ------------------------------------------------------------- planner

/**
 * Two ways from A to B (positions in metres, north = up):
 *
 *        C ------------- D        y = 33 m  (shaded by the tall building just south of it)
 *        |   [building]  |
 *        A ------------- B        y = 0     (direct, sunny)
 */
class WalkPlannerTest : public ::testing::Test {
 protected:
  friend class WalkOptionsTest;
  static LatLng at(double east_m, double north_m) {
    constexpr double kLat0 = 33.4000;
    constexpr double kLng0 = -111.9400;
    const double m_lng = 111195.0 * std::cos(kLat0 * 3.141592653589793 / 180.0);
    return {kLat0 + north_m / 111195.0, kLng0 + east_m / m_lng};
  }

  void SetUp() override {
    RoadGraphBuilder b;
    const NodeId a = b.add_node(1, at(0, 0));
    const NodeId bb = b.add_node(2, at(90, 0));
    const NodeId c = b.add_node(3, at(0, 33));
    const NodeId d = b.add_node(4, at(90, 33));
    auto road = [&](NodeId u, NodeId v, const char* name) {
      const double len = haversine_m(b_pos_[u], b_pos_[v]);
      b.add_edge(u, v, len, len / 1.4, b.intern_name(name));
      b.add_edge(v, u, len, len / 1.4, b.intern_name(name));
    };
    b_pos_ = {at(0, 0), at(90, 0), at(0, 33), at(90, 33)};
    road(a, bb, "Sunny Street");
    road(a, c, "West Path");
    road(c, d, "Shady Lane");
    road(d, bb, "East Path");
    network_ = std::make_unique<RouteService>(b.build());

    ShadeCasters casters;  // 30 m tall building between the two east-west paths
    casters.buildings.push_back({{at(-5, 10), at(95, 10), at(95, 25), at(-5, 25)}, 30.0});
    planner_ = std::make_unique<WalkPlanner>(*network_, std::make_shared<const ShadeMap>(std::move(casters)));
  }

  std::vector<LatLng> b_pos_;
  std::unique_ptr<RouteService> network_;
  std::unique_ptr<WalkPlanner> planner_;
};

TEST_F(WalkPlannerTest, ShortestIgnoresTheSun) {
  const WalkPlan plan = planner_->plan(at(0, 0), at(90, 0), {kWinterNoon, ShadePreference::kShortest});
  ASSERT_TRUE(plan.preferred.route.search.found);
  EXPECT_NEAR(plan.preferred.route.search.length_m, 90.0, 1.0);
  EXPECT_LT(plan.preferred.shade_fraction(), 0.05);
}

TEST_F(WalkPlannerTest, ShadePreferenceTakesTheShadedDetour) {
  const WalkPlan plan = planner_->plan(at(0, 0), at(90, 0), {kWinterNoon, ShadePreference::kMaxShade});
  EXPECT_TRUE(plan.sun.is_up());
  EXPECT_NEAR(plan.sun.azimuth_deg, 180.0, 3.0);

  ASSERT_TRUE(plan.preferred.route.search.found);
  EXPECT_NEAR(plan.preferred.route.search.length_m, 156.0, 1.5);  // 33 + 90 + 33
  EXPECT_GT(plan.preferred.shade_fraction(), 0.5);
  // The comparison route is still the plain shortest one.
  EXPECT_NEAR(plan.shortest.route.search.length_m, 90.0, 1.0);
  EXPECT_LT(plan.shortest.shade_fraction(), plan.preferred.shade_fraction());

  // Runs alternate between shade and sun and cover the whole route.
  ASSERT_FALSE(plan.preferred.runs.empty());
  for (std::size_t i = 1; i < plan.preferred.runs.size(); ++i) {
    EXPECT_NE(plan.preferred.runs[i].shaded, plan.preferred.runs[i - 1].shaded);
  }
}

TEST_F(WalkPlannerTest, AStarAgreesWithDijkstra) {
  WalkOptions dijkstra{kWinterNoon, ShadePreference::kBalanced, Algorithm::kDijkstra};
  WalkOptions astar{kWinterNoon, ShadePreference::kBalanced, Algorithm::kAStar};
  const auto d = planner_->plan(at(0, 0), at(90, 0), dijkstra);
  const auto a = planner_->plan(at(0, 0), at(90, 0), astar);
  EXPECT_NEAR(a.preferred.route.search.cost, d.preferred.route.search.cost, 1e-6);
}

TEST_F(WalkPlannerTest, AtNightEverythingIsShadeAndShortestWins) {
  const WalkPlan plan = planner_->plan(at(0, 0), at(90, 0), {kWinterNight, ShadePreference::kMaxShade});
  EXPECT_FALSE(plan.sun.is_up());
  EXPECT_NEAR(plan.preferred.route.search.length_m, 90.0, 1.0);
  EXPECT_DOUBLE_EQ(plan.preferred.shade_fraction(), 1.0);
}

TEST_F(WalkPlannerTest, OvercastSkyMakesShadeIrrelevant) {
  WalkOptions options{kWinterNoon, ShadePreference::kMaxShade};
  options.sun_strength = 0.0;  // live weather: thick clouds, no direct beam
  const WalkPlan plan = planner_->plan(at(0, 0), at(90, 0), options);
  EXPECT_DOUBLE_EQ(plan.sun_strength, 0.0);
  EXPECT_NEAR(plan.preferred.route.search.length_m, 90.0, 1.0);
}

TEST_F(WalkPlannerTest, WeakSunShrinksTheDetourBudget) {
  // The 156 m shaded detour costs 156; the 90 m sunny street costs
  // 90 * (1 + 5 * strength). Full sun favours the detour, weak sun does not.
  WalkOptions options{kWinterNoon, ShadePreference::kMaxShade};
  options.sun_strength = 1.0;
  EXPECT_NEAR(planner_->plan(at(0, 0), at(90, 0), options).preferred.route.search.length_m, 156.0, 1.5);
  options.sun_strength = 0.1;
  EXPECT_NEAR(planner_->plan(at(0, 0), at(90, 0), options).preferred.route.search.length_m, 90.0, 1.0);
  options.sun_strength = 7.0;  // out of range: clamped
  EXPECT_DOUBLE_EQ(planner_->plan(at(0, 0), at(90, 0), options).sun_strength, 1.0);
}

TEST_F(WalkPlannerTest, WalksToTheBestEntrance) {
  // Two candidate "entrances": the far corner and the near one.
  const std::vector<LatLng> from{at(0, 0)};
  const std::vector<LatLng> to{at(90, 33), at(90, 0)};
  const WalkPlan plan = planner_->plan(from, to, {kWinterNight, ShadePreference::kBalanced});
  ASSERT_TRUE(plan.preferred.route.search.found);
  EXPECT_NEAR(plan.preferred.route.search.length_m, 90.0, 1.0);
  EXPECT_LT(haversine_m(plan.preferred.route.destination.position, at(90, 0)), 1.0);
}

// ------------------------------------------- accessibility, light, stops

/// a --(50 m, stairs or unlit)--> b, or the detour a -> c -> b (60 + 60 m).
class WalkOptionsTest : public ::testing::Test {
 protected:
  static LatLng at(double east_m, double north_m) { return WalkPlannerTest::at(east_m, north_m); }

  std::unique_ptr<WalkPlanner> make(std::uint8_t direct_flags, std::uint8_t detour_flags,
                                    std::vector<WalkStop> stops = {}) {
    RoadGraphBuilder b;
    const NodeId a = b.add_node(1, at(0, 0));
    const NodeId bb = b.add_node(2, at(50, 0));
    const NodeId c = b.add_node(3, at(25, 55));
    auto both = [&](NodeId u, NodeId v, double len, std::uint8_t flags) {
      b.add_edge(u, v, len, len / 1.4, 0, flags);
      b.add_edge(v, u, len, len / 1.4, 0, flags);
    };
    both(a, bb, 50, direct_flags);
    both(a, c, 60, detour_flags);
    both(c, bb, 60, detour_flags);
    network_ = std::make_unique<RouteService>(b.build());
    return std::make_unique<WalkPlanner>(*network_, std::make_shared<const ShadeMap>(ShadeCasters{}), std::move(stops));
  }

  std::unique_ptr<RouteService> network_;
};

TEST_F(WalkOptionsTest, StepFreeRoutesAvoidStairs) {
  auto planner = make(kEdgeStairs, 0);
  WalkOptions options{kWinterNight, ShadePreference::kShortest};
  EXPECT_NEAR(planner->plan(at(0, 0), at(50, 0), options).preferred.route.search.length_m, 50.0, 0.5);
  options.avoid_stairs = true;
  const WalkPlan plan = planner->plan(at(0, 0), at(50, 0), options);
  EXPECT_NEAR(plan.preferred.route.search.length_m, 120.0, 0.5);
  EXPECT_NEAR(plan.shortest.route.search.length_m, 120.0, 0.5) << "the comparison route is step-free too";
}

TEST_F(WalkOptionsTest, AtNightLitWaysWin) {
  auto planner = make(kEdgeUnlit, kEdgeLit);
  const WalkPlan night = planner->plan(at(0, 0), at(50, 0), {kWinterNight, ShadePreference::kBalanced});
  EXPECT_TRUE(night.lighting_applied);
  EXPECT_NEAR(night.preferred.route.search.length_m, 120.0, 0.5);  // 120 lit beats 50 x 3 unlit
  EXPECT_NEAR(night.preferred.lit_length_m, 120.0, 0.5);
  EXPECT_NEAR(night.shortest.route.search.length_m, 50.0, 0.5);

  WalkOptions dont_care{kWinterNight, ShadePreference::kBalanced};
  dont_care.prefer_lit = false;
  EXPECT_NEAR(planner->plan(at(0, 0), at(50, 0), dont_care).preferred.route.search.length_m, 50.0, 0.5);
  EXPECT_FALSE(planner->plan(at(0, 0), at(50, 0), {kWinterNoon, ShadePreference::kBalanced}).lighting_applied)
      << "lighting only matters after dark";
}

TEST_F(WalkOptionsTest, UnknownLightingIsOnlyMildlyPenalised) {
  auto planner = make(0, kEdgeLit);  // direct: lighting unknown, 50 x 1.25 < 120
  EXPECT_NEAR(planner->plan(at(0, 0), at(50, 0), {kWinterNight}).preferred.route.search.length_m, 50.0, 0.5);
}

TEST_F(WalkOptionsTest, StopsAtTheCheapestWaterPoint) {
  auto planner = make(0, 0, {{"Far fountain", at(25, 200)}, {"Detour fountain", at(25, 55)}});
  EXPECT_EQ(planner->stop_count(), 1U) << "the far fountain is off the network";
  const std::vector<LatLng> from{at(0, 0)};
  const std::vector<LatLng> to{at(50, 0)};
  const WalkPlan plan = planner->plan_via_stop(from, to, {kWinterNight, ShadePreference::kShortest});
  ASSERT_TRUE(plan.via);
  EXPECT_EQ(plan.via->name, "Detour fountain");
  EXPECT_NEAR(plan.preferred.route.search.length_m, 120.0, 0.5);
  EXPECT_NEAR(plan.shortest.route.search.length_m, 50.0, 0.5) << "comparison: the walk without the stop";
  const auto& steps = plan.preferred.route.steps;
  EXPECT_TRUE(std::any_of(steps.begin(), steps.end(), [](const RouteStep& s) { return s.maneuver == "waypoint"; }));
  EXPECT_EQ(steps.back().maneuver, "arrive");
  EXPECT_EQ(plan.preferred.route.geometry.size(), plan.preferred.route.search.nodes.size());

  auto no_stops = make(0, 0);
  EXPECT_THROW((void)no_stops->plan_via_stop(from, to, {kWinterNight}), RoutingError);
}

TEST_F(WalkOptionsTest, ComparesDepartureTimes) {
  auto planner = make(0, 0);
  const std::vector<LatLng> from{at(0, 0)};
  const std::vector<LatLng> to{at(50, 0)};
  const std::vector<std::int64_t> times{kWinterNight, kWinterNoon};
  const auto options = planner->compare_departures(from, to, times, {}, [](std::int64_t) { return 0.5; });
  ASSERT_EQ(options.size(), 2U);
  EXPECT_FALSE(options[0].sun.is_up());
  EXPECT_TRUE(options[1].sun.is_up());
  EXPECT_DOUBLE_EQ(options[1].sun_strength, 0.5);
  EXPECT_TRUE(options[0].found && options[1].found);
}

TEST(WaterLoader, ParsesPublicWaterPoints) {
  const auto points = load_water_file(kDataDir + "/mini_water.json");
  ASSERT_EQ(points.size(), 3U);  // the private tap is skipped
  EXPECT_EQ(points[0].name, "Bottle filler");
  EXPECT_EQ(points[1].name, "Library fountain");
  EXPECT_NEAR(points[2].position.lat, 33.4002, 1e-9);  // way centre
}

TEST(SunPenalty, IncreasesWithPreference) {
  EXPECT_DOUBLE_EQ(sun_penalty(ShadePreference::kShortest), 0.0);
  EXPECT_LT(sun_penalty(ShadePreference::kBalanced), sun_penalty(ShadePreference::kMaxShade));
}

}  // namespace
}  // namespace umbra
