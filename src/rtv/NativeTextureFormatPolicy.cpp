#include "rtv/NativeTextureFormatPolicy.h"

#include <cctype>
#include <array>

namespace rtv {
namespace {

std::string lowerAscii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool formatHasSampledImageSupport(VkPhysicalDevice physicalDevice, VkFormat format) {
    if (physicalDevice == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
        return false;
    }
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

bool preferCandidate(VkFormat format, const NativeTextureFormatSupport& support) {
    return nativeTextureFormatSupportedByPolicy(format, support);
}

NativeTextureFormatSelection makeSelection(
    NativeTextureRole role,
    NativeTextureColorSpace colorSpace,
    const NativeTextureFormatSupport& support,
    std::vector<VkFormat> candidates,
    std::string reason) {
    NativeTextureFormatSelection selection;
    selection.role = role;
    selection.colorSpace = colorSpace;
    selection.candidates = std::move(candidates);
    selection.reason = std::move(reason);
    for (const VkFormat candidate : selection.candidates) {
        if (preferCandidate(candidate, support)) {
            selection.selectedFormat = candidate;
            selection.supported = true;
            selection.fallbackUsed = candidate != selection.candidates.front();
            break;
        }
    }
    if (!selection.supported) {
        selection.fallbackReason = "No candidate format is sampled-image supported by the selected platform policy.";
        return selection;
    }
    switch (selection.selectedFormat) {
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC1_sRGB";
        break;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC1_UNORM";
        break;
    case VK_FORMAT_BC3_SRGB_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC3_sRGB";
        break;
    case VK_FORMAT_BC2_SRGB_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC2_sRGB";
        break;
    case VK_FORMAT_BC2_UNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC2_UNORM";
        break;
    case VK_FORMAT_BC3_UNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC3_UNORM";
        break;
    case VK_FORMAT_BC7_SRGB_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC7_sRGB";
        break;
    case VK_FORMAT_BC7_UNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC7_UNORM";
        break;
    case VK_FORMAT_BC5_UNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC5_UNORM";
        break;
    case VK_FORMAT_BC5_SNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC5_SNORM";
        break;
    case VK_FORMAT_BC4_UNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC4_UNORM";
        break;
    case VK_FORMAT_BC4_SNORM_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC4_SNORM";
        break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC6H_UFLOAT";
        break;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        selection.blockCompressed = true;
        selection.compressionFamily = "BC6H_SFLOAT";
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        selection.compressionFamily = "RGBA16F";
        break;
    case VK_FORMAT_R8G8B8A8_SRGB:
        selection.compressionFamily = "RGBA8_sRGB";
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
        selection.compressionFamily = "RGBA8_UNORM";
        break;
    default:
        selection.compressionFamily = nativeTextureFormatName(selection.selectedFormat);
        break;
    }
    if (selection.fallbackUsed) {
        selection.fallbackReason = "Preferred block-compressed format is unavailable; selected sampled RGBA fallback.";
    }
    return selection;
}

} // namespace

NativeTextureFormatSupport nativeTextureFormatSupportFromPhysicalDevice(VkPhysicalDevice physicalDevice, std::string platformName) {
    NativeTextureFormatSupport support;
    support.queriedFromVulkan = physicalDevice != VK_NULL_HANDLE;
    support.platformName = std::move(platformName);
    support.bc1SrgbSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
    support.bc1UnormSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    support.bc3SrgbSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC3_SRGB_BLOCK);
    support.bc3UnormSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC3_UNORM_BLOCK);
    support.bc7SrgbSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC7_SRGB_BLOCK);
    support.bc7UnormSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC7_UNORM_BLOCK);
    support.bc5UnormSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC5_UNORM_BLOCK);
    support.bc4UnormSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC4_UNORM_BLOCK);
    support.bc6hUfloatSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC6H_UFLOAT_BLOCK);
    support.bc6hSfloatSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_BC6H_SFLOAT_BLOCK);
    support.rgba8SrgbSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_R8G8B8A8_SRGB);
    support.rgba8UnormSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_R8G8B8A8_UNORM);
    support.rgba16fSampled = formatHasSampledImageSupport(physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT);
    return support;
}

