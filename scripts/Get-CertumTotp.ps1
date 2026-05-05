# Get-CertumTotp.ps1
#
# Generates a 6-digit RFC 6238 TOTP code from a Base32-encoded shared secret.
# Pure PowerShell + .NET — no external dependencies, no PowerShell modules
# to install on the runner.
#
# Used by CI to authenticate non-interactively to Certum SimplySign Cloud:
#   Sign-Artifact.ps1 calls this once per signing session, feeds the result
#   to SimplySignDesktop.exe's /login flow, and signtool then sees the cert
#   through the Windows certificate store.
#
# The seed is the Base32 string Certum shows behind the QR code in the
# SimplySign portal under "Show secret key". Treat it as you would the
# certificate's private key — anyone with the seed can mint TOTPs forever
# (until reset). It MUST live only in a GitHub Secret, never in the repo.
#
# RFC 6238 defaults are used: HMAC-SHA1, 30s step, 6 digits, Unix epoch.
#
# Usage:
#   $totp = & .\scripts\Get-CertumTotp.ps1 -Base32Seed $env:CERTUM_TOTP_SEED
#
# Tested cross-checks against:
#   - https://totp.danhersam.com/  (paste seed + read code at the same wall
#     clock; must match)
#   - oathtool --totp -b <seed>
#

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Base32Seed,

    # Override only for unit tests against RFC 6238 vectors.
    [int]    $Digits  = 6,
    [int]    $Period  = 30,
    [long]   $Time    = -1
)

$ErrorActionPreference = 'Stop'

function ConvertFrom-Base32 {
    param([Parameter(Mandatory)] [string] $Text)

    # Strip whitespace and padding; uppercase. Certum displays the seed in
    # groups of 4 chars separated by spaces — that has to round-trip.
    $clean = ($Text -replace '\s', '' -replace '=', '').ToUpperInvariant()
    if ($clean.Length -eq 0) {
        throw "Base32 seed is empty after stripping whitespace/padding."
    }

    $alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'
    $bits     = New-Object System.Text.StringBuilder

    foreach ($ch in $clean.ToCharArray()) {
        $idx = $alphabet.IndexOf($ch)
        if ($idx -lt 0) {
            throw "Invalid Base32 character '$ch' in seed."
        }
        [void]$bits.Append([Convert]::ToString($idx, 2).PadLeft(5, '0'))
    }

    $bitStr  = $bits.ToString()
    $byteLen = [Math]::Floor($bitStr.Length / 8)
    $bytes   = New-Object byte[] $byteLen
    for ($i = 0; $i -lt $byteLen; $i++) {
        $chunk    = $bitStr.Substring($i * 8, 8)
        $bytes[$i] = [Convert]::ToByte($chunk, 2)
    }
    return ,$bytes
}

# Resolve the time counter (RFC 6238 section 4.2: T = floor((now - T0) / X)).
if ($Time -lt 0) {
    $Time = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}
$counter = [long]([Math]::Floor($Time / $Period))

# Pack counter as 8-byte big-endian (RFC 4226 section 5.1).
$counterBytes = [BitConverter]::GetBytes($counter)
if ([BitConverter]::IsLittleEndian) {
    [Array]::Reverse($counterBytes)
}

$keyBytes = ConvertFrom-Base32 -Text $Base32Seed

$hmac = [System.Security.Cryptography.HMACSHA1]::new($keyBytes)
try {
    $hash = $hmac.ComputeHash($counterBytes)
} finally {
    $hmac.Dispose()
}

# Dynamic truncation (RFC 4226 section 5.3).
$offset  = $hash[$hash.Length - 1] -band 0x0F
$binCode = (([int]$hash[$offset]     -band 0x7F) -shl 24) -bor `
           (([int]$hash[$offset + 1] -band 0xFF) -shl 16) -bor `
           (([int]$hash[$offset + 2] -band 0xFF) -shl 8)  -bor `
            ([int]$hash[$offset + 3] -band 0xFF)

$modulo = [Math]::Pow(10, $Digits)
$code   = ($binCode % $modulo).ToString().PadLeft($Digits, '0')
return $code
