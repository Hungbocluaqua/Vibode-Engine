# Vulkan Resource System

This stage adds the resource foundation needed by the path tracer without migrating descriptors, shaders, or scene logic yet.

## Ownership

- `ResourceAllocator` owns the VMA allocator and exposes Vulkan device/physical-device handles needed by resource wrappers.
- `Buffer` owns one `VkBuffer` and one `VmaAllocation`. It is movable, non-copyable, and destroys through VMA.
- `Image` owns one `VkImage`, one `VmaAllocation`, and one default `VkImageView`. It is movable, non-copyable, and tracks the current layout.
- `UploadContext` owns transient upload command resources and synchronizes completion with a fence.
- `BufferUploader` is stateless orchestration around staging buffers and upload commands.
- `ResourceDemo` is a validation demo, not an engine subsystem. It exists only to exercise uploads, layout transitions, storage images, and destruction order before descriptors/shaders are introduced.

## Barrier Rationale

Buffer uploads:

- Source: `VK_PIPELINE_STAGE_2_COPY_BIT`, `VK_ACCESS_2_TRANSFER_WRITE_BIT`
- Destination: `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`, `VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT`

The upload command writes device-local memory through a transfer operation. The broad destination scope is intentional at this stage because the same upload path will initialize vertex, index, uniform, storage, and indirect buffers before their specialized users exist.

Texture uploads:

- `UNDEFINED` or tracked old layout to `TRANSFER_DST_OPTIMAL`
- Source stage/access: none when previous contents are discarded
- Destination stage/access: copy/transfer write

The image starts with no valid contents, so no previous memory needs to be made visible. The destination access makes the transfer write valid.

Texture use in the demo:

- `SHADER_READ_ONLY_OPTIMAL` to `TRANSFER_SRC_OPTIMAL`
- Source: shader sampled read
- Destination: blit transfer read

The image is descriptor-ready after upload. The demo temporarily borrows it as a transfer source so it can be shown without adding shaders/descriptors in this stage.

Storage image demo:

- tracked old layout to `GENERAL`
- Destination: clear transfer write

`GENERAL` is the required long-term storage-image layout for compute writes. The demo clears it with `vkCmdClearColorImage`, which is a transfer-style write and validates that the image can be used as a storage-capable render resource.

Swapchain demo:

- `COLOR_ATTACHMENT_OPTIMAL` to `TRANSFER_DST_OPTIMAL`
- Source: color attachment write
- Destination: blit transfer write
- Then `TRANSFER_DST_OPTIMAL` to `PRESENT_SRC_KHR`

The dynamic-rendering clear writes the swapchain image first. The uploaded texture blit writes over part of that image. Presentation must see all transfer writes complete before the present operation.

## Descriptor Readiness

`Buffer::descriptorInfo()` returns `VkDescriptorBufferInfo`.

`Image::sampledDescriptor()` returns `VkDescriptorImageInfo` with `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.

`Image::storageDescriptor()` returns `VkDescriptorImageInfo` with `VK_IMAGE_LAYOUT_GENERAL`.

The descriptor system should consume these directly in the next stage rather than reaching into raw allocations.
