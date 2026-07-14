#include "rtv/RenderGraph.h"

#include "rtv/NsightMarkers.h"
#include "rtv/NsightPerfMarkers.h"
#include "rtv/ResourceAllocator.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
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
        return it->second.image;
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
    imageInfo.sharingMode = allocator_->graphicsComputeSharingMode();
    if (imageInfo.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        imageInfo.queueFamilyIndexCount = allocator_->graphicsComputeQueueFamilyCount();
        imageInfo.pQueueFamilyIndices = allocator_->graphicsComputeQueueFamilies();
    }

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator_->handle(), &imageInfo, &allocInfo, &image, &allocation, nullptr) == VK_SUCCESS) {
        const size_t estimatedBytes = desc.extent.width * desc.extent.height * std::max(desc.extent.depth, 1u) * 4u;
        TransientImageAllocation transient{image, allocation, estimatedBytes};
        aliasImages_[aliasGroup] = transient;
        imagePool_.push_back(transient);
        totalBytesAllocated_ += estimatedBytes;
        activeBytes_ += estimatedBytes;
        peakBytes_ = std::max(peakBytes_, activeBytes_);
    }
    return image;
}

VkBuffer TransientResourcePool::acquireOrCreateBuffer(uint32_t aliasGroup, const RenderGraphResource& desc) {
    auto it = aliasBuffers_.find(aliasGroup);
    if (it != aliasBuffers_.end()) {
        return it->second.buffer;
    }
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = desc.size;
    bufferInfo.usage = desc.bufferUsage;
    bufferInfo.sharingMode = allocator_->graphicsComputeSharingMode();
    if (bufferInfo.sharingMode == VK_SHARING_MODE_CONCURRENT) {
        bufferInfo.queueFamilyIndexCount = allocator_->graphicsComputeQueueFamilyCount();
        bufferInfo.pQueueFamilyIndices = allocator_->graphicsComputeQueueFamilies();
    }
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(allocator_->handle(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) == VK_SUCCESS) {
        TransientBufferAllocation transient{buffer, allocation, static_cast<size_t>(desc.size)};
        aliasBuffers_[aliasGroup] = transient;
        bufferPool_.push_back(transient);
        activeBytes_ += desc.size;
        totalBytesAllocated_ += desc.size;
        peakBytes_ = std::max(peakBytes_, activeBytes_);
    }
    return buffer;
}

void TransientResourcePool::releaseImage(uint32_t aliasGroup) {
    auto it = aliasImages_.find(aliasGroup);
    if (it != aliasImages_.end()) {
        const TransientImageAllocation allocation = it->second;
        vmaDestroyImage(allocator_->handle(), allocation.image, allocation.allocation);
        aliasImages_.erase(it);
        imagePool_.erase(std::remove_if(imagePool_.begin(), imagePool_.end(), [allocation](const TransientImageAllocation& entry) {
            return entry.image == allocation.image;
        }), imagePool_.end());
        activeBytes_ = activeBytes_ > allocation.estimatedBytes ? activeBytes_ - allocation.estimatedBytes : 0;
    }
}

void TransientResourcePool::releaseBuffer(uint32_t aliasGroup) {
    auto it = aliasBuffers_.find(aliasGroup);
    if (it != aliasBuffers_.end()) {
        const TransientBufferAllocation allocation = it->second;
        vmaDestroyBuffer(allocator_->handle(), allocation.buffer, allocation.allocation);
        aliasBuffers_.erase(it);
        bufferPool_.erase(std::remove_if(bufferPool_.begin(), bufferPool_.end(), [allocation](const TransientBufferAllocation& entry) {
            return entry.buffer == allocation.buffer;
        }), bufferPool_.end());
        activeBytes_ = activeBytes_ > allocation.bytes ? activeBytes_ - allocation.bytes : 0;
    }
}

void TransientResourcePool::beginFrame() {
    destroyAll();
    peakBytes_ = 0;
}

