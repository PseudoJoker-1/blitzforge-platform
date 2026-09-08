param(
    [Parameter(Mandatory = $true)]
    [string]$DataDirectory,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$ModsDirectory,
    [string]$GameDirectory,
    [string]$LoaderLogPath,
    [string]$BindingPackPath
)

$ErrorActionPreference = 'Stop'

function ConvertTo-SanitizedText {
    param([string]$Text)
    if ($null -eq $Text) { return '' }
    $value = $Text
    $value = [regex]::Replace(
        $value,
        '(?im)([\x22](?:authorization|bearer|token|password|secret|api[_-]?key)[\x22]\s*:\s*)[\x22][^\x22\r\n]*[\x22]',
        '$1"[REDACTED]"')
    $value = [regex]::Replace(
        $value,
        '(?im)(authorization|bearer|token|password|secret|api[_-]?key)\s*[:=]\s*[^\s,;"}]+',
        '$1=[REDACTED]')
    $value = [regex]::Replace(
        $value,
        '(?i)\bBearer\s+[A-Za-z0-9._~+/=-]+',
        'Bearer [REDACTED]')
    $value = [regex]::Replace(
        $value,
        '(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b',
        '[REDACTED_EMAIL]')
    $value = [regex]::Replace(
        $value,
        '(?im)^.*(?:chat|conversation|message_body|rpc_payload).*(?:\r?\n|$)',
        '')
    return $value
}

function Copy-SanitizedTextFile {
    param(
        [string]$Source,
        [string]$Destination,
        [switch]$DropRpcTrace
    )
    if (-not [System.IO.File]::Exists($Source)) { return $false }
    $text = [System.IO.File]::ReadAllText($Source)
    if ($DropRpcTrace) {
        $lines = $text -split "`r?`n" | Where-Object {
            $_ -and
            $_ -notmatch '(?i)rpc' -and
            $_ -notmatch '(?i)chat|conversation|message_body'
        }
        $text = ($lines -join [Environment]::NewLine)
        if ($text) { $text += [Environment]::NewLine }
    }
    [System.IO.File]::WriteAllText(
        $Destination,
        (ConvertTo-SanitizedText $text),
        [System.Text.UTF8Encoding]::new($false))
    return $true
}

$dataRoot = [System.IO.Path]::GetFullPath($DataDirectory)
if (-not [System.IO.Directory]::Exists($dataRoot)) {
    throw "Validation data directory does not exist: $dataRoot"
}

if ([string]::IsNullOrWhiteSpace($ModsDirectory)) {
    $dataParent = [System.IO.Directory]::GetParent($dataRoot)
    $modsParent = if ($dataParent) { $dataParent.Parent } else { $null }
    if ($modsParent) { $ModsDirectory = $modsParent.FullName }
}
$modsRoot = if ([string]::IsNullOrWhiteSpace($ModsDirectory)) {
    $null
} else {
    [System.IO.Path]::GetFullPath($ModsDirectory)
}
if ([string]::IsNullOrWhiteSpace($GameDirectory) -and $modsRoot) {
    $GameDirectory = [System.IO.Directory]::GetParent($modsRoot).FullName
}
$gameRoot = if ([string]::IsNullOrWhiteSpace($GameDirectory)) {
    $null
} else {
    [System.IO.Path]::GetFullPath($GameDirectory)
}

$required = @(
    'LIVE_CAPABILITY_MATRIX.json',
    'LIVE_EVENT_TRACE.jsonl',
    'LIVE_VALIDATION_RESULTS.json',
    'LIVE_FAILURES.md'
)
foreach ($name in $required) {
    $candidate = [System.IO.Path]::Combine($dataRoot, $name)
    if (-not [System.IO.File]::Exists($candidate)) {
        throw "Required validation artifact is missing: $candidate"
    }
}

$matrixPath = [System.IO.Path]::Combine($dataRoot, 'LIVE_CAPABILITY_MATRIX.json')
$resultsPath = [System.IO.Path]::Combine($dataRoot, 'LIVE_VALIDATION_RESULTS.json')
$matrix = Get-Content -Raw -LiteralPath $matrixPath | ConvertFrom-Json
$results = Get-Content -Raw -LiteralPath $resultsPath | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace([string]$matrix.fingerprint_key) -or
    [string]::IsNullOrWhiteSpace([string]$results.fingerprint_key) -or
    $matrix.fingerprint_key -ne $results.fingerprint_key) {
    throw 'Matrix/results fingerprint_key is absent or does not match.'
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
}
if ([System.IO.File]::Exists($resolvedOutput)) {
    throw "Output already exists; choose a new path: $resolvedOutput"
}

