#include <gtest/gtest.h>

#include <stdexcept>

#include "umbra/osm_loader.hpp"
#include "umbra/route_service.hpp"
#include "umbra/search_index.hpp"

namespace umbra {
namespace {

const std::string kDataDir = UMBRA_TEST_DATA_DIR;

std::vector<std::string> names(const SearchIndex& index, const std::vector<SearchHit>& hits) {
  std::vector<std::string> out;
  for (const SearchHit& h : hits) out.push_back(index.entry(h.entry).name);
  return out;
}

SearchIndex sample_index() {
  SearchIndex index;
  index.add({EntryKind::kStreet, "South Mill Avenue", "Street", "", {}, 0.8});
  index.add({EntryKind::kStreet, "East University Drive", "Street", "", {}, 0.8});
  index.add({EntryKind::kIntersection, "South Mill Avenue & East University Drive", "Intersection", "", {}, 0.3});
  index.add({EntryKind::kIntersection, "South Mill Avenue & 5th Street", "Intersection", "", {}, 0.3});
  index.add({EntryKind::kPlace, "Arizona State University", "University", "ASU", {}, 3.0});
  index.add({EntryKind::kPlace, "Mill Cue Club", "Bar", "", {}, 0.5});
  index.add({EntryKind::kPlace, "McDonald's", "Fast food", "", {}, 0.5});
  index.build();
  return index;
}

// ----------------------------------------------------------------- tokenize

TEST(SearchTokenize, NormalisesCaseAndPunctuation) {
  EXPECT_EQ(SearchIndex::tokenize("McDonald's"), (std::vector<std::string>{"mcdonalds"}));
  EXPECT_EQ(SearchIndex::tokenize("  Mill/University "), (std::vector<std::string>{"mill", "university"}));
}

TEST(SearchTokenize, ExpandsStreetAbbreviations) {
  EXPECT_EQ(SearchIndex::tokenize("S Mill Ave"), (std::vector<std::string>{"south", "mill", "avenue"}));
  EXPECT_EQ(SearchIndex::tokenize("E. University Dr."),
            (std::vector<std::string>{"east", "university", "drive"}));
  EXPECT_EQ(SearchIndex::tokenize("5th St"), (std::vector<std::string>{"5th", "street"}));
}

TEST(SearchTokenize, DropsConnectorWords) {
  EXPECT_EQ(SearchIndex::tokenize("Mill & University"), (std::vector<std::string>{"mill", "university"}));
  EXPECT_EQ(SearchIndex::tokenize("mill and 5th"), (std::vector<std::string>{"mill", "5th"}));
  EXPECT_EQ(SearchIndex::tokenize("mill at 5th"), (std::vector<std::string>{"mill", "5th"}));
}

// ------------------------------------------------------------------- search

TEST(SearchIndex, PrefixMatchesWhileTyping) {
  const SearchIndex index = sample_index();
  const auto hits = index.search("arizona sta");
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(index.entry(hits[0].entry).name, "Arizona State University");
}

TEST(SearchIndex, FindsPlacesByAlternativeName) {
  const SearchIndex index = sample_index();
  const auto hits = index.search("asu");
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(index.entry(hits[0].entry).name, "Arizona State University");
}

TEST(SearchIndex, StreetRanksAboveItsIntersections) {
  const SearchIndex index = sample_index();
  const auto result = names(index, index.search("mill ave"));
  ASSERT_GE(result.size(), 3U);
  EXPECT_EQ(result[0], "South Mill Avenue");
}

TEST(SearchIndex, FindsIntersectionsFromBothStreetNames) {
  const SearchIndex index = sample_index();
  for (const char* query : {"mill & university", "Mill Ave and University Dr", "university mill"}) {
    const auto hits = index.search(query);
    ASSERT_FALSE(hits.empty()) << query;
    EXPECT_EQ(index.entry(hits[0].entry).name, "South Mill Avenue & East University Drive") << query;
  }
  const auto fifth = index.search("mill 5th");
  ASSERT_FALSE(fifth.empty());
  EXPECT_EQ(index.entry(fifth[0].entry).name, "South Mill Avenue & 5th Street");
}

TEST(SearchIndex, ToleratesOneTypo) {
  const SearchIndex index = sample_index();
  const auto hits = index.search("univrsity");
  ASSERT_FALSE(hits.empty());
  EXPECT_NE(index.entry(hits[0].entry).name.find("University"), std::string::npos);
}

TEST(SearchIndex, RequiresEveryWordToMatch) {
  const SearchIndex index = sample_index();
  EXPECT_TRUE(index.search("mill zzzzzz").empty());
  EXPECT_TRUE(index.search("").empty());
  EXPECT_TRUE(index.search("   &  ").empty());
}

TEST(SearchIndex, RespectsLimit) {
  const SearchIndex index = sample_index();
  EXPECT_EQ(index.search("mill", 2).size(), 2U);
  EXPECT_TRUE(index.search("mill", 0).empty());
}

TEST(SearchIndex, EnforcesBuildLifecycle) {
  SearchIndex index;
  index.add({EntryKind::kStreet, "Main Street", "Street", "", {}, 0});
  EXPECT_THROW((void)index.search("main"), std::logic_error);
  index.build();
  EXPECT_THROW(index.add({EntryKind::kStreet, "Oak Street", "Street", "", {}, 0}), std::logic_error);
  EXPECT_EQ(index.search("main").size(), 1U);
}

// ------------------------------------------------------------------- places

TEST(PlacesLoader, ParsesFiltersAndMergesPlaces) {
  const std::vector<Place> places = load_places_file(kDataDir + "/mini_places.json");

  // Kept: university (way center), merged Cactus Coffee, light-rail stop, faraway diner.
  // Skipped: parking lot, unnamed restaurant, relation without coordinates.
  ASSERT_EQ(places.size(), 4U);
  EXPECT_EQ(places[0].name, "Desert State University");
  EXPECT_EQ(places[0].category, "University");
  EXPECT_EQ(places[0].keywords, "DSU");
  EXPECT_NEAR(places[0].position.lat, 33.4004, 1e-9);
  EXPECT_GT(places[0].importance, places[1].importance);
  EXPECT_EQ(places[1].name, "Cactus Coffee");
  EXPECT_EQ(places[2].category, "Light rail stop");
}

TEST(PlacesLoader, RejectsMalformedInput) {
  EXPECT_THROW((void)parse_overpass_places("{"), MapLoadError);
  EXPECT_THROW((void)parse_overpass_places(R"({"nope": []})"), MapLoadError);
}

// ------------------------------------------------- integration with routing

TEST(RouteServiceSearch, IndexesPlacesStreetsAndIntersections) {
  const RouteService service(load_overpass_file(kDataDir + "/mini_city.osm.json"),
                             load_places_file(kDataDir + "/mini_places.json"));

  const auto place = service.search("dsu");
  ASSERT_FALSE(place.empty());
  EXPECT_EQ(place[0].name, "Desert State University");
  EXPECT_EQ(place[0].kind, EntryKind::kPlace);
  EXPECT_NE(place[0].subtitle.find("near"), std::string::npos);

  const auto street = service.search("third ave");
  ASSERT_FALSE(street.empty());
  EXPECT_EQ(street[0].name, "Third Avenue");
  EXPECT_EQ(street[0].kind, EntryKind::kStreet);

  const auto crossing = service.search("main & third");
  ASSERT_FALSE(crossing.empty());
  EXPECT_EQ(crossing[0].kind, EntryKind::kIntersection);
  EXPECT_NE(crossing[0].name.find("Main Street"), std::string::npos);
  EXPECT_NE(crossing[0].name.find("Third Avenue"), std::string::npos);

  // Places outside the road network's coverage are not searchable.
  EXPECT_TRUE(service.search("faraway").empty());
  // Roads that were filtered out (private, footway) are not searchable either.
  EXPECT_TRUE(service.search("private drive").empty());
  EXPECT_TRUE(service.search("campus path").empty());
}

}  // namespace
}  // namespace umbra
