# GPU Scene And Path Tracer Migration

This stage replaces the compute demo as the active native render path. It keeps the WebGPU renderer's compute-driven identity: a compute pass writes HDR radiance, accumulation state, and compact auxiliary buffers, then a fullscreen pass presents the result.

## Migrated GPU Binding Shape

The Vulkan path tracing shader keeps the WebGPU raytracer binding numbers where possible:

- `0`: accumulation SSBO, `vec4` per pixel
- `1`: camera UBO
- `2`: packed variance SSBO, `uint` per pixel
- `3`: `rgba16f` storage image output
- `4`: packed depth/normal SSBO, `uvec2` per pixel
- `5`: packed world-position SSBO, `uvec2` per pixel
- `6`: mesh vertex SSBO
- `7`: mesh index SSBO
- `8`: packed BVH node SSBO
- `9`: triangle data SSBO
- `10`: material SSBO
- `11`: mesh parameter UBO
- `12`: environment sampled image
- `13`: environment sampler
- `14`: environment row CDF SSBO
- `15`: environment column CDF SSBO
- `16`: environment parameter UBO
- `17`: sphere primitive SSBO

This keeps the descriptor shape compatible with the WebGPU architecture and gives the later denoiser migration the same auxiliary inputs.

## Scene Upload

`GpuScene` creates a Cornell-box style scene with colored walls, an emissive ceiling panel, and three sphere primitives. It uploads:

- packed vertices
- indices
- packed BVH nodes
- leaf-ordered triangle records
- packed material records
- packed sphere records
- procedural environment texture
- environment row and column CDF buffers
- mesh and environment parameter UBOs

The BVH upload uses the existing four-`vec4` node representation. The native builder now mirrors the WebGPU build path:

- 30-bit Morton codes sort triangle references deterministically.
- A 12-bin SAH split evaluates all centroid axes and falls back to longest-axis median only when bins degenerate.
- The binary SAH tree collapses into BVH4-style packed nodes.
- Ropes are generated across packed children so shader traversal stays stackless by entering `child0` and following sibling ropes.

The packed node layout stays compatible with the WGSL contract: `d0.xyz`/`d1.xyz` are bounds, `d0.w` is the leaf flag, `d1.w` is `rope + 1`, `d2.xyzw` stores leaf triangle range or up to four children, and `d3.x/d3.y` stores child count and the first Morton code.

## Shader Migration

`pathtrace.comp` is the first GLSL migration of `raytracer.wgsl`. It preserves:

- compute dispatch at `16x16`
- progressive accumulation
- compact variance packing
- octahedral normal packing
- camera-relative world-position packing
- BVH buffer traversal
- sphere primitive traversal
- emissive mesh lighting
- analytical sunlight
- sampled environment lighting
- descriptor-driven scene access

It does not yet preserve the full WebGPU material model or MIS path exactly. GGX, dielectric/refraction parity, full emissive-light MIS, and using the environment CDF for next-event sampling inside the path tracer remain follow-up work before visual parity can be claimed.

## Temporal Denoiser

`denoiser.wgsl` has been migrated to `denoiser.comp` as a compute pass after path tracing. It preserves the compact inputs:

- packed variance: one `uint` per pixel
- packed depth plus octahedral normal: one `uvec2` per pixel
- camera-relative packed world position: one `uvec2` per pixel
- previous-frame packed world position

The pass performs variance-guided multi-scale a-trous filtering, world-position reprojection, bilinear history sampling, luminance clipping, reactive masking, normal/depth disocclusion confidence, and confidence-based temporal blending. The first frame bypasses temporal reads so newly-created history resources do not affect output.

## HDR Environment

The environment path now uses `rgba16f` images and shared CDF generation through `EnvironmentImportanceSampler`. `HdrLoader` supports Radiance `.hdr`/`.pic` loading through stb_image and vertically flips loaded images to keep the existing shader convention: row 0 is the lower hemisphere. The native app accepts `--hdr path/to/environment.hdr`; loaded HDR data is uploaded to the environment image and the row/column CDF buffers are rebuilt from the same pixels. If `--hdr` is omitted, the renderer keeps the procedural fallback.

## GPU Profiling

The Vulkan renderer records timestamp queries around path tracing, denoising, and fullscreen presentation using `vkCmdWriteTimestamp2`. Each frame-in-flight owns its query pool, so query reset and reuse happen only after that frame's fence has completed. Smoothed timings are retained by the renderer and printed periodically in the native run loop.

## Synchronization

The path tracing pass transitions the HDR output image to `GENERAL` before compute writes:

- destination stage: compute shader
- destination access: shader storage write

After dispatch, the raw image stays in `GENERAL` and is made visible to the denoiser:

- source stage/access: compute shader storage write
- destination stage/access: compute shader storage read

The denoised image transitions from `GENERAL` to transfer source for the history copy, then to `SHADER_READ_ONLY_OPTIMAL` for fullscreen presentation. The history image transitions back to `GENERAL` for next-frame compute reads.

World-position history is copied after the denoiser consumes the previous history buffer. The copy is followed by a transfer-write to compute-read barrier so next-frame reprojection sees valid data.

## Accumulation

`PathTracerRenderer` recreates resolution-dependent resources on resize and resets `frameCount` to zero. The shader treats frame 1 as a fresh accumulation frame and ignores previous accumulation contents, so explicit clearing is not required for correctness after recreation or reset.

The camera is currently static. When native camera controls are added, movement/settings changes should reset `frameCount` exactly like the WebGPU app does.

## Current Validation Status

The native path tracing plus temporal denoiser path builds and runs validation-clean for an 8-frame smoke test on the RTX 4070 Ti SUPER.
