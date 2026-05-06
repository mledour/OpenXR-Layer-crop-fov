# Development

Notes for contributors and anyone building the layer from source.
End-user install and configuration are in the [README](../README.md).

## What the layer does (internal view)

The layer intercepts exactly two OpenXR calls:

- [`xrEnumerateViewConfigurationViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews) —
  scales down the `recommendedImageRectWidth` / `recommendedImageRectHeight`
  returned to the application, so smaller swapchains are allocated and the
  app renders fewer pixels per frame.
- [`xrLocateViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews) —
  narrows the `fov.angle{Left,Right,Up,Down}` values the app uses to build
  its projection matrix. The app forwards this narrowed fov to
  `xrEndFrame` naturally (via `XrCompositionLayerProjectionView::fov`), so
  the active runtime composites the image with the same frustum the app
  rendered — the black bars around the content come from the compositor
  placing that narrower frustum inside the HMD's native FOV.

Everything else (including `xrEndFrame`) passes through untouched. An
earlier design also rewrote `xrEndFrame` and tracked swapchains — we
dropped it when we confirmed the single-application path (see below)
produces correct geometry on reprojection and saves ~20pp more GPU for
the same visible crop.

## The crop math (why tan, why average)

**Factors, not percents.** A `CropConfig` stores four per-edge factors
in `[0, 1]` (one per edge). `clampFactor` converts the user-facing
`crop_*_percent` (0-50, "fraction of the image covered by the bar on
that edge") to a factor via `1 - percent/50`: 0% → factor 1.0 (no
crop), 50% → factor 0.0 (bar reaches the image center).

**Narrowing is done in tan-space, not angle-space.**
`narrowFov(fov, cfg)` computes each output edge as:

```
out.angleX = atan( tan(in.angleX) * cfg.cropXFactor )
```

The `tan` is there because an OpenXR projection is a perspective
projection: swapchain pixels are uniform in `tan(angle)`, not in angle
itself. Scaling `tan(angle)` directly by the factor is what makes a
config of `50%` land the bar exactly at the image center — if we
scaled the raw angle instead, the bar would drift off-center as the
factor gets smaller (non-linearly, because `tan` is super-linear near
π/2).

Both `tan` and `atan` are odd functions, so the OpenXR sign convention
(`angleLeft`/`angleDown` negative, `angleRight`/`angleUp` positive) is
preserved automatically. At the boundary factor = 0 the result is
exactly 0 for every edge, which is the "bar at middle" configuration
for that side.

**Swapchain scaling uses the per-axis average.**
`scaleSwapchainExtents` picks a single width factor and a single height
factor out of the four per-edge ones:

```
widthFactor  = (cropLeftFactor + cropRightFactor) / 2
heightFactor = (cropTopFactor  + cropBottomFactor) / 2
```

Average (not min, not max) is the best simple choice here:

- For a symmetric HMD FOV the average matches the tan-extent of the
  narrowed frustum exactly, so pixel density stays native (1:1).
- For strongly asymmetric FOVs (Pimax canted, WMR off-axis) the
  average can under- or over-provision by up to ~25% — acceptable as a
  conservative default; the alternative is plumbing the raw fov into
  this pure helper, which we explicitly avoid to keep `crop_math.h`
  dependency-free and trivially unit-testable.
- Using `min` would collapse the swapchain to zero as soon as one
  edge hits factor 0 (e.g. `crop_bottom_percent: 50`), which crashed
  the Pimax runtime on Le Mans Ultimate — the regression is pinned
  by the `scaleSwapchainExtents: factor 0 on a single edge` unit test
  and the `crop_bottom_percent 50 ... end-to-end` integration test.

The result is always aligned down to a multiple of 8 pixels with 8 as a
floor, so BC-compressed formats, tiled memory layouts, and the usual
DLSS/FSR tile sizes stay well-behaved, and no downstream runtime ever
receives a zero-dimension swapchain.

