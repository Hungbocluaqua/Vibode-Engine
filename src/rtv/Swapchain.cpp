#include "rtv/Swapchain.h"

#include "rtv/Check.h"
#include "rtv/VulkanContext.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace rtv {

Swapchain::Swapchain(const VulkanContext& context, GLFWwindow* window)
    : context_(context), window_(window) {
    create();
}

Swapchain::~Swapchain() {
    destroy();
}

void Swapchain::recreate() {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    while (width == 0 || height == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window_, &width, &height);
    }

    destroy();
    create();
}

void Swapchain::destroy() {
    for (VkImageView view : imageViews_) {
        vkDestroyImageView(context_.device(), view, nullptr);
    }
    imageViews_.clear();
    images_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(context_.device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

Swapchain::SupportDetails Swapchain::querySupport() const {
    SupportDetails details;
    checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice(), context_.surface(), &details.capabilities), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t formatCount = 0;
    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), context_.surface(), &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
    details.formats.resize(formatCount);
    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice(), context_.surface(), &formatCount, details.formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");

    uint32_t presentModeCount = 0;
    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice(), context_.surface(), &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
    details.presentModes.resize(presentModeCount);
    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice(), context_.surface(), &presentModeCount, details.presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");

    return details;
}

VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
    const auto preferred = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (preferred != formats.end()) {
        return *preferred;
    }
    return formats.front();
}

VkPresentModeKHR Swapchain::choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
    if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);

    VkExtent2D actual{
        static_cast<uint32_t>(std::max(width, 1)),
        static_cast<uint32_t>(std::max(height, 1)),
    };
    actual.width = std::clamp(actual.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    actual.height = std::clamp(actual.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return actual;
}

void Swapchain::create() {
    const auto support = querySupport();
    if (support.formats.empty() || support.presentModes.empty()) {
        throw std::runtime_error("Swapchain surface has no formats or present modes");
    }

    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
    extent_ = chooseExtent(support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, support.capabilities.maxImageCount);
    }

    const auto& queues = context_.queueFamilies();
    const std::array<uint32_t, 2> queueFamilyIndices = {
        queues.graphics.value(),
        queues.present.value(),
    };

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = context_.surface();
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (queues.graphics != queues.present) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size());
        createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    checkVk(vkCreateSwapchainKHR(context_.device(), &createInfo, nullptr, &swapchain_), "vkCreateSwapchainKHR");

    imageFormat_ = surfaceFormat.format;
    checkVk(vkGetSwapchainImagesKHR(context_.device(), swapchain_, &imageCount, nullptr), "vkGetSwapchainImagesKHR(count)");
    images_.resize(imageCount);
    checkVk(vkGetSwapchainImagesKHR(context_.device(), swapchain_, &imageCount, images_.data()), "vkGetSwapchainImagesKHR");
    createImageViews();
}

void Swapchain::createImageViews() {
    imageViews_.reserve(images_.size());
    for (VkImage image : images_) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = imageFormat_;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        checkVk(vkCreateImageView(context_.device(), &createInfo, nullptr, &view), "vkCreateImageView");
        imageViews_.push_back(view);
    }
}

} // namespace rtv