NativeTextureFormatSupport nativeTextureOfflineFallbackFormatSupport() {
    NativeTextureFormatSupport support;
    support.queriedFromVulkan = false;
    support.platformName = "offline-rgba-fallback";
    support.bc1SrgbSampled = false;
    support.bc1UnormSampled = false;
    support.bc3SrgbSampled = false;
    support.bc3UnormSampled = false;
    support.bc7SrgbSampled = false;
    support.bc7UnormSampled = false;
    support.bc5UnormSampled = false;
    support.bc4UnormSampled = false;
    support.bc6hUfloatSampled = false;
    support.bc6hSfloatSampled = false;
    support.rgba8SrgbSampled = true;
    support.rgba8UnormSampled = true;
    support.rgba16fSampled = true;
    return support;
}

NativeTextureFormatSupport nativeTextureAllBcFormatSupportForAudit() {
    NativeTextureFormatSupport support;
    support.queriedFromVulkan = false;
    support.platformName = "offline-all-bc-audit";
    support.bc1SrgbSampled = true;
    support.bc1UnormSampled = true;
    support.bc3SrgbSampled = true;
    support.bc3UnormSampled = true;
    support.bc7SrgbSampled = true;
    support.bc7UnormSampled = true;
    support.bc5UnormSampled = true;
    support.bc4UnormSampled = true;
    support.bc6hUfloatSampled = true;
    support.bc6hSfloatSampled = true;
    support.rgba8SrgbSampled = true;
    support.rgba8UnormSampled = true;
    support.rgba16fSampled = true;
    return support;
}

NativeTextureFormatSelection selectNativeTextureFormat(
    NativeTextureRole role,
    NativeTextureColorSpace colorSpace,
    const NativeTextureFormatSupport& support) {
    switch (role) {
    case NativeTextureRole::BaseColor:
    case NativeTextureRole::Emissive:
    case NativeTextureRole::SpecularColor:
    case NativeTextureRole::SheenColor:
        return makeSelection(role, colorSpace, support, {VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_SRGB}, "Color texture: prefer BC7 sRGB, fall back to RGBA8 sRGB.");
    case NativeTextureRole::Normal:
    case NativeTextureRole::ClearcoatNormal:
        return makeSelection(role, colorSpace, support, {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM}, "Normal map: prefer BC5, fall back to RGBA8 UNORM.");
    case NativeTextureRole::MetallicRoughness:
    case NativeTextureRole::Specular:
    case NativeTextureRole::Transmission:
    case NativeTextureRole::Clearcoat:
    case NativeTextureRole::Sheen:
    case NativeTextureRole::Iridescence:
    case NativeTextureRole::Anisotropy:
    case NativeTextureRole::Thickness:
    case NativeTextureRole::Data:
        return makeSelection(role, colorSpace, support, {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM}, "Linear multi-channel mask/data texture: prefer BC7 UNORM, fall back to RGBA8 UNORM.");
    case NativeTextureRole::Metallic:
    case NativeTextureRole::Roughness:
    case NativeTextureRole::Occlusion:
    case NativeTextureRole::Opacity:
    case NativeTextureRole::Height:
    case NativeTextureRole::ClearcoatRoughness:
    case NativeTextureRole::SheenRoughness:
    case NativeTextureRole::IridescenceThickness:
        return makeSelection(role, colorSpace, support, {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM}, "Scalar mask texture: prefer BC4, fall back to RGBA8 UNORM.");
    case NativeTextureRole::EnvironmentHdr:
        return makeSelection(role, colorSpace, support, {VK_FORMAT_R16G16B16A16_SFLOAT}, "HDR environment texture: use RGBA16F v1 target.");
    case NativeTextureRole::Unknown:
    default:
        if (colorSpace == NativeTextureColorSpace::Srgb) {
            return makeSelection(role, colorSpace, support, {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}, "Unknown sRGB texture role: use RGBA8 fallback until role confidence improves.");
        }
        return makeSelection(role, colorSpace, support, {VK_FORMAT_R8G8B8A8_UNORM}, "Unknown/data texture role: use RGBA8 fallback until role confidence improves.");
    }
}

