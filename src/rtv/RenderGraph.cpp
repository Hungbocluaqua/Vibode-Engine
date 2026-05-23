#include "rtv/RenderGraph.h"

#include "rtv/ResourceAllocator.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <stdexcept>

namespace rtv {

TransientResourcePool::TransientResourcePool(ResourceAllocator& allocator)
    : allocator_(&allocator) {}

TransientResourcePool::~TransientResourcePool() {
    destroyAll();
}

VkImage TransientResourcePool::acquireOrCreateImage(uint32_t aliasGroup, const RenderGraphResource& desc) {
    auto it = aliasImages_.find(aliasGroup);
    if (it != aliasImages_.end()) {
        return it->second;
    }
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = desc.format;
    imageInfo.extent = desc.extent;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = desc.usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator_->handle(), &imageInfo, &allocInfo, &image, &allocation, nullptr) == VK_SUCCESS) {
        aliasImages_[aliasGroup] = image;
        imagePool_.push_back(image);
        const size_t estimatedBytes = desc.extent.width * desc.extent.height * std::max(desc.extent.depth, 1u) * 4u;
        totalBytesAllocated_ += estimatedBytes;
        activeBytes_ += estimatedBytes;
        peakBytes_ = std::max(peakBytes_, activeBytes_);
    }
    return image;
}

VkBuffer TransientResourcePool::acquireOrCreateBuffer(uint32_t aliasGroup, const RenderGraphResource& desc) {
    auto it = aliasBuffers_.find(aliasGroup);
    if (it != aliasBuffers_.end()) {
        return it->second;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = desc.bufferUsage;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(allocator_->handle(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) == VK_SUCCESS) {
        aliasBuffers_[aliasGroup] = buffer;
        bufferPool_.push_back(buffer);
        activeBytes_ += desc.size;
        totalBytesAllocated_ += desc.size;
        peakBytes_ = std::max(peakBytes_, activeBytes_);
    }
    return buffer;
}

void TransientResourcePool::releaseImage(uint32_t aliasGroup) {
    auto it = aliasImages_.find(aliasGroup);
    if (it != aliasImages_.end()) {
        vmaDestroyImage(allocator_->handle(), it->second, VK_NULL_HANDLE);
        aliasImages_.erase(it);
        if (activeBytes_ > 0) activeBytes_ = activeBytes_ > 256 ? activeBytes_ - 256 : 0;
    }
}

void TransientResourcePool::releaseBuffer(uint32_t aliasGroup) {
    auto it = aliasBuffers_.find(aliasGroup);
    if (it != aliasBuffers_.end()) {
        vmaDestroyBuffer(allocator_->handle(), it->second, VK_NULL_HANDLE);
        aliasBuffers_.erase(it);
        if (activeBytes_ > 1024) activeBytes_ -= 1024;
    }
}

void TransientResourcePool::beginFrame() {
    destroyAll();
    peakBytes_ = 0;
}

void TransientResourcePool::destroyAll() {
    for (VkImage img : imagePool_) {
        vmaDestroyImage(allocator_->handle(), img, VK_NULL_HANDLE);
    }
    for (VkBuffer buf : bufferPool_) {
        vmaDestroyBuffer(allocator_->handle(), buf, VK_NULL_HANDLE);
    }
    imagePool_.clear();
    bufferPool_.clear();
    aliasImages_.clear();
    aliasBuffers_.clear();
    totalBytesAllocated_ = 0;
    activeBytes_ = 0;
}

namespace {

bool writesResource(const RenderGraphResourceUse& use) {
    return use.access == PassAccess::Write || use.access == PassAccess::ReadWrite;
}

bool readsResource(const RenderGraphResourceUse& use) {
    return use.access == PassAccess::Read || use.access == PassAccess::ReadWrite;
}

void beginDebugLabel(VkCommandBuffer commandBuffer, const char* name) {
    if (vkCmdBeginDebugUtilsLabelEXT == nullptr || name == nullptr || name[0] == '\0') {
        return;
    }
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0] = 0.20f;
    label.color[1] = 0.55f;
    label.color[2] = 1.00f;
    label.color[3] = 1.00f;
    vkCmdBeginDebugUtilsLabelEXT(commandBuffer, &label);
}

void endDebugLabel(VkCommandBuffer commandBuffer) {
    if (vkCmdEndDebugUtilsLabelEXT != nullptr) {
        vkCmdEndDebugUtilsLabelEXT(commandBuffer);
    }
}

void insertDebugBreadcrumb(VkCommandBuffer commandBuffer, const char* name) {
    if (vkCmdInsertDebugUtilsLabelEXT == nullptr || name == nullptr || name[0] == '\0') {
        return;
    }
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    label.color[0] = 1.00f;
    label.color[1] = 0.30f;
    label.color[2] = 0.30f;
    label.color[3] = 1.00f;
    vkCmdInsertDebugUtilsLabelEXT(commandBuffer, &label);
}

void emitBarrier(VkCommandBuffer commandBuffer, const RenderGraphResource& resource, const RenderGraphBarrier& barrier) {
    if (resource.type == RenderGraphResource::Type::Texture && resource.image != VK_NULL_HANDLE) {
        if (barrier.before.stage == barrier.after.stage &&
            barrier.before.access == barrier.after.access &&
            barrier.before.layout == barrier.after.layout) {
            return;
        }

        VkImageMemoryBarrier2 imageBarrier{};
        imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        imageBarrier.srcStageMask = barrier.before.stage;
        imageBarrier.srcAccessMask = barrier.before.access;
        imageBarrier.dstStageMask = barrier.after.stage;
        imageBarrier.dstAccessMask = barrier.after.access;
        imageBarrier.oldLayout = barrier.before.layout;
        imageBarrier.newLayout = barrier.after.layout;
        imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image = resource.image;
        imageBarrier.subresourceRange = resource.imageRange;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &imageBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    } else if (resource.type == RenderGraphResource::Type::Buffer && resource.buffer != VK_NULL_HANDLE) {
        if (barrier.before.stage == barrier.after.stage &&
            barrier.before.access == barrier.after.access) {
            return;
        }

        VkBufferMemoryBarrier2 bufferBarrier{};
        bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        bufferBarrier.srcStageMask = barrier.before.stage;
        bufferBarrier.srcAccessMask = barrier.before.access;
        bufferBarrier.dstStageMask = barrier.after.stage;
        bufferBarrier.dstAccessMask = barrier.after.access;
        bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufferBarrier.buffer = resource.buffer;
        bufferBarrier.offset = 0;
        bufferBarrier.size = resource.size == 0 ? VK_WHOLE_SIZE : resource.size;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &bufferBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }
}

} // namespace

