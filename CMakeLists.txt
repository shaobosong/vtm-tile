cmake_minimum_required(VERSION 3.22)
include(FetchContent)

project("vtm" CXX C) # C is needed for the vendored parvionsftp (FileZilla/PuTTY) backend.
# project("term")
# project("calc")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the build type to Release if none is specified.
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

if(NOT WIN32 AND "$ENV{LINUX_CXX_FLAGS}" STREQUAL "") # If CMakeSettings.json is not used (MS Visual Studio).
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -pthread")
endif()

add_executable(vtm-desk "src/vtm-desk.cpp" $ENV{WIN32_RESOURCES})
add_executable(vtm-tile "src/vtm-tile.cpp" $ENV{WIN32_RESOURCES})
# add_executable(term "src/netxs/apps/term.cpp")
# add_executable(calc "src/netxs/apps/calc.cpp")

find_package(Lua 5.4  REQUIRED)

add_library(Lua::Lua INTERFACE IMPORTED)
set_target_properties(Lua::Lua PROPERTIES INTERFACE_LINK_LIBRARIES "${LUA_LIBRARIES}")

target_include_directories(vtm-desk PRIVATE ${LUA_INCLUDE_DIR})
target_include_directories(vtm-tile PRIVATE ${LUA_INCLUDE_DIR})

target_link_libraries(vtm-desk PRIVATE Lua::Lua)
target_link_libraries(vtm-tile PRIVATE Lua::Lua)
# target_link_libraries(term PRIVATE Lua::Lua)
# target_link_libraries(calc PRIVATE Lua::Lua)

