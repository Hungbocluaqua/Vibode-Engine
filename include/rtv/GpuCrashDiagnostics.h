#pragma once

#include <filesystem>

namespace rtv {

struct GpuCrashDiagnosticsStatus {
    bool buildAvailable = false;
    bool requested = false;
    bool enabled = false;
    std::filesystem::path outputDirectory;
    std::string unavailableReason;
    uint32_t enableResult = 0;
    uint32_t dumpCount = 0;
    uint32_t shaderDebugInfoCount = 0;
};

class GpuCrashDiagnostics {
public:
    GpuCrashDiagnostics() = default;
    ~GpuCrashDiagnostics();

    GpuCrashDiagnostics(const GpuCrashDiagnostics&) = delete;
    GpuCrashDiagnostics& operator=(const GpuCrashDiagnostics&) = delete;

    bool enable(const std::filesystem::path& outputDirectory);
    [[nodiscard]] bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
};

void recordGpuCrashDiagnosticsRequest(bool requested, const std::filesystem::path& outputDirectory);
[[nodiscard]] GpuCrashDiagnosticsStatus gpuCrashDiagnosticsStatus();

} // namespace rtv
