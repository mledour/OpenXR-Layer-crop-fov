# Sign-Artifact.ps1
#
# Code-signs a Windows binary (DLL or EXE) using a Certum SimplySign Cloud
# certificate, automatically, from CI.
#
# Why this script is the way it is
# --------------------------------
# Certum SimplySign Desktop has NO documented headless CLI. There is no
#   `SimplySignDesktop.exe /login /username X /otp Y`
# despite what the Certum manual's "automatic login" section suggests at
# first read — that section is about persisting credentials across GUI
# sessions, not feeding them via flags. We confirmed this by:
#   - Reading the official manual (CS-Code_Signing_in_the_Cloud_*.pdf).
#   - Searching the wider community of CI/CD users; everyone who
#     automates Certum cloud signing either drives the GUI via SendKeys
#     (devas.life) or runs SimplySign Desktop in Xvnc + p11-kit
#     (hpvb/certum-container).
#
# So this script drives the GUI. The flow:
#   1. Generate a fresh TOTP from the shared seed (Get-CertumTotp.ps1,
#      HMAC-SHA256 — Certum's otpauth URI says algorithm=SHA256).
#   2. Launch SimplySignDesktop.exe (no flags — it shows its login form).
#   3. Use the WScript.Shell COM object to focus the login window.
#   4. SendKeys: clear-username, type-username, Tab, type-OTP, Enter.
#   5. Poll Cert:\CurrentUser\My until the cert appears (proves the
#      cloud HSM session is live and bridged to the Windows store).
#   6. Run signtool with /sha1 thumbprint — same invocation the Certum
#      manual prescribes for the manual flow. Then signtool verify /pa.
#
# Brittleness budget
# ------------------
# SendKeys is fragile by nature. If a Certum update changes the login
# window's title, tab order, or layout, this script silently sends keys
# to the wrong field. Mitigations baked in:
#   - Window-title match: `AppActivate('SimplySign Desktop')` — Certum
#     hasn't changed this title in years (devas.life uses it; the manual
#     uses it).
#   - Field clear (`^a{DEL}`) before typing username, in case the
#     installer pre-populated something.
#   - Final cert-store poll. If SendKeys missed, the cert never
#     appears, and we fail fast with a clear message instead of
#     "succeeding" with an unsigned binary.
# When SimplySign Desktop ships an update we don't trust, pin
# SIMPLYSIGN_VERSION in the workflow first, retest locally, then bump.
#
# Required GitHub Secrets (mapped into env vars by the workflow):
#   CERTUM_USERNAME         — SimplySign portal email
#   CERTUM_TOTP_SEED        — Base32 seed from "Show secret key"
#   CERTUM_CERT_THUMBPRINT  — 40-hex-char SHA-1 of the issued certificate
#                             (no spaces). Find it once with:
#                               Get-ChildItem Cert:\CurrentUser\My |
#                                   Where-Object Subject -Match 'Le ?[Dd]our' |
#                                   Format-List Thumbprint, Subject
#
# Certum SimplySign uses 2FA where the TOTP IS the second factor — there
# is no separate static password to provide alongside the username.
#
# This script intentionally does NOT echo any of those into logs. The OTP
# is the only short-lived secret in flight; it expires within 30 s of
# generation, so even a logged copy is moot quickly.

[CmdletBinding()]
param(
    # One or more files to sign. Globs allowed.
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]] $Path,

    # SimplySignDesktop.exe location. The 64-bit MSI installs to
    # `C:\Program Files\Certum\SimplySign Desktop\` (real 64-bit app —
    # `Program Files (x86)` would be wrong here). Override for
    # self-hosted runners with a non-default install layout.
    [string] $SimplySignExe = 'C:\Program Files\Certum\SimplySign Desktop\SimplySignDesktop.exe',

    # signtool.exe — present on PATH on the GitHub Actions windows-2022
    # image via the Windows SDK. Override only if needed.
    [string] $SignToolExe = 'signtool.exe',

    # Description embedded in the signature ("More info" line in the UAC
    # prompt).
    [string] $Description = 'XR_APILAYER_MLEDOUR_fov_crop',

    # Login-flow timing. Defaults are conservative; tune only if you see
    # races on a slow runner.
    [int] $WindowAppearMs   = 6000,
    [int] $BetweenKeysMs    = 250,
    [int] $CertAppearTotalS = 60
)

