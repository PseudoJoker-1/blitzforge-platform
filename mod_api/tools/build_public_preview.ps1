param(
    [string]$OutputDirectory = '',
    [string]$Version = '0.1.0-preview.1',
    [string]$KeyId = 'blitzforge-preview-2026',
    [string]$KeyName = 'BlitzForge.WotbMod.PublicPreview.2026',
    [switch]$SkipBuild,
    [switch]$SkipExe,
    [switch]$SkipSetup
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Directory]::GetParent($PSScriptRoot).FullName
$workspaceRoot = [System.IO.Directory]::GetParent($repoRoot).FullName
$gameRoot = [System.IO.Directory]::GetParent($workspaceRoot).FullName
. (Join-Path $repoRoot 'release\public_preview\common.ps1')

if ($Version -notmatch '^0\.1\.0-preview\.[0-9]+$') {
    throw 'Public-preview version must use 0.1.0-preview.N.'
}
if (-not $SkipBuild) {
    & (Join-Path $repoRoot 'build.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'Mod API build failed.' }
    Push-Location (Join-Path $workspaceRoot 'proxy_dll')
    try {
        & .\build.cmd
        if ($LASTEXITCODE -ne 0) { throw 'Proxy build failed.' }
    } finally { Pop-Location }
    & (Join-Path $repoRoot 'loader\build_live.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'Live loader build failed.' }
}

$buildRoot = Join-Path $repoRoot 'build'
$previewRoot = Join-Path $buildRoot 'public_preview'
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $previewRoot ("BlitzForge-API-$Version-win32")
}
$bundleRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not $bundleRoot.StartsWith(
        [System.IO.Path]::GetFullPath($previewRoot).TrimEnd('\') + '\',
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must remain under $previewRoot"
}
if (Test-Path -LiteralPath $bundleRoot) {
    Remove-WotbModOwnedTree -Root $previewRoot -Path $bundleRoot
}
[System.IO.Directory]::CreateDirectory($bundleRoot) | Out-Null

$packageDirectory = Join-Path $buildRoot 'lua_host_packages'
& (Join-Path $repoRoot 'tools\build_lua_host_package.ps1') `
    -OutputDirectory $packageDirectory -SkipBuild
if ($LASTEXITCODE -ne 0) { throw 'Lua host packaging failed.' }

$client = Join-Path $gameRoot 'wotblitz.exe'
$proxy = Join-Path $workspaceRoot 'proxy_dll\version.dll'
$loader = Join-Path $buildRoot 'wotb_mod_loader.dll'
$package = Join-Path $packageDirectory 'wotbmod.lua_host.wotbmod'
foreach ($required in @($client, $proxy, $loader, $package)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Release input is missing: $required"
    }
}
$expectedClientBuild = '11.20.0.887'
$expectedClientHash = '4813544d3d6b9f45a357e87f5a14d065bd108c4acd324ac1506cb6d1ad6ff0af'
$expectedBindingPack = 112000887
if ((Get-WotbModSha256 $client) -ne $expectedClientHash) {
    throw "The local game client does not match the frozen $expectedClientBuild fingerprint."
}

foreach ($name in @('common.ps1', 'install.ps1', 'verify.ps1', 'uninstall.ps1', 'README_RU.md')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "release\public_preview\$name") -Destination (Join-Path $bundleRoot $name) -Force
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'THIRD_PARTY_NOTICES.txt') -Destination $bundleRoot -Force

$payloadRoot = Join-Path $bundleRoot 'payload'
$modsPayload = Join-Path $payloadRoot 'mods'
$trustPayload = Join-Path $modsPayload 'trust\keys'
[System.IO.Directory]::CreateDirectory($trustPayload) | Out-Null
Copy-Item -LiteralPath $proxy -Destination (Join-Path $payloadRoot 'version.dll') -Force
Copy-Item -LiteralPath $loader -Destination (Join-Path $payloadRoot 'wotb_mod_loader.dll') -Force
$payloadPackage = Join-Path $modsPayload 'wotbmod.lua_host.wotbmod'
Copy-Item -LiteralPath $package -Destination $payloadPackage -Force
$payloadSignature = $payloadPackage + '.sig'
$payloadKey = Join-Path $trustPayload ($KeyId + '.p256')
& (Join-Path $repoRoot 'tools\sign_release_artifact.ps1') `
    -Artifact $payloadPackage `
    -SignatureOutput $payloadSignature `
    -PublicKeyOutput $payloadKey `
    -KeyId $KeyId `
    -KeyName $KeyName
if ($LASTEXITCODE -ne 0) { throw 'Release signing failed.' }

# The player's tools, installed next to the game as <game>\wotbmod\: the CLI
# (install/update/rollback/list/info/quarantine/report-crash/verify) and the
# wotbmod:// launcher the portal's install button talks to. Plain Python
# files; the installer can register the launcher with -RegisterLauncher.
$sdkPayload = Join-Path $payloadRoot 'wotbmod'
[System.IO.Directory]::CreateDirectory($sdkPayload) | Out-Null
$sdkFiles = @('wotbmod.py', 'wotbmod_packages.py', 'wotbmod_trust.py', 'wotbmod_scan.py', 'wotbmod_dvpl.py', 'wotbmod_launcher.py', 'wotbmod.cmd')
foreach ($name in $sdkFiles) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "tools\$name") -Destination (Join-Path $sdkPayload $name) -Force
}
# wotbmod.exe (CLI + launcher in one file, no Python needed) comes from
# PyInstaller; -SkipExe reuses build/exe/dist. The setup program itself is
# the Inno Setup installer built after the bundle (see setup.iss).
$exeDist = Join-Path $buildRoot 'exe\dist'
if (-not $SkipExe) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $pythonCommand) { throw 'python is required to build the executables (or pass -SkipExe).' }
    $exeJobs = @(
        @{ Name = 'wotbmod'; Script = 'tools\wotbmod_entry.py'; Windowed = $false; Data = $true }
    )
    foreach ($job in $exeJobs) {
        $pyArgs = @('-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile', '--name', $job.Name,
                    '--distpath', $exeDist, '--workpath', (Join-Path $buildRoot 'exe\work'),
                    '--specpath', (Join-Path $buildRoot 'exe\spec'), '--paths', (Join-Path $repoRoot 'tools'))
        if ($job.Windowed) { $pyArgs += '--windowed' } else { $pyArgs += '--console' }
        if ($job.Data) {
            foreach ($name in $sdkFiles) {
                if ($name -like '*.py') { $pyArgs += @('--add-data', ((Join-Path $repoRoot "tools\$name") + ';.')) }
            }
        }
        $pyArgs += (Join-Path $repoRoot $job.Script)
        & $pythonCommand.Source @pyArgs 2>&1 | Where-Object { $_ -match 'ERROR|completed successfully' } | ForEach-Object { Write-Host "  pyinstaller: $_" }
        if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed for $($job.Name)." }
    }
}
$cliExe = Join-Path $exeDist 'wotbmod.exe'
$haveExe = Test-Path -LiteralPath $cliExe -PathType Leaf
if ($haveExe) {
    Copy-Item -LiteralPath $cliExe -Destination (Join-Path $sdkPayload 'wotbmod.exe') -Force
} else {
    Write-Warning 'wotbmod.exe is missing under build/exe/dist; the bundle ships the Python tools only.'
}
# The portal's public key, when a portal key sits next to the SDK checkout
# (wotbmod-portal/data/<id>.p256): releases it signs become trusted on install.
$portalKeys = @()
$portalData = Join-Path $workspaceRoot 'wotbmod-portal\data'
if (Test-Path -LiteralPath $portalData -PathType Container) {
    foreach ($key in Get-ChildItem -LiteralPath $portalData -Filter '*.p256' -File) {
        if ($key.BaseName -ne $KeyId) {
            Copy-Item -LiteralPath $key.FullName -Destination (Join-Path $trustPayload $key.Name) -Force
            $portalKeys += $key.Name
        }
    }
}

