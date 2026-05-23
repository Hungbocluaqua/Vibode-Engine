#pragma once

#include "rtv/EditorLayer.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace rtv {

class PathTracerRenderer;
class Swapchain;
class VulkanContext;
class AssetManager;
class CameraController;
class NotificationManager;
class SceneDocument;
struct SceneAsset;

class UiOverlay final : private NonCopyable {
public:
    UiOverlay(GLFWwindow* window, const VulkanContext& context, const Swapchain& swapchain);
    ~UiOverlay();

    void beginFrame();
    [[nodiscard]] EditorRequests build(
        PathTracerRenderer& renderer,
        VkExtent2D extent,
        const SceneAsset* importedScene,
        SceneDocument* sceneDocument,
        const AssetManager* assets,
        const std::optional<std::filesystem::path>& gltfPath,
        const std::optional<std::filesystem::path>& hdrPath,
        const std::vector<EntityId>* instanceEntities,
        const std::string& sceneLoadingStatus,
        const CameraController* camera,
        float cpuFrameMs,
        NotificationManager* notifications);
    void record(VkCommandBuffer commandBuffer);
    void onSwapchainRecreated(const Swapchain& swapchain);

    [[nodiscard]] bool wantsMouse() const;
    [[nodiscard]] bool wantsKeyboard() const;
    [[nodiscard]] bool wantsTextInput() const;
    [[nodiscard]] bool viewportInteractionActive() const;
    [[nodiscard]] bool viewportHovered() const;
    [[nodiscard]] bool rendersPathTracerInViewport() const { return true; }
    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    void invalidateViewportTexture();
    [[nodiscard]] EditorLayer& editor() { return editor_; }

private:
    static void checkVkResult(VkResult result);
    void applyDarkStyle();

    GLFWwindow* window_ = nullptr;
    const VulkanContext& context_;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    EditorLayer editor_;
    VkImageView viewportImageView_ = VK_NULL_HANDLE;
    VkDescriptorSet viewportTexture_ = VK_NULL_HANDLE;
    VkExtent2D viewportTextureExtent_{};
    bool frameBegun_ = false;
};

} // namespace rtv
