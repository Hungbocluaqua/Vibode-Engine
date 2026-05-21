#pragma once

#include <Volk/volk.h>

#include <cstdint>
#include <limits>

namespace rtv {

struct RenderGraphResourceId {
    uint32_t index = std::numeric_limits<uint32_t>::max();

    [[nodiscard]] bool valid() const {
        return index != std::numeric_limits<uint32_t>::max();
    }
};

enum class ResourceState : uint8_t {
    Undefined,
    PreRasterization,
    ColorAttachment,
    DepthStencilAttachment,
    ShaderRead,
    ShaderStorage,
    UniformBuffer,
    RayTracing,
    ComputeShaderRead,
    ComputeShaderStorage,
    TransferSource,
    TransferDest,
    Present,
};

enum class PassAccess : uint8_t {
    Read,
    Write,
    ReadWrite,
};

enum class PipelineDomain : uint8_t {
    Graphics,
    Compute,
    RayTracing,
    Transfer,
};

struct ResourceAccess {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct RenderGraphResource {
    enum class Type : uint8_t {
        Texture,
        Buffer,
    };

    enum class Lifetime : uint8_t {
        Transient,
        Persistent,
        Temporal,
    };

    Type type = Type::Texture;
    Lifetime lifetime = Lifetime::Transient;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{1, 1, 1};
    VkDeviceSize size = 0;
    VkImageUsageFlags usage = 0;
    VkBufferUsageFlags bufferUsage = 0;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImageSubresourceRange imageRange{};
    bool external = false;
    bool hasInitialAccess = false;
    ResourceAccess initialAccess{};
    bool hasFinalAccess = false;
    ResourceAccess finalAccess{};
    const char* debugName = nullptr;
};

[[nodiscard]] ResourceAccess resourceAccessFor(ResourceState state, PipelineDomain domain);

} // namespace rtv
