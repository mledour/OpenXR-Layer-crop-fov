// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Microbench: measures the layer's per-call CPU overhead with no game, no
// runtime variability, no thermal/temporal drift. Runs against the in-process
// mock_runtime so the only CPU work in the loop is (a) the layer's own
// override + (b) the mock's nearly-empty implementation. The mock cost is
// identical between any two builds, so it cancels out in the relative
// comparison performed by tools/compare_bench.py — what changes between
// builds is the layer-side delta, which is what we want to track.
//
// Each TEST_CASE emits one line on stdout in the format
//   MICROBENCH:<key>:<float-ns-per-call>
// which compare_bench.py greps for. The doctest's normal pass/fail still
// reports through CHECK on a sanity bound (we measured something positive),
// but the actual regression decision is made by comparing PR vs main on the
// SAME runner, in the same job — see .github/workflows/microbench.yml.
//
// Why no absolute threshold here: GitHub-hosted runners are shared Azure
// VMs with ~10-30 % run-to-run variance depending on host load and CPU
// generation. An absolute "ns/call < N" check would flake. The relative
// comparison sidesteps that entirely.

#include "pch.h"

#include "mock_runtime.h"
#include "test_fixture.h"

#include <layer.h>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using openxr_api_layer::OpenXrApi;
using namespace test_fixture;

namespace {

// Iteration counts. 200k is enough that QPC granularity (~100 ns) is
// dwarfed by the cumulative time and the per-iteration noise from
// cache/branch-predictor effects has averaged out. We keep it at 200k
// (rather than 1M) so the same TEST_CASEs are cheap enough to also run
// inside the existing Release+Debug matrix in build-and-release.yml
// without bloating that workflow's runtime — Debug builds the layer
// without inlining/optimisation and per-call cost can be 10× a Release
// build, so 1M would be ~2 s × 3 cases × 2 configs in the matrix run.
//
// 200k × ~150 ns Release ≈ 30 ms per case, ×3 ≈ 100 ms total — under
// the noise floor of the existing test suite. The dedicated microbench
// workflow (Release-only) gets ~6× more samples per second of runtime
// than this default budget would suggest, which is plenty for the
// relative comparison the workflow does against main.
constexpr int kWarmupIterations = 5'000;
constexpr int kMeasureIterations = 200'000;

// Sentinel format consumed by tools/compare_bench.py. One line per metric,
// stdout, flushed immediately so a fatal in a later TEST_CASE doesn't
// swallow the lines we already produced.
void emitMetric(const char* key, double nanosecondsPerCall) {
    std::printf("MICROBENCH:%s:%.3f\n", key, nanosecondsPerCall);
    std::fflush(stdout);
}

// Generic timing helper. Runs the lambda `iterations` times under
// QueryPerformanceCounter and returns the average wall-clock time per
// iteration in nanoseconds.
template <typename Fn>
double timeNsPerCall(int iterations, Fn&& fn) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < iterations; ++i) {
        fn();
    }
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    const double elapsedNs =
        double(t1.QuadPart - t0.QuadPart) * 1e9 / double(freq.QuadPart);
    return elapsedNs / double(iterations);
}

// xrLocateViews driver — extracted so the steady-state and live-edit cases
// share the exact same hot loop. Calling this exercises:
//   - The OpenXrApi virtual dispatch
//   - OpenXrLayer::xrLocateViews (live_edit pending check + cache lookup +
//     OpenXrApi forward + (cache hit) the FOV cache hit early-out path)
//   - mock::m_xrLocateViews (writes the configured locateFovs into views)
auto makeLocateViewsCall(OpenXrApi* layer) {
    static XrSession s_fakeSession =
        reinterpret_cast<XrSession>(static_cast<uintptr_t>(0xCAFE));
    return [layer]() {
        uint32_t count = 0;
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        layer->xrLocateViews(s_fakeSession, &li, &viewState, 2, &count, views);
    };
}

} // namespace

// ---------------------------------------------------------------------------
// xrLocateViews steady-state CPU cost. live_edit OFF (production default).
// FOV cache is warm after the warmup loop so each measured call is a cache
// hit — exactly what the layer does on every frame after the first.
// ---------------------------------------------------------------------------

TEST_CASE("microbench: xrLocateViews steady-state (live_edit off, cache hit)") {
    LayerFixture fx;
    fx.writeSettings(R"({
        "enabled": true,
        "live_edit": false,
        "crop_left_percent": 10,
        "crop_right_percent": 10,
        "crop_top_percent": 10,
        "crop_bottom_percent": 10
    })");
    mock::state().viewCount = 2;
    const XrFovf fov = defaultFov();
    mock::state().locateFovs = {fov, fov};

    auto* layer = fx.boot();
    auto call = makeLocateViewsCall(layer);

    for (int i = 0; i < kWarmupIterations; ++i) call();

    const double ns = timeNsPerCall(kMeasureIterations, call);
    emitMetric("xrLocateViews_steady_ns", ns);

    // Sanity: we did at least one nontrivial measurement. The actual
    // regression decision is made by compare_bench.py against main.
    CHECK(ns > 0.0);
    CHECK(ns < 100'000.0);  // Catches a catastrophic 100 µs/call regression.
}