$ErrorActionPreference = 'Stop'

function Require-Env {
    param([string] $Name)
    $val = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($val)) {
        throw "Required environment variable '$Name' is empty. Add it as a GitHub Secret and map it in the workflow."
    }
    return $val
}

# SendKeys interprets `+ ^ % ~ ( ) { }` as modifier syntax. A username
# email like "michael.ledour@gmail.com" doesn't contain any of those, but
# the function is here so a future ID with `+` (Gmail tagging) doesn't
# silently lose characters.
function Escape-SendKeys {
    param([string] $s)
    return ($s -replace '([+^%~(){}])', '{$1}')
}

# Win32 P/Invoke surface for window enumeration + force-show. Loaded
# once and reused across diagnostics + the main flow. Note: `$pid` is a
# PowerShell read-only automatic variable, so we use `procId` in the
# managed code below.
if (-not ('NS.Win32' -as [type])) {
    Add-Type -Namespace 'NS' -Name 'Win32' -MemberDefinition @"
        public delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        public static extern bool EnumWindows(EnumWindowsProc enumProc, System.IntPtr lParam);

        [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Auto)]
        public static extern int GetWindowText(System.IntPtr hWnd, System.Text.StringBuilder text, int count);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        public static extern bool IsWindowVisible(System.IntPtr hWnd);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint procId);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        public static extern bool ShowWindow(System.IntPtr hWnd, int nCmdShow);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(System.IntPtr hWnd);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        public static extern bool BringWindowToTop(System.IntPtr hWnd);

        // ShowWindow nCmdShow constants. SW_RESTORE undoes minimization
        // AND makes the window visible — the right hammer for an app
        // that started hidden (system-tray apps like SimplySign Desktop
        // do this on launch).
        public const int SW_HIDE        = 0;
        public const int SW_SHOWNORMAL  = 1;
        public const int SW_SHOW        = 5;
        public const int SW_RESTORE     = 9;
"@
}

# Enumerate every top-level window on the runner. Returns a list of
# pscustomobject{Handle, Visible, Pid, Title}.
function Get-AllTopLevelWindows {
    $list = New-Object 'System.Collections.Generic.List[object]'
    $cb = [NS.Win32+EnumWindowsProc] {
        param($hWnd, $lParam)
        $sb  = New-Object System.Text.StringBuilder 256
        [void][NS.Win32]::GetWindowText($hWnd, $sb, $sb.Capacity)
        $vis = [NS.Win32]::IsWindowVisible($hWnd)
        $procId = [uint32]0
        [void][NS.Win32]::GetWindowThreadProcessId($hWnd, [ref]$procId)
        $list.Add([pscustomobject]@{
            Handle  = $hWnd
            Visible = $vis
            Pid     = $procId
            Title   = $sb.ToString()
        })
        return $true  # keep enumerating
    }
    [void][NS.Win32]::EnumWindows($cb, [System.IntPtr]::Zero)
    return ,$list
}

# Find a SimplySign Desktop login window. Prefers a VISIBLE window
# whose title contains 'SimplySign' or 'Login' (login dialogs typically
# carry one of those, even if Certum localizes the rest of the form).
# Falls back to a hidden 'SimplySign Desktop' window, but the caller
# should treat that as a poor target — calling ShowWindow on Certum's
# hidden tray host has been observed to destroy the window outright on
# CI runners.
# Returns the handle as IntPtr, or [System.IntPtr]::Zero if not found.
function Find-SimplySignWindow {
    param([int] $TargetPid = -1, [switch] $RequireVisible)
    $all = Get-AllTopLevelWindows | Where-Object {
        $TargetPid -lt 0 -or $_.Pid -eq $TargetPid
    }
    # Prefer visible windows with a SimplySign-ish or Login-ish title.
    $visible = $all | Where-Object {
        $_.Visible -and ($_.Title -match 'SimplySign' -or $_.Title -match 'Login' -or $_.Title -match 'Logowanie')
    }
    if ($visible.Count -gt 0) { return $visible[0].Handle }
    if ($RequireVisible) { return [System.IntPtr]::Zero }
    # Fallback to hidden ones — the caller should be cautious.
    $hidden = $all | Where-Object {
        $_.Title -eq 'SimplySign Desktop'
    }
    if ($hidden.Count -gt 0) { return $hidden[0].Handle }
    return [System.IntPtr]::Zero
}

