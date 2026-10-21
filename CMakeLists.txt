cmake_minimum_required(VERSION 3.24)
project(casement_moonlight VERSION 0.1.0 LANGUAGES CXX)

# MSVC links everything statically, the C runtime included, to match
# vcpkg's static triplets: the plugin is one exe.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/casement.cmake)
enable_testing()

# Everything but the stream itself. The unit tests link this.
add_library(moonlight_core STATIC
    src/moonlight/keymap.cpp
    src/moonlight/options.cpp)
target_include_directories(moonlight_core PUBLIC src)
target_link_libraries(moonlight_core PUBLIC casement)

casement_test(unit_tests unit
    tests/unit/main.cpp
    tests/unit/keymap_tests.cpp
    tests/unit/options_tests.cpp)
target_link_libraries(unit_tests PRIVATE moonlight_core)