$examplesRoot = Join-Path $bundleRoot 'examples'
[System.IO.Directory]::CreateDirectory($examplesRoot) | Out-Null
foreach ($sample in @(
        'lua_hello',
        'lua_ui_framework',
        'lua_ally_tracker',
        'lua_battle_telemetry',
        'lua_session_stats',
        'lua_dava_workshop'
    )) {
    $source = Join-Path $repoRoot ("examples\$sample")
    $sampleManifestPath = Join-Path $source 'manifest.json'
    $sampleManifest = Get-Content -LiteralPath $sampleManifestPath -Raw | ConvertFrom-Json
    $sampleId = [string]$sampleManifest.id
    if ([string]::IsNullOrWhiteSpace($sampleId) -or
        $sampleId -match '[\\/]' -or
        $sampleId -in @('.', '..')) {
        throw "Lua example has an invalid manifest id: $sampleManifestPath"
    }
    $destination = Join-Path $examplesRoot $sampleId
    [System.IO.Directory]::CreateDirectory($destination) | Out-Null
    foreach ($file in @('manifest.json', 'main.lua', 'README_RU.md')) {
        Copy-Item -LiteralPath (Join-Path $source $file) -Destination $destination -Force
    }
    $fixtures = Join-Path $source 'fixtures'
    if (Test-Path -LiteralPath $fixtures -PathType Container) {
        Copy-Item -LiteralPath $fixtures -Destination $destination -Recurse -Force
    }
    if ($sample -eq 'lua_dava_workshop') {
        & python (Join-Path $repoRoot 'tools\make_workshop_archive.py') `
            (Join-Path $destination 'fixtures\archive_payload.txt') `
            (Join-Path $destination 'fixtures\workshop.zip')
        if ($LASTEXITCODE -ne 0) {
            throw 'Lua DAVA workshop archive generation failed.'
        }
    }
}