NativeTextureRole nativeTextureRoleFromString(std::string_view role) {
    const std::string normalized = lowerAscii(role);
    if (normalized == "basecolor" || normalized == "base_color" || normalized == "albedo" || normalized == "diffuse") return NativeTextureRole::BaseColor;
    if (normalized == "normal" || normalized == "norm" || normalized == "nrm") return NativeTextureRole::Normal;
    if (normalized == "metallicroughness" || normalized == "metallic_roughness" || normalized == "metalrough" || normalized == "orm" || normalized == "rma" || normalized == "mra" || normalized == "mrao" || normalized == "rmao") return NativeTextureRole::MetallicRoughness;
    if (normalized == "metallic" || normalized == "metalness" || normalized == "metal") return NativeTextureRole::Metallic;
    if (normalized == "roughness" || normalized == "rough") return NativeTextureRole::Roughness;
    if (normalized == "occlusion" || normalized == "ambientocclusion" || normalized == "ao") return NativeTextureRole::Occlusion;
    if (normalized == "emissive" || normalized == "emission" || normalized == "emit") return NativeTextureRole::Emissive;
    if (normalized == "opacity" || normalized == "alpha" || normalized == "transparency" || normalized == "mask") return NativeTextureRole::Opacity;
    if (normalized == "height" || normalized == "displacement" || normalized == "disp" || normalized == "bump") return NativeTextureRole::Height;
    if (normalized == "thickness" || normalized == "volume_thickness" || normalized == "volumethickness") return NativeTextureRole::Thickness;
    if (normalized == "environmenthdr" || normalized == "environment" || normalized == "hdr" || normalized == "hdri") return NativeTextureRole::EnvironmentHdr;
    if (normalized == "data") return NativeTextureRole::Data;
    if (normalized == "specular") return NativeTextureRole::Specular;
    if (normalized == "specularcolor" || normalized == "specular_color") return NativeTextureRole::SpecularColor;
    if (normalized == "transmission") return NativeTextureRole::Transmission;
    if (normalized == "clearcoat") return NativeTextureRole::Clearcoat;
    if (normalized == "clearcoatroughness" || normalized == "clearcoat_roughness") return NativeTextureRole::ClearcoatRoughness;
    if (normalized == "clearcoatnormal" || normalized == "clearcoat_normal") return NativeTextureRole::ClearcoatNormal;
    if (normalized == "sheen") return NativeTextureRole::Sheen;
    if (normalized == "sheencolor" || normalized == "sheen_color") return NativeTextureRole::SheenColor;
    if (normalized == "sheenroughness" || normalized == "sheen_roughness") return NativeTextureRole::SheenRoughness;
    if (normalized == "iridescence") return NativeTextureRole::Iridescence;
    if (normalized == "iridescencethickness" || normalized == "iridescence_thickness") return NativeTextureRole::IridescenceThickness;
    if (normalized == "anisotropy") return NativeTextureRole::Anisotropy;
    return NativeTextureRole::Unknown;
}

bool nativeTextureFormatSupportedByPolicy(VkFormat format, const NativeTextureFormatSupport& support) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return support.bc1SrgbSampled;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return support.bc1UnormSampled;
    case VK_FORMAT_BC2_SRGB_BLOCK: return support.bc3SrgbSampled;
    case VK_FORMAT_BC2_UNORM_BLOCK: return support.bc3UnormSampled;
    case VK_FORMAT_BC3_SRGB_BLOCK: return support.bc3SrgbSampled;
    case VK_FORMAT_BC3_UNORM_BLOCK: return support.bc3UnormSampled;
    case VK_FORMAT_BC7_SRGB_BLOCK: return support.bc7SrgbSampled;
    case VK_FORMAT_BC7_UNORM_BLOCK: return support.bc7UnormSampled;
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK: return support.bc5UnormSampled;
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return support.bc4UnormSampled;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return support.bc6hUfloatSampled;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return support.bc6hSfloatSampled;
    case VK_FORMAT_R8G8B8A8_SRGB: return support.rgba8SrgbSampled;
    case VK_FORMAT_R8G8B8A8_UNORM: return support.rgba8UnormSampled;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return support.rgba16fSampled;
    default: return false;
    }
}

