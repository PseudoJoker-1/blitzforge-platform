$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::Combine($PSScriptRoot, '..'))
$testRoot = [System.IO.Path]::Combine(
    $repoRoot, 'build', 'validation_bundle_export_test')
$buildRoot = [System.IO.Path]::Combine($repoRoot, 'build')
if (-not $testRoot.StartsWith(
        $buildRoot + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe validation bundle test root: $testRoot"
}
if ([System.IO.Directory]::Exists($testRoot)) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

$checks = 0
function Assert-True {
    param([bool]$Condition, [string]$Message)
    $script:checks++
    if (-not $Condition) { throw "VALIDATION BUNDLE TEST FAILED: $Message" }
}

$fingerprint = '11.19.0.834|0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
$data = [System.IO.Path]::Combine($testRoot, 'data')
$mods = [System.IO.Path]::Combine($testRoot, 'game', 'mods')
$game = [System.IO.Path]::Combine($testRoot, 'game')
$cache = [System.IO.Path]::Combine($mods, 'cache')
$package = [System.IO.Path]::Combine($mods, 'sample.fixture')
$logs = [System.IO.Path]::Combine($mods, 'logs')
$expanded = [System.IO.Path]::Combine($testRoot, 'expanded')
$bundle = [System.IO.Path]::Combine($testRoot, 'validation-bundle.zip')
foreach ($directory in @($data, $cache, $package, $logs, $expanded)) {
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null
}

[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($data, 'LIVE_CAPABILITY_MATRIX.json'),
    (@{
        schema = 'wotbmod.live-capability-matrix/v1'
        fingerprint_key = $fingerprint
        capabilities = @(@{ name = 'wotbmod.core'; status = 'AVAILABLE' })
    } | ConvertTo-Json -Depth 8),
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($data, 'LIVE_VALIDATION_RESULTS.json'),
    (@{
        fingerprint_key = $fingerprint
        client_version = '11.19.0'
        client_build = '11.19.0.834'
        executable_sha256 = $fingerprint.Split('|')[1]
        loader_version = '3.0.0-rc1'
        validation_mod_version = '3.0.0-rc1'
        tests = @(@{ id = 'HOST'; status = 'PASS' })
    } | ConvertTo-Json -Depth 8),
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($data, 'LIVE_EVENT_TRACE.jsonl'),
    "{`"event`":`"wotbmod.client.hangar.enter`",`"token`":`"trace-secret`"}`n" +
    "{`"event`":`"rpc_payload`",`"payload`":`"private-rpc-body`"}`n" +
    "{`"event`":`"chat`",`"message_body`":`"private-chat-body`"}`n",
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($data, 'LIVE_FAILURES.md'),
    "owner=tester@example.com`nsecret=validation-secret`n",
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($data, 'binding_pack_validation.json'),
    "{`"fingerprint_key`":`"$fingerprint`",`"api_key`":`"binding-secret`"}",
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($cache, 'runtime_session.marker'),
    "[session]`nphase=client_event`nlast_event=wotbmod.client.hangar.enter`npassword=marker-secret`n",
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($cache, 'auto_disabled_mod.ini'),
    "[auto_disable]`nid=sample.fixture`nreason=repeated_crash_attribution`n",
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($package, 'manifest.json'),
    (@{
        manifest_version = 1
        type = 'content'
        id = 'sample.fixture'
        name = 'Fixture'
        version = '1.0.0'
        developer = 'tests'
        permissions = @('content', 'resources.mod')
        dependencies = @{ 'sample.base' = '^1.0' }
        content = 'content.json'
        private_note = 'raw-manifest-must-not-be-exported'
    } | ConvertTo-Json -Depth 8),
    [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    [System.IO.Path]::Combine($logs, 'loader.log'),
    "loader started`nauthorization=loader-secret`nuser=tester@example.com`n" +
    "rpc_payload=private-rpc-log`nchat=private-chat-log`n",
    [System.Text.UTF8Encoding]::new($false))

try {
    & ([System.IO.Path]::Combine(
        $repoRoot, 'tools', 'export_live_validation_bundle.ps1')) `
        -DataDirectory $data `
        -OutputPath $bundle `
        -ModsDirectory $mods `
        -GameDirectory $game
    $exportOk = $?
    Assert-True $exportOk 'exporter returned a failure code'
    Assert-True ([System.IO.File]::Exists($bundle)) 'bundle was not created'
    Expand-Archive -LiteralPath $bundle -DestinationPath $expanded -Force

    $expected = @(
        'BUNDLE_MANIFEST.json',
        'LIVE_CAPABILITY_MATRIX.json',
        'LIVE_EVENT_TRACE.jsonl',
        'LIVE_VALIDATION_RESULTS.json',
        'LIVE_FAILURES.md',
        'binding_pack_validation.json',
        'runtime_session.marker',
        'auto_disabled_mod.ini',
        'MOD_INVENTORY.json',
        'sanitized_loader_log_1.txt'
    )
    foreach ($name in $expected) {
        Assert-True (
            [System.IO.File]::Exists(
                [System.IO.Path]::Combine($expanded, $name))) `
            "bundle is missing $name"
    }

    $manifest = Get-Content -Raw -LiteralPath (
        [System.IO.Path]::Combine($expanded, 'BUNDLE_MANIFEST.json')) |
        ConvertFrom-Json
    Assert-True ($manifest.fingerprint_key -eq $fingerprint) `
        'bundle fingerprint is not exact'
    Assert-True ($manifest.loader_version -eq '3.0.0-rc1') `
        'loader version is missing'
    Assert-True (
        $manifest.privacy.secrets_redacted -and
        $manifest.privacy.email_redacted -and
        $manifest.privacy.chat_excluded -and
        $manifest.privacy.rpc_payload_excluded -and
        $manifest.privacy.raw_manifests_excluded) `
        'privacy contract flags are incomplete'

    $inventoryText = Get-Content -Raw -LiteralPath (
        [System.IO.Path]::Combine($expanded, 'MOD_INVENTORY.json'))
    Assert-True ($inventoryText -match 'sample\.fixture') `
        'mod inventory omits mod identity'
    Assert-True (
        $inventoryText -match 'resources\.mod' -and
        $inventoryText -match 'sample\.base') `
        'mod inventory omits permissions or dependency IDs'
    Assert-True ($inventoryText -notmatch 'raw-manifest-must-not-be-exported') `
        'raw manifest contents leaked into inventory'

    $eventText = Get-Content -Raw -LiteralPath (
        [System.IO.Path]::Combine($expanded, 'LIVE_EVENT_TRACE.jsonl'))
    Assert-True ($eventText -match 'wotbmod\.client\.hangar\.enter') `
        'non-sensitive event was removed'
    Assert-True (
        $eventText -notmatch 'private-rpc-body' -and
        $eventText -notmatch 'private-chat-body') `
        'RPC or chat trace leaked'

    $allText = (Get-ChildItem -LiteralPath $expanded -File |
        ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }) -join "`n"
    foreach ($secret in @(
        'trace-secret',
        'validation-secret',
        'binding-secret',
        'marker-secret',
        'loader-secret',
        'tester@example.com',
        'private-rpc-log',
        'private-chat-log')) {
        Assert-True ($allText -notmatch [regex]::Escape($secret)) `
            "sensitive value leaked: $secret"
    }
    Assert-True ($allText -match '\[REDACTED\]') `
        'sanitizer did not leave an explicit redaction marker'

    Write-Output "VALIDATION BUNDLE OK: checks=$checks failures=0"
} finally {
    if ([System.IO.Directory]::Exists($testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