// ---------------------------------------------------------------------------
// xrLocateViews with live_edit ON. The watcher thread is running at 1 Hz
// BELOW_NORMAL but in steady state has no pending swap, so the frame-thread
// fast path is the same atomic-load + cache hit as above. Documenting this
// number explicitly so a future change that accidentally moves work onto
// the frame thread (e.g. taking m_liveEditMutex unconditionally) shows up
// as a delta even though the steady-state path is unchanged.
// ---------------------------------------------------------------------------

TEST_CASE("microbench: xrLocateViews with live_edit watcher running") {
    // Use a long poll interval so the watcher never fires during the bench
    // — we want to measure the frame-thread steady state, not the swap
    // path. setLiveEditPollIntervalForTesting(60s) gives us the whole
    // bench duration with zero watcher wakes.
    openxr_api_layer::setLiveEditPollIntervalForTesting(std::chrono::seconds(60));

    LayerFixture fx;
    fx.writeSettings(R"({
        "enabled": true,
        "live_edit": true,
        "crop_left_percent": 10,
        "crop_right_percent": 10,
        "crop_top_percent": 10,
        "crop_bottom_percent": 10
    })");
    mock::state().viewCount = 2;
    const XrFovf fov = defaultFov();
    mock::state().locateFovs = {fov, fov};

    auto* layer = fx.boot();
    auto call = makeLocateViewsCall(layer);

    for (int i = 0; i < kWarmupIterations; ++i) call();

    const double ns = timeNsPerCall(kMeasureIterations, call);
    emitMetric("xrLocateViews_live_edit_ns", ns);

    CHECK(ns > 0.0);
    CHECK(ns < 100'000.0);

    // Restore production default for any later TEST_CASE.
    openxr_api_layer::setLiveEditPollIntervalForTesting(std::chrono::milliseconds(1000));
}

// ---------------------------------------------------------------------------
// xrEndFrame fast-path CPU cost. helmet_overlay disabled, no QUAD layers,
// so OpenXrLayer::xrEndFrame should hit the early-out at line 582 of
// layer.cpp ("Fast path: nothing to strip and nothing to add — forward
// untouched") and forward to the runtime without copying the layer array.
//
// We pass a stereo projection layer with garbage swapchain handles — the
// layer never dereferences swapchain in the fast path, only checks layer
// types for the QUAD-strip scan.
// ---------------------------------------------------------------------------

TEST_CASE("microbench: xrEndFrame fast-path (helmet off, no QUAD strip)") {
    LayerFixture fx;
    fx.writeSettings(R"({
        "enabled": true,
        "live_edit": false,
        "helmet_overlay": { "enabled": false }
    })");
    mock::state().viewCount = 2;
    mock::state().locateFovs = {defaultFov(), defaultFov()};

    auto* layer = fx.boot();

    // Build a stereo projection layer. The layer's fast path inspects only
    // base->type, so the rest of the fields can be defaulted; we still
    // populate them sensibly so the mock's view-copy doesn't see garbage
    // floats.
    XrCompositionLayerProjectionView projViews[2] = {};
    for (int i = 0; i < 2; ++i) {
        projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projViews[i].pose.orientation.w = 1.0f;
        projViews[i].fov = defaultFov();
        projViews[i].subImage.swapchain =
            reinterpret_cast<XrSwapchain>(static_cast<uintptr_t>(0xBEE0 + i));
        projViews[i].subImage.imageRect.offset = {0, 0};
        projViews[i].subImage.imageRect.extent = {1000, 1000};
        projViews[i].subImage.imageArrayIndex = 0;
    }
    XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projLayer.viewCount = 2;
    projLayer.views = projViews;
    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer),
    };
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = 1;
    info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    info.layerCount = 1;
    info.layers = layers;

    XrSession fakeSession = reinterpret_cast<XrSession>(static_cast<uintptr_t>(0xCAFE));
    auto call = [&]() {
        layer->xrEndFrame(fakeSession, &info);
    };

    for (int i = 0; i < kWarmupIterations; ++i) call();

    const double ns = timeNsPerCall(kMeasureIterations, call);
    emitMetric("xrEndFrame_fastpath_ns", ns);

    CHECK(ns > 0.0);
    CHECK(ns < 100'000.0);
}
