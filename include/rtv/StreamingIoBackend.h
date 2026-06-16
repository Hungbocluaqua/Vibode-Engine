#pragma once

#include "rtv/StreamingRuntime.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rtv {

struct StreamingIoReadRequest {
    std::filesystem::path path;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string label;
};

struct StreamingIoReadResult {
    bool ok = false;
    StreamingIoBackendKind backend = StreamingIoBackendKind::Win32;
    std::filesystem::path path;
    uint64_t offset = 0;
    uint64_t requestedBytes = 0;
    uint64_t completedBytes = 0;
    std::vector<std::byte> bytes;
    std::string fallbackReason;
    std::string error;
};

struct StreamingIoMetrics {
    StreamingIoBackendKind requestedBackend = StreamingIoBackendKind::Win32;
    StreamingIoBackendKind effectiveBackend = StreamingIoBackendKind::Win32;
    bool requestedBackendAvailable = true;
    std::string requestedBackendUnavailableReason;
    uint32_t queueCount = 1;
    uint32_t requestCount = 0;
    uint32_t batchCount = 0;
    uint32_t batchedRequestCount = 0;
    uint32_t maxBatchRequestCount = 0;
    uint32_t failedRequestCount = 0;
    uint32_t fallbackRequestCount = 0;
    uint32_t cpuDecompressionFallbackCount = 0;
    uint32_t gpuDecompressionCount = 0;
    uint64_t bytesSubmitted = 0;
    uint64_t bytesCompleted = 0;
    double totalLatencyMs = 0.0;
    double maxLatencyMs = 0.0;
    std::vector<uint32_t> latencyBuckets;
    std::string decompressionFormat = "none";
};

class StreamingIoBackend {
public:
    virtual ~StreamingIoBackend() = default;

    [[nodiscard]] virtual StreamingIoBackendKind kind() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual std::string unavailableReason() const = 0;
    [[nodiscard]] virtual StreamingIoReadResult read(const StreamingIoReadRequest& request) = 0;
    [[nodiscard]] virtual std::vector<StreamingIoReadResult> readBatch(const std::vector<StreamingIoReadRequest>& requests);
    [[nodiscard]] virtual StreamingIoMetrics metrics() const = 0;
};

[[nodiscard]] std::unique_ptr<StreamingIoBackend> makeStreamingIoBackend(const StreamingRuntimeOptions& options);
[[nodiscard]] nlohmann::json streamingIoBackendAvailabilityJson(const StreamingRuntimeOptions& options);
[[nodiscard]] nlohmann::json streamingIoMetricsJson(const StreamingIoMetrics& metrics);
[[nodiscard]] int simulateStreamingIoBackendCommand(
    const std::filesystem::path& input,
    const StreamingRuntimeOptions& options,
    const std::filesystem::path& jsonOut = {});
[[nodiscard]] int simulateStreamingIoBatchCommand(
    const std::filesystem::path& input,
    const StreamingRuntimeOptions& options,
    uint32_t requestCount,
    uint64_t chunkBytes,
    const std::filesystem::path& jsonOut = {});

} // namespace rtv
