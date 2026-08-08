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

# Unit test for the rapid double double-click maximize/restore fix: drives the
# real input::mouse click-counter and asserts break_click_chain() lets a second
# same-cell double-click register instead of being folded into a multi-click run.
add_executable(tile_dblclick_chain_test "tile_dblclick_chain_test.cpp")
target_include_directories(tile_dblclick_chain_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                            ${LUA_INCLUDE_DIR})
target_link_libraries(tile_dblclick_chain_test PRIVATE Lua::Lua)
add_test(NAME tile_dblclick_chain_test COMMAND tile_dblclick_chain_test)

# Unit test for the Parvion transfer-rate meter (resume-aware instantaneous speed/average).
add_executable(parvion_rate_meter_test "parvion_rate_meter_test.cpp")
target_include_directories(parvion_rate_meter_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME parvion_rate_meter_test COMMAND parvion_rate_meter_test)

# Unit test for the Parvion "Pin to Top" reorder (no-op must not wipe row contents).
add_executable(parvion_pin_top_test "parvion_pin_top_test.cpp")
target_include_directories(parvion_pin_top_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME parvion_pin_top_test COMMAND parvion_pin_top_test)

# Unit test for Parvion table horizontal clipping.
add_executable(parvion_table_clip_test "parvion_table_clip_test.cpp")
target_include_directories(parvion_table_clip_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                           ${LUA_INCLUDE_DIR})
target_link_libraries(parvion_table_clip_test PRIVATE Lua::Lua)
add_test(NAME parvion_table_clip_test COMMAND parvion_table_clip_test)

# Unit test for the shared Parvion button renderer and palette contract.
add_executable(parvion_button_test "parvion_button_test.cpp")
target_include_directories(parvion_button_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                        ${LUA_INCLUDE_DIR})
target_link_libraries(parvion_button_test PRIVATE Lua::Lua)
add_test(NAME parvion_button_test COMMAND parvion_button_test)

# Unit test for the shared Parvion fixed/resizable grid layout.
add_executable(parvion_grid_test "parvion_grid_test.cpp")
target_include_directories(parvion_grid_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                      ${LUA_INCLUDE_DIR})
target_link_libraries(parvion_grid_test PRIVATE Lua::Lua)
add_test(NAME parvion_grid_test COMMAND parvion_grid_test)

# Unit test for the shared Parvion flex layout.
add_executable(parvion_flex_test "parvion_flex_test.cpp")
target_include_directories(parvion_flex_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                      ${LUA_INCLUDE_DIR})
target_link_libraries(parvion_flex_test PRIVATE Lua::Lua)
add_test(NAME parvion_flex_test COMMAND parvion_flex_test)

# Unit test for the shared Parvion progress-bar renderer and table-cell embedding contract.
add_executable(parvion_progressbar_test "parvion_progressbar_test.cpp")
target_include_directories(parvion_progressbar_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                             ${LUA_INCLUDE_DIR})
target_link_libraries(parvion_progressbar_test PRIVATE Lua::Lua)
add_test(NAME parvion_progressbar_test COMMAND parvion_progressbar_test)

# Unit test for the Parvion message logger (FileZilla-style detailed queueing).
add_executable(parvion_logging_test "parvion_logging_test.cpp")
target_include_directories(parvion_logging_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME parvion_logging_test COMMAND parvion_logging_test)

# Unit test for the Parvion textbox selection modes (quadruple-click select-all and friends).
add_executable(parvion_textbox_selection_test "parvion_textbox_selection_test.cpp")
target_include_directories(parvion_textbox_selection_test PRIVATE "${CMAKE_SOURCE_DIR}/src"
                                                                  ${LUA_INCLUDE_DIR})
target_link_libraries(parvion_textbox_selection_test PRIVATE Lua::Lua)
add_test(NAME parvion_textbox_selection_test COMMAND parvion_textbox_selection_test)

# Unit test for saved-site Host syntax and its DNS/IPv4/IPv6 boundaries.
add_executable(parvion_site_host_test "parvion_site_host_test.cpp")
target_include_directories(parvion_site_host_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME parvion_site_host_test COMMAND parvion_site_host_test)

