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

# Dump everything we can find about the runner's window state when login
# fails, so we don't have to guess. Used only on the failure path — logs
# don't carry the OTP since we never echo it.
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
    Get-Process -Name 'SimplySign*' -ErrorAction SilentlyContinue |
        Select-Object Id, ProcessName, MainWindowHandle, MainWindowTitle, HasExited |
        Format-Table -AutoSize | Out-Host

    # Enumerate every top-level window on the runner via Win32 so we see
    # whatever Certum's GUI is *actually* called. If nothing shows up, we
    # know the runner has no interactive window station and SendKeys has
    # no chance of working — that's the signal to switch to a self-hosted
    # runner or a different signing strategy.
    if (-not ('Win32Enum' -as [type])) {
        Add-Type -Namespace 'NS' -Name 'Win32Enum' -MemberDefinition @"
            [System.Runtime.InteropServices.DllImport("user32.dll")]
            public static extern bool EnumWindows(EnumWindowsProc enumProc, System.IntPtr lParam);
            public delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);
            [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Auto)]
            public static extern int GetWindowText(System.IntPtr hWnd, System.Text.StringBuilder text, int count);
            [System.Runtime.InteropServices.DllImport("user32.dll")]
            public static extern bool IsWindowVisible(System.IntPtr hWnd);
            [System.Runtime.InteropServices.DllImport("user32.dll")]
            public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint lpdwProcessId);
"@
    }

    $windows = New-Object 'System.Collections.Generic.List[object]'
    $cb = [NS.Win32Enum+EnumWindowsProc] {
        param($hWnd, $lParam)
        $sb  = New-Object System.Text.StringBuilder 256
        [void][NS.Win32Enum]::GetWindowText($hWnd, $sb, $sb.Capacity)
        $vis = [NS.Win32Enum]::IsWindowVisible($hWnd)
        $pid = 0
        [void][NS.Win32Enum]::GetWindowThreadProcessId($hWnd, [ref]$pid)
        $windows.Add([pscustomobject]@{
            Handle  = $hWnd
            Visible = $vis
            Pid     = $pid
            Title   = $sb.ToString()
        })
        return $true  # keep enumerating
    }
    [void][NS.Win32Enum]::EnumWindows($cb, [System.IntPtr]::Zero)

    Write-Host ("Top-level windows (count={0}):" -f $windows.Count)
    $windows |
        Where-Object { $_.Title -ne '' -or $_.Visible } |
        Sort-Object Visible -Descending |
        Select-Object -First 30 |
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

# Try to focus by process ID first (most reliable when there's only one
# SimplySign Desktop instance). Fall back to title match — Certum has
# kept "SimplySign Desktop" as the window title across versions.
$focused = $false
for ($i = 0; $i -lt 10 -and -not $focused; $i++) {
    $focused = $wshell.AppActivate($proc.Id)
    if (-not $focused) {
        $focused = $wshell.AppActivate('SimplySign Desktop')
    }
    if (-not $focused) { Start-Sleep -Milliseconds 500 }
}
if (-not $focused) {
    Dump-WindowDiagnostics -LaunchedProc $proc
    throw @"
Could not focus the SimplySign Desktop login window after 10 retries.
See diagnostic dump above. Common causes:

  1. The GitHub-hosted windows-2022 runner has no interactive
     window station — top-level window count above will be very
     low (maybe 0 visible). In that case, SendKeys cannot work
     here regardless of titles, and the path forward is a
     self-hosted runner (machine kept logged in) or local-sign
     + manual-upload.

  2. The window title changed in this Certum version (we expect
     'SimplySign Desktop'). Look at the dump's "Top-level windows"
     table for what's actually there and update the AppActivate
     call in this script.

  3. The process exited before showing a window (HasExited=True
     above) — the MSI may need user-context state we don't have.
"@
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