$payload = @(
    [ordered]@{
        source = 'payload/version.dll'; target = 'version.dll'
        sha256 = Get-WotbModSha256 (Join-Path $payloadRoot 'version.dll')
        marker = 'wotb_mod proxy loaded'
    },
    [ordered]@{
        source = 'payload/wotb_mod_loader.dll'; target = 'wotb_mod_loader.dll'
        sha256 = Get-WotbModSha256 (Join-Path $payloadRoot 'wotb_mod_loader.dll')
        marker = 'BlitzForge live loader starting'
    },
    [ordered]@{
        source = 'payload/mods/wotbmod.lua_host.wotbmod'; target = 'mods/wotbmod.lua_host.wotbmod'
        sha256 = Get-WotbModSha256 $payloadPackage
    },
    [ordered]@{
        source = 'payload/mods/wotbmod.lua_host.wotbmod.sig'; target = 'mods/wotbmod.lua_host.wotbmod.sig'
        sha256 = Get-WotbModSha256 $payloadSignature
    },
    [ordered]@{
        source = "payload/mods/trust/keys/$KeyId.p256"; target = "mods/trust/keys/$KeyId.p256"
        sha256 = Get-WotbModSha256 $payloadKey
    }
)
foreach ($name in $sdkFiles) {
    $payload += [ordered]@{
        source = "payload/wotbmod/$name"; target = "wotbmod/$name"
        sha256 = Get-WotbModSha256 (Join-Path $sdkPayload $name)
    }
}
if ($haveExe) {
    $payload += [ordered]@{
        source = 'payload/wotbmod/wotbmod.exe'; target = 'wotbmod/wotbmod.exe'
        sha256 = Get-WotbModSha256 (Join-Path $sdkPayload 'wotbmod.exe')
    }
}
foreach ($name in $portalKeys) {
    $payload += [ordered]@{
        source = "payload/mods/trust/keys/$name"; target = "mods/trust/keys/$name"
        sha256 = Get-WotbModSha256 (Join-Path $trustPayload $name)
    }
}
$manifest = [ordered]@{
    schema = 1
    release_id = 'wotbmod-api-public-preview'
    version = $Version
    channel = 'public-preview'
    architecture = 'windows-x86'
    client = [ordered]@{
        version = $expectedClientBuild
        executable = 'wotblitz.exe'
        executable_sha256 = $expectedClientHash
        binding_pack = $expectedBindingPack
    }
    tools = [ordered]@{
        cli = $(if ($haveExe) { 'wotbmod/wotbmod.exe' } else { 'wotbmod/wotbmod.py' })
        setup = "BlitzForge-Setup-$Version.exe"
        launcher = $(if ($haveExe) { 'wotbmod/wotbmod.exe' } else { 'wotbmod/wotbmod_launcher.py' })
        launcher_scheme = 'wotbmod'
        register = $(if ($haveExe) { 'wotbmod\wotbmod.exe launcher register --game-root <game>' } else { 'python wotbmod\wotbmod_launcher.py register --game-root <game>' })
        path_entry = 'wotbmod'
        portal_keys = $portalKeys
    }
    signer = [ordered]@{
        algorithm = 'ecdsa-p256-sha256'
        key_id = $KeyId
        package_target = 'mods/wotbmod.lua_host.wotbmod'
        signature_target = 'mods/wotbmod.lua_host.wotbmod.sig'
        public_key_target = "mods/trust/keys/$KeyId.p256"
    }
    payload = $payload
}
Write-WotbModUtf8NoBom -Path (Join-Path $bundleRoot 'release-manifest.json') -Text (($manifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine)

$archive = $bundleRoot + '.zip'
if (Test-Path -LiteralPath $archive -PathType Leaf) { Remove-Item -LiteralPath $archive -Force }
Compress-Archive -LiteralPath $bundleRoot -DestinationPath $archive -CompressionLevel Optimal

# The setup wizard players download: Inno Setup around the bundle (see
# release\public_preview\setup.iss). -SkipSetup, or no ISCC.exe, leaves only the zip.
$setupExe = ''
if (-not $SkipSetup) {
    $isccCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )
    $iscc = @($isccCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    if ($iscc.Count -eq 0) {
        Write-Warning 'Inno Setup 6 (ISCC.exe) was not found; no setup wizard built. Install it with: winget install JRSoftware.InnoSetup'
    } else {
        $buildNumber = [int]($Version -replace '^0\.1\.0-preview\.', '')
        $setupBase = "BlitzForge-Setup-$Version"
        & $iscc[0] '/Qp' "/DAppVersion=$Version" "/DBuildNumber=$buildNumber" "/DBundleDir=$bundleRoot" "/DBundleZip=$archive" `
            "/DClientVersion=$expectedClientBuild" "/DClientHash=$expectedClientHash" "/DOutputDir=$previewRoot" `
            "/DOutputBaseName=$setupBase" (Join-Path $repoRoot 'release\public_preview\setup.iss')
        if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
        $setupExe = Join-Path $previewRoot ($setupBase + '.exe')
    }
}
[PSCustomObject]@{
    Bundle = $bundleRoot
    Archive = $archive
    ArchiveSha256 = Get-WotbModSha256 $archive
    Setup = $setupExe
    SetupSha256 = $(if ($setupExe) { Get-WotbModSha256 $setupExe } else { '' })
    Version = $Version
    Client = "$expectedClientBuild x86"
    KeyId = $KeyId
    CngKeyName = $KeyName
} | Format-List
