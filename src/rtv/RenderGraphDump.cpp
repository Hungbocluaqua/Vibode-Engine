#include "rtv/RenderGraphDump.h"

#include "rtv/RenderGraph.h"
#include "rtv/RenderGraphPass.h"
#include "rtv/RenderGraphResource.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace rtv {

namespace {

const char* resourceTypeName(RenderGraphResource::Type type) {
    switch (type) {
    case RenderGraphResource::Type::Texture: return "Texture";
    case RenderGraphResource::Type::Buffer:  return "Buffer";
    }
    return "Unknown";
}

const char* resourceLifetimeName(RenderGraphResource::Lifetime lifetime) {
    switch (lifetime) {
    case RenderGraphResource::Lifetime::Transient:  return "Transient";
    case RenderGraphResource::Lifetime::Persistent: return "Persistent";
    case RenderGraphResource::Lifetime::Temporal:   return "Temporal";
    }
    return "Unknown";
}

const char* queueNameForDomain(PipelineDomain domain) {
    switch (domain) {
    case PipelineDomain::Graphics:    return "graphics";
    case PipelineDomain::Compute:     return "compute";
    case PipelineDomain::RayTracing:  return "raytracing";
    case PipelineDomain::Transfer:    return "transfer";
    }
    return "unknown";
}

float timingForPassName(const GpuFrameTimings& timings, const std::string& name) {
    if (name.find("path_trace") == 0) return timings.pathTraceMs;
    if (name.find("restir_history_clear") == 0) return timings.restirHistoryClearMs;
    if (name.find("restir_gi_clear") == 0) return timings.restirGiClearMs;
    if (name.find("restir_spatial_copy") == 0) return timings.restirSpatialCopyMs;
    if (name.find("restir_spatial") == 0) return timings.restirSpatialMs;
    if (name.find("restir_gi_spatial") == 0) return timings.restirGiSpatialMs;
    if (name.find("restir_gi_final") == 0) return timings.restirGiFinalMs;
    if (name.find("fog") == 0) return timings.fogIntegrateMs;
    if (name.find("atmosphere") == 0) return timings.atmosphereMs;
    if (name.find("temporal_denoiser") == 0) return timings.denoiserMs;
    if (name.find("moment_update") == 0) return timings.momentUpdateMs;
    if (name.find("skip_denoiser_copy") == 0) return timings.skipDenoiserCopyMs;
    if (name.find("history_copy") == 0) return timings.historyCopyMs;
    if (name.find("taa_history_copy") == 0) return timings.taaHistoryCopyMs;
    if (name.find("taa") == 0) return timings.taaMs;
    if (name.find("auto_exposure_histogram_clear") == 0) return timings.autoExposureHistogramClearMs;
    if (name.find("auto_exposure_histogram") == 0) return timings.autoExposureHistogramMs;
    if (name.find("auto_exposure_reduce") == 0) return timings.autoExposureReduceMs;
    if (name.find("tone_map") == 0) return timings.toneMapMs;
    if (name.find("selection_outline") == 0) return timings.selectionOutlineMs;
    if (name.find("fullscreen") == 0) return timings.fullscreenMs;
    if (name.find("editor_presentation") == 0) return timings.editorPresentationMs;
    return 0.0f;
}

const char* formatName(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_R32G32B32A32_UINT: return "R32G32B32A32_UINT";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R32_UINT: return "R32_UINT";
    case VK_FORMAT_R32_SFLOAT: return "R32_SFLOAT";
    case VK_FORMAT_R32G32_SFLOAT: return "R32G32_SFLOAT";
    case VK_FORMAT_R32G32B32_SFLOAT: return "R32G32B32_SFLOAT";
    case VK_FORMAT_R8_UNORM: return "R8_UNORM";
    case VK_FORMAT_UNDEFINED: return "UNDEFINED";
    default: return "UNKNOWN";
    }
}

