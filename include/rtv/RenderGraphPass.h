#pragma once

#include "rtv/RenderGraphResource.h"

#include <Volk/volk.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rtv {

class RenderGraph;

struct FrameGraphContext {
    uint64_t frameIndex = 0;
    RenderGraph* graph = nullptr;
};

struct RenderGraphResourceUse {
    RenderGraphResourceId resource{};
    ResourceState state = ResourceState::Undefined;
    PassAccess access = PassAccess::Read;
    PipelineDomain domain = PipelineDomain::Graphics;
};

enum class RenderGraphQueueDomain : uint8_t {
    Graphics,
    RayTracing,
    Compute,
    Transfer,
    SameFamilyCompute,
};

class RenderGraphPass {
public:
    using ExecuteCallback = std::function<void(FrameGraphContext&, VkCommandBuffer)>;

    explicit RenderGraphPass(std::string name = {});

    RenderGraphPass& addColorAttachment(RenderGraphResourceId id);
    RenderGraphPass& addInputAttachment(RenderGraphResourceId id);
    RenderGraphPass& addStorageRead(RenderGraphResourceId id, PipelineDomain domain);
    RenderGraphPass& addStorageWrite(RenderGraphResourceId id, PipelineDomain domain);
    RenderGraphPass& addStorageReadWrite(RenderGraphResourceId id, PipelineDomain domain);
    RenderGraphPass& addUniformBuffer(RenderGraphResourceId id, PipelineDomain domain);
    RenderGraphPass& setQueueDomain(RenderGraphQueueDomain domain);
    RenderGraphPass& setExecuteCallback(ExecuteCallback callback);

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::vector<RenderGraphResourceUse>& uses() const { return uses_; }
    [[nodiscard]] RenderGraphQueueDomain queueDomain() const { return queueDomain_; }
    [[nodiscard]] const ExecuteCallback& callback() const { return callback_; }

private:
    RenderGraphPass& addUse(RenderGraphResourceId id, ResourceState state, PassAccess access, PipelineDomain domain);

    std::string name_;
    std::vector<RenderGraphResourceUse> uses_;
    RenderGraphQueueDomain queueDomain_ = RenderGraphQueueDomain::Graphics;
    bool queueDomainExplicit_ = false;
    ExecuteCallback callback_;
};

} // namespace rtv
