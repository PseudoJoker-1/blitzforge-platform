param(
    [string]$GameDirectory = '',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Directory]::GetParent($PSScriptRoot).FullName
if ([string]::IsNullOrWhiteSpace($GameDirectory)) {
    $GameDirectory = [System.IO.Directory]::GetParent(
        [System.IO.Directory]::GetParent($repoRoot).FullName).FullName
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot 'build\rc1_packages'
}
$GameDirectory = [System.IO.Path]::GetFullPath($GameDirectory)
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$stagingRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'build\rc1_sample_staging'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build'))
if (-not $stagingRoot.StartsWith($buildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'RC1 staging root escaped the repository build directory.'
}

& (Join-Path $repoRoot 'tests\build_rc1_samples_tests.cmd')
if ($LASTEXITCODE -ne 0) { throw 'RC1 sample host tests failed.' }

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
[System.IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
[System.IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

function New-SampleStage {
    param([string]$Name)
    $stage = Join-Path $stagingRoot $Name
    [System.IO.Directory]::CreateDirectory($stage) | Out-Null
    [System.IO.Directory]::CreateDirectory(
        (Join-Path $stage 'bin\windows-x86')) | Out-Null
    return $stage
}

function Copy-RequiredFile {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required RC1 sample input is missing: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Pack-Sample {
    param([string]$Stage, [string]$Id)
    $package = Join-Path $OutputDirectory ($Id + '.wotbmod')
    & python (Join-Path $repoRoot 'tools\wotbmod.py') pack $Stage `
        --output $package --no-client-check | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Packing failed for $Id" }
    & python (Join-Path $repoRoot 'tools\wotbmod.py') validate $package `
        --no-client-check | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Package validation failed for $Id" }
    return $package
}

$uiStage = New-SampleStage 'sample.ui_transaction'
$uiSource = Join-Path $repoRoot 'examples\sample_ui_transaction'
Copy-RequiredFile (Join-Path $uiSource 'manifest.json') $uiStage
Copy-RequiredFile (Join-Path $uiSource 'README.md') $uiStage
Copy-RequiredFile (Join-Path $uiSource 'README_RU.md') $uiStage
Copy-RequiredFile `
    (Join-Path $repoRoot 'build\rc1_samples\sample_ui_transaction.dll') `
    (Join-Path $uiStage 'bin\windows-x86\sample_ui_transaction.dll')

$vehicleStage = New-SampleStage 'sample.vehicle_cosmetic'
$vehicleSource = Join-Path $repoRoot 'examples\sample_vehicle_cosmetic'
[System.IO.Directory]::CreateDirectory((Join-Path $vehicleStage 'skin')) | Out-Null
Copy-RequiredFile (Join-Path $vehicleSource 'manifest.json') $vehicleStage
Copy-RequiredFile (Join-Path $vehicleSource 'README.md') $vehicleStage
Copy-RequiredFile (Join-Path $vehicleSource 'README_RU.md') $vehicleStage
Copy-RequiredFile (Join-Path $vehicleSource 'skin\skin.json') (Join-Path $vehicleStage 'skin')
Copy-RequiredFile `
    (Join-Path $vehicleSource 'skin\R110_Object_260.material.yaml') `
    (Join-Path $vehicleStage 'skin')
Copy-RequiredFile `
    (Join-Path $repoRoot 'build\rc1_samples\sample_vehicle_cosmetic.dll') `
    (Join-Path $vehicleStage 'bin\windows-x86\sample_vehicle_cosmetic.dll')
$clientAssets = @{
    'Data\3d\Tanks\USSR\T-34-85.sc2.dvpl' = 'T-34-85.sc2.dvpl'
    'Data\3d\Tanks\USSR\T-34-85.scg.dvpl' = 'T-34-85.scg.dvpl'
    'Data\3d\Tanks\USSR\images\T-34-85.dx11.dds.dvpl' = 'T-34-85.dx11.dds.dvpl'
    'Data\3d\Tanks\USSR\images\T-34-85_NM.dx11.dds.dvpl' = 'T-34-85_NM.dx11.dds.dvpl'
}
foreach ($entry in $clientAssets.GetEnumerator()) {
    Copy-RequiredFile `
        (Join-Path $GameDirectory $entry.Key) `
        (Join-Path $vehicleStage ('skin\' + $entry.Value))
}

$cameraStage = New-SampleStage 'sample.camera_render'
$cameraSource = Join-Path $repoRoot 'examples\sample_camera_render'
Copy-RequiredFile (Join-Path $cameraSource 'manifest.json') $cameraStage
Copy-RequiredFile (Join-Path $cameraSource 'README.md') $cameraStage
Copy-RequiredFile (Join-Path $cameraSource 'README_RU.md') $cameraStage
Copy-RequiredFile `
    (Join-Path $repoRoot 'build\rc1_samples\sample_camera_render.dll') `
    (Join-Path $cameraStage 'bin\windows-x86\sample_camera_render.dll')

$packages = @(
    (Pack-Sample $uiStage 'sample.ui_transaction'),
    (Pack-Sample $vehicleStage 'sample.vehicle_cosmetic'),
    (Pack-Sample $cameraStage 'sample.camera_render')
)
$packages | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    [PSCustomObject]@{
        Package = $item.FullName
        Bytes = $item.Length
        Sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
} | Format-Table -AutoSize
