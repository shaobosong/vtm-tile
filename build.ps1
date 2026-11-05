[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-x64',
    [string]$VcpkgRoot,
    [ValidateRange(1, 65535)]
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Read-Utf8ProcessOutput {
    param([string]$FilePath, [string]$Arguments)

    # Decode native UTF-8 output independently of the terminal code page.
    $process = New-Object Diagnostics.Process
    $process.StartInfo.FileName = $FilePath
    $process.StartInfo.Arguments = $Arguments
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process.StartInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    try {
        $process.Start() | Out-Null
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $output = $stdout.GetAwaiter().GetResult()
        $errorOutput = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "$FilePath failed (exit $($process.ExitCode)): $errorOutput" }
        return $output
    }
    finally { $process.Dispose() }
}

function Select-VisualStudio {
    param($Instances, $Generators, [string]$CachedGenerator, [string]$CachedInstance)

    $candidates = @(foreach ($instance in ($Instances | Sort-Object { [version]$_.installationVersion } -Descending)) {
        $major = ([version]$instance.installationVersion).Major
        if ($major -lt 17) { continue }
        foreach ($generator in $Generators) {
            if ($generator.name -match "^Visual Studio $major [0-9]{4}$") {
                [pscustomobject]@{ Generator = $generator.name; Instance = $instance.installationPath }
            }
        }
    })

    if ($CachedGenerator) {
        $cachedLocation = if ($CachedInstance) { ($CachedInstance -split ',')[0].Replace('\', '/').TrimEnd('/') } else { '' }
        $candidates = @($candidates | Where-Object {
            $_.Generator -eq $CachedGenerator -and
            (!$cachedLocation -or $_.Instance.Replace('\', '/').TrimEnd('/') -eq $cachedLocation)
        })
        if (!$candidates.Count) {
            throw 'The cached generator or VS instance is unavailable or unsupported. Use a new -BuildDirectory; the existing cache has been preserved.'
        }
    }
    if (!$candidates.Count) {
        throw 'No compatible VS 2022 or newer C++ installation was found. Install the C++ build tools and a CMake version supporting that VS release.'
    }
    return $candidates[0]
}

Get-Command cmake -ErrorAction Stop | Out-Null
$capabilitiesJson = & cmake -E capabilities
if ($LASTEXITCODE -ne 0) { throw "CMake capability detection failed (exit $LASTEXITCODE)." }
$capabilities = ($capabilitiesJson -join "`n") | ConvertFrom-Json

$vswhereCommand = Get-Command vswhere.exe -ErrorAction SilentlyContinue
$vswhere = if ($vswhereCommand) { $vswhereCommand.Source } else {
    Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
}
if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'vswhere.exe was not found. Install Visual Studio Installer or put vswhere.exe on PATH.'
}
$instancesJson = Read-Utf8ProcessOutput $vswhere '-products * -version "[17.0,)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8'
$instances = @($instancesJson | ConvertFrom-Json)

if (![IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot $BuildDirectory }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$cachedGenerator = ''
$cachedInstance = ''
$cacheFile = Join-Path $BuildDirectory 'CMakeCache.txt'
if (Test-Path -LiteralPath $cacheFile) {
    foreach ($line in Get-Content -LiteralPath $cacheFile) {
        if ($line -match '^CMAKE_GENERATOR:[^=]+=(.*)$') { $cachedGenerator = $Matches[1] }
        if ($line -match '^CMAKE_GENERATOR_INSTANCE:[^=]+=(.*)$') { $cachedInstance = $Matches[1] }
    }
}
$selected = Select-VisualStudio $instances $capabilities.generators $cachedGenerator $cachedInstance

if (!$VcpkgRoot) {
    $roots = @((Join-Path $PSScriptRoot 'vcpkg'), $env:VCPKG_ROOT, (Join-Path $selected.Instance 'VC/vcpkg'))
    $VcpkgRoot = $roots | Where-Object {
        $_ -and (Test-Path -LiteralPath (Join-Path $_ 'scripts/buildsystems/vcpkg.cmake') -PathType Leaf)
    } | Select-Object -First 1
}
if (!$VcpkgRoot) { throw 'vcpkg was not found. Supply -VcpkgRoot, set VCPKG_ROOT, or install vcpkg in the repository.' }
if (![IO.Path]::IsPathRooted($VcpkgRoot)) { $VcpkgRoot = Join-Path $PSScriptRoot $VcpkgRoot }
$toolchain = Join-Path ([IO.Path]::GetFullPath($VcpkgRoot)) 'scripts/buildsystems/vcpkg.cmake'
if (!(Test-Path -LiteralPath $toolchain -PathType Leaf)) { throw "vcpkg toolchain not found: $toolchain" }

$configureArgs = @(
    '-S', $PSScriptRoot, '-B', $BuildDirectory,
    '-G', $selected.Generator, '-A', 'x64',
    "-DCMAKE_GENERATOR_INSTANCE=$($selected.Instance)",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    '-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO',
    '-DCMAKE_CXX_FLAGS_RELEASE=/MP /MT /O2 /DNDEBUG /Zi /Zc:preprocessor /W4 /EHsc /bigobj /utf-8',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_VERBOSE_MAKEFILE=ON'
)
$buildArgs = @('--build', $BuildDirectory, '--config', 'Release', '--parallel', "$Jobs")

Write-Host "Generator: $($selected.Generator)"
Write-Host "Visual Studio: $($selected.Instance)"
Write-Host "vcpkg toolchain: $toolchain"
Write-Host "Build directory: $BuildDirectory"
if ($DryRun) {
    foreach ($arguments in @(@{ Args = $configureArgs }, @{ Args = $buildArgs })) {
        Write-Host ('cmake ' + (($arguments.Args | ForEach-Object { '"' + $_ + '"' }) -join ' '))
    }
    return
}

$previousResources = $env:WIN32_RESOURCES
try {
    # vcpkg forwards this value through CMake code; avoid backslash escapes.
    $env:WIN32_RESOURCES = (Join-Path $PSScriptRoot '.resources/images/vtm.rc').Replace('\', '/')
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)." }
}
finally {
    $env:WIN32_RESOURCES = $previousResources
}
