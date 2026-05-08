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

#pragma once

// Extracted from helmet_overlay.cpp so the graphics-binding detection
// priority (D3D11 over D3D12) can be unit-tested without spinning up a
// real graphics device.
//
// The function inspects an XrSessionCreateInfo::next chain and returns
// the simplest backend the helmet overlay can target:
//
//   - GraphicsBindingType::D3D11 if a XR_TYPE_GRAPHICS_BINDING_D3D11_KHR
//     node is present (preferred — direct device use, no bridge).
//   - GraphicsBindingType::D3D12 if a XR_TYPE_GRAPHICS_BINDING_D3D12_KHR
//     node is present and there is no D3D11 binding (D3D11On12 bridge
//     path).
//   - GraphicsBindingType::None otherwise (Vulkan / OpenGL / no binding
//     at all — overlay degrades to bypass per CLAUDE.md rule 9).
//
// Two passes (D3D11 first, then D3D12) make the priority explicit so a
// future refactor can't silently flip it. OpenXR forbids stacking two
// graphics bindings in the same session, but we tolerate it
// defensively in case some runtime layer happens to inject extra
// debug nodes between the binding and the head of the chain.
//
// Pure function on an opaque next-chain pointer; does not call any
// graphics API, does not allocate, safe to call before any device is
// alive. Header-only because the whole logic fits in a few lines.
//
// Requires <openxr/openxr_platform.h> with both XR_USE_GRAPHICS_API_D3D11
// and XR_USE_GRAPHICS_API_D3D12 defined so the
// XR_TYPE_GRAPHICS_BINDING_D3D{11,12}_KHR enum values exist. The layer
// project's pch.h sets all of those up; test TUs that include this
// header set them up explicitly at the top of the file.

#include <openxr/openxr.h>

namespace openxr_api_layer {

    enum class GraphicsBindingType {
        None,
        D3D11,
        D3D12,
    };

    inline GraphicsBindingType detectGraphicsBindingType(const void* nextChain) {
        const auto* const head = reinterpret_cast<const XrBaseInStructure*>(nextChain);

        // Pass 1: prefer native D3D11 even if D3D12 is also present.
        for (const auto* n = head; n != nullptr; n = n->next) {
            if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                return GraphicsBindingType::D3D11;
            }
        }
        // Pass 2: fall back to D3D12 (D3D11On12 bridge will be created).
        for (const auto* n = head; n != nullptr; n = n->next) {
            if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                return GraphicsBindingType::D3D12;
            }
        }
        return GraphicsBindingType::None;
    }

}  // namespace openxr_api_layer
