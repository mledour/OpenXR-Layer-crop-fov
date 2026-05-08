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

// Unit tests for the graphics-binding detection priority extracted from
// helmet_overlay.cpp. The detection logic decides whether the helmet
// overlay should target native D3D11, the D3D11On12 bridge for D3D12
// hosts, or bypass entirely — and the priority between them is the
// only piece that can silently regress without a live GPU to notice.
//
// We hand-build XrSessionCreateInfo::next-chain layouts (sometimes
// noisy, sometimes adversarial) and assert what
// detectGraphicsBindingType() picks. No real graphics API is touched.

// The header transitively requires both XR_USE_GRAPHICS_API_D3D11 and
// XR_USE_GRAPHICS_API_D3D12 so XR_TYPE_GRAPHICS_BINDING_D3D{11,12}_KHR
// have known enum values. The layer's pch.h sets these up, but this
// test TU compiles without pch.h (per the test vcxproj — see
// PrecompiledHeader>NotUsing) so we set them up manually here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <d3d12.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <doctest/doctest.h>

#include <utils/helmet_graphics_binding.h>

using openxr_api_layer::GraphicsBindingType;
using openxr_api_layer::detectGraphicsBindingType;

// ---------------------------------------------------------------------------
// Trivial cases
// ---------------------------------------------------------------------------

TEST_CASE("detectGraphicsBindingType: nullptr next-chain returns None") {
    CHECK(detectGraphicsBindingType(nullptr) == GraphicsBindingType::None);
}

TEST_CASE("detectGraphicsBindingType: D3D11 binding alone returns D3D11") {
    XrGraphicsBindingD3D11KHR d3d11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11.next = nullptr;
    CHECK(detectGraphicsBindingType(&d3d11) == GraphicsBindingType::D3D11);
}

TEST_CASE("detectGraphicsBindingType: D3D12 binding alone returns D3D12") {
    XrGraphicsBindingD3D12KHR d3d12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    d3d12.next = nullptr;
    CHECK(detectGraphicsBindingType(&d3d12) == GraphicsBindingType::D3D12);
}

// ---------------------------------------------------------------------------
// Priority — the whole reason this helper is extracted
// ---------------------------------------------------------------------------

TEST_CASE("detectGraphicsBindingType: D3D11 wins when listed first") {
    // OpenXR forbids two graphics-binding next-structs in the same
    // session, but defending against a misbehaving runtime / future
    // layer that injects extra debug nodes is cheap. The point is that
    // the answer is *deterministic* regardless of order — the function
    // does not "first match wins".
    XrGraphicsBindingD3D12KHR d3d12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrGraphicsBindingD3D11KHR d3d11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11.next = reinterpret_cast<const XrBaseInStructure*>(&d3d12);
    d3d12.next = nullptr;
    CHECK(detectGraphicsBindingType(&d3d11) == GraphicsBindingType::D3D11);
}

TEST_CASE("detectGraphicsBindingType: D3D11 still wins when listed second") {
    // Same fixture as above but D3D12 first in the chain. D3D11 must
    // still win because the helper does TWO passes — the priority is
    // by binding type, not by chain order.
    XrGraphicsBindingD3D11KHR d3d11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrGraphicsBindingD3D12KHR d3d12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    d3d12.next = reinterpret_cast<const XrBaseInStructure*>(&d3d11);
    d3d11.next = nullptr;
    CHECK(detectGraphicsBindingType(&d3d12) == GraphicsBindingType::D3D11);
}

// ---------------------------------------------------------------------------
// Robustness against nodes the helper doesn't know about
// ---------------------------------------------------------------------------

TEST_CASE("detectGraphicsBindingType: unknown XrStructureType-only chain returns None") {
    // Stand-in for a Vulkan / OpenGL session — the helmet overlay does
    // not support those, so the helper must return None even if the
    // node looks structurally valid.
    XrBaseInStructure other{};
    other.type = static_cast<XrStructureType>(0x12345678);
    other.next = nullptr;
    CHECK(detectGraphicsBindingType(&other) == GraphicsBindingType::None);
}

TEST_CASE("detectGraphicsBindingType: walks past unrelated nodes to find the binding") {
    // Some runtimes / debug layers stack extra structs between the
    // user's binding and the head of the chain (frame-stat hooks,
    // capture markers, etc.). The helper must keep walking, not stop
    // at the first non-binding node.
    XrBaseInStructure noise1{};
    noise1.type = static_cast<XrStructureType>(0xDEADBEEF);
    XrBaseInStructure noise2{};
    noise2.type = static_cast<XrStructureType>(0xCAFEBABE);
    XrGraphicsBindingD3D12KHR d3d12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};

    noise1.next = &noise2;
    noise2.next = reinterpret_cast<XrBaseInStructure*>(&d3d12);
    d3d12.next = nullptr;

    CHECK(detectGraphicsBindingType(&noise1) == GraphicsBindingType::D3D12);
}

TEST_CASE("detectGraphicsBindingType: long chain of unrelated nodes returns None") {
    // No binding anywhere in a long chain — the helper must NOT loop
    // forever (defensive against a malformed cycle would be a separate
    // concern; this only checks the linear case finishes cleanly).
    constexpr int kChainLen = 16;
    XrBaseInStructure nodes[kChainLen]{};
    for (int i = 0; i < kChainLen; ++i) {
        nodes[i].type = static_cast<XrStructureType>(0x10000 + i);
        nodes[i].next = (i + 1 < kChainLen) ? &nodes[i + 1] : nullptr;
    }
    CHECK(detectGraphicsBindingType(&nodes[0]) == GraphicsBindingType::None);
}
