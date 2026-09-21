cmake_minimum_required(VERSION 3.24)

project(mc_terrain_gen_render VERSION 0.1.0 LANGUAGES CXX)

option(MCWORLD_BUILD_VIEWER "Build the interactive Raylib viewer" ON)
option(MCWORLD_BUILD_TESTS "Build unit tests" ON)
option(MCWORLD_FETCH_FAITHFUL_TEXTURES "Download Faithful 32x textures for the voxel viewer" ON)

add_library(mcworld
    src/noise.cpp
    src/spline.cpp
    src/worldgen.cpp
    src/isosurface.cpp
)
add_library(mcworld::mcworld ALIAS mcworld)

target_compile_features(mcworld PUBLIC cxx_std_20)
target_include_directories(mcworld PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(mcworld PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(mcworld PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcworld PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MCWORLD_BUILD_VIEWER)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG c1ab645ca298a2801097931d1079b10ff7eb9df8 # 5.5
    )
    FetchContent_MakeAvailable(raylib)

    add_executable(terrain_viewer src/main.cpp src/voxel_renderer.cpp)
    target_compile_features(terrain_viewer PRIVATE cxx_std_20)
    target_link_libraries(terrain_viewer PRIVATE mcworld::mcworld raylib)

    if(MCWORLD_FETCH_FAITHFUL_TEXTURES)
        set(FAITHFUL_ARCHIVE "${CMAKE_CURRENT_BINARY_DIR}/_faithful-32x/Faithful-32x-26.3.zip")
        set(FAITHFUL_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/_faithful-32x/extracted")
        if(NOT EXISTS "${FAITHFUL_ARCHIVE}")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_faithful-32x")
            file(DOWNLOAD
                "https://cdn.modrinth.com/data/w0TnApzs/versions/lDYpMiqk/Faithful%2032x%20-%2026.3.zip"
                "${FAITHFUL_ARCHIVE}"
                EXPECTED_HASH SHA512=e90ec63af7f239dbb99aa20c98b23d830e76898f65d3e31b7f0b53bb36db12dcd8c38d6930c743237c9c3db7bf708423fa1d1b2fd497dd6c281dbe216b779db9
                STATUS FAITHFUL_DOWNLOAD_STATUS
                SHOW_PROGRESS
            )
            list(GET FAITHFUL_DOWNLOAD_STATUS 0 FAITHFUL_DOWNLOAD_ERROR)
            if(NOT FAITHFUL_DOWNLOAD_ERROR EQUAL 0)
                list(GET FAITHFUL_DOWNLOAD_STATUS 1 FAITHFUL_DOWNLOAD_MESSAGE)
                message(FATAL_ERROR "Could not download Faithful 32x: ${FAITHFUL_DOWNLOAD_MESSAGE}")
            endif()
        endif()
        file(ARCHIVE_EXTRACT
            INPUT "${FAITHFUL_ARCHIVE}"
            DESTINATION "${FAITHFUL_EXTRACT_DIR}"
            PATTERNS
                "assets/minecraft/textures/block/grass_block_top.png"
                "assets/minecraft/textures/block/grass_block_side.png"
                "assets/minecraft/textures/block/dirt.png"
                "assets/minecraft/textures/block/stone.png"
                "assets/minecraft/textures/block/deepslate.png"
        )
        target_compile_definitions(terrain_viewer PRIVATE
            MCWORLD_FAITHFUL_TEXTURE_DIR="${FAITHFUL_EXTRACT_DIR}/assets/minecraft/textures/block"
        )
    endif()
endif()

if(MCWORLD_BUILD_TESTS)
    enable_testing()
    add_executable(mcworld_tests tests/worldgen_tests.cpp)
    target_compile_features(mcworld_tests PRIVATE cxx_std_20)
    target_link_libraries(mcworld_tests PRIVATE mcworld::mcworld)
    target_include_directories(mcworld_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME mcworld_tests COMMAND mcworld_tests)
    if(MCWORLD_BUILD_VIEWER)
        add_test(NAME viewer_headless COMMAND terrain_viewer --seed 12345 --chunk-x -1 --chunk-z 2 --headless)
        add_test(NAME viewer_voxel_headless COMMAND terrain_viewer --seed 12345 --chunk-x -1 --chunk-z 2 --voxel --headless)
        set_tests_properties(viewer_voxel_headless PROPERTIES
            PASS_REGULAR_EXPRESSION "solid_voxels=[1-9][0-9]* voxel_faces=[1-9][0-9]*"
        )
        add_test(NAME viewer_invalid_chunk COMMAND terrain_viewer --chunk-x 2147483647 --headless)
        set_tests_properties(viewer_invalid_chunk PROPERTIES WILL_FAIL TRUE)
    endif()
endif()
