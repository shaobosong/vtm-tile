# .\vcpkg\bootstrap-vcpkg.bat -disableMetrics

$env:WIN32_RESOURCES = ".resources/images/vtm.rc"

cmake -B build-x64 -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE="$PWD\vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded" -DCMAKE_EXE_LINKER_FLAGS_RELEASE="/DEBUG /OPT:REF /OPT:ICF" -DCMAKE_CXX_FLAGS_RELEASE="/MT /O2 /DNDEBUG /Zi /Zc:preprocessor /W4 /EHsc /bigobj /utf-8 /Zc:preprocessor" -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -S .

cmake --build build-x64 --config Release
