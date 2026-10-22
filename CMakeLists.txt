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

# FFmpeg (avcodec, swscale, avutil) and Opus: vcpkg's packages for MSVC,
# the static libraries ci/mingw-deps.sh builds for MinGW.
add_library(moonlight_media INTERFACE)
if(MSVC)
    find_package(FFMPEG REQUIRED)
    find_package(Opus CONFIG REQUIRED)
    target_include_directories(moonlight_media SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(moonlight_media INTERFACE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(moonlight_media INTERFACE ${FFMPEG_LIBRARIES} Opus::opus)
else()
    find_package(PkgConfig REQUIRED)
    # Only the prefixes' .pc files, never the build machine's own.
    set(pc_dirs "")
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND pc_dirs "${prefix}/lib/pkgconfig")
    endforeach()
    string(REPLACE ";" ":" pc_dirs "${pc_dirs}")
    set(ENV{PKG_CONFIG_LIBDIR} "${pc_dirs}")
    pkg_check_modules(MEDIA REQUIRED libavcodec libswscale libavutil opus)
    target_include_directories(moonlight_media SYSTEM INTERFACE ${MEDIA_STATIC_INCLUDE_DIRS})
    target_link_libraries(moonlight_media INTERFACE ${MEDIA_STATIC_LDFLAGS})
endif()

# moonlight-common-c (GPL-3.0) with its ENet fork and nanors, as a static
# library. Its CMake file uses check_cxx_source_compiles without
# including the module.
include(CheckCXXSourceCompiles)
set(BUILD_SHARED_LIBS OFF)
set(ENET_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/moonlight-common-c EXCLUDE_FROM_ALL)
# It links libcrypto by path; a static libcrypto needs these after it.
target_link_libraries(moonlight-common-c PRIVATE ws2_32 crypt32 gdi32 advapi32 user32 bcrypt)
if(MSVC)
    # It builds with /WX; warnings a newer compiler finds in its code
    # shouldn't stop this build.
    get_target_property(mlc_options moonlight-common-c COMPILE_OPTIONS)
    list(REMOVE_ITEM mlc_options /WX)
    set_property(TARGET moonlight-common-c PROPERTY COMPILE_OPTIONS ${mlc_options})
endif()

add_library(pugixml STATIC third_party/pugixml/pugixml.cpp)
target_include_directories(pugixml SYSTEM PUBLIC third_party/pugixml)

# Everything but the stream itself: options, keys, the control API,
# pairing, the data folder, the PIN screen and the frame path. The unit
# tests link this.
add_library(moonlight_core STATIC
    src/moonlight/frames.cpp
    src/moonlight/http.cpp
    src/moonlight/identity.cpp
    src/moonlight/keymap.cpp
    src/moonlight/nvhttp.cpp
    src/moonlight/options.cpp
    src/moonlight/pairing.cpp
    src/moonlight/screen.cpp
    src/moonlight/xml.cpp)
target_include_directories(moonlight_core PUBLIC src)
target_link_libraries(moonlight_core PUBLIC casement pugixml OpenSSL::SSL OpenSSL::Crypto
    ws2_32 crypt32 bcrypt gdi32 user32 advapi32 shell32 ole32)

casement_plugin_executable(casement-moonlight
    src/main.cpp
    src/moonlight/audio.cpp
    src/moonlight/session.cpp
    src/moonlight/video.cpp)
target_link_libraries(casement-moonlight PRIVATE moonlight_core moonlight-common-c moonlight_media
    d3d11 dxgi ole32 winmm ws2_32 crypt32 bcrypt)
target_compile_definitions(casement-moonlight PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")

casement_test(unit_tests unit
    tests/unit/main.cpp
    tests/unit/frame_tests.cpp
    tests/unit/http_tests.cpp
    tests/unit/identity_tests.cpp
    tests/unit/keymap_tests.cpp
    tests/unit/nvhttp_tests.cpp
    tests/unit/options_tests.cpp
    tests/unit/pairing_tests.cpp
    tests/unit/xml_tests.cpp)
target_link_libraries(unit_tests PRIVATE moonlight_core)

casement_test(e2e_tests e2e tests/e2e/moonlight_tests.cpp)
target_link_libraries(e2e_tests PRIVATE casement_sim moonlight_core)
target_compile_definitions(e2e_tests PRIVATE PLUGIN_EXE=L"$<TARGET_FILE_NAME:casement-moonlight>")
add_dependencies(e2e_tests casement-moonlight)
# The end-to-end tests drive a real host: give them time.
set_tests_properties(e2e_tests PROPERTIES TIMEOUT 900)