const char* domainColor(PipelineDomain domain) {
    switch (domain) {
    case PipelineDomain::Graphics:    return "lightblue";
    case PipelineDomain::Compute:     return "lightgreen";
    case PipelineDomain::RayTracing:  return "lightblue";
    case PipelineDomain::Transfer:    return "lightyellow";
    }
    return "white";
}

} // namespace

const char* pipelineDomainName(PipelineDomain domain) {
    switch (domain) {
    case PipelineDomain::Graphics:    return "GRAPHICS";
    case PipelineDomain::Compute:     return "COMPUTE";
    case PipelineDomain::RayTracing:  return "RAY_TRACING";
    case PipelineDomain::Transfer:    return "TRANSFER";
    }
    return "UNKNOWN";
}

const char* vulkanStageName(VkPipelineStageFlags2 stage) {
    if (stage == VK_PIPELINE_STAGE_2_NONE) return "NONE";
    if (stage & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) return "RAY_TRACING_SHADER";
    if (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) return "COMPUTE_SHADER";
    if (stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) return "FRAGMENT_SHADER";
    if (stage & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) return "VERTEX_SHADER";
    if (stage & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) return "COLOR_ATTACHMENT_OUTPUT";
    if (stage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) return "TRANSFER";
    if (stage == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT) return "ALL_GRAPHICS";
    if (stage == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) return "ALL_COMMANDS";
    return "UNKNOWN";
}

const char* vulkanAccessName(VkAccessFlags2 access) {
    if (access == VK_ACCESS_2_NONE) return "NONE";
    if (access & VK_ACCESS_2_SHADER_WRITE_BIT) return "SHADER_WRITE";
    if (access & VK_ACCESS_2_SHADER_READ_BIT) return "SHADER_READ";
    if (access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) return "COLOR_ATTACHMENT_WRITE";
    if (access & VK_ACCESS_2_TRANSFER_WRITE_BIT) return "TRANSFER_WRITE";
    if (access & VK_ACCESS_2_TRANSFER_READ_BIT) return "TRANSFER_READ";
    if (access & VK_ACCESS_2_MEMORY_WRITE_BIT) return "MEMORY_WRITE";
    if (access & VK_ACCESS_2_MEMORY_READ_BIT) return "MEMORY_READ";
    return "UNKNOWN";
}

const char* vulkanLayoutName(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "undefined";
    case VK_IMAGE_LAYOUT_GENERAL: return "general";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "color_attachment";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "shader_read_only";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "transfer_src";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "transfer_dst";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return "present_src";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "depth_stencil_attachment";
    default: return "unknown";
    }
}

