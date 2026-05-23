#pragma once

#include "rtv/RenderGraphPass.h"
#include "rtv/RenderGraphResource.h"

#include <Volk/volk.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

class ResourceAllocator;

struct RenderGraphBarrier {
    RenderGraphResourceId resource{};
    uint32_t beforePass = 0;
    uint32_t afterPass = 0;
    ResourceAccess before{};
    ResourceAccess after{};
};

struct TransientResourceLifetime {
    uint32_t resourceIndex = 0;
    uint32_t firstUsePass = UINT32_MAX;
    uint32_t lastUsePass = UINT32_MAX;
    bool aliased = false;
    uint32_t aliasGroup = 0;
};

class TransientResourcePool {
public:
    explicit TransientResourcePool(ResourceAllocator& allocator);
    ~TransientResourcePool();

    VkImage acquireOrCreateImage(uint32_t aliasGroup, const RenderGraphResource& desc);
    VkBuffer acquireOrCreateBuffer(uint32_t aliasGroup, const RenderGraphResource& desc);
    void releaseImage(uint32_t aliasGroup);
    void releaseBuffer(uint32_t aliasGroup);
    void beginFrame();
    void destroyAll();

    [[nodiscard]] size_t imageCount() const { return imagePool_.size(); }
    [[nodiscard]] size_t bufferCount() const { return bufferPool_.size(); }
    [[nodiscard]] size_t aliasGroupCount() const { return aliasImages_.size(); }
    [[nodiscard]] size_t totalBytesAllocated() const { return totalBytesAllocated_; }
    [[nodiscard]] size_t activeBytes() const { return activeBytes_; }
    [[nodiscard]] size_t peakBytes() const { return peakBytes_; }
    [[nodiscard]] float fragmentationRatio() const { return aliasImages_.size() > 1 ? static_cast<float>(aliasImages_.size() - 1) / static_cast<float>(imagePool_.size()) : 0.0f; }

private:
    std::unordered_map<uint32_t, VkImage> aliasImages_;
    std::unordered_map<uint32_t, VkBuffer> aliasBuffers_;
    std::vector<VkImage> imagePool_;
    std::vector<VkBuffer> bufferPool_;
    ResourceAllocator* allocator_ = nullptr;
    size_t totalBytesAllocated_ = 0;
    size_t activeBytes_ = 0;
    size_t peakBytes_ = 0;
};

class RenderGraph {
public:
    explicit RenderGraph(ResourceAllocator* allocator = nullptr);

    RenderGraphResourceId createTexture(const RenderGraphResource& desc);
    RenderGraphResourceId createBuffer(const RenderGraphResource& desc);
    RenderGraphPass& addPass(const char* name);
    bool removePass(const char* name);

    void compile();
    void execute(VkCommandBuffer commandBuffer, uint64_t frameIndex = 0);
    void emitCompiledBarriers(VkCommandBuffer commandBuffer) const;
    void reset();

    [[nodiscard]] std::vector<RenderGraphResource>& resources() { return resources_; }
    [[nodiscard]] const std::vector<RenderGraphResource>& resources() const { return resources_; }
    [[nodiscard]] const std::vector<RenderGraphPass>& passes() const { return passes_; }
    [[nodiscard]] const std::vector<uint32_t>& compiledPassOrder() const { return compiledPassOrder_; }
    [[nodiscard]] const std::vector<RenderGraphBarrier>& compiledBarriers() const { return compiledBarriers_; }
    [[nodiscard]] const std::vector<TransientResourceLifetime>& resourceLifetimes() const { return resourceLifetimes_; }
    [[nodiscard]] bool compiled() const { return compiled_; }

    [[nodiscard]] bool hasAsyncCompute() const { return transientPool_.operator bool(); }
    void setAsyncComputeQueue(VkQueue queue, uint32_t familyIndex);
    void setTimelineSemaphore(VkSemaphore semaphore);
    [[nodiscard]] uint64_t nextTimelineValue();
    void executeAsync(VkCommandBuffer graphicsCommandBuffer, VkCommandBuffer computeCommandBuffer, uint64_t frameIndex = 0);

private:
    RenderGraphResourceId addResource(RenderGraphResource desc);

    std::vector<RenderGraphResource> resources_;
    std::vector<RenderGraphPass> passes_;
    std::vector<uint32_t> compiledPassOrder_;
    std::vector<RenderGraphBarrier> compiledBarriers_;
    std::vector<TransientResourceLifetime> resourceLifetimes_;
    std::unique_ptr<TransientResourcePool> transientPool_;
    VkQueue asyncComputeQueue_ = VK_NULL_HANDLE;
    uint32_t asyncComputeFamily_ = UINT32_MAX;
    VkSemaphore timelineSemaphore_ = VK_NULL_HANDLE;
    uint64_t timelineValue_ = 0;
    bool compiled_ = false;
};

} // namespace rtv
