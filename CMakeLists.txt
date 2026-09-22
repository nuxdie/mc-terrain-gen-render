cmake_minimum_required(VERSION 3.24)

project(mc_terrain_gen_render VERSION 0.1.0 LANGUAGES CXX)

option(MCWORLD_BUILD_VIEWER "Build the interactive Raylib viewer" ON)
option(MCWORLD_BUILD_TESTS "Build unit tests" ON)
option(MCWORLD_FETCH_FAITHFUL_TEXTURES "Download Faithful 32x textures for the viewer" ON)

add_library(mcworld
    src/noise.cpp
    src/spline.cpp
    src/worldgen.cpp
    src/isosurface.cpp
    src/biome.cpp
    src/biome_environment.cpp
    src/terrain.cpp
    src/aquifer.cpp
    src/materials.cpp
    src/carvers.cpp
    src/generation.cpp
    src/structures.cpp
    src/structure_templates.cpp
    src/decoration.cpp
    src/feature_placement.cpp
    src/features.cpp
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
        set(FAITHFUL_SHA512 "e90ec63af7f239dbb99aa20c98b23d830e76898f65d3e31b7f0b53bb36db12dcd8c38d6930c743237c9c3db7bf708423fa1d1b2fd497dd6c281dbe216b779db9")
        if(EXISTS "${FAITHFUL_ARCHIVE}")
            file(SHA512 "${FAITHFUL_ARCHIVE}" FAITHFUL_ARCHIVE_SHA512)
        endif()
        if(NOT FAITHFUL_ARCHIVE_SHA512 STREQUAL FAITHFUL_SHA512)
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_faithful-32x")
            file(DOWNLOAD
                "https://cdn.modrinth.com/data/w0TnApzs/versions/lDYpMiqk/Faithful%2032x%20-%2026.3.zip"
                "${FAITHFUL_ARCHIVE}"
                EXPECTED_HASH SHA512=${FAITHFUL_SHA512}
                STATUS FAITHFUL_DOWNLOAD_STATUS
                SHOW_PROGRESS
            )
            list(GET FAITHFUL_DOWNLOAD_STATUS 0 FAITHFUL_DOWNLOAD_ERROR)
            if(NOT FAITHFUL_DOWNLOAD_ERROR EQUAL 0)
                list(GET FAITHFUL_DOWNLOAD_STATUS 1 FAITHFUL_DOWNLOAD_MESSAGE)
                message(FATAL_ERROR "Could not download Faithful 32x: ${FAITHFUL_DOWNLOAD_MESSAGE}")
            endif()
        endif()

        set(FAITHFUL_BLOCK_TEXTURES
            bedrock calcite cinnabar coarse_dirt copper_ore deepslate deepslate_iron_ore deepslate_top
            dirt granite grass_block_side grass_block_top gravel ice lava_still light_gray_terracotta mud
            mycelium_side mycelium_top orange_terracotta packed_ice podzol_side podzol_top powder_snow
            raw_copper_block raw_iron_block red_sand red_sandstone red_sandstone_bottom red_sandstone_top
            red_terracotta sand sandstone sandstone_bottom sandstone_top snow stone sulfur terracotta tuff
            water_still white_terracotta yellow_terracotta brown_terracotta
        )
        set(FAITHFUL_TEXTURE_PATTERNS)
        foreach(TEXTURE IN LISTS FAITHFUL_BLOCK_TEXTURES)
            list(APPEND FAITHFUL_TEXTURE_PATTERNS "assets/minecraft/textures/block/${TEXTURE}.png")
        endforeach()
        file(ARCHIVE_EXTRACT
            INPUT "${FAITHFUL_ARCHIVE}"
            DESTINATION "${FAITHFUL_EXTRACT_DIR}"
            PATTERNS ${FAITHFUL_TEXTURE_PATTERNS}
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
    add_executable(terrain_tests tests/terrain_tests.cpp)
    target_compile_features(terrain_tests PRIVATE cxx_std_20)
    target_link_libraries(terrain_tests PRIVATE mcworld::mcworld)
    target_include_directories(terrain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME terrain_tests COMMAND terrain_tests)
    add_executable(generation_tests tests/generation_tests.cpp)
    target_compile_features(generation_tests PRIVATE cxx_std_20)
    target_link_libraries(generation_tests PRIVATE mcworld::mcworld)
    target_include_directories(generation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME generation_tests COMMAND generation_tests)
    if(MCWORLD_BUILD_VIEWER)
        add_executable(viewer_mesh_tests tests/viewer_mesh_tests.cpp src/voxel_renderer.cpp)
        target_compile_features(viewer_mesh_tests PRIVATE cxx_std_20)
        target_include_directories(viewer_mesh_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(viewer_mesh_tests PRIVATE mcworld::mcworld raylib)
        add_test(NAME viewer_mesh_tests COMMAND viewer_mesh_tests)
        add_test(NAME viewer_headless COMMAND terrain_viewer --seed 12345 --chunk-x -1 --chunk-z 2 --chunks 1 --terrain-only --headless)
        set_tests_properties(viewer_headless PROPERTIES
            PASS_REGULAR_EXPRESSION "mesh_triangles=[1-9][0-9]*"
        )
        add_test(NAME viewer_voxel_headless COMMAND terrain_viewer --seed 12345 --chunk-x -1 --chunk-z 2 --chunks 2 --voxel --terrain-only --headless)
        set_tests_properties(viewer_voxel_headless PROPERTIES
            PASS_REGULAR_EXPRESSION "solid_blocks=[1-9][0-9]* water_blocks=[0-9]+ lava_blocks=[0-9]+ voxel_faces=[1-9][0-9]*"
        )
        add_test(NAME viewer_invalid_chunk COMMAND terrain_viewer --chunk-x 2147483647 --headless)
        set_tests_properties(viewer_invalid_chunk PROPERTIES WILL_FAIL TRUE)
        add_test(NAME viewer_invalid_area COMMAND terrain_viewer --chunks 0 --headless)
        set_tests_properties(viewer_invalid_area PROPERTIES WILL_FAIL TRUE)
    endif()
endif()
