#include "rtv/RenderGraphDump.h"

#include "rtv/RenderGraph.h"
#include "rtv/RenderGraphPass.h"
#include "rtv/RenderGraphResource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

const char* queueNameForDomain(RenderGraphQueueDomain domain) {
    switch (domain) {
    case RenderGraphQueueDomain::Graphics:          return "graphics";
    case RenderGraphQueueDomain::RayTracing:        return "raytracing";
    case RenderGraphQueueDomain::Compute:           return "compute";
    case RenderGraphQueueDomain::Transfer:          return "transfer";
    case RenderGraphQueueDomain::SameFamilyCompute: return "same_family_compute";
    }
    return "unknown";
}

struct TimingMapping {
    float gpuMs = 0.0f;
    const char* profileTimingKey = "";
    const char* timingSource = "unmapped";
    const char* note = "";
    bool mapped = false;
};

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

TimingMapping mappedTiming(const char* key, float gpuMs, const char* note = "") {
    return {
        .gpuMs = gpuMs,
        .profileTimingKey = key,
        .timingSource = "per_frame_gpu_timings",
        .note = note,
        .mapped = true,
    };
}

TimingMapping timingForPassName(const GpuFrameTimings& timings, const std::string& name) {
    if (name == "path_trace_rt" || startsWith(name, "path_trace")) {
        return mappedTiming("path_trace", timings.pathTraceMs);
    }
    if (startsWith(name, "restir_history_clear")) return mappedTiming("restir_history_clear", timings.restirHistoryClearMs);
    if (startsWith(name, "restir_gi_clear")) return mappedTiming("restir_gi_clear", timings.restirGiClearMs);
    if (startsWith(name, "restir_gi_temporal")) return mappedTiming("restir_gi_temporal", timings.restirGiTemporalMs);
    if (startsWith(name, "restir_spatial_copy")) return mappedTiming("restir_spatial_copy", timings.restirSpatialCopyMs);
    if (startsWith(name, "restir_spatial")) return mappedTiming("restir_spatial", timings.restirSpatialMs);
    if (startsWith(name, "restir_gi_spatial")) return mappedTiming("restir_gi_spatial", timings.restirGiSpatialMs);
    if (startsWith(name, "restir_gi_upsample")) return mappedTiming("restir_gi_upsample", timings.restirGiUpsampleMs);
    if (startsWith(name, "restir_gi_final")) return mappedTiming("restir_gi_final", timings.restirGiFinalMs);
    if (startsWith(name, "restir_gi_counters_readback")) return mappedTiming("restir_gi_counters_readback", timings.restirGiCountersReadbackMs);
    if (startsWith(name, "regir_spatial_reuse")) return mappedTiming("regir_spatial_reuse", timings.regirSpatialReuseMs);
    if (startsWith(name, "regir_temporal_reuse")) return mappedTiming("regir_temporal_reuse", timings.regirTemporalReuseMs);
    if (startsWith(name, "regir_build")) return mappedTiming("regir_build", timings.regirBuildMs);
    if (startsWith(name, "restir_di_temporal")) return mappedTiming("restir_di_temporal", timings.restirDiTemporalMs);
    if (startsWith(name, "restir_di_spatial")) return mappedTiming("restir_di_spatial", timings.restirDiSpatialMs);
    if (startsWith(name, "restir_di_final")) return mappedTiming("restir_di_final", timings.restirDiFinalMs);
    if (startsWith(name, "fog")) return mappedTiming("fog_integrate", timings.fogIntegrateMs);
    if (startsWith(name, "atmosphere")) return mappedTiming("atmosphere", timings.atmosphereMs, "Aggregate atmosphere timing.");
    if (startsWith(name, "temporal_denoiser") || startsWith(name, "nrd_reblur")) {
        return mappedTiming("denoiser", timings.denoiserMs, "Aggregate denoiser timing.");
    }
    if (startsWith(name, "moment_update")) return mappedTiming("moment_update", timings.momentUpdateMs);
    if (startsWith(name, "adaptive_sampling_prepare")) return mappedTiming("adaptive_sampling_diagnostics", timings.adaptiveSamplingDiagnosticsMs);
    if (startsWith(name, "adaptive_sampling_diagnostics")) return mappedTiming("adaptive_sampling_diagnostics", timings.adaptiveSamplingDiagnosticsMs);
    if (startsWith(name, "adaptive_sampling_fill")) return mappedTiming("adaptive_sampling_fill", timings.adaptiveSamplingFillMs);
    if (startsWith(name, "skip_denoiser_copy")) return mappedTiming("skip_denoiser_copy", timings.skipDenoiserCopyMs);
    if (startsWith(name, "history_copy")) return mappedTiming("history_copy", timings.historyCopyMs);
    if (startsWith(name, "taa_history_copy")) return mappedTiming("taa_history_copy", timings.taaHistoryCopyMs);
    if (name == "taa_resolve" || startsWith(name, "taa")) return mappedTiming("taa", timings.taaMs);
    if (name == "dlss_guides") return mappedTiming("dlss_guides", timings.dlssGuidesMs);
    if (name == "dlss_upscale") return mappedTiming("dlss", timings.dlssMs);
    if (name == "dlss_rr_guides") return mappedTiming("dlss_rr_guides", timings.dlssRayReconstructionGuidesMs);
    if (name == "dlss_ray_reconstruction") return mappedTiming("dlss_rr", timings.dlssRayReconstructionMs);
    if (startsWith(name, "nrd_prepare") || startsWith(name, "nrd_resolve")) {
        return mappedTiming("denoiser", timings.denoiserMs, "NRD prepare/resolve are currently represented by aggregate denoiser timing.");
    }
    if (startsWith(name, "auto_exposure_histogram_clear")) return mappedTiming("auto_exposure_histogram_clear", timings.autoExposureHistogramClearMs);
    if (startsWith(name, "auto_exposure_histogram")) return mappedTiming("auto_exposure_histogram", timings.autoExposureHistogramMs);
    if (startsWith(name, "auto_exposure_reduce")) return mappedTiming("auto_exposure_reduce", timings.autoExposureReduceMs);
    if (startsWith(name, "tone_map")) return mappedTiming("tone_map", timings.toneMapMs);
    if (startsWith(name, "selection_outline")) return mappedTiming("selection_outline", timings.selectionOutlineMs);
    if (startsWith(name, "fullscreen")) return mappedTiming("fullscreen", timings.fullscreenMs);
    if (startsWith(name, "editor_presentation")) return mappedTiming("editor_presentation", timings.editorPresentationMs);
    if (name == "wavefront_trace_rt") return mappedTiming("wavefront_trace", timings.wavefrontTraceMs);
    if (name == "wavefront_secondary_trace_rt") return mappedTiming("wavefront_secondary_trace", timings.wavefrontSecondaryTraceMs);
    if (name == "wavefront_sorted_trace_rt") return mappedTiming("wavefront_sorted_trace", timings.wavefrontSortedTraceMs);
    if (name == "wavefront_shadow_trace_rt") return mappedTiming("wavefront_shadow_trace", timings.wavefrontShadowTraceMs);
    if (name == "wavefront_shade") return mappedTiming("wavefront_shade", timings.wavefrontShadeMs);
    if (name == "wavefront_secondary_shade") return mappedTiming("wavefront_secondary_shade", timings.wavefrontSecondaryShadeMs);
    if (name == "wavefront_sorted_shade") return mappedTiming("wavefront_sorted_shade", timings.wavefrontSortedShadeMs);
    if (name == "wavefront_compact") return mappedTiming("wavefront_compact", timings.wavefrontCompactMs);
    if (name == "wavefront_sort") return mappedTiming("wavefront_sort", timings.wavefrontSortMs);
    return {};
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

const char* domainColor(RenderGraphQueueDomain domain) {
    switch (domain) {
    case RenderGraphQueueDomain::Graphics:          return "lightblue";
    case RenderGraphQueueDomain::RayTracing:        return "lightblue";
    case RenderGraphQueueDomain::Compute:           return "lightgreen";
    case RenderGraphQueueDomain::SameFamilyCompute: return "palegreen";
    case RenderGraphQueueDomain::Transfer:          return "lightyellow";
    }
    return "white";
}

std::string passNameOrNull(const std::vector<RenderGraphPass>& passes, uint32_t passIndex) {
    if (passIndex == std::numeric_limits<uint32_t>::max()) {
        return {};
    }
    return passIndex < passes.size() ? passes[passIndex].name() : std::string{};
}

bool lifetimesOverlap(const TransientResourceLifetime& a, const TransientResourceLifetime& b) {
    if (a.firstUsePass == std::numeric_limits<uint32_t>::max() ||
        b.firstUsePass == std::numeric_limits<uint32_t>::max()) {
        return true;
    }
    return !(a.lastUsePass < b.firstUsePass || b.lastUsePass < a.firstUsePass);
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
    if ((access & VK_ACCESS_2_SHADER_READ_BIT) && (access & VK_ACCESS_2_SHADER_WRITE_BIT)) return "SHADER_READ_WRITE";
    if ((access & VK_ACCESS_2_SHADER_STORAGE_READ_BIT) && (access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) return "SHADER_STORAGE_READ_WRITE";
    if (access & VK_ACCESS_2_SHADER_WRITE_BIT) return "SHADER_WRITE";
    if (access & VK_ACCESS_2_SHADER_READ_BIT) return "SHADER_READ";
    if (access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) return "SHADER_STORAGE_WRITE";
    if (access & VK_ACCESS_2_SHADER_STORAGE_READ_BIT) return "SHADER_STORAGE_READ";
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
    const auto& lifetimes = graph.resourceLifetimes();

    j["resource_aliasing"] = {
        {"enabled", graph.aliasingEnabled()},
        {"mode", "diagnostic_lifetimes"},
    };

    nlohmann::json passesJson = nlohmann::json::array();
    for (uint32_t passIndex : compiledOrder) {
        const auto& pass = passes[passIndex];
        nlohmann::json pj;
        pj["name"] = pass.name();
        pj["skipped"] = false;

        const RenderGraphQueueDomain domain = pass.queueDomain();
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
                if (res.type == RenderGraphResource::Type::Buffer) {
                    bj["buffer_offset"] = res.bufferOffset;
                    bj["size_bytes"] = res.size;
                }
                bj["before_pass"] = beforePassName;
                bj["after_pass"] = pass.name();
                bj["before_queue"] = queueNameForDomain(barrier.beforeQueue);
                bj["after_queue"] = queueNameForDomain(barrier.afterQueue);
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
        const TimingMapping timing = timingForPassName(timings, pass.name());
        pj["gpu_ms"] = timing.gpuMs;
        pj["gpu_ms_mapped"] = timing.mapped;
        pj["timing_source"] = timing.timingSource;
        pj["profile_timing_key"] = timing.mapped ? nlohmann::json(timing.profileTimingKey) : nlohmann::json(nullptr);
        if (timing.note[0] != '\0') {
            pj["timing_note"] = timing.note;
        }

        passesJson.push_back(pj);
    }
    j["passes"] = passesJson;

    nlohmann::json resourcesJson = nlohmann::json::array();
    for (uint32_t resourceIndex = 0; resourceIndex < resources.size(); ++resourceIndex) {
        const auto& res = resources[resourceIndex];
        nlohmann::json rj;
        rj["index"] = resourceIndex;
        rj["name"] = res.debugName ? res.debugName : "unnamed";
        rj["type"] = resourceTypeName(res.type);
        rj["lifetime"] = resourceLifetimeName(res.lifetime);
        rj["external"] = res.external;
        if (res.type == RenderGraphResource::Type::Texture) {
            rj["format"] = static_cast<int>(res.format);
            rj["extent"] = { {"width", res.extent.width}, {"height", res.extent.height} };
        } else {
            rj["size_bytes"] = res.size;
            rj["buffer_offset"] = res.bufferOffset;
        }
        if (resourceIndex < lifetimes.size()) {
            const auto& lifetime = lifetimes[resourceIndex];
            auto passNameOrJsonNull = [&passes](uint32_t passIndex) -> nlohmann::json {
                if (passIndex == std::numeric_limits<uint32_t>::max()) {
                    return nullptr;
                }
                return passIndex < passes.size() ? nlohmann::json(passes[passIndex].name()) : nlohmann::json(nullptr);
            };
            rj["lifetime_interval"] = {
                {"first_use_pass", passNameOrJsonNull(lifetime.firstUsePass)},
                {"last_use_pass", passNameOrJsonNull(lifetime.lastUsePass)},
                {"first_read_pass", passNameOrJsonNull(lifetime.firstReadPass)},
                {"last_read_pass", passNameOrJsonNull(lifetime.lastReadPass)},
                {"first_write_pass", passNameOrJsonNull(lifetime.firstWritePass)},
                {"last_write_pass", passNameOrJsonNull(lifetime.lastWritePass)},
                {"first_queue", queueNameForDomain(lifetime.firstUseQueue)},
                {"last_queue", queueNameForDomain(lifetime.lastUseQueue)},
                {"first_access", {
                    {"stage", vulkanStageName(lifetime.firstAccess.stage)},
                    {"access", vulkanAccessName(lifetime.firstAccess.access)},
                    {"layout", vulkanLayoutName(lifetime.firstAccess.layout)}
                }},
                {"last_access", {
                    {"stage", vulkanStageName(lifetime.lastAccess.stage)},
                    {"access", vulkanAccessName(lifetime.lastAccess.access)},
                    {"layout", vulkanLayoutName(lifetime.lastAccess.layout)}
                }},
            };
            rj["aliasing"] = {
                {"eligible", lifetime.aliasEligible},
                {"aliased", lifetime.aliased},
                {"alias_group", lifetime.aliasGroup},
                {"estimated_bytes", lifetime.estimatedBytes},
            };
        }
        resourcesJson.push_back(rj);
    }
    j["resources"] = resourcesJson;

    nlohmann::json aliasChecks = nlohmann::json::array();
    for (uint32_t i = 0; i < resources.size(); ++i) {
        if (i >= lifetimes.size() || lifetimes[i].firstUsePass == std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        for (uint32_t k = i + 1; k < resources.size(); ++k) {
            if (k >= lifetimes.size() || lifetimes[k].firstUsePass == std::numeric_limits<uint32_t>::max()) {
                continue;
            }
            if (!resourcesAliasCompatible(resources[i], resources[k])) {
                continue;
            }
            const bool overlap = lifetimesOverlap(lifetimes[i], lifetimes[k]);
            const bool sharedPhysicalHandle = resourcesSharePhysicalHandle(resources[i], resources[k]);
            const bool physicalCandidate = !resources[i].external && !resources[k].external &&
                lifetimes[i].aliasEligible && lifetimes[k].aliasEligible && !overlap;
            const bool physicalAliasActive = sharedPhysicalHandle && !overlap;
            const bool scheduleCandidate = !overlap;
            nlohmann::json check;
            check["resource_a"] = resources[i].debugName ? resources[i].debugName : "unnamed";
            check["resource_b"] = resources[k].debugName ? resources[k].debugName : "unnamed";
            check["compatible_shape"] = true;
            check["lifetimes_overlap"] = overlap;
            check["shared_physical_handle"] = sharedPhysicalHandle;
            check["schedule_candidate"] = scheduleCandidate;
            check["physical_alias_candidate"] = physicalCandidate || physicalAliasActive;
            check["estimated_saved_bytes"] = scheduleCandidate ? std::min(lifetimes[i].estimatedBytes, lifetimes[k].estimatedBytes) : 0;
            check["reason"] = physicalAliasActive
                ? "active physical handle reuse"
                : (overlap
                ? "lifetimes overlap"
                : (physicalCandidate ? "eligible transient non-overlap" : "non-overlap but externally allocated or not transient"));
            aliasChecks.push_back(check);
        }
    }
    j["alias_checks"] = aliasChecks;

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
        if (res.type == RenderGraphResource::Type::Buffer) {
            bj["buffer_offset"] = res.bufferOffset;
            bj["size_bytes"] = res.size;
        }
        bj["before_pass"] = beforePassName;
        bj["after_pass"] = afterPassName;
        bj["before_queue"] = queueNameForDomain(barrier.beforeQueue);
        bj["after_queue"] = queueNameForDomain(barrier.afterQueue);
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

    std::vector<std::vector<std::string>> resourceReaders(resources.size());
    std::vector<std::vector<std::string>> resourceWriters(resources.size());
    nlohmann::json invalidUses = nlohmann::json::array();
    for (uint32_t passIndex : compiledOrder) {
        if (passIndex >= passes.size()) {
            continue;
        }
        const auto& pass = passes[passIndex];
        for (const auto& use : pass.uses()) {
            if (!use.resource.valid() || use.resource.index >= resources.size()) {
                invalidUses.push_back({
                    {"pass", pass.name()},
                    {"resource_index", use.resource.index},
                    {"access", use.access == PassAccess::Read ? "read" : (use.access == PassAccess::Write ? "write" : "read_write")},
                    {"domain", pipelineDomainName(use.domain)},
                });
                continue;
            }
            if (use.access == PassAccess::Read || use.access == PassAccess::ReadWrite) {
                resourceReaders[use.resource.index].push_back(pass.name());
            }
            if (use.access == PassAccess::Write || use.access == PassAccess::ReadWrite) {
                resourceWriters[use.resource.index].push_back(pass.name());
            }
        }
    }

    nlohmann::json resourceOwnership = nlohmann::json::array();
    nlohmann::json resourcesWithoutWriters = nlohmann::json::array();
    nlohmann::json multiWriterResources = nlohmann::json::array();
    for (uint32_t resourceIndex = 0; resourceIndex < resources.size(); ++resourceIndex) {
        const auto& res = resources[resourceIndex];
        const char* resourceName = res.debugName ? res.debugName : "unnamed";
        nlohmann::json ownership = {
            {"resource", resourceName},
            {"index", resourceIndex},
            {"lifetime", resourceLifetimeName(res.lifetime)},
            {"external", res.external},
            {"has_initial_access", res.hasInitialAccess},
            {"first_writer", resourceWriters[resourceIndex].empty() ? nlohmann::json(nullptr) : nlohmann::json(resourceWriters[resourceIndex].front())},
            {"writer_count", resourceWriters[resourceIndex].size()},
            {"reader_count", resourceReaders[resourceIndex].size()},
            {"writers", resourceWriters[resourceIndex]},
            {"readers", resourceReaders[resourceIndex]},
        };
        resourceOwnership.push_back(ownership);

        if (!res.external && !res.hasInitialAccess && resourceReaders[resourceIndex].size() > 0 && resourceWriters[resourceIndex].empty()) {
            resourcesWithoutWriters.push_back(ownership);
        }
        if (resourceWriters[resourceIndex].size() > 1) {
            multiWriterResources.push_back(ownership);
        }
    }
    const bool ownershipValidationPassed = invalidUses.empty();
    j["resource_ownership_validation"] = {
        {"schema_version", 1},
        {"pass_count", passes.size()},
        {"compiled_pass_count", compiledOrder.size()},
        {"resource_count", resources.size()},
        {"invalid_use_count", invalidUses.size()},
        {"resources_without_declared_writer_count", resourcesWithoutWriters.size()},
        {"multi_writer_resource_count", multiWriterResources.size()},
        {"invalid_uses", std::move(invalidUses)},
        {"resources_without_declared_writer", std::move(resourcesWithoutWriters)},
        {"multi_writer_resources", std::move(multiWriterResources)},
        {"resource_ownership", std::move(resourceOwnership)},
        {"passed", ownershipValidationPassed},
    };

    const uint32_t invalidPassIndex = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> passExecutionPosition(passes.size(), invalidPassIndex);
    for (uint32_t orderIndex = 0; orderIndex < compiledOrder.size(); ++orderIndex) {
        const uint32_t passIndex = compiledOrder[orderIndex];
        if (passIndex < passExecutionPosition.size()) {
            passExecutionPosition[passIndex] = orderIndex;
        }
    }
    auto passIndexValid = [&](uint32_t passIndex) {
        return passIndex == invalidPassIndex || passIndex < passes.size();
    };
    auto passExecutionOrder = [&](uint32_t passIndex) {
        if (passIndex == invalidPassIndex || passIndex >= passExecutionPosition.size()) {
            return invalidPassIndex;
        }
        return passExecutionPosition[passIndex];
    };
    auto passNameJson = [&](uint32_t passIndex) -> nlohmann::json {
        if (passIndex == invalidPassIndex || passIndex >= passes.size()) {
            return nullptr;
        }
        return passes[passIndex].name();
    };
    auto lifetimeResourceEvidence = [&](uint32_t resourceIndex, const TransientResourceLifetime& lifetime) {
        const auto& res = resources[resourceIndex];
        return nlohmann::json{
            {"resource", res.debugName ? res.debugName : "unnamed"},
            {"index", resourceIndex},
            {"lifetime", resourceLifetimeName(res.lifetime)},
            {"external", res.external},
            {"first_use_pass", passNameJson(lifetime.firstUsePass)},
            {"last_use_pass", passNameJson(lifetime.lastUsePass)},
            {"first_read_pass", passNameJson(lifetime.firstReadPass)},
            {"last_read_pass", passNameJson(lifetime.lastReadPass)},
            {"first_write_pass", passNameJson(lifetime.firstWritePass)},
            {"last_write_pass", passNameJson(lifetime.lastWritePass)},
            {"alias_eligible", lifetime.aliasEligible},
            {"aliased", lifetime.aliased},
            {"alias_group", lifetime.aliasGroup},
            {"estimated_bytes", lifetime.estimatedBytes},
        };
    };
    auto lifetimeOverlapsByExecution = [&](const TransientResourceLifetime& a, const TransientResourceLifetime& b) {
        const uint32_t aFirst = passExecutionOrder(a.firstUsePass);
        const uint32_t aLast = passExecutionOrder(a.lastUsePass);
        const uint32_t bFirst = passExecutionOrder(b.firstUsePass);
        const uint32_t bLast = passExecutionOrder(b.lastUsePass);
        if (aFirst == invalidPassIndex || aLast == invalidPassIndex ||
            bFirst == invalidPassIndex || bLast == invalidPassIndex) {
            return true;
        }
        return !(aLast < bFirst || bLast < aFirst);
    };

    nlohmann::json lifetimeViolations = nlohmann::json::array();
    nlohmann::json lifetimeWarnings = nlohmann::json::array();
    auto addLifetimeViolation = [&](const char* code, const char* message, nlohmann::json evidence) {
        lifetimeViolations.push_back({
            {"code", code},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
    };
    auto addLifetimeWarning = [&](const char* code, const char* message, nlohmann::json evidence) {
        lifetimeWarnings.push_back({
            {"code", code},
            {"message", message},
            {"evidence", std::move(evidence)},
        });
    };

    uint32_t usedResourceCount = 0;
    uint32_t transientResourceCount = 0;
    uint32_t aliasEligibleResourceCount = 0;
    uint32_t aliasedResourceCount = 0;
    uint64_t estimatedTransientBytes = 0;
    for (uint32_t resourceIndex = 0; resourceIndex < resources.size(); ++resourceIndex) {
        const auto& res = resources[resourceIndex];
        const bool used = !resourceReaders[resourceIndex].empty() || !resourceWriters[resourceIndex].empty();
        const bool expectedAliasEligible = !res.external && res.lifetime == RenderGraphResource::Lifetime::Transient;
        if (used) {
            ++usedResourceCount;
        }
        if (res.lifetime == RenderGraphResource::Lifetime::Transient) {
            ++transientResourceCount;
        }
        if (resourceIndex >= lifetimes.size()) {
            addLifetimeViolation(
                "missing_lifetime_record",
                "Every render graph resource must have a compiled lifetime record.",
                {
                    {"resource", res.debugName ? res.debugName : "unnamed"},
                    {"index", resourceIndex},
                    {"used", used},
                });
            continue;
        }

        const TransientResourceLifetime& lifetime = lifetimes[resourceIndex];
        if (lifetime.resourceIndex != resourceIndex) {
            addLifetimeViolation(
                "lifetime_resource_index_mismatch",
                "Compiled lifetime record must point back to its resource index.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (used && (lifetime.firstUsePass == invalidPassIndex || lifetime.lastUsePass == invalidPassIndex)) {
            addLifetimeViolation(
                "used_resource_missing_use_interval",
                "Used resources must report first and last use passes.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (!passIndexValid(lifetime.firstUsePass) ||
            !passIndexValid(lifetime.lastUsePass) ||
            !passIndexValid(lifetime.firstReadPass) ||
            !passIndexValid(lifetime.lastReadPass) ||
            !passIndexValid(lifetime.firstWritePass) ||
            !passIndexValid(lifetime.lastWritePass)) {
            addLifetimeViolation(
                "lifetime_pass_index_out_of_range",
                "Lifetime pass indices must reference compiled graph passes or be unset.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        const uint32_t firstUseOrder = passExecutionOrder(lifetime.firstUsePass);
        const uint32_t lastUseOrder = passExecutionOrder(lifetime.lastUsePass);
        if (used && firstUseOrder != invalidPassIndex && lastUseOrder != invalidPassIndex && firstUseOrder > lastUseOrder) {
            addLifetimeViolation(
                "lifetime_interval_inverted",
                "Resource lifetime intervals must be ordered by compiled execution order.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (!resourceReaders[resourceIndex].empty() &&
            (lifetime.firstReadPass == invalidPassIndex || lifetime.lastReadPass == invalidPassIndex)) {
            addLifetimeViolation(
                "resource_readers_missing_read_interval",
                "Resources with readers must report first and last read passes.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (!resourceWriters[resourceIndex].empty() &&
            (lifetime.firstWritePass == invalidPassIndex || lifetime.lastWritePass == invalidPassIndex)) {
            addLifetimeViolation(
                "resource_writers_missing_write_interval",
                "Resources with writers must report first and last write passes.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        auto intervalWithinUse = [&](uint32_t passIndex) {
            const uint32_t order = passExecutionOrder(passIndex);
            return passIndex == invalidPassIndex ||
                (firstUseOrder != invalidPassIndex &&
                 lastUseOrder != invalidPassIndex &&
                 order != invalidPassIndex &&
                 firstUseOrder <= order &&
                 order <= lastUseOrder);
        };
        if (!intervalWithinUse(lifetime.firstReadPass) ||
            !intervalWithinUse(lifetime.lastReadPass) ||
            !intervalWithinUse(lifetime.firstWritePass) ||
            !intervalWithinUse(lifetime.lastWritePass)) {
            addLifetimeViolation(
                "read_write_interval_outside_use_interval",
                "Read/write lifetime intervals must be contained inside the first/last use interval.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (!used && lifetime.firstUsePass != invalidPassIndex) {
            addLifetimeWarning(
                "unused_resource_has_lifetime_interval",
                "A resource with no compiled readers or writers still reports a lifetime interval.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (lifetime.aliasEligible != expectedAliasEligible) {
            addLifetimeViolation(
                "alias_eligibility_mismatch",
                "Only non-external transient resources may be alias-eligible.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (!expectedAliasEligible && (lifetime.aliased || lifetime.aliasGroup != 0u)) {
            addLifetimeViolation(
                "non_transient_resource_alias_state",
                "Persistent, temporal, or external resources must not be assigned transient alias state.",
                lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (lifetime.aliasEligible) {
            ++aliasEligibleResourceCount;
            estimatedTransientBytes += static_cast<uint64_t>(lifetime.estimatedBytes);
        }
        if (lifetime.aliased) {
            ++aliasedResourceCount;
        }
    }

    nlohmann::json aliasGroups = nlohmann::json::array();
    uint32_t aliasGroupCount = 0;
    uint64_t estimatedAliasSavingsBytes = 0;
    std::vector<uint32_t> observedAliasGroups;
    for (const TransientResourceLifetime& lifetime : lifetimes) {
        if (lifetime.aliasGroup == 0u ||
            std::find(observedAliasGroups.begin(), observedAliasGroups.end(), lifetime.aliasGroup) != observedAliasGroups.end()) {
            continue;
        }
        observedAliasGroups.push_back(lifetime.aliasGroup);
    }
    for (uint32_t aliasGroup : observedAliasGroups) {
        std::vector<uint32_t> groupResources;
        uint64_t groupBytes = 0;
        uint64_t groupMaxBytes = 0;
        nlohmann::json groupResourceJson = nlohmann::json::array();
        for (uint32_t resourceIndex = 0; resourceIndex < lifetimes.size() && resourceIndex < resources.size(); ++resourceIndex) {
            const TransientResourceLifetime& lifetime = lifetimes[resourceIndex];
            if (lifetime.aliasGroup != aliasGroup) {
                continue;
            }
            groupResources.push_back(resourceIndex);
            groupBytes += static_cast<uint64_t>(lifetime.estimatedBytes);
            groupMaxBytes = std::max<uint64_t>(groupMaxBytes, static_cast<uint64_t>(lifetime.estimatedBytes));
            groupResourceJson.push_back(lifetimeResourceEvidence(resourceIndex, lifetime));
        }
        if (groupResources.size() > 1) {
            ++aliasGroupCount;
            estimatedAliasSavingsBytes += groupBytes > groupMaxBytes ? groupBytes - groupMaxBytes : 0ull;
        }
        for (size_t a = 0; a < groupResources.size(); ++a) {
            for (size_t b = a + 1; b < groupResources.size(); ++b) {
                const uint32_t resourceA = groupResources[a];
                const uint32_t resourceB = groupResources[b];
                if (!resourcesAliasCompatible(resources[resourceA], resources[resourceB])) {
                    addLifetimeViolation(
                        "alias_group_incompatible_resources",
                        "Resources assigned to the same alias group must have compatible type, format, extent, and usage.",
                        {
                            {"alias_group", aliasGroup},
                            {"resource_a", lifetimeResourceEvidence(resourceA, lifetimes[resourceA])},
                            {"resource_b", lifetimeResourceEvidence(resourceB, lifetimes[resourceB])},
                        });
                }
                if (lifetimeOverlapsByExecution(lifetimes[resourceA], lifetimes[resourceB])) {
                    addLifetimeViolation(
                        "alias_group_lifetime_overlap",
                        "Resources assigned to the same alias group must not overlap in compiled execution order.",
                        {
                            {"alias_group", aliasGroup},
                            {"resource_a", lifetimeResourceEvidence(resourceA, lifetimes[resourceA])},
                            {"resource_b", lifetimeResourceEvidence(resourceB, lifetimes[resourceB])},
                        });
                }
            }
        }
        aliasGroups.push_back({
            {"alias_group", aliasGroup},
            {"resource_count", groupResources.size()},
            {"estimated_group_bytes", groupBytes},
            {"estimated_physical_bytes", groupMaxBytes},
            {"estimated_saved_bytes", groupBytes > groupMaxBytes ? groupBytes - groupMaxBytes : 0ull},
            {"resources", std::move(groupResourceJson)},
        });
    }

    nlohmann::json sharedPhysicalBacking = nlohmann::json::array();
    uint32_t sharedPhysicalBackingPairCount = 0;
    uint32_t overlappingSharedPhysicalBackingPairCount = 0;
    for (uint32_t i = 0; i < resources.size() && i < lifetimes.size(); ++i) {
        if (lifetimes[i].firstUsePass == invalidPassIndex) {
            continue;
        }
        for (uint32_t k = i + 1; k < resources.size() && k < lifetimes.size(); ++k) {
            if (lifetimes[k].firstUsePass == invalidPassIndex || !resourcesSharePhysicalHandle(resources[i], resources[k])) {
                continue;
            }
            const bool overlap = lifetimeOverlapsByExecution(lifetimes[i], lifetimes[k]);
            const bool graphControlledAlias =
                lifetimes[i].aliasEligible &&
                lifetimes[k].aliasEligible &&
                lifetimes[i].aliasGroup != 0u &&
                lifetimes[i].aliasGroup == lifetimes[k].aliasGroup;
            ++sharedPhysicalBackingPairCount;
            if (overlap) {
                ++overlappingSharedPhysicalBackingPairCount;
            }
            nlohmann::json backingPair = {
                {"resource_a", lifetimeResourceEvidence(i, lifetimes[i])},
                {"resource_b", lifetimeResourceEvidence(k, lifetimes[k])},
                {"lifetimes_overlap", overlap},
                {"graph_controlled_alias", graphControlledAlias},
            };
            sharedPhysicalBacking.push_back(backingPair);
            if (overlap && (resources[i].external || resources[k].external)) {
                addLifetimeWarning(
                    "external_shared_physical_backing_overlap",
                    "External persistent resources share the same physical backing over overlapping lifetimes; this is reported separately from graph-controlled transient aliasing.",
                    backingPair);
            } else if (overlap && !graphControlledAlias) {
                addLifetimeViolation(
                    "internal_shared_physical_backing_overlap",
                    "Internal resources that share physical backing over overlapping lifetimes must be represented by an explicit graph-controlled alias contract.",
                    backingPair);
            }
        }
    }

    const bool lifetimeValidationPassed = lifetimeViolations.empty();
    j["resource_lifetime_validation"] = {
        {"schema_version", 1},
        {"resource_count", resources.size()},
        {"lifetime_count", lifetimes.size()},
        {"used_resource_count", usedResourceCount},
        {"transient_resource_count", transientResourceCount},
        {"aliasing_enabled", graph.aliasingEnabled()},
        {"alias_eligible_resource_count", aliasEligibleResourceCount},
        {"aliased_resource_count", aliasedResourceCount},
        {"alias_group_count", aliasGroupCount},
        {"estimated_transient_bytes", estimatedTransientBytes},
        {"estimated_alias_savings_bytes", estimatedAliasSavingsBytes},
        {"alias_groups", std::move(aliasGroups)},
        {"shared_physical_backing_pair_count", sharedPhysicalBackingPairCount},
        {"overlapping_shared_physical_backing_pair_count", overlappingSharedPhysicalBackingPairCount},
        {"shared_physical_backing", std::move(sharedPhysicalBacking)},
        {"violation_count", lifetimeViolations.size()},
        {"warning_count", lifetimeWarnings.size()},
        {"violations", std::move(lifetimeViolations)},
        {"warnings", std::move(lifetimeWarnings)},
        {"passed", lifetimeValidationPassed},
    };

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

    for (uint32_t passIndex : compiledOrder) {
        const auto& pass = passes[passIndex];
        const RenderGraphQueueDomain domain = pass.queueDomain();

        const TimingMapping timing = timingForPassName(timings, pass.name());
        file << "    \"" << pass.name() << "\" [fillcolor=" << domainColor(domain)
             << ", label=\"" << pass.name() << "\\n(";
        switch (domain) {
        case RenderGraphQueueDomain::RayTracing: file << "RayTracing"; break;
        case RenderGraphQueueDomain::Compute: file << "Compute"; break;
        case RenderGraphQueueDomain::SameFamilyCompute: file << "SameFamilyCompute"; break;
        case RenderGraphQueueDomain::Graphics: file << "Graphics"; break;
        case RenderGraphQueueDomain::Transfer: file << "Transfer"; break;
        }
        file << ")\\n" << timing.gpuMs << "ms\"];\n";
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