void TransientResourcePool::destroyAll() {
    for (const TransientImageAllocation& image : imagePool_) {
        vmaDestroyImage(allocator_->handle(), image.image, image.allocation);
    }
    for (const TransientBufferAllocation& buffer : bufferPool_) {
        vmaDestroyBuffer(allocator_->handle(), buffer.buffer, buffer.allocation);
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

VkDeviceSize estimateResourceBytes(const RenderGraphResource& resource) {
    if (resource.type == RenderGraphResource::Type::Buffer) {
        return resource.size;
    }
    const VkDeviceSize pixelCount = static_cast<VkDeviceSize>(resource.extent.width) *
        std::max(1u, resource.extent.height) *
        std::max(1u, resource.extent.depth);
    VkDeviceSize bytesPerPixel = 4;
    switch (resource.format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
        bytesPerPixel = 8;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_UINT:
        bytesPerPixel = 16;
        break;
    case VK_FORMAT_R32G32_SFLOAT:
        bytesPerPixel = 8;
        break;
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
        bytesPerPixel = 4;
        break;
    case VK_FORMAT_R8_UNORM:
        bytesPerPixel = 1;
        break;
    default:
        bytesPerPixel = 4;
        break;
    }
    return pixelCount * bytesPerPixel;
}

bool resourcesAliasCompatible(const RenderGraphResource& a, const RenderGraphResource& b) {
    if (a.type != b.type) {
        return false;
    }
    if (a.type == RenderGraphResource::Type::Buffer) {
        return a.size == b.size && a.bufferUsage == b.bufferUsage;
    }
    return a.format == b.format &&
        a.extent.width == b.extent.width &&
        a.extent.height == b.extent.height &&
        a.extent.depth == b.extent.depth &&
        a.usage == b.usage;
}

bool lifetimeOverlaps(const TransientResourceLifetime& a, const TransientResourceLifetime& b) {
    if (a.firstUsePass == UINT32_MAX || b.firstUsePass == UINT32_MAX) {
        return true;
    }
    return !(a.lastUsePass < b.firstUsePass || b.lastUsePass < a.firstUsePass);
}

VkDeviceSize bufferRangeEnd(VkDeviceSize offset, VkDeviceSize size) {
    const VkDeviceSize maxValue = std::numeric_limits<VkDeviceSize>::max();
    if (size == VK_WHOLE_SIZE || maxValue - offset < size) {
        return maxValue;
    }
    return offset + size;
}

bool bufferRangesOverlap(const RenderGraphResource& a, const RenderGraphResource& b) {
    if (a.buffer == VK_NULL_HANDLE || a.buffer != b.buffer) {
        return false;
    }
    const VkDeviceSize aEnd = bufferRangeEnd(a.bufferOffset, a.size);
    const VkDeviceSize bEnd = bufferRangeEnd(b.bufferOffset, b.size);
    return a.bufferOffset < bEnd && b.bufferOffset < aEnd;
}

bool resourcesSharePhysicalHandle(const RenderGraphResource& a, const RenderGraphResource& b) {
    if (a.type != b.type) {
        return false;
    }
    if (a.type == RenderGraphResource::Type::Buffer) {
        return bufferRangesOverlap(a, b);
    }
    return a.image != VK_NULL_HANDLE && a.image == b.image;
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

bool renderGraphTraceEnabled() {
    static const bool enabled = [] {
#if defined(_WIN32)
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "RTV_MAIN_LOOP_TRACE") != 0 || value == nullptr) {
            return false;
        }
        const bool result = value[0] != '\0' && value[0] != '0';
        std::free(value);
        return result;
#else
        const char* value = std::getenv("RTV_MAIN_LOOP_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
    }();
    return enabled;
}

void traceRenderGraphPass(const char* phase, const std::string& name) {
    if (!renderGraphTraceEnabled()) {
        return;
    }
    std::cout << "RENDER_GRAPH pass=" << name << " phase=" << phase << '\n' << std::flush;
}

bool accessIncludesWrite(VkAccessFlags2 access) {
    constexpr VkAccessFlags2 writeAccess =
        VK_ACCESS_2_SHADER_WRITE_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_WRITE_BIT |
        VK_ACCESS_2_HOST_WRITE_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    return (access & writeAccess) != 0;
}

bool barrierRequired(
    const RenderGraphResource& resource,
    const ResourceAccess& before,
    const ResourceAccess& after) {
    if (resource.type == RenderGraphResource::Type::Texture && before.layout != after.layout) {
        return true;
    }
    return accessIncludesWrite(before.access) || accessIncludesWrite(after.access);
}

void emitBarrierBatch(
    VkCommandBuffer commandBuffer,
    const std::vector<RenderGraphResource>& resources,
    const std::vector<RenderGraphBarrier>& barriers,
    const std::vector<uint32_t>& barrierIndices) {
    if (barrierIndices.empty()) {
        return;
    }

    std::vector<VkImageMemoryBarrier2> imageBarriers;
    std::vector<VkBufferMemoryBarrier2> bufferBarriers;
    imageBarriers.reserve(barrierIndices.size());
    bufferBarriers.reserve(barrierIndices.size());

    for (uint32_t barrierIndex : barrierIndices) {
        if (barrierIndex >= barriers.size()) {
            continue;
        }
        const RenderGraphBarrier& barrier = barriers[barrierIndex];
        if (barrier.resource.index >= resources.size()) {
            continue;
        }
        const RenderGraphResource& resource = resources[barrier.resource.index];
        if (!barrierRequired(resource, barrier.before, barrier.after)) {
            continue;
        }
        if (resource.type == RenderGraphResource::Type::Texture && resource.image != VK_NULL_HANDLE) {
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
            imageBarriers.push_back(imageBarrier);
        } else if (resource.type == RenderGraphResource::Type::Buffer && resource.buffer != VK_NULL_HANDLE) {
            VkBufferMemoryBarrier2 bufferBarrier{};
            bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            bufferBarrier.srcStageMask = barrier.before.stage;
            bufferBarrier.srcAccessMask = barrier.before.access;
            bufferBarrier.dstStageMask = barrier.after.stage;
            bufferBarrier.dstAccessMask = barrier.after.access;
            bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bufferBarrier.buffer = resource.buffer;
            bufferBarrier.offset = resource.bufferOffset;
            bufferBarrier.size = resource.size == 0 ? VK_WHOLE_SIZE : resource.size;
            bufferBarriers.push_back(bufferBarrier);
        }
    }

    if (!imageBarriers.empty() || !bufferBarriers.empty()) {
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size());
        dependency.pImageMemoryBarriers = imageBarriers.data();
        dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size());
        dependency.pBufferMemoryBarriers = bufferBarriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }
}

} // namespace

