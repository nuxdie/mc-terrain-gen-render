cmake_minimum_required(VERSION 3.24)

project(mc_terrain_gen_render VERSION 0.1.0 LANGUAGES CXX)

option(MCWORLD_BUILD_VIEWER "Build the interactive Raylib viewer" ON)
option(MCWORLD_BUILD_TESTS "Build unit tests" ON)

add_library(mcworld
    src/noise.cpp
    src/spline.cpp
    src/worldgen.cpp
    src/isosurface.cpp
    src/biome.cpp
    src/terrain.cpp
    src/aquifer.cpp
    src/materials.cpp
    src/carvers.cpp
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
    if(MCWORLD_BUILD_VIEWER)
        add_test(NAME viewer_headless COMMAND terrain_viewer --seed 12345 --chunk-x -1 --chunk-z 2 --chunks 1 --headless)
        add_test(NAME viewer_voxel_headless COMMAND terrain_viewer --seed 12345 --chunk-x -1 --chunk-z 2 --chunks 2 --voxel --headless)
        set_tests_properties(viewer_voxel_headless PROPERTIES
            PASS_REGULAR_EXPRESSION "solid_blocks=[1-9][0-9]* water_blocks=[0-9]+ lava_blocks=[0-9]+ voxel_faces=[1-9][0-9]*"
        )
        add_test(NAME viewer_invalid_chunk COMMAND terrain_viewer --chunk-x 2147483647 --headless)
        set_tests_properties(viewer_invalid_chunk PROPERTIES WILL_FAIL TRUE)
        add_test(NAME viewer_invalid_area COMMAND terrain_viewer --chunks 0 --headless)
        set_tests_properties(viewer_invalid_area PROPERTIES WILL_FAIL TRUE)
    endif()
endif()
