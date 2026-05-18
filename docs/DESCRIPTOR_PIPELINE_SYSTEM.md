# Descriptor And Pipeline System

This stage establishes the GPU execution architecture for future path tracing and denoising passes.

## Descriptor Lifetime Strategy

Descriptor layouts are cached for the life of the renderer. They are immutable Vulkan objects and are safe to reuse across pipelines when the reflected binding signature matches.

Descriptor sets are allocated from per-frame descriptor arenas in `FrameResources`. The command system waits the frame fence before reusing that frame index, then `FrameResources::beginFrame()` resets the frame descriptor pools. That means a descriptor set is never reset while a command buffer that references it can still be executing.

The allocator keeps used and free pools instead of creating a new pool every frame. This amortizes pool creation while still allowing a frame with unusually many descriptors to grow.

## Reflection Strategy

GLSL is compiled to SPIR-V with `glslangValidator`. `ShaderReflection` uses SPIRV-Reflect to extract:

- descriptor set number
- binding number
- descriptor type
- descriptor array count
- shader stage visibility
- push constant offset and size

The reflected descriptor bindings are merged per set. If two shader stages declare the same binding with incompatible type or count, reflection throws before pipeline creation. This catches pipeline layout mismatches early.

## Pipeline Strategy

`PipelineCache` owns the Vulkan pipeline cache. The current cache is memory-only and passed into compute/graphics pipeline creation. Disk persistence can be added later without changing pipeline users.

`ComputePipeline` owns a `VkPipelineLayout` and `VkPipeline`. It exposes `dispatch(width, height, localX, localY)` so future compute passes can dispatch in image dimensions instead of duplicating workgroup math.

`GraphicsPipeline` owns a dynamic-rendering graphics pipeline. It uses a fullscreen triangle, dynamic viewport/scissor, no vertex buffers, no legacy render pass, and the swapchain format as the color attachment format.

## Demo Render Flow

1. Reset the current frame descriptor arena after the frame fence is signaled.
2. Transition the compute image from its tracked layout to `GENERAL`.
3. Bind the reflected compute descriptor set and compute pipeline.
4. Push animation/time constants.
5. Dispatch compute to write the storage image.
6. Transition the image from `GENERAL` to `SHADER_READ_ONLY_OPTIMAL`.
7. Begin dynamic rendering on the swapchain image.
8. Bind the reflected fullscreen descriptor set and graphics pipeline.
9. Draw a fullscreen triangle sampling the compute image.
10. End rendering and transition the swapchain image to present.

## Synchronization

The compute image transition into `GENERAL` uses compute shader destination stage/access because the next use is a storage image write.

The transition from `GENERAL` to `SHADER_READ_ONLY_OPTIMAL` uses:

- source: compute shader, shader storage write
- destination: fragment shader, shader sampled read

This makes the compute-written pixels visible to the fullscreen fragment shader.

The swapchain image remains synchronized by the command system's acquire semaphore, color attachment transition, dynamic rendering, and present transition. The compute pass does not touch the swapchain, so it can run before the color attachment wait stage without creating a swapchain dependency.

## Next Stage

The next logical migration stage is porting the WebGPU screen pass first: camera/exposure uniform, ACES tone mapping, sampled HDR render target, and exact WebGPU presentation behavior. After that, the raytracer and denoiser compute passes can use the same descriptor/pipeline infrastructure.
