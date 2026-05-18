#include "rtv/DebugProfilerPanel.h"

#include "rtv/RendererDebug.h"

#include <imgui.h>

#include <string>

namespace rtv {

void DebugProfilerPanel::draw(EditorRuntimeState& state, EditorRequests& requests) {
    if (!ImGui::Begin("Debug / Profiler")) {
        ImGui::End();
        return;
    }

    const GpuFrameTimings& timings = state.renderer.timings();
    const float gpuMs = timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs;
    ImGui::Text("FPS: %.1f", state.cpuFrameMs > 0.0f ? 1000.0f / state.cpuFrameMs : 0.0f);
    ImGui::Text("CPU frame time: %.2f ms", state.cpuFrameMs);
    ImGui::Text("GPU frame time: %.2f ms", gpuMs);
    ImGui::Text("Path trace: %.2f ms", timings.pathTraceMs);
    ImGui::Text("Denoiser: %.2f ms", timings.denoiserMs);
    ImGui::Text("Presentation: %.2f ms", timings.fullscreenMs);
    ImGui::Text("Sample count: %u", state.renderer.sampleCount());
    ImGui::Text("Last reset: %s", accumulationResetReasonName(state.renderer.lastAccumulationResetReason()));

    const auto& invalidations = state.renderer.validationLog().invalidations();
    if (invalidations.size() >= 2) {
        ImGui::Text("Previous reset: %s", invalidations[invalidations.size() - 2].reason.c_str());
    }

    RendererSettings settings = state.renderer.settings();
    bool changed = false;
    ImGui::SeparatorText("Debug View");
    ImGui::Text("Active: %s", rendererDebugViewName(settings.debugView));
    editorDebugViewCombo("View", settings, changed);
    const bool computeTraversalView =
        settings.debugView == RendererDebugView::TraversalSteps ||
        settings.debugView == RendererDebugView::BvhDepth ||
        settings.debugView == RendererDebugView::TlasSteps ||
        settings.debugView == RendererDebugView::TraversalMismatch;
    if (computeTraversalView && state.renderer.activeBackend() == RendererBackend::HardwareRayTracing) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "This diagnostic is only available on the Compute backend.");
    }
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
    if (ImGui::TreeNode("Accumulation resets")) {
        const size_t first = invalidations.size() > 8 ? invalidations.size() - 8 : 0;
        for (size_t i = first; i < invalidations.size(); ++i) {
            ImGui::Text("frame %llu: %s",
                static_cast<unsigned long long>(invalidations[i].frame),
                invalidations[i].reason.c_str());
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

} // namespace rtv
