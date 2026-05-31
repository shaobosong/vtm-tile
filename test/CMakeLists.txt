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
        LABELS "tui;tile;desk;terminal")

    add_test(NAME tile_workspace_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_workspace.py")
    set_tests_properties(tile_workspace_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;workspace")

    add_test(NAME terminal_find_bar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal_find_bar.py")
    set_tests_properties(terminal_find_bar_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>"
        TIMEOUT 300
        LABELS "tui;terminal;find")

    add_test(NAME tile_command_bar_terminal_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_command_bar_terminal.py")
    set_tests_properties(tile_command_bar_terminal_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;commandbar")

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
        LABELS "tui;tile;shutdown")

    # Regression test: pane focus must fire on LeftDown (press), not LeftClick
    # (release).  Also verifies the × close button still fires on LeftClick.
    add_test(NAME tile_leftdown_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_leftdown_focus.py")
    set_tests_properties(tile_leftdown_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus")

    # Regression test: ctrl+LeftDown toggles pane in/out of the focus group.
    add_test(NAME tile_ctrl_press_group_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_press_group.py")
    set_tests_properties(tile_ctrl_press_group_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus;group")

    # Regression test: ctrl+LeftDown on the tile gate menubar defocuses the whole
    # gate; a second ctrl+LeftDown refocuses it and restores prior pane state.
    add_test(NAME tile_ctrl_press_menubar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_press_menubar.py")
    set_tests_properties(tile_ctrl_press_menubar_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus;gate")

    add_test(NAME tile_capture_startup_bytes_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_capture_startup_bytes.py")
    set_tests_properties(tile_capture_startup_bytes_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;startup")

    add_test(NAME terminal_ctrllist_close_button_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal_ctrllist_close_button.py")
    set_tests_properties(terminal_ctrllist_close_button_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>"
        TIMEOUT 300
        LABELS "tui;desk;terminal;ctrllist")

    add_test(NAME dropdown_menu_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_dropdown_menu.py")
    set_tests_properties(dropdown_menu_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;menu")

    # Regression test for the menu item type="radiomenu" feature: renders a
    # group of options as radio-menu rows (◉/○) and tracks the live selection
    # published via vtm.item.Check(idx), including when nested in a dropdown.
    add_test(NAME radiomenu_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_radiomenu.py")
    set_tests_properties(radiomenu_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;menu")

    add_test(NAME tile_select_app_picker_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_select_app_picker.py")
    set_tests_properties(tile_select_app_picker_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;pickapp")

    add_test(NAME terminal_restart_cwd_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal_restart_cwd.py")
    set_tests_properties(terminal_restart_cwd_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;terminal;cwd")

    add_test(NAME tile_closepane_nvim_orphan_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_closepane_nvim_orphan.py")
    set_tests_properties(tile_closepane_nvim_orphan_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;close")

    add_test(NAME tile_ctrl_click_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_click_focus.py")
    set_tests_properties(tile_ctrl_click_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus")

    add_test(NAME tile_cwd_inherit_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_cwd_inherit.py")
    set_tests_properties(tile_cwd_inherit_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;cwd")

    add_test(NAME tile_input_bar_modes_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_input_bar_modes.py")
    set_tests_properties(tile_input_bar_modes_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;inputbar")

    add_test(NAME tile_pane_close_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_pane_close_focus.py")
    set_tests_properties(tile_pane_close_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;focus")

    add_test(NAME tile_pickapp_default_action_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_pickapp_default_action.py")
    set_tests_properties(tile_pickapp_default_action_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;pickapp")

    add_test(NAME tile_rerun_application_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_rerun_application.py")
    set_tests_properties(tile_rerun_application_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;rerun")

    add_test(NAME tile_shell_input_display_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_shell_input_display.py")
    set_tests_properties(tile_shell_input_display_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;shell")

    add_test(NAME tile_zoom_close_button_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_zoom_close_button.py")
    set_tests_properties(tile_zoom_close_button_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 300
        LABELS "tui;tile;zoom")
endif()