# Force a window to be visible + foreground. SimplySign Desktop on
# first launch on a CI runner sits hidden (its tray-icon UI metaphor),
# so AppActivate fails until we do this. The sequence is the standard
# Win32 incantation: SW_RESTORE → BringWindowToTop → SetForegroundWindow.
function Show-WindowForeground {
    param([System.IntPtr] $Handle)
    [void][NS.Win32]::ShowWindow($Handle, [NS.Win32]::SW_RESTORE)
    Start-Sleep -Milliseconds 200
    [void][NS.Win32]::BringWindowToTop($Handle)
    Start-Sleep -Milliseconds 100
    [void][NS.Win32]::SetForegroundWindow($Handle)
}

# Dump everything we can find about the runner's window state when login
# fails. Used only on the failure path — logs don't carry the OTP since
# we never echo it.
function Dump-WindowDiagnostics {
    param([System.Diagnostics.Process] $LaunchedProc)

    Write-Host '--- Window diagnostics ---'
    if ($LaunchedProc) {
        $LaunchedProc.Refresh()
        Write-Host ("LaunchedProc.Id              = {0}" -f $LaunchedProc.Id)
        Write-Host ("LaunchedProc.HasExited       = {0}" -f $LaunchedProc.HasExited)
        if ($LaunchedProc.HasExited) {
            Write-Host ("LaunchedProc.ExitCode      = {0}" -f $LaunchedProc.ExitCode)
        } else {
            Write-Host ("LaunchedProc.MainWindowHandle = {0}" -f $LaunchedProc.MainWindowHandle)
            Write-Host ("LaunchedProc.MainWindowTitle  = '{0}'" -f $LaunchedProc.MainWindowTitle)
        }
    }

    Write-Host 'All SimplySign* processes:'
    $simplyPids = @(Get-Process -Name 'SimplySign*' -ErrorAction SilentlyContinue |
                    ForEach-Object { $_.Id })
    Get-Process -Name 'SimplySign*' -ErrorAction SilentlyContinue |
        Select-Object Id, ProcessName, MainWindowHandle, MainWindowTitle, HasExited |
        Format-Table -AutoSize | Out-Host

    $windows = Get-AllTopLevelWindows

    # 1. EVERY top-level window owned by a SimplySign* process — even
    #    untitled ones, even hidden ones. This tells us whether the
    #    process is silently sitting on a window we'd otherwise miss
    #    in the title-filtered listing below.
    if ($simplyPids.Count -gt 0) {
        Write-Host ("All windows owned by SimplySign* PIDs ({0}):" -f ($simplyPids -join ', '))
        $windows |
            Where-Object { $simplyPids -contains $_.Pid } |
            Format-Table -AutoSize | Out-Host
    }

    # 2. The wider top-level dump — limited to titled OR visible
    #    windows so the table stays readable. We bumped the cap to 50
    #    because the previous 30 truncated relevant entries.
    Write-Host ("Top-level windows on the runner (count={0}, showing titled+visible):" -f $windows.Count)
    $windows |
        Where-Object { $_.Title -ne '' -or $_.Visible } |
        Sort-Object Visible -Descending |
        Select-Object -First 50 |
        Format-Table -AutoSize | Out-Host
    Write-Host '--- end diagnostics ---'
}

# --- Resolve inputs -------------------------------------------------------
$username   = Require-Env 'CERTUM_USERNAME'
$totpSeed   = Require-Env 'CERTUM_TOTP_SEED'
$thumbprint = (Require-Env 'CERTUM_CERT_THUMBPRINT') -replace '\s', ''