std::string nativeTextureFormatName(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return "BC1_RGB_UNORM";
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return "BC1_RGB_SRGB";
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return "BC1_RGBA_UNORM";
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return "BC1_RGBA_SRGB";
    case VK_FORMAT_BC2_UNORM_BLOCK: return "BC2_UNORM";
    case VK_FORMAT_BC2_SRGB_BLOCK: return "BC2_SRGB";
    case VK_FORMAT_BC3_UNORM_BLOCK: return "BC3_UNORM";
    case VK_FORMAT_BC3_SRGB_BLOCK: return "BC3_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4_UNORM";
    case VK_FORMAT_BC4_SNORM_BLOCK: return "BC4_SNORM";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5_UNORM";
    case VK_FORMAT_BC5_SNORM_BLOCK: return "BC5_SNORM";
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return "BC6H_UFLOAT";
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return "BC6H_SFLOAT";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7_UNORM";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7_SRGB";
    case VK_FORMAT_UNDEFINED: return "UNDEFINED";
    default: return "VkFormat_" + std::to_string(static_cast<uint32_t>(format));
    }
}

std::string nativeTextureRoleName(NativeTextureRole role) {
    switch (role) {
    case NativeTextureRole::BaseColor: return "baseColor";
    case NativeTextureRole::Normal: return "normal";
    case NativeTextureRole::MetallicRoughness: return "metallicRoughness";
    case NativeTextureRole::Metallic: return "metallic";
    case NativeTextureRole::Roughness: return "roughness";
    case NativeTextureRole::Occlusion: return "occlusion";
    case NativeTextureRole::Emissive: return "emissive";
    case NativeTextureRole::Opacity: return "opacity";
    case NativeTextureRole::Height: return "height";
    case NativeTextureRole::EnvironmentHdr: return "environmentHDR";
    case NativeTextureRole::Data: return "data";
    case NativeTextureRole::Specular: return "specular";
    case NativeTextureRole::SpecularColor: return "specularColor";
    case NativeTextureRole::Transmission: return "transmission";
    case NativeTextureRole::Clearcoat: return "clearcoat";
    case NativeTextureRole::ClearcoatRoughness: return "clearcoatRoughness";
    case NativeTextureRole::ClearcoatNormal: return "clearcoatNormal";
    case NativeTextureRole::Sheen: return "sheen";
    case NativeTextureRole::SheenColor: return "sheenColor";
    case NativeTextureRole::SheenRoughness: return "sheenRoughness";
    case NativeTextureRole::Iridescence: return "iridescence";
    case NativeTextureRole::IridescenceThickness: return "iridescenceThickness";
    case NativeTextureRole::Anisotropy: return "anisotropy";
    case NativeTextureRole::Thickness: return "thickness";
    case NativeTextureRole::Unknown:
    default: return "unknown";
    }
}

std::string nativeTextureColorSpaceName(NativeTextureColorSpace colorSpace) {
    switch (colorSpace) {
    case NativeTextureColorSpace::Srgb: return "sRGB";
    case NativeTextureColorSpace::SourceDefined: return "sourceDefined";
    case NativeTextureColorSpace::HdrLinear: return "linearHDR";
    case NativeTextureColorSpace::Linear:
    default: return "linear";
    }
}

std::string nativeTextureCompressionPolicyName(NativeTextureCompressionPolicy policy) {
    switch (policy) {
    case NativeTextureCompressionPolicy::PreserveSourceContainer: return "preserveSourceContainer";
    case NativeTextureCompressionPolicy::DecodedRgba8: return "decodedRgba8";
    case NativeTextureCompressionPolicy::DecodedHdr: return "decodedHdr";
    case NativeTextureCompressionPolicy::None:
    default: return "none";
    }
}

} // namespace rtv
