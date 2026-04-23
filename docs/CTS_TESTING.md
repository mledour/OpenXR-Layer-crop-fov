# CTS testing

Playbook for running the [OpenXR Conformance Test Suite][cts-repo] against
this layer. The goal is to catch any case where the layer would make a
conformant runtime look non-conformant to an application — a hard
requirement for any OpenXR API layer. See also the [OpenXR Loader
specification][loader-spec] for the layer-loader contract this relies on.

Everything in this document is **manual** for now. The in-repo PR gate
(mock runtime + doctest) covers the contract surface cheaply on every
push. The CTS complements it by catching the things the mock cannot see:
real loader behaviour, real asymmetric FOVs, real swapchains and real
compositors.

## When to run what

| Moment | Target | Tool |
|---|---|---|
| Every PR | Contract-level regressions | Mock runtime tests (CI, automatic) |
| Pre-release (before a `v*.*.*` tag) | Full conformance on the platform users will run | **Pimax native OpenXR**, parallel projection OFF |
| Pre-release, if a failure is ambiguous | Is this my layer or a Pimax runtime quirk? | Pimax via SteamVR OpenXR (triangulation) |
| Iterating without the HMD powered on | Quick sanity | SteamVR + null driver (optional) |
| Conformance submission (hypothetical) | Full matrix | All three runtimes + multiple graphics plugins |

**The primary release gate is Pimax native.** That is the runtime your
users run; a green CTS there is the only thing that ships. The other
two runtimes in this document are conveniences — install them only
when you need them.

## Get the CTS binary

### Prebuilt (recommended)

Khronos publishes tagged releases:

- https://github.com/KhronosGroup/OpenXR-CTS/releases

Pick the latest `openxr-cts-*-win64.zip` that matches the
`XR_CURRENT_API_VERSION` of this layer's SDK submodule (currently 1.0.x).
The archive contains `conformance_cli.exe` and `conformance_test.exe`
plus their graphics-plugin DLLs (`conformance_test_D3D11.dll`,
`conformance_test_D3D12.dll`, `conformance_test_Vulkan2.dll`,
`conformance_test_OpenGL.dll`).

