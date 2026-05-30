#pragma once

#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <cstdint>
#include <vector>

struct GLFWwindow;

namespace rtv {

class VulkanContext;

class Swapchain final : private NonCopyable {
public:
    Swapchain(const VulkanContext& context, GLFWwindow* window);
    Swapchain(const VulkanContext& context, VkExtent2D extent);
    ~Swapchain();

    void recreate();
    void destroy();

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkFormat format() const { return imageFormat_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkImage image(uint32_t index) const { return images_.at(index); }
    [[nodiscard]] VkImageView imageView(uint32_t index) const { return imageViews_.at(index); }
    [[nodiscard]] uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }

private:
    struct SupportDetails {
        VkSurfaceCapabilitiesKHR capabilities{};
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    [[nodiscard]] SupportDetails querySupport() const;
    [[nodiscard]] VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
    [[nodiscard]] VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    [[nodiscard]] VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

    void create();
    void createHeadless();
    void createImageViews();
    void createHeadlessImages();

    const VulkanContext& context_;
    GLFWwindow* window_ = nullptr;
    bool headless_ = false;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    VkDeviceMemory headlessMemory_[2] = {};
};

} // namespace rtv
