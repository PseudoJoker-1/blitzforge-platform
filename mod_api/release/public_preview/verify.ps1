param([string]$GameRoot = '')

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\World of Tanks Blitz'
}
$bundleRoot = Resolve-WotbModFullPath $PSScriptRoot
$gameRootPath = Resolve-WotbModFullPath $GameRoot
$manifest = Read-WotbModReleaseManifest $bundleRoot
$clientPath = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$manifest.client.executable))
if ((Get-WotbModSha256 $clientPath) -ne ([string]$manifest.client.executable_sha256).ToLowerInvariant()) {
    throw 'Installed client executable does not match this release.'
}
# The wizard installs only these scripts and the manifest into
# <game>\wotbmod\setup; the bundle payload is not there. Verify the bundle
# when run from the bundle, and the installed files either way (below).
$firstSource = [string](@($manifest.payload)[0].source)
if (Test-Path -LiteralPath (Join-Path $bundleRoot $firstSource) -PathType Leaf) {
    Assert-WotbModBundlePayload -BundleRoot $bundleRoot -Manifest $manifest
} else {
    Write-Host 'Bundle payload is not next to this script (installed copy); verifying the game folder only.'
}

foreach ($entry in @($manifest.payload)) {
    $installed = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$entry.target))
    if ((Get-WotbModSha256 $installed) -ne ([string]$entry.sha256).ToLowerInvariant()) {
        throw "Installed payload mismatch: $($entry.target)"
    }
}
$packageEntry = @($manifest.payload | Where-Object { $_.target -eq $manifest.signer.package_target })[0]
$signatureEntry = @($manifest.payload | Where-Object { $_.target -eq $manifest.signer.signature_target })[0]
$keyEntry = @($manifest.payload | Where-Object { $_.target -eq $manifest.signer.public_key_target })[0]
Test-WotbModDetachedSignature `
    -PackagePath (Join-Path $gameRootPath ([string]$packageEntry.target)) `
    -SignaturePath (Join-Path $gameRootPath ([string]$signatureEntry.target)) `
    -PublicKeyPath (Join-Path $gameRootPath ([string]$keyEntry.target)) `
    -ExpectedKeyId ([string]$manifest.signer.key_id) | Out-Null

$iniPath = Join-Path $gameRootPath 'mods\mods.ini'
$enabled = Get-WotbModIniValue -Path $iniPath -Section 'mods' -Key 'wotbmod.lua_host'
$permission = Get-WotbModIniValue -Path $iniPath -Section 'permissions' -Key 'wotbmod.lua_host'
if (-not $enabled.Present -or $enabled.Value -ne '1' -or
    -not $permission.Present -or $permission.Value -ne '3') {
    throw 'wotbmod.lua_host is not enabled with REVIEWED permission tier.'
}
$statePath = Join-Path $gameRootPath 'mods\cache\public-preview-install.json'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
    throw 'Installer state is missing.'
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($state.release_id -ne $manifest.release_id -or $state.version -ne $manifest.version) {
    throw 'Installer state belongs to another release.'
}
Write-Host "PASS: BlitzForge API $($manifest.version) payload, signature, client fingerprint, and configuration are valid."
