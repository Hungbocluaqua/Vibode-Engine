#pragma once

#include "rtv/NativeAssetFormat.h"

#include <Volk/volk.h>

#include <string>
#include <string_view>
#include <vector>

namespace rtv {

struct NativeTextureFormatSupport {
    bool queriedFromVulkan = false;
    std::string platformName = "offline-default";
    bool bc1SrgbSampled = false;
    bool bc1UnormSampled = false;
    bool bc3SrgbSampled = false;
    bool bc3UnormSampled = false;
    bool bc7SrgbSampled = false;
    bool bc7UnormSampled = false;
    bool bc5UnormSampled = false;
    bool bc4UnormSampled = false;
    bool bc6hUfloatSampled = false;
    bool bc6hSfloatSampled = false;
    bool rgba8SrgbSampled = true;
    bool rgba8UnormSampled = true;
    bool rgba16fSampled = true;
};

struct NativeTextureFormatSelection {
    NativeTextureRole role = NativeTextureRole::Unknown;
    NativeTextureColorSpace colorSpace = NativeTextureColorSpace::Linear;
    VkFormat selectedFormat = VK_FORMAT_UNDEFINED;
    bool supported = false;
    bool fallbackUsed = false;
    bool blockCompressed = false;
    std::string compressionFamily;
    std::string reason;
    std::string fallbackReason;
    std::vector<VkFormat> candidates;
};

[[nodiscard]] NativeTextureFormatSupport nativeTextureFormatSupportFromPhysicalDevice(VkPhysicalDevice physicalDevice, std::string platformName = "vulkan-physical-device");
[[nodiscard]] NativeTextureFormatSupport nativeTextureOfflineFallbackFormatSupport();
[[nodiscard]] NativeTextureFormatSupport nativeTextureAllBcFormatSupportForAudit();
[[nodiscard]] NativeTextureFormatSelection selectNativeTextureFormat(
    NativeTextureRole role,
    NativeTextureColorSpace colorSpace,
    const NativeTextureFormatSupport& support);
[[nodiscard]] NativeTextureRole nativeTextureRoleFromString(std::string_view role);
[[nodiscard]] bool nativeTextureFormatSupportedByPolicy(VkFormat format, const NativeTextureFormatSupport& support);
[[nodiscard]] std::string nativeTextureFormatName(VkFormat format);
[[nodiscard]] std::string nativeTextureRoleName(NativeTextureRole role);
[[nodiscard]] std::string nativeTextureColorSpaceName(NativeTextureColorSpace colorSpace);
[[nodiscard]] std::string nativeTextureCompressionPolicyName(NativeTextureCompressionPolicy policy);

} // namespace rtv
