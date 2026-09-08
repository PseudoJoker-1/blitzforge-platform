param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$SignatureOutput,
    [Parameter(Mandatory = $true)][string]$PublicKeyOutput,
    [string]$KeyId = 'blitzforge-preview-2026',
    [string]$KeyName = 'BlitzForge.WotbMod.PublicPreview.2026'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if ($KeyId -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,126}$') {
    throw 'KeyId contains unsupported characters or has an invalid length.'
}
$artifactPath = [System.IO.Path]::GetFullPath($Artifact)
if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
    throw "Artifact is missing: $artifactPath"
}
foreach ($output in @($SignatureOutput, $PublicKeyOutput)) {
    $parent = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($output))
    [System.IO.Directory]::CreateDirectory($parent) | Out-Null
}

$provider = [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
if ([System.Security.Cryptography.CngKey]::Exists($KeyName, $provider)) {
    $key = [System.Security.Cryptography.CngKey]::Open($KeyName, $provider)
} else {
    $parameters = New-Object System.Security.Cryptography.CngKeyCreationParameters
    $parameters.Provider = $provider
    $parameters.KeyUsage = [System.Security.Cryptography.CngKeyUsages]::Signing
    $parameters.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::None
    $key = [System.Security.Cryptography.CngKey]::Create(
        [System.Security.Cryptography.CngAlgorithm]::EcdsaP256,
        $KeyName,
        $parameters)
}

try {
    if ($key.AlgorithmGroup -ne [System.Security.Cryptography.CngAlgorithmGroup]::Ecdsa) {
        throw "Persisted CNG key is not ECDSA: $KeyName"
    }
    $digest = [System.Security.Cryptography.SHA256]::Create().ComputeHash(
        [System.IO.File]::ReadAllBytes($artifactPath))
    $ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)
    try {
        $ecdsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
        $signature = $ecdsa.SignHash($digest)
        if ($signature.Length -ne 64 -or -not $ecdsa.VerifyHash($digest, $signature)) {
            throw 'CNG did not produce a valid 64-byte P-256 R||S signature.'
        }
    } finally { $ecdsa.Dispose() }

    $publicBlob = $key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    if ($publicBlob.Length -ne 72 -or [BitConverter]::ToUInt32($publicBlob, 4) -ne 32) {
        throw 'CNG returned an unexpected P-256 public-key blob.'
    }
    $publicBytes = New-Object byte[] 64
    [Array]::Copy($publicBlob, 8, $publicBytes, 0, 64)

    $digestHex = -join ($digest | ForEach-Object { $_.ToString('x2') })
    $signatureHex = -join ($signature | ForEach-Object { $_.ToString('x2') })
    $publicHex = -join ($publicBytes | ForEach-Object { $_.ToString('x2') })
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText(
        [System.IO.Path]::GetFullPath($PublicKeyOutput),
        $publicHex + [Environment]::NewLine,
        $encoding)
    $sidecar = @(
        'WOTBMOD-SIGNATURE-V1'
        'algorithm=ecdsa-p256-sha256'
        "key_id=$KeyId"
        "sha256=$digestHex"
        "signature=$signatureHex"
        ''
    ) -join [Environment]::NewLine
    [System.IO.File]::WriteAllText(
        [System.IO.Path]::GetFullPath($SignatureOutput),
        $sidecar,
        $encoding)

    [PSCustomObject]@{
        Artifact = $artifactPath
        Sha256 = $digestHex
        KeyId = $KeyId
        KeyName = $KeyName
        Signature = [System.IO.Path]::GetFullPath($SignatureOutput)
        PublicKey = [System.IO.Path]::GetFullPath($PublicKeyOutput)
        PrivateKeyExportable = $false
    } | Format-List
} finally {
    $key.Dispose()
}
