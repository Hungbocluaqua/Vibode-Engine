#include "rtv/DebugProfilerPanel.h"

#include "rtv/AtmosphereLutSystem.h"
#include "rtv/RendererDebug.h"
#include "rtv/TemporalSystem.h"
#include "rtv/ValidationSceneSuite.h"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace rtv {

namespace {

const char* imageLayoutName(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        return "undefined";
    case VK_IMAGE_LAYOUT_GENERAL:
        return "general";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return "shader-read";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return "transfer-src";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return "transfer-dst";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return "color-attachment";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return "depth-stencil";
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        return "present";
    default:
        return "other";
    }
}

const char* atmosphereLutName(size_t index) {
    switch (index) {
    case 0:
        return "Transmittance";
    case 1:
        return "Multi-scatter";
    case 2:
        return "Sky-view";
    case 3:
        return "Aerial perspective";
    default:
        return "Unknown";
    }
}

} // namespace

void DebugProfilerPanel::draw(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Debug / Profiler")) {
        ImGui::End();
        return;
    }

    const GpuFrameTimings& timings = state.renderer.timings();
    const float gpuMs =
        timings.pathTraceMs +
        timings.restirSpatialMs +
        timings.fogIntegrateMs +
        timings.atmosphereMs +
        timings.denoiserMs +
        timings.historyCopyMs +
        timings.taaMs +
        timings.autoExposureMs +
        timings.toneMapMs +
        timings.selectionOutlineMs +
        timings.fullscreenMs;
    ImGui::Text("FPS: %.1f", state.cpuFrameMs > 0.0f ? 1000.0f / state.cpuFrameMs : 0.0f);
    ImGui::Text("CPU frame time: %.2f ms", state.cpuFrameMs);
    ImGui::Text("GPU frame time: %.2f ms", gpuMs);
    ImGui::Text("Atmosphere: %.2f ms", timings.atmosphereMs);
    ImGui::Text("Path trace: %.2f ms", timings.pathTraceMs);
    ImGui::Text("ReSTIR spatial: %.2f ms", timings.restirSpatialMs);
    ImGui::Text("Fog integrate: %.2f ms", timings.fogIntegrateMs);
    ImGui::Text("Denoiser: %.2f ms", timings.denoiserMs);
    ImGui::Text("History copy: %.2f ms", timings.historyCopyMs);
    ImGui::Text("TAA: %.2f ms", timings.taaMs);
    ImGui::Text("Auto exposure: %.2f ms", timings.autoExposureMs);
    ImGui::Text("Tone map: %.2f ms", timings.toneMapMs);
    ImGui::Text("Selection outline: %.2f ms", timings.selectionOutlineMs);
    ImGui::Text("Presentation: %.2f ms", timings.fullscreenMs);
    ImGui::Text("Sample count: %u", state.renderer.sampleCount());
    ImGui::Text("Last reset: %s", accumulationResetReasonName(state.renderer.lastAccumulationResetReason()));

    const GpuPipelineStatistics stats = state.renderer.pipelineStats();
    if (stats.valid) {
        ImGui::SeparatorText("Pipeline Statistics");
        const auto fmtCount = [](uint64_t val) {
            if (val >= 1000000000ull) {
                ImGui::Text("%.2f G", static_cast<double>(val) / 1.0e9);
            } else if (val >= 1000000ull) {
                ImGui::Text("%.2f M", static_cast<double>(val) / 1.0e6);
            } else if (val >= 1000ull) {
                ImGui::Text("%.2f K", static_cast<double>(val) / 1.0e3);
            } else {
                ImGui::Text("%llu", static_cast<unsigned long long>(val));
            }
        };
        ImGui::Text("Ray invocations: "); ImGui::SameLine(); fmtCount(stats.rayInvocations);
        ImGui::Text("Triangle hits: "); ImGui::SameLine(); fmtCount(stats.triangleHits);
        ImGui::Text("AABB hits: "); ImGui::SameLine(); fmtCount(stats.aabbHits);
    }

    const auto& invalidations = state.renderer.validationLog().invalidations();
    if (invalidations.size() >= 2) {
        ImGui::Text("Previous reset: %s", invalidations[invalidations.size() - 2].reason.c_str());
    }

    RendererSettings settings = state.renderer.settings();
    bool changed = false;
    ImGui::SeparatorText("Debug View");
    ImGui::Text("Active: %s", rendererDebugViewName(settings.debugView));
    editorDebugViewCombo("View", settings, changed);
    changed |= ImGui::SliderFloat("Debug Scale", &settings.debugScale, 0.05f, 10.0f, "%.2f");
    if (changed) {
        requestSettings(requests, settings);
    }

    ImGui::SeparatorText("Scene Stats");
    const MeshParamsUniform& meshParams = state.renderer.scene().meshParams();
    ImGui::Text("Vertices: %u", meshParams.vertexCount);
    ImGui::Text("Triangles: %u", meshParams.triangleCount);
    ImGui::Text("Meshes: %u  Primitives: %u  Instances: %u", meshParams.meshCount, meshParams.primitiveCount, meshParams.instanceCount);
    ImGui::Text("Materials: %u  Spheres: %u", meshParams.materialCount, meshParams.sphereCount);
    ImGui::Text("Lights: %u  Emissive area: %.3f", meshParams.lightCount, meshParams.emissiveTotalArea);
    ImGui::Text("BVH nodes: %u", meshParams.bvhNodeCount);
    ImGui::Text("Local BVH: %u nodes  %u triangles", meshParams.localBvhNodeCount, meshParams.localTriangleCount);
    ImGui::Text("TLAS: %u nodes  %u indices", meshParams.tlasNodeCount, meshParams.tlasInstanceIndexCount);
    ImGui::Text("Descriptor texture slots: %zu", state.renderer.scene().materialTextureDescriptors().size());
    const RayTracingRendererStats rtStats = state.renderer.rayTracingStats();
    if (rtStats.active) {
        ImGui::SeparatorText("Hardware RT");
        ImGui::Text("BLAS: %u  Instances: %u", rtStats.blasCount, rtStats.instanceCount);
        ImGui::Text("AS memory: %.2f MB", static_cast<double>(rtStats.accelerationStructureBytes) / (1024.0 * 1024.0));
        ImGui::Text("Last TLAS refit: %.3f ms", rtStats.lastTlasRefitMs);
        ImGui::Text("SBT: %.2f KB", static_cast<double>(rtStats.sbtBytes) / 1024.0);
    }

    ImGui::SeparatorText("Render Graph");
    const RendererValidationLog& validation = state.renderer.validationLog();
    if (ImGui::TreeNodeEx("Pass order", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const std::string& pass : validation.passEvents()) {
            ImGui::BulletText("%s", pass.c_str());
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Barriers")) {
        for (const std::string& barrier : validation.barrierEvents()) {
            ImGui::TextWrapped("%s", barrier.c_str());
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Resource states")) {
        for (const ResourceStateEvent& event : validation.resourceStateEvents()) {
            ImGui::TextWrapped(
                "%s: %s -> %s | layout %s -> %s | stage %llu -> %llu | access %llu -> %llu",
                event.resource.c_str(),
                event.beforePass.c_str(),
                event.afterPass.c_str(),
                imageLayoutName(event.beforeLayout),
                imageLayoutName(event.afterLayout),
                static_cast<unsigned long long>(event.beforeStage),
                static_cast<unsigned long long>(event.afterStage),
                static_cast<unsigned long long>(event.beforeAccess),
                static_cast<unsigned long long>(event.afterAccess));
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Accumulation resets")) {
        const size_t first = invalidations.size() > 8 ? invalidations.size() - 8 : 0;
        for (size_t i = first; i < invalidations.size(); ++i) {
            ImGui::Text("frame %llu: %s",
                static_cast<unsigned long long>(invalidations[i].frame),
                invalidations[i].reason.c_str());
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Scene update routes")) {
        for (const SceneUpdateRouteEvent& route : validation.sceneUpdateRoutes()) {
            ImGui::BulletText(
                "%s -> %s: %llu",
                route.kind.c_str(),
                route.action.c_str(),
                static_cast<unsigned long long>(route.count));
        }
        ImGui::TreePop();
    }
    if (const TemporalSystem* temporal = state.renderer.temporalSystem(); temporal != nullptr && ImGui::TreeNode("Temporal state")) {
        ImGui::Text("Frame: %llu", static_cast<unsigned long long>(temporal->frameIndex()));
        ImGui::Text("Camera cut: %s", temporal->isCameraCut() ? "yes" : "no");
        ImGui::Text("Last reset: %s", accumulationResetReasonName(temporal->lastResetReason()));
        ImGui::Text("History memory: %.2f MB", static_cast<double>(temporal->totalHistoryMemoryBytes()) / (1024.0 * 1024.0));
        for (const auto& [name, slot] : temporal->historySlots()) {
            ImGui::BulletText(
                "%s: %s %s %ux%u %.2f MB last=%llu",
                name.c_str(),
                slot.resident ? "resident" : "evicted",
                slot.valid ? "valid" : "invalid",
                slot.extent.width,
                slot.extent.height,
                static_cast<double>(slot.estimatedBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(slot.lastWrittenFrame));
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Atmosphere LUTs")) {
        const AtmosphereLutStats atmosphere = state.renderer.atmosphereLutStats();
        for (size_t i = 0; i < atmosphere.dirty.size(); ++i) {
            ImGui::BulletText(
                "%s: %s %s generated=%llu",
                atmosphereLutName(i),
                atmosphere.dirty[i] ? "dirty" : "clean",
                atmosphere.generatedThisRecord[i] ? "generated-this-frame" : "idle",
                static_cast<unsigned long long>(atmosphere.generationCounts[i]));
        }
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Validation Scenes");
    for (const ValidationSceneDescriptor& scene : validationSceneSuite()) {
        ImGui::PushID(scene.relativePath.data());
        const std::filesystem::path path = validationScenePath(scene);
        ImGui::Text("%s", scene.name.data());
        ImGui::SameLine();
        if (ImGui::SmallButton("Load")) {
            requests.loadSceneJson = path;
            requests.sceneUpdate = SceneUpdateKind::TopologyChanged;
        }
        ImGui::TextDisabled("%s", scene.category.data());
        ImGui::TextWrapped("%s", scene.purpose.data());
        ImGui::TextDisabled("Reference: %s", scene.referenceChecksum.data());
        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace rtv
