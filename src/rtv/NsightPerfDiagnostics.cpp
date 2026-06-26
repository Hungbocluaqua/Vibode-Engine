#include "rtv/NsightPerfDiagnostics.h"

#include "rtv/VulkanContext.h"

#include <filesystem>
#include <sstream>
#include <vector>

#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
#include <NvPerfVulkan.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rtv {
namespace {

std::string executableDirectory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0 || size >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path().string();
#else
    return {};
#endif
}

std::string runtimeDllPath() {
    const std::filesystem::path exeDir = executableDirectory();
    if (!exeDir.empty()) {
        const std::filesystem::path deployed = exeDir / "nvperf_grfx_host.dll";
        if (std::filesystem::exists(deployed)) {
            return deployed.string();
        }
    }
#if defined(RTV_NSIGHT_PERF_RUNTIME_DLL)
    return RTV_NSIGHT_PERF_RUNTIME_DLL;
#else
    return {};
#endif
}

} // namespace

NsightPerfDiagnosticsReport collectNsightPerfDiagnostics(const VulkanContext& context) {
    NsightPerfDiagnosticsReport report{};
    report.commandBufferRanges = nsightPerfMarkerStatus();
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    report.sdkConfigured = true;
    report.sdkDirectory = RTV_NSIGHT_PERF_SDK_DIR;
    report.runtimeDll = runtimeDllPath();
    if (report.runtimeDll.empty() || !std::filesystem::exists(report.runtimeDll)) {
        report.warnings.push_back("nvperf_grfx_host.dll was not found beside the executable or in the configured SDK runtime path");
    }

    const VkInstance instance = context.instance();
    const VkPhysicalDevice physicalDevice = context.physicalDevice();
    const VkDevice device = context.device();

    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        report.unavailableReason = "Vulkan instance, physical device, or device is not initialized";
        return report;
    }

    report.deviceName = nv::perf::VulkanGetDeviceName(
        physicalDevice,
        instance,
        vkGetInstanceProcAddr);
    report.nvidiaDevice = nv::perf::VulkanIsNvidiaDevice(
        physicalDevice,
        instance,
        vkGetInstanceProcAddr);
    if (!report.nvidiaDevice) {
        report.unavailableReason = "The selected Vulkan physical device is not an NVIDIA GPU";
        return report;
    }

    report.initialized = nv::perf::InitializeNvPerf();
    if (!report.initialized) {
        report.unavailableReason = "InitializeNvPerf failed";
        return report;
    }

    report.vulkanDriverLoaded = nv::perf::VulkanLoadDriver(instance);
    if (!report.vulkanDriverLoaded) {
        report.unavailableReason = "NVPW_VK_LoadDriver failed";
        return report;
    }

    const size_t deviceIndex = nv::perf::VulkanGetNvperfDeviceIndex(
        instance,
        physicalDevice,
        device,
        vkGetInstanceProcAddr,
        vkGetDeviceProcAddr);
    if (deviceIndex == ~size_t(0)) {
        report.unavailableReason = "NVPW_VK_Device_GetDeviceIndex failed";
        return report;
    }

    const nv::perf::DeviceIdentifiers identifiers = nv::perf::GetDeviceIdentifiers(deviceIndex);
    if (identifiers.pDeviceName && report.deviceName.empty()) {
        report.deviceName = identifiers.pDeviceName;
    }
    if (identifiers.pChipName) {
        report.chipName = identifiers.pChipName;
    }

    report.gpuSupported = nv::perf::profiler::VulkanIsGpuSupported(
        instance,
        physicalDevice,
        device,
        vkGetInstanceProcAddr,
        vkGetDeviceProcAddr);
    if (!report.gpuSupported) {
        report.unavailableReason = report.chipName.empty()
            ? "Nsight Perf SDK reports this Vulkan GPU is not supported"
            : "Nsight Perf SDK reports chip " + report.chipName + " is not supported";
        return report;
    }

    const nv::perf::ClockInfo clockInfo = nv::perf::VulkanGetDeviceClockState(
        instance,
        physicalDevice,
        device,
        vkGetInstanceProcAddr,
        vkGetDeviceProcAddr);
    report.clockStatus = nv::perf::ToCString(clockInfo);

    const size_t scratchBufferSize = nv::perf::VulkanCalculateMetricsEvaluatorScratchBufferSize(report.chipName.c_str());
    if (scratchBufferSize == 0u) {
        report.unavailableReason = "Nsight Perf SDK could not calculate a Vulkan metrics evaluator scratch buffer";
        return report;
    }
    std::vector<uint8_t> scratchBuffer(scratchBufferSize);
    NVPW_MetricsEvaluator* metricsEvaluator = nv::perf::VulkanCreateMetricsEvaluator(
        scratchBuffer.data(),
        scratchBuffer.size(),
        report.chipName.c_str());
    report.metricsEvaluatorInitialized = metricsEvaluator != nullptr;
    if (metricsEvaluator) {
        NVPW_MetricsEvaluator_Destroy_Params destroyParams = {NVPW_MetricsEvaluator_Destroy_Params_STRUCT_SIZE};
        destroyParams.pMetricsEvaluator = metricsEvaluator;
        NVPW_MetricsEvaluator_Destroy(&destroyParams);
    } else {
        report.unavailableReason = "Nsight Perf SDK could not initialize a Vulkan metrics evaluator";
        return report;
    }

    NVPW_RawCounterConfig* rawCounterConfig = nv::perf::profiler::VulkanCreateRawCounterConfig(report.chipName.c_str());
    report.rawCounterConfigCreated = rawCounterConfig != nullptr;
    if (rawCounterConfig) {
        NVPW_RawCounterConfig_Destroy_Params destroyParams = {NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE};
        destroyParams.pRawCounterConfig = rawCounterConfig;
        NVPW_RawCounterConfig_Destroy(&destroyParams);
    } else {
        report.unavailableReason = "Nsight Perf SDK could not create a Vulkan raw counter config";
        return report;
    }

    report.reportGeneratorInitialized = report.commandBufferRanges.reportGeneratorInitialized;
    if (!report.reportGeneratorInitialized) {
        report.unavailableReason = report.commandBufferRanges.unavailableReason.empty()
            ? "Nsight Perf SDK ReportGeneratorVulkan is not initialized"
            : report.commandBufferRanges.unavailableReason;
        return report;
    }
    report.initStatus = "native Vulkan Perf SDK report generator ready";
#else
    (void)context;
    report.unavailableReason = "Nsight Perf SDK was not configured at build time";
#endif
    return report;
}

} // namespace rtv