**Unused-but-kept helper: `computeCroppedImageRect`.**
`crop_math.h` still exposes `computeCroppedImageRect`, which maps the
narrowed tan-fov onto a `XrRect2Di` inside the swapchain. The layer no
longer uses it (that was the `xrEndFrame` sub-image path we removed),
but it's kept as a reusable utility with full unit-test coverage in
case we ever need a defensive `xrEndFrame` override for apps that
don't forward the `xrLocateViews` fov.

## Prerequisites

- Visual Studio 2019 or newer (MSBuild, NuGet)
- Python 3 in `PATH` — required by `openxr-api-layer/framework/dispatch_generator.py`
- Populated OpenXR SDK sources under `external/` (see below)
- Inno Setup 6 (only needed if you want to build the installer locally)

## Relationship to the template

The project is a copy of [`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)
with the layer-specific edits applied. Upstream changes are pulled into a
pristine vendored copy that lives outside this repository, so we can pick
up fixes without merge conflicts on our divergences.

## Building from source

Populate the `external/` submodules:

```bash
git submodule update --init --recursive
```

Then open `XR_APILAYER_MLEDOUR_fov_crop.sln` in Visual Studio and build
`Release|x64`. A pre-build step runs `openxr-api-layer/framework/dispatch_generator.py`
to regenerate `dispatch.gen.{h,cpp}` from `layer_apis.py`, and a second
pre-build script generates `openxr-api-layer.rc` from the `.rc.in`
template with the current version.

A post-build step runs `scripts\sed.exe` to substitute the `$(SolutionName)`
placeholder into `XR_APILAYER_MLEDOUR_fov_crop.json`, so the layer name
always tracks the `.sln` filename.

The test binary `openxr-api-layer-tests.exe` is built alongside the DLL
and runs unit tests (`crop_math`, `name_utils` helpers) plus integration
tests against an in-process mock OpenXR runtime.

## Releases

The GitHub Actions workflow ([`build-and-release.yml`](../.github/workflows/build-and-release.yml))
builds `Release` and `Debug` x64 on every push to `main` (as a sanity
check) and on every `v*.*.*` tag. On a tag push it additionally creates
a GitHub Release and attaches:

- `XR_APILAYER_MLEDOUR_fov_crop-<version>-x64-Setup.exe` — Inno Setup
  installer (recommended for end users).
- `XR_APILAYER_MLEDOUR_fov_crop-Release-x64.zip` — raw DLL + JSON +
  PowerShell scripts, for manual installation or development.
- `XR_APILAYER_MLEDOUR_fov_crop-Debug-x64.zip` — debug build with full
  symbols, for troubleshooting.

To publish a new release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The tag version is derived into the DLL's `VERSIONINFO` resource at
build time via [`scripts/Generate-VersionRc.ps1`](../scripts/Generate-VersionRc.ps1),
and into the installer's filename and `AppVersion` via the `/DMyAppVersion`
flag passed to ISCC.

## Code signing

Release binaries are signed with a **Certum Open Source Code Signing
Cloud** certificate (issued to the project author, valid for one year
at a time, FIPS 140-2 Level 2 cloud HSM). Both the DLL and the
`Setup.exe` installer are signed.

Anti-cheat systems may still flag any layer DLL — even a signed one —
loaded into a hooked process; SmartScreen "Unknown publisher" warnings
disappear once the signed installer accumulates enough downloads to
build reputation.

**Only tag pushes are signed.** Push-to-main, pull-request,
workflow_dispatch and fork-triggered builds all produce **unsigned**
artifacts on purpose — they're verification builds, not user-facing
ones, and signing every commit would burn a Certum cloud-HSM session
per push for no benefit. Forks additionally don't have access to the
GitHub secrets by GitHub's own design. So only binaries from the
[official GitHub Releases page](../../../releases) of this repository,
which are produced from `v*.*.*` tag pushes, should be trusted as
signed.

### How CI signs in an automated way

Certum's official manual only documents the interactive flow (open
SimplySign Desktop, type the TOTP from a phone, click "Sign in").
SimplySign Desktop **does ship an undocumented headless mode**,
discovered by reverse-engineering the binary with ILSpy:

```
SimplySignDesktop.exe /autologin <username> <totp>
```

Discovered and shared in a comment thread on
[devas.life](https://www.devas.life/how-to-automate-signing-your-windows-app-with-certum/).
Certum doesn't document this anywhere — including the official
"Code Signing in the Cloud" manual — but it's been confirmed to work
on non-interactive Windows agents (CI services running as Windows
services), which is exactly the GitHub Actions runner profile.

After `/autologin` succeeds, SimplySignDesktop runs as a background
process (no UI, no tray icon) acting as the bridge between Certum's
cloud HSM and the Windows certificate store. signtool with
`/sha1 <thumbprint>` then sees the cert through CurrentUser\My and
signs as normal.

The companion flag `/close` shuts the session down gracefully — useful
both as cleanup at the end of a run, and as a "clear leftover state"
step before a new login (Certum allows only one cloud session per
host at a time).

The pieces:

1. The workflow downloads the SimplySign Desktop MSI from
   `files.certum.eu` at a **pinned version** (`SIMPLYSIGN_VERSION`
   env var in `build-and-release.yml`) and installs it silently with
   `msiexec /qn /norestart`.
2. [`scripts/Get-CertumTotp.ps1`](../scripts/Get-CertumTotp.ps1)
   regenerates a 6-digit RFC 6238 TOTP on demand from the Base32 seed
   that the SimplySign portal exposes under "Show secret key" (the
   same string visible in the `secret=` parameter of the
   `otpauth://` enrolment URI). Pure PowerShell + .NET, no extra
   modules to install on the runner. Uses **HMAC-SHA256** — Certum's
   `otpauth://` URI specifies `algorithm=SHA256`, not the RFC 6238
   default of SHA-1. Generating SHA-1 codes against a SHA-256 seed
   produces wrong codes silently, so this is verified against RFC 6238
   Appendix B vectors in
   [`scripts/Test-CertumTotp.ps1`](../scripts/Test-CertumTotp.ps1).
3. [`scripts/Sign-Artifact.ps1`](../scripts/Sign-Artifact.ps1)
   stops any leftover SimplySignDesktop process (`/close` then kill
   if needed), then runs `/autologin <username> <totp>` and watches
   the launched process: a successful login keeps the process alive
   indefinitely (the bridge is now active), a failed login makes it
   exit within a couple of seconds. We probe for ~5 s before deciding.
   On failure we retry with the previous and next TOTP windows
   (drift offsets `-1`, `+1` × 30 s) to absorb runner-clock skew of
   up to ±30 s. If all three fail, we give up with a clear actionable
   message. After login, we poll `Cert:\CurrentUser\My` for the
   configured thumbprint to confirm the bridge is live, then run
   `signtool sign /sha1 <thumb> /tr http://time.certum.pl /td sha256
   /fd sha256` exactly as the Certum manual prescribes, then
   `signtool verify /pa /v`. A `finally` block runs `/close` so the
   runner doesn't leave a lingering session that would block the
   next CI run.
4. The workflow gates every signing-related step (MSI cache restore,
   MSI install, sign DLL, sign Setup.exe) on a `should_sign` flag
   computed once up front. The flag is true iff **all three** of:
   - `GITHUB_REF` matches `refs/tags/v*` (we're on a release tag),
   - the matrix entry is `Release` (Debug never signs), and
   - the `CERTUM_USERNAME` secret is set (PR/fork builds without
     access to secrets fall through cleanly).

   Computing it once and only once keeps the per-step `if:`
   conditions tidy and avoids referencing `secrets.*` inside step-
   level `if:` expressions, which GitHub Actions does not allow.

5. The MSI itself is cached via `actions/cache@v4` keyed on
   `SIMPLYSIGN_VERSION`. The first signed-release run after a
   version bump downloads the ~50-100 MB installer; subsequent runs
   on the same version reuse the cached bytes. The msiexec install
   step always runs — we deliberately do **not** cache the installed
   `Program Files\Certum\SimplySign Desktop\` tree because the
   install registers components the cert-store bridge depends on,
   and a plain directory copy would skip that registration.

### Brittleness notes

The `/autologin` flag is undocumented, so a future SimplySign Desktop
release could rename or remove it. Mitigations baked in:

- **Version-pinned MSI download.** The workflow installs a specific
  `SIMPLYSIGN_VERSION` (currently `9.4.3.90`) — Certum can't push a
  silent breaking change. Bumping is a deliberate step, see below.
- **Process-state probe.** A bad OTP or a removed `/autologin` flag
  both manifest as the launched process exiting within a couple of
  seconds; we detect that and fail fast with a labelled error
  instead of "succeeding" with an unsigned binary.
- **Cert-store poll.** Even if `/autologin` somehow returned 0
  without genuinely logging in, the cert wouldn't appear in
  `CurrentUser\My`, and we'd fail before signtool ever runs.

When bumping `SIMPLYSIGN_VERSION` in the workflow:

1. Run `scripts\Sign-Artifact.ps1` against the new MSI on a local
   Windows VM with the secrets set. Verify a sample DLL signs.
2. If `/autologin` no longer behaves the same way, fall back to
   self-hosted runner + interactive login (see "Why not just SendKeys"
   below) until a fix is found.
3. Then bump the env var in `.github/workflows/build-and-release.yml`.

### Why not just SendKeys

We tried first. Certum's GUI login window does have a recognizable
title (`SimplySign Desktop`) and tab order (username → Tab → OTP →
Enter), so a `WScript.Shell.AppActivate` + `SendKeys` flow looks
plausible on paper, and works on a developer's interactive desktop
(per [devas.life](https://www.devas.life/how-to-automate-signing-your-windows-app-with-certum/)).
On a `windows-2022` GitHub-hosted runner, however, SimplySign
Desktop creates its windows hidden by default (it's tray-app
flavored), and force-showing them via `ShowWindow(SW_RESTORE)` was
observed to *destroy* the window outright — runs left the
SimplySign* PID owning only `Default IME` windows. After 60 s of
patient waiting, the login dialog never auto-appeared either.
`/autologin` sidesteps the entire UI lifecycle.

### Required GitHub Secrets

These three secrets must be configured at the repository level
(Settings → Secrets and variables → Actions → New repository secret)
for Release-tag builds to produce signed binaries. They are **never**
echoed by `Sign-Artifact.ps1` and `Get-CertumTotp.ps1`, and the
PowerShell scripts pass them as process arguments rather than through
`cmd /c` so they don't leak into the shell-history transcript.

Certum SimplySign uses 2FA where the **TOTP is the second factor** —
there is no separate static password to set, so we don't need a
`CERTUM_PASSWORD` secret. Username + freshly-generated TOTP is the
full credential set SimplySign Desktop expects.

| Secret | Source | Format |
|--------|--------|--------|
| `CERTUM_USERNAME` | SimplySign portal login (the email you registered with) | string |
| `CERTUM_TOTP_SEED` | SimplySign portal → "Show secret key" (the Base32 string behind the QR code, NOT a snapshot of the current 6-digit code) | Base32, 16+ chars |
| `CERTUM_CERT_THUMBPRINT` | SHA-1 thumbprint of the issued certificate, no spaces | 40 hex chars |

To find the thumbprint once the cert is loaded into your local
SimplySign Desktop session:

```powershell
Get-ChildItem Cert:\CurrentUser\My |
    Where-Object Subject -Match '<your-name>' |
    Format-List Thumbprint, Subject, NotAfter
```

Re-run that command after each Certum certificate renewal — the
thumbprint changes with every new cert, so the secret has to be
updated. The renewal cadence is yearly for the Open Source tier.

### Renewing the seed / rotating credentials

If the TOTP seed leaks (or you suspect it has), reset it from the
SimplySign portal: a new seed invalidates the old one immediately.
Update `CERTUM_TOTP_SEED` in the GitHub Secrets in the same window
or the next CI run will fail to log in.

## License

MIT License — see [LICENSE](../LICENSE).

Based on the [OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`), Copyright © 2022–2023. The
framework code (dispatch generator, entry point, logging, graphics
helpers) is his work; the `fov_crop` logic in `layer.cpp` / `layer.h`
is this project.
