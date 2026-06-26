#include "rtv/NsightPerfMarkers.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>

#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
#include <NvPerfReportGeneratorVulkan.h>
#endif

namespace rtv {
namespace {

struct MarkerState {
    std::atomic_bool enabled{false};
    std::atomic_bool initialized{false};
    std::atomic_bool nvidiaDevice{false};
    std::atomic_bool commandBufferRangesAvailable{false};
    std::atomic_bool reportGeneratorInitialized{false};
    std::atomic_bool captureRequested{false};
    std::atomic_bool collectionActive{false};
    std::atomic_bool rangeEmissionActive{false};
    std::atomic_bool captureCompleted{false};
    std::atomic_bool captureSucceeded{false};
    std::atomic_bool captureFailed{false};
    std::atomic_bool framePrepared{false};
    std::atomic_bool frameStarted{false};
    std::atomic_uint64_t framesObserved{0};
    std::atomic_uint64_t pushedRanges{0};
    std::atomic_uint64_t poppedRanges{0};
    std::atomic_uint64_t failedPushes{0};
    std::atomic_uint64_t failedPops{0};
    std::atomic_uint64_t captureStartFailures{0};
    std::atomic_uint64_t frameStartFailures{0};
    std::atomic_uint64_t frameEndFailures{0};
    std::mutex sdkMutex;
    std::vector<std::string> recentRangeNames;
    NsightPerfReportOptions reportOptions;
    std::string lastReportDirectory;
    std::string unavailableReason;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    nv::perf::profiler::ReportGeneratorVulkan reportGenerator;
#endif
};

MarkerState& markerState() {
    static MarkerState state;
    return state;
}

void rememberRangeName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    MarkerState& state = markerState();
    std::lock_guard<std::mutex> lock(state.sdkMutex);
    if (std::find(state.recentRangeNames.begin(), state.recentRangeNames.end(), name) != state.recentRangeNames.end()) {
        return;
    }
    if (state.recentRangeNames.size() >= 32u) {
        state.recentRangeNames.erase(state.recentRangeNames.begin());
    }
    state.recentRangeNames.emplace_back(name);
}

#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
void applyReportOptions(MarkerState& state) {
    state.reportGenerator.SetFrameLevelRangeName("Frame");
    state.reportGenerator.SetNumNestingLevels(std::max<uint16_t>(1u, state.reportOptions.nestingLevels));
    state.reportGenerator.SetMaxNumRanges(4096u);
    state.reportGenerator.outputOptions.directoryName = state.reportOptions.outputDirectory;
    state.reportGenerator.outputOptions.appendDateTimeToDirName = nv::perf::AppendDateTime::no;
    state.reportGenerator.outputOptions.enableHtmlReport = state.reportOptions.enableHtmlReport;
    state.reportGenerator.outputOptions.enableCsvReport = state.reportOptions.enableCsvReport;
    state.reportGenerator.outputOptions.writeCounterConfigImage = state.reportOptions.writeCounterImages;
    state.reportGenerator.outputOptions.writeCounterDataImage = state.reportOptions.writeCounterImages;
}

bool reinitializeReportGenerator(MarkerState& state) {
#if defined(VK_NO_PROTOTYPES)
    const bool initialized = state.reportGenerator.InitializeReportGenerator(
        state.instance,
        state.physicalDevice,
        state.device,
        state.getInstanceProcAddr,
        state.getDeviceProcAddr);
#else
    const bool initialized = state.reportGenerator.InitializeReportGenerator(
        state.instance,
        state.physicalDevice,
        state.device);
#endif
    state.reportGeneratorInitialized.store(initialized, std::memory_order_relaxed);
    state.commandBufferRangesAvailable.store(initialized, std::memory_order_relaxed);
    if (initialized) {
        applyReportOptions(state);
    }
    return initialized;
}
#endif

void clearCaptureState(MarkerState& state) {
    state.captureRequested.store(false, std::memory_order_relaxed);
    state.collectionActive.store(false, std::memory_order_relaxed);
    state.rangeEmissionActive.store(false, std::memory_order_relaxed);
    state.captureCompleted.store(false, std::memory_order_relaxed);
    state.captureSucceeded.store(false, std::memory_order_relaxed);
    state.captureFailed.store(false, std::memory_order_relaxed);
    state.framePrepared.store(false, std::memory_order_relaxed);
    state.frameStarted.store(false, std::memory_order_relaxed);
    state.framesObserved.store(0, std::memory_order_relaxed);
    state.captureStartFailures.store(0, std::memory_order_relaxed);
    state.frameStartFailures.store(0, std::memory_order_relaxed);
    state.frameEndFailures.store(0, std::memory_order_relaxed);
    state.lastReportDirectory.clear();
}

} // namespace