RenderGraph::RenderGraph(ResourceAllocator* allocator) {
    if (allocator != nullptr) {
        transientPool_ = std::make_unique<TransientResourcePool>(*allocator);
    }
}

ResourceAccess resourceAccessFor(ResourceState state, PipelineDomain domain) {
    switch (state) {
    case ResourceState::Undefined:
        return {};
    case ResourceState::PreRasterization:
        return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ResourceState::ColorAttachment:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    case ResourceState::DepthStencilAttachment:
        return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    case ResourceState::ShaderRead:
        return {domain == PipelineDomain::Compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ResourceState::ShaderStorage:
        return {domain == PipelineDomain::Compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::UniformBuffer:
        return {domain == PipelineDomain::Compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_UNIFORM_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
    case ResourceState::RayTracing:
        return {VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::ComputeShaderRead:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    case ResourceState::ComputeShaderStorage:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
    case ResourceState::TransferSource:
        return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    case ResourceState::TransferDest:
        return {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    case ResourceState::Present:
        return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    }
    return {};
}

RenderGraphPass::RenderGraphPass(std::string name)
    : name_(std::move(name)) {}

RenderGraphPass& RenderGraphPass::addColorAttachment(RenderGraphResourceId id) {
    return addUse(id, ResourceState::ColorAttachment, PassAccess::Write, PipelineDomain::Graphics);
}

RenderGraphPass& RenderGraphPass::addInputAttachment(RenderGraphResourceId id) {
    return addUse(id, ResourceState::ShaderRead, PassAccess::Read, PipelineDomain::Graphics);
}

RenderGraphPass& RenderGraphPass::addStorageRead(RenderGraphResourceId id, PipelineDomain domain) {
    const ResourceState state = domain == PipelineDomain::Transfer
        ? ResourceState::TransferSource
        : (domain == PipelineDomain::Compute ? ResourceState::ComputeShaderRead : ResourceState::ShaderRead);
    return addUse(id, state, PassAccess::Read, domain);
}

RenderGraphPass& RenderGraphPass::addStorageWrite(RenderGraphResourceId id, PipelineDomain domain) {
    const ResourceState state = domain == PipelineDomain::Transfer
        ? ResourceState::TransferDest
        : (domain == PipelineDomain::Compute ? ResourceState::ComputeShaderStorage : ResourceState::ShaderStorage);
    return addUse(id, state, PassAccess::Write, domain);
}

RenderGraphPass& RenderGraphPass::addStorageReadWrite(RenderGraphResourceId id, PipelineDomain domain) {
    const ResourceState state = domain == PipelineDomain::Transfer
        ? ResourceState::TransferDest
        : (domain == PipelineDomain::Compute ? ResourceState::ComputeShaderStorage : ResourceState::ShaderStorage);
    return addUse(id, state, PassAccess::ReadWrite, domain);
}

RenderGraphPass& RenderGraphPass::addUniformBuffer(RenderGraphResourceId id, PipelineDomain domain) {
    return addUse(id, ResourceState::UniformBuffer, PassAccess::Read, domain);
}

RenderGraphPass& RenderGraphPass::setExecuteCallback(ExecuteCallback callback) {
    callback_ = std::move(callback);
    return *this;
}

RenderGraphPass& RenderGraphPass::addUse(RenderGraphResourceId id, ResourceState state, PassAccess access, PipelineDomain domain) {
    if (!id.valid()) {
        throw std::runtime_error("RenderGraphPass received an invalid resource id");
    }
    uses_.push_back(RenderGraphResourceUse{
        .resource = id,
        .state = state,
        .access = access,
        .domain = domain,
    });
    return *this;
}

RenderGraphResourceId RenderGraph::createTexture(const RenderGraphResource& desc) {
    RenderGraphResource texture = desc;
    texture.type = RenderGraphResource::Type::Texture;
    return addResource(std::move(texture));
}

RenderGraphResourceId RenderGraph::createBuffer(const RenderGraphResource& desc) {
    RenderGraphResource buffer = desc;
    buffer.type = RenderGraphResource::Type::Buffer;
    return addResource(std::move(buffer));
}

RenderGraphResourceId RenderGraph::addResource(RenderGraphResource desc) {
    const RenderGraphResourceId id{static_cast<uint32_t>(resources_.size())};
    resources_.push_back(desc);
    compiled_ = false;
    return id;
}

RenderGraphPass& RenderGraph::addPass(const char* name) {
    passes_.emplace_back(name != nullptr ? name : "");
    compiled_ = false;
    return passes_.back();
}

bool RenderGraph::removePass(const char* name) {
    if (name == nullptr) return false;
    const auto it = std::find_if(passes_.begin(), passes_.end(),
        [name](const RenderGraphPass& pass) { return pass.name() == name; });
    if (it == passes_.end()) return false;
    passes_.erase(it);
    compiled_ = false;
    return true;
}

void RenderGraph::compile() {
    compiledPassOrder_.clear();
    compiledBarriers_.clear();
    if (passes_.empty()) {
        compiled_ = true;
        return;
    }

    std::vector<uint8_t> live(passes_.size(), 0);
    std::vector<uint8_t> neededResources(resources_.size(), 0);
    for (uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
        if (resources_[resourceIndex].external || resources_[resourceIndex].lifetime == RenderGraphResource::Lifetime::Temporal) {
            neededResources[resourceIndex] = 1;
        }
    }

    for (uint32_t passIndex = static_cast<uint32_t>(passes_.size()); passIndex-- > 0;) {
        bool passNeeded = false;
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            if (writesResource(use) && use.resource.index < neededResources.size() && neededResources[use.resource.index] != 0) {
                passNeeded = true;
            }
        }
        if (!passNeeded) {
            continue;
        }
        live[passIndex] = 1;
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            if (readsResource(use) && use.resource.index < neededResources.size()) {
                neededResources[use.resource.index] = 1;
            }
        }
    }

    std::vector<std::vector<uint32_t>> edges(passes_.size());
    std::vector<uint32_t> indegree(passes_.size(), 0);
    std::vector<uint32_t> lastWriter(resources_.size(), std::numeric_limits<uint32_t>::max());

    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        if (live[passIndex] == 0) {
            continue;
        }
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            if (use.resource.index >= resources_.size()) {
                throw std::runtime_error("RenderGraph pass references a missing resource");
            }
            const uint32_t writer = lastWriter[use.resource.index];
            if (writer != std::numeric_limits<uint32_t>::max() && writer != passIndex) {
                edges[writer].push_back(passIndex);
                ++indegree[passIndex];
            }
            if (writesResource(use)) {
                lastWriter[use.resource.index] = passIndex;
            }
        }
    }

    std::queue<uint32_t> ready;
    for (uint32_t i = 0; i < passes_.size(); ++i) {
        if (live[i] != 0 && indegree[i] == 0) {
            ready.push(i);
        }
    }
    while (!ready.empty()) {
        const uint32_t passIndex = ready.front();
        ready.pop();
        compiledPassOrder_.push_back(passIndex);
        for (uint32_t next : edges[passIndex]) {
            if (--indegree[next] == 0) {
                ready.push(next);
            }
        }
    }
    const uint32_t liveCount = static_cast<uint32_t>(std::count(live.begin(), live.end(), 1));
    if (compiledPassOrder_.size() != liveCount) {
        throw std::runtime_error("RenderGraph compile failed: cycle detected");
    }

    std::vector<RenderGraphResourceUse> previousUse(resources_.size());
    std::vector<uint32_t> previousUsePass(resources_.size(), 0);
    std::vector<uint8_t> hasPreviousUse(resources_.size(), 0);
    for (uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
        const RenderGraphResource& resource = resources_[resourceIndex];
        if (!resource.hasInitialAccess) {
            continue;
        }
        previousUse[resourceIndex] = RenderGraphResourceUse{
            .resource = RenderGraphResourceId{resourceIndex},
            .state = ResourceState::Undefined,
            .access = PassAccess::ReadWrite,
            .domain = PipelineDomain::Graphics,
        };
        previousUsePass[resourceIndex] = std::numeric_limits<uint32_t>::max();
        hasPreviousUse[resourceIndex] = 1;
    }
    for (uint32_t passIndex : compiledPassOrder_) {
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            if (hasPreviousUse[use.resource.index] != 0) {
                const RenderGraphResourceUse& previous = previousUse[use.resource.index];
                compiledBarriers_.push_back(RenderGraphBarrier{
                    .resource = use.resource,
                    .beforePass = previousUsePass[use.resource.index],
                    .afterPass = passIndex,
                    .before = previousUsePass[use.resource.index] == std::numeric_limits<uint32_t>::max()
                        ? resources_[use.resource.index].initialAccess
                        : resourceAccessFor(previous.state, previous.domain),
                    .after = resourceAccessFor(use.state, use.domain),
                });
            }
            previousUse[use.resource.index] = use;
            previousUsePass[use.resource.index] = passIndex;
            hasPreviousUse[use.resource.index] = 1;
        }
    }
    for (uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
        const RenderGraphResource& resource = resources_[resourceIndex];
        if (!resource.hasFinalAccess || hasPreviousUse[resourceIndex] == 0) {
            continue;
        }
        const RenderGraphResourceUse& previous = previousUse[resourceIndex];
        compiledBarriers_.push_back(RenderGraphBarrier{
            .resource = RenderGraphResourceId{resourceIndex},
            .beforePass = previousUsePass[resourceIndex],
            .afterPass = std::numeric_limits<uint32_t>::max(),
            .before = resourceAccessFor(previous.state, previous.domain),
            .after = resource.finalAccess,
        });
    }

    compiled_ = true;

    resourceLifetimes_.clear();
    resourceLifetimes_.resize(resources_.size());
    for (uint32_t i = 0; i < resources_.size(); ++i) {
        if (resources_[i].lifetime == RenderGraphResource::Lifetime::Transient &&
            !resources_[i].external) {
            resourceLifetimes_[i].resourceIndex = i;
            resourceLifetimes_[i].firstUsePass = UINT32_MAX;
            resourceLifetimes_[i].lastUsePass = 0;
        }
    }

    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        if (live[passIndex] == 0) continue;
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            const uint32_t ri = use.resource.index;
            if (ri < resourceLifetimes_.size() &&
                resources_[ri].lifetime == RenderGraphResource::Lifetime::Transient &&
                !resources_[ri].external) {
                resourceLifetimes_[ri].firstUsePass = std::min(resourceLifetimes_[ri].firstUsePass, passIndex);
                resourceLifetimes_[ri].lastUsePass = std::max(resourceLifetimes_[ri].lastUsePass, passIndex);
            }
        }
    }

    uint32_t nextAliasGroup = 1;
    for (uint32_t i = 0; i < resources_.size(); ++i) {
        if (resources_[i].lifetime != RenderGraphResource::Lifetime::Transient || resources_[i].external) {
            continue;
        }
        if (resourceLifetimes_[i].firstUsePass == UINT32_MAX) {
            continue;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (resources_[j].lifetime != RenderGraphResource::Lifetime::Transient || resources_[j].external) {
                continue;
            }
            if (resourceLifetimes_[j].firstUsePass == UINT32_MAX) {
                continue;
            }
            if (resources_[i].type != resources_[j].type) {
                continue;
            }
            const bool overlap = !(resourceLifetimes_[i].lastUsePass < resourceLifetimes_[j].firstUsePass ||
                                   resourceLifetimes_[j].lastUsePass < resourceLifetimes_[i].firstUsePass);
            if (!overlap && resourceLifetimes_[j].aliasGroup != 0) {
                resourceLifetimes_[i].aliasGroup = resourceLifetimes_[j].aliasGroup;
                resourceLifetimes_[i].aliased = true;
                resourceLifetimes_[j].aliased = true;
                break;
            }
        }
        if (resourceLifetimes_[i].aliasGroup == 0) {
            resourceLifetimes_[i].aliasGroup = nextAliasGroup++;
        }
    }
}

