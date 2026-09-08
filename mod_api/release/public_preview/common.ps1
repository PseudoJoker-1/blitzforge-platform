Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Get-WotbModSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    $stream = [System.IO.File]::OpenRead([System.IO.Path]::GetFullPath($Path))
    try {
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            return -join ($sha256.ComputeHash($stream) | ForEach-Object {
                $_.ToString('x2')
            })
        } finally { $sha256.Dispose() }
    } finally { $stream.Dispose() }
}

function Write-WotbModUtf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Resolve-WotbModFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-WotbModChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $fullRoot = Resolve-WotbModFullPath $Root
    $fullPath = Resolve-WotbModFullPath $Path
    $prefix = $fullRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped the allowed root: $fullPath"
    }
    return $fullPath
}

function Remove-WotbModOwnedTree {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $owned = Assert-WotbModChildPath -Root $Root -Path $Path
    if (Test-Path -LiteralPath $owned) {
        Remove-Item -LiteralPath $owned -Recurse -Force
    }
}

function Read-WotbModReleaseManifest {
    param([Parameter(Mandatory = $true)][string]$BundleRoot)
    $root = Resolve-WotbModFullPath $BundleRoot
    $path = Join-Path $root 'release-manifest.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Release manifest is missing: $path"
    }
    $manifest = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or
        [string]::IsNullOrWhiteSpace([string]$manifest.release_id) -or
        [string]::IsNullOrWhiteSpace([string]$manifest.version) -or
        [string]::IsNullOrWhiteSpace([string]$manifest.client.executable_sha256) -or
        @($manifest.payload).Count -eq 0) {
        throw 'Release manifest is incomplete or uses an unsupported schema.'
    }
    return $manifest
}

function Test-WotbModAsciiMarker {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Marker
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.IndexOf($Marker, [System.StringComparison]::Ordinal) -ge 0
}

function Assert-WotbModBundlePayload {
    param(
        [Parameter(Mandatory = $true)][string]$BundleRoot,
        [Parameter(Mandatory = $true)]$Manifest
    )
    $root = Resolve-WotbModFullPath $BundleRoot
    $seenTargets = @{}
    foreach ($entry in @($Manifest.payload)) {
        $source = Assert-WotbModChildPath -Root $root -Path (Join-Path $root ([string]$entry.source))
        $target = [string]$entry.target
        if ([string]::IsNullOrWhiteSpace($target) -or [System.IO.Path]::IsPathRooted($target)) {
            throw "Unsafe payload target: $target"
        }
        $normalizedTarget = $target.Replace('/', '\').ToLowerInvariant()
        if ($seenTargets.ContainsKey($normalizedTarget)) {
            throw "Duplicate payload target: $target"
        }
        $seenTargets[$normalizedTarget] = $true
        $actual = Get-WotbModSha256 $source
        if ($actual -ne ([string]$entry.sha256).ToLowerInvariant()) {
            throw "Payload hash mismatch: $($entry.source)"
        }
    }
}

function Get-WotbModIniValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Section,
        [Parameter(Mandatory = $true)][string]$Key
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [PSCustomObject]@{ Present = $false; Value = $null }
    }
    $current = ''
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $current = $Matches[1]
            continue
        }
        if ($current -ieq $Section -and $line -match '^\s*([^=;#]+?)\s*=\s*(.*?)\s*$') {
            if ($Matches[1].Trim() -ieq $Key) {
                return [PSCustomObject]@{ Present = $true; Value = $Matches[2] }
            }
        }
    }
    return [PSCustomObject]@{ Present = $false; Value = $null }
}

