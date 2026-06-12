#include "rtv/StreamlineRuntime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <vector>

#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>
#include <sl_pcl.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace rtv {
namespace {

constexpr std::array<const char*, 10> kRequiredEntryPoints{
    "slInit",
    "slShutdown",
    "slIsFeatureSupported",
    "slGetFeatureRequirements",
    "slGetFeatureFunction",
    "slGetNewFrameToken",
    "slSetTagForFrame",
    "slSetConstants",
    "slEvaluateFeature",
    "slFreeResources",
};

#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
struct StreamlineDispatch {
    PFun_slInit* slInit = nullptr;
    PFun_slShutdown* slShutdown = nullptr;
    PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
    PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
    PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
    PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
    PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
    PFun_slSetConstants* slSetConstants = nullptr;
    PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
    PFun_slFreeResources* slFreeResources = nullptr;
    PFun_slSetVulkanInfo* slSetVulkanInfo = nullptr;
    PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;
    PFun_slDLSSDSetOptions* slDLSSDSetOptions = nullptr;
    PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
};

template <typename T>
T* streamlineProcAddress(void* module, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<T*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    (void)module;
    (void)name;
    return nullptr;
#endif
}

sl::Feature streamlineFeatureId(StreamlineFeature feature) {
    switch (feature) {
    case StreamlineFeature::Dlss: return sl::kFeatureDLSS;
    case StreamlineFeature::DlssRayReconstruction: return sl::kFeatureDLSS_RR;
    case StreamlineFeature::DlssFrameGeneration: return sl::kFeatureDLSS_G;
    case StreamlineFeature::Reflex: return sl::kFeatureReflex;
    case StreamlineFeature::Nis: return sl::kFeatureNIS;
    case StreamlineFeature::Nrd: return sl::kFeatureNRD_INVALID;
    }
    return sl::kFeatureCommon;
}

std::string streamlineResultName(sl::Result result) {
    switch (result) {
    case sl::Result::eOk: return "eOk";
    case sl::Result::eErrorIO: return "eErrorIO";
    case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
    case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
    case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
    case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
    case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins";
    case sl::Result::eErrorVulkanAPI: return "eErrorVulkanAPI";
    case sl::Result::eErrorInvalidIntegration: return "eErrorInvalidIntegration";
    case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
    case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
    case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled";
    case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
    case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
    case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
    case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
    case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
    case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
    default: break;
    }
    std::ostringstream out;
    out << "StreamlineResult(" << static_cast<int>(result) << ")";
    return out.str();
}

std::string streamlineFeatureRequirementsText(const sl::FeatureRequirements& requirements) {
    std::ostringstream out;
    out << "vk_instance_extensions=" << requirements.vkNumInstanceExtensions
        << "; vk_device_extensions=" << requirements.vkNumDeviceExtensions
        << "; vk_features12=" << requirements.vkNumFeatures12
        << "; vk_features13=" << requirements.vkNumFeatures13
        << "; compute_queues=" << requirements.vkNumComputeQueuesRequired
        << "; graphics_queues=" << requirements.vkNumGraphicsQueuesRequired
        << "; required_tags=" << requirements.numRequiredTags
        << "; max_viewports=" << requirements.maxNumViewports;
    return out.str();
}

void appendUnique(std::vector<std::string>& target, const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    if (std::find(target.begin(), target.end(), value) == target.end()) {
        target.emplace_back(value);
    }
}

void appendUnique(std::vector<std::string>& target, const std::string& value) {
    appendUnique(target, value.c_str());
}

void appendVulkanRequirements(StreamlineVulkanRequirements& target, const sl::FeatureRequirements& source) {
    for (uint32_t i = 0; i < source.vkNumInstanceExtensions; ++i) {
        appendUnique(target.instanceExtensions, source.vkInstanceExtensions != nullptr ? source.vkInstanceExtensions[i] : nullptr);
    }
    for (uint32_t i = 0; i < source.vkNumDeviceExtensions; ++i) {
        appendUnique(target.deviceExtensions, source.vkDeviceExtensions != nullptr ? source.vkDeviceExtensions[i] : nullptr);
    }
    for (uint32_t i = 0; i < source.vkNumFeatures12; ++i) {
        appendUnique(target.features12, source.vkFeatures12 != nullptr ? source.vkFeatures12[i] : nullptr);
    }
    for (uint32_t i = 0; i < source.vkNumFeatures13; ++i) {
        appendUnique(target.features13, source.vkFeatures13 != nullptr ? source.vkFeatures13[i] : nullptr);
    }
    for (uint32_t i = 0; i < source.numRequiredTags; ++i) {
        if (source.requiredTags != nullptr) {
            appendUnique(target.requiredTags, sl::getBufferTypeAsStr(source.requiredTags[i]));
        }
    }
    target.computeQueuesRequired = std::max(target.computeQueuesRequired, source.vkNumComputeQueuesRequired);
    target.graphicsQueuesRequired = std::max(target.graphicsQueuesRequired, source.vkNumGraphicsQueuesRequired);
    target.maxViewports = std::max(target.maxViewports, source.maxNumViewports);
}

std::vector<sl::Feature> streamlineRequestedFeatures() {
    std::vector<sl::Feature> features;
#if defined(RTV_STREAMLINE_HAS_DLSS)
    features.push_back(sl::kFeatureDLSS);
#endif
#if defined(RTV_STREAMLINE_HAS_DLSS_RR)
    features.push_back(sl::kFeatureDLSS_RR);
#endif
#if defined(RTV_STREAMLINE_HAS_DLSS_FG)
    features.push_back(sl::kFeatureDLSS_G);
#endif
#if defined(RTV_STREAMLINE_HAS_REFLEX)
    features.push_back(sl::kFeatureReflex);
#endif
#if defined(RTV_STREAMLINE_HAS_NIS)
    features.push_back(sl::kFeatureNIS);
#endif
    return features;
}

std::string lowercaseAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

sl::BufferType streamlineBufferTypeForRole(const std::string& role) {
    const std::string key = lowercaseAscii(role);
    if (key == "depth" || key == "linear-depth") return sl::kBufferTypeDepth;
    if (key == "motion" || key == "motion-vectors") return sl::kBufferTypeMotionVectors;
    if (key == "color" || key == "hudless-color" || key == "input-color") return sl::kBufferTypeHUDLessColor;
    if (key == "scaling-input" || key == "scaling-input-color") return sl::kBufferTypeScalingInputColor;
    if (key == "scaling-output" || key == "scaling-output-color") return sl::kBufferTypeScalingOutputColor;
    if (key == "normals" || key == "normal") return sl::kBufferTypeNormals;
    if (key == "roughness") return sl::kBufferTypeRoughness;
    if (key == "albedo") return sl::kBufferTypeAlbedo;
    if (key == "specular-albedo") return sl::kBufferTypeSpecularAlbedo;
    if (key == "diffuse-hit-distance" || key == "diffuse-hit" || key == "diffuse-hit-noisy") return sl::kBufferTypeDiffuseHitDistance;
    if (key == "specular-hit-distance" || key == "specular-hit" || key == "specular-hit-noisy") return sl::kBufferTypeSpecularHitDistance;
    if (key == "diffuse-ray-direction") return sl::kBufferTypeDiffuseRayDirection;
    if (key == "specular-ray-direction") return sl::kBufferTypeSpecularRayDirection;
    if (key == "diffuse-ray-direction-hit-distance") return sl::kBufferTypeDiffuseRayDirectionHitDistance;
    if (key == "specular-ray-direction-hit-distance") return sl::kBufferTypeSpecularRayDirectionHitDistance;
    if (key == "reflected-albedo") return sl::kBufferTypeReflectedAlbedo;
    if (key == "disocclusion-mask") return sl::kBufferTypeDisocclusionMask;
    if (key == "reactive-mask" || key == "reactive") return sl::kBufferTypeReactiveMaskHint;
    if (key == "transparency-mask" || key == "composition-mask") return sl::kBufferTypeTransparencyAndCompositionMaskHint;
    if (key == "backbuffer" || key == "swapchain") return sl::kBufferTypeBackbuffer;
    return sl::INVALID_UINT;
}

void fillStreamlineMatrix(sl::float4x4& out, const std::array<float, 16>& values) {
    for (uint32_t row = 0; row < 4; ++row) {
        out[row] = sl::float4(
            values[row * 4 + 0],
            values[row * 4 + 1],
            values[row * 4 + 2],
            values[row * 4 + 3]);
    }
}

sl::DLSSMode streamlineDlssMode(StreamlineDlssQualityMode mode) {
    switch (mode) {
    case StreamlineDlssQualityMode::Dlaa: return sl::DLSSMode::eDLAA;
    case StreamlineDlssQualityMode::Quality: return sl::DLSSMode::eMaxQuality;
    case StreamlineDlssQualityMode::Balanced: return sl::DLSSMode::eBalanced;
    case StreamlineDlssQualityMode::Performance: return sl::DLSSMode::eMaxPerformance;
    case StreamlineDlssQualityMode::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
    }
    return sl::DLSSMode::eBalanced;
}

sl::PCLMarker streamlinePclMarker(StreamlineReflexMarker marker) {
    switch (marker) {
    case StreamlineReflexMarker::SimulationStart: return sl::PCLMarker::eSimulationStart;
    case StreamlineReflexMarker::SimulationEnd: return sl::PCLMarker::eSimulationEnd;
    case StreamlineReflexMarker::RenderSubmitStart: return sl::PCLMarker::eRenderSubmitStart;
    case StreamlineReflexMarker::RenderSubmitEnd: return sl::PCLMarker::eRenderSubmitEnd;
    case StreamlineReflexMarker::PresentStart: return sl::PCLMarker::ePresentStart;
    case StreamlineReflexMarker::PresentEnd: return sl::PCLMarker::ePresentEnd;
    }
    return sl::PCLMarker::eMaximum;
}

sl::Constants streamlineConstantsFromDesc(const StreamlineConstantsDesc& desc) {
    sl::Constants constants{};
    fillStreamlineMatrix(constants.cameraViewToClip, desc.cameraViewToClip);
    fillStreamlineMatrix(constants.clipToCameraView, desc.clipToCameraView);
    fillStreamlineMatrix(constants.clipToPrevClip, desc.clipToPrevClip);
    fillStreamlineMatrix(constants.prevClipToClip, desc.prevClipToClip);
    constants.jitterOffset = sl::float2(desc.jitterX, desc.jitterY);
    constants.mvecScale = sl::float2(desc.motionVectorScaleX, desc.motionVectorScaleY);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(desc.cameraPosition[0], desc.cameraPosition[1], desc.cameraPosition[2]);
    constants.cameraUp = sl::float3(desc.cameraUp[0], desc.cameraUp[1], desc.cameraUp[2]);
    constants.cameraRight = sl::float3(desc.cameraRight[0], desc.cameraRight[1], desc.cameraRight[2]);
    constants.cameraFwd = sl::float3(desc.cameraForward[0], desc.cameraForward[1], desc.cameraForward[2]);
    constants.cameraNear = desc.cameraNear;
    constants.cameraFar = desc.cameraFar;
    constants.cameraFOV = desc.cameraFovRadians;
    constants.cameraAspectRatio = desc.cameraAspectRatio;
    constants.depthInverted = desc.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.cameraMotionIncluded = desc.cameraMotionIncluded ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.motionVectors3D = desc.motionVectors3D ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.reset = desc.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    return constants;
}
#endif

std::string featureUnavailableReason(StreamlineFeature feature) {
    switch (feature) {
    case StreamlineFeature::Dlss:
#if defined(RTV_STREAMLINE_HAS_DLSS)
        return "Streamline DLSS feature query is not active until Vulkan startup uses the Streamline lifecycle";
#else
        return "Streamline DLSS plugin was not detected in the configured SDK";
#endif
    case StreamlineFeature::DlssRayReconstruction:
#if defined(RTV_STREAMLINE_HAS_DLSS_RR)
        return "Streamline DLSS Ray Reconstruction feature query is not active until guide resources are tagged";
#else
        return "Streamline DLSS Ray Reconstruction plugin was not detected in the configured SDK";
#endif
    case StreamlineFeature::DlssFrameGeneration:
#if defined(RTV_STREAMLINE_HAS_DLSS_FG)
        return "DLSS Frame Generation is status-only for Vulkan until Streamline feature query and presentation integration report support";
#else
        return "Streamline DLSS Frame Generation plugin was not detected in the configured SDK";
#endif
    case StreamlineFeature::Reflex:
#if defined(RTV_STREAMLINE_HAS_REFLEX)
        return "Streamline Reflex markers require runtime marker success before production Reflex is requestable";
#else
        return "Streamline Reflex plugin was not detected in the configured SDK";
#endif
    case StreamlineFeature::Nis:
#if defined(RTV_STREAMLINE_HAS_NIS)
        return "Streamline NIS runtime evaluation is not wired; renderer can use its built-in temporal/spatial upscaler path";
#else
        return "Streamline NIS plugin was not detected in the configured SDK";
#endif
    case StreamlineFeature::Nrd:
#if defined(RTV_STREAMLINE_HAS_NRD)
        return "Streamline NRD backend is not active; direct NRD remains the production fallback";
#else
        return "Configured Streamline SDK does not expose an NRD plugin path; direct NRD remains the production path";
#endif
    }
    return "Unknown Streamline feature";
}

StreamlineFeatureStatus makeUnavailableFeatureStatus(StreamlineFeature feature) {
    StreamlineFeatureStatus status{};
    status.requestable = false;
    status.supported = false;
    status.unavailableReason = featureUnavailableReason(feature);
    return status;
}

std::filesystem::path defaultRuntimeDirectory() {
#if defined(RTV_STREAMLINE_RUNTIME_DIR)
    return std::filesystem::path(RTV_STREAMLINE_RUNTIME_DIR);
#else
    return {};
#endif
}

} // namespace

