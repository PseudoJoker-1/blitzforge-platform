<#
.SYNOPSIS
    Runs the real WoT Blitz client once, captures screenshots and a loader-log
    slice, shuts the client down politely, and hands the evidence to
    judge_live_slice.ps1 for a verdict.

.DESCRIPTION
    This is the checked-in version of the live-evidence procedure that used to
    be re-typed inline for every run. It performs, in order:

      1. Preflight. Nothing is touched until every precondition holds.
      2. Baselines. Loader-log byte length, wotb_mod.log byte length and the
         CrashDumps inventory are recorded BEFORE the client starts.
      3. Hygiene. A stale mods\cache\runtime_session.marker is MOVED to a
         timestamped backup, never deleted.
      4. Staging. The replay is copied into a fresh GUID temp directory so the
         client cannot mutate the operator's original.
      5. Launch. wotblitz.exe is started directly (not through Steam) with the
         replay as a bare positional argument.
      6. Schedule. DPI-aware desktop captures and optional key sends fire at
         caller-specified moments, anchored to launch or to readiness.
      7. Shutdown. WM_CLOSE via CloseMainWindow + WaitForExit. Never a kill.
      8. Slice. The loader log is sliced by byte offset (it is ~144 MB and
         append-only; it is never read whole).
      9. Verdict. judge_live_slice.ps1 is invoked as a child process.

    DO NOT run this against a half-built tree. It exercises whatever loader is
    currently installed in the game root.

.NOTES
    Evidence is deliberately written OUTSIDE mod_api\build, because the
    documented clean-build step is `Remove-Item -Recurse -Force mod_api\build`
    and that is exactly how the previous run's screenshots were destroyed.
    The default output root is %LOCALAPPDATA%\wotbmod\live_evidence.
    Writing into mod_api\build is refused outright.
#>

# PositionalBinding = $false is load-bearing. Invoked as
# `powershell -File run_live_evidence.ps1 -Captures ready ready+6`, PowerShell
# binds only the FIRST value to -Captures and lets the rest fall through to
# whatever positional parameter comes next -- silently landing 'ready+6' in
# -Replay or -OutputDirectory. With positional binding off, that misuse is a
# hard error instead. Multi-value arguments must be comma-separated:
#   -Captures ready,ready+6,preclose
# A single comma-separated STRING is also accepted for -Captures and -Keys.
[CmdletBinding(PositionalBinding = $false)]
param(
    # Game installation root. Must contain wotblitz.exe.
    [string]$GameRoot = (Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\World of Tanks Blitz'),

    # Offline local replay to load. Omit to launch straight into the hangar.
    [string]$Replay = '',

    # Evidence root. A per-run subdirectory is created inside it.
    [string]$OutputDirectory = (Join-Path $env:LOCALAPPDATA 'wotbmod\live_evidence'),

    # Capture moments. See Get-WotbLiveScheduleEntry for the grammar.
    # Examples: 'ready', 'ready+5', 'launch+30', '45', 'preclose',
    #           'ready+8:battle-hud'
    [string[]]$Captures = @('launch+25', 'ready', 'ready+6', 'preclose'),

    # Key sends. Same moment grammar after '@'.
    # Examples: 'F8', 'F8@ready+3', '0x77@launch+40'
    [string[]]$Keys = @(),

    # Hard wall-clock budget from launch to the start of shutdown.
    [int]$TimeoutSeconds = 300,

    # How long to wait for the readiness pattern before giving up on
    # ready-anchored schedule items. The run still completes and is judged.
    [int]$ReadyTimeoutSeconds = 180,

    # Readiness signal in the loader log. Default depends on -Replay.
    # NOTE: the replay default 'SetBattleState...LOADED' is reconstructed from a
    # prior run transcript and is NOT verified against any checked-in source.
    [string]$ReadyPattern = '',

    # Seconds to keep running after the last scheduled item before shutdown.
    [int]$PostScheduleSeconds = 3,

    # How long to let the runtime finish clearing mods\cache\runtime_session.marker
    # AFTER the process handle signals exit. The marker cleanup lags process exit
    # by several seconds: an observed run whose handle signalled at 22:44:22 had
    # its marker renamed to runtime_session.marker.ended-<stamp> only at 22:44:54,
    # with no further launch in between. Checking the marker the instant
    # WaitForExit returns therefore produces a false DIRTY verdict.
    [int]$MarkerSettleSeconds = 30,

    # 'Window' blits the client's window rect out of the desktop DC.
    # 'Screen' blits the whole virtual desktop.
    [ValidateSet('Window', 'Screen')]
    [string]$CaptureMode = 'Window',

    # Bad-line regex handed to the judge. Case-insensitive.
    [string]$BadLinePattern = '\[(error|fatal)\]|safe-mode=active|package blocked|manifest refused|instruction budget|script disabled|callback failure|access violation|load complete.*failed=[1-9]',

    # Positive patterns the run must prove. Combined with -RequirePreset.
    [string[]]$RequirePattern = @(),

    # Named bundles of verified positive patterns: loader, native-bindings,
    # lua-host, battle-unverified. See $script:RequirePresets for the patterns
    # and their emitters. Comma-separated. Validated by hand rather than with
    # [ValidateSet] because under `powershell -File` a ValidateSet array
    # parameter cannot accept more than one value at all.
    [string[]]$RequirePreset = @('loader'),

    # Emit the plan and the resolved preflight, then stop. Launches nothing.
    [switch]$DryRun,

    # Allow a game root without steam_appid.txt (the client may then demand the
    # Steam launcher and the run will stall).
    [switch]$AllowMissingSteamAppId,

    # Produce evidence but skip the verdict step.
    [switch]$SkipJudge,

    # Keep the staged replay copy instead of best-effort deleting it.
    [switch]$KeepStagedReplay
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Native interop. SetProcessDPIAware() must run before ANY bounds or rect read
# in this process: on a 125% desktop a DPI-unaware process reports 1536x864 and
# silently captures a downscaled frame. The loader independently logs
# "DAVA UI viewport=1536x864 backbuffer=1920x1080 dpi=120" on this machine.
# ---------------------------------------------------------------------------
if (-not ('WotbLive.Native' -as [type])) {
    Add-Type -Namespace 'WotbLive' -Name 'Native' -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

[DllImport("user32.dll", SetLastError = true)] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll", SetLastError = true)] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll", SetLastError = true)] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll", SetLastError = true)] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
'@
}
Add-Type -AssemblyName System.Drawing

