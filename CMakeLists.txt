cmake_minimum_required(VERSION 3.24)

project(mc_worldgen VERSION 0.1.0 LANGUAGES CXX)

option(MCWORLD_BUILD_TESTS "Build unit tests" ON)
set(MCWORLD_GAME_JAR "" CACHE FILEPATH "Local Minecraft 26.3 game JAR to import structure resources from")

add_library(mcworld
    src/noise.cpp
    src/spline.cpp
    src/worldgen.cpp
    src/biome.cpp
    src/biome_environment.cpp
    src/terrain.cpp
    src/aquifer.cpp
    src/materials.cpp
    src/carvers.cpp
    src/generation.cpp
    src/structures.cpp
    src/structure_templates.cpp
    src/template_catalog.cpp
    src/decoration.cpp
    src/feature_placement.cpp
    src/features.cpp
    src/vegetation.cpp
    src/plant_features.cpp
    src/schematic.cpp
)
add_library(mcworld::mcworld ALIAS mcworld)

find_package(Threads REQUIRED)

target_compile_features(mcworld PUBLIC cxx_std_20)
target_include_directories(mcworld PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(mcworld PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mcworld PUBLIC Threads::Threads)

if(MCWORLD_GAME_JAR)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MCWORLD_TEMPLATE_CATALOG "${CMAKE_CURRENT_BINARY_DIR}/structures.mcwc")
    add_custom_command(OUTPUT "${MCWORLD_TEMPLATE_CATALOG}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/import_worldgen.py"
                "${MCWORLD_GAME_JAR}" "${MCWORLD_TEMPLATE_CATALOG}"
        DEPENDS "${MCWORLD_GAME_JAR}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/import_worldgen.py"
        VERBATIM)
    add_custom_target(mcworld_template_assets ALL DEPENDS "${MCWORLD_TEMPLATE_CATALOG}")
endif()

if(MSVC)
    target_compile_options(mcworld PRIVATE /W4 /permissive-)
else()
    target_compile_options(mcworld PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(terrain_gen src/main.cpp)
target_compile_features(terrain_gen PRIVATE cxx_std_20)
target_link_libraries(terrain_gen PRIVATE mcworld::mcworld)
if(MCWORLD_GAME_JAR)
    add_dependencies(terrain_gen mcworld_template_assets)
    target_compile_definitions(terrain_gen PRIVATE MCWORLD_DEFAULT_TEMPLATE_CATALOG="${MCWORLD_TEMPLATE_CATALOG}")
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
    add_executable(schematic_tests tests/schematic_tests.cpp)
    target_compile_features(schematic_tests PRIVATE cxx_std_20)
    target_link_libraries(schematic_tests PRIVATE mcworld::mcworld)
    add_test(NAME schematic_tests COMMAND schematic_tests)
    if(MCWORLD_GAME_JAR)
        add_dependencies(generation_tests mcworld_template_assets)
        add_test(NAME imported_structure_tests COMMAND generation_tests --catalog "${MCWORLD_TEMPLATE_CATALOG}")
    endif()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME template_importer_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/template_importer_tests.py" "$<TARGET_FILE:generation_tests>")
        add_test(NAME generation_cli_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generation_cli_tests.py" "$<TARGET_FILE:terrain_gen>")
    endif()
endif()
