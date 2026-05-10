add_executable(tile_session_shutdown_test "tile_session_shutdown_test.cpp")
target_include_directories(tile_session_shutdown_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                              ${LUA_INCLUDE_DIR})
target_link_libraries(tile_session_shutdown_test PRIVATE Lua::Lua)
add_test(NAME tile_session_shutdown_test COMMAND tile_session_shutdown_test)

add_executable(tile_close_slot_test "tile_close_slot_test.cpp")
target_include_directories(tile_close_slot_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                        ${LUA_INCLUDE_DIR})
target_link_libraries(tile_close_slot_test PRIVATE Lua::Lua)
add_test(NAME tile_close_slot_test COMMAND tile_close_slot_test)

add_executable(command_bar_fuzzy_test "command_bar_fuzzy_test.cpp")
target_include_directories(command_bar_fuzzy_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                          ${LUA_INCLUDE_DIR})
target_link_libraries(command_bar_fuzzy_test PRIVATE Lua::Lua)
add_test(NAME command_bar_fuzzy_test COMMAND command_bar_fuzzy_test)

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

    add_test(NAME workspace_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_workspace.py")
    set_tests_properties(workspace_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;workspace")

    add_test(NAME find_bar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_find_bar.py")
    set_tests_properties(find_bar_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>"
        TIMEOUT 300
        LABELS "tui;find")

    add_test(NAME command_bar_terminal_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_command_bar_terminal.py")
    set_tests_properties(command_bar_terminal_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;commandbar")

    # Regression test for the vtm-tile shutdown orphan-process bug:
    # an interactive bash with a half-buffered escape sequence in
    # readline (e.g. user pressed Esc) ignores the SIGHUP that vtm-tile
    # sends on shutdown, leaving the shell reparented to PID 1. This
    # test exercises the full cmdbar shutdown path and asserts that no
    # tagged shell child survives.
    add_test(NAME tile_shutdown_orphans_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_shutdown_orphans.py")
    set_tests_properties(tile_shutdown_orphans_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;shutdown;regression")

    # Regression test: pane focus must fire on LeftDown (press), not LeftClick
    # (release).  Also verifies the × close button still fires on LeftClick.
    add_test(NAME tile_leftdown_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_leftdown_focus.py")
    set_tests_properties(tile_leftdown_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus;regression")

    # Regression test: ctrl+LeftDown toggles pane in/out of the focus group.
    add_test(NAME tile_ctrl_press_group_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_press_group.py")
    set_tests_properties(tile_ctrl_press_group_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus;group;regression")

    # Regression test: ctrl+LeftDown on the tile gate menubar defocuses the whole
    # gate; a second ctrl+LeftDown refocuses it and restores prior pane state.
    add_test(NAME tile_ctrl_press_menubar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_press_menubar.py")
    set_tests_properties(tile_ctrl_press_menubar_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus;gate;regression")
endif()
