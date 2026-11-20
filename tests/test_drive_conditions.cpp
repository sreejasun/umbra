// Drive conditions: live traffic (HERE flow parsing and map matching), weather
// slowdowns (FHWA ranges) and sun glare, plus end-to-end route scenarios.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

#include "umbra/shortest_path.hpp"
#include "umbra/solar.hpp"
#include "umbra/traffic.hpp"

namespace umbra {
namespace {

std::string read_fixture(const char* name) {
  std::ifstream in(std::string(UMBRA_TEST_DATA_DIR) + "/" + name);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

/// 0 -> 1 -> 3 is a 90 km/h freeway (1,000 m legs, 40 s each); 0 -> 2 -> 3 a
/// 30 km/h street (600 m legs, 72 s each). Free flow: freeway 80 s, street 144 s.
RoadGraph freeway_and_street() {
  RoadGraphBuilder b;
  b.add_node(0, {33.40, -111.95});
  b.add_node(1, {33.41, -111.94});
  b.add_node(2, {33.40, -111.94});
  b.add_node(3, {33.41, -111.93});
  b.add_edge(0, 1, 1000, 40, b.intern_name("Loop 202"));
  b.add_edge(1, 3, 1000, 40, b.intern_name("Loop 202"));
  b.add_edge(0, 2, 600, 72, b.intern_name("Apache Blvd"));
  b.add_edge(2, 3, 600, 72, b.intern_name("Apache Blvd"));
  return b.build();
}

/// Fastest route under conditions, as the API computes it.
SearchResult drive(const RoadGraph& g, const DriveConditions& conditions) {
  const std::vector<NodeId> from{0};
  const std::vector<NodeId> to{3};
  return find_shortest_path_between(
      g, from, to, [&](EdgeId id, const Edge& e) { return conditioned_duration_s(id, e, conditions); },
      1.0 / g.max_speed_mps(), {Metric::kTime, Algorithm::kAStar});
}

WeatherSample weather(int code, double precipitation_mm = 0.0) {
  WeatherSample w;
  w.weather_code = code;
  w.precipitation_mm = precipitation_mm;
  return w;
}

// -------------------------------------------------------------- traffic data

TEST(TrafficParse, ReadsFlowShapesAndSpeeds) {
  const TrafficSnapshot s = parse_here_flow(read_fixture("here_flow_sample.json"));
  EXPECT_EQ(s.source_updated, "2026-11-20T22:00:00Z");
  // Results without a shape or without a free-flow speed are dropped.
  ASSERT_EQ(s.flows.size(), 2U);
  EXPECT_DOUBLE_EQ(s.flows[0].speed_mps, 5.0);
  EXPECT_DOUBLE_EQ(s.flows[0].free_flow_mps, 25.0);
  EXPECT_EQ(s.flows[0].shape.size(), 2U);
  EXPECT_DOUBLE_EQ(s.flows[1].speed_mps, 0.0) << "a closed road (jam factor 10) reports no speed";
}

TEST(TrafficParse, RejectsBadResponses) {
  EXPECT_THROW((void)parse_here_flow("nope"), TrafficError);
  EXPECT_THROW((void)parse_here_flow(R"({"error": "Unauthorized"})"), TrafficError);
}

TEST(TrafficParse, BuildsTheBoundingBoxUrlWestSouthEastNorth) {
  const std::string url = here_flow_url("https://example.test/v7/flow", {33.39, -111.965, 33.44, -111.895}, "KEY");
  EXPECT_EQ(url, "https://example.test/v7/flow?locationReferencing=shape&in=bbox:-111.96500,33.39000,-111.89500,"
                 "33.44000&apiKey=KEY");
}

TEST(TrafficMatch, MatchesOnlyEdgesRunningTheSameWay) {
  RoadGraph g = freeway_and_street();
  const TrafficMatch m = match_traffic(g, parse_here_flow(read_fixture("here_flow_sample.json")));
  EXPECT_EQ(m.matched_edges, 1U);
  EXPECT_NEAR(m.speed_factor[0], 0.2, 1e-6);  // 0 -> 1: 5 / 25 m/s
  for (EdgeId id = 1; id < g.edge_count(); ++id) EXPECT_FLOAT_EQ(m.speed_factor[id], 1.0F);

  // The same road in the opposite direction must not take the eastbound speed.
  RoadGraphBuilder b;
  b.add_node(0, {33.41, -111.94});
  b.add_node(1, {33.40, -111.95});
  b.add_edge(0, 1, 1300, 52);
  const RoadGraph westbound = b.build();
  EXPECT_EQ(match_traffic(westbound, parse_here_flow(read_fixture("here_flow_sample.json"))).matched_edges, 0U);
}

TEST(TrafficService, FetchesMatchesAndCaches) {
  const RoadGraph g = freeway_and_street();
  int calls = 0;
  std::string url;
  TrafficService service(g, "SECRET", [&](const std::string& u) {
    ++calls;
    url = u;
    return read_fixture("here_flow_sample.json");
  });
  ASSERT_TRUE(service.current());
  EXPECT_EQ(service.current()->matched_edges, 1U);
  EXPECT_EQ(calls, 1);
  EXPECT_NE(url.find("apiKey=SECRET"), std::string::npos);
}

// ------------------------------------------------------------------ weather

TEST(WeatherSlowdown, UsesFhwaMidpoints) {
  const WeatherSlowdown dry = drive_weather_slowdown(weather(0));
  EXPECT_DOUBLE_EQ(dry.freeway, 1.0);
  EXPECT_DOUBLE_EQ(dry.street, 1.0);
  EXPECT_TRUE(dry.reason.empty());

  const WeatherSlowdown rain = drive_weather_slowdown(weather(61, 1.0));
  EXPECT_NEAR(rain.freeway, 0.925, 1e-9);  // light rain: -2..13 % -> 7.5 %
  EXPECT_NEAR(rain.street, 0.825, 1e-9);   // wet arterials: -10..25 % -> 17.5 %
  EXPECT_EQ(rain.reason, "Rain");

  const WeatherSlowdown heavy = drive_weather_slowdown(weather(63, 9.0));
  EXPECT_NEAR(heavy.freeway, 0.885, 1e-9);  // heavy rain: -6..17 % -> 11.5 %
  EXPECT_EQ(heavy.reason, "Heavy rain");

  EXPECT_NEAR(drive_weather_slowdown(weather(45)).freeway, 0.89, 1e-9);  // low visibility -10..12 %
  EXPECT_NEAR(drive_weather_slowdown(weather(73)).street, 0.65, 1e-9);   // snowy arterials -30..40 %
}

// ---------------------------------------------------------------- scenarios

TEST(DriveScenario, NoTrafficTakesTheFreeway) {
  const RoadGraph g = freeway_and_street();
  const SearchResult r = drive(g, {});
  ASSERT_TRUE(r.found);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{0, 1, 3}));
  EXPECT_DOUBLE_EQ(r.cost, 80.0);
}

TEST(DriveScenario, AJamSendsTheRouteDownTheStreet) {
  const RoadGraph g = freeway_and_street();
  DriveConditions jam;
  auto traffic = std::make_shared<TrafficMatch>();
  traffic->speed_factor.assign(g.edge_count(), 1.0F);
  traffic->speed_factor[0] = 0.2F;  // freeway crawling at a fifth of free flow: 200 s + 40 s
  jam.traffic = traffic;
  const SearchResult r = drive(g, jam);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{0, 2, 3}));
  EXPECT_DOUBLE_EQ(r.cost, 144.0);
}