void configureNsightPerfReport(const NsightPerfReportOptions& options) {
    MarkerState& state = markerState();
    std::lock_guard<std::mutex> lock(state.sdkMutex);
    state.reportOptions = options;
    if (state.reportOptions.outputDirectory.empty()) {
        state.reportOptions.outputDirectory = "out/nvperf";
    }
    state.reportOptions.nestingLevels = std::max<uint16_t>(1u, state.reportOptions.nestingLevels);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (state.reportGeneratorInitialized.load(std::memory_order_relaxed)) {
        applyReportOptions(state);
    }
#endif
}

void initializeNsightPerfMarkers(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    PFN_vkGetInstanceProcAddr getInstanceProcAddr,
    PFN_vkGetDeviceProcAddr getDeviceProcAddr) {
    MarkerState& state = markerState();
    std::lock_guard<std::mutex> lock(state.sdkMutex);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (state.reportGeneratorInitialized.load(std::memory_order_relaxed)) {
        state.reportGenerator.Reset();
    }
#endif
    clearCaptureState(state);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        getInstanceProcAddr == nullptr || getDeviceProcAddr == nullptr) {
        state.unavailableReason = "Vulkan instance, physical device, device, or Vulkan proc address is unavailable";
        state.initialized.store(false, std::memory_order_relaxed);
        state.nvidiaDevice.store(false, std::memory_order_relaxed);
        state.commandBufferRangesAvailable.store(false, std::memory_order_relaxed);
        state.reportGeneratorInitialized.store(false, std::memory_order_relaxed);
        return;
    }
    state.getInstanceProcAddr = getInstanceProcAddr;
    state.getDeviceProcAddr = getDeviceProcAddr;
    state.instance = instance;
    state.physicalDevice = physicalDevice;
    state.device = device;

    const bool isNvidia = nv::perf::VulkanIsNvidiaDevice(
        physicalDevice,
        instance,
        getInstanceProcAddr);
    state.nvidiaDevice.store(isNvidia, std::memory_order_relaxed);
    state.initialized.store(true, std::memory_order_relaxed);
    if (!isNvidia) {
        state.commandBufferRangesAvailable.store(false, std::memory_order_relaxed);
        state.reportGeneratorInitialized.store(false, std::memory_order_relaxed);
        state.unavailableReason = "The selected Vulkan physical device is not an NVIDIA GPU";
        return;
    }

    const bool reportInitialized = reinitializeReportGenerator(state);
    if (reportInitialized) {
        state.unavailableReason.clear();
    } else {
        state.unavailableReason = "Nsight Perf SDK ReportGeneratorVulkan initialization failed";
    }
#else
    (void)instance;
    (void)physicalDevice;
    (void)device;
    (void)getInstanceProcAddr;
    (void)getDeviceProcAddr;
    state.unavailableReason = "Nsight Perf SDK was not configured at build time";
    state.initialized.store(false, std::memory_order_relaxed);
    state.nvidiaDevice.store(false, std::memory_order_relaxed);
    state.commandBufferRangesAvailable.store(false, std::memory_order_relaxed);
    state.reportGeneratorInitialized.store(false, std::memory_order_relaxed);
#endif
}

