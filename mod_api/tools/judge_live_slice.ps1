<#
.SYNOPSIS
    Applies the live-run verdict to a loader-log slice plus the baselines that
    run_live_evidence.ps1 recorded before the client started.

.DESCRIPTION
    Five gates. All five must hold for a CLEAN verdict.

      1. slice_clean          zero bad lines in the evidence text
      2. process_exited       the client actually exited
      3. marker_absent        mods\cache\runtime_session.marker is gone
      4. no_new_crash_dumps     no dump file appeared that was not there before
      5. exit_code_not_a_crash  the client did not exit with an NTSTATUS fault
      5. positives_present    every required positive pattern was found

    Gates 1-4 are the original four-part contract. Gate 5 exists because the
    other four all pass on an all-quiet log where nothing was ever loaded: a run
    that proves nothing must FAIL, not pass. That distinction is the point of
    this script, so -RequirePattern is mandatory and may not be empty.

.OUTPUTS
    Exit 0  CLEAN.
    Exit 1  DIRTY. At least one gate failed; the offending lines are printed.
    Exit 2  The judge could not run (bad or missing inputs). Not a verdict.

.EXAMPLE
    judge_live_slice.ps1 -ParameterFile <run>\judge_request.json

.EXAMPLE
    judge_live_slice.ps1 `
      -SlicePath  '<run>\loader_slice.log' `
      -MarkerPath 'C:\...\World of Tanks Blitz\mods\cache\runtime_session.marker' `
      -ProcessExited true `
      -BaselineCrashDumpCount 10 `
      -RequirePattern '\[loader\] BlitzForge live loader starting'
#>

# PositionalBinding = $false is load-bearing: under `powershell -File`, only the
# first value of an array parameter binds and the rest silently fall through to
# the next positional parameter. With positional binding off that misuse is a
# hard error. Multi-value arguments must be comma-separated. Regex parameters
# are never comma-split, because a comma is legal inside a regex.
[CmdletBinding(PositionalBinding = $false)]
param(
    # JSON file whose properties supply any parameter not given on the command
    # line. run_live_evidence.ps1 uses this so regex metacharacters survive
    # verbatim and the exact judge inputs stay inside the evidence directory.
    [string]$ParameterFile = '',

    # The loader-log slice. Bad lines and positives are both searched here.
    [string]$SlicePath = '',

    # Further evidence text searched for BOTH bad lines and positives, e.g. the
    # per-run wotb_mod.log copy that carries "=== wotb_mod proxy loaded ===".
    [string[]]$AdditionalEvidenceFile = @(),

    # Path that must NOT exist after a clean run.
    [string]$MarkerPath = '',

    # Seconds to wait for the marker to disappear before failing gate 3. The
    # runtime clears mods\cache\runtime_session.marker several seconds AFTER the
    # process handle signals exit, so an instantaneous check yields a false
    # DIRTY. Zero disables the wait.
    [int]$MarkerSettleSeconds = 30,

    # 'true' or 'false'. Only the runner knows this; the judge cannot re-derive
    # it, so an unset value is a hard input error, never an assumed pass.
    [string]$ProcessExited = '',

    # Crash dump count recorded BEFORE the client started. -1 means unset.
    #
    # Kept for compatibility and for the printed summary, but it is NOT what
    # gate 4 decides on. Counting cannot detect a crash: Windows Error
    # Reporting caps the CrashDumps folder (DumpCount, default 10) and rotates
    # it, so once it is full the count after a crash is identical to the count
    # before. That is exactly the state this machine was in when a client
    # crashed with 0xC0000005, two fresh dumps were written, and the count
    # stayed at 10 - so the gate passed and the run was reported CLEAN.
    [int]$BaselineCrashDumpCount = -1,

    # The dump file NAMES recorded BEFORE the client started. This is what gate
    # 4 actually compares, because identity survives rotation where a count
    # does not. Empty means the caller did not supply them, which is itself
    # reported rather than silently treated as "no crash".
    [string[]]$BaselineCrashDumpName = @(),

    # The client's process exit code, as an unsigned 32-bit value in string
    # form, or '' when the runner could not obtain one. A crash is visible here
    # even when no dump is written at all - a Windows unhandled exception exits
    # with the NTSTATUS itself, e.g. 0xC0000005 for an access violation - so
    # this is an independent second witness and not a duplicate of gate 4.
    [string]$ProcessExitCode = '',

    [string]$CrashDumpDirectory = (Join-Path $env:LOCALAPPDATA 'CrashDumps'),

    [string]$CrashDumpFilter = 'wotblitz*.dmp',

    # Positive patterns the run must prove. Mandatory and non-empty.
    [string[]]$RequirePattern = @(),

    [string]$BadLinePattern = '\[(error|fatal)\]|safe-mode=active|package blocked|manifest refused|instruction budget|script disabled|callback failure|access violation|load complete.*failed=[1-9]',

    # How many offending lines to print in full. The count is always exact.
    [int]$MaxOffendingLines = 50,

    # Optional machine-readable verdict.
    [string]$JsonOutputPath = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Exit-WotbJudgeInputError {
    param([Parameter(Mandatory = $true)][string]$Message)
    Write-Host ''
    Write-Host "INPUT ERROR: $Message"
    Write-Host 'The judge produced no verdict. Exit code 2 is not "dirty"; it means the check never ran.'
    exit 2
}

# ---------------------------------------------------------------------------
# Merge -ParameterFile under anything given explicitly on the command line.
# ---------------------------------------------------------------------------
if (-not [string]::IsNullOrWhiteSpace($ParameterFile)) {
    if (-not [System.IO.File]::Exists($ParameterFile)) {
        Exit-WotbJudgeInputError "Parameter file does not exist: $ParameterFile"
    }
    $fromFile = $null
    try {
        $fromFile = Get-Content -LiteralPath $ParameterFile -Raw | ConvertFrom-Json
    } catch {
        Exit-WotbJudgeInputError "Parameter file is not valid JSON: $ParameterFile"
    }
    foreach ($property in @($fromFile.PSObject.Properties)) {
        $name = [string]$property.Name
        if ($name -eq 'ParameterFile') { continue }
        if ($PSBoundParameters.ContainsKey($name)) { continue }
        switch ($name) {
            'SlicePath'              { $SlicePath = [string]$property.Value }
            'AdditionalEvidenceFile' { $AdditionalEvidenceFile = @($property.Value | ForEach-Object { [string]$_ }) }
            'MarkerPath'             { $MarkerPath = [string]$property.Value }
            'MarkerSettleSeconds'    { $MarkerSettleSeconds = [int]$property.Value }
            'ProcessExited'          { $ProcessExited = [string]$property.Value }
            'BaselineCrashDumpCount' { $BaselineCrashDumpCount = [int]$property.Value }
            'BaselineCrashDumpName'  { $BaselineCrashDumpName = @($property.Value) }
            'ProcessExitCode'        { $ProcessExitCode = [string]$property.Value }
            'CrashDumpDirectory'     { $CrashDumpDirectory = [string]$property.Value }
            'CrashDumpFilter'        { $CrashDumpFilter = [string]$property.Value }
            'RequirePattern'         { $RequirePattern = @($property.Value | ForEach-Object { [string]$_ }) }
            'BadLinePattern'         { $BadLinePattern = [string]$property.Value }
            'MaxOffendingLines'      { $MaxOffendingLines = [int]$property.Value }
            'JsonOutputPath'         { $JsonOutputPath = [string]$property.Value }
            default {
                Exit-WotbJudgeInputError "Parameter file contains an unknown key '$name'."
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Input validation. Everything here is a usage error, not a verdict.
# ---------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($SlicePath)) {
    Exit-WotbJudgeInputError '-SlicePath is required.'
}
$slicePathFull = [System.IO.Path]::GetFullPath($SlicePath)
if (-not [System.IO.File]::Exists($slicePathFull)) {
    Exit-WotbJudgeInputError "Slice file does not exist: $slicePathFull"
}
if ([string]::IsNullOrWhiteSpace($MarkerPath)) {
    Exit-WotbJudgeInputError '-MarkerPath is required (usually <game root>\mods\cache\runtime_session.marker).'
}
$markerPathFull = [System.IO.Path]::GetFullPath($MarkerPath)
if ($ProcessExited -notin @('true', 'false')) {
    Exit-WotbJudgeInputError "-ProcessExited must be 'true' or 'false'. It was '$ProcessExited'. Only the runner knows whether the client exited; the judge will not guess."
}
if ($BaselineCrashDumpCount -lt 0) {
    Exit-WotbJudgeInputError '-BaselineCrashDumpCount is required and must be >= 0. It has to be recorded BEFORE the client starts.'
}
$requirePatterns = @($RequirePattern | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($requirePatterns.Count -eq 0) {
    Exit-WotbJudgeInputError '-RequirePattern is required and may not be empty. An all-quiet log with no mod loaded satisfies every other gate; without a positive assertion a run that proves nothing would pass as clean.'
}

$badRegex = $null
try {
    $badRegex = [regex]::new($BadLinePattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
} catch {
    Exit-WotbJudgeInputError "-BadLinePattern is not a valid regex: $($_.Exception.Message)"
}
$positiveRegexes = New-Object 'System.Collections.Generic.List[object]'
foreach ($pattern in $requirePatterns) {
    try {
        $positiveRegexes.Add([PSCustomObject]@{
                Pattern = $pattern
                Regex   = [regex]::new($pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
                Found   = $false
                Source  = ''
                Line    = 0
                Text    = ''
            })
    } catch {
        Exit-WotbJudgeInputError "Required pattern is not a valid regex: $pattern -- $($_.Exception.Message)"
    }
}

$evidenceFiles = New-Object 'System.Collections.Generic.List[string]'
$evidenceFiles.Add($slicePathFull)
foreach ($extra in @($AdditionalEvidenceFile)) {
    if ([string]::IsNullOrWhiteSpace($extra)) { continue }
    $full = [System.IO.Path]::GetFullPath($extra)
    if (-not [System.IO.File]::Exists($full)) {
        Exit-WotbJudgeInputError "Additional evidence file does not exist: $full"
    }
    if (-not $evidenceFiles.Contains($full)) { $evidenceFiles.Add($full) }
}

# ---------------------------------------------------------------------------
# Scan.
# ---------------------------------------------------------------------------
$offending = New-Object 'System.Collections.Generic.List[object]'
$totalLines = 0
$emptyFiles = New-Object 'System.Collections.Generic.List[string]'

foreach ($file in $evidenceFiles) {
    $lines = @()
    try {
        $lines = [System.IO.File]::ReadAllLines($file)
    } catch {
        Exit-WotbJudgeInputError "Evidence file could not be read: $file -- $($_.Exception.Message)"
    }
    if ($lines.Count -eq 0) { $emptyFiles.Add($file) }
    $name = [System.IO.Path]::GetFileName($file)
    for ($index = 0; $index -lt $lines.Count; ++$index) {
        ++$totalLines
        $text = [string]$lines[$index]
        if ($badRegex.IsMatch($text)) {
            $offending.Add([PSCustomObject]@{
                    file = $name
                    line = $index + 1
                    text = $text
                })
        }
        foreach ($positive in $positiveRegexes) {
            if ($positive.Found) { continue }
            if ($positive.Regex.IsMatch($text)) {
                $positive.Found = $true
                $positive.Source = $name
                $positive.Line = $index + 1
                $positive.Text = $text
            }
        }
    }
}

$crashDumpsAfter = @()
if ([System.IO.Directory]::Exists($CrashDumpDirectory)) {
    $crashDumpsAfter = @(Get-ChildItem -LiteralPath $CrashDumpDirectory -Filter $CrashDumpFilter -File -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Name | Sort-Object)
}
function Test-WotbJudgeMarker {
    param([Parameter(Mandatory = $true)][string]$Path)
    return ([System.IO.File]::Exists($Path) -or [System.IO.Directory]::Exists($Path))
}

# Poll rather than sample once: the runtime renames the marker to
# runtime_session.marker.ended-<stamp> several seconds after the client's
# process handle signals exit.
$markerPresent = Test-WotbJudgeMarker -Path $markerPathFull
$markerSettleWaited = 0.0
if ($markerPresent -and $MarkerSettleSeconds -gt 0) {
    $settleStart = [DateTime]::UtcNow
    while ($markerPresent -and
        ([DateTime]::UtcNow - $settleStart).TotalSeconds -lt $MarkerSettleSeconds) {
        Start-Sleep -Milliseconds 500
        $markerPresent = Test-WotbJudgeMarker -Path $markerPathFull
    }
    $markerSettleWaited = [Math]::Round(([DateTime]::UtcNow - $settleStart).TotalSeconds, 1)
}
$missingPositives = @($positiveRegexes | Where-Object { -not $_.Found })

# Compare dump IDENTITY, not count. See the note on -BaselineCrashDumpCount:
# the folder rotates, so a crash can leave the count untouched. Any name
# present now that was not present before is a new dump.
$baselineNames = @($BaselineCrashDumpName | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$newDumps = @()
if ($baselineNames.Count -gt 0 -or $crashDumpsAfter.Count -gt 0) {
    $before = @{}
    foreach ($n in $baselineNames) { $before[$n] = $true }
    $newDumps = @($crashDumpsAfter | Where-Object { -not $before.ContainsKey($_) })
}
# Without baseline names the gate cannot answer honestly. It then falls back to
# the count, which is weak, and says so in the report rather than pretending.
$dumpIdentityKnown = ($baselineNames.Count -gt 0)
$noNewDumps = if ($dumpIdentityKnown) { $newDumps.Count -eq 0 }
              else { $crashDumpsAfter.Count -le $BaselineCrashDumpCount }

# An unhandled Windows exception exits with the NTSTATUS itself. Treat the
# reserved 0xC0000000-0xCFFFFFFF failure range as a crash; a normal exit is 0,
# and the client is also known to answer 1 on a WM_CLOSE during load.
$exitCodeKnown = -not [string]::IsNullOrWhiteSpace($ProcessExitCode)
$exitLooksLikeCrash = $false
$exitCodeValue = [uint32]0
if ($exitCodeKnown) {
    try {
        # Decimal literals, not 0xC0000000u: the 'u' numeric suffix is
        # PowerShell 7 only and this script must also parse under Windows
        # PowerShell 5.1, which is what the runner invokes it with.
        # 3221225472 = 0xC0000000, 3489660927 = 0xCFFFFFFF.
        $exitCodeValue = [uint32]([int64]$ProcessExitCode -band 4294967295)
        $exitLooksLikeCrash = ($exitCodeValue -ge [uint32]3221225472 -and
                               $exitCodeValue -le [uint32]3489660927)
    } catch {
        $exitCodeKnown = $false
    }
}

$gates = [ordered]@{
    slice_clean           = ($offending.Count -eq 0)
    process_exited        = ($ProcessExited -eq 'true')
    marker_absent         = (-not $markerPresent)
    no_new_crash_dumps    = $noNewDumps
    exit_code_not_a_crash = (-not $exitLooksLikeCrash)
    positives_present     = ($missingPositives.Count -eq 0)
}
$clean = $true
foreach ($value in $gates.Values) { if (-not $value) { $clean = $false } }

# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------
# NOTE: never write @($someList) where the variable is a
# System.Collections.Generic.List[object]. On PowerShell 7.6.5 the array
# subexpression operator throws "Argument types do not match" for that exact
# type (List[string] is fine). Use .ToArray() instead.
$evidenceFileArray = $evidenceFiles.ToArray()
$emptyFileArray = $emptyFiles.ToArray()
$offendingArray = $offending.ToArray()
$positiveArray = $positiveRegexes.ToArray()

Write-Host '=== live slice verdict ==========================================='
Write-Host "  slice            : $slicePathFull"
foreach ($file in $evidenceFileArray) {
    if ($file -eq $slicePathFull) { continue }
    Write-Host "  also scanned     : $file"
}
Write-Host "  lines scanned    : $totalLines"
Write-Host "  bad-line regex   : $BadLinePattern"
Write-Host "  marker path      : $markerPathFull"
Write-Host "  crash dumps      : baseline $BaselineCrashDumpCount, now $($crashDumpsAfter.Count) ($CrashDumpFilter in $CrashDumpDirectory)"
if (-not $dumpIdentityKnown) {
    Write-Host "  NOTE             : no baseline dump names supplied; gate 4 fell back to counting, which cannot see a crash once the folder has rotated."
}
if ($exitCodeKnown) {
    Write-Host ("  client exit code : 0x{0:X8}" -f $exitCodeValue)
} else {
    Write-Host "  client exit code : (not supplied)"
}
Write-Host ''

foreach ($file in $emptyFileArray) {
    Write-Host "  NOTE: evidence file is empty: $file"
}
if ($emptyFileArray.Count -gt 0) { Write-Host '' }

Write-Host "gate 1  slice_clean            : $(if ($gates.slice_clean) { 'PASS' } else { "FAIL ($($offending.Count) bad line(s))" })"
if ($offending.Count -gt 0) {
    $shown = 0
    foreach ($item in $offendingArray) {
        if ($shown -ge $MaxOffendingLines) {
            Write-Host "        ... $($offending.Count - $shown) further bad line(s) suppressed; see the slice."
            break
        }
        Write-Host "        $($item.file):$($item.line): $($item.text)"
        ++$shown
    }
}

Write-Host "gate 2  process_exited         : $(if ($gates.process_exited) { 'PASS' } else { 'FAIL (the client did not exit; never force-kill it, close it by hand)' })"
Write-Host "gate 3  marker_absent          : $(if ($gates.marker_absent) { "PASS (settled after ${markerSettleWaited}s)" } else { "FAIL (runtime_session.marker survived ${markerSettleWaited}s of settle budget; the next launch would enter safe mode)" })"
Write-Host "gate 4  no_new_crash_dumps     : $(if ($gates.no_new_crash_dumps) { 'PASS' } else { "FAIL ($($newDumps.Count) new dump(s))" })"
foreach ($dump in $newDumps) { Write-Host "        new dump: $dump" }
Write-Host "gate 5  exit_code_not_a_crash  : $(if ($gates.exit_code_not_a_crash) { 'PASS' } else { "FAIL (exit 0x{0:X8} is an NTSTATUS fault)" -f $exitCodeValue })"
Write-Host "gate 6  positives_present      : $(if ($gates.positives_present) { 'PASS' } else { "FAIL ($($missingPositives.Count) of $($positiveArray.Count) required pattern(s) never appeared)" })"
foreach ($positive in $positiveArray) {
    if ($positive.Found) {
        Write-Host "        found   $($positive.Pattern)"
        Write-Host "                -> $($positive.Source):$($positive.Line): $($positive.Text)"
    } else {
        Write-Host "        MISSING $($positive.Pattern)"
    }
}

Write-Host ''
if ($clean) {
    Write-Host 'VERDICT: CLEAN'
} else {
    Write-Host 'VERDICT: DIRTY'
}
Write-Host '=================================================================='

if (-not [string]::IsNullOrWhiteSpace($JsonOutputPath)) {
    # The verdict is already decided. A failure to serialise it is a reporting
    # problem and must never turn CLEAN into a non-zero exit code.
    try {
        $report = [ordered]@{
            schema         = 'wotbmod.live-slice-verdict/v1'
            judged_at_utc  = [DateTime]::UtcNow.ToString('o')
            verdict        = $(if ($clean) { 'CLEAN' } else { 'DIRTY' })
            gates          = $gates
            slice          = $slicePathFull
            evidence_files = $evidenceFileArray
            lines_scanned  = $totalLines
            empty_files    = $emptyFileArray
            bad_line_regex = $BadLinePattern
            bad_lines      = $offendingArray
            marker_path           = $markerPathFull
            marker_present        = $markerPresent
            marker_settle_budget  = $MarkerSettleSeconds
            marker_settle_waited  = $markerSettleWaited
            crash_dumps    = [ordered]@{
                directory = $CrashDumpDirectory
                filter    = $CrashDumpFilter
                baseline  = $BaselineCrashDumpCount
                current   = $crashDumpsAfter.Count
                names     = @($crashDumpsAfter)
            }
            required       = @($positiveArray | ForEach-Object {
                    [ordered]@{
                        pattern = $_.Pattern
                        found   = $_.Found
                        source  = $_.Source
                        line    = $_.Line
                        text    = $_.Text
                    }
                })
        }
        $parent = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($JsonOutputPath))
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            [System.IO.Directory]::CreateDirectory($parent) | Out-Null
        }
        $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $JsonOutputPath -Encoding UTF8
        Write-Host "Verdict JSON: $JsonOutputPath"
    } catch {
        Write-Host "WARNING: the verdict JSON could not be written ($($_.Exception.Message)). The verdict above still stands."
    }
}

if ($clean) { exit 0 } else { exit 1 }
