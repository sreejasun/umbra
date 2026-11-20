cmake_minimum_required(VERSION 3.25)

project(Umbra
  VERSION 1.0.0
  DESCRIPTION "Drive and shade-aware walking routes on real OpenStreetMap data"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(UMBRA_BUILD_TESTS      "Build unit tests"            ON)
option(UMBRA_BUILD_TOOLS      "Build benchmark tool"        ON)
option(UMBRA_ENABLE_SANITIZERS "Build with ASan and UBSan" OFF)
option(UMBRA_ENABLE_LIVE_DATA "Live weather/traffic/assistant via libcurl" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Dependencies)

find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
# Core library: graph, priority queue, search algorithms, OSM import
# ----------------------------------------------------------------------------
add_library(umbra_core
  src/core/geo.cpp
  src/core/road_graph.cpp
  src/core/shade_map.cpp
  src/core/shortest_path.cpp
  src/core/solar.cpp
  src/core/spatial_index.cpp
  src/core/traffic.cpp
  src/core/weather.cpp
  src/io/osm_loader.cpp
  src/service/route_service.cpp
  src/service/search_index.cpp
  src/service/walk_planner.cpp
)
add_library(Umbra::core ALIAS umbra_core)
target_include_directories(umbra_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(umbra_core PRIVATE nlohmann_json::nlohmann_json)
umbra_set_warnings(umbra_core)

# ----------------------------------------------------------------------------
# Storage: accounts, sessions, saved places, trip history (SQLite + libsodium)
# ----------------------------------------------------------------------------
find_package(SQLite3 REQUIRED)  # ships with macOS; Debian/Ubuntu: libsqlite3-dev
find_package(Sodium REQUIRED)   # macOS: brew install libsodium; Debian/Ubuntu: libsodium-dev

add_library(umbra_storage src/storage/user_store.cpp)
add_library(Umbra::storage ALIAS umbra_storage)
target_include_directories(umbra_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(umbra_storage PRIVATE SQLite::SQLite3 Sodium::Sodium)
umbra_set_warnings(umbra_storage)

# ----------------------------------------------------------------------------
# Server library (API, security, assistant, config) + the umbra_server binary
# ----------------------------------------------------------------------------
add_library(umbra_server_lib
  src/server/api_common.cpp
  src/server/assistant.cpp
  src/server/config.cpp
  src/server/http_api.cpp
  src/server/http_client.cpp
  src/server/security.cpp
  src/server/trip_queries.cpp
)
target_include_directories(umbra_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(umbra_server_lib PUBLIC
  Umbra::core Umbra::storage nlohmann_json::nlohmann_json httplib::httplib Threads::Threads)
target_compile_definitions(umbra_server_lib PRIVATE
  UMBRA_DEFAULT_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
  UMBRA_DEFAULT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Live weather, traffic and the assistant need an HTTPS client. libcurl ships
# with macOS; on Debian/Ubuntu install libcurl4-openssl-dev. Without it the
# server builds and runs offline.
set(UMBRA_HAS_LIBCURL 0)
if(UMBRA_ENABLE_LIVE_DATA)
  find_package(CURL)
  if(CURL_FOUND)
    target_link_libraries(umbra_server_lib PRIVATE CURL::libcurl)
    set(UMBRA_HAS_LIBCURL 1)
  else()
    message(WARNING "libcurl not found: building umbra_server without live weather, traffic or assistant")
  endif()
endif()
target_compile_definitions(umbra_server_lib PRIVATE UMBRA_HAS_LIBCURL=${UMBRA_HAS_LIBCURL})
umbra_set_warnings(umbra_server_lib)

add_executable(umbra_server src/server/main.cpp)
target_link_libraries(umbra_server PRIVATE umbra_server_lib)
umbra_set_warnings(umbra_server)

# ----------------------------------------------------------------------------
# Tools
# ----------------------------------------------------------------------------
if(UMBRA_BUILD_TOOLS)
  add_executable(umbra_bench tools/benchmark.cpp)
  target_link_libraries(umbra_bench PRIVATE Umbra::core)
  target_compile_definitions(umbra_bench PRIVATE
    UMBRA_DEFAULT_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data/tempe.osm.json")
  umbra_set_warnings(umbra_bench)
endif()

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
if(UMBRA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
