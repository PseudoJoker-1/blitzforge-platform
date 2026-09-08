param(
    [string]$GameRoot = '',
    [string]$SystemVersionPath = '',
    [switch]$RegisterLauncher,
    [switch]$AddToPath
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\World of Tanks Blitz'
}
if ([string]::IsNullOrWhiteSpace($SystemVersionPath)) {
    $SystemVersionPath = Join-Path $env:SystemRoot 'SysWOW64\version.dll'
}
$bundleRoot = Resolve-WotbModFullPath $PSScriptRoot
$gameRootPath = Resolve-WotbModFullPath $GameRoot
$manifest = Read-WotbModReleaseManifest $bundleRoot

if (-not (Test-Path -LiteralPath $gameRootPath -PathType Container)) {
    throw "Game directory is missing: $gameRootPath"
}
$clientPath = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$manifest.client.executable))
if ((Get-WotbModSha256 $clientPath) -ne ([string]$manifest.client.executable_sha256).ToLowerInvariant()) {
    throw "Unsupported game executable. This preview requires $($manifest.client.version) x86 with the exact published SHA-256."
}
Assert-WotbModBundlePayload -BundleRoot $bundleRoot -Manifest $manifest

$packageEntry = @($manifest.payload | Where-Object { $_.target -eq $manifest.signer.package_target })
$signatureEntry = @($manifest.payload | Where-Object { $_.target -eq $manifest.signer.signature_target })
$keyEntry = @($manifest.payload | Where-Object { $_.target -eq $manifest.signer.public_key_target })
if ($packageEntry.Count -ne 1 -or $signatureEntry.Count -ne 1 -or $keyEntry.Count -ne 1) {
    throw 'Release manifest does not identify one package, signature, and public key.'
}
Test-WotbModDetachedSignature `
    -PackagePath (Join-Path $bundleRoot ([string]$packageEntry[0].source)) `
    -SignaturePath (Join-Path $bundleRoot ([string]$signatureEntry[0].source)) `
    -PublicKeyPath (Join-Path $bundleRoot ([string]$keyEntry[0].source)) `
    -ExpectedKeyId ([string]$manifest.signer.key_id) | Out-Null

$modsRoot = Join-Path $gameRootPath 'mods'
$cacheRoot = Join-Path $modsRoot 'cache'
$statePath = Join-Path $cacheRoot 'public-preview-install.json'
$iniPath = Join-Path $modsRoot 'mods.ini'
$backupParent = Join-Path $cacheRoot 'install_backups'
$transactionParent = Join-Path $cacheRoot 'install_transactions'

$oldState = $null
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
    $oldState = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($oldState.schema -ne 1 -or @($oldState.files).Count -eq 0) {
        throw 'Existing public-preview install state is invalid; refusing to overwrite it.'
    }
    foreach ($entry in @($oldState.files)) {
        $installed = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$entry.target))
        if (-not (Test-Path -LiteralPath $installed -PathType Leaf) -or
            (Get-WotbModSha256 $installed) -ne ([string]$entry.installed_sha256).ToLowerInvariant()) {
            throw "Installed file was changed outside the installer: $($entry.target)"
        }
    }
}

foreach ($entry in @($manifest.payload)) {
    $target = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$entry.target))
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $relative = ([string]$entry.target).Replace('/', '\')
        if ($relative -ieq 'version.dll' -and
            -not (Test-WotbModAsciiMarker -Path $target -Marker ([string]$entry.marker))) {
            throw 'Existing version.dll is not the BlitzForge proxy; refusing to replace a foreign loader.'
        }
        if ($relative -ieq 'wotb_mod_loader.dll' -and
            -not (Test-WotbModAsciiMarker -Path $target -Marker ([string]$entry.marker))) {
            throw 'Existing wotb_mod_loader.dll is foreign; refusing to replace it.'
        }
        if ($relative -ieq ([string]$manifest.signer.public_key_target) -and
            (Get-WotbModSha256 $target) -ne ([string]$entry.sha256).ToLowerInvariant()) {
            throw 'The trusted-key path already contains a different key.'
        }
    }
}

if (-not (Test-Path -LiteralPath $SystemVersionPath -PathType Leaf)) {
    throw "32-bit system version.dll is missing: $SystemVersionPath"
}
[System.IO.Directory]::CreateDirectory($cacheRoot) | Out-Null