void RenderGraph::execute(VkCommandBuffer commandBuffer, uint64_t frameIndex) {
    if (!compiled_) {
        compile();
    }

    if (transientPool_) {
        for (uint32_t ri = 0; ri < resources_.size(); ++ri) {
            if (resources_[ri].lifetime != RenderGraphResource::Lifetime::Transient || resources_[ri].external) {
                continue;
            }
            if (ri >= resourceLifetimes_.size() || resourceLifetimes_[ri].firstUsePass == UINT32_MAX) {
                continue;
            }
            const uint32_t aliasGroup = resourceLifetimes_[ri].aliasGroup;
            if (resources_[ri].type == RenderGraphResource::Type::Texture) {
                VkImage acquired = transientPool_->acquireOrCreateImage(aliasGroup, resources_[ri]);
                if (resources_[ri].image == VK_NULL_HANDLE) {
                    resources_[ri].image = acquired;
                    resources_[ri].imageRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    };
                }
            } else if (resources_[ri].type == RenderGraphResource::Type::Buffer) {
                VkBuffer acquired = transientPool_->acquireOrCreateBuffer(aliasGroup, resources_[ri]);
                if (resources_[ri].buffer == VK_NULL_HANDLE) {
                    resources_[ri].buffer = acquired;
                }
            }
        }
    }

    FrameGraphContext context{
        .frameIndex = frameIndex,
        .graph = this,
    };
    for (uint32_t passIndex : compiledPassOrder_) {
        for (const RenderGraphBarrier& barrier : compiledBarriers_) {
            if (barrier.afterPass == passIndex && barrier.resource.index < resources_.size()) {
                emitBarrier(commandBuffer, resources_[barrier.resource.index], barrier);
            }
        }
        const RenderGraphPass::ExecuteCallback& callback = passes_[passIndex].callback();
        if (callback) {
            insertDebugBreadcrumb(commandBuffer, passes_[passIndex].name().c_str());
            beginDebugLabel(commandBuffer, passes_[passIndex].name().c_str());
            callback(context, commandBuffer);
            endDebugLabel(commandBuffer);
        }
    }
    for (const RenderGraphBarrier& barrier : compiledBarriers_) {
        if (barrier.afterPass == std::numeric_limits<uint32_t>::max() && barrier.resource.index < resources_.size()) {
            emitBarrier(commandBuffer, resources_[barrier.resource.index], barrier);
        }
    }
}