$script:DpiAware = [WotbLive.Native]::SetProcessDPIAware()

$script:SwRestore = 9
$script:KeyEventKeyUp = [uint32]2
$script:SmXVirtualScreen = 76
$script:SmYVirtualScreen = 77
$script:SmCxVirtualScreen = 78
$script:SmCyVirtualScreen = 79

# Verified positive patterns and their emitters. File:line references are into
# this repository at the time this script was written.
$script:RequirePresets = @{
    # proxy_dll\dllmain.cpp:255 (wotb_mod.log, reopened "w" per game process)
    # mod_api\loader\wotb_mod_loader.cpp:3479 (wotb_mod_loader.log)
    # mod_api\src\wotb_mod_runtime.cpp:8125 (routed to the loader log)
    'loader'            = @(
        '=== wotb_mod proxy loaded ==='
        '\[loader\] BlitzForge live loader starting'
        'load complete package-discovered=\d+ package-loaded=[1-9]\d*'
    )
    # mod_api\loader\v3_native_bindings.cpp:868-872 via NativeBindingsLog
    'native-bindings'   = @(
        '\[native-v3\] V3 native binding id=\S+ kind=\S+ rva=0x[0-9A-Fa-f]{8} state=BOUND'
    )
    # mod_api\loader\lua\lua_host_mod.cpp:767 through the "[v3:%s] %s" format at
    # mod_api\src\wotb_mod_runtime.cpp:7355
    'lua-host'          = @(
        '\[v3:lua\.host\] \S+: installed Lua mod loaded'
    )
    # UNVERIFIED. Reconstructed from a prior run transcript only; no checked-in
    # source emits this. The name says so on purpose.
    'battle-unverified' = @(
        'SetBattleState[^\r\n]*LOADED'
    )
}