void shutdownNsightPerfMarkers() {
    MarkerState& state = markerState();
    std::lock_guard<std::mutex> lock(state.sdkMutex);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (state.reportGeneratorInitialized.load(std::memory_order_relaxed)) {
        state.reportGenerator.Reset();
    }
#endif
    state.enabled.store(false, std::memory_order_relaxed);
    state.initialized.store(false, std::memory_order_relaxed);
    state.reportGeneratorInitialized.store(false, std::memory_order_relaxed);
    state.commandBufferRangesAvailable.store(false, std::memory_order_relaxed);
    state.rangeEmissionActive.store(false, std::memory_order_relaxed);
}

void setNsightPerfMarkersEnabled(bool enabled) {
    markerState().enabled.store(enabled, std::memory_order_relaxed);
}

bool requestNsightPerfReport(const NsightPerfReportOptions& options) {
    MarkerState& state = markerState();
    std::lock_guard<std::mutex> lock(state.sdkMutex);
    state.reportOptions = options;
    if (state.reportOptions.outputDirectory.empty()) {
        state.reportOptions.outputDirectory = "out/nvperf";
    }
    state.reportOptions.nestingLevels = std::max<uint16_t>(1u, state.reportOptions.nestingLevels);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (!state.reportGeneratorInitialized.load(std::memory_order_relaxed)) {
        state.unavailableReason = "Nsight Perf SDK report generator is not initialized";
        return false;
    }
    state.reportGenerator.Reset();
    clearCaptureState(state);
    if (!reinitializeReportGenerator(state)) {
        state.unavailableReason = "Nsight Perf SDK report generator restart failed";
        return false;
    }
    state.enabled.store(true, std::memory_order_relaxed);
    state.unavailableReason.clear();
    return true;
#else
    state.unavailableReason = "Nsight Perf SDK was not configured at build time";
    return false;
#endif
}

void cancelNsightPerfReport() {
    MarkerState& state = markerState();
    std::lock_guard<std::mutex> lock(state.sdkMutex);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (state.reportGeneratorInitialized.load(std::memory_order_relaxed)) {
        state.reportGenerator.Reset();
        if (!reinitializeReportGenerator(state)) {
            state.unavailableReason = "Nsight Perf SDK report generator restart failed";
        }
    }
#endif
    clearCaptureState(state);
}

NsightPerfMarkerStatus nsightPerfMarkerStatus() {
    MarkerState& state = markerState();
    NsightPerfMarkerStatus status{};
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    status.sdkConfigured = true;
#endif
    status.initialized = state.initialized.load(std::memory_order_relaxed);
    status.nvidiaDevice = state.nvidiaDevice.load(std::memory_order_relaxed);
    status.enabled = state.enabled.load(std::memory_order_relaxed);
    status.commandBufferRangesAvailable = state.commandBufferRangesAvailable.load(std::memory_order_relaxed);
    status.reportGeneratorInitialized = state.reportGeneratorInitialized.load(std::memory_order_relaxed);
    status.captureRequested = state.captureRequested.load(std::memory_order_relaxed);
    status.collectionActive = state.collectionActive.load(std::memory_order_relaxed);
    status.captureCompleted = state.captureCompleted.load(std::memory_order_relaxed);
    status.captureSucceeded = state.captureSucceeded.load(std::memory_order_relaxed);
    status.captureFailed = state.captureFailed.load(std::memory_order_relaxed);
    status.pushedRanges = state.pushedRanges.load(std::memory_order_relaxed);
    status.poppedRanges = state.poppedRanges.load(std::memory_order_relaxed);
    status.failedPushes = state.failedPushes.load(std::memory_order_relaxed);
    status.failedPops = state.failedPops.load(std::memory_order_relaxed);
    status.captureStartFailures = state.captureStartFailures.load(std::memory_order_relaxed);
    status.frameStartFailures = state.frameStartFailures.load(std::memory_order_relaxed);
    status.frameEndFailures = state.frameEndFailures.load(std::memory_order_relaxed);
    status.framesObserved = state.framesObserved.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state.sdkMutex);
        status.startAfterFrames = state.reportOptions.startAfterFrames;
        status.nestingLevels = state.reportOptions.nestingLevels;
        status.htmlReportEnabled = state.reportOptions.enableHtmlReport;
        status.csvReportEnabled = state.reportOptions.enableCsvReport;
        status.counterImagesEnabled = state.reportOptions.writeCounterImages;
        status.configuredOutputDirectory = state.reportOptions.outputDirectory;
        status.lastReportDirectory = state.lastReportDirectory;
        status.unavailableReason = state.unavailableReason;
        status.recentRangeNames = state.recentRangeNames;
    }
    return status;
}