StreamlineRuntime::StreamlineRuntime() {
    refreshCompileTimeStatus();
}

StreamlineRuntime::~StreamlineRuntime() {
    shutdown();
}

StreamlineStatus StreamlineRuntime::compileTimeStatus() {
    StreamlineRuntime runtime;
    return runtime.featureStatus();
}

StreamlineVulkanRequirements StreamlineRuntime::collectVulkanRequirements() {
    StreamlineVulkanRequirements requirements{};
#if !defined(RTV_STREAMLINE_RUNTIME_ENABLED)
    const StreamlineStatus status = compileTimeStatus();
    requirements.unavailableReason = status.unavailableReason;
    return requirements;
#else
    StreamlineRuntime runtime;
    StreamlineInitDesc initDesc{};
    initDesc.allowInterposer = false;
    initDesc.enableOtaUpdates = false;
    initDesc.enableLogging = false;
    requirements.available = runtime.featureStatus().runtimeConfigured;
    if (!runtime.initialize(initDesc)) {
        requirements.unavailableReason = runtime.featureStatus().unavailableReason;
        return requirements;
    }
    requirements.initialized = true;
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    if (runtime.dispatch_ != nullptr) {
        auto* dispatch = static_cast<StreamlineDispatch*>(runtime.dispatch_);
        for (sl::Feature feature : streamlineRequestedFeatures()) {
            sl::FeatureRequirements featureRequirements{};
            const sl::Result result = dispatch->slGetFeatureRequirements(feature, featureRequirements);
            if (result == sl::Result::eOk) {
                appendVulkanRequirements(requirements, featureRequirements);
            } else {
                appendUnique(requirements.requiredTags, std::string("requirements_query_failed:") + streamlineResultName(result));
            }
        }
    }
#endif
    runtime.shutdown();
    return requirements;
#endif
}

