# Run with PowerShell 5.1 or newer; no VS installation or build is required.
$ErrorActionPreference = 'Stop'
$buildScript = Join-Path $PSScriptRoot '../build.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($buildScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
# Load helpers without running configure or build commands.
foreach ($helper in 'Select-VisualStudio', 'Read-Utf8ProcessOutput') {
    $definition = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $helper
    }, $true)
    . ([scriptblock]::Create($definition.Extent.Text))
}

$instances = @(
    [pscustomobject]@{ installationVersion = '16.11.1.0'; installationPath = 'C:\VS2019' },
    [pscustomobject]@{ installationVersion = '17.14.1.0'; installationPath = 'C:\VS2022' },
    [pscustomobject]@{ installationVersion = '18.0.1.0'; installationPath = 'C:\VS2026' }
)
$generators = @('Visual Studio 16 2019', 'Visual Studio 17 2022', 'Visual Studio 18 2026') |
    ForEach-Object { [pscustomobject]@{ name = $_ } }
$cases = 0
function Assert-Equal($actual, $expected) {
    if ($actual -ne $expected) { throw "Expected '$expected', received '$actual'." }
    $script:cases++
}
function Assert-Throws([scriptblock]$action, [string]$message) {
    try { & $action }
    catch {
        if ($_.Exception.Message -notlike "*$message*") { throw }
        $script:cases++
        return
    }
    throw 'Expected selection to fail.'
}

Assert-Equal (Select-VisualStudio $instances $generators).Generator 'Visual Studio 18 2026'
Assert-Equal (Select-VisualStudio $instances @($generators[1])).Instance 'C:\VS2022'
Assert-Equal (Select-VisualStudio @($instances[1]) $generators).Generator 'Visual Studio 17 2022'
Assert-Equal (Select-VisualStudio $instances $generators 'Visual Studio 17 2022' 'C:\VS2022').Instance 'C:\VS2022'
Assert-Equal (Select-VisualStudio $instances $generators 'Visual Studio 17 2022' 'C:/VS2022/').Instance 'C:\VS2022'
Assert-Equal (Select-VisualStudio $instances $generators 'Visual Studio 17 2022' 'C:\VS2022\,version=17.14.1.0').Instance 'C:\VS2022'
Assert-Throws { Select-VisualStudio @() $generators } 'No compatible VS 2022'
Assert-Throws { Select-VisualStudio @($instances[0]) $generators } 'No compatible VS 2022'
Assert-Throws { Select-VisualStudio @($instances[2]) @($generators[1]) } 'No compatible VS 2022'
Assert-Throws { Select-VisualStudio $instances $generators 'Ninja' '' } 'new -BuildDirectory'
Assert-Throws { Select-VisualStudio $instances $generators 'Visual Studio 18 2026' 'C:\Removed' } 'new -BuildDirectory'

$newerInstances = $instances + [pscustomobject]@{ installationVersion = '19.0.0.0'; installationPath = 'C:\FutureVS' }
$newerGenerators = $generators + [pscustomobject]@{ name = 'Visual Studio 19 2030' }
Assert-Equal (Select-VisualStudio $newerInstances $newerGenerators).Instance 'C:\FutureVS'
$sideBySide = $instances + [pscustomobject]@{ installationVersion = '17.14.2.0'; installationPath = 'C:\OtherVS2022' }
Assert-Equal (Select-VisualStudio $sideBySide $generators 'Visual Studio 17 2022' 'C:\VS2022').Instance 'C:\VS2022'
Assert-Equal (Select-VisualStudio $sideBySide $generators 'Visual Studio 17 2022' '').Instance 'C:\OtherVS2022'

# Emit native UTF-8 bytes, then read them with a legacy console code page.
# Use Unicode escapes so Windows PowerShell can also read this BOM-less file.
$fixture = '{"displayName":"Visual Studio \u751f\u6210\u5de5\u5177", "installationPath":"C:\\\u5de5\u5177\\VS2022"}' | ConvertFrom-Json
$json = $fixture | ConvertTo-Json -Compress
$payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($json))
$emit = '[Console]::OpenStandardOutput().Write([Convert]::FromBase64String("' + $payload + '"), 0, ' + [Text.Encoding]::UTF8.GetByteCount($json) + ')'
$encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($emit))
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
$previousEncoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [Text.Encoding]::GetEncoding(936)
    $decoded = Read-Utf8ProcessOutput $powershell "-NoProfile -EncodedCommand $encodedCommand" | ConvertFrom-Json
    Assert-Equal $decoded.displayName $fixture.displayName
    Assert-Equal $decoded.installationPath $fixture.installationPath
    Assert-Equal ([Console]::OutputEncoding.CodePage) 936
    $failure = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('exit 7'))
    Assert-Throws { Read-Utf8ProcessOutput $powershell "-NoProfile -EncodedCommand $failure" } 'failed (exit 7)'
}
finally { [Console]::OutputEncoding = $previousEncoding }
Write-Output "PASS: $cases build script cases"
