Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Directory]::GetParent($PSScriptRoot).FullName
$releaseSource = Join-Path $repoRoot 'release\public_preview'
$signer = Join-Path $repoRoot 'tools\sign_release_artifact.ps1'
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('wotb-public-preview-test-' + [Guid]::NewGuid().ToString('N'))
$keyId = 'bundle-test-' + [Guid]::NewGuid().ToString('N')
$keyName = 'BlitzForge.WotbMod.BundleTest.' + [Guid]::NewGuid().ToString('N')
$assertions = 0

function Assert-PreviewTest {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
    $script:assertions++
}

function Write-TestBytes {
    param([string]$Path, [string]$Text)
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
    [System.IO.File]::WriteAllBytes($Path, [System.Text.Encoding]::ASCII.GetBytes($Text))
}

function Invoke-ExpectedSuccess {
    param([string]$Script, [hashtable]$Arguments)
    try { & $Script @Arguments | Out-Host } catch { throw "Expected success from $Script`: $($_.Exception.Message)" }
}

function Invoke-ExpectedFailure {
    param([string]$Script, [hashtable]$Arguments, [string]$Pattern)
    $failed = $false
    try { & $Script @Arguments | Out-Host } catch {
        $failed = $true
        Assert-PreviewTest -Condition ($_.Exception.Message -match $Pattern) -Message "failure message matches $Pattern"
    }
    Assert-PreviewTest -Condition $failed -Message "$Script failed closed"
}

function New-FakeGame {
    param([string]$Path, [byte[]]$ClientBytes)
    [System.IO.Directory]::CreateDirectory((Join-Path $Path 'mods')) | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $Path 'wotblitz.exe'), $ClientBytes)
    $ini = @(
        '; unrelated settings must survive'
        '[mods]'
        'unrelated.mod=1'
        ''
        '[permissions]'
        'unrelated.mod=2'
        ''
        '[custom]'
        'keep=this'
        ''
    ) -join [Environment]::NewLine
    [System.IO.File]::WriteAllText((Join-Path $Path 'mods\mods.ini'), $ini, (New-Object System.Text.UTF8Encoding($false)))
}

