# WebGPU to Vulkan Migration Roadmap

This native renderer is being added incrementally beside the existing browser renderer. The WebGPU project remains the behavioral reference until each Vulkan stage reaches parity.

## Current WebGPU Rendering Model

- `GPUDevice` and `GPUQueue` are created in `src/main.js`.
- One compute pipeline path traces into `rawTexture` and writes accumulation, variance, depth/normal, and world-position storage buffers.
- A second compute pipeline denoises from current and previous-frame history into `denoisedTexture`.
- A fullscreen render pipeline tone maps `denoisedTexture` to the canvas.
- Bind groups are split by pass: raytracer bindings `0..17`, denoiser bindings `0..8`, and screen bindings `0..1`.
- Resize recreates all resolution-dependent textures and history buffers.

## Staged Plan

1. Vulkan bootstrap: GLFW window, Vulkan 1.3 instance/device, validation, surface, swapchain, command buffers, Synchronization2 submission, dynamic rendering clear.
2. Resource layer: VMA buffers/images, staging uploads, mapped uniform ring, image layout ownership helpers.
3. Descriptor system: descriptor layouts mirroring WebGPU bind groups, pool allocator, descriptor update helpers, optional descriptor indexing after parity.
4. Shader toolchain: GLSL equivalents for WGSL shaders, SPIR-V compilation, reflection checks against descriptor layouts.
5. Screen render pass: fullscreen triangle/quad tone mapper matching `screen.wgsl`, preserving manual gamma and `bgra8unorm` behavior.
6. Compute texture and history resources: `rgba16float` raw/denoised/history images plus packed auxiliary storage buffers.
7. Raytracer compute port: translate `raytracer.wgsl` to GLSL compute and preserve dispatch size, buffer packing, camera/env/mesh uniforms.
8. Denoiser compute port: translate temporal and a-trous denoiser logic, preserve reprojection and packed depth/normal/world-position decoding.
9. Scene upload parity: port Cornell box mesh generation, sphere primitive upload, binned SAH BVH4 packing, rope traversal data, material buffers.
10. HDR environment loading: port Radiance HDR decode or add `stb_image` for HDR input, then rebuild row/column CDF buffers.
11. Profiler: CPU timings, GPU timestamp queries, pass breakdown, and frame history in the native UI/logging layer.
12. Engine expansion: async upload queue, pipeline cache, shader hot reload, mesh import, hardware ray tracing backend.

## Architecture Proposal

- `Application`: owns the OS window and top-level loop.
- `VulkanContext`: owns instance, debug messenger, surface, physical device selection, logical device, queues, and enabled Vulkan features.
- `Swapchain`: owns swapchain images, views, format, extent, image acquisition, and resize recreation.
- `CommandSystem`: owns per-frame command pools, command buffers, fences, image-available semaphores, and render-finished semaphores.
- `ResourceAllocator`: owns VMA and will become the single creation path for GPU buffers/images.
- Future `DescriptorAllocator`, `PipelineManager`, `Scene`, `MaterialSystem`, `TextureSystem`, `HistoryResources`, and `FrameGraph` modules should be added only when their WebGPU equivalents are migrated.

## WebGPU to Vulkan Binding Strategy

The Vulkan descriptor sets will initially mirror the WebGPU bind groups one-to-one:

- Set 0, raytracer pipeline: accumulation SSBO, camera UBO, variance SSBO, storage image, packed G-buffer SSBOs, mesh SSBOs, environment sampled image/sampler/CDFs, environment UBO, sphere SSBO.
- Set 0, denoiser pipeline: input image, packed G-buffer SSBOs, history image, output storage image, denoiser UBO, world-position SSBOs, previous-camera UBO.
- Set 0, screen pipeline: denoised sampled image and camera UBO.

The first port should keep explicit bindings compatible with the WGSL numbering. Descriptor indexing can be introduced later for dynamic scene resources, but not before parity.

## Frame Flow Target

1. Acquire swapchain image.
2. Upload changed uniforms and pending staging resources.
3. Dispatch raytracer compute.
4. Barrier raw/history resources.
5. Dispatch denoiser compute or copy raw to denoised when denoising is skipped.
6. Copy current history resources for next frame.
7. Transition swapchain image and render tone-mapped fullscreen output with dynamic rendering.
8. Submit with Synchronization2 and present.

## Shader Migration Strategy

WGSL will be translated to GLSL compute/vertex/fragment for SPIR-V. The first compatibility rule is to preserve descriptor binding numbers, workgroup sizes, storage formats, and packed data layouts. Uniform structs must be audited against `std140`; storage arrays should use `std430`. The current screen pass manually applies exposure, ACES, and gamma, so the Vulkan swapchain should prefer `VK_FORMAT_B8G8R8A8_UNORM` rather than silently changing to sRGB.
