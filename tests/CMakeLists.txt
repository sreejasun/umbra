include(GoogleTest)

add_executable(umbra_tests
  test_geo.cpp
  test_indexed_min_heap.cpp
  test_road_graph.cpp
  test_shortest_path.cpp
  test_spatial_index.cpp
  test_osm_loader.cpp
  test_route_service.cpp
  test_search_index.cpp
  test_solar.cpp
  test_geometry2d.cpp
  test_shade_map.cpp
  test_walk_planner.cpp
  test_weather.cpp
  test_drive_conditions.cpp
  test_user_store.cpp
  test_security.cpp
  test_assistant.cpp
)
target_include_directories(umbra_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(umbra_tests PRIVATE umbra_server_lib GTest::gtest_main)
target_compile_definitions(umbra_tests PRIVATE
  UMBRA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
umbra_set_warnings(umbra_tests)

gtest_discover_tests(umbra_tests)

# Browser-module unit tests (navigation, safety, departures) run with Node's
# built-in test runner when Node is installed; the C++ suite does not need it.
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  file(GLOB UMBRA_WEB_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/web/*.test.mjs)
  add_test(NAME web_unit_tests COMMAND ${NODE_EXECUTABLE} --test ${UMBRA_WEB_TESTS}
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
else()
  message(STATUS "node not found: skipping the web unit tests (run `npm test` once Node is installed)")
endif()
