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
class UndoStack;
struct SceneAsset;

class UiOverlay final : private NonCopyable {
public:
    struct DescriptorPoolStats {
        bool present = false;
        uint32_t maxSets = 0;
        uint32_t combinedImageSamplerDescriptors = 0;
        uint32_t sampledImageDescriptors = 0;
        uint32_t samplerDescriptors = 0;
        uint32_t viewportDescriptorAllocated = 0;
    };

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
        const std::optional<std::filesystem::path>& scenePath,
        const ProjectContext* project,
        const AssetRegistry* assetRegistry,
        bool sceneDirty,
        const std::vector<EntityId>* instanceEntities,
        const std::string& sceneLoadingStatus,
        bool sceneLoadRunning,
        float sceneLoadProgress,
        const CameraController* camera,
        const UndoStack* undoStack,
        float cpuFrameMs,
        NotificationManager* notifications,
        bool externalMouseCapture = false);
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
    [[nodiscard]] DescriptorPoolStats descriptorPoolStats() const { return descriptorPoolStats_; }

private:
    static void checkVkResult(VkResult result);
    void applyDarkStyle();

    GLFWwindow* window_ = nullptr;
    const VulkanContext& context_;
    VkFormat colorAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    DescriptorPoolStats descriptorPoolStats_{};
    EditorLayer editor_;
    VkImageView viewportImageView_ = VK_NULL_HANDLE;
    VkDescriptorSet viewportTexture_ = VK_NULL_HANDLE;
    VkExtent2D viewportTextureExtent_{};
    bool frameBegun_ = false;
};

} // namespace rtv
