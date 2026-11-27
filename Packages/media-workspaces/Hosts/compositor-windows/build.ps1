param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$HostRoot,
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory
)
$ErrorActionPreference = 'Stop'
$Source = (Resolve-Path -LiteralPath $Source).Path
$RuntimeDirectory = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$Build = [IO.Path]::GetFullPath($Build)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$Revision = '04a31186e74cfa5e5baf502ea18b67601bb9d2b1'

function Invoke-Checked($Tool, $Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE" }
}

if ((git -C $Source rev-parse HEAD) -ne $Revision) {
    throw "Use the pinned CompositorWindows source revision $Revision."
}
$Adapter = Join-Path $PSScriptRoot 'LuciaCompositor.cpp'
$Patch = Join-Path $PSScriptRoot 'lucia-host.patch'
& git -C $Source apply --unidiff-zero --reverse --check $Patch 2>$null
if ($LASTEXITCODE -ne 0) {
    Invoke-Checked git @('-C', $Source, 'apply', '--unidiff-zero', $Patch)
}

$Qt = Join-Path $Source 'dependencies/qt'
Invoke-Checked cmake @('-S', $Source, '-B', $Build, '-G', 'Visual Studio 17 2022',
    '-A', 'x64', '-DCMAKE_SYSTEM_VERSION=10.0.26100.0', '-DBUILD_TESTING=OFF',
    "-DCMAKE_PREFIX_PATH=$Qt", "-DLUCIA_COMPOSITOR_HOST_SOURCE=$Adapter")
Invoke-Checked cmake @('--build', $Build, '--config', 'Release', '--target', 'LuciaMediaWorkspace')

$Destination = Join-Path $HostRoot 'Compositor'
if (Test-Path -LiteralPath $Destination) { throw "Use a new host destination: $Destination" }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
# Use the matching portable release's runtime and license files.
$Payload = @('*.dll', 'platforms', 'plugins', 'styles', 'imageformats', 'iconengines',
             'models', 'shaders', 'licenses', 'LICENSE*', 'THIRD*')
foreach ($Name in $Payload) {
    Get-ChildItem -Path (Join-Path $RuntimeDirectory $Name) | Copy-Item -Destination $Destination -Recurse
}
Invoke-Checked cmake @('--install', $Build, '--config', 'Release', '--component',
    'LuciaWorkspace', '--prefix', $Destination)