void dumpRenderGraphJson(
    const RenderGraph& graph,
    const GpuFrameTimings& timings,
    const std::filesystem::path& outputPath) {
    nlohmann::json j;

    const auto& passes = graph.passes();
    const auto& compiledOrder = graph.compiledPassOrder();
    const auto& resources = graph.resources();
    const auto& barriers = graph.compiledBarriers();

    nlohmann::json passesJson = nlohmann::json::array();
    std::unordered_map<uint32_t, PipelineDomain> passDomains;

    for (uint32_t passIndex : compiledOrder) {
        const auto& pass = passes[passIndex];
        nlohmann::json pj;
        pj["name"] = pass.name();
        pj["skipped"] = false;

        PipelineDomain domain = PipelineDomain::Compute;
        for (const auto& use : pass.uses()) {
            if (use.domain == PipelineDomain::RayTracing) {
                domain = PipelineDomain::RayTracing;
                break;
            }
        }
        if (domain == PipelineDomain::Compute) {
            for (const auto& use : pass.uses()) {
                if (use.domain == PipelineDomain::Compute) {
                    domain = PipelineDomain::Compute;
                    break;
                }
                if (use.domain == PipelineDomain::Graphics) {
                    domain = PipelineDomain::Graphics;
                }
            }
        }
        passDomains[passIndex] = domain;
        pj["queue"] = queueNameForDomain(domain);

        nlohmann::json inputs = nlohmann::json::array();
        nlohmann::json outputs = nlohmann::json::array();
        for (const auto& use : pass.uses()) {
            if (!use.resource.valid() || use.resource.index >= resources.size()) {
                continue;
            }
            const auto& res = resources[use.resource.index];
            const char* resName = res.debugName ? res.debugName : "unnamed";
            if (use.access == PassAccess::Read || use.access == PassAccess::ReadWrite) {
                inputs.push_back(resName);
            }
            if (use.access == PassAccess::Write || use.access == PassAccess::ReadWrite) {
                outputs.push_back(resName);
            }
        }
        pj["inputs"] = inputs;
        pj["outputs"] = outputs;

        nlohmann::json formats = nlohmann::json::object();
        nlohmann::json extents = nlohmann::json::object();
        for (const auto& use : pass.uses()) {
            if (!use.resource.valid() || use.resource.index >= resources.size()) {
                continue;
            }
            const auto& res = resources[use.resource.index];
            const char* resName = res.debugName ? res.debugName : "unnamed";
            if (res.type == RenderGraphResource::Type::Texture && res.format != VK_FORMAT_UNDEFINED) {
                formats[resName] = formatName(res.format);
            }
            if (res.type == RenderGraphResource::Type::Texture && res.extent.width > 0) {
                extents[resName] = { {"width", res.extent.width}, {"height", res.extent.height} };
            }
        }
        pj["resource_formats"] = formats;
        pj["extents"] = extents;

        nlohmann::json passBarriers = nlohmann::json::array();
        for (const auto& barrier : barriers) {
            if (barrier.afterPass == passIndex && barrier.resource.valid() && barrier.resource.index < resources.size()) {
                const auto& res = resources[barrier.resource.index];
                const char* resName = res.debugName ? res.debugName : "unnamed";
                const char* beforePassName = barrier.beforePass < passes.size() ? passes[barrier.beforePass].name().c_str() : "<external>";
                nlohmann::json bj;
                bj["resource"] = resName;
                bj["before_pass"] = beforePassName;
                bj["after_pass"] = pass.name();
                bj["before"] = {
                    {"stage", vulkanStageName(barrier.before.stage)},
                    {"access", vulkanAccessName(barrier.before.access)},
                    {"layout", vulkanLayoutName(barrier.before.layout)}
                };
                bj["after"] = {
                    {"stage", vulkanStageName(barrier.after.stage)},
                    {"access", vulkanAccessName(barrier.after.access)},
                    {"layout", vulkanLayoutName(barrier.after.layout)}
                };
                passBarriers.push_back(bj);
            }
        }
        pj["barriers"] = passBarriers;
        pj["gpu_ms"] = timingForPassName(timings, pass.name());

        passesJson.push_back(pj);
    }
    j["passes"] = passesJson;

    nlohmann::json resourcesJson = nlohmann::json::array();
    for (const auto& res : resources) {
        nlohmann::json rj;
        rj["name"] = res.debugName ? res.debugName : "unnamed";
        rj["type"] = resourceTypeName(res.type);
        rj["lifetime"] = resourceLifetimeName(res.lifetime);
        if (res.type == RenderGraphResource::Type::Texture) {
            rj["format"] = static_cast<int>(res.format);
            rj["extent"] = { {"width", res.extent.width}, {"height", res.extent.height} };
        } else {
            rj["size_bytes"] = res.size;
        }
        resourcesJson.push_back(rj);
    }
    j["resources"] = resourcesJson;

    nlohmann::json barriersJson = nlohmann::json::array();
    for (const auto& barrier : barriers) {
        if (!barrier.resource.valid() || barrier.resource.index >= resources.size()) {
            continue;
        }
        const auto& res = resources[barrier.resource.index];
        const char* resName = res.debugName ? res.debugName : "unnamed";
        const char* beforePassName = barrier.beforePass < passes.size() ? passes[barrier.beforePass].name().c_str() : "<external>";
        const char* afterPassName = barrier.afterPass < passes.size() ? passes[barrier.afterPass].name().c_str() : "<external>";
        nlohmann::json bj;
        bj["resource"] = resName;
        bj["before_pass"] = beforePassName;
        bj["after_pass"] = afterPassName;
        bj["before"] = {
            {"stage", vulkanStageName(barrier.before.stage)},
            {"access", vulkanAccessName(barrier.before.access)},
            {"layout", vulkanLayoutName(barrier.before.layout)}
        };
        bj["after"] = {
            {"stage", vulkanStageName(barrier.after.stage)},
            {"access", vulkanAccessName(barrier.after.access)},
            {"layout", vulkanLayoutName(barrier.after.layout)}
        };
        barriersJson.push_back(bj);
    }
    j["barriers"] = barriersJson;

    const auto dir = outputPath.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }

    std::ofstream file(outputPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open render graph JSON output file: " + outputPath.string());
    }
    file << j.dump(2);
}