if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
    throw "CERTUM_CERT_THUMBPRINT must be 40 hex characters (SHA-1 of the cert). Got length $($thumbprint.Length)."
}

if (-not (Test-Path $SimplySignExe)) {
    throw "SimplySign Desktop not found at '$SimplySignExe'. Install it on the runner first (see workflow's 'Install SimplySign Desktop' step)."
}

# --- Generate fresh TOTP --------------------------------------------------
# Generated as late as possible before the keystroke send so we have the
# full 30 s window after typing it in. Algorithm defaults to SHA-256
# (Certum's otpauth `algorithm=` value); leave the param off here so a
# future change in Get-CertumTotp.ps1's default propagates.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$totp = & (Join-Path $scriptDir 'Get-CertumTotp.ps1') -Base32Seed $totpSeed
if ([string]::IsNullOrWhiteSpace($totp) -or $totp.Length -ne 6) {
    throw "Get-CertumTotp.ps1 produced an unexpected value (length=$($totp.Length))."
}

# --- Launch SimplySign Desktop and inject credentials via SendKeys -------
Write-Host "Launching SimplySign Desktop ..."
$proc = Start-Process -FilePath $SimplySignExe -PassThru
Start-Sleep -Milliseconds $WindowAppearMs

$wshell = New-Object -ComObject WScript.Shell

# Strategy: wait patiently for SimplySign Desktop to show a VISIBLE
# login window on its own. We don't force-show its hidden tray host
# any more — that's been observed to destroy the window outright on
# the CI runner (run #2 ended up with the SimplySign process owning
# only "Default IME" windows after a ShowWindow(SW_RESTORE) call).
#
# Run #1 saw the window 'SimplySign Desktop' with Visible=False. Two
# possibilities now:
#
#   (a) Certum eventually shows the login dialog by itself if we wait
#       long enough — first launch on a fresh install commonly does
#       this with a multi-second initialization delay. We give it up
#       to $WaitForVisibleS seconds.
#
#   (b) Certum never shows it on a non-interactive desktop (despite
#       the desktop being technically present — Program Manager and
#       all are there). In that case we fail with a full diagnostic
#       and switch strategies.
$WaitForVisibleS = 60
Write-Host ("Waiting up to ${WaitForVisibleS}s for a visible SimplySign Desktop login window ...")

