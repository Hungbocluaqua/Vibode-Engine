#include "rtv/ViewportPanel.h"

#include "rtv/RendererDebug.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace rtv {

void ViewportPanel::draw(EditorRuntimeState& state, EditorRequests&) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Viewport")) {
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        hovered_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
        state.viewport.focused = focused_;
        state.viewport.hovered = hovered_;

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        lastContentExtent_.width = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.x)));
        lastContentExtent_.height = static_cast<uint32_t>(std::max(1.0f, std::floor(avail.y)));
        VkExtent2D expectedExtent = lastContentExtent_;
        const float scale = state.renderer.settings().renderResolutionScale;
        expectedExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(expectedExtent.width) * scale));
        expectedExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(expectedExtent.height) * scale));
        const bool imageMatchesPanel =
            expectedExtent.width == state.viewport.renderExtent.width &&
            expectedExtent.height == state.viewport.renderExtent.height;

        const ImVec2 imagePos = ImGui::GetCursorScreenPos();
        if (imageMatchesPanel && state.viewport.textureReady && state.viewport.texture != VK_NULL_HANDLE) {
            ImGui::Image(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(state.viewport.texture)),
                avail,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f));
        } else {
            ImGui::Dummy(avail);
            ImGui::GetWindowDrawList()->AddRectFilled(
                imagePos,
                ImVec2(imagePos.x + avail.x, imagePos.y + avail.y),
                IM_COL32(18, 20, 23, 255));
        }

        const RendererSettings& settings = state.renderer.settings();
        const GpuFrameTimings& timings = state.renderer.timings();
        const VkExtent2D extent = state.viewport.renderExtent;
        const float frameMs = timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(imagePos.x + 10.0f, imagePos.y + 10.0f),
            ImVec2(imagePos.x + 360.0f, imagePos.y + 104.0f),
            IM_COL32(0, 0, 0, 150),
            4.0f);
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 18.0f), IM_COL32_WHITE,
            ("Resolution: " + std::to_string(extent.width) + "x" + std::to_string(extent.height)).c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 38.0f), IM_COL32_WHITE,
            ("Samples: " + std::to_string(state.renderer.sampleCount())).c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 58.0f), IM_COL32_WHITE,
            rendererDebugViewName(settings.debugView));
        ImGui::GetWindowDrawList()->AddText(ImVec2(imagePos.x + 18.0f, imagePos.y + 78.0f), IM_COL32_WHITE,
            ("Frame: " + std::to_string(frameMs) + " ms").c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

VkExtent2D ViewportPanel::desiredRenderExtent(VkExtent2D fallback) const {
    if (lastContentExtent_.width == 0 || lastContentExtent_.height == 0) {
        return fallback;
    }
    return lastContentExtent_;
}

} // namespace rtv