$createdBackup = $false
if ($null -ne $oldState) {
    $backupRoot = Assert-WotbModChildPath -Root $backupParent -Path ([string]$oldState.backup_root)
    $fileStates = @($oldState.files)
    $originalIni = $oldState.original_ini
} else {
    [System.IO.Directory]::CreateDirectory($backupParent) | Out-Null
    $backupRoot = Join-Path $backupParent ('public-preview-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    $backupRoot = Assert-WotbModChildPath -Root $backupParent -Path $backupRoot
    [System.IO.Directory]::CreateDirectory($backupRoot) | Out-Null
    $createdBackup = $true
    $fileStates = @()
    $originalIni = [PSCustomObject]@{
        mod = Get-WotbModIniValue -Path $iniPath -Section 'mods' -Key 'wotbmod.lua_host'
        permission = Get-WotbModIniValue -Path $iniPath -Section 'permissions' -Key 'wotbmod.lua_host'
    }
}

function Get-InstalledStateIndex {
    param([string]$Target)
    $normalized = $Target.Replace('/', '\')
    for ($index = 0; $index -lt $script:fileStates.Count; ++$index) {
        if (([string]$script:fileStates[$index].target).Replace('/', '\') -ieq $normalized) {
            return $index
        }
    }
    return -1
}

function Add-OriginalFileState {
    param([string]$RelativeTarget, [string]$CurrentPath)
    $index = Get-InstalledStateIndex $RelativeTarget
    if ($index -ge 0) { return $index }
    $originalExisted = Test-Path -LiteralPath $CurrentPath -PathType Leaf
    $backupRelative = $null
    if ($originalExisted) {
        $backupRelative = 'originals\' + $script:fileStates.Count.ToString('D4') + '.bin'
        $backupPath = Assert-WotbModChildPath -Root $backupRoot -Path (Join-Path $backupRoot $backupRelative)
        [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($backupPath)) | Out-Null
        Copy-Item -LiteralPath $CurrentPath -Destination $backupPath -Force
    }
    $script:fileStates += [PSCustomObject]@{
        target = $RelativeTarget.Replace('/', '\')
        original_existed = [bool]$originalExisted
        backup_relative = $backupRelative
        installed_sha256 = ''
    }
    return $script:fileStates.Count - 1
}

$planned = @()
foreach ($entry in @($manifest.payload)) {
    $target = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$entry.target))
    $stateIndex = Add-OriginalFileState -RelativeTarget ([string]$entry.target) -CurrentPath $target
    $planned += [PSCustomObject]@{
        Source = Assert-WotbModChildPath -Root $bundleRoot -Path (Join-Path $bundleRoot ([string]$entry.source))
        Target = $target
        RelativeTarget = ([string]$entry.target).Replace('/', '\')
        Sha256 = ([string]$entry.sha256).ToLowerInvariant()
        StateIndex = $stateIndex
    }
}

$vorigPath = Join-Path $gameRootPath 'vorig.dll'
if (-not (Test-Path -LiteralPath $vorigPath -PathType Leaf)) {
    $stateIndex = Add-OriginalFileState -RelativeTarget 'vorig.dll' -CurrentPath $vorigPath
    $planned += [PSCustomObject]@{
        Source = Resolve-WotbModFullPath $SystemVersionPath
        Target = $vorigPath
        RelativeTarget = 'vorig.dll'
        Sha256 = Get-WotbModSha256 $SystemVersionPath
        StateIndex = $stateIndex
    }
}

[System.IO.Directory]::CreateDirectory($transactionParent) | Out-Null
$transactionRoot = Assert-WotbModChildPath -Root $transactionParent -Path (Join-Path $transactionParent ([Guid]::NewGuid().ToString('N')))
[System.IO.Directory]::CreateDirectory($transactionRoot) | Out-Null
$transactionRecords = @()
$transactionPaths = @($planned.Target) + @($iniPath, $statePath) | Select-Object -Unique
for ($index = 0; $index -lt $transactionPaths.Count; ++$index) {
    $path = [string]$transactionPaths[$index]
    $existed = Test-Path -LiteralPath $path -PathType Leaf
    $backup = Join-Path $transactionRoot ($index.ToString('D4') + '.bin')
    if ($existed) { Copy-Item -LiteralPath $path -Destination $backup -Force }
    $transactionRecords += [PSCustomObject]@{ Path = $path; Existed = $existed; Backup = $backup }
}

try {
    foreach ($item in $planned) {
        [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($item.Target)) | Out-Null
        Copy-Item -LiteralPath $item.Source -Destination $item.Target -Force
        if ((Get-WotbModSha256 $item.Target) -ne $item.Sha256) {
            throw "Installed file failed post-copy verification: $($item.RelativeTarget)"
        }
        $fileStates[$item.StateIndex].installed_sha256 = $item.Sha256
    }
    Set-WotbModIniValue -Path $iniPath -Section 'mods' -Key 'wotbmod.lua_host' -Value '1'
    Set-WotbModIniValue -Path $iniPath -Section 'permissions' -Key 'wotbmod.lua_host' -Value '3'

    # The in-game catalogue is a Lua package installed through the CLI, so it
    # sits in the ledger with its signature and updates from the portal like
    # any other mod. The staged archive (under wotbmod\setup\packages, never
    # under mods\, where it would already count as installed) was just copied
    # and hash-checked above; --require-signature makes the trust key (also
    # just copied) load-bearing.
    $stagedPackages = Join-Path $gameRootPath 'wotbmod\setup\packages'
    if (Test-Path -LiteralPath $stagedPackages -PathType Container) {
        $cliExe = Join-Path $gameRootPath 'wotbmod\wotbmod.exe'
        $cliScript = Join-Path $gameRootPath 'wotbmod\wotbmod.py'
        $python = $null
        if (-not (Test-Path -LiteralPath $cliExe -PathType Leaf)) {
            $python = Get-Command python -ErrorAction SilentlyContinue
            if ($null -eq $python) { throw 'python is not on PATH and the bundle has no wotbmod.exe; the catalogue cannot be installed.' }
        }
        foreach ($staged in @(Get-ChildItem -LiteralPath $stagedPackages -Filter '*.wotbmod' | Sort-Object Name)) {
            if ($null -eq $python) {
                & $cliExe install $staged.FullName --game-root $gameRootPath --yes --require-signature | Out-Host
            } else {
                & $python.Source $cliScript install $staged.FullName --game-root $gameRootPath --yes --require-signature | Out-Host
            }
            if ($LASTEXITCODE -ne 0) { throw "The in-game catalogue failed to install ($($staged.Name), wotbmod exit code $LASTEXITCODE)." }
        }
    }

    $state = [ordered]@{
        schema = 1
        release_id = [string]$manifest.release_id
        version = [string]$manifest.version
        installed_at_utc = [DateTime]::UtcNow.ToString('o')
        backup_root = $backupRoot
        files = @($fileStates)
        original_ini = $originalIni
    }
    Write-WotbModUtf8NoBom -Path $statePath -Text (($state | ConvertTo-Json -Depth 8) + [Environment]::NewLine)
} catch {
    foreach ($record in $transactionRecords) {
        if ($record.Existed) {
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($record.Path)) | Out-Null
            Copy-Item -LiteralPath $record.Backup -Destination $record.Path -Force
        } elseif (Test-Path -LiteralPath $record.Path -PathType Leaf) {
            Remove-Item -LiteralPath $record.Path -Force
        }
    }
    if ($createdBackup) { Remove-WotbModOwnedTree -Root $backupParent -Path $backupRoot }
    throw
} finally {
    Remove-WotbModOwnedTree -Root $transactionParent -Path $transactionRoot
}

Write-Host "Installed BlitzForge API $($manifest.version) for client $($manifest.client.version)."
Write-Host "State: $statePath"
Write-Host "Recovery backup: $backupRoot"
# The player's tools ship in <game>\wotbmod\; with -RegisterLauncher the
# wotbmod:// scheme is registered for the current user so the portal's
# install button reaches this machine. Registration is a per-user registry
# key, never a system change, and can be undone with `unregister`.
if ($RegisterLauncher) {
    $launcherExe = Join-Path $gameRootPath 'wotbmod\wotbmod.exe'
    $launcher = Join-Path $gameRootPath 'wotbmod\wotbmod_launcher.py'
    if (Test-Path -LiteralPath $launcherExe -PathType Leaf) {
        # The frozen tools need no Python on the player's machine.
        & $launcherExe launcher register --game-root $gameRootPath
        if ($LASTEXITCODE -ne 0) { Write-Warning 'Launcher registration failed; run it by hand later.' }
    } elseif (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
        Write-Warning 'This bundle has no wotbmod\wotbmod_launcher.py; nothing to register.'
    } else {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($null -eq $python) {
            Write-Warning "python is not on PATH; register later with: python `"$launcher`" register --game-root `"$gameRootPath`""
        } else {
            & $python.Source $launcher register --game-root $gameRootPath
            if ($LASTEXITCODE -ne 0) { Write-Warning 'Launcher registration failed; run it by hand later.' }
        }
    }
}
# With -AddToPath the tools folder joins the user's PATH, so `wotbmod install ...`
# works in any new terminal (wotbmod.exe, or wotbmod.cmd + Python). User
# scope only; uninstall.ps1 removes the entry again.
if ($AddToPath) {
    $toolsDir = Join-Path $gameRootPath 'wotbmod'
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @()
    if ($userPath) { $entries = @($userPath -split ';' | Where-Object { $_ -ne '' }) }
    $present = @($entries | Where-Object { $_.TrimEnd('\') -ieq $toolsDir.TrimEnd('\') })
    if ($present.Count -eq 0) {
        [Environment]::SetEnvironmentVariable('Path', (($entries + $toolsDir) -join ';'), 'User')
        Write-Host "Added to the user PATH: $toolsDir (open a new terminal to use the wotbmod command)."
    } else {
        Write-Host "Already on the user PATH: $toolsDir"
    }
}
