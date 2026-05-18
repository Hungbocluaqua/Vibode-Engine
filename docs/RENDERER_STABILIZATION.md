# Renderer Stabilization And Asset Pipeline

This stage keeps the compute path tracer architecture intact and adds systems needed for renderer correctness, debugging, and larger content.

## MIS And Environment Sampling

`pathtrace.comp` now uses the uploaded environment row/column CDFs directly. The shader binary-searches the row marginal CDF and the row-local column CDF, reconstructs a lat-long direction, evaluates the directional PDF, and combines it with diffuse BSDF sampling through a power heuristic.

The direct-light estimator now has separate paths for:

- area-weighted emissive triangle/sphere sampling
- environment next-event sampling
- BSDF sampling continuation
- analytical sunlight

The environment PDF follows the same convention as the WebGPU shader:

`pdf(w) = luminance(texel) * width * height * invTotalLuminance / (2*pi*pi)`

The CDF weights include the lat-long solid-angle term, so the PDF evaluation cancels the latitude Jacobian consistently with the original renderer.

## Debug Visualization

The renderer now has a permanent debug UBO and CLI switch:

`rtvulkan.exe --debug-view environment`

Supported modes:

- `variance`
- `normals`
- `reprojection`
- `rejection`
- `depth`
- `roughness`
- `direct`
- `indirect`
- `emissive`
- `environment`
- `traversal`
- `bvh`

Denoiser-owned debug views are routed through `denoiser.comp`. Path-tracer-owned views bypass denoising so the debug output is not temporally filtered.

## Asset Pipeline

The asset layer is now separate from renderer internals:

- `AssetManager` owns CPU-side texture, material, and mesh assets.
- `GltfLoader` imports glTF/GLB through tinygltf.
- `TextureAsset`, `MeshAsset`, and `MaterialAsset` expose stable handles instead of raw renderer pointers.
- `GpuScene` can build path-tracing buffers from an imported `SceneAsset`, or fall back to the Cornell-box scene.

Use:

`rtvulkan.exe --gltf path/to/scene.glb`

Imported geometry is flattened through scene-node transforms, rebuilt through the same Morton + SAH + BVH4 path, and rendered by the compute path tracer. PBR texture references are captured and tracked by the asset layer; shader-side texture material sampling is intentionally left for the future bindless transition.

## Bindless Groundwork

`BindlessResources` queries descriptor indexing support and exposes bindless material texture references. The renderer prints whether descriptor indexing and runtime descriptor arrays are available at startup. Full bindless descriptor arrays are not enabled yet because the current material shader path still uses packed material parameters.

## Indirect Groundwork

`IndirectDraw.h` defines future GPU-visible draw metadata and indexed indirect command records. These are not used by the path tracer yet, but they establish the data contract for later GPU culling, visibility passes, and raster/compute hybrid paths.

## Validation Notes

The render path now has explicit ownership points for:

- path trace storage writes to denoiser reads
- denoised image copy into history
- history image transition back to compute-readable layout
- previous world-position buffer copy after temporal reads
- path-debug output bypassing temporal filtering

`RendererValidationLog` records barrier and accumulation invalidation events for future render graph inspection tooling.