void StreamlineRuntime::refreshCompileTimeStatus() {
    status_ = {};
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    status_.sdkConfigured = true;
#else
    status_.unavailableReason = "Streamline SDK is not configured; set STREAMLINE_SDK_DIR when configuring CMake";
#endif

#if defined(RTV_STREAMLINE_RUNTIME_ENABLED)
    status_.runtimeConfigured = true;
    status_.runtimeDirectory = defaultRuntimeDirectory().string();
    status_.runtimeDll = (defaultRuntimeDirectory() / "sl.interposer.dll").string();
#else
    if (status_.sdkConfigured) {
        status_.unavailableReason = "Streamline SDK headers were found, but sl.interposer.dll was not detected in STREAMLINE_SDK_DIR";
    }
#endif

    status_.dlss = makeUnavailableFeatureStatus(StreamlineFeature::Dlss);
    status_.dlssRayReconstruction = makeUnavailableFeatureStatus(StreamlineFeature::DlssRayReconstruction);
    status_.dlssFrameGeneration = makeUnavailableFeatureStatus(StreamlineFeature::DlssFrameGeneration);
    status_.reflex = makeUnavailableFeatureStatus(StreamlineFeature::Reflex);
    status_.nis = makeUnavailableFeatureStatus(StreamlineFeature::Nis);
    status_.nrd = makeUnavailableFeatureStatus(StreamlineFeature::Nrd);
    if (!status_.runtimeConfigured && !status_.unavailableReason.empty()) {
        for (StreamlineFeature feature : {StreamlineFeature::Dlss, StreamlineFeature::DlssRayReconstruction, StreamlineFeature::DlssFrameGeneration, StreamlineFeature::Reflex, StreamlineFeature::Nis, StreamlineFeature::Nrd}) {
            StreamlineFeatureStatus& featureState = mutableFeatureStatus(feature);
            featureState.requestable = false;
            featureState.supported = false;
            featureState.unavailableReason = status_.unavailableReason;
        }
    }
}

