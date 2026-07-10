[CmdletBinding()]
param(
    [Parameter()]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    [Parameter()]
    [string]$Target,

    [Parameter()]
    [switch]$Test
)

$ErrorActionPreference = 'Stop'

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: vswhere'
    exit 1
}

$vswhereArguments = @(
    '-latest'
    '-products', 'Microsoft.VisualStudio.Product.BuildTools'
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
)
$installationPath = (& $vswhere @vswhereArguments -property installationPath | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($installationPath)) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: VS2022 VC x64'
    exit 1
}

$installationVersion = (& $vswhere @vswhereArguments -property installationVersion | Select-Object -First 1)
if ($installationVersion.Trim() -cne '17.14.36109.1') {
    Write-Host 'BUILD_TOOL_VERSION_MISMATCH: VS2022 Build Tools 17.14.36109.1'
    exit 1
}

$windowsSdkInclude = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
if (-not (Test-Path -LiteralPath $windowsSdkInclude -PathType Container)) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: Windows SDK 10.0.26100.0'
    exit 1
}

$cmakeCommand = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -eq $cmakeCommand) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: cmake 3.29.2'
    exit 1
}
$cmakePath = $cmakeCommand.Source
$cmakeVersionLine = (& $cmakePath --version | Select-Object -First 1)
if ($cmakeVersionLine -cne 'cmake version 3.29.2') {
    Write-Host 'BUILD_TOOL_NOT_FOUND: cmake 3.29.2'
    exit 1
}

$ninjaCommand = Get-Command ninja.exe -CommandType Application -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($null -eq $ninjaCommand) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: ninja 1.12.0'
    exit 1
}
$ninjaPath = $ninjaCommand.Source
$ninjaVersion = (& $ninjaPath --version | Select-Object -First 1)
if ($ninjaVersion -cne '1.12.0') {
    Write-Host 'BUILD_TOOL_NOT_FOUND: ninja 1.12.0'
    exit 1
}

$devCommand = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path -LiteralPath $devCommand -PathType Leaf)) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: VS2022 VC x64'
    exit 1
}

$environmentLines = & $env:ComSpec /s /c "`"$devCommand`" -no_logo -arch=x64 -host_arch=x64 && set"
if ($LASTEXITCODE -ne 0) {
    Write-Host 'BUILD_TOOL_NOT_FOUND: VS2022 VC x64'
    exit 1
}

foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -LiteralPath "Env:$name" -Value $value
    }
}

$preset = "windows-x64-$($Configuration.ToLowerInvariant())"
& $cmakePath --preset $preset
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$buildArguments = @('--build', '--preset', $preset)
if (-not [string]::IsNullOrWhiteSpace($Target)) {
    $buildArguments += @('--target', $Target)
}

& $cmakePath @buildArguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($Test) {
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    & $ctestPath --preset $preset
    exit $LASTEXITCODE
}

exit 0
