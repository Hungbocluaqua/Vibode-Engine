#pragma once

#include "rtv/EditorLayer.h"
#include "rtv/Image.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace rtv {

class PathTracerRenderer;
class BufferUploader;
class ResourceAllocator;
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

    UiOverlay(GLFWwindow* window, const VulkanContext& context, const Swapchain& swapchain, ResourceAllocator& allocator, BufferUploader& uploader);
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
        CameraController* camera,
        const UndoStack* undoStack,
        const EditorRenderJobStatus* renderJob,
        const EditorPlacementStatus* placement,
        float cpuFrameMs,
        NotificationManager* notifications,
        bool externalMouseCapture = false);
    [[nodiscard]] EditorRequests buildProjectManager(
        const ProjectContext* project,
        const std::string& sceneLoadingStatus,
        bool sceneLoadRunning,
        float sceneLoadProgress,
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
    [[nodiscard]] DescriptorPoolStats descriptorPoolStats() const { return descriptorPoolStats_; }

private:
    struct UiTextureKey {
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        [[nodiscard]] bool operator==(const UiTextureKey& other) const {
            return imageView == other.imageView && imageLayout == other.imageLayout;
        }
    };
    struct UiTextureKeyHash {
        [[nodiscard]] size_t operator()(const UiTextureKey& key) const {
            const auto view = static_cast<size_t>(reinterpret_cast<uintptr_t>(key.imageView));
            return view ^ (static_cast<size_t>(key.imageLayout) + 0x9e3779b9u + (view << 6u) + (view >> 2u));
        }
    };
    struct AssetPreviewTexture {
        Image image{};
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t writeStamp = 0;
        uint64_t sourceSize = 0;
    };
    struct RetiredTextureDescriptor {
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        uint64_t releaseFrame = 0;
    };
    struct RetiredAssetPreviewTexture {
        AssetPreviewTexture texture;
        uint64_t releaseFrame = 0;
    };

    static void checkVkResult(VkResult result);
    static VkDescriptorSet acquireEditorTextureCallback(void* user, VkImageView imageView, VkImageLayout imageLayout);
    static VkDescriptorSet acquireEditorAssetPreviewCallback(void* user, const std::filesystem::path& path, uint32_t* width, uint32_t* height);
    [[nodiscard]] VkDescriptorSet acquireEditorTexture(VkImageView imageView, VkImageLayout imageLayout);
    [[nodiscard]] VkDescriptorSet acquireEditorAssetPreviewTexture(const std::filesystem::path& path, uint32_t* width, uint32_t* height);
    [[nodiscard]] uint64_t nextTextureReleaseFrame() const;
    void retireTextureDescriptor(VkDescriptorSet descriptor);
    void retireAssetPreviewTexture(AssetPreviewTexture&& texture);
    void releaseRetiredTextures(bool force = false);
    void invalidateEditorTextures();
    void invalidateAssetPreviewTextures();
    void loadEditorFonts();
    void applyDarkStyle();

    GLFWwindow* window_ = nullptr;
    const VulkanContext& context_;
    ResourceAllocator& allocator_;
    BufferUploader& uploader_;
    VkFormat colorAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    DescriptorPoolStats descriptorPoolStats_{};
    EditorLayer editor_;
    std::unordered_map<UiTextureKey, VkDescriptorSet, UiTextureKeyHash> editorTextures_;
    std::unordered_map<std::string, AssetPreviewTexture> assetPreviewTextures_;
    std::vector<RetiredTextureDescriptor> retiredTextureDescriptors_;
    std::vector<RetiredAssetPreviewTexture> retiredAssetPreviewTextures_;
    VkSampler assetPreviewSampler_ = VK_NULL_HANDLE;
    VkImageView viewportImageView_ = VK_NULL_HANDLE;
    VkDescriptorSet viewportTexture_ = VK_NULL_HANDLE;
    VkExtent2D viewportTextureExtent_{};
    uint64_t uiFrameSerial_ = 0;
    uint64_t textureRetireFrameDelay_ = 4;
    bool frameBegun_ = false;
};

} // namespace rtv
