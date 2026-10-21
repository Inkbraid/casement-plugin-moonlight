cmake_minimum_required(VERSION 3.24)
project(casement_moonlight VERSION 0.1.0 LANGUAGES CXX)

# MSVC links everything statically, the C runtime included, to match
# vcpkg's static triplets: the plugin is one exe.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/casement.cmake)
enable_testing()

# Cross builds take their libraries from CMAKE_PREFIX_PATH (/opt/mingw64).
if(CMAKE_CROSSCOMPILING)
    list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
endif()

find_package(OpenSSL 3.0 REQUIRED)

add_library(pugixml STATIC third_party/pugixml/pugixml.cpp)
target_include_directories(pugixml SYSTEM PUBLIC third_party/pugixml)

# Everything but the stream itself: options, keys, the control API,
# pairing and the data folder. The unit tests link this.
add_library(moonlight_core STATIC
    src/moonlight/http.cpp
    src/moonlight/identity.cpp
    src/moonlight/keymap.cpp
    src/moonlight/nvhttp.cpp
    src/moonlight/options.cpp
    src/moonlight/pairing.cpp
    src/moonlight/xml.cpp)
target_include_directories(moonlight_core PUBLIC src)
target_link_libraries(moonlight_core PUBLIC casement pugixml OpenSSL::SSL OpenSSL::Crypto
    ws2_32 crypt32 bcrypt gdi32 user32 advapi32 shell32 ole32)

casement_test(unit_tests unit
    tests/unit/main.cpp
    tests/unit/http_tests.cpp
    tests/unit/identity_tests.cpp
    tests/unit/keymap_tests.cpp
    tests/unit/nvhttp_tests.cpp
    tests/unit/options_tests.cpp
    tests/unit/pairing_tests.cpp
    tests/unit/xml_tests.cpp)
target_link_libraries(unit_tests PRIVATE moonlight_core)