# ──────────────────────────────────────────────────────────────────────────────
# parvionsftp backend (FileZilla/PuTTY-derived SFTP engine).
#
# Compiled straight into vtm-tile as a static lib so that `vtm-tile -r parvionsftp`
# IS the SFTP backend (multi-call binary). The backend C sources carry the
# parvion rebranding (names/strings only) but are otherwise unmodified; the
# single platform entry point's `main` is renamed to `parvionsftp_main` via a
# per-file compile define, and vtm-tile's main() dispatches to it. Source list
# and per-platform flags mirror external/.../putty/Makefile.am.
# ──────────────────────────────────────────────────────────────────────────────
set(PUTTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/filezilla/FileZilla3/src/putty")

set(PARVIONSFTP_COMMON_SRC
    conf.c console.c ecc.c import.c marshal.c memory.c misc.c mpint.c rfc6979.c
    sshaes.c sshargon2.c sshauxcrypt.c sshbcrypt.c sshblake2.c sshblowf.c sshdes.c
    sshdss.c sshecc.c sshhmac.c sshmd5.c sshprng.c sshpubk.c sshrand.c sshrsa.c
    sshsha.c sshsh256.c sshsh512.c sshsha3.c stripctrl.c tree234.c utils.c
    fzprintf.c wcwidth.c)

set(PARVIONSFTP_CORE_SRC
    be_misc.c be_ssh.c callback.c clicons.c cmdline.c cproxy.c errsock.c fzsftp.c
    logging.c mainchan.c noshare.c nullplug.c portfwd.c psftp.c proxy.c pproxy.c
    pinger.c settings.c sftp.c sftpcommon.c ssh.c ssh2bpp.c ssh2censor.c
    ssh2connection.c ssh2connection-client.c ssh2kex-client.c ssh2transhk.c
    ssh2transport.c ssh2userauth.c ssharcf.c sshccp.c sshcommon.c sshcrc.c
    sshcrcda.c sshdh.c sshmac.c sshshare.c sshutils.c sshverstring.c sshzlib.c
    timing.c version.c wildcard.c x11fwd.c)

if(WIN32)
    set(PARVIONSFTP_PLATFORM_SRC
        windows/wincons.c windows/winmisc.c windows/winmiscs.c windows/winnohlp.c
        windows/winnoise.c windows/winnojmp.c windows/winstore.c
        windows/wincapi.c windows/wincliloop.c windows/windefs.c windows/winhandl.c
        windows/winhsock.c windows/winnet.c windows/winnpc.c windows/winnps.c
        windows/winpgntc.c windows/winsecur.c windows/winselcli.c windows/winsftp.c
        windows/wintime.c)
    set(PARVIONSFTP_MAIN_SRC windows/winsftp.c) # owns main() -> renamed to parvionsftp_main
else()
    set(PARVIONSFTP_PLATFORM_SRC
        unix/uxcons.c unix/uxmisc.c unix/uxnoise.c unix/uxpoll.c unix/uxstore.c
        unix/uxutils.c
        time.c unix/uxagentc.c unix/uxcliloop.c unix/uxnet.c unix/uxpeer.c
        unix/uxsel.c unix/uxsftp.c)
    set(PARVIONSFTP_MAIN_SRC unix/uxsftp.c) # owns main() -> renamed to parvionsftp_main
endif()

set(PARVIONSFTP_ALL_SRC ${PARVIONSFTP_COMMON_SRC} ${PARVIONSFTP_CORE_SRC} ${PARVIONSFTP_PLATFORM_SRC})
list(TRANSFORM PARVIONSFTP_ALL_SRC PREPEND "${PUTTY_DIR}/")

# ── Vendored libnettle AES-GCM subset ────────────────────────────────────────
# sshaes.c uses nettle's AES-GCM (nettle_gcm_aes128/256_*). Instead of depending on
# system/vcpkg nettle — whose package drags in gmp, which does not cross-build under
# MSVC — compile just the AES-GCM source files from the vendored nettle tree. These
# need no gmp and no generated headers: nettle-types.h pulls <stdint.h> directly, and
# config.h includes are guarded by #if HAVE_CONFIG_H (left undefined → safe C/little-
# endian defaults). GCM_TABLE_BITS is pinned to nettle's standard 8-bit table.
set(NETTLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/filezilla/nettle")
add_library(nettle_aes STATIC
    "${NETTLE_DIR}/gcm.c"
    "${NETTLE_DIR}/gcm-aes128.c"
    "${NETTLE_DIR}/gcm-aes256.c"
    "${NETTLE_DIR}/ctr16.c"
    "${NETTLE_DIR}/aes-encrypt.c"
    "${NETTLE_DIR}/aes-encrypt-internal.c"
    "${NETTLE_DIR}/aes-encrypt-table.c"
    "${NETTLE_DIR}/aes128-set-encrypt-key.c"
    "${NETTLE_DIR}/aes256-set-encrypt-key.c"
    "${NETTLE_DIR}/aes-set-key-internal.c"
    "${NETTLE_DIR}/memxor.c"
    "${NETTLE_DIR}/memxor3.c")
target_include_directories(nettle_aes PRIVATE "${NETTLE_DIR}")
target_compile_definitions(nettle_aes PRIVATE GCM_TABLE_BITS=8)
if(MSVC)
    target_compile_options(nettle_aes PRIVATE /w)
else()
    target_compile_options(nettle_aes PRIVATE -w)
endif()

add_library(parvionsftp_be STATIC ${PARVIONSFTP_ALL_SRC})
# PUTTY_DIR for the backend; external/filezilla so sshaes.c's <nettle/aes.h> resolves
# to the vendored nettle/ headers (matching the nettle_aes sources we compile).
target_include_directories(parvionsftp_be PRIVATE
    "${PUTTY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/filezilla")
target_compile_definitions(parvionsftp_be PRIVATE NO_GSSAPI)
target_link_libraries(parvionsftp_be PRIVATE nettle_aes)

# Rename the single platform entry point's main() -> parvionsftp_main (no source edit).
set_source_files_properties("${PUTTY_DIR}/${PARVIONSFTP_MAIN_SRC}"
    TARGET_DIRECTORY parvionsftp_be
    PROPERTIES COMPILE_DEFINITIONS "main=parvionsftp_main")

if(WIN32)
    target_compile_definitions(parvionsftp_be PRIVATE _WINDOWS SECURITY_WIN32 _ISOC99_SOURCE)
    if(MSVC)
        target_compile_options(parvionsftp_be PRIVATE /w)
    else()
        target_compile_options(parvionsftp_be PRIVATE -w)
    endif()
    target_link_libraries(parvionsftp_be PRIVATE ws2_32 ole32)
else()
    # Linux: the PuTTY backend uses its generated autotools config.h.
    target_include_directories(parvionsftp_be PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/filezilla/FileZilla3/config")
    target_compile_definitions(parvionsftp_be PRIVATE HAVE_CONFIG_H _FILE_OFFSET_BITS=64)
    target_compile_options(parvionsftp_be PRIVATE -w)
endif()

target_link_libraries(vtm-tile PRIVATE parvionsftp_be)

# ── pvputtygen backend (private-key Comment/Data parsing; vtm-tile -r pvputtygen) ──
# FileZilla's ufxputtygen is just cmdgen.c over the shared PuTTY crypto/key code that
# parvionsftp_be already vendors and compiles. To avoid linking a second copy of that
# code (duplicate symbols in the one multi-call binary), pvputtygen_be holds ONLY
# cmdgen.c and links parvionsftp_be for everything else (crypto, key loaders, PuTTY
# globals, platform stubs). cmdgen.c's own main() is renamed to pvputtygen_main and
# dispatched from vtm-tile's main(). Listed BEFORE parvionsftp_be on vtm-tile's link
# line (via the dependency below) so cmdgen.o's references resolve into it.
add_library(pvputtygen_be STATIC "${PUTTY_DIR}/cmdgen.c")
target_include_directories(pvputtygen_be PRIVATE
    "${PUTTY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/filezilla")
target_compile_definitions(pvputtygen_be PRIVATE NO_GSSAPI)
set_source_files_properties("${PUTTY_DIR}/cmdgen.c"
    TARGET_DIRECTORY pvputtygen_be
    PROPERTIES COMPILE_DEFINITIONS "main=pvputtygen_main")
if(WIN32)
    target_compile_definitions(pvputtygen_be PRIVATE _WINDOWS SECURITY_WIN32 _ISOC99_SOURCE)
    if(MSVC)
        target_compile_options(pvputtygen_be PRIVATE /w)
    else()
        target_compile_options(pvputtygen_be PRIVATE -w)
    endif()
else()
    target_include_directories(pvputtygen_be PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/filezilla/FileZilla3/config")
    target_compile_definitions(pvputtygen_be PRIVATE HAVE_CONFIG_H _FILE_OFFSET_BITS=64)
    target_compile_options(pvputtygen_be PRIVATE -w)
endif()
target_link_libraries(pvputtygen_be PRIVATE parvionsftp_be nettle_aes)
target_link_libraries(vtm-tile PRIVATE pvputtygen_be)

option(VTM_BUILD_TESTS "Build vtm regression tests" OFF)
if(VTM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(NOT WIN32)
    install(TARGETS vtm-desk vtm-tile DESTINATION bin)
endif()