function Set-WotbModIniValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Section,
        [Parameter(Mandatory = $true)][string]$Key,
        [AllowNull()][string]$Value,
        [switch]$Remove
    )
    $lines = New-Object 'System.Collections.Generic.List[string]'
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in [System.IO.File]::ReadAllLines($Path)) { $lines.Add($line) }
    }

    $sectionStart = -1
    $sectionEnd = $lines.Count
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        if ($lines[$index] -match '^\s*\[([^\]]+)\]\s*$') {
            if ($sectionStart -ge 0) { $sectionEnd = $index; break }
            if ($Matches[1] -ieq $Section) { $sectionStart = $index }
        }
    }

    $keyIndex = -1
    if ($sectionStart -ge 0) {
        for ($index = $sectionStart + 1; $index -lt $sectionEnd; ++$index) {
            if ($lines[$index] -match '^\s*([^=;#]+?)\s*=') {
                if ($Matches[1].Trim() -ieq $Key) { $keyIndex = $index; break }
            }
        }
    }

    if ($Remove) {
        if ($keyIndex -ge 0) { $lines.RemoveAt($keyIndex) }
    } elseif ($keyIndex -ge 0) {
        $lines[$keyIndex] = "$Key=$Value"
    } elseif ($sectionStart -ge 0) {
        $lines.Insert($sectionEnd, "$Key=$Value")
    } else {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') { $lines.Add('') }
        $lines.Add("[$Section]")
        $lines.Add("$Key=$Value")
    }

    $parent = [System.IO.Path]::GetDirectoryName((Resolve-WotbModFullPath $Path))
    [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    $content = [string]::Join([Environment]::NewLine, $lines)
    if ($lines.Count -gt 0) { $content += [Environment]::NewLine }
    Write-WotbModUtf8NoBom -Path $Path -Text $content
}

function Test-WotbModDetachedSignature {
    param(
        [Parameter(Mandatory = $true)][string]$PackagePath,
        [Parameter(Mandatory = $true)][string]$SignaturePath,
        [Parameter(Mandatory = $true)][string]$PublicKeyPath,
        [Parameter(Mandatory = $true)][string]$ExpectedKeyId
    )
    $fields = @{}
    $lines = [System.IO.File]::ReadAllLines($SignaturePath)
    if ($lines.Count -lt 5 -or $lines[0].Trim() -ne 'WOTBMOD-SIGNATURE-V1') {
        throw 'Detached signature header is invalid.'
    }
    foreach ($line in $lines | Select-Object -Skip 1) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $separator = $line.IndexOf('=')
        if ($separator -le 0) { throw 'Detached signature entry is invalid.' }
        $name = $line.Substring(0, $separator).Trim()
        $value = $line.Substring($separator + 1).Trim()
        if ($fields.ContainsKey($name) -or [string]::IsNullOrWhiteSpace($value)) {
            throw 'Detached signature contains an empty or duplicate field.'
        }
        $fields[$name] = $value
    }
    if ($fields.Count -ne 4 -or
        $fields.algorithm -ne 'ecdsa-p256-sha256' -or
        $fields.key_id -ne $ExpectedKeyId -or
        $fields.sha256 -ne (Get-WotbModSha256 $PackagePath) -or
        $fields.signature -notmatch '^[0-9a-fA-F]{128}$') {
        throw 'Detached signature metadata does not match the package.'
    }

    $publicHex = ([System.IO.File]::ReadAllText($PublicKeyPath)).Trim()
    if ($publicHex -notmatch '^[0-9a-fA-F]{128}$') {
        throw 'Trusted P-256 public key must be 64-byte X||Y hexadecimal.'
    }
    $publicBytes = New-Object byte[] 64
    for ($index = 0; $index -lt 64; ++$index) {
        $publicBytes[$index] = [Convert]::ToByte($publicHex.Substring($index * 2, 2), 16)
    }
    $blob = New-Object byte[] 72
    [Array]::Copy([BitConverter]::GetBytes([uint32]0x31534345), 0, $blob, 0, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32]32), 0, $blob, 4, 4)
    [Array]::Copy($publicBytes, 0, $blob, 8, 64)
    $signature = New-Object byte[] 64
    for ($index = 0; $index -lt 64; ++$index) {
        $signature[$index] = [Convert]::ToByte($fields.signature.Substring($index * 2, 2), 16)
    }
    $digest = New-Object byte[] 32
    for ($index = 0; $index -lt 32; ++$index) {
        $digest[$index] = [Convert]::ToByte($fields.sha256.Substring($index * 2, 2), 16)
    }
    $key = [System.Security.Cryptography.CngKey]::Import(
        $blob, [System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    try {
        $ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)
        try {
            if (-not $ecdsa.VerifyHash($digest, $signature)) {
                throw 'ECDSA P-256 signature verification failed.'
            }
        } finally { $ecdsa.Dispose() }
    } finally { $key.Dispose() }
    return $true
}
