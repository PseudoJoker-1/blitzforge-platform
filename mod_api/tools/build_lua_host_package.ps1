param(
    [string]$OutputDirectory = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Directory]::GetParent($PSScriptRoot).FullName
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build'))
$stagingRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $buildRoot 'lua_host_package_staging'))
$distributionRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $buildRoot 'lua_host_distribution'))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $buildRoot 'lua_host_packages'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

foreach ($ownedPath in @($stagingRoot, $distributionRoot)) {
    if (-not $ownedPath.StartsWith(
            $buildRoot + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Lua host output escaped the repository build directory: $ownedPath"
    }
}

if (-not $SkipBuild) {
    & (Join-Path $repoRoot 'tests\build_lua_vendor_tests.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'Lua vendor tests failed.' }
    & (Join-Path $repoRoot 'tests\build_lua_host_tests.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'Lua host tests failed.' }
}

$releaseDll = Join-Path $buildRoot 'lua_host_release\wotbmod_lua_host.dll'
if (-not (Test-Path -LiteralPath $releaseDll -PathType Leaf)) {
    throw "Release Lua host DLL is missing: $releaseDll"
}

foreach ($ownedPath in @($stagingRoot, $distributionRoot)) {
    if (Test-Path -LiteralPath $ownedPath) {
        Remove-Item -LiteralPath $ownedPath -Recurse -Force
    }
}
[System.IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($distributionRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

$hostStage = Join-Path $stagingRoot 'wotbmod.lua_host'
[System.IO.Directory]::CreateDirectory(
    (Join-Path $hostStage 'bin\windows-x86')) | Out-Null

function Copy-RequiredFile {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required Lua host package input is missing: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Get-Sha256Hex {
    param([string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            return -join @(
                $sha256.ComputeHash($stream) |
                    ForEach-Object { $_.ToString('x2') }
            )
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

$hostSource = Join-Path $repoRoot 'examples\lua_host'
Copy-RequiredFile (Join-Path $hostSource 'manifest.json') $hostStage
Copy-RequiredFile (Join-Path $hostSource 'README_RU.md') $hostStage
Copy-RequiredFile (Join-Path $repoRoot 'docs\LUA_MODS_RU.md') $hostStage
Copy-RequiredFile (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt') $hostStage
Copy-RequiredFile $releaseDll `
    (Join-Path $hostStage 'bin\windows-x86\wotbmod_lua_host.dll')

$package = Join-Path $OutputDirectory 'wotbmod.lua_host.wotbmod'
& python (Join-Path $repoRoot 'tools\wotbmod.py') pack $hostStage `
    --output $package --no-client-check | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Packing the Lua host failed.' }
& python (Join-Path $repoRoot 'tools\wotbmod.py') validate $package `
    --no-client-check | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Lua host package validation failed.' }

Copy-RequiredFile $package $distributionRoot
Copy-RequiredFile (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt') $distributionRoot
foreach ($sample in @(
        @{
            Source = 'lua_hello'
            Destination = 'example.lua_hello'
        },
        @{
            Source = 'lua_ui_framework'
            Destination = 'example.lua_ui_framework'
        },
        @{
            Source = 'lua_ally_tracker'
            Destination = 'example.lua_ally_tracker'
        },
        @{
            Source = 'lua_battle_telemetry'
            Destination = 'example.lua_battle_telemetry'
        },
        @{
            Source = 'lua_session_stats'
            Destination = 'example.lua_session_stats'
        },
        @{
            Source = 'lua_dava_workshop'
            Destination = 'example.lua_dava_workshop'
        }
    )) {
    $sampleSource = Join-Path $repoRoot ('examples\' + $sample.Source)
    $sampleDestination = Join-Path $distributionRoot `
        ('mods\lua\' + $sample.Destination)
    [System.IO.Directory]::CreateDirectory($sampleDestination) | Out-Null
    foreach ($file in @('manifest.json', 'main.lua', 'README_RU.md')) {
        Copy-RequiredFile (Join-Path $sampleSource $file) $sampleDestination
    }
    $fixtures = Join-Path $sampleSource 'fixtures'
    if (Test-Path -LiteralPath $fixtures -PathType Container) {
        Copy-Item -LiteralPath $fixtures -Destination $sampleDestination `
            -Recurse -Force
    }
    if ($sample.Source -eq 'lua_dava_workshop') {
        & python (Join-Path $repoRoot 'tools\make_workshop_archive.py') `
            (Join-Path $sampleDestination 'fixtures\archive_payload.txt') `
            (Join-Path $sampleDestination 'fixtures\workshop.zip')
        if ($LASTEXITCODE -ne 0) {
            throw 'Lua DAVA workshop archive generation failed.'
        }
    }
}

$packageItem = Get-Item -LiteralPath $package
[PSCustomObject]@{
    Package = $packageItem.FullName
    Bytes = $packageItem.Length
    Sha256 = Get-Sha256Hex $packageItem.FullName
    Distribution = $distributionRoot
} | Format-List
