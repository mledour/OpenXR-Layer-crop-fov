// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
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

// The FOV-crop portion of this layer does no graphics work of its own —
// it only modifies OpenXR data structures. The helmet-overlay portion
// (utils/helmet_overlay.cpp) does need D3D11 to render its quad, so
// XR_USE_GRAPHICS_API_D3D11 + <d3d11.h> are pulled in here. To avoid
// forcing d3d11.dll / d3dcompiler_47.dll to be loaded into every
// OpenXR game process (even Vulkan games like X-Plane 12 that would
// never exercise the overlay), the vcxproj delay-loads those DLLs —
// they are only actually loaded when the overlay path runs.

// Standard library.
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstring>
#include <ctime>
#define _USE_MATH_DEFINES
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <optional>
#include <unordered_map>

using namespace std::chrono_literals;

// Windows header files.
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <wrl.h>
#include <wil/resource.h>
#include <traceloggingactivity.h>
#include <traceloggingprovider.h>

// D3D11 is used by the helmet-overlay path in utils/helmet_overlay.cpp.
// These includes are harmless for every other TU (types only, no symbol
// references) and the d3d11.dll / d3dcompiler_47.dll DLLs are delay-loaded
// at link time so they are not mapped into host processes that never
// exercise the overlay.
#include <d3d11.h>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

// OpenXR + Windows-specific definitions.
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// OpenXR loader interfaces. Promoted to a public header in OpenXR 1.1;
// was <loader_interfaces.h> under src/common/ in 1.0.
#include <openxr/openxr_loader_negotiation.h>

// OpenXR/DirectX utilities.
#include <XrError.h>
#include <XrMath.h>
#include <XrSide.h>
#include <XrStereoView.h>
#include <XrToString.h>

// FMT formatter.
#include <fmt/format.h>