void RenderGraph::setAsyncComputeQueue(VkQueue queue, uint32_t familyIndex) {
    asyncComputeQueue_ = queue;
    asyncComputeFamily_ = familyIndex;
}

void RenderGraph::setTimelineSemaphore(VkSemaphore semaphore) {
    timelineSemaphore_ = semaphore;
}

uint64_t RenderGraph::nextTimelineValue() {
    return ++timelineValue_;
}

void RenderGraph::executeAsync(VkCommandBuffer graphicsCommandBuffer, VkCommandBuffer computeCommandBuffer, uint64_t frameIndex) {
    if (asyncComputeQueue_ == VK_NULL_HANDLE || computeCommandBuffer == VK_NULL_HANDLE) {
        execute(graphicsCommandBuffer, frameIndex);
        return;
    }
    if (!compiled_) {
        compile();
    }

    if (transientPool_) {
        for (uint32_t ri = 0; ri < resources_.size(); ++ri) {
            if (resources_[ri].lifetime != RenderGraphResource::Lifetime::Transient || resources_[ri].external) continue;
            if (ri >= resourceLifetimes_.size() || resourceLifetimes_[ri].firstUsePass == UINT32_MAX) continue;
            const uint32_t aliasGroup = resourceLifetimes_[ri].aliasGroup;
            if (resources_[ri].type == RenderGraphResource::Type::Texture && resources_[ri].image == VK_NULL_HANDLE) {
                resources_[ri].image = transientPool_->acquireOrCreateImage(aliasGroup, resources_[ri]);
                resources_[ri].imageRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            } else if (resources_[ri].type == RenderGraphResource::Type::Buffer && resources_[ri].buffer == VK_NULL_HANDLE) {
                resources_[ri].buffer = transientPool_->acquireOrCreateBuffer(aliasGroup, resources_[ri]);
            }
        }
    }

    FrameGraphContext context{.frameIndex = frameIndex, .graph = this};

    for (uint32_t passIndex : compiledPassOrder_) {
        const RenderGraphPass& pass = passes_[passIndex];
        bool computeDomain = false;
        for (const RenderGraphResourceUse& use : pass.uses()) {
            if (use.domain == PipelineDomain::Compute || use.domain == PipelineDomain::RayTracing) {
                computeDomain = true;
                break;
            }
        }
        VkCommandBuffer targetCmd = computeDomain ? computeCommandBuffer : graphicsCommandBuffer;

        for (const RenderGraphBarrier& barrier : compiledBarriers_) {
            if (barrier.afterPass == passIndex && barrier.resource.index < resources_.size()) {
                emitBarrier(targetCmd, resources_[barrier.resource.index], barrier);
            }
        }

        const RenderGraphPass::ExecuteCallback& callback = pass.callback();
        if (callback) {
            insertDebugBreadcrumb(targetCmd, pass.name().c_str());
            beginDebugLabel(targetCmd, pass.name().c_str());
            callback(context, targetCmd);
            endDebugLabel(targetCmd);
        }
    }

    for (const RenderGraphBarrier& barrier : compiledBarriers_) {
        if (barrier.afterPass == std::numeric_limits<uint32_t>::max() && barrier.resource.index < resources_.size()) {
            emitBarrier(graphicsCommandBuffer, resources_[barrier.resource.index], barrier);
        }
    }
}

void RenderGraph::emitCompiledBarriers(VkCommandBuffer commandBuffer) const {
    if (!compiled_) {
        throw std::runtime_error("RenderGraph::emitCompiledBarriers called before compile");
    }
    for (const RenderGraphBarrier& barrier : compiledBarriers_) {
        if (barrier.resource.index >= resources_.size()) {
            continue;
        }
        emitBarrier(commandBuffer, resources_[barrier.resource.index], barrier);
    }
}

void RenderGraph::reset() {
    resources_.clear();
    passes_.clear();
    compiledPassOrder_.clear();
    compiledBarriers_.clear();
    resourceLifetimes_.clear();
    if (transientPool_) {
        transientPool_->beginFrame();
    }
    compiled_ = false;
}

} // namespace rtv
