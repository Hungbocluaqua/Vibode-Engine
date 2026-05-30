#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/RendererDebug.h"

#include <Volk/volk.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

class Buffer;
class ResourceAllocator;
class VulkanContext;
class PathTracerRenderer;

class DiagnosticImageExport final : private NonCopyable {
public:
    DiagnosticImageExport(const VulkanContext& context, ResourceAllocator& allocator);
    ~DiagnosticImageExport();

    bool initialize(VkFormat format, VkExtent2D extent);
    void destroy();

    bool exportView(
        PathTracerRenderer& renderer,
        RendererDebugView view,
        const std::filesystem::path& outputPath,
        uint32_t warmupFrames = 4);

    void writeExportManifest(
        const std::filesystem::path& dir,
        const std::vector<std::string>& exported,
        uint32_t width,
        uint32_t height);

    [[nodiscard]] static std::vector<RendererDebugView> allExportViews();

private:
    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::unique_ptr<Buffer> readbackBuffer_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence completionFence_ = VK_NULL_HANDLE;
    bool initialized_ = false;
};

} // namespace rtv
