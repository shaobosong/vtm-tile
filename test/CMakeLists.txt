add_executable(tile_session_shutdown_test "tile_session_shutdown_test.cpp")
target_include_directories(tile_session_shutdown_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                              ${LUA_INCLUDE_DIR})
target_link_libraries(tile_session_shutdown_test PRIVATE Lua::Lua)
add_test(NAME tile_session_shutdown_test COMMAND tile_session_shutdown_test)

add_executable(ansi_sgr_state_machine_test "ansi_sgr_state_machine_test.cpp")
target_include_directories(ansi_sgr_state_machine_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                               ${LUA_INCLUDE_DIR})
target_link_libraries(ansi_sgr_state_machine_test PRIVATE Lua::Lua)
add_test(NAME ansi_sgr_state_machine_test COMMAND ansi_sgr_state_machine_test)

# Python TUI regression tests for close confirmation dialog.
# Requires vtm-desk and vtm-tile binaries to be built first.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME confirm_close_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_confirm_close.py")
    set_tests_properties(confirm_close_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>;VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;regression")
endif()
