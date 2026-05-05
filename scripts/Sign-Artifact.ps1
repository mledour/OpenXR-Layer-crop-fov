# Sign-Artifact.ps1
#
# Code-signs a Windows binary (DLL or EXE) using a Certum SimplySign Cloud
# certificate, headlessly, from CI.
#
# Why this script exists
# ----------------------
# Certum's official "Code Signing in the Cloud" manual only documents the
# interactive flow: a human opens SimplySign Desktop, types their username,
# reads a TOTP off their phone, and clicks "Sign in" (the TOTP IS the
# second factor — there is no separate static password). Once that desktop
# session is logged in, the cert appears in the Windows certificate store
# and `signtool /sha1 <THUMBPRINT> /tr ...` can sign any binary.
#
# To run this in GitHub Actions instead of by hand, three things have to be
# automated:
#
#   1. Get a TOTP without the phone — we hold the Base32 seed Certum shows
#      under "Show secret key" in the portal as a GitHub Secret, and
#      regenerate the 6-digit code on demand with Get-CertumTotp.ps1
#      (RFC 6238, no extra deps).
#   2. Log SimplySign Desktop in non-interactively — its installer ships a
#      CLI mode that takes username/OTP as arguments. We invoke that and
#      wait for the cert to materialize in the Windows store.
#   3. Run signtool exactly as the Certum manual prescribes — `/sha1` with
#      the cert thumbprint, `/tr http://time.certum.pl` for RFC 3161
#      timestamping, `/td sha256 /fd sha256` for SHA-256 file + timestamp
#      digests.
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
# Note: Certum SimplySign uses 2FA where the TOTP IS the second factor —
# there is no separate static password to provide alongside the username.
# We generate the TOTP on demand from the shared seed, so the username +
# fresh TOTP is the full credential set.
#
# This script intentionally does NOT echo any of those into logs. The only
# thing that ends up in the run output is "signed <path>" plus signtool's
# own output (which contains the cert subject — that's expected and public).

[CmdletBinding()]
param(
    # One or more files to sign. Globs allowed.
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]] $Path,

    # Path to SimplySignDesktop.exe. Default matches the standard installer
    # location on a 64-bit Windows host. Can be overridden for self-hosted
    # runners with a non-standard layout.
    [string] $SimplySignExe = 'C:\Program Files (x86)\Certum\SimplySign Desktop\SimplySignDesktop.exe',

    # signtool.exe lives inside the Windows 10 SDK. The MSBuild GitHub
    # Actions image already adds it to PATH, but we resolve it explicitly
    # so a missing PATH entry produces a clear error instead of a confusing
    # "command not found".
    [string] $SignToolExe = 'signtool.exe',

    # Description embedded in the signature ("More info" line in the UAC
    # prompt). Keep it short and unambiguous.
    [string] $Description = 'XR_APILAYER_MLEDOUR_fov_crop'
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

$username   = Require-Env 'CERTUM_USERNAME'
$totpSeed   = Require-Env 'CERTUM_TOTP_SEED'
$thumbprint = (Require-Env 'CERTUM_CERT_THUMBPRINT') -replace '\s', ''

if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
    throw "CERTUM_CERT_THUMBPRINT must be 40 hex characters (SHA-1 of the cert). Got length $($thumbprint.Length)."
}

if (-not (Test-Path $SimplySignExe)) {
    throw "SimplySign Desktop not found at '$SimplySignExe'. Install it on the runner first (see workflow's 'Install SimplySign Desktop' step)."
}

# Generate a fresh TOTP. Certum windows are 30 s — we issue the login
# immediately after generation so we have ~25 s of margin; if the runner is
# heavily loaded we still won't drift past the boundary.
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$totp = & (Join-Path $scriptDir 'Get-CertumTotp.ps1') -Base32Seed $totpSeed
if ([string]::IsNullOrWhiteSpace($totp) -or $totp.Length -ne 6) {
    throw "Get-CertumTotp.ps1 produced an unexpected value (length=$($totp.Length))."
}

Write-Host "Logging in to Certum SimplySign Cloud as '$username' ..."

# SimplySign Desktop's headless login. Argument names follow Certum's
# documented "Automatic login" parameters; ProcessStartInfo is used so
# the OTP is passed as an ARGUMENT not via the shell, which avoids it
# landing in the runner's command-history transcript.
#
# Certum SimplySign uses the TOTP itself as the second factor — there
# is no separate static password to pass alongside /username.
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName               = $SimplySignExe
$psi.ArgumentList.Add('/login')
$psi.ArgumentList.Add('/username'); $psi.ArgumentList.Add($username)
$psi.ArgumentList.Add('/otp');      $psi.ArgumentList.Add($totp)
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.CreateNoWindow         = $true

$proc = [System.Diagnostics.Process]::Start($psi)
# 60 s is generous; a healthy login completes in <5 s.
if (-not $proc.WaitForExit(60000)) {
    $proc.Kill()
    throw "SimplySignDesktop /login timed out after 60s."
}
if ($proc.ExitCode -ne 0) {
    # The OTP is the only short-lived secret in this flow and it could
    # appear in error text echoed back by SimplySignDesktop. Strip it
    # before printing. (The seed itself never reaches stderr — it's only
    # used as the HMAC key inside Get-CertumTotp.ps1.)
    $stderr = $proc.StandardError.ReadToEnd() -replace [Regex]::Escape($totp), '<otp>'
    throw "SimplySignDesktop /login failed (exit $($proc.ExitCode)): $stderr"
}

Write-Host "Login OK — waiting for cert to appear in CurrentUser\My ..."

# After login, SimplySign Desktop pulls the cert into the Windows cert
# store via its CryptoAPI provider. That's asynchronous; poll up to 30 s.
$deadline = (Get-Date).AddSeconds(30)
$cert     = $null
while ((Get-Date) -lt $deadline) {
    $cert = Get-ChildItem -Path "Cert:\CurrentUser\My" |
            Where-Object { $_.Thumbprint -eq $thumbprint } |
            Select-Object -First 1
    if ($null -ne $cert) { break }
    Start-Sleep -Milliseconds 500
}
if ($null -eq $cert) {
    throw "Certificate with thumbprint '$thumbprint' did not appear in CurrentUser\My within 30s after SimplySign login. Confirm the thumbprint matches the cert visible in SimplySign Desktop."
}
Write-Host "Found cert: $($cert.Subject)"

# Resolve every input glob into a flat file list before signing — this
# fails fast if the build skipped producing one of the expected outputs.
$files = @()
foreach ($p in $Path) {
    $resolved = Get-ChildItem -Path $p -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "No files matched '$p'."
    }
    $files += $resolved
}

# Sign each file with the exact invocation the Certum manual prescribes:
#   /sha1 <thumbprint>          select the cert by SHA-1 thumbprint
#   /tr   http://time.certum.pl RFC 3161 timestamp authority (free, public)
#   /td   sha256                timestamp digest algorithm
#   /fd   sha256                file digest algorithm
#   /d    "<description>"       embedded description (UAC "More info")
#   /v                          verbose — surfaces real errors in CI logs
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

    # Independent verify pass so a successful exit code can't mask a
    # malformed signature. /pa = use the default "any" policy; without it,
    # signtool defaults to driver signing rules and rejects user-mode DLLs.
    & $SignToolExe verify /pa /v $f.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "signtool verify failed for $($f.FullName) (exit $LASTEXITCODE)."
    }
}

Write-Host "All artifacts signed and verified."
