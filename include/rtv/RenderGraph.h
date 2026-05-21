#pragma once

#include "rtv/RenderGraphPass.h"
#include "rtv/RenderGraphResource.h"

#include <Volk/volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

struct RenderGraphBarrier {
    RenderGraphResourceId resource{};
    uint32_t beforePass = 0;
    uint32_t afterPass = 0;
    ResourceAccess before{};
    ResourceAccess after{};
};

class RenderGraph {
public:
    RenderGraphResourceId createTexture(const RenderGraphResource& desc);
    RenderGraphResourceId createBuffer(const RenderGraphResource& desc);
    RenderGraphPass& addPass(const char* name);

    void compile();
    void execute(VkCommandBuffer commandBuffer, uint64_t frameIndex = 0);
    void emitCompiledBarriers(VkCommandBuffer commandBuffer) const;
    void reset();

    [[nodiscard]] std::vector<RenderGraphResource>& resources() { return resources_; }
    [[nodiscard]] const std::vector<RenderGraphResource>& resources() const { return resources_; }
    [[nodiscard]] const std::vector<RenderGraphPass>& passes() const { return passes_; }
    [[nodiscard]] const std::vector<uint32_t>& compiledPassOrder() const { return compiledPassOrder_; }
    [[nodiscard]] const std::vector<RenderGraphBarrier>& compiledBarriers() const { return compiledBarriers_; }
    [[nodiscard]] bool compiled() const { return compiled_; }

private:
    RenderGraphResourceId addResource(RenderGraphResource desc);

    std::vector<RenderGraphResource> resources_;
    std::vector<RenderGraphPass> passes_;
    std::vector<uint32_t> compiledPassOrder_;
    std::vector<RenderGraphBarrier> compiledBarriers_;
    bool compiled_ = false;
};

} // namespace rtv
