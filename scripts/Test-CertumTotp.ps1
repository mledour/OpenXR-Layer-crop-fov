# Test-CertumTotp.ps1
#
# Offline smoke test for Get-CertumTotp.ps1 against the RFC 6238
# Appendix B reference vectors (SHA-1 column). No network, no Certum,
# no real secrets — just confirms the algorithm matches the standard.
#
# Run before relying on the CI signing flow; if these four checks pass,
# the TOTP we feed SimplySign Desktop is correct in principle, and any
# auth failure can be blamed on credentials/clock skew/network rather
# than the math.
#
# Usage (from the repo root):
#   pwsh -File scripts\Test-CertumTotp.ps1
#
# Exit codes: 0 on success, 1 if any vector fails.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$gen       = Join-Path $scriptDir 'Get-CertumTotp.ps1'

# RFC 6238 secret: ASCII "12345678901234567890" (20 bytes).
# Base32-encoded (no padding) is the seed below.
$seed = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ'

# (Time, expected 8-digit code) tuples from RFC 6238 Appendix B.
# We truncate to 6 digits because Certum's tokens are 6-digit.
$vectors = @(
    @{ Time = 59;         Expected8 = '94287082'; Expected6 = '287082' },
    @{ Time = 1111111109; Expected8 = '07081804'; Expected6 = '081804' },
    @{ Time = 1234567890; Expected8 = '89005924'; Expected6 = '005924' },
    @{ Time = 2000000000; Expected8 = '69279037'; Expected6 = '279037' }
)

$failed = 0
foreach ($v in $vectors) {
    $got8 = & $gen -Base32Seed $seed -Digits 8 -Time $v.Time
    $got6 = & $gen -Base32Seed $seed -Digits 6 -Time $v.Time
    $ok8  = $got8 -eq $v.Expected8
    $ok6  = $got6 -eq $v.Expected6
    if ($ok8 -and $ok6) {
        Write-Host ("OK   T={0,10}  6={1}  8={2}" -f $v.Time, $got6, $got8)
    } else {
        Write-Host ("FAIL T={0,10}  6={1} (want {2})  8={3} (want {4})" -f `
                   $v.Time, $got6, $v.Expected6, $got8, $v.Expected8)
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Error "$failed of $($vectors.Count) RFC 6238 vectors failed."
    exit 1
}
Write-Host "All $($vectors.Count) RFC 6238 vectors passed."