bool StreamlineRuntime::initialize(const StreamlineInitDesc& desc) {
    refreshCompileTimeStatus();
    clearFrameState();
    clearRuntimeDispatch();

#if !defined(RTV_STREAMLINE_RUNTIME_ENABLED)
    (void)desc;
    return false;
#else
    const std::filesystem::path runtimeDir = desc.runtimeDirectory.empty()
        ? defaultRuntimeDirectory()
        : desc.runtimeDirectory;
    const std::filesystem::path runtimeDll = runtimeDir / "sl.interposer.dll";
    status_.runtimeDirectory = runtimeDir.string();
    status_.runtimeDll = runtimeDll.string();
    status_.interposerIntegration = desc.allowInterposer;
    status_.manualVulkanIntegration = !desc.allowInterposer;
    status_.resolvedEntryPoints.clear();
    status_.missingEntryPoints.clear();

    if (runtimeDir.empty() || !std::filesystem::exists(runtimeDll)) {
        status_.unavailableReason = "Streamline runtime DLL was not found in the trusted configured runtime directory";
        return false;
    }

#if defined(_WIN32)
    HMODULE module = LoadLibraryExW(runtimeDll.wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        status_.unavailableReason = "Streamline runtime DLL could not be loaded from the trusted configured runtime directory";
        return false;
    }
    auto* dispatch = new StreamlineDispatch{};
    dispatch->slInit = streamlineProcAddress<PFun_slInit>(module, "slInit");
    dispatch->slShutdown = streamlineProcAddress<PFun_slShutdown>(module, "slShutdown");
    dispatch->slIsFeatureSupported = streamlineProcAddress<PFun_slIsFeatureSupported>(module, "slIsFeatureSupported");
    dispatch->slGetFeatureRequirements = streamlineProcAddress<PFun_slGetFeatureRequirements>(module, "slGetFeatureRequirements");
    dispatch->slGetFeatureFunction = streamlineProcAddress<PFun_slGetFeatureFunction>(module, "slGetFeatureFunction");
    dispatch->slGetNewFrameToken = streamlineProcAddress<PFun_slGetNewFrameToken>(module, "slGetNewFrameToken");
    dispatch->slSetTagForFrame = streamlineProcAddress<PFun_slSetTagForFrame>(module, "slSetTagForFrame");
    dispatch->slSetConstants = streamlineProcAddress<PFun_slSetConstants>(module, "slSetConstants");
    dispatch->slEvaluateFeature = streamlineProcAddress<PFun_slEvaluateFeature>(module, "slEvaluateFeature");
    dispatch->slFreeResources = streamlineProcAddress<PFun_slFreeResources>(module, "slFreeResources");
    dispatch->slSetVulkanInfo = streamlineProcAddress<PFun_slSetVulkanInfo>(module, "slSetVulkanInfo");

    for (const char* entryPoint : kRequiredEntryPoints) {
        if (GetProcAddress(module, entryPoint) != nullptr) {
            status_.resolvedEntryPoints.push_back(entryPoint);
        } else {
            status_.missingEntryPoints.push_back(entryPoint);
        }
    }
    if (dispatch->slSetVulkanInfo != nullptr) {
        status_.resolvedEntryPoints.push_back("slSetVulkanInfo");
    } else {
        status_.missingEntryPoints.push_back("slSetVulkanInfo");
    }
    if (!status_.missingEntryPoints.empty()) {
        status_.unavailableReason = "Streamline runtime DLL is missing required entry points";
        delete dispatch;
        FreeLibrary(module);
        return false;
    }
#else
    status_.unavailableReason = "Streamline runtime loading is currently implemented for Windows builds only";
    return false;
#endif

    static std::vector<sl::Feature> featuresToLoad;
    featuresToLoad = streamlineRequestedFeatures();
    static std::wstring pluginPath;
    pluginPath = runtimeDir.wstring();
    const wchar_t* pluginPaths[] = {pluginPath.c_str()};

    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = desc.enableLogging ? sl::LogLevel::eDefault : sl::LogLevel::eOff;
    preferences.pathsToPlugins = pluginPaths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = featuresToLoad.empty() ? nullptr : featuresToLoad.data();
    preferences.numFeaturesToLoad = static_cast<uint32_t>(featuresToLoad.size());
    preferences.applicationId = 0;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "rtvulkan";
    preferences.projectId = "rtvulkan-native-streamline";
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    if (desc.enableOtaUpdates) {
        preferences.flags |= sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins;
    }

    const sl::Result initResult = dispatch->slInit(preferences, sl::kSDKVersion);
    if (initResult != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slInit failed: " + streamlineResultName(initResult);
        delete dispatch;
        FreeLibrary(module);
        return false;
    }

    runtimeModule_ = module;
    dispatch_ = dispatch;
    status_.initialized = true;
    status_.unavailableReason.clear();
    if (vulkanInfo_.physicalDevice != VK_NULL_HANDLE) {
        queryCapabilities();
    }
    return true;
#endif
}