$staging = [System.IO.Path]::Combine(
    [System.IO.Path]::GetTempPath(),
    'wotbmod-native-validation-' + [System.Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($staging) | Out-Null
try {
    foreach ($name in @(
        'LIVE_CAPABILITY_MATRIX.json',
        'LIVE_VALIDATION_RESULTS.json',
        'LIVE_FAILURES.md',
        'FAIL_COMMENT.txt',
        'ACTIVE_NATIVE_TEST.json')) {
        $source = [System.IO.Path]::Combine($dataRoot, $name)
        Copy-SanitizedTextFile `
            -Source $source `
            -Destination ([System.IO.Path]::Combine($staging, $name)) | Out-Null
    }
    Copy-SanitizedTextFile `
        -Source ([System.IO.Path]::Combine($dataRoot, 'LIVE_EVENT_TRACE.jsonl')) `
        -Destination ([System.IO.Path]::Combine($staging, 'LIVE_EVENT_TRACE.jsonl')) `
        -DropRpcTrace | Out-Null

    $bindingCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($BindingPackPath)) {
        $bindingCandidates += [System.IO.Path]::GetFullPath($BindingPackPath)
    }
    $bindingCandidates += [System.IO.Path]::Combine($dataRoot, 'binding_pack_validation.json')
    if ($modsRoot) {
        $bindingCandidates += [System.IO.Path]::Combine(
            $modsRoot, 'cache', 'binding_pack_validation.json')
    }
    $bindingSource = $bindingCandidates |
        Where-Object { [System.IO.File]::Exists($_) } |
        Select-Object -First 1
    if ($bindingSource) {
        Copy-SanitizedTextFile `
            -Source $bindingSource `
            -Destination ([System.IO.Path]::Combine($staging, 'binding_pack_validation.json')) |
            Out-Null
    }

    if ($modsRoot) {
        $crashMarker = [System.IO.Path]::Combine(
            $modsRoot, 'cache', 'runtime_session.marker')
        Copy-SanitizedTextFile `
            -Source $crashMarker `
            -Destination ([System.IO.Path]::Combine($staging, 'runtime_session.marker')) |
            Out-Null
        $autoDisabled = [System.IO.Path]::Combine(
            $modsRoot, 'cache', 'auto_disabled_mod.ini')
        Copy-SanitizedTextFile `
            -Source $autoDisabled `
            -Destination ([System.IO.Path]::Combine($staging, 'auto_disabled_mod.ini')) |
            Out-Null

        $inventory = @()
        Get-ChildItem -LiteralPath $modsRoot -Filter manifest.json -File -Recurse `
            -ErrorAction SilentlyContinue | ForEach-Object {
                try {
                    $manifest = Get-Content -Raw -LiteralPath $_.FullName |
                        ConvertFrom-Json
                    $dependencies = @()
                    if ($manifest.dependencies) {
                        $dependencies = @(
                            $manifest.dependencies.PSObject.Properties.Name |
                                Sort-Object)
                    }
                    $inventory += [ordered]@{
                        id = [string]$manifest.id
                        version = [string]$manifest.version
                        type = [string]$manifest.type
                        permissions = @($manifest.permissions | Sort-Object)
                        dependency_ids = $dependencies
                    }
                } catch {
                    $inventory += [ordered]@{
                        id = '[UNREADABLE_MANIFEST]'
                        version = ''
                        type = ''
                        permissions = @()
                        dependency_ids = @()
                    }
                }
            }
        [ordered]@{
            schema = 'wotbmod.validation-mod-inventory/v1'
            mods = @($inventory | Sort-Object id, version)
        } | ConvertTo-Json -Depth 8 |
            Set-Content -LiteralPath (
                [System.IO.Path]::Combine($staging, 'MOD_INVENTORY.json')) `
                -Encoding UTF8
    }

    $logCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($LoaderLogPath)) {
        $logCandidates += [System.IO.Path]::GetFullPath($LoaderLogPath)
    }
    $logCandidates += [System.IO.Path]::Combine($dataRoot, 'loader.log')
    $logCandidates += [System.IO.Path]::Combine($dataRoot, 'live_host.log')
    if ($modsRoot) {
        $logCandidates += [System.IO.Path]::Combine($modsRoot, 'logs', 'loader.log')
    }
    $logIndex = 0
    foreach ($log in ($logCandidates | Select-Object -Unique)) {
        if (-not [System.IO.File]::Exists($log)) { continue }
        ++$logIndex
        Copy-SanitizedTextFile `
            -Source $log `
            -Destination ([System.IO.Path]::Combine(
                $staging, "sanitized_loader_log_$logIndex.txt")) |
            Out-Null
    }

    $loaderFileVersion = ''
    if ($gameRoot) {
        foreach ($name in @('wotb_mod_loader.dll', 'wotb_mod_loader.asi')) {
            $candidate = [System.IO.Path]::Combine($gameRoot, $name)
            if ([System.IO.File]::Exists($candidate)) {
                $loaderFileVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo(
                    $candidate).FileVersion
                break
            }
        }
    }

    $manifest = [ordered]@{
        schema = 'wotbmod.live-validation-bundle/v2'
        exported_at_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        fingerprint_key = [string]$results.fingerprint_key
        client_version = [string]$results.client_version
        client_build = [string]$results.client_build
        executable_sha256 = [string]$results.executable_sha256
        loader_version = [string]$results.loader_version
        loader_file_version = [string]$loaderFileVersion
        validation_mod_version = [string]$results.validation_mod_version
        privacy = [ordered]@{
            secrets_redacted = $true
            email_redacted = $true
            chat_excluded = $true
            rpc_payload_excluded = $true
            raw_manifests_excluded = $true
        }
        files = @(Get-ChildItem -LiteralPath $staging -File |
            Select-Object -ExpandProperty Name |
            Sort-Object)
    }
    $manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (
            [System.IO.Path]::Combine($staging, 'BUNDLE_MANIFEST.json')) `
            -Encoding UTF8

    Compress-Archive -Path ([System.IO.Path]::Combine($staging, '*')) `
        -DestinationPath $resolvedOutput -CompressionLevel Optimal
} finally {
    if ([System.IO.Directory]::Exists($staging)) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}

Write-Output "Validation bundle: $resolvedOutput"
Write-Output "Fingerprint: $($results.fingerprint_key)"
