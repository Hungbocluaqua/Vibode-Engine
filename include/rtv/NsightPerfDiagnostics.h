#pragma once

#include "rtv/NsightPerfMarkers.h"

#include <string>
#include <vector>

namespace rtv {

class VulkanContext;

struct NsightPerfDiagnosticsReport {
    bool sdkConfigured = false;
    bool initialized = false;
    bool vulkanDriverLoaded = false;
    bool nvidiaDevice = false;
    bool gpuSupported = false;
    bool reportGeneratorInitialized = false;
    bool metricsEvaluatorInitialized = false;
    bool rawCounterConfigCreated = false;
    std::string initStatus;
    std::string runtimeDll;
    std::string sdkDirectory;
    std::string deviceName;
    std::string chipName;
    std::string clockStatus;
    std::string unavailableReason;
    std::vector<std::string> warnings;
    NsightPerfMarkerStatus commandBufferRanges;
};

[[nodiscard]] NsightPerfDiagnosticsReport collectNsightPerfDiagnostics(const VulkanContext& context);

} // namespace rtv
