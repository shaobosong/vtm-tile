# Build Windows x64 with MSVC on Linux

This repository can be built for `Windows x64` on Linux with:

- Debian host packages installed on demand
- `msvc-wine`: cloned automatically into the build directory
- `vcpkg`: cloned automatically into the build directory, with the manifest baseline commit fetched on demand

## One-time prerequisites

The build entrypoint is:

- `./build-windows-x64-debian.sh`

The script performs these steps:

1. Clone `vcpkg` if it does not exist yet.
2. Clone `msvc-wine` if it does not exist yet.
3. Install the Debian build prerequisites if they are missing.
4. Bootstrap and install the MSVC toolchain if it is missing.
5. Bootstrap `vcpkg` if needed.
6. Generate build-local `vcpkg` triplet and Meson cross files that point at the MSVC toolchain inside the current build directory.
7. Run `vcpkg install` for the manifest in this repository.
8. Configure CMake if the build directory is new or `FORCE_CONFIGURE=1`.
9. Build the `vtm.exe` target.

## Build

```bash
cd /data/freedom/vtm
./build-windows-x64-debian.sh
```

Default output directory:

- `<current-working-directory>/build-msvc-x64-release`

Default vcpkg install root:

- `<current-working-directory>/build-msvc-x64-release/vcpkg_installed`

Default vcpkg root:

- `<current-working-directory>/build-msvc-x64-release/vcpkg`

Default msvc-wine source root:

- `<current-working-directory>/build-msvc-x64-release/msvc-wine-src`

Default MSVC install root:

- `<current-working-directory>/build-msvc-x64-release/msvc-wine/msvc`

Default Wine prefix:

- `<current-working-directory>/build-msvc-x64-release/msvc-wine/wineprefix`

Default msvc-wine download cache:

- `<current-working-directory>/build-msvc-x64-release/msvc-wine/cache`

Generated triplet files:

- `<current-working-directory>/build-msvc-x64-release/triplets/x64-windows-static-msvc-wine.cmake`
- `<current-working-directory>/build-msvc-x64-release/triplets/meson-windows-x64-wine.ini`

Main artifacts:

- `build-msvc-x64-release/vtm.exe`
- `build-msvc-x64-release/vtm.pdb`

## Reconfigure

Force a fresh CMake configure:

```bash
cd /data/freedom/vtm
FORCE_CONFIGURE=1 ./build-windows-x64-debian.sh
```

Use a different build directory:

```bash
cd /data/freedom/vtm
BUILD_DIR=/data/freedom/vtm/build-custom ./build-windows-x64-debian.sh
```

Skip the explicit `vcpkg install` step:

```bash
cd /data/freedom/vtm
SKIP_VCPKG_INSTALL=1 ./build-windows-x64-debian.sh
```

## Environment used by the script

The script exports:

- `XDG_RUNTIME_DIR=/tmp/wine-runtime`
- `WINEPREFIX=<build-dir>/msvc-wine/wineprefix`
- `PATH=<build-dir>/msvc-wine/msvc/bin/x64:$PATH`
- `VS170COMNTOOLS=<build-dir>/msvc-wine/msvc/Common7/Tools/`
- `VCPKG_VISUAL_STUDIO_PATH=<build-dir>/msvc-wine/msvc`
- `WIN32_RESOURCES=.resources/images/vtm.rc`

You can override these through environment variables before invoking the script.