function Write-FakeManifest {
    param([string]$Bundle, [string]$Version, [string]$ClientHash)
    . (Join-Path $Bundle 'common.ps1')
    $payloadRoot = Join-Path $Bundle 'payload'
    $manifest = [ordered]@{
        schema = 1
        release_id = 'wotbmod-api-public-preview-test'
        version = $Version
        channel = 'test'
        architecture = 'windows-x86'
        client = [ordered]@{
            version = 'test-client'
            executable = 'wotblitz.exe'
            executable_sha256 = $ClientHash
            binding_pack = 1
        }
        signer = [ordered]@{
            algorithm = 'ecdsa-p256-sha256'
            key_id = $keyId
            package_target = 'mods/wotbmod.lua_host.wotbmod'
            signature_target = 'mods/wotbmod.lua_host.wotbmod.sig'
            public_key_target = "mods/trust/keys/$keyId.p256"
        }
        payload = @(
            [ordered]@{ source = 'payload/version.dll'; target = 'version.dll'; sha256 = Get-WotbModSha256 (Join-Path $payloadRoot 'version.dll'); marker = 'wotb_mod proxy loaded' },
            [ordered]@{ source = 'payload/wotb_mod_loader.dll'; target = 'wotb_mod_loader.dll'; sha256 = Get-WotbModSha256 (Join-Path $payloadRoot 'wotb_mod_loader.dll'); marker = 'BlitzForge live loader starting' },
            [ordered]@{ source = 'payload/mods/wotbmod.lua_host.wotbmod'; target = 'mods/wotbmod.lua_host.wotbmod'; sha256 = Get-WotbModSha256 (Join-Path $payloadRoot 'mods\wotbmod.lua_host.wotbmod') },
            [ordered]@{ source = 'payload/mods/wotbmod.lua_host.wotbmod.sig'; target = 'mods/wotbmod.lua_host.wotbmod.sig'; sha256 = Get-WotbModSha256 (Join-Path $payloadRoot 'mods\wotbmod.lua_host.wotbmod.sig') },
            [ordered]@{ source = "payload/mods/trust/keys/$keyId.p256"; target = "mods/trust/keys/$keyId.p256"; sha256 = Get-WotbModSha256 (Join-Path $payloadRoot "mods\trust\keys\$keyId.p256") }
        )
    }
    Write-WotbModUtf8NoBom -Path (Join-Path $Bundle 'release-manifest.json') -Text (($manifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine)
}

[System.IO.Directory]::CreateDirectory($testRoot) | Out-Null
try {
    $bundle = Join-Path $testRoot 'bundle'
    [System.IO.Directory]::CreateDirectory($bundle) | Out-Null
    foreach ($name in @('common.ps1', 'install.ps1', 'verify.ps1', 'uninstall.ps1')) {
        Copy-Item -LiteralPath (Join-Path $releaseSource $name) -Destination $bundle -Force
    }
    $payload = Join-Path $bundle 'payload'
    Write-TestBytes -Path (Join-Path $payload 'version.dll') -Text 'fake binary: wotb_mod proxy loaded'
    Write-TestBytes -Path (Join-Path $payload 'wotb_mod_loader.dll') -Text 'fake binary: BlitzForge live loader starting'
    Write-TestBytes -Path (Join-Path $payload 'mods\wotbmod.lua_host.wotbmod') -Text 'fake signed package v1'
    $signature = Join-Path $payload 'mods\wotbmod.lua_host.wotbmod.sig'
    $publicKey = Join-Path $payload "mods\trust\keys\$keyId.p256"
    & $signer -Artifact (Join-Path $payload 'mods\wotbmod.lua_host.wotbmod') -SignatureOutput $signature -PublicKeyOutput $publicKey -KeyId $keyId -KeyName $keyName | Out-Host

    $clientBytes = [System.Text.Encoding]::ASCII.GetBytes('exact fake game client')
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $clientHash = -join ($sha.ComputeHash($clientBytes) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose() }
    Write-FakeManifest -Bundle $bundle -Version '0.1.0-preview.1' -ClientHash $clientHash
    $systemVersion = Join-Path $testRoot 'system-version.dll'
    Write-TestBytes -Path $systemVersion -Text 'fake 32-bit system version.dll'

    $game = Join-Path $testRoot 'game'
    New-FakeGame -Path $game -ClientBytes $clientBytes
    $args = @{ GameRoot = $game; SystemVersionPath = $systemVersion }
    Invoke-ExpectedSuccess -Script (Join-Path $bundle 'install.ps1') -Arguments $args
    Assert-PreviewTest (Test-Path -LiteralPath (Join-Path $game 'version.dll')) 'proxy installed'
    Assert-PreviewTest (Test-Path -LiteralPath (Join-Path $game 'wotb_mod_loader.dll')) 'loader installed'
    Assert-PreviewTest (Test-Path -LiteralPath (Join-Path $game 'vorig.dll')) 'system version copy installed'
    Assert-PreviewTest (Test-Path -LiteralPath (Join-Path $game 'mods\cache\public-preview-install.json')) 'state recorded'
    Invoke-ExpectedSuccess -Script (Join-Path $bundle 'verify.ps1') -Arguments @{ GameRoot = $game }

    Write-TestBytes -Path (Join-Path $payload 'mods\wotbmod.lua_host.wotbmod') -Text 'fake signed package v2 update'
    & $signer -Artifact (Join-Path $payload 'mods\wotbmod.lua_host.wotbmod') -SignatureOutput $signature -PublicKeyOutput $publicKey -KeyId $keyId -KeyName $keyName | Out-Host
    Write-FakeManifest -Bundle $bundle -Version '0.1.0-preview.2' -ClientHash $clientHash
    Invoke-ExpectedSuccess -Script (Join-Path $bundle 'install.ps1') -Arguments $args
    $state = Get-Content -LiteralPath (Join-Path $game 'mods\cache\public-preview-install.json') -Raw | ConvertFrom-Json
    Assert-PreviewTest ($state.version -eq '0.1.0-preview.2') 'update advances state version'
    Invoke-ExpectedSuccess -Script (Join-Path $bundle 'verify.ps1') -Arguments @{ GameRoot = $game }

    $loaderPath = Join-Path $game 'wotb_mod_loader.dll'
    Write-TestBytes -Path $loaderPath -Text 'changed after install'
    Invoke-ExpectedFailure -Script (Join-Path $bundle 'uninstall.ps1') -Arguments @{ GameRoot = $game } -Pattern 'changed'
    Assert-PreviewTest (Test-Path -LiteralPath (Join-Path $game 'mods\cache\public-preview-install.json')) 'failed uninstall preserves state'
    Copy-Item -LiteralPath (Join-Path $payload 'wotb_mod_loader.dll') -Destination $loaderPath -Force
    Invoke-ExpectedSuccess -Script (Join-Path $bundle 'uninstall.ps1') -Arguments @{ GameRoot = $game }
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $game 'version.dll'))) 'created proxy removed'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $game 'wotb_mod_loader.dll'))) 'created loader removed'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $game 'vorig.dll'))) 'created system copy removed'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $game 'mods\cache\public-preview-install.json'))) 'state removed'
    $restoredIni = [System.IO.File]::ReadAllText((Join-Path $game 'mods\mods.ini'))
    Assert-PreviewTest ($restoredIni -match 'unrelated\.mod=1') 'unrelated mod setting preserved'
    Assert-PreviewTest ($restoredIni -match 'keep=this') 'unrelated section preserved'
    Assert-PreviewTest ($restoredIni -notmatch 'wotbmod\.lua_host') 'host settings restored to absent'

    $foreignGame = Join-Path $testRoot 'foreign-game'
    New-FakeGame -Path $foreignGame -ClientBytes $clientBytes
    Write-TestBytes -Path (Join-Path $foreignGame 'version.dll') -Text 'foreign proxy must survive'
    . (Join-Path $bundle 'common.ps1')
    $foreignHash = Get-WotbModSha256 (Join-Path $foreignGame 'version.dll')
    $foreignIni = [System.IO.File]::ReadAllText((Join-Path $foreignGame 'mods\mods.ini'))
    Invoke-ExpectedFailure -Script (Join-Path $bundle 'install.ps1') -Arguments @{ GameRoot = $foreignGame; SystemVersionPath = $systemVersion } -Pattern 'foreign loader'
    Assert-PreviewTest ((Get-WotbModSha256 (Join-Path $foreignGame 'version.dll')) -eq $foreignHash) 'foreign proxy unchanged'
    Assert-PreviewTest ([System.IO.File]::ReadAllText((Join-Path $foreignGame 'mods\mods.ini')) -eq $foreignIni) 'foreign refusal leaves config unchanged'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $foreignGame 'mods\cache\public-preview-install.json'))) 'foreign refusal creates no state'

    $rollbackGame = Join-Path $testRoot 'rollback-game'
    New-FakeGame -Path $rollbackGame -ClientBytes $clientBytes
    Write-TestBytes -Path (Join-Path $rollbackGame 'mods\blocked-parent') -Text 'this file prevents child creation'
    Write-TestBytes -Path (Join-Path $payload 'rollback.bin') -Text 'must never remain installed'
    $rollbackManifestPath = Join-Path $bundle 'release-manifest.json'
    $rollbackManifest = Get-Content -LiteralPath $rollbackManifestPath -Raw | ConvertFrom-Json
    $rollbackManifest.payload += [PSCustomObject]@{
        source = 'payload/rollback.bin'
        target = 'mods/blocked-parent/child.bin'
        sha256 = Get-WotbModSha256 (Join-Path $payload 'rollback.bin')
    }
    [System.IO.File]::WriteAllText(
        $rollbackManifestPath,
        ($rollbackManifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine,
        (New-Object System.Text.UTF8Encoding($false)))
    $rollbackIni = [System.IO.File]::ReadAllText((Join-Path $rollbackGame 'mods\mods.ini'))
    Invoke-ExpectedFailure -Script (Join-Path $bundle 'install.ps1') -Arguments @{ GameRoot = $rollbackGame; SystemVersionPath = $systemVersion } -Pattern 'blocked-parent|already exists'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $rollbackGame 'version.dll'))) 'failed partial install rolls proxy back'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $rollbackGame 'wotb_mod_loader.dll'))) 'failed partial install rolls loader back'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $rollbackGame 'vorig.dll'))) 'failed partial install rolls system copy back'
    Assert-PreviewTest ([System.IO.File]::ReadAllText((Join-Path $rollbackGame 'mods\mods.ini')) -eq $rollbackIni) 'failed partial install restores config'
    Assert-PreviewTest (-not (Test-Path -LiteralPath (Join-Path $rollbackGame 'mods\cache\public-preview-install.json'))) 'failed partial install creates no state'

    Write-Host "PASS: public-preview installer/update/verify/uninstall/foreign-protection ($assertions assertions)"
} finally {
    $provider = [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
    if ([System.Security.Cryptography.CngKey]::Exists($keyName, $provider)) {
        $key = [System.Security.Cryptography.CngKey]::Open($keyName, $provider)
        try { $key.Delete() } finally { $key.Dispose() }
    }
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $owned = [System.IO.Path]::GetFullPath($testRoot)
    if (-not $owned.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Test cleanup escaped the temp root: $owned"
    }
    if (Test-Path -LiteralPath $owned) { Remove-Item -LiteralPath $owned -Recurse -Force }
}