TEST(DriveScenario, RainSlowsEveryRoadButKeepsTheFreeway) {
  const RoadGraph g = freeway_and_street();
  DriveConditions rain;
  rain.weather = drive_weather_slowdown(weather(61, 1.0));
  const SearchResult r = drive(g, rain);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{0, 1, 3}));
  EXPECT_NEAR(r.cost, 80.0 / 0.925, 1e-9);
}

TEST(DriveScenario, JamAndHeavyRainCombine) {
  const RoadGraph g = freeway_and_street();
  DriveConditions storm;
  auto traffic = std::make_shared<TrafficMatch>();
  traffic->speed_factor.assign(g.edge_count(), 0.5F);
  storm.traffic = traffic;
  storm.weather = drive_weather_slowdown(weather(65));
  const SearchResult r = drive(g, storm);
  EXPECT_NEAR(r.cost, 80.0 / (0.5 * 0.885), 1e-6);
}

TEST(DriveScenario, ConditionsNeverMakeARoadFaster) {
  const RoadGraph g = freeway_and_street();
  DriveConditions odd;
  auto traffic = std::make_shared<TrafficMatch>();
  traffic->speed_factor.assign(g.edge_count(), 1.0F);
  odd.traffic = traffic;
  for (EdgeId id = 0; id < g.edge_count(); ++id) {
    EXPECT_GE(conditioned_duration_s(id, g.edge(id), odd), g.edge(id).duration_s);
  }
}

