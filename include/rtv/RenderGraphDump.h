#pragma once

#include "rtv/GpuProfiler.h"
#include "rtv/RenderGraphResource.h"

#include <Volk/volk.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

class RenderGraph;

void dumpRenderGraphJson(
    const RenderGraph& graph,
    const GpuFrameTimings& timings,
    const std::filesystem::path& outputPath);

void dumpRenderGraphDot(
    const RenderGraph& graph,
    const GpuFrameTimings& timings,
    const std::filesystem::path& outputPath);

[[nodiscard]] const char* pipelineDomainName(PipelineDomain domain);
[[nodiscard]] const char* vulkanStageName(VkPipelineStageFlags2 stage);
[[nodiscard]] const char* vulkanAccessName(VkAccessFlags2 access);
[[nodiscard]] const char* vulkanLayoutName(VkImageLayout layout);

} // namespace rtv
