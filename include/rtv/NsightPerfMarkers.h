#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace rtv {

struct NsightPerfReportOptions {
    std::string outputDirectory = "out/nvperf";
    uint32_t startAfterFrames = 0;
    uint16_t nestingLevels = 2;
    bool enableHtmlReport = true;
    bool enableCsvReport = true;
    bool writeCounterImages = false;
};

struct NsightPerfMarkerStatus {
    bool sdkConfigured = false;
    bool initialized = false;
    bool nvidiaDevice = false;
    bool enabled = false;
    bool commandBufferRangesAvailable = false;
    bool reportGeneratorInitialized = false;
    bool captureRequested = false;
    bool collectionActive = false;
    bool captureCompleted = false;
    bool captureSucceeded = false;
    bool captureFailed = false;
    uint32_t startAfterFrames = 0;
    uint16_t nestingLevels = 0;
    bool htmlReportEnabled = false;
    bool csvReportEnabled = false;
    bool counterImagesEnabled = false;
    uint64_t framesObserved = 0;
    uint64_t pushedRanges = 0;
    uint64_t poppedRanges = 0;
    uint64_t failedPushes = 0;
    uint64_t failedPops = 0;
    uint64_t captureStartFailures = 0;
    uint64_t frameStartFailures = 0;
    uint64_t frameEndFailures = 0;
    std::vector<std::string> recentRangeNames;
    std::string configuredOutputDirectory;
    std::string lastReportDirectory;
    std::string unavailableReason;
};

void configureNsightPerfReport(const NsightPerfReportOptions& options);
void initializeNsightPerfMarkers(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    PFN_vkGetInstanceProcAddr getInstanceProcAddr,
    PFN_vkGetDeviceProcAddr getDeviceProcAddr);
void shutdownNsightPerfMarkers();
void setNsightPerfMarkersEnabled(bool enabled);
[[nodiscard]] bool requestNsightPerfReport(const NsightPerfReportOptions& options);
void cancelNsightPerfReport();
[[nodiscard]] NsightPerfMarkerStatus nsightPerfMarkerStatus();
[[nodiscard]] bool nsightPerfReportNeedsMoreFrames();
void resetNsightPerfMarkerFrameCounters();
void prepareNsightPerfFrame();
void beginNsightPerfFrame(VkQueue queue, uint32_t queueFamilyIndex);
void endNsightPerfFrame();
[[nodiscard]] bool beginNsightPerfCommandBufferRange(VkCommandBuffer commandBuffer, const char* name);
[[nodiscard]] bool endNsightPerfCommandBufferRange(VkCommandBuffer commandBuffer);

class ScopedNsightPerfCommandBufferRange {
public:
    ScopedNsightPerfCommandBufferRange(VkCommandBuffer commandBuffer, const char* name);
    ~ScopedNsightPerfCommandBufferRange();

    ScopedNsightPerfCommandBufferRange(const ScopedNsightPerfCommandBufferRange&) = delete;
    ScopedNsightPerfCommandBufferRange& operator=(const ScopedNsightPerfCommandBufferRange&) = delete;

private:
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    bool active_ = false;
};

} // namespace rtv