RenderGraph::RenderGraph(ResourceAllocator* allocator, bool enableAliasing)
    : aliasingEnabled_(enableAliasing) {
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
    case ResourceState::RayTracingRead:
        return {VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL};
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
        : (domain == PipelineDomain::RayTracing ? ResourceState::RayTracingRead : (domain == PipelineDomain::Compute ? ResourceState::ComputeShaderRead : ResourceState::ShaderRead));
    return addUse(id, state, PassAccess::Read, domain);
}

RenderGraphPass& RenderGraphPass::addStorageWrite(RenderGraphResourceId id, PipelineDomain domain) {
    const ResourceState state = domain == PipelineDomain::Transfer
        ? ResourceState::TransferDest
        : (domain == PipelineDomain::RayTracing ? ResourceState::RayTracing : (domain == PipelineDomain::Compute ? ResourceState::ComputeShaderStorage : ResourceState::ShaderStorage));
    return addUse(id, state, PassAccess::Write, domain);
}

RenderGraphPass& RenderGraphPass::addStorageReadWrite(RenderGraphResourceId id, PipelineDomain domain) {
    const ResourceState state = domain == PipelineDomain::Transfer
        ? ResourceState::TransferDest
        : (domain == PipelineDomain::RayTracing ? ResourceState::RayTracing : (domain == PipelineDomain::Compute ? ResourceState::ComputeShaderStorage : ResourceState::ShaderStorage));
    return addUse(id, state, PassAccess::ReadWrite, domain);
}

RenderGraphPass& RenderGraphPass::addUniformBuffer(RenderGraphResourceId id, PipelineDomain domain) {
    return addUse(id, ResourceState::UniformBuffer, PassAccess::Read, domain);
}