bool nsightPerfReportNeedsMoreFrames() {
    const MarkerState& state = markerState();
    return state.enabled.load(std::memory_order_relaxed) &&
        state.reportGeneratorInitialized.load(std::memory_order_relaxed) &&
        !state.captureCompleted.load(std::memory_order_relaxed);
}

void resetNsightPerfMarkerFrameCounters() {
    MarkerState& state = markerState();
    state.pushedRanges.store(0, std::memory_order_relaxed);
    state.poppedRanges.store(0, std::memory_order_relaxed);
    state.failedPushes.store(0, std::memory_order_relaxed);
    state.failedPops.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(state.sdkMutex);
    state.recentRangeNames.clear();
}

void prepareNsightPerfFrame() {
    MarkerState& state = markerState();
    if (!state.enabled.load(std::memory_order_relaxed) ||
        !state.reportGeneratorInitialized.load(std::memory_order_relaxed) ||
        state.captureCompleted.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(state.sdkMutex);
    if (state.framePrepared.load(std::memory_order_relaxed)) {
        return;
    }
    state.framePrepared.store(true, std::memory_order_relaxed);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    const uint64_t nextFrame = state.framesObserved.load(std::memory_order_relaxed) + 1u;
    if (!state.captureRequested.load(std::memory_order_relaxed) &&
        nextFrame > state.reportOptions.startAfterFrames) {
        const std::filesystem::path outputDirectory(state.reportOptions.outputDirectory);
        std::error_code error;
        if (!outputDirectory.parent_path().empty()) {
            std::filesystem::create_directories(outputDirectory.parent_path(), error);
        }
        applyReportOptions(state);
        if (state.reportGenerator.StartCollectionOnNextFrame()) {
            state.captureRequested.store(true, std::memory_order_relaxed);
        } else {
            state.captureStartFailures.fetch_add(1, std::memory_order_relaxed);
            state.captureFailed.store(true, std::memory_order_relaxed);
            state.captureCompleted.store(true, std::memory_order_relaxed);
            state.unavailableReason = "Nsight Perf SDK failed to queue report collection";
        }
    }
    const bool emitRanges = state.captureRequested.load(std::memory_order_relaxed) ||
        state.reportGenerator.IsCollectingReport();
    state.rangeEmissionActive.store(emitRanges, std::memory_order_relaxed);
#endif
}

void beginNsightPerfFrame(VkQueue queue, uint32_t queueFamilyIndex) {
    MarkerState& state = markerState();
    if (!state.framePrepared.load(std::memory_order_relaxed) ||
        !state.captureRequested.load(std::memory_order_relaxed) ||
        state.captureCompleted.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.sdkMutex);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
#if defined(VK_NO_PROTOTYPES)
    const bool started = state.reportGenerator.OnFrameStart(
        queue,
        queueFamilyIndex,
        state.getInstanceProcAddr,
        state.getDeviceProcAddr);
#else
    const bool started = state.reportGenerator.OnFrameStart(queue, queueFamilyIndex);
#endif
    state.frameStarted.store(started, std::memory_order_relaxed);
    state.collectionActive.store(state.reportGenerator.IsCollectingReport(), std::memory_order_relaxed);
    if (!started) {
        state.frameStartFailures.fetch_add(1, std::memory_order_relaxed);
        state.captureFailed.store(true, std::memory_order_relaxed);
        state.captureCompleted.store(true, std::memory_order_relaxed);
        state.rangeEmissionActive.store(false, std::memory_order_relaxed);
        state.unavailableReason = "Nsight Perf SDK OnFrameStart failed";
    }
#else
    (void)queue;
    (void)queueFamilyIndex;
#endif
}

void endNsightPerfFrame() {
    MarkerState& state = markerState();
    if (!state.framePrepared.exchange(false, std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(state.sdkMutex);
    state.framesObserved.fetch_add(1, std::memory_order_relaxed);
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    if (!state.frameStarted.exchange(false, std::memory_order_relaxed)) {
        return;
    }
    const bool ended = state.reportGenerator.OnFrameEnd();
    const bool stillCollecting = state.reportGenerator.IsCollectingReport();
    state.collectionActive.store(stillCollecting, std::memory_order_relaxed);
    state.rangeEmissionActive.store(stillCollecting, std::memory_order_relaxed);
    if (!ended) {
        state.frameEndFailures.fetch_add(1, std::memory_order_relaxed);
        state.captureFailed.store(true, std::memory_order_relaxed);
        state.captureCompleted.store(true, std::memory_order_relaxed);
        state.rangeEmissionActive.store(false, std::memory_order_relaxed);
        state.unavailableReason = "Nsight Perf SDK OnFrameEnd failed";
        return;
    }
    if (!stillCollecting) {
        state.lastReportDirectory = state.reportGenerator.GetLastReportDirectoryName();
        state.captureCompleted.store(true, std::memory_order_relaxed);
        state.captureSucceeded.store(!state.lastReportDirectory.empty(), std::memory_order_relaxed);
        state.captureFailed.store(state.lastReportDirectory.empty(), std::memory_order_relaxed);
        state.rangeEmissionActive.store(false, std::memory_order_relaxed);
        if (state.lastReportDirectory.empty()) {
            state.unavailableReason = "Nsight Perf SDK collection ended without a report directory";
        }
    }
#endif
}

bool beginNsightPerfCommandBufferRange(VkCommandBuffer commandBuffer, const char* name) {
    MarkerState& state = markerState();
    if (!state.enabled.load(std::memory_order_relaxed) ||
        !state.commandBufferRangesAvailable.load(std::memory_order_relaxed) ||
        !state.rangeEmissionActive.load(std::memory_order_relaxed) ||
        commandBuffer == VK_NULL_HANDLE ||
        name == nullptr ||
        name[0] == '\0') {
        return false;
    }
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    bool pushed = false;
    {
        std::lock_guard<std::mutex> lock(state.sdkMutex);
        pushed = state.reportGenerator.rangeCommands.PushRange(commandBuffer, name);
    }
    if (pushed) {
        state.pushedRanges.fetch_add(1, std::memory_order_relaxed);
        rememberRangeName(name);
    } else {
        state.failedPushes.fetch_add(1, std::memory_order_relaxed);
    }
    return pushed;
#else
    (void)commandBuffer;
    (void)name;
    return false;
#endif
}

bool endNsightPerfCommandBufferRange(VkCommandBuffer commandBuffer) {
    MarkerState& state = markerState();
    if (!state.enabled.load(std::memory_order_relaxed) ||
        !state.commandBufferRangesAvailable.load(std::memory_order_relaxed) ||
        !state.rangeEmissionActive.load(std::memory_order_relaxed) ||
        commandBuffer == VK_NULL_HANDLE) {
        return false;
    }
#if defined(RTV_NSIGHT_PERF_SDK_CONFIGURED)
    bool popped = false;
    {
        std::lock_guard<std::mutex> lock(state.sdkMutex);
        popped = state.reportGenerator.rangeCommands.PopRange(commandBuffer);
    }
    if (popped) {
        state.poppedRanges.fetch_add(1, std::memory_order_relaxed);
    } else {
        state.failedPops.fetch_add(1, std::memory_order_relaxed);
    }
    return popped;
#else
    (void)commandBuffer;
    return false;
#endif
}

ScopedNsightPerfCommandBufferRange::ScopedNsightPerfCommandBufferRange(VkCommandBuffer commandBuffer, const char* name)
    : commandBuffer_(commandBuffer) {
    active_ = beginNsightPerfCommandBufferRange(commandBuffer_, name);
}

ScopedNsightPerfCommandBufferRange::~ScopedNsightPerfCommandBufferRange() {
    if (active_) {
        (void)endNsightPerfCommandBufferRange(commandBuffer_);
    }
}

} // namespace rtv