void StreamlineRuntime::shutdown() {
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    if (status_.initialized && dispatch_ != nullptr) {
        auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
        if (dispatch->slShutdown != nullptr) {
            (void)dispatch->slShutdown();
        }
    }
#endif
    clearFrameState();
    vulkanInfo_ = StreamlineVulkanInfo{};
    status_.initialized = false;
    status_.vulkanInfoSet = false;
    clearRuntimeDispatch();
}

bool StreamlineRuntime::setVulkanInfo(const StreamlineVulkanInfo& info) {
    const bool valid = info.instance != VK_NULL_HANDLE &&
        info.physicalDevice != VK_NULL_HANDLE &&
        info.device != VK_NULL_HANDLE &&
        info.graphicsQueue != VK_NULL_HANDLE;
    status_.vulkanInfoSet = valid;
    vulkanInfo_ = valid ? info : StreamlineVulkanInfo{};
    if (!valid) {
        status_.unavailableReason = "Streamline Vulkan info handoff requires instance, physical device, device, and graphics queue";
    }
    if (!valid || !status_.initialized || dispatch_ == nullptr) {
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    sl::VulkanInfo vulkanInfo{};
    vulkanInfo.instance = info.instance;
    vulkanInfo.physicalDevice = info.physicalDevice;
    vulkanInfo.device = info.device;
    vulkanInfo.graphicsQueueFamily = info.graphicsQueueFamily;
    vulkanInfo.graphicsQueueIndex = info.graphicsQueueIndex;
    vulkanInfo.computeQueueFamily = info.computeQueueFamily;
    vulkanInfo.computeQueueIndex = info.computeQueueIndex;
    const sl::Result result = dispatch->slSetVulkanInfo(vulkanInfo);
    status_.vulkanInfoSet = result == sl::Result::eOk;
    if (!status_.vulkanInfoSet) {
        status_.unavailableReason = "Streamline slSetVulkanInfo failed: " + streamlineResultName(result);
        return false;
    }
    queryCapabilities();
    return true;
#else
    return false;
#endif
}

bool StreamlineRuntime::beginFrame(const StreamlineFrameDesc& desc) {
    frame_ = desc;
    frameToken_ = desc.frameIndex;
    nativeFrameToken_ = nullptr;
    frameActive_ = status_.initialized && dispatch_ != nullptr;
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    if (frameActive_) {
        auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
        const uint32_t frameIndex = static_cast<uint32_t>(desc.frameIndex & 0xffffffffu);
        sl::FrameToken* token = nullptr;
        const sl::Result result = dispatch->slGetNewFrameToken(token, &frameIndex);
        if (result != sl::Result::eOk || token == nullptr) {
            status_.unavailableReason = "Streamline slGetNewFrameToken failed: " + streamlineResultName(result);
            frameActive_ = false;
        } else {
            nativeFrameToken_ = token;
        }
    }
#endif
    frameTags_.clear();
    return frameActive_;
}

void StreamlineRuntime::endFrame() {
    clearFrameState();
}

void StreamlineRuntime::queryCapabilities() {
    status_.dlss = makeUnavailableFeatureStatus(StreamlineFeature::Dlss);
    status_.dlssRayReconstruction = makeUnavailableFeatureStatus(StreamlineFeature::DlssRayReconstruction);
    status_.dlssFrameGeneration = makeUnavailableFeatureStatus(StreamlineFeature::DlssFrameGeneration);
    status_.reflex = makeUnavailableFeatureStatus(StreamlineFeature::Reflex);
    status_.nis = makeUnavailableFeatureStatus(StreamlineFeature::Nis);
    status_.nrd = makeUnavailableFeatureStatus(StreamlineFeature::Nrd);
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    if (!status_.initialized || dispatch_ == nullptr) {
        return;
    }
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    for (StreamlineFeature feature : {StreamlineFeature::Dlss, StreamlineFeature::DlssRayReconstruction, StreamlineFeature::DlssFrameGeneration, StreamlineFeature::Reflex, StreamlineFeature::Nis}) {
        StreamlineFeatureStatus& featureState = mutableFeatureStatus(feature);
        featureState.requestable = true;
        sl::AdapterInfo adapter{};
        adapter.vkPhysicalDevice = vulkanInfo_.physicalDevice;
        const sl::Result result = dispatch->slIsFeatureSupported(streamlineFeatureId(feature), adapter);
        featureState.supported = result == sl::Result::eOk;
        featureState.unavailableReason = featureState.supported ? std::string{} : "slIsFeatureSupported returned " + streamlineResultName(result);
        sl::FeatureRequirements requirements{};
        const sl::Result requirementsResult = dispatch->slGetFeatureRequirements(streamlineFeatureId(feature), requirements);
        if (requirementsResult == sl::Result::eOk) {
            featureState.requirements = streamlineFeatureRequirementsText(requirements);
        }
    }
#endif
}

StreamlineFeatureStatus StreamlineRuntime::queryFeatureRequirements(StreamlineFeature feature) const {
    StreamlineFeatureStatus result = featureStatus(feature);
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    if (status_.initialized && dispatch_ != nullptr) {
        auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
        sl::FeatureRequirements requirements{};
        const sl::Result requirementsResult = dispatch->slGetFeatureRequirements(streamlineFeatureId(feature), requirements);
        if (requirementsResult == sl::Result::eOk) {
            result.requestable = true;
            result.requirements = streamlineFeatureRequirementsText(requirements);
        } else {
            result.unavailableReason = "slGetFeatureRequirements returned " + streamlineResultName(requirementsResult);
        }
    }
#endif
    return result;
}

bool StreamlineRuntime::setConstants(const StreamlineConstantsDesc& desc) {
    if (!status_.initialized || !frameActive_ || nativeFrameToken_ == nullptr || dispatch_ == nullptr) {
        return false;
    }
    if (!desc.matricesValid || desc.cameraNear <= 0.0f || desc.cameraFar <= 0.0f || desc.cameraFovRadians <= 0.0f || desc.cameraAspectRatio <= 0.0f) {
        status_.unavailableReason = "Streamline constants require explicit row-major matrices and valid camera near/far/FOV/aspect values";
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    sl::Constants constants = streamlineConstantsFromDesc(desc);
    sl::ViewportHandle viewport(frame_.viewportId);
    const sl::Result result = dispatch->slSetConstants(constants, *static_cast<sl::FrameToken*>(nativeFrameToken_), viewport);
    if (result != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slSetConstants failed: " + streamlineResultName(result);
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool StreamlineRuntime::setDlssOptions(const StreamlineDlssOptionsDesc& desc) {
    if (!status_.initialized || !frameActive_ || dispatch_ == nullptr) {
        return false;
    }
    if (desc.outputExtent.width == 0u || desc.outputExtent.height == 0u) {
        status_.unavailableReason = "Streamline DLSS options require a valid output extent";
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    if (dispatch->slDLSSSetOptions == nullptr) {
        void* function = nullptr;
        const sl::Result functionResult = dispatch->slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", function);
        if (functionResult != sl::Result::eOk || function == nullptr) {
            status_.unavailableReason = "Streamline slDLSSSetOptions function was unavailable: " + streamlineResultName(functionResult);
            return false;
        }
        dispatch->slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(function);
    }

    sl::DLSSOptions options{};
    options.mode = streamlineDlssMode(desc.qualityMode);
    options.outputWidth = desc.outputExtent.width;
    options.outputHeight = desc.outputExtent.height;
    options.sharpness = desc.sharpness;
    options.preExposure = desc.preExposure;
    options.exposureScale = desc.exposureScale;
    options.colorBuffersHDR = desc.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.useAutoExposure = desc.useAutoExposure ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.alphaUpscalingEnabled = desc.alphaUpscaling ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    sl::ViewportHandle viewport(frame_.viewportId);
    const sl::Result result = dispatch->slDLSSSetOptions(viewport, options);
    if (result != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slDLSSSetOptions failed: " + streamlineResultName(result);
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool StreamlineRuntime::setDlssRayReconstructionOptions(const StreamlineDlssRayReconstructionOptionsDesc& desc) {
    if (!status_.initialized || !frameActive_ || dispatch_ == nullptr) {
        return false;
    }
    if (desc.outputExtent.width == 0u || desc.outputExtent.height == 0u) {
        status_.unavailableReason = "Streamline DLSS Ray Reconstruction options require a valid output extent";
        return false;
    }
    if (!desc.matricesValid) {
        status_.unavailableReason = "Streamline DLSS Ray Reconstruction options require valid world/view matrices";
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    if (dispatch->slDLSSDSetOptions == nullptr) {
        void* function = nullptr;
        const sl::Result functionResult = dispatch->slGetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", function);
        if (functionResult != sl::Result::eOk || function == nullptr) {
            status_.unavailableReason = "Streamline slDLSSDSetOptions function was unavailable: " + streamlineResultName(functionResult);
            return false;
        }
        dispatch->slDLSSDSetOptions = reinterpret_cast<PFun_slDLSSDSetOptions*>(function);
    }

    sl::DLSSDOptions options{};
    options.mode = streamlineDlssMode(desc.qualityMode);
    options.outputWidth = desc.outputExtent.width;
    options.outputHeight = desc.outputExtent.height;
    options.sharpness = desc.sharpness;
    options.preExposure = desc.preExposure;
    options.exposureScale = desc.exposureScale;
    options.colorBuffersHDR = desc.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.alphaUpscalingEnabled = desc.alphaUpscaling ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.normalRoughnessMode = desc.normalRoughnessPacked
        ? sl::DLSSDNormalRoughnessMode::ePacked
        : sl::DLSSDNormalRoughnessMode::eUnpacked;
    fillStreamlineMatrix(options.worldToCameraView, desc.worldToCameraView);
    fillStreamlineMatrix(options.cameraViewToWorld, desc.cameraViewToWorld);
    sl::ViewportHandle viewport(frame_.viewportId);
    const sl::Result result = dispatch->slDLSSDSetOptions(viewport, options);
    if (result != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slDLSSDSetOptions failed: " + streamlineResultName(result);
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool StreamlineRuntime::tagResourceForFrame(const StreamlineResourceTagDesc& desc) {
    if (!status_.initialized || !frameActive_ || nativeFrameToken_ == nullptr || dispatch_ == nullptr ||
        desc.image == VK_NULL_HANDLE || desc.memory == VK_NULL_HANDLE || desc.imageView == VK_NULL_HANDLE ||
        desc.commandBuffer == VK_NULL_HANDLE || desc.role.empty()) {
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    const sl::BufferType bufferType = streamlineBufferTypeForRole(desc.role);
    if (bufferType == sl::INVALID_UINT) {
        status_.unavailableReason = "Unknown Streamline resource tag role: " + desc.role;
        return false;
    }
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    sl::Resource resource(
        sl::ResourceType::eTex2d,
        reinterpret_cast<void*>(desc.image),
        reinterpret_cast<void*>(desc.memory),
        reinterpret_cast<void*>(desc.imageView),
        static_cast<uint32_t>(desc.layout));
    resource.width = desc.extent.width;
    resource.height = desc.extent.height;
    resource.nativeFormat = static_cast<uint32_t>(desc.format);
    resource.mipLevels = 1u;
    resource.arrayLayers = 1u;
    resource.flags = 0u;
    resource.usage = 0u;
    sl::Extent extent{};
    extent.width = desc.extent.width;
    extent.height = desc.extent.height;
    sl::ResourceTag tag(&resource, bufferType, sl::ResourceLifecycle::eValidUntilEvaluate, &extent);
    sl::ViewportHandle viewport(frame_.viewportId);
    auto* nativeCommandBuffer = reinterpret_cast<sl::CommandBuffer*>(desc.commandBuffer);
    const sl::Result result = dispatch->slSetTagForFrame(*static_cast<sl::FrameToken*>(nativeFrameToken_), viewport, &tag, 1, nativeCommandBuffer);
    if (result != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slSetTagForFrame failed for role '" + desc.role + "': " + streamlineResultName(result);
        return false;
    }
    frameTags_.push_back(desc);
    return true;
#else
    return false;
#endif
}

bool StreamlineRuntime::evaluateFeature(StreamlineFeature feature, VkCommandBuffer commandBuffer) {
    if (!status_.initialized || !frameActive_ || nativeFrameToken_ == nullptr || dispatch_ == nullptr || commandBuffer == VK_NULL_HANDLE) {
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    sl::CommandBuffer* nativeCommandBuffer = reinterpret_cast<sl::CommandBuffer*>(commandBuffer);
    const sl::Result result = dispatch->slEvaluateFeature(streamlineFeatureId(feature), *static_cast<sl::FrameToken*>(nativeFrameToken_), nullptr, 0, nativeCommandBuffer);
    if (result != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slEvaluateFeature failed: " + streamlineResultName(result);
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool StreamlineRuntime::setReflexMarker(StreamlineReflexMarker marker) {
    if (!status_.initialized || !frameActive_ || nativeFrameToken_ == nullptr || dispatch_ == nullptr) {
        return false;
    }
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
    if (dispatch->slPCLSetMarker == nullptr) {
        void* function = nullptr;
        const sl::Result functionResult = dispatch->slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", function);
        if (functionResult != sl::Result::eOk || function == nullptr) {
            status_.unavailableReason = "Streamline slPCLSetMarker function was unavailable: " + streamlineResultName(functionResult);
            return false;
        }
        dispatch->slPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(function);
    }
    const sl::PCLMarker pclMarker = streamlinePclMarker(marker);
    if (pclMarker == sl::PCLMarker::eMaximum) {
        status_.unavailableReason = "Unknown Streamline Reflex/PCL marker";
        return false;
    }
    const sl::Result result = dispatch->slPCLSetMarker(pclMarker, *static_cast<sl::FrameToken*>(nativeFrameToken_));
    if (result != sl::Result::eOk) {
        status_.unavailableReason = "Streamline slPCLSetMarker failed: " + streamlineResultName(result);
        return false;
    }
    return true;
#else
    (void)marker;
    return false;
#endif
}

void StreamlineRuntime::freeFeatureResources(StreamlineFeature feature) {
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    if (status_.initialized && dispatch_ != nullptr) {
        auto* dispatch = static_cast<StreamlineDispatch*>(dispatch_);
        sl::ViewportHandle viewport(frame_.viewportId);
        (void)dispatch->slFreeResources(streamlineFeatureId(feature), viewport);
    }
#else
    (void)feature;
#endif
}

void StreamlineRuntime::releaseResourcesForSwapchain() {
    clearFrameState();
}

void StreamlineRuntime::releaseResourcesForRenderer() {
    clearFrameState();
}

void StreamlineRuntime::clearFrameState() {
    frame_ = {};
    frameToken_ = 0;
    nativeFrameToken_ = nullptr;
    frameActive_ = false;
    frameTags_.clear();
}

void StreamlineRuntime::clearRuntimeDispatch() {
#if defined(RTV_STREAMLINE_SDK_CONFIGURED)
    delete static_cast<StreamlineDispatch*>(dispatch_);
#endif
    dispatch_ = nullptr;
#if defined(_WIN32)
    if (runtimeModule_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(runtimeModule_));
    }
#endif
    runtimeModule_ = nullptr;
}

StreamlineFeatureStatus& StreamlineRuntime::mutableFeatureStatus(StreamlineFeature feature) {
    switch (feature) {
    case StreamlineFeature::Dlss: return status_.dlss;
    case StreamlineFeature::DlssRayReconstruction: return status_.dlssRayReconstruction;
    case StreamlineFeature::DlssFrameGeneration: return status_.dlssFrameGeneration;
    case StreamlineFeature::Reflex: return status_.reflex;
    case StreamlineFeature::Nis: return status_.nis;
    case StreamlineFeature::Nrd: return status_.nrd;
    }
    return status_.dlss;
}

const StreamlineFeatureStatus& StreamlineRuntime::featureStatus(StreamlineFeature feature) const {
    switch (feature) {
    case StreamlineFeature::Dlss: return status_.dlss;
    case StreamlineFeature::DlssRayReconstruction: return status_.dlssRayReconstruction;
    case StreamlineFeature::DlssFrameGeneration: return status_.dlssFrameGeneration;
    case StreamlineFeature::Reflex: return status_.reflex;
    case StreamlineFeature::Nis: return status_.nis;
    case StreamlineFeature::Nrd: return status_.nrd;
    }
    return status_.dlss;
}

} // namespace rtv