// ------------------------------------------------------- glare avoidance

/// Destination due west: straight into a setting sun (40 s), or a dog-leg
/// north-west then south-west (2 x 40 s) that keeps the sun off-axis.
RoadGraph west_or_dogleg() {
  RoadGraphBuilder b;
  b.add_node(0, {33.400, -111.940});
  b.add_node(1, {33.405, -111.945});
  b.add_node(2, {33.400, -111.950});
  b.add_edge(0, 2, 930, 40, b.intern_name("West Street"));
  b.add_edge(0, 1, 700, 40, b.intern_name("Northwest Road"));
  b.add_edge(1, 2, 700, 40, b.intern_name("Southwest Road"));
  return b.build();
}

SearchResult drive_west(const RoadGraph& g, const DriveConditions& c) {
  const std::vector<NodeId> from{0};
  const std::vector<NodeId> to{2};
  return find_shortest_path_between(
      g, from, to, [&](EdgeId id, const Edge& e) { return drive_cost_s(g, id, e, c); }, 1.0 / g.max_speed_mps(),
      {Metric::kTime, Algorithm::kAStar});
}

TEST(GlareAvoidance, SunsetDriveTakesTheDogLeg) {
  const RoadGraph g = west_or_dogleg();
  DriveConditions c;
  c.glare.sun = {268.0, 6.0};  // setting sun, due west
  EXPECT_EQ(drive_west(g, c).nodes, (std::vector<NodeId>{0, 2})) << "off: fastest, into the sun";
  c.glare.enabled = true;
  const SearchResult r = drive_west(g, c);
  EXPECT_EQ(r.nodes, (std::vector<NodeId>{0, 1, 2})) << "on: 80 s off-axis beats 40 s x 3 into the sun";
  EXPECT_DOUBLE_EQ(r.cost, 80.0);
}

TEST(GlareAvoidance, NoSunNoDetour) {
  const RoadGraph g = west_or_dogleg();
  DriveConditions night;
  night.glare = {true, {268.0, -10.0}, kGlareMaxElevationDeg};
  EXPECT_EQ(drive_west(g, night).nodes, (std::vector<NodeId>{0, 2}));
}

TEST(GlareAvoidance, LowSeatDriversAvoidAHigherSun) {
  const RoadGraph g = west_or_dogleg();
  const Edge& west = g.edge(g.first_edge(0));  // 0 -> 2 is added first
  GlareAvoidance glare{true, {270.0, 30.0}, kGlareMaxElevationDeg};
  EXPECT_FALSE(edge_into_sun(g, west, glare)) << "30 degrees: the visor blocks it for most drivers";
  glare.max_elevation_deg = kGlareMaxElevationLowSeatDeg;
  EXPECT_TRUE(edge_into_sun(g, west, glare)) << "but not when the visor sits above the driver's eyes";
}

TEST(GlareAvoidance, PenaltyKeepsTheHeuristicAdmissible) {
  const RoadGraph g = west_or_dogleg();
  DriveConditions c;
  c.glare = {true, {268.0, 6.0}, kGlareMaxElevationDeg};
  for (EdgeId id = 0; id < g.edge_count(); ++id) EXPECT_GE(drive_cost_s(g, id, g.edge(id), c), g.edge(id).duration_s);
}

// -------------------------------------------------------------------- glare

TEST(SunGlare, LowSunAheadOnly) {
  const SunPosition low_west{270.0, 8.0};
  EXPECT_TRUE(sun_glare(270.0, low_west));    // driving west at sunset
  EXPECT_TRUE(sun_glare(250.0, low_west));    // within 25 degrees
  EXPECT_FALSE(sun_glare(220.0, low_west));   // 50 degrees off
  EXPECT_FALSE(sun_glare(90.0, low_west));    // sun behind
  EXPECT_FALSE(sun_glare(270.0, {270.0, 45.0}));  // high sun: the visor blocks it
  EXPECT_FALSE(sun_glare(270.0, {270.0, -5.0}));  // night
}

}  // namespace
}  // namespace umbra
