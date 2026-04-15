cmake_minimum_required(VERSION 3.22)
include(FetchContent)

project("vtm" CXX)
# project("term")
# project("calc")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the build type to Release if none is specified.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

if(NOT WIN32 AND "$ENV{LINUX_CXX_FLAGS}" STREQUAL "") # If CMakeSettings.json is not used (MS Visual Studio).
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g3 -pthread")
endif()

add_executable(vtm-desk "src/vtm-desk.cpp" $ENV{WIN32_RESOURCES})
add_executable(vtm-tile "src/vtm-tile.cpp" $ENV{WIN32_RESOURCES})
# add_executable(term "src/netxs/apps/term.cpp")
# add_executable(calc "src/netxs/apps/calc.cpp")

find_package(Freetype REQUIRED)
find_package(harfbuzz REQUIRED)
find_package(Lua 5.4  REQUIRED)

add_library(Lua::Lua INTERFACE IMPORTED)
set_target_properties(Lua::Lua PROPERTIES INTERFACE_LINK_LIBRARIES "${LUA_LIBRARIES}")

target_include_directories(vtm-desk PRIVATE ${FREETYPE_INCLUDE_DIRS}
                                            ${HARFBUZZ_INCLUDE_DIRS}
                                            ${LUA_INCLUDE_DIR})
target_include_directories(vtm-tile PRIVATE ${FREETYPE_INCLUDE_DIRS}
                                            ${HARFBUZZ_INCLUDE_DIRS}
                                            ${LUA_INCLUDE_DIR})

target_link_libraries(vtm-desk PRIVATE Lua::Lua Freetype::Freetype harfbuzz::harfbuzz)
target_link_libraries(vtm-tile PRIVATE Lua::Lua Freetype::Freetype harfbuzz::harfbuzz)
# target_link_libraries(term PRIVATE Lua::Lua Freetype::Freetype harfbuzz::harfbuzz)
# target_link_libraries(calc PRIVATE Lua::Lua Freetype::Freetype harfbuzz::harfbuzz)

option(VTM_BUILD_TESTS "Build vtm regression tests" OFF)
if(VTM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(NOT WIN32)
    install(TARGETS vtm-desk vtm-tile DESTINATION bin)
endif()
