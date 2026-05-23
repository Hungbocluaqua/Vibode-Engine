#include "rtv/GpuDiagnosticsPanel.h"

#include "rtv/PathTracerRenderer.h"
#include "rtv/TemporalSystem.h"

#include <imgui.h>

#include <cmath>

namespace rtv {

void GpuDiagnosticsPanel::draw(const EditorRuntimeState& state) {
    if (!ImGui::Begin("GPU Diagnostics")) {
        ImGui::End();
        return;
    }

    const GpuFrameTimings& timings = state.renderer.timings();
    constexpr double mb = 1024.0 * 1024.0;

    ImGui::SeparatorText("Frame Timing");
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

    ImGui::Text("Total GPU:     %.2f ms", gpuMs);
    ImGui::Text("Atmosphere:    %.2f ms", timings.atmosphereMs);
    ImGui::Text("Path trace:    %.2f ms", timings.pathTraceMs);
    ImGui::Text("ReSTIR:        %.2f ms", timings.restirSpatialMs);
    ImGui::Text("Fog:           %.2f ms", timings.fogIntegrateMs);
    ImGui::Text("Denoiser:      %.2f ms", timings.denoiserMs);
    ImGui::Text("History copy:  %.2f ms", timings.historyCopyMs);
    ImGui::Text("TAA:           %.2f ms", timings.taaMs);
    ImGui::Text("Auto exposure: %.2f ms", timings.autoExposureMs);
    ImGui::Text("Tone map:      %.2f ms", timings.toneMapMs);
    ImGui::Text("Selection:     %.2f ms", timings.selectionOutlineMs);
    ImGui::Text("Present:       %.2f ms", timings.fullscreenMs);

    ImGui::SeparatorText("Rays");
    const VkExtent2D extent = state.renderer.renderExtent();
    const RendererSettings& settings = state.renderer.settings();
    const uint64_t pixels = static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height);
    const uint64_t raysPerFrame = pixels * std::max(settings.maxBounces, 1u);
    const float gpuSec = gpuMs > 0.0f ? gpuMs / 1000.0f : 0.001f;
    ImGui::Text("Resolution:    %u x %u", extent.width, extent.height);
    ImGui::Text("Pixels:        %llu", static_cast<unsigned long long>(pixels));
    ImGui::Text("Rays/frame:    %llu", static_cast<unsigned long long>(raysPerFrame));
    ImGui::Text("Rays/sec:      %.2f M", (static_cast<double>(raysPerFrame) / gpuSec) / 1.0e6);

    ImGui::SeparatorText("Sample State");
    ImGui::Text("Sample count:  %u", state.renderer.sampleCount());
    ImGui::Text("Last reset:    %s", accumulationResetReasonName(state.renderer.lastAccumulationResetReason()));

    if (const TemporalSystem* temporal = state.renderer.temporalSystem()) {
        ImGui::Text("Camera cut:    %s", temporal->isCameraCut() ? "yes" : "no");
        ImGui::Text("History VRAM:  %.2f MB", static_cast<double>(temporal->totalHistoryMemoryBytes()) / mb);
    }

    ImGui::SeparatorText("Memory");
    const auto& texTable = state.renderer.scene().materialTextureTable();
    ImGui::Text("Textures:      %u / %u slots", texTable.residentCount(), texTable.slotCount());
    ImGui::Text("Texture alloc: %u allocated, %.1f%% frag",
        texTable.allocatedCount(), texTable.fragmentation() * 100.0f);
    if (state.sceneDocument != nullptr) {
        ImGui::Text("Entities:      %zu", state.sceneDocument->registry().liveCount());
    }
    const RayTracingRendererStats rt = state.renderer.rayTracingStats();
    if (rt.active) {
        ImGui::Text("AS memory:     %.2f MB", static_cast<double>(rt.accelerationStructureBytes) / mb);
    }

    ImGui::End();
}

} // namespace rtv