# Python TUI regression tests for close confirmation dialog.
# Requires vtm-desk and vtm-tile binaries to be built first.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME confirm_close_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_confirm_close.py")
    set_tests_properties(confirm_close_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>;VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;desk;terminal")

    add_test(NAME tile_workspace_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_workspace.py")
    set_tests_properties(tile_workspace_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;workspace")

    add_test(NAME terminal_find_bar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal_find_bar.py")
    set_tests_properties(terminal_find_bar_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>"
        TIMEOUT 900
        LABELS "tui;terminal;find")

    add_test(NAME tile_command_bar_terminal_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_command_bar_terminal.py")
    set_tests_properties(tile_command_bar_terminal_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
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
        TIMEOUT 900
        LABELS "tui;tile;shutdown")

    # Regression test: pane focus must fire on LeftDown (press), not LeftClick
    # (release).  Also verifies the × close button still fires on LeftClick.
    add_test(NAME tile_leftdown_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_leftdown_focus.py")
    set_tests_properties(tile_leftdown_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;focus")

    # Regression test: ctrl+LeftDown toggles pane in/out of the focus group.
    add_test(NAME tile_ctrl_press_group_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_press_group.py")
    set_tests_properties(tile_ctrl_press_group_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;focus;group")

    # Regression test: ctrl+LeftDown on the tile gate menubar defocuses the whole
    # gate; a second ctrl+LeftDown refocuses it and restores prior pane state.
    add_test(NAME tile_ctrl_press_menubar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_press_menubar.py")
    set_tests_properties(tile_ctrl_press_menubar_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;focus;gate")

    add_test(NAME tile_capture_startup_bytes_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_capture_startup_bytes.py")
    set_tests_properties(tile_capture_startup_bytes_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;startup")

    add_test(NAME terminal_ctrllist_close_button_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal_ctrllist_close_button.py")
    set_tests_properties(terminal_ctrllist_close_button_test PROPERTIES
        ENVIRONMENT "VTM_DESK_BINARY=$<TARGET_FILE:vtm-desk>"
        TIMEOUT 900
        LABELS "tui;desk;terminal;ctrllist")

    add_test(NAME dropdown_menu_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_dropdown_menu.py")
    set_tests_properties(dropdown_menu_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;menu")

    # Regression test for the menu item type="radiomenu" feature: renders a
    # group of options as radio-menu rows (◉/○) and tracks the live selection
    # published via vtm.item.Check(idx), including when nested in a dropdown.
    add_test(NAME radiomenu_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_radiomenu.py")
    set_tests_properties(radiomenu_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;menu")

    add_test(NAME tile_select_app_picker_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_select_app_picker.py")
    set_tests_properties(tile_select_app_picker_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;pickapp")

    add_test(NAME terminal_restart_cwd_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_terminal_restart_cwd.py")
    set_tests_properties(terminal_restart_cwd_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;terminal;cwd")

    add_test(NAME tile_closepane_nvim_orphan_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_closepane_nvim_orphan.py")
    set_tests_properties(tile_closepane_nvim_orphan_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;close")

    add_test(NAME tile_ctrl_click_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_ctrl_click_focus.py")
    set_tests_properties(tile_ctrl_click_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;focus")

    add_test(NAME tile_cwd_inherit_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_cwd_inherit.py")
    set_tests_properties(tile_cwd_inherit_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;cwd")

    add_test(NAME tile_input_bar_modes_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_input_bar_modes.py")
    set_tests_properties(tile_input_bar_modes_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;inputbar")

    add_test(NAME tile_pane_close_focus_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_pane_close_focus.py")
    set_tests_properties(tile_pane_close_focus_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;focus")

    add_test(NAME tile_pickapp_default_action_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_pickapp_default_action.py")
    set_tests_properties(tile_pickapp_default_action_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;pickapp")

    add_test(NAME tile_rerun_application_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_rerun_application.py")
    set_tests_properties(tile_rerun_application_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;rerun")

    add_test(NAME tile_shell_input_display_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_shell_input_display.py")
    set_tests_properties(tile_shell_input_display_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;shell")

    add_test(NAME tile_zoom_close_button_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_zoom_close_button.py")
    set_tests_properties(tile_zoom_close_button_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;zoom")

    # Regression test for the Parvion transfer-queue tables: Reason-column
    # resize handle, selection highlight ending at the last column, and the
    # unified right-click context menus (selected actions plus Select All).
    add_test(NAME parvion_queue_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_queue.py")
    set_tests_properties(parvion_queue_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;queue")

    # Regression test for the file-preview pane operations (blank-area selection rules, right-click
    # context menus, and the local New Document / Folder / Delete / Rename filesystem operations).
    add_test(NAME parvion_panes_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_panes.py")
    set_tests_properties(parvion_panes_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;panes")

    # Folder (recursive) Download/Upload/Delete in the file-preview pane. The recursive Download case
    # connects to the public read-only test.rebex.net SFTP server and skips itself when offline, so a
    # longer timeout covers the real network transfers.
    add_test(NAME parvion_folder_ops_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_folder_ops.py")
    set_tests_properties(parvion_folder_ops_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;panes;folder;network")

    # A resumed transfer with more chunks than the six-at-a-time SSH handshake burst must retain
    # every part's checkpoint when the initially parked workers are activated. Uses a throttled
    # loopback SFTP server and covers both 10-channel downloads and uploads.
    add_test(NAME parvion_parallel_resume_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_parallel_resume.py")
    set_tests_properties(parvion_parallel_resume_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;transfer;resume")

    # Remote (SFTP) directory navigation timing: the destination path is revealed only with its
    # listing (no premature switch), and a second double-click issued while the first cd/ls is still
    # in flight does not over-descend (enter <dir>/<dir>). Connects to the public read-only
    # test.rebex.net and skips itself when offline; uses the PARVION_DEBUG_LS_DELAY_MS test seam to make
    # the in-flight race deterministic.
    add_test(NAME parvion_nav_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_nav.py")
    set_tests_properties(parvion_nav_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;panes;network")

    # The editable address bar in the pane title strip: click-to-edit (block caret), Enter
    # navigates (absolute or relative to the current dir), Esc reverts, bad paths error.
    add_test(NAME parvion_address_bar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_address_bar.py")
    set_tests_properties(parvion_address_bar_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;panes")

    # The Quick Connect bar's input fields: a left press (not the release) focuses and
    # activates the field under the cursor, and a left press-drag scrubs the caret.
    add_test(NAME parvion_connectbar_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_connectbar.py")
    set_tests_properties(parvion_connectbar_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;connectbar")

    # Shared main-grid integration: hidden connect-bar boundary plus draggable/resettable
    # Local|Remote and panes|tabs handles.
    add_test(NAME parvion_grid_layout_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_grid.py")
    set_tests_properties(parvion_grid_layout_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;parvion;layout")

    # Regression test: a rapid second double-click on a pane's menu bar (same
    # cell, within the double-click timeout) must restore the pane instead of
    # leaving it stuck maximized (the click run was misread as a triple/quad).
    add_test(NAME tile_dblclick_maximize_restore_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_tile_dblclick_maximize_restore.py")
    set_tests_properties(tile_dblclick_maximize_restore_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>"
        TIMEOUT 900
        LABELS "tui;tile;maximize")

    # The Parvion Edit -> Settings dialog (SFTP subset of FileZilla's Connection
    # pages): the top-tabbed modal persists to <XDG_CONFIG_HOME>/parvion/settings,
    # the SFTP tab's compression/threshold/key-table widgets, the pvputtygen
    # public-key parse and encrypted-key -> .ppk conversion flow (save picker,
    # overwrite confirm, wrong-passphrase retry), and Esc handling. Each case runs
    # against an isolated XDG_CONFIG_HOME; key cases self-skip without ssh-keygen
    # or /root/.ssh/id_rsa.
    add_test(NAME parvion_settings_test
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_parvion_settings.py")
    set_tests_properties(parvion_settings_test PROPERTIES
        ENVIRONMENT "VTM_TILE_BINARY=$<TARGET_FILE:vtm-tile>;PUTTYRANDOMSEED=${CMAKE_CURRENT_BINARY_DIR}/parvion-settings-randomseed"
        TIMEOUT 900
        LABELS "tui;tile;parvion;settings")
endif()