$hwnd     = [System.IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($WaitForVisibleS)
while ((Get-Date) -lt $deadline -and $hwnd -eq [System.IntPtr]::Zero) {
    $hwnd = Find-SimplySignWindow -TargetPid $proc.Id -RequireVisible
    if ($hwnd -eq [System.IntPtr]::Zero) {
        Start-Sleep -Seconds 2
    }
}

if ($hwnd -eq [System.IntPtr]::Zero) {
    Dump-WindowDiagnostics -LaunchedProc $proc
    throw @"
SimplySign Desktop never produced a visible login window after ${WaitForVisibleS}s.
The diagnostic dump above shows what windows the process did create.
If the only entries owned by the SimplySign* PID are "Default IME" and
nothing else, Certum is failing to render its login UI on this runner
type — switch to a self-hosted runner (Windows machine kept logged in)
or local-sign + manual upload.
"@
}
Write-Host ("Found visible SimplySign window: handle={0}" -f $hwnd)

# Belt-and-braces foreground bring-up. SetForegroundWindow on a window
# that's ALREADY visible is benign (no destruction risk; that risk was
# specific to forcing a hidden tray-host visible).
[void][NS.Win32]::BringWindowToTop($hwnd)
[void][NS.Win32]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds $BetweenKeysMs

$focused = $false
for ($i = 0; $i -lt 10 -and -not $focused; $i++) {
    $focused = $wshell.AppActivate($proc.Id)
    if (-not $focused) {
        $focused = $wshell.AppActivate('SimplySign Desktop')
    }
    if (-not $focused) { Start-Sleep -Milliseconds 300 }
}
if (-not $focused) {
    Dump-WindowDiagnostics -LaunchedProc $proc
    throw "Found visible window (handle $hwnd) but AppActivate refused to focus it. See diagnostic dump above."
}

Start-Sleep -Milliseconds $BetweenKeysMs

# Type into the username field. `^a{DEL}` first, in case the installer
# pre-filled it with something (e.g. a previously-used account on a
# self-hosted runner). Using Escape-SendKeys to be safe even though a
# typical email address has no SendKeys metachars.
Write-Host "Sending credentials ..."
$wshell.SendKeys('^a{DEL}')
Start-Sleep -Milliseconds $BetweenKeysMs
$wshell.SendKeys((Escape-SendKeys $username))
Start-Sleep -Milliseconds $BetweenKeysMs

# Tab to the OTP field. SimplySign Desktop's login form has the OTP
# directly after the username — one Tab moves focus there.
$wshell.SendKeys('{TAB}')
Start-Sleep -Milliseconds $BetweenKeysMs

# OTP is digits only — no escaping needed, but we run it through
# Escape-SendKeys anyway for symmetry (and to keep one code path).
$wshell.SendKeys((Escape-SendKeys $totp))
Start-Sleep -Milliseconds $BetweenKeysMs

# Submit. {ENTER} = "press the default button", which is "Sign in".
$wshell.SendKeys('{ENTER}')

# --- Wait for the cert to materialize in the Windows store ---------------
# This is the real proof of a successful login. Anything before this
# point could plausibly succeed silently while sending keystrokes to the
# wrong window or wrong field. If the cert shows up, we know:
#   1. SimplySign Desktop accepted the username + TOTP.
#   2. The cloud HSM session is live.
#   3. The CryptoAPI bridge has populated the local cert store.
Write-Host "Waiting up to $CertAppearTotalS s for cert thumbprint $thumbprint to appear in CurrentUser\My ..."
$deadline = (Get-Date).AddSeconds($CertAppearTotalS)
$cert     = $null
while ((Get-Date) -lt $deadline) {
    $cert = Get-ChildItem -Path 'Cert:\CurrentUser\My' -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $thumbprint } |
            Select-Object -First 1
    if ($null -ne $cert) { break }
    Start-Sleep -Milliseconds 1000
}
if ($null -eq $cert) {
    # Don't dump the OTP — but DO say what was attempted so the failure
    # mode is actionable.
    throw @"
Certificate $thumbprint did not appear in CurrentUser\My within $CertAppearTotalS s.
Likely causes (in order):
  1. The OTP was wrong (clock skew on the runner > 30 s, or wrong CERTUM_TOTP_SEED).
  2. The username was wrong (typo in CERTUM_USERNAME secret).
  3. The thumbprint changed (Certum certs renew yearly — update CERTUM_CERT_THUMBPRINT).
  4. SimplySign Desktop's login form layout changed in version $((Get-Item $SimplySignExe).VersionInfo.FileVersion).
"@
}
Write-Host "Found cert: $($cert.Subject)"

# --- Resolve files and sign ---------------------------------------------
$files = @()
foreach ($p in $Path) {
    $resolved = Get-ChildItem -Path $p -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "No files matched '$p'."
    }
    $files += $resolved
}

# signtool invocation matches the Certum manual prescription verbatim.
foreach ($f in $files) {
    Write-Host "Signing $($f.FullName) ..."
    & $SignToolExe sign `
        /sha1 $thumbprint `
        /tr   'http://time.certum.pl' `
        /td   sha256 `
        /fd   sha256 `
        /d    $Description `
        /v    `
        $f.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed for $($f.FullName) (exit $LASTEXITCODE)."
    }

    # /pa = use the default "any" policy. Without it, signtool verify
    # defaults to driver-signing rules and rejects user-mode DLLs that
    # are perfectly correctly signed.
    & $SignToolExe verify /pa /v $f.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "signtool verify failed for $($f.FullName) (exit $LASTEXITCODE)."
    }
}

Write-Host "All artifacts signed and verified."