$script:VirtualKeys = @{
    'F1' = 0x70; 'F2' = 0x71; 'F3' = 0x72; 'F4' = 0x73; 'F5' = 0x74; 'F6' = 0x75
    'F7' = 0x76; 'F8' = 0x77; 'F9' = 0x78; 'F10' = 0x79; 'F11' = 0x7A; 'F12' = 0x7B
    'ESC' = 0x1B; 'ESCAPE' = 0x1B; 'ENTER' = 0x0D; 'RETURN' = 0x0D
    'SPACE' = 0x20; 'TAB' = 0x09; 'BACKSPACE' = 0x08
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function Write-WotbLiveStep {
    param([Parameter(Mandatory = $true)][string]$Message)
    $stamp = [DateTime]::UtcNow.ToString('HH:mm:ss.fff')
    Write-Host "[$stamp] $Message"
}

function Resolve-WotbLiveFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-WotbLivePathInside {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $fullRoot = (Resolve-WotbLiveFullPath $Root).TrimEnd('\', '/')
    $fullPath = Resolve-WotbLiveFullPath $Path
    if ($fullPath.Equals($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
    $prefix = $fullRoot + [System.IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

# Accepts both -Captures a,b,c and -Captures 'a,b,c'. Deliberately NOT applied
# to regex parameters: a comma is legal inside a regex, e.g. \d{2,3}.
function Expand-WotbLiveTokenList {
    param([AllowNull()][string[]]$Value)
    $result = New-Object 'System.Collections.Generic.List[string]'
    foreach ($item in @($Value)) {
        if ([string]::IsNullOrWhiteSpace($item)) { continue }
        foreach ($part in ($item -split '[,;]')) {
            $trimmed = $part.Trim()
            if (-not [string]::IsNullOrWhiteSpace($trimmed)) { $result.Add($trimmed) }
        }
    }
    return $result.ToArray()
}

function Get-WotbLiveVirtualKey {
    param([Parameter(Mandatory = $true)][string]$Name)
    $token = $Name.Trim()
    if ($token -match '^(?i)0x([0-9a-f]{1,2})$') {
        return [int]('0x' + $Matches[1])
    }
    $upper = $token.ToUpperInvariant()
    if ($script:VirtualKeys.ContainsKey($upper)) { return [int]$script:VirtualKeys[$upper] }
    if ($upper.Length -eq 1 -and $upper -match '^[A-Z0-9]$') {
        return [int][char]$upper
    }
    throw "Unknown key token '$Name'. Use F1..F12, a single A-Z/0-9 character, a named key, or a 0xNN virtual-key code."
}

<#
Moment grammar, shared by -Captures and the part of -Keys after '@':

    ready                 the instant readiness is detected
    ready+<n>             n seconds after readiness
    launch | launch+<n>   at launch / n seconds after launch
    t=<n> | <n> | <n>s    same as launch+<n>
    preclose              immediately before WM_CLOSE is sent

An optional ':<label>' suffix names the artifact.
#>
function Get-WotbLiveScheduleEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Token,
        [Parameter(Mandatory = $true)][string]$Kind
    )
    $text = $Token.Trim()
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Empty $Kind schedule token."
    }
    $label = ''
    $colon = $text.IndexOf(':')
    if ($colon -ge 0) {
        $label = $text.Substring($colon + 1).Trim()
        $text = $text.Substring(0, $colon).Trim()
    }

    $anchor = ''
    $offset = 0.0
    if ($text -match '^(?i)preclose$') {
        $anchor = 'preclose'
    } elseif ($text -match '^(?i)ready$') {
        $anchor = 'ready'
    } elseif ($text -match '^(?i)ready\s*\+\s*([0-9]+(?:\.[0-9]+)?)s?$') {
        $anchor = 'ready'
        $offset = [double]$Matches[1]
    } elseif ($text -match '^(?i)launch$') {
        $anchor = 'launch'
    } elseif ($text -match '^(?i)launch\s*\+\s*([0-9]+(?:\.[0-9]+)?)s?$') {
        $anchor = 'launch'
        $offset = [double]$Matches[1]
    } elseif ($text -match '^(?i)(?:t\s*=\s*)?([0-9]+(?:\.[0-9]+)?)s?$') {
        $anchor = 'launch'
        $offset = [double]$Matches[1]
    } else {
        throw "Unparsable $Kind moment '$Token'. Use ready | ready+N | launch+N | N | t=N | preclose, with an optional ':label'."
    }

    if ([string]::IsNullOrWhiteSpace($label)) {
        $label = ($anchor + $(if ($offset -gt 0) { '+' + $offset.ToString([System.Globalization.CultureInfo]::InvariantCulture) } else { '' }))
    }
    $label = [regex]::Replace($label, '[^A-Za-z0-9._+-]', '-')

    return [PSCustomObject]@{
        Kind     = $Kind
        Token    = $Token
        Anchor   = $anchor
        Offset   = $offset
        Label    = $label
        Key      = ''
        KeyCode  = 0
        Fired    = $false
        Skipped  = $false
        Note     = ''
        FiredUtc = $null
    }
}

function Get-WotbLiveKeyEntry {
    param([Parameter(Mandatory = $true)][string]$Token)
    $text = $Token.Trim()
    $at = $text.IndexOf('@')
    $keyName = $text
    $moment = 'ready'
    if ($at -ge 0) {
        $keyName = $text.Substring(0, $at).Trim()
        $moment = $text.Substring($at + 1).Trim()
        if ([string]::IsNullOrWhiteSpace($moment)) { $moment = 'ready' }
    }
    $entry = Get-WotbLiveScheduleEntry -Token $moment -Kind 'key'
    $entry.Key = $keyName
    $entry.KeyCode = Get-WotbLiveVirtualKey -Name $keyName
    $entry.Label = ($keyName + '-at-' + $entry.Label)
    return $entry
}

function Get-WotbLiveFileLength {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not [System.IO.File]::Exists($Path)) { return [long](-1) }
    # Open shared: the client holds this file with FILE_SHARE_READ|FILE_SHARE_WRITE.
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    try { return [long]$stream.Length } finally { $stream.Dispose() }
}

function Read-WotbLiveLogDelta {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$Offset,
        [ref]$NewOffset
    )
    $NewOffset.Value = $Offset
    if (-not [System.IO.File]::Exists($Path)) { return '' }
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    try {
        $length = [long]$stream.Length
        if ($length -le $Offset) {
            $NewOffset.Value = $Offset
            return ''
        }
        [void]$stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $count = [int][Math]::Min([long]4194304, $length - $Offset)
        $buffer = New-Object byte[] $count
        $read = 0
        while ($read -lt $count) {
            $chunk = $stream.Read($buffer, $read, $count - $read)
            if ($chunk -le 0) { break }
            $read += $chunk
        }
        $NewOffset.Value = $Offset + $read
        if ($read -le 0) { return '' }
        return [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    } finally {
        $stream.Dispose()
    }
}

function Get-WotbLiveCrashDumpNames {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Filter
    )
    if (-not [System.IO.Directory]::Exists($Directory)) { return @() }
    return @(Get-ChildItem -LiteralPath $Directory -Filter $Filter -File -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Name | Sort-Object)
}

function Set-WotbLiveForeground {
    param(
        [Parameter(Mandatory = $true)][IntPtr]$Handle,
        [int]$Attempts = 6
    )
    if ($Handle -eq [IntPtr]::Zero) { return $false }
    for ($index = 0; $index -lt $Attempts; ++$index) {
        [void][WotbLive.Native]::ShowWindow($Handle, $script:SwRestore)
        [void][WotbLive.Native]::SetForegroundWindow($Handle)
        Start-Sleep -Milliseconds 180
        if ([WotbLive.Native]::GetForegroundWindow() -eq $Handle) { return $true }
    }
    return $false
}

function Save-WotbLiveCapture {
    param(
        [Parameter(Mandatory = $true)][IntPtr]$Handle,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Mode
    )
    $left = 0; $top = 0; $width = 0; $height = 0
    $rectSource = $Mode
    if ($Mode -eq 'Window' -and $Handle -ne [IntPtr]::Zero) {
        $rect = New-Object 'WotbLive.Native+RECT'
        if ([WotbLive.Native]::GetWindowRect($Handle, [ref]$rect)) {
            $left = $rect.Left; $top = $rect.Top
            $width = $rect.Right - $rect.Left
            $height = $rect.Bottom - $rect.Top
        }
    }
    if ($width -le 0 -or $height -le 0) {
        $rectSource = 'Screen'
        $left = [WotbLive.Native]::GetSystemMetrics($script:SmXVirtualScreen)
        $top = [WotbLive.Native]::GetSystemMetrics($script:SmYVirtualScreen)
        $width = [WotbLive.Native]::GetSystemMetrics($script:SmCxVirtualScreen)
        $height = [WotbLive.Native]::GetSystemMetrics($script:SmCyVirtualScreen)
    }
    if ($width -le 0 -or $height -le 0) {
        throw "Capture geometry is degenerate: ${width}x${height}."
    }

    $bitmap = New-Object System.Drawing.Bitmap(
        $width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            # Desktop-DC blit. PrintWindow is deliberately NOT used: it returns
            # black on a DXGI swapchain.
            $graphics.CopyFromScreen(
                $left, $top, 0, 0,
                (New-Object System.Drawing.Size($width, $height)),
                [System.Drawing.CopyPixelOperation]::SourceCopy)
        } finally { $graphics.Dispose() }

        [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

        # Cheap all-black detector. A DPI- or swapchain-related capture failure
        # produces a uniform black frame; recording this makes that visible in
        # run.json instead of only in a PNG nobody opens.
        $steps = 24
        $sum = 0.0
        $samples = 0
        $distinct = @{}
        for ($y = 0; $y -lt $steps; ++$y) {
            for ($x = 0; $x -lt $steps; ++$x) {
                $px = [int](($x + 0.5) * $width / $steps)
                $py = [int](($y + 0.5) * $height / $steps)
                if ($px -ge $width) { $px = $width - 1 }
                if ($py -ge $height) { $py = $height - 1 }
                $color = $bitmap.GetPixel($px, $py)
                $sum += (0.2126 * $color.R + 0.7152 * $color.G + 0.0722 * $color.B)
                ++$samples
                $distinct[$color.ToArgb()] = $true
            }
        }
        $mean = if ($samples -gt 0) { [Math]::Round($sum / $samples, 3) } else { 0.0 }
        return [PSCustomObject]@{
            path                   = $Path
            rect_source            = $rectSource
            left                   = $left
            top                    = $top
            width                  = $width
            height                 = $height
            mean_luma              = $mean
            distinct_sample_colors = $distinct.Count
            looks_blank            = ($distinct.Count -le 2)
        }
    } finally {
        $bitmap.Dispose()
    }
}

# ---------------------------------------------------------------------------
# Phase 1: preflight. Nothing on disk is touched here.
# ---------------------------------------------------------------------------
Write-WotbLiveStep 'Preflight.'

if (-not $script:DpiAware) {
    Write-Warning 'SetProcessDPIAware() returned false. It is already true for this process only if a manifest set it; captures may be downscaled.'
}

$gameRootPath = Resolve-WotbLiveFullPath $GameRoot
if (-not [System.IO.Directory]::Exists($gameRootPath)) {
    throw "Game root does not exist: $gameRootPath. Pass -GameRoot explicitly."
}
$gameExe = Join-Path $gameRootPath 'wotblitz.exe'
if (-not [System.IO.File]::Exists($gameExe)) {
    throw "wotblitz.exe was not found in the game root: $gameExe. This harness launches the client directly, not through Steam."
}
$steamAppId = Join-Path $gameRootPath 'steam_appid.txt'
if (-not [System.IO.File]::Exists($steamAppId)) {
    if (-not $AllowMissingSteamAppId) {
        throw "steam_appid.txt is missing next to wotblitz.exe: $steamAppId. Without it steam_api.dll cannot initialise outside the Steam launcher and a direct launch stalls. Create it containing 444200, or pass -AllowMissingSteamAppId."
    }
    Write-Warning "steam_appid.txt is missing; the client may demand the Steam launcher."
}

$loaderLogPath = Join-Path $gameRootPath 'wotb_mod_loader.log'
$proxyLogPath = Join-Path $gameRootPath 'wotb_mod.log'
$modsRoot = Join-Path $gameRootPath 'mods'
$cacheRoot = Join-Path $modsRoot 'cache'
$markerPath = Join-Path $cacheRoot 'runtime_session.marker'
$bindingReportPath = Join-Path $cacheRoot 'binding_pack_validation.json'
$modsIniPath = Join-Path $modsRoot 'mods.ini'
$crashDumpDirectory = Join-Path $env:LOCALAPPDATA 'CrashDumps'
$crashDumpFilter = 'wotblitz*.dmp'

$stagedReplay = ''
$replayFullPath = ''
if (-not [string]::IsNullOrWhiteSpace($Replay)) {
    $replayFullPath = Resolve-WotbLiveFullPath $Replay
    if (-not [System.IO.File]::Exists($replayFullPath)) {
        throw "Replay does not exist: $replayFullPath"
    }
    if ([System.IO.Path]::GetExtension($replayFullPath).ToLowerInvariant() -ne '.wotbreplay') {
        throw "Replay must be a .wotbreplay file: $replayFullPath"
    }
}

# Refuse to write evidence anywhere the documented clean-build step deletes.
$outputRoot = Resolve-WotbLiveFullPath $OutputDirectory
$modApiRoot = Resolve-WotbLiveFullPath (Join-Path $PSScriptRoot '..')
$modApiBuild = Join-Path $modApiRoot 'build'
if (Test-WotbLivePathInside -Root $modApiBuild -Path $outputRoot) {
    throw "-OutputDirectory is inside $modApiBuild. That directory is removed by the documented clean-build step (Remove-Item -Recurse -Force mod_api\build) and is how the last run's evidence was lost. Choose a location outside it."
}
if (Test-WotbLivePathInside -Root (Resolve-WotbLiveFullPath (Join-Path $modApiRoot '..')) -Path $outputRoot) {
    $ignored = Test-WotbLivePathInside -Root $modApiRoot -Path $outputRoot
    $note = if ($ignored) {
        'mod_api\.gitignore excludes live_evidence/ under mod_api, so name the directory live_evidence.'
    } else {
        'It is OUTSIDE mod_api, so mod_api\.gitignore does not cover it and the captures could be committed.'
    }
    Write-Warning "-OutputDirectory is inside the repository working tree. $note Prefer the default %LOCALAPPDATA%\wotbmod\live_evidence: these are screenshots of a logged-in client."
}

$judgeScript = Join-Path $PSScriptRoot 'judge_live_slice.ps1'
if (-not $SkipJudge -and -not [System.IO.File]::Exists($judgeScript)) {
    throw "judge_live_slice.ps1 was not found next to this script: $judgeScript. Pass -SkipJudge to produce evidence without a verdict."
}

$running = @(Get-Process -Name 'wotblitz' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    throw "wotblitz is already running (PID $(($running | ForEach-Object { $_.Id }) -join ', ')). Close the client and re-run. This harness never attaches to or kills an existing client."
}

if ($TimeoutSeconds -le 0) { throw '-TimeoutSeconds must be positive.' }
if ($ReadyTimeoutSeconds -le 0) { throw '-ReadyTimeoutSeconds must be positive.' }
if ($ReadyTimeoutSeconds -gt $TimeoutSeconds) {
    throw "-ReadyTimeoutSeconds ($ReadyTimeoutSeconds) exceeds -TimeoutSeconds ($TimeoutSeconds)."
}

if ([string]::IsNullOrWhiteSpace($ReadyPattern)) {
    $ReadyPattern = if ([string]::IsNullOrWhiteSpace($replayFullPath)) {
        'load complete package-discovered='
    } else {
        'SetBattleState[^\r\n]*LOADED'
    }
}
$readyRegex = [regex]::new($ReadyPattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

$schedule = New-Object 'System.Collections.Generic.List[object]'
foreach ($token in (Expand-WotbLiveTokenList -Value $Captures)) {
    $schedule.Add((Get-WotbLiveScheduleEntry -Token $token -Kind 'capture'))
}
foreach ($token in (Expand-WotbLiveTokenList -Value $Keys)) {
    $schedule.Add((Get-WotbLiveKeyEntry -Token $token))
}
if ($schedule.Count -eq 0) {
    throw 'Nothing scheduled: pass at least one -Captures or -Keys entry.'
}

$requiredPatterns = New-Object 'System.Collections.Generic.List[string]'
foreach ($preset in (Expand-WotbLiveTokenList -Value $RequirePreset)) {
    if (-not $script:RequirePresets.ContainsKey($preset)) {
        $valid = (($script:RequirePresets.Keys | Sort-Object) -join ', ')
        throw "Unknown -RequirePreset '$preset'. Valid presets: $valid."
    }
    foreach ($pattern in @($script:RequirePresets[$preset])) {
        if ([string]::IsNullOrWhiteSpace($pattern)) { continue }
        if (-not $requiredPatterns.Contains($pattern)) { $requiredPatterns.Add($pattern) }
    }
}
foreach ($pattern in @($RequirePattern)) {
    if ([string]::IsNullOrWhiteSpace($pattern)) { continue }
    if (-not $requiredPatterns.Contains($pattern)) { $requiredPatterns.Add($pattern) }
}
if (-not $SkipJudge -and $requiredPatterns.Count -eq 0) {
    throw 'No positive patterns were requested. A run that asserts nothing cannot produce a clean verdict; pass -RequirePreset and/or -RequirePattern, or -SkipJudge.'
}
foreach ($pattern in $requiredPatterns) {
    # Fail on a malformed pattern now, not after the client has been launched.
    [void][regex]::new($pattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
}
[void][regex]::new($BadLinePattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)

$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runDirectory = Join-Path $outputRoot $runId
$captureDirectory = Join-Path $runDirectory 'captures'

Write-Host ''
Write-Host '--- live evidence plan -------------------------------------------'
Write-Host "  game root       : $gameRootPath"
Write-Host "  replay          : $(if ($replayFullPath) { $replayFullPath } else { '(none - hangar launch)' })"
Write-Host "  evidence        : $runDirectory"
Write-Host "  loader log      : $loaderLogPath"
Write-Host "  ready pattern   : $ReadyPattern"
Write-Host "  capture mode    : $CaptureMode"
Write-Host "  dpi aware       : $script:DpiAware"
Write-Host "  timeout         : ${TimeoutSeconds}s (ready budget ${ReadyTimeoutSeconds}s)"
Write-Host '  schedule        :'
foreach ($entry in $schedule) {
    $suffix = if ($entry.Kind -eq 'key') { " key=$($entry.Key) (VK 0x{0:X2})" -f $entry.KeyCode } else { '' }
    # Format the offset invariantly: this desktop's locale renders 6.5 as "6,5".
    $offsetText = ([double]$entry.Offset).ToString([System.Globalization.CultureInfo]::InvariantCulture)
    Write-Host ("    {0,-8} {1}+{2}s -> {3}{4}" -f $entry.Kind, $entry.Anchor, $offsetText, $entry.Label, $suffix)
}
Write-Host '  required        :'
foreach ($pattern in $requiredPatterns) { Write-Host "    $pattern" }
Write-Host '------------------------------------------------------------------'
Write-Host ''

if ($DryRun) {
    Write-WotbLiveStep 'DryRun: preflight passed, nothing was launched or written.'
    exit 0
}

# ---------------------------------------------------------------------------
# Phase 2: baselines and hygiene. First disk mutation happens here.
# ---------------------------------------------------------------------------
[System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
[System.IO.Directory]::CreateDirectory($captureDirectory) | Out-Null

$loaderLogBaseline = Get-WotbLiveFileLength $loaderLogPath
$proxyLogBaseline = Get-WotbLiveFileLength $proxyLogPath
$crashDumpsBefore = Get-WotbLiveCrashDumpNames -Directory $crashDumpDirectory -Filter $crashDumpFilter
Write-WotbLiveStep "Baselines: loader log $loaderLogBaseline bytes, wotb_mod.log $proxyLogBaseline bytes, $($crashDumpsBefore.Count) crash dump(s)."

if ($loaderLogBaseline -lt 0) {
    Write-Warning "$loaderLogPath does not exist yet. The slice will contain the whole file the loader creates."
    $loaderLogBaseline = [long]0
}

$markerBackupPath = ''
if ([System.IO.File]::Exists($markerPath)) {
    $stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
    $markerBackupPath = "$markerPath.pre-run-$stamp.bak"
    Move-Item -LiteralPath $markerPath -Destination $markerBackupPath -Force
    Copy-Item -LiteralPath $markerBackupPath -Destination (Join-Path $runDirectory 'runtime_session.marker.pre-run') -Force
    Write-Warning "A stale runtime_session.marker was present and was MOVED to $markerBackupPath. The previous session did not shut down cleanly; without this move the client would start in safe mode."
}

if ([System.IO.File]::Exists($modsIniPath)) {
    Copy-Item -LiteralPath $modsIniPath -Destination (Join-Path $runDirectory 'mods.ini.snapshot') -Force
}

$stagingRoot = ''
if (-not [string]::IsNullOrWhiteSpace($replayFullPath)) {
    $stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('wotbmod-live-replay-' + [Guid]::NewGuid().ToString('N'))
    [System.IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
    $stagedReplay = Join-Path $stagingRoot ([System.IO.Path]::GetFileName($replayFullPath))
    Copy-Item -LiteralPath $replayFullPath -Destination $stagedReplay -Force
    Write-WotbLiveStep "Replay staged to $stagedReplay (the original is never handed to the client)."
}

# ---------------------------------------------------------------------------
# Phase 3: launch and schedule.
# ---------------------------------------------------------------------------
$process = $null
$launchUtc = $null
$readyUtc = $null
$readyDetected = $false
$exitedEarly = $false
$exitCode = $null
$closeRequested = $false
$closeAccepted = $false
$processExited = $false
$sliceBuilder = New-Object System.Text.StringBuilder
$logOffset = $loaderLogBaseline
$captureRecords = New-Object 'System.Collections.Generic.List[object]'
$keyRecords = New-Object 'System.Collections.Generic.List[object]'
$runError = ''
$captureIndex = 0

function Invoke-WotbLiveScheduleEntry {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [Parameter(Mandatory = $true)][IntPtr]$Handle
    )
    $Entry.Fired = $true
    $Entry.FiredUtc = [DateTime]::UtcNow.ToString('o')
    $focused = Set-WotbLiveForeground -Handle $Handle
    if (-not $focused) {
        $Entry.Note = 'foreground was not confirmed'
        Write-Warning "Could not confirm foreground for '$($Entry.Label)'. keybd_event only reaches the game with focus; treat this entry as unproven."
    }
    if ($Entry.Kind -eq 'key') {
        [WotbLive.Native]::keybd_event([byte]$Entry.KeyCode, 0, 0, [UIntPtr]::Zero)
        # The validation panel polls GetAsyncKeyState once per rendered frame.
        # A 60 ms hold was lost in the garage (2026-09-04, runs 8 and 11): the
        # frame there is longer than the press. Hold across several frames.
        Start-Sleep -Milliseconds 400
        [WotbLive.Native]::keybd_event([byte]$Entry.KeyCode, 0, $script:KeyEventKeyUp, [UIntPtr]::Zero)
        Write-WotbLiveStep ("Key {0} (VK 0x{1:X2}) sent at {2}." -f $Entry.Key, $Entry.KeyCode, $Entry.Label)
        return [PSCustomObject]@{
            label      = $Entry.Label
            key        = $Entry.Key
            vk         = ('0x{0:X2}' -f $Entry.KeyCode)
            anchor     = $Entry.Anchor
            offset_s   = $Entry.Offset
            fired_utc  = $Entry.FiredUtc
            foreground = $focused
        }
    }

    $script:captureIndex = $script:captureIndex + 1
    $name = ('{0:D2}-{1}.png' -f $script:captureIndex, $Entry.Label)
    $path = Join-Path $script:captureDirectory $name
    Start-Sleep -Milliseconds 250
    $shot = Save-WotbLiveCapture -Handle $Handle -Path $path -Mode $script:CaptureMode
    if ($shot.looks_blank) {
        Write-Warning "Capture '$name' has $($shot.distinct_sample_colors) distinct sampled colour(s) and mean luma $($shot.mean_luma). That is what a failed swapchain capture looks like."
    }
    Write-WotbLiveStep ("Captured {0} ({1}x{2}, mean luma {3})." -f $name, $shot.width, $shot.height, $shot.mean_luma)
    return [PSCustomObject]@{
        label                  = $Entry.Label
        file                   = $name
        anchor                 = $Entry.Anchor
        offset_s               = $Entry.Offset
        fired_utc              = $Entry.FiredUtc
        foreground             = $focused
        rect_source            = $shot.rect_source
        left                   = $shot.left
        top                    = $shot.top
        width                  = $shot.width
        height                 = $shot.height
        mean_luma              = $shot.mean_luma
        distinct_sample_colors = $shot.distinct_sample_colors
        looks_blank            = $shot.looks_blank
    }
}

try {
    # There is no replay flag. The path is a bare positional argument that the
    # shell-open path consumes. The engine's own parser prints
    # "Unknown argument: [1] <path>" and the battle loads anyway; that line is
    # expected noise, not a failure. -ArgumentList is passed as ONE pre-quoted
    # string so Start-Process cannot re-quote a path containing spaces.
    $argumentLine = ''
    if (-not [string]::IsNullOrWhiteSpace($stagedReplay)) {
        $argumentLine = '"' + $stagedReplay + '"'
    }
    Write-WotbLiveStep "Launching $gameExe $argumentLine"
    if ([string]::IsNullOrWhiteSpace($argumentLine)) {
        $process = Start-Process -FilePath $gameExe -WorkingDirectory $gameRootPath -PassThru
    } else {
        $process = Start-Process -FilePath $gameExe -ArgumentList $argumentLine -WorkingDirectory $gameRootPath -PassThru
    }
    $launchUtc = [DateTime]::UtcNow
    Write-WotbLiveStep "Client PID $($process.Id)."

    $windowHandle = [IntPtr]::Zero
    $deadline = $launchUtc.AddSeconds($TimeoutSeconds)
    $readyDeadline = $launchUtc.AddSeconds($ReadyTimeoutSeconds)
    $lastScheduledUtc = $null

    while ($true) {
        $now = [DateTime]::UtcNow

        $process.Refresh()
        if ($process.HasExited) {
            $exitedEarly = $true
            $processExited = $true
            $exitCode = $process.ExitCode
            Write-Warning "The client exited on its own after $([Math]::Round(($now - $launchUtc).TotalSeconds, 1))s with exit code $exitCode."
            break
        }
        if ($windowHandle -eq [IntPtr]::Zero -and $process.MainWindowHandle -ne [IntPtr]::Zero) {
            $windowHandle = $process.MainWindowHandle
            Write-WotbLiveStep "Main window handle $windowHandle."
        }

        $newOffset = $logOffset
        $delta = Read-WotbLiveLogDelta -Path $loaderLogPath -Offset $logOffset -NewOffset ([ref]$newOffset)
        $logOffset = $newOffset
        if ($delta.Length -gt 0) {
            [void]$sliceBuilder.Append($delta)
            if (-not $readyDetected -and $readyRegex.IsMatch($delta)) {
                $readyDetected = $true
                $readyUtc = $now
                Write-WotbLiveStep "Readiness pattern matched after $([Math]::Round(($now - $launchUtc).TotalSeconds, 1))s."
            }
        }

        if (-not $readyDetected -and $now -ge $readyDeadline) {
            Write-Warning "Readiness pattern '$ReadyPattern' did not appear within ${ReadyTimeoutSeconds}s. Ready-anchored schedule entries will be SKIPPED and the run will be judged as-is."
            foreach ($entry in $schedule) {
                if ($entry.Anchor -eq 'ready' -and -not $entry.Fired -and -not $entry.Skipped) {
                    $entry.Skipped = $true
                    $entry.Note = 'readiness never detected'
                }
            }
        }

        foreach ($entry in $schedule) {
            if ($entry.Fired -or $entry.Skipped -or $entry.Anchor -eq 'preclose') { continue }
            $anchorUtc = if ($entry.Anchor -eq 'ready') { $readyUtc } else { $launchUtc }
            if ($null -eq $anchorUtc) { continue }
            if ($now -lt $anchorUtc.AddSeconds($entry.Offset)) { continue }
            $record = Invoke-WotbLiveScheduleEntry -Entry $entry -Handle $windowHandle
            if ($entry.Kind -eq 'key') { $keyRecords.Add($record) } else { $captureRecords.Add($record) }
            $lastScheduledUtc = [DateTime]::UtcNow
        }

        $pending = @($schedule | Where-Object { -not $_.Fired -and -not $_.Skipped -and $_.Anchor -ne 'preclose' })
        if ($pending.Count -eq 0) {
            if ($null -eq $lastScheduledUtc) { $lastScheduledUtc = $now }
            if (([DateTime]::UtcNow - $lastScheduledUtc).TotalSeconds -ge $PostScheduleSeconds) { break }
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            Write-Warning "Wall-clock budget of ${TimeoutSeconds}s expired; proceeding to shutdown. Entries that never fired are recorded as skipped."
            foreach ($entry in $schedule) {
                if (-not $entry.Fired -and -not $entry.Skipped -and $entry.Anchor -ne 'preclose') {
                    $entry.Skipped = $true
                    $entry.Note = 'wall-clock budget expired'
                }
            }
            break
        }
        Start-Sleep -Milliseconds 250
    }

    if (-not $exitedEarly) {
        foreach ($entry in $schedule) {
            if ($entry.Anchor -ne 'preclose' -or $entry.Fired -or $entry.Skipped) { continue }
            if ($entry.Offset -gt 0) { Start-Sleep -Milliseconds ([int]($entry.Offset * 1000)) }
            $record = Invoke-WotbLiveScheduleEntry -Entry $entry -Handle $windowHandle
            if ($entry.Kind -eq 'key') { $keyRecords.Add($record) } else { $captureRecords.Add($record) }
        }
    }
} catch {
    $runError = $_.Exception.Message
    Write-Warning "Run phase failed: $runError. Shutting the client down and writing whatever evidence exists."
} finally {
    # -----------------------------------------------------------------------
    # Phase 4: shutdown and artifacts. Runs even when phase 3 threw.
    # -----------------------------------------------------------------------
    if ($null -ne $process) {
        try { $process.Refresh() } catch { }
        $hasExited = $true
        try { $hasExited = $process.HasExited } catch { $hasExited = $true }
        if (-not $hasExited) {
            Write-WotbLiveStep 'Requesting shutdown via CloseMainWindow (WM_CLOSE).'
            $closeRequested = $true
            try { $closeAccepted = $process.CloseMainWindow() } catch { $closeAccepted = $false }
            try { $processExited = $process.WaitForExit(120000) } catch { $processExited = $false }
            if (-not $processExited) {
                # Never force-kill: a killed client leaves runtime_session.marker
                # behind and sends the NEXT launch into safe mode. One whole run
                # was discarded to exactly this.
                Write-Warning 'The client did not exit within 120s of WM_CLOSE. This harness will NOT kill it. Close the client by hand, then check that mods\cache\runtime_session.marker is gone before the next run.'
            } else {
                try { $exitCode = $process.ExitCode } catch { $exitCode = $null }
                Write-WotbLiveStep "Client exited with code $exitCode."
            }
        } else {
            $processExited = $true
            try { $exitCode = $process.ExitCode } catch { $exitCode = $null }
        }
    }

    # Drain whatever the loader wrote during shutdown.
    for ($drain = 0; $drain -lt 8; ++$drain) {
        $newOffset = $logOffset
        $delta = Read-WotbLiveLogDelta -Path $loaderLogPath -Offset $logOffset -NewOffset ([ref]$newOffset)
        $logOffset = $newOffset
        if ($delta.Length -gt 0) { [void]$sliceBuilder.Append($delta) }
        if ($delta.Length -eq 0 -and $drain -ge 2) { break }
        Start-Sleep -Milliseconds 400
    }

    $slicePath = Join-Path $runDirectory 'loader_slice.log'
    $loaderLogFinal = Get-WotbLiveFileLength $loaderLogPath
    $sliceBaselineValid = ($loaderLogFinal -ge $loaderLogBaseline)
    if (-not $sliceBaselineValid) {
        Write-Warning "The loader log shrank during the run ($loaderLogBaseline -> $loaderLogFinal bytes). It was rotated or replaced; the slice cannot be trusted to cover only this run."
    }
    [System.IO.File]::WriteAllText(
        $slicePath, $sliceBuilder.ToString(), [System.Text.UTF8Encoding]::new($false))
    Write-WotbLiveStep "Loader slice: $($sliceBuilder.Length) chars over bytes [$loaderLogBaseline, $logOffset) -> $slicePath"

    $proxyLogCopy = ''
    if ([System.IO.File]::Exists($proxyLogPath)) {
        # wotb_mod.log is reopened "w" by the proxy for wotblitz.exe, so the
        # whole file belongs to this run.
        $proxyLogCopy = Join-Path $runDirectory 'wotb_mod.log'
        Copy-Item -LiteralPath $proxyLogPath -Destination $proxyLogCopy -Force
    }
    if ([System.IO.File]::Exists($bindingReportPath)) {
        Copy-Item -LiteralPath $bindingReportPath -Destination (Join-Path $runDirectory 'binding_pack_validation.json') -Force
    }
    # Give the runtime its documented shutdown cleanup time before judging the
    # marker. It renames the marker to runtime_session.marker.ended-<stamp>
    # seconds AFTER the process handle signals exit.
    $markerSettleWaited = 0.0
    $markerPresentAfter = [System.IO.File]::Exists($markerPath)
    if ($markerPresentAfter -and $MarkerSettleSeconds -gt 0) {
        Write-WotbLiveStep "runtime_session.marker still present; allowing up to ${MarkerSettleSeconds}s for shutdown cleanup."
        $settleStart = [DateTime]::UtcNow
        while ($markerPresentAfter -and
            ([DateTime]::UtcNow - $settleStart).TotalSeconds -lt $MarkerSettleSeconds) {
            Start-Sleep -Milliseconds 500
            $markerPresentAfter = [System.IO.File]::Exists($markerPath)
        }
        $markerSettleWaited = [Math]::Round(([DateTime]::UtcNow - $settleStart).TotalSeconds, 1)
        if (-not $markerPresentAfter) {
            Write-WotbLiveStep "runtime_session.marker cleared after ${markerSettleWaited}s."
        }
    }
    if ($markerPresentAfter) {
        Copy-Item -LiteralPath $markerPath -Destination (Join-Path $runDirectory 'runtime_session.marker.post-run') -Force
        Write-Warning "mods\cache\runtime_session.marker is STILL present ${markerSettleWaited}s after the client exited. The session did not shut down cleanly; the next launch would enter safe mode."
    }

    $crashDumpsAfter = Get-WotbLiveCrashDumpNames -Directory $crashDumpDirectory -Filter $crashDumpFilter
    $newDumps = @($crashDumpsAfter | Where-Object { $crashDumpsBefore -notcontains $_ })
    if ($newDumps.Count -gt 0) {
        Write-Warning "New crash dump(s): $($newDumps -join ', ')"
        [System.IO.File]::WriteAllLines(
            (Join-Path $runDirectory 'crashdumps_new.txt'), $newDumps, [System.Text.UTF8Encoding]::new($false))
    }

    if (-not [string]::IsNullOrWhiteSpace($stagingRoot) -and -not $KeepStagedReplay) {
        try {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force
        } catch {
            Write-Warning "Staged replay directory could not be removed (the client may still hold it): $stagingRoot"
        }
    }

    $manifest = [ordered]@{
        schema                = 'wotbmod.live-evidence-run/v1'
        run_id                = $runId
        started_utc           = $(if ($launchUtc) { $launchUtc.ToString('o') } else { '' })
        finished_utc          = [DateTime]::UtcNow.ToString('o')
        harness               = [ordered]@{
            script          = $PSCommandPath
            powershell      = $PSVersionTable.PSVersion.ToString()
            dpi_aware       = $script:DpiAware
            capture_mode    = $CaptureMode
            capture_api     = 'System.Drawing.Graphics.CopyFromScreen (desktop DC blit)'
            dry_run         = [bool]$DryRun
            run_error       = $runError
        }
        game                  = [ordered]@{
            root                 = $gameRootPath
            executable           = $gameExe
            steam_appid_present  = [System.IO.File]::Exists($steamAppId)
            mods_ini             = $modsIniPath
        }
        replay                = [ordered]@{
            original = $replayFullPath
            staged   = $stagedReplay
            kept     = [bool]$KeepStagedReplay
        }
        readiness             = [ordered]@{
            pattern           = $ReadyPattern
            pattern_verified  = $false
            detected          = $readyDetected
            detected_utc      = $(if ($readyUtc) { $readyUtc.ToString('o') } else { '' })
            timeout_s         = $ReadyTimeoutSeconds
        }
        baselines             = [ordered]@{
            loader_log_path         = $loaderLogPath
            loader_log_bytes_before = $loaderLogBaseline
            loader_log_bytes_after  = $loaderLogFinal
            slice_end_offset        = $logOffset
            slice_baseline_valid    = $sliceBaselineValid
            proxy_log_bytes_before  = $proxyLogBaseline
            crash_dump_directory    = $crashDumpDirectory
            crash_dump_filter       = $crashDumpFilter
            crash_dumps_before      = $crashDumpsBefore.Count
            crash_dumps_after       = $crashDumpsAfter.Count
            crash_dumps_new         = $newDumps
        }
        hygiene               = [ordered]@{
            stale_marker_found   = (-not [string]::IsNullOrWhiteSpace($markerBackupPath))
            stale_marker_backup  = $markerBackupPath
            marker_path          = $markerPath
            marker_after_run     = $markerPresentAfter
            marker_settle_budget = $MarkerSettleSeconds
            marker_settle_waited = $markerSettleWaited
        }
        process               = [ordered]@{
            pid              = $(if ($process) { $process.Id } else { 0 })
            exited           = $processExited
            exited_early     = $exitedEarly
            exit_code        = $exitCode
            close_requested  = $closeRequested
            close_accepted   = $closeAccepted
            force_killed     = $false
        }
        # NOTE: never write @($someList) where the variable is a
        # System.Collections.Generic.List[object]. On PowerShell 7.6.5 the array
        # subexpression operator throws "Argument types do not match" for that
        # exact type (List[string] is fine). Use .ToArray() instead. Piping a
        # list through ForEach-Object/Where-Object is unaffected.
        schedule              = @($schedule | ForEach-Object {
                [ordered]@{
                    kind    = $_.Kind
                    token   = $_.Token
                    anchor  = $_.Anchor
                    offset  = $_.Offset
                    label   = $_.Label
                    fired   = $_.Fired
                    skipped = $_.Skipped
                    note    = $_.Note
                }
            })
        captures              = $captureRecords.ToArray()
        keys                  = $keyRecords.ToArray()
        artifacts             = [ordered]@{
            slice        = $slicePath
            proxy_log    = $proxyLogCopy
            capture_dir  = $captureDirectory
        }
        verdict_inputs        = [ordered]@{
            bad_line_pattern  = $BadLinePattern
            required_patterns = $requiredPatterns.ToArray()
        }
    }
    $manifestPath = Join-Path $runDirectory 'run.json'
    $manifest | ConvertTo-Json -Depth 10 |
        Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-WotbLiveStep "Run manifest: $manifestPath"
}

# ---------------------------------------------------------------------------
# Phase 5: verdict.
# ---------------------------------------------------------------------------
if ($SkipJudge) {
    Write-Host ''
    Write-Host "Evidence written to $runDirectory. -SkipJudge was set, so no verdict was produced."
    exit 0
}

$additional = @()
if (-not [string]::IsNullOrWhiteSpace($proxyLogCopy)) { $additional += $proxyLogCopy }

# The judge is driven through a request file rather than a command line so that
# regex metacharacters survive verbatim and the exact inputs stay in evidence.
$request = [ordered]@{
    SlicePath                = $slicePath
    AdditionalEvidenceFile   = $additional
    MarkerPath               = $markerPath
    # The runner already spent its own settle budget above; do not wait twice.
    MarkerSettleSeconds      = 0
    ProcessExited            = $(if ($processExited) { 'true' } else { 'false' })
    BaselineCrashDumpCount   = $crashDumpsBefore.Count
    # The NAMES are what the judge's crash gate decides on. The count alone is
    # blind once Windows has rotated the CrashDumps folder, which is precisely
    # when a crash is most likely to be missed.
    BaselineCrashDumpName    = @($crashDumpsBefore)
    # An unhandled exception exits with its NTSTATUS, so this catches a crash
    # even when no dump file is produced at all.
    ProcessExitCode          = $(if ($null -ne $exitCode) { [string]$exitCode } else { '' })
    CrashDumpDirectory       = $crashDumpDirectory
    CrashDumpFilter          = $crashDumpFilter
    RequirePattern           = $requiredPatterns.ToArray()
    BadLinePattern           = $BadLinePattern
    JsonOutputPath           = (Join-Path $runDirectory 'verdict.json')
}
$requestPath = Join-Path $runDirectory 'judge_request.json'
$request | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $requestPath -Encoding UTF8

$hostExe = ''
try { $hostExe = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName } catch { $hostExe = '' }
if ([string]::IsNullOrWhiteSpace($hostExe) -or
    -not ([System.IO.Path]::GetFileNameWithoutExtension($hostExe) -match '^(?i)(pwsh|powershell)$')) {
    $hostExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
}

Write-Host ''
& $hostExe -NoProfile -ExecutionPolicy Bypass -File $judgeScript -ParameterFile $requestPath |
    Tee-Object -FilePath (Join-Path $runDirectory 'judge.txt')
$judgeExit = $LASTEXITCODE

Write-Host ''
Write-Host "Evidence: $runDirectory"
if ($judgeExit -eq 0) {
    Write-Host 'Verdict: CLEAN'
} else {
    Write-Host "Verdict: DIRTY (judge exit $judgeExit)"
}
exit $judgeExit
