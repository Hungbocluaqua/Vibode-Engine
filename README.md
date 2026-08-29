# Vulkan Ray Tracing Engine

A Vulkan 1.3 and C++20 real-time path tracing engine with an ImGui editor,
RenderGraph, ReSTIR DI/GI, denoising, temporal upscaling, streaming, and
headless diagnostics.

## Requirements

- Windows with a Vulkan ray tracing capable GPU and current driver
- Vulkan SDK with Volk and `glslangValidator`
- CMake 3.27 or newer and Visual Studio 2022
- Package dependencies discoverable by CMake: GLFW, GLM, VMA, stb,
  nlohmann-json, ImGuizmo, SPIRV-Reflect, KTX, TIFF, tinygltf, and ImGui

The ImGui source directory can be supplied with `-DIMGUI_SOURCE_DIR=<path>`.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Launch with `run_rtvulkan.bat`. The editor starts without bundled projects or
scenes; pass your own `.rtlevel`, glTF, HDR, or project paths through the CLI or
editor.

## Optional SDKs

Optional integrations are intentionally not vendored. Configure them with:

- `-DRENDERDOC_SDK_DIR=<path>`
- `-DNRD_SDK_DIR=<path>`
- `-DDLSS_SDK_DIR=<path>`
- `-DSTREAMLINE_SDK_DIR=<path>`
- `-DDIRECTSTORAGE_SDK_DIR=<path>`
- `-DTINYOBJLOADER_INCLUDE_DIRS=<path>` with `-DRTV_ENABLE_TINYOBJ_IMPORTER=ON`

The engine builds without these optional SDKs and falls back to its built-in
runtime paths where supported.

---

[Support me on Ko-fi](https://ko-fi.com/hungchayqua)