RenderGraphPass& RenderGraphPass::setQueueDomain(RenderGraphQueueDomain domain) {
    queueDomain_ = domain;
    queueDomainExplicit_ = true;
    return *this;
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
    if (!queueDomainExplicit_) {
        switch (domain) {
        case PipelineDomain::RayTracing:
            queueDomain_ = RenderGraphQueueDomain::RayTracing;
            break;
        case PipelineDomain::Compute:
            if (queueDomain_ != RenderGraphQueueDomain::RayTracing) {
                queueDomain_ = RenderGraphQueueDomain::Compute;
            }
            break;
        case PipelineDomain::Transfer:
            if (queueDomain_ != RenderGraphQueueDomain::RayTracing && queueDomain_ != RenderGraphQueueDomain::Compute) {
                queueDomain_ = RenderGraphQueueDomain::Transfer;
            }
            break;
        case PipelineDomain::Graphics:
            break;
        }
    }
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
    compiledBarrierBatches_.clear();
    compiledFinalBarrierBatch_.clear();
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
    std::vector<std::vector<uint32_t>> readersSinceWrite(resources_.size());
    const auto addEdge = [&](uint32_t before, uint32_t after) {
        if (before == std::numeric_limits<uint32_t>::max() || before == after) {
            return;
        }
        std::vector<uint32_t>& outgoing = edges[before];
        if (std::find(outgoing.begin(), outgoing.end(), after) == outgoing.end()) {
            outgoing.push_back(after);
            ++indegree[after];
        }
    };

    for (uint32_t passIndex = 0; passIndex < passes_.size(); ++passIndex) {
        if (live[passIndex] == 0) {
            continue;
        }
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            if (use.resource.index >= resources_.size()) {
                throw std::runtime_error("RenderGraph pass references a missing resource");
            }
            const uint32_t writer = lastWriter[use.resource.index];
            addEdge(writer, passIndex);
            if (writesResource(use)) {
                for (uint32_t reader : readersSinceWrite[use.resource.index]) {
                    addEdge(reader, passIndex);
                }
                readersSinceWrite[use.resource.index].clear();
                lastWriter[use.resource.index] = passIndex;
            } else if (readsResource(use)) {
                std::vector<uint32_t>& readers = readersSinceWrite[use.resource.index];
                if (std::find(readers.begin(), readers.end(), passIndex) == readers.end()) {
                    readers.push_back(passIndex);
                }
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

    std::vector<ResourceAccess> previousAccess(resources_.size());
    std::vector<uint32_t> previousUsePass(resources_.size(), 0);
    std::vector<RenderGraphQueueDomain> previousUseQueue(resources_.size(), RenderGraphQueueDomain::Graphics);
    std::vector<uint8_t> previousUsesAreReads(resources_.size(), 0);
    std::vector<uint8_t> hasPreviousUse(resources_.size(), 0);
    for (uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
        const RenderGraphResource& resource = resources_[resourceIndex];
        if (!resource.hasInitialAccess) {
            continue;
        }
        previousAccess[resourceIndex] = resource.initialAccess;
        previousUsePass[resourceIndex] = std::numeric_limits<uint32_t>::max();
        previousUsesAreReads[resourceIndex] = accessIncludesWrite(resource.initialAccess.access) ? 0 : 1;
        hasPreviousUse[resourceIndex] = 1;
    }
    for (uint32_t passIndex : compiledPassOrder_) {
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            const ResourceAccess currentAccess = resourceAccessFor(use.state, use.domain);
            const bool currentIsReadOnly = readsResource(use) && !writesResource(use);
            if (hasPreviousUse[use.resource.index] != 0) {
                RenderGraphBarrier barrier{
                    .resource = use.resource,
                    .beforePass = previousUsePass[use.resource.index],
                    .afterPass = passIndex,
                    .before = previousAccess[use.resource.index],
                    .after = currentAccess,
                    .beforeQueue = previousUseQueue[use.resource.index],
                    .afterQueue = passes_[passIndex].queueDomain(),
                };
                const bool samePass = previousUsePass[use.resource.index] == passIndex;
                const bool needsBarrier = barrierRequired(resources_[use.resource.index], barrier.before, barrier.after);
                if (!samePass && needsBarrier) {
                    compiledBarriers_.push_back(barrier);
                }

                const bool accumulateReaders = !samePass && !needsBarrier &&
                    previousUsesAreReads[use.resource.index] != 0 && currentIsReadOnly;
                if (samePass || accumulateReaders) {
                    previousAccess[use.resource.index].stage |= currentAccess.stage;
                    previousAccess[use.resource.index].access |= currentAccess.access;
                    if (samePass && previousAccess[use.resource.index].layout != currentAccess.layout) {
                        throw std::runtime_error("RenderGraph pass uses one resource with conflicting image layouts");
                    }
                    previousUsePass[use.resource.index] = passIndex;
                    previousUseQueue[use.resource.index] = passes_[passIndex].queueDomain();
                    previousUsesAreReads[use.resource.index] =
                        previousUsesAreReads[use.resource.index] != 0 && currentIsReadOnly ? 1 : 0;
                    continue;
                }
            }
            previousAccess[use.resource.index] = currentAccess;
            previousUsePass[use.resource.index] = passIndex;
            previousUseQueue[use.resource.index] = passes_[passIndex].queueDomain();
            previousUsesAreReads[use.resource.index] = currentIsReadOnly ? 1 : 0;
            hasPreviousUse[use.resource.index] = 1;
        }
    }
    for (uint32_t resourceIndex = 0; resourceIndex < resources_.size(); ++resourceIndex) {
        const RenderGraphResource& resource = resources_[resourceIndex];
        if (!resource.hasFinalAccess || hasPreviousUse[resourceIndex] == 0) {
            continue;
        }
        RenderGraphBarrier barrier{
            .resource = RenderGraphResourceId{resourceIndex},
            .beforePass = previousUsePass[resourceIndex],
            .afterPass = std::numeric_limits<uint32_t>::max(),
            .before = previousAccess[resourceIndex],
            .after = resource.finalAccess,
            .beforeQueue = previousUseQueue[resourceIndex],
            .afterQueue = RenderGraphQueueDomain::Graphics,
        };
        if (barrierRequired(resources_[resourceIndex], barrier.before, barrier.after)) {
            compiledBarriers_.push_back(barrier);
        }
    }

    compiledBarrierBatches_.resize(passes_.size());
    for (uint32_t barrierIndex = 0; barrierIndex < compiledBarriers_.size(); ++barrierIndex) {
        const uint32_t afterPass = compiledBarriers_[barrierIndex].afterPass;
        if (afterPass == std::numeric_limits<uint32_t>::max()) {
            compiledFinalBarrierBatch_.push_back(barrierIndex);
        } else if (afterPass < compiledBarrierBatches_.size()) {
            compiledBarrierBatches_[afterPass].push_back(barrierIndex);
        }
    }

    compiled_ = true;

    resourceLifetimes_.clear();
    resourceLifetimes_.resize(resources_.size());
    for (uint32_t i = 0; i < resources_.size(); ++i) {
        resourceLifetimes_[i].resourceIndex = i;
        resourceLifetimes_[i].firstUsePass = UINT32_MAX;
        resourceLifetimes_[i].lastUsePass = UINT32_MAX;
        resourceLifetimes_[i].firstReadPass = UINT32_MAX;
        resourceLifetimes_[i].lastReadPass = UINT32_MAX;
        resourceLifetimes_[i].firstWritePass = UINT32_MAX;
        resourceLifetimes_[i].lastWritePass = UINT32_MAX;
        resourceLifetimes_[i].estimatedBytes = estimateResourceBytes(resources_[i]);
        resourceLifetimes_[i].aliasEligible = !resources_[i].external &&
            resources_[i].lifetime == RenderGraphResource::Lifetime::Transient;
    }

    for (uint32_t passIndex : compiledPassOrder_) {
        for (const RenderGraphResourceUse& use : passes_[passIndex].uses()) {
            const uint32_t ri = use.resource.index;
            if (ri < resourceLifetimes_.size()) {
                TransientResourceLifetime& lifetime = resourceLifetimes_[ri];
                if (lifetime.firstUsePass == UINT32_MAX) {
                    lifetime.firstUsePass = passIndex;
                    lifetime.firstUseQueue = passes_[passIndex].queueDomain();
                    lifetime.firstAccess = resourceAccessFor(use.state, use.domain);
                }
                lifetime.lastUsePass = passIndex;
                lifetime.lastUseQueue = passes_[passIndex].queueDomain();
                lifetime.lastAccess = resourceAccessFor(use.state, use.domain);
                if (readsResource(use)) {
                    lifetime.firstReadPass = std::min(lifetime.firstReadPass, passIndex);
                    lifetime.lastReadPass = passIndex;
                }
                if (writesResource(use)) {
                    lifetime.firstWritePass = std::min(lifetime.firstWritePass, passIndex);
                    lifetime.lastWritePass = passIndex;
                }
            }
        }
    }

    uint32_t nextAliasGroup = 1;
    for (uint32_t i = 0; i < resources_.size(); ++i) {
        if (!resourceLifetimes_[i].aliasEligible) {
            continue;
        }
        if (resourceLifetimes_[i].firstUsePass == UINT32_MAX) {
            continue;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (!resourceLifetimes_[j].aliasEligible) {
                continue;
            }
            if (resourceLifetimes_[j].firstUsePass == UINT32_MAX) {
                continue;
            }
            if (!resourcesAliasCompatible(resources_[i], resources_[j])) {
                continue;
            }
            if (aliasingEnabled_ && !lifetimeOverlaps(resourceLifetimes_[i], resourceLifetimes_[j]) && resourceLifetimes_[j].aliasGroup != 0) {
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

    for (uint32_t i = 0; i < resources_.size(); ++i) {
        if (!resourceLifetimes_[i].aliasEligible) {
            continue;
        }
        if (resourceLifetimes_[i].firstUsePass == UINT32_MAX) {
            continue;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (!resourceLifetimes_[j].aliasEligible) {
                continue;
            }
            if (resourceLifetimes_[j].firstUsePass == UINT32_MAX) {
                continue;
            }
            if (!resourcesSharePhysicalHandle(resources_[i], resources_[j])) {
                continue;
            }
            if (lifetimeOverlaps(resourceLifetimes_[i], resourceLifetimes_[j])) {
                continue;
            }
            if (resourceLifetimes_[j].aliasGroup == 0) {
                resourceLifetimes_[j].aliasGroup = nextAliasGroup++;
            }
            resourceLifetimes_[i].aliasGroup = resourceLifetimes_[j].aliasGroup;
            resourceLifetimes_[i].aliased = true;
            resourceLifetimes_[j].aliased = true;
            break;
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
        if (passIndex < compiledBarrierBatches_.size()) {
            emitBarrierBatch(commandBuffer, resources_, compiledBarriers_, compiledBarrierBatches_[passIndex]);
        }
        const RenderGraphPass::ExecuteCallback& callback = passes_[passIndex].callback();
        if (callback) {
            traceRenderGraphPass("begin", passes_[passIndex].name());
            insertDebugBreadcrumb(commandBuffer, passes_[passIndex].name().c_str());
            beginDebugLabel(commandBuffer, passes_[passIndex].name().c_str());
            ScopedNsightRange nsightRange(passes_[passIndex].name().c_str());
            ScopedNsightPerfCommandBufferRange nsightPerfRange(commandBuffer, passes_[passIndex].name().c_str());
            callback(context, commandBuffer);
            endDebugLabel(commandBuffer);
            traceRenderGraphPass("end", passes_[passIndex].name());
        }
    }
    emitBarrierBatch(commandBuffer, resources_, compiledBarriers_, compiledFinalBarrierBatch_);
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
        const RenderGraphQueueDomain queueDomain = pass.queueDomain();
        const bool computeDomain = queueDomain == RenderGraphQueueDomain::Compute ||
            queueDomain == RenderGraphQueueDomain::SameFamilyCompute;
        VkCommandBuffer targetCmd = computeDomain ? computeCommandBuffer : graphicsCommandBuffer;

        if (passIndex < compiledBarrierBatches_.size()) {
            emitBarrierBatch(targetCmd, resources_, compiledBarriers_, compiledBarrierBatches_[passIndex]);
        }

        const RenderGraphPass::ExecuteCallback& callback = pass.callback();
        if (callback) {
            traceRenderGraphPass("begin", pass.name());
            insertDebugBreadcrumb(targetCmd, pass.name().c_str());
            beginDebugLabel(targetCmd, pass.name().c_str());
            ScopedNsightRange nsightRange(pass.name().c_str());
            ScopedNsightPerfCommandBufferRange nsightPerfRange(targetCmd, pass.name().c_str());
            callback(context, targetCmd);
            endDebugLabel(targetCmd);
            traceRenderGraphPass("end", pass.name());
        }
    }

    emitBarrierBatch(graphicsCommandBuffer, resources_, compiledBarriers_, compiledFinalBarrierBatch_);
}

void RenderGraph::emitCompiledBarriers(VkCommandBuffer commandBuffer) const {
    if (!compiled_) {
        throw std::runtime_error("RenderGraph::emitCompiledBarriers called before compile");
    }
    std::vector<uint32_t> allBarrierIndices(compiledBarriers_.size());
    std::iota(allBarrierIndices.begin(), allBarrierIndices.end(), 0u);
    emitBarrierBatch(commandBuffer, resources_, compiledBarriers_, allBarrierIndices);
}

void RenderGraph::reset() {
    resources_.clear();
    passes_.clear();
    compiledPassOrder_.clear();
    compiledBarriers_.clear();
    compiledBarrierBatches_.clear();
    compiledFinalBarrierBatch_.clear();
    resourceLifetimes_.clear();
    if (transientPool_) {
        transientPool_->beginFrame();
    }
    compiled_ = false;
}

} // namespace rtv