void dumpRenderGraphDot(
    const RenderGraph& graph,
    const GpuFrameTimings& timings,
    const std::filesystem::path& outputPath) {
    const auto& passes = graph.passes();
    const auto& compiledOrder = graph.compiledPassOrder();
    const auto& resources = graph.resources();

    const auto dir = outputPath.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }

    std::ofstream file(outputPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open render graph DOT output file: " + outputPath.string());
    }

    file << "digraph RenderGraph {\n";
    file << "    rankdir=TB;\n";
    file << "    node [shape=box, style=filled];\n\n";

    std::unordered_map<uint32_t, PipelineDomain> passDomains;
    for (uint32_t passIndex : compiledOrder) {
        const auto& pass = passes[passIndex];
        PipelineDomain domain = PipelineDomain::Compute;
        for (const auto& use : pass.uses()) {
            if (use.domain == PipelineDomain::RayTracing) {
                domain = PipelineDomain::RayTracing;
                break;
            }
        }
        if (domain == PipelineDomain::Compute) {
            for (const auto& use : pass.uses()) {
                if (use.domain == PipelineDomain::Compute) {
                    domain = PipelineDomain::Compute;
                    break;
                }
                if (use.domain == PipelineDomain::Graphics) {
                    domain = PipelineDomain::Graphics;
                }
            }
        }
        passDomains[passIndex] = domain;

        float gpuMs = timingForPassName(timings, pass.name());
        file << "    \"" << pass.name() << "\" [fillcolor=" << domainColor(domain)
             << ", label=\"" << pass.name() << "\\n(";
        switch (domain) {
        case PipelineDomain::RayTracing: file << "RayTracing"; break;
        case PipelineDomain::Compute: file << "Compute"; break;
        case PipelineDomain::Graphics: file << "Graphics"; break;
        case PipelineDomain::Transfer: file << "Transfer"; break;
        }
        file << ")\\n" << gpuMs << "ms\"];\n";
    }

    file << "\n";

    for (uint32_t passIndex : compiledOrder) {
        const auto& pass = passes[passIndex];
        for (const auto& use : pass.uses()) {
            if (use.access != PassAccess::Write && use.access != PassAccess::ReadWrite) {
                continue;
            }
            if (!use.resource.valid() || use.resource.index >= resources.size()) {
                continue;
            }
            const auto& res = resources[use.resource.index];
            const char* resName = res.debugName ? res.debugName : "unnamed";
            for (uint32_t consumerIndex : compiledOrder) {
                if (consumerIndex == passIndex) {
                    continue;
                }
                const auto& consumer = passes[consumerIndex];
                for (const auto& consumerUse : consumer.uses()) {
                    if (consumerUse.resource.index == use.resource.index &&
                        (consumerUse.access == PassAccess::Read || consumerUse.access == PassAccess::ReadWrite)) {
                        file << "    \"" << pass.name() << "\" -> \"" << consumer.name()
                             << "\" [label=\"" << resName << "\"];\n";
                        break;
                    }
                }
            }
        }
    }

    file << "}\n";
}

} // namespace rtv
