param([string]$GameRoot = '')

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\World of Tanks Blitz'
}
$gameRootPath = Resolve-WotbModFullPath $GameRoot
$modsRoot = Join-Path $gameRootPath 'mods'
$cacheRoot = Join-Path $modsRoot 'cache'
$statePath = Join-Path $cacheRoot 'public-preview-install.json'
$iniPath = Join-Path $modsRoot 'mods.ini'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
    throw 'Public-preview install state is missing; refusing an untracked uninstall.'
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($state.schema -ne 1 -or @($state.files).Count -eq 0) {
    throw 'Public-preview install state is invalid.'
}
$backupParent = Join-Path $cacheRoot 'install_backups'
$backupRoot = Assert-WotbModChildPath -Root $backupParent -Path ([string]$state.backup_root)

foreach ($entry in @($state.files)) {
    $target = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$entry.target))
    if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
        (Get-WotbModSha256 $target) -ne ([string]$entry.installed_sha256).ToLowerInvariant()) {
        throw "Installed file was changed; refusing to remove or overwrite it: $($entry.target)"
    }
    if ([bool]$entry.original_existed) {
        $backup = Assert-WotbModChildPath -Root $backupRoot -Path (Join-Path $backupRoot ([string]$entry.backup_relative))
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
            throw "Original backup is missing: $($entry.target)"
        }
    }
}

# The catalogue was installed through the CLI, so the CLI takes it out again
# (ledger entry, Lua folder, cached archive). Not fatal: a player who already
# removed it by hand, or whose tools are gone, still gets the files restored.
$cliExe = Join-Path $gameRootPath 'wotbmod\wotbmod.exe'
$cliScript = Join-Path $gameRootPath 'wotbmod\wotbmod.py'
$ledgerPath = Join-Path $cacheRoot 'wotbmod_installs.json'
$ledger = $null
if (Test-Path -LiteralPath $ledgerPath -PathType Leaf) {
    try { $ledger = Get-Content -LiteralPath $ledgerPath -Raw | ConvertFrom-Json } catch { $ledger = $null }
}
foreach ($catalogId in @('blitzforge.catalog', 'blitzforge.catalog.ui')) {
    $entry = if ($null -ne $ledger) { $ledger.packages.$catalogId } else { $null }
    if ($null -eq $entry -or $entry.state -ne 'installed') { continue }
    if (Test-Path -LiteralPath $cliExe -PathType Leaf) {
        & $cliExe uninstall $catalogId --game-root $gameRootPath --yes | Out-Host
    } else {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($null -ne $python) { & $python.Source $cliScript uninstall $catalogId --game-root $gameRootPath --yes | Out-Host }
    }
    if ($LASTEXITCODE -ne 0) { Write-Warning "$catalogId could not be uninstalled through wotbmod; run `wotbmod uninstall $catalogId` by hand." }
}

$transactionParent = Join-Path $cacheRoot 'install_transactions'
[System.IO.Directory]::CreateDirectory($transactionParent) | Out-Null
$transactionRoot = Assert-WotbModChildPath -Root $transactionParent -Path (Join-Path $transactionParent ([Guid]::NewGuid().ToString('N')))
[System.IO.Directory]::CreateDirectory($transactionRoot) | Out-Null
$paths = @($state.files | ForEach-Object {
    Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$_.target))
}) + @($iniPath, $statePath) | Select-Object -Unique
$transactionRecords = @()
for ($index = 0; $index -lt $paths.Count; ++$index) {
    $path = [string]$paths[$index]
    $existed = Test-Path -LiteralPath $path -PathType Leaf
    $backup = Join-Path $transactionRoot ($index.ToString('D4') + '.bin')
    if ($existed) { Copy-Item -LiteralPath $path -Destination $backup -Force }
    $transactionRecords += [PSCustomObject]@{ Path = $path; Existed = $existed; Backup = $backup }
}

try {
    foreach ($entry in @($state.files)) {
        $target = Assert-WotbModChildPath -Root $gameRootPath -Path (Join-Path $gameRootPath ([string]$entry.target))
        if ([bool]$entry.original_existed) {
            $backup = Assert-WotbModChildPath -Root $backupRoot -Path (Join-Path $backupRoot ([string]$entry.backup_relative))
            Copy-Item -LiteralPath $backup -Destination $target -Force
        } else {
            Remove-Item -LiteralPath $target -Force
        }
    }
    if ([bool]$state.original_ini.mod.Present) {
        Set-WotbModIniValue -Path $iniPath -Section 'mods' -Key 'wotbmod.lua_host' -Value ([string]$state.original_ini.mod.Value)
    } else {
        Set-WotbModIniValue -Path $iniPath -Section 'mods' -Key 'wotbmod.lua_host' -Remove
    }
    if ([bool]$state.original_ini.permission.Present) {
        Set-WotbModIniValue -Path $iniPath -Section 'permissions' -Key 'wotbmod.lua_host' -Value ([string]$state.original_ini.permission.Value)
    } else {
        Set-WotbModIniValue -Path $iniPath -Section 'permissions' -Key 'wotbmod.lua_host' -Remove
    }
    Remove-Item -LiteralPath $statePath -Force
} catch {
    foreach ($record in $transactionRecords) {
        if ($record.Existed) {
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($record.Path)) | Out-Null
            Copy-Item -LiteralPath $record.Backup -Destination $record.Path -Force
        } elseif (Test-Path -LiteralPath $record.Path -PathType Leaf) {
            Remove-Item -LiteralPath $record.Path -Force
        }
    }
    throw
} finally {
    Remove-WotbModOwnedTree -Root $transactionParent -Path $transactionRoot
}

Write-Host "Uninstalled BlitzForge API $($state.version)."
Write-Host "Original recovery backup retained at: $backupRoot"
# Undo what install.ps1 -AddToPath / -RegisterLauncher did for this game folder
# (and nothing else: another game copy keeps its own entries).
$toolsDir = Join-Path $gameRootPath 'wotbmod'
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($userPath) {
    $all = @($userPath -split ';' | Where-Object { $_ -ne '' })
    $kept = @($all | Where-Object { $_.TrimEnd('\') -ine $toolsDir.TrimEnd('\') })
    if ($kept.Count -ne $all.Count) {
        [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
        Write-Host "Removed from the user PATH: $toolsDir"
    }
}
$schemeKey = 'HKCU:\Software\Classes\wotbmod'
if (Test-Path -LiteralPath $schemeKey) {
    $commandKey = Join-Path $schemeKey 'shell\open\command'
    $command = ''
    if (Test-Path -LiteralPath $commandKey) {
        $command = [string](Get-ItemProperty -LiteralPath $commandKey -ErrorAction SilentlyContinue).'(default)'
    }
    if ($command -and $command.IndexOf($gameRootPath, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        Remove-Item -LiteralPath $schemeKey -Recurse -Force
        Write-Host 'Removed the wotbmod:// registration.'
    }
}