Extract somewhere permanent (e.g. `C:\OpenXR-CTS\`). The CTS does not
install itself into any system path.

### Build from source

Only worth it if you need to patch the CTS or run a non-released version:

```powershell
git clone --recursive https://github.com/KhronosGroup/OpenXR-CTS
cd OpenXR-CTS
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j
```

Output lands in `build\src\conformance\conformance_test\Release\`.

## Runtime setup

Only one OpenXR runtime can be "active" at a time — the CTS talks to
whichever runtime is currently registered under
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`. The three configurations
below cover progressively-broader needs; most releases only need (1).

### 1. Pimax native OpenXR runtime (primary, release gate)

**Cost**: Pimax Crystal / 12K / newer + Pimax Client. Older 5K+/8K
models do not ship a native OpenXR runtime — skip to (2) in that case.
**Coverage**: real asymmetric FOVs, real GPU path, real compositor,
and it is what end users actually run. A green CTS here is the only
runtime signal that matters for a release tag.
**Blind spot**: the Pimax runtime is newer and less battle-tested than
SteamVR's, so a CTS failure could be a Pimax bug rather than a layer
bug. Use (2) to disambiguate when that happens.

**Setup**

1. Launch Pimax Client. Confirm the HMD is detected and tracking.
2. **Turn parallel projection OFF** in Pimax Client →
   Display/Advanced. With parallel projection ON, the driver hides the
   native per-eye asymmetry behind a symmetric emulation, and you lose
   the main signal the Pimax gives you for testing this layer.
3. Pimax Client → Settings → **Set Pimax OpenXR Runtime as active**.
4. Verify:
   ```powershell
   Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1" ActiveRuntime
   # Must point to the Pimax runtime's JSON, not SteamVR's.
   ```
5. Leave SteamVR closed — running both simultaneously sometimes
   confuses the Pimax driver.

**Sanity check**

Wake the HMD by setting it face-up on your desk so the proximity
sensor and IMU come alive. No need to wear it — the mirror window
shows what the CTS renders.

```powershell
cd C:\OpenXR-CTS
.\hello_xr.exe -G D3D11
```

A mirror window with a rendered scene (cube on dark background)
should open and keep running. If `hello_xr` crashes or exits with
`XR_ERROR_RUNTIME_UNAVAILABLE`, fix this before touching the CTS — no
amount of CTS output will be interpretable on a broken runtime.

If the mirror window renders but the Pimax panel stays off, check
Pimax Client's "wear detection" setting — some firmware gates
rendering on the proximity sensor.

### 2. Pimax via SteamVR OpenXR (optional triangulation)

**When to install this**: you hit a CTS failure on Pimax native and
can't tell whether the layer or the Pimax runtime is at fault. SteamVR
OpenXR is heavily battle-tested, so a test that passes under SteamVR
OpenXR but fails under Pimax native points at the Pimax runtime, not
your layer. Skip this section unless that situation arises.

**Setup**

1. Install Steam and SteamVR from the Steam client.
2. Leave Pimax Client's parallel projection at the same setting as
   your Pimax-native run (OFF for the comparison to be meaningful).
3. Launch SteamVR (Pimax Client should auto-launch it). Confirm the
   HMD is green in the SteamVR status window.
4. SteamVR → Settings → Developer → **Set SteamVR as OpenXR Runtime**.
5. Verify:
   ```powershell
   Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1" ActiveRuntime
   # Now points to SteamVR's JSON instead of Pimax's.
   ```

Run the CTS the same way as for (1). The output is directly comparable.

To switch back to Pimax native, use Pimax Client's runtime toggle — do
not edit the registry by hand; Pimax Client and SteamVR both rewrite
it on launch otherwise.

### 3. SteamVR with the null driver (optional, no-HMD)

**When to install this**: you want to iterate on a CTS-surfaced bug
without powering the HMD on, or you are onboarding a contributor who
does not own a Pimax. Skip otherwise.

**Cost**: free, no HMD required.
**Coverage**: contract-level correctness — enumeration, handle
lifetimes, error codes, instance/session lifecycle, extension
interaction.
**Blind spot**: the null driver reports symmetric FOVs and does not
exercise a real GPU pipeline. Any bug that only shows up on asymmetric
FOVs (very likely for this layer) will pass here silently. Never
substitute this for (1) on a release gate.

**Setup**

1. Install Steam and SteamVR from the Steam client.
2. Edit
   `"%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\drivers\null\resources\settings\default.vrsettings"`:
   - Set `"enable": true` in the `driver_null` section.
3. Edit
   `"%ProgramFiles(x86)%\Steam\steamapps\common\SteamVR\resources\settings\default.vrsettings"`:
   - Set `"activateMultipleDrivers": true` and `"forcedDriver": "null"`
     in the `steamvr` section.
4. Launch SteamVR once. Ignore the "no HMD detected" warning — the null
   driver exposes a virtual head-mounted system that satisfies the
   OpenXR runtime.
5. SteamVR → Settings → Developer → **Set SteamVR as OpenXR Runtime**.
6. Sanity-check with `hello_xr.exe -G D3D11` as above.

## Install or disable the layer

Unlike install/uninstall cycles (which need admin elevation), the OpenXR
loader honours the `disable_environment` field of the JSON manifest. For
this layer the variable is:

```
DISABLE_XR_APILAYER_MLEDOUR_fov_crop=1
```

Install the layer **once** in an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Layer.ps1
```

Then toggle with the env var per-shell:

```powershell
# Baseline: pretend the layer isn't there.
$env:DISABLE_XR_APILAYER_MLEDOUR_fov_crop = "1"
# ... run CTS, capture baseline.xml ...

# With the layer:
Remove-Item Env:\DISABLE_XR_APILAYER_MLEDOUR_fov_crop
# ... run CTS, capture with_layer.xml ...
```

This is the reliable way to produce two directly-comparable runs in the
same session with the same runtime.

## The canonical CTS invocation

```powershell
cd C:\OpenXR-CTS
.\conformance_cli.exe `
  -G D3D11 `
  --reporter "junit::out=baseline.xml" `
  "~[interactive]"
```

Flag-by-flag (cross-checked against CTS 1.0.34 `--help`):

- `-G D3D11` — graphics plugin. **Always run D3D11 first** — it is the
  most robust. Once it passes, repeat for `D3D12` and `Vulkan2`. Long
  form is `--graphicsPlugin D3D11`.
- `--reporter "junit::out=FILE.xml"` — machine-readable XML output
  (Catch2 v3 `name::key=value` syntax). Keep this for the diff.
- `"~[interactive]"` — **critical**. Excludes the Catch2 tag
  `[interactive]`, which covers tests that require a human to put the
  HMD on and move it. Without this filter the CTS will appear to hang
  on the first interactive test.

The CTS does NOT accept an `--apiVersion` flag — the API version is
determined from the binary itself. Adding one makes the parser reject
everything that follows, and you end up with empty XMLs that the diff
script will happily report as "no regressions" unless you have the
`Test-Path` guard (see below).

Before the first real run, sanity-check the argument line with
`--list-tests` (runs nothing, just prints the inventory):

```powershell
.\conformance_cli.exe -G D3D11 --list-tests
```

If that produces a clean list, your flags are accepted.

Expected runtime on `~[interactive]`: ~30–60 min on a real HMD,
~20–40 min on SteamVR null.

## The golden rule: diff baseline vs with-layer

**Never judge a CTS run in isolation.** Every runtime + graphics plugin
combination produces a handful of pre-existing failures (Pimax has
known quirks, null driver skips things). A single run with failures
tells you nothing. The only defensible signal is: *did this layer cause
tests that passed without it to now fail with it?*

Always run the pair, back-to-back, on the same runtime, in the same
session.

```powershell
# Baseline
$env:DISABLE_XR_APILAYER_MLEDOUR_fov_crop = "1"
.\conformance_cli.exe -G D3D11 `
    --reporter "junit::out=baseline.xml" "~[interactive]"

# With layer
Remove-Item Env:\DISABLE_XR_APILAYER_MLEDOUR_fov_crop
.\conformance_cli.exe -G D3D11 `
    --reporter "junit::out=with_layer.xml" "~[interactive]"

# Guard: fail loudly if the CTS didn't produce output. Without this,
# Compare-Object on two null inputs reports "no regressions" and you
# would ship thinking the CTS passed.
if (-not (Test-Path baseline.xml)) {
    throw "baseline.xml missing — CTS did not run. Check arguments."
}
if (-not (Test-Path with_layer.xml)) {
    throw "with_layer.xml missing — CTS did not run. Check arguments."
}

# Diff: which testcases FAILED with the layer but not in the baseline?
$base = Select-Xml -Path baseline.xml   -XPath "//testcase[failure]" |
        ForEach-Object { $_.Node.name } | Sort-Object
$with = Select-Xml -Path with_layer.xml -XPath "//testcase[failure]" |
        ForEach-Object { $_.Node.name } | Sort-Object
$regressions = Compare-Object $base $with -IncludeEqual |
               Where-Object SideIndicator -eq '=>' |
               ForEach-Object InputObject

if ($regressions) {
    "REGRESSIONS (failed only with the layer installed):"
    $regressions
    throw "CTS regressed under layer — do not tag."
} else {
    "No regressions. Layer does not break tests the runtime already passes."
}
```

Save this as `scripts\Run-CTS-Diff.ps1` if you end up running it often.

## Tests to watch closely for `fov_crop`

The layer touches `xrEnumerateViewConfigurationViews`, `xrLocateViews`,
`xrCreateSwapchain`, `xrDestroySwapchain`, and historically `xrEndFrame`.
Regressions most often surface in these Catch2 tags:

- `[xrEnumerateViewConfigurationViews]` — the CTS checks the contract
  for two-call enumeration. A bug in `scaleSwapchainExtents` that
  returns different counts on the probe vs real call will be caught
  here.
- `[xrLocateViews]` — the CTS asserts cross-call consistency. Any hidden
  assumption of FOV symmetry will surface on a real Pimax here.
- `[xrEndFrame]` — validation of submitted composition layers. If the
  layer ever rewrites `XrCompositionLayerProjectionView::fov` or
  `subImage.imageRect` inconsistently with the runtime's expectations,
  this category catches it.
- `[XR_KHR_composition_layer_*]` — extension-level composition tests.
  These often exercise combinations the minimal `hello_xr` path never
  reaches.
- `[interaction]` + `[scenario]` — broader end-to-end. Usually noise on
  null, meaningful on Pimax.

## Visual sanity check

After any CTS-passing run, spend 30 seconds on `hello_xr` **on the
HMD**, with the layer installed and an aggressive config (20/20/25/25 in
`settings.json`). The CTS cannot catch:

- Visible head-tilt deformation (the memo-in-memory bug class).
- Jitter or swimming at the cropped edges.
- Wrong-eye FOVs (both eyes see the same crop where they shouldn't).
- Black-bar asymmetry — on a Pimax with parallel projection OFF, the
  black bars should follow the asymmetric native FOV, *not* look
  mirror-symmetric.

The cube in `hello_xr` must sit inside a visibly smaller rendered area,
surrounded by true-black bars. Anything else — warped, shifted,
flickering — is a bug the CTS missed.

## Troubleshooting

**Pimax runtime refuses to load the layer.**
Pimax native runtime reads HKLM like any conformant loader, so this
should not happen. If it does: Pimax Client → Settings → verify
"Enable API layers" is on (if such a toggle exists on your version),
then reboot. Some Pimax builds cache the layer list at install time.

**Pimax shows the HMD in standby during the CTS run.**
Wake it by moving the HMD — some Pimax models gate the display on
proximity and IMU motion. Once the HMD is green in Pimax Client, rerun
the CTS; the failed run is noise.

**`conformance_cli.exe` hangs on a specific test.**
Probably an `[interactive]` test that slipped past your filter. Check
that `"~[interactive]"` is quoted — PowerShell eats the tilde
otherwise. If it still hangs, filter to the category:
`"[xrLocateViews]"` instead.

**Every test reports SKIPPED.**
Either no OpenXR runtime is active
(`Get-ItemProperty HKLM:\SOFTWARE\Khronos\OpenXR\1 ActiveRuntime`) or
you specified a graphics plugin (`-G`) whose DLL is missing from the
CTS directory.

**"API layer is not present" from the CTS startup log.**
The layer was disabled via env var or never installed. Verify with:
```powershell
Get-ItemProperty "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit\*"
```
Re-run `Install-Layer.ps1` from an elevated PowerShell if the output is
empty.

**The diff script lists a test as a "regression" but manually running it
in isolation passes.**
Catch2 tests share state across a run. The regression is real but its
root cause might be a preceding test that the layer mutates global
state for. Re-run with `--order rand --rng-seed N` and bisect with
`--start-at TESTNAME`.

## Release checklist

Before pushing a `v*.*.*` tag:

1. [ ] Mock-runtime tests green on the PR branch's latest CI run.
2. [ ] **Pimax native OpenXR**, parallel projection OFF, D3D11, diff
       empty. This is the required gate.
3. [ ] `hello_xr` on the HMD with a 20/20/25/25 crop looks right (black
       bars, no deformation, no jitter).
4. [ ] (If a diff surfaced a failure you can't pin down): re-run under
       SteamVR OpenXR to triangulate before either fixing the layer or
       filing a Pimax-runtime bug.
5. [ ] Attach `baseline.xml` + `with_layer.xml` to the GitHub release
       notes for traceability.

If step 2 fails, do not tag. Either fix the layer or — if the failure
is pre-existing in the runtime — document it in the release notes as a
known quirk and get a second opinion before shipping.

[cts-repo]: https://github.com/KhronosGroup/OpenXR-CTS
[loader-spec]: https://registry.khronos.org/OpenXR/specs/1.0/loader.html
