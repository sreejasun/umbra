# Find libsodium (password hashing and secure random tokens for accounts).
# Defines Sodium_FOUND and the imported target Sodium::Sodium.
#   macOS:  brew install libsodium      Debian/Ubuntu:  apt install libsodium-dev
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_SODIUM QUIET libsodium)
endif()

find_path(Sodium_INCLUDE_DIR sodium.h HINTS ${PC_SODIUM_INCLUDE_DIRS} /opt/homebrew/include /usr/local/include)
find_library(Sodium_LIBRARY NAMES sodium libsodium HINTS ${PC_SODIUM_LIBRARY_DIRS} /opt/homebrew/lib /usr/local/lib)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Sodium REQUIRED_VARS Sodium_LIBRARY Sodium_INCLUDE_DIR)

if(Sodium_FOUND AND NOT TARGET Sodium::Sodium)
  add_library(Sodium::Sodium UNKNOWN IMPORTED)
  set_target_properties(Sodium::Sodium PROPERTIES
    IMPORTED_LOCATION "${Sodium_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}")
endif()
mark_as_advanced(Sodium_INCLUDE_DIR Sodium_LIBRARY)
