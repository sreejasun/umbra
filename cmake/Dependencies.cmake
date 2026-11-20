# Third-party dependencies, pinned to exact releases and fetched at configure time.
include(FetchContent)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# SYSTEM: third-party headers do not trigger our strict warning flags. CMake
# 3.25.0/3.25.1 wrongly forward that keyword into the download step ("At least
# one entry of URL is a path"), so it is only used from 3.25.2 on.
set(UMBRA_FETCH_SYSTEM SYSTEM)
if(CMAKE_VERSION VERSION_LESS 3.25.2)
  set(UMBRA_FETCH_SYSTEM "")
endif()

# JSON parsing / serialization
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  ${UMBRA_FETCH_SYSTEM})

# Single-header HTTP server
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
  ${UMBRA_FETCH_SYSTEM})

FetchContent_MakeAvailable(nlohmann_json httplib)

if(UMBRA_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    ${UMBRA_FETCH_SYSTEM})
  FetchContent_MakeAvailable(googletest)
endif()
