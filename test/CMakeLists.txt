add_executable(tile_session_shutdown_test "tile_session_shutdown_test.cpp")
target_include_directories(tile_session_shutdown_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                              ${FREETYPE_INCLUDE_DIRS}
                                                              ${HARFBUZZ_INCLUDE_DIRS}
                                                              ${LUA_INCLUDE_DIR})
target_link_libraries(tile_session_shutdown_test PRIVATE Lua::Lua Freetype::Freetype harfbuzz::harfbuzz)
add_test(NAME tile_session_shutdown_test COMMAND tile_session_shutdown_test)

add_executable(ansi_sgr_state_machine_test "ansi_sgr_state_machine_test.cpp")
target_include_directories(ansi_sgr_state_machine_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                               ${FREETYPE_INCLUDE_DIRS}
                                                               ${HARFBUZZ_INCLUDE_DIRS}
                                                               ${LUA_INCLUDE_DIR})
target_link_libraries(ansi_sgr_state_machine_test PRIVATE Lua::Lua Freetype::Freetype harfbuzz::harfbuzz)
add_test(NAME ansi_sgr_state_machine_test COMMAND ansi_sgr_state_machine_test)
