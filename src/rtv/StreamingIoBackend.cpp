#include "rtv/StreamingIoBackend.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>

#if defined(RTV_WITH_DIRECTSTORAGE) && defined(_WIN32)
#include <wrl/client.h>
#include <dstorage.h>
#endif

namespace rtv {
namespace {

constexpr double kLatencyBucketLimitsMs[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};

void ensureLatencyBuckets(StreamingIoMetrics& metrics) {
    if (metrics.latencyBuckets.empty()) {
        metrics.latencyBuckets.resize(std::size(kLatencyBucketLimitsMs) + 1u, 0u);
    }
}

void recordLatency(StreamingIoMetrics& metrics, double latencyMs) {
    ensureLatencyBuckets(metrics);
    metrics.totalLatencyMs += latencyMs;
    metrics.maxLatencyMs = std::max(metrics.maxLatencyMs, latencyMs);
    size_t bucket = 0;
    while (bucket < std::size(kLatencyBucketLimitsMs) && latencyMs > kLatencyBucketLimitsMs[bucket]) {
        ++bucket;
    }
    ++metrics.latencyBuckets[bucket];
}

void recordReadMetrics(
    StreamingIoMetrics& metrics,
    const StreamingIoReadRequest& request,
    const StreamingIoReadResult& result,
    double latencyMs) {
    ensureLatencyBuckets(metrics);
    ++metrics.requestCount;
    metrics.bytesSubmitted += request.size;
    metrics.bytesCompleted += result.completedBytes;
    if (!result.ok) {
        ++metrics.failedRequestCount;
    }
    if (!result.fallbackReason.empty()) {
        ++metrics.fallbackRequestCount;
    }
    metrics.effectiveBackend = result.backend;
    recordLatency(metrics, latencyMs);
}

void recordBatchMetrics(StreamingIoMetrics& metrics, size_t requestCount) {
    ++metrics.batchCount;
    metrics.batchedRequestCount += static_cast<uint32_t>(std::min<size_t>(requestCount, std::numeric_limits<uint32_t>::max()));
    metrics.maxBatchRequestCount = std::max(metrics.maxBatchRequestCount, static_cast<uint32_t>(std::min<size_t>(requestCount, std::numeric_limits<uint32_t>::max())));
}

nlohmann::json makeStreamingIoMetricsJson(const StreamingIoMetrics& metrics) {
    const double averageRequestBytes = metrics.requestCount == 0
        ? 0.0
        : static_cast<double>(metrics.bytesSubmitted) / static_cast<double>(metrics.requestCount);
    const double averageLatencyMs = metrics.requestCount == 0
        ? 0.0
        : metrics.totalLatencyMs / static_cast<double>(metrics.requestCount);
    return {
        {"requested_backend", streamingIoBackendKindName(metrics.requestedBackend)},
        {"effective_backend", streamingIoBackendKindName(metrics.effectiveBackend)},
        {"requested_backend_available", metrics.requestedBackendAvailable},
        {"requested_backend_unavailable_reason", metrics.requestedBackendUnavailableReason},
        {"queue_count", metrics.queueCount},
        {"request_count", metrics.requestCount},
        {"batch_count", metrics.batchCount},
        {"batched_request_count", metrics.batchedRequestCount},
        {"max_batch_request_count", metrics.maxBatchRequestCount},
        {"failed_request_count", metrics.failedRequestCount},
        {"fallback_request_count", metrics.fallbackRequestCount},
        {"bytes_submitted", metrics.bytesSubmitted},
        {"bytes_completed", metrics.bytesCompleted},
        {"average_request_bytes", averageRequestBytes},
        {"decompression_format", metrics.decompressionFormat},
        {"cpu_decompression_fallback_count", metrics.cpuDecompressionFallbackCount},
        {"gpu_decompression_count", metrics.gpuDecompressionCount},
        {"average_latency_ms", averageLatencyMs},
        {"max_latency_ms", metrics.maxLatencyMs},
        {"latency_bucket_limits_ms", kLatencyBucketLimitsMs},
        {"latency_buckets", metrics.latencyBuckets},
    };
}

class Win32StreamingIoBackend final : public StreamingIoBackend {
public:
    Win32StreamingIoBackend() {
        metrics_.requestedBackend = kind();
        metrics_.effectiveBackend = kind();
        metrics_.requestedBackendAvailable = true;
        ensureLatencyBuckets(metrics_);
    }

    [[nodiscard]] StreamingIoBackendKind kind() const override { return StreamingIoBackendKind::Win32; }
    [[nodiscard]] std::string name() const override { return streamingIoBackendKindName(kind()); }
    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string unavailableReason() const override { return {}; }

    [[nodiscard]] StreamingIoReadResult read(const StreamingIoReadRequest& request) override {
        const auto start = std::chrono::steady_clock::now();
        StreamingIoReadResult result;
        result.backend = kind();
        result.path = request.path;
        result.offset = request.offset;
        result.requestedBytes = request.size;

        std::ifstream file(request.path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            result.error = "could not open input";
            return result;
        }
        const std::streamoff fileSize = file.tellg();
        if (fileSize < 0 || request.offset > static_cast<uint64_t>(fileSize)) {
            result.error = "read offset is outside input";
            return result;
        }
        const uint64_t availableBytes = static_cast<uint64_t>(fileSize) - request.offset;
        const uint64_t readBytes = request.size == 0 ? availableBytes : std::min(request.size, availableBytes);
        result.bytes.resize(static_cast<size_t>(readBytes));
        file.seekg(static_cast<std::streamoff>(request.offset), std::ios::beg);
        if (readBytes > 0) {
            file.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(readBytes));
        }
        if (!file.good() && readBytes > 0) {
            result.error = "could not read requested byte range";
            result.bytes.clear();
            return result;
        }
        result.ok = true;
        result.completedBytes = readBytes;
        const auto end = std::chrono::steady_clock::now();
        recordReadMetrics(metrics_, request, result, std::chrono::duration<double, std::milli>(end - start).count());
        return result;
    }

    [[nodiscard]] std::vector<StreamingIoReadResult> readBatch(const std::vector<StreamingIoReadRequest>& requests) override {
        recordBatchMetrics(metrics_, requests.size());
        std::vector<StreamingIoReadResult> results;
        results.reserve(requests.size());
        for (const StreamingIoReadRequest& request : requests) {
            results.push_back(read(request));
        }
        return results;
    }

    [[nodiscard]] StreamingIoMetrics metrics() const override { return metrics_; }

private:
    StreamingIoMetrics metrics_{};
};

class MockStreamingIoBackend final : public StreamingIoBackend {
public:
    MockStreamingIoBackend() {
        metrics_.requestedBackend = kind();
        metrics_.effectiveBackend = kind();
        metrics_.requestedBackendAvailable = true;
        ensureLatencyBuckets(metrics_);
    }

    [[nodiscard]] StreamingIoBackendKind kind() const override { return StreamingIoBackendKind::Mock; }
    [[nodiscard]] std::string name() const override { return streamingIoBackendKindName(kind()); }
    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string unavailableReason() const override { return {}; }

    [[nodiscard]] StreamingIoReadResult read(const StreamingIoReadRequest& request) override {
        const auto start = std::chrono::steady_clock::now();
        Win32StreamingIoBackend win32;
        StreamingIoReadResult result = win32.read(request);
        result.backend = kind();
        const auto end = std::chrono::steady_clock::now();
        recordReadMetrics(metrics_, request, result, std::chrono::duration<double, std::milli>(end - start).count());
        return result;
    }

    [[nodiscard]] std::vector<StreamingIoReadResult> readBatch(const std::vector<StreamingIoReadRequest>& requests) override {
        recordBatchMetrics(metrics_, requests.size());
        std::vector<StreamingIoReadResult> results;
        results.reserve(requests.size());
        for (const StreamingIoReadRequest& request : requests) {
            results.push_back(read(request));
        }
        return results;
    }

    [[nodiscard]] StreamingIoMetrics metrics() const override { return metrics_; }

private:
    StreamingIoMetrics metrics_{};
};

class DirectStorageStreamingIoBackend final : public StreamingIoBackend {
public:
    DirectStorageStreamingIoBackend() {
        metrics_.requestedBackend = kind();
        metrics_.effectiveBackend = kind();
        metrics_.requestedBackendAvailable = false;
        metrics_.requestedBackendUnavailableReason = unavailableReason();
        ensureLatencyBuckets(metrics_);
#if defined(RTV_WITH_DIRECTSTORAGE) && defined(_WIN32)
        HRESULT hr = DStorageGetFactory(__uuidof(IDStorageFactory), reinterpret_cast<void**>(factory_.GetAddressOf()));
        if (FAILED(hr) || factory_ == nullptr) {
            available_ = false;
            unavailableReason_ = "DStorageGetFactory failed: HRESULT 0x" + hresultHex(hr);
            metrics_.requestedBackendUnavailableReason = unavailableReason_;
            return;
        }

        DSTORAGE_QUEUE_DESC desc{};
        desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        desc.Priority = DSTORAGE_PRIORITY_NORMAL;
        desc.Name = "rtv streaming file-to-memory queue";
        desc.Device = nullptr;
        hr = factory_->CreateQueue(&desc, __uuidof(IDStorageQueue1), reinterpret_cast<void**>(queue_.GetAddressOf()));
        if (FAILED(hr) || queue_ == nullptr) {
            available_ = false;
            unavailableReason_ = "IDStorageFactory::CreateQueue failed: HRESULT 0x" + hresultHex(hr);
            metrics_.requestedBackendUnavailableReason = unavailableReason_;
            return;
        }
        available_ = true;
        unavailableReason_.clear();
        metrics_.requestedBackendAvailable = true;
        metrics_.requestedBackendUnavailableReason.clear();
        metrics_.effectiveBackend = kind();
#endif
    }

    [[nodiscard]] StreamingIoBackendKind kind() const override { return StreamingIoBackendKind::DirectStorage; }
    [[nodiscard]] std::string name() const override { return streamingIoBackendKindName(kind()); }
    [[nodiscard]] bool available() const override { return available_; }
    [[nodiscard]] std::string unavailableReason() const override {
        return unavailableReason_;
    }

    [[nodiscard]] StreamingIoReadResult read(const StreamingIoReadRequest& request) override {
#if defined(RTV_WITH_DIRECTSTORAGE) && defined(_WIN32)
        if (available_) {
            return readDirectStorage(request);
        }
#endif
        const auto start = std::chrono::steady_clock::now();
        Win32StreamingIoBackend fallback;
        StreamingIoReadResult result = fallback.read(request);
        result.backend = fallback.kind();
        result.fallbackReason = unavailableReason();
        const auto end = std::chrono::steady_clock::now();
        recordReadMetrics(metrics_, request, result, std::chrono::duration<double, std::milli>(end - start).count());
        return result;
    }

    [[nodiscard]] std::vector<StreamingIoReadResult> readBatch(const std::vector<StreamingIoReadRequest>& requests) override {
        recordBatchMetrics(metrics_, requests.size());
#if defined(RTV_WITH_DIRECTSTORAGE) && defined(_WIN32)
        if (available_) {
            return readBatchDirectStorage(requests);
        }
#endif
        Win32StreamingIoBackend fallback;
        std::vector<StreamingIoReadResult> results;
        results.reserve(requests.size());
        for (const StreamingIoReadRequest& request : requests) {
            const auto start = std::chrono::steady_clock::now();
            StreamingIoReadResult result = fallback.read(request);
            result.backend = fallback.kind();
            result.fallbackReason = unavailableReason();
            const auto end = std::chrono::steady_clock::now();
            recordReadMetrics(metrics_, request, result, std::chrono::duration<double, std::milli>(end - start).count());
            results.push_back(std::move(result));
        }
        return results;
    }

    [[nodiscard]] StreamingIoMetrics metrics() const override { return metrics_; }

private:
    [[nodiscard]] static std::string hresultHex(long value) {
        constexpr char digits[] = "0123456789ABCDEF";
        uint32_t v = static_cast<uint32_t>(value);
        std::string out(8, '0');
        for (int i = 7; i >= 0; --i) {
            out[static_cast<size_t>(i)] = digits[v & 0xfu];
            v >>= 4u;
        }
        return out;
    }

#if defined(RTV_WITH_DIRECTSTORAGE) && defined(_WIN32)
    bool prepareDirectStorageRead(
        const StreamingIoReadRequest& request,
        StreamingIoReadResult& result,
        Microsoft::WRL::ComPtr<IDStorageFile>& file,
        uint64_t& readBytes) {
        result.backend = kind();
        result.path = request.path;
        result.offset = request.offset;
        result.requestedBytes = request.size;

        std::ifstream sizeProbe(request.path, std::ios::binary | std::ios::ate);
        if (!sizeProbe.is_open()) {
            result.error = "could not open input for size probe";
            return false;
        }
        const std::streamoff fileSize = sizeProbe.tellg();
        if (fileSize < 0 || request.offset > static_cast<uint64_t>(fileSize)) {
            result.error = "read offset is outside input";
            return false;
        }
        const uint64_t availableBytes = static_cast<uint64_t>(fileSize) - request.offset;
        readBytes = request.size == 0 ? availableBytes : std::min(request.size, availableBytes);
        if (readBytes > std::numeric_limits<UINT32>::max()) {
            result.error = "DirectStorage memory read request exceeds UINT32 size limit";
            return false;
        }

        const std::wstring widePath = request.path.wstring();
        const HRESULT hr = factory_->OpenFile(widePath.c_str(), __uuidof(IDStorageFile), reinterpret_cast<void**>(file.GetAddressOf()));
        if (FAILED(hr) || file == nullptr) {
            result.error = "IDStorageFactory::OpenFile failed: HRESULT 0x" + hresultHex(hr);
            return false;
        }

        result.bytes.resize(static_cast<size_t>(readBytes));
        return true;
    }

    [[nodiscard]] StreamingIoReadResult readDirectStorage(const StreamingIoReadRequest& request) {
        const auto start = std::chrono::steady_clock::now();
        StreamingIoReadResult result;
        Microsoft::WRL::ComPtr<IDStorageFile> file;
        uint64_t readBytes = 0;
        if (!prepareDirectStorageRead(request, result, file, readBytes)) {
            recordReadMetrics(metrics_, request, result, 0.0);
            return result;
        }

        result.bytes.resize(static_cast<size_t>(readBytes));
        HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (completionEvent == nullptr) {
            result.error = "CreateEventW failed for DirectStorage completion";
            recordReadMetrics(metrics_, request, result, 0.0);
            return result;
        }

        DSTORAGE_REQUEST dsRequest{};
        dsRequest.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        dsRequest.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        dsRequest.Source.File.Source = file.Get();
        dsRequest.Source.File.Offset = request.offset;
        dsRequest.Source.File.Size = static_cast<UINT32>(readBytes);
        dsRequest.Destination.Memory.Buffer = result.bytes.data();
        dsRequest.Destination.Memory.Size = static_cast<UINT32>(readBytes);
        dsRequest.Name = request.label.empty() ? "rtv streaming read" : request.label.c_str();

        queue_->EnqueueRequest(&dsRequest);
        queue_->EnqueueSetEvent(completionEvent);
        queue_->Submit();
        const DWORD waitResult = WaitForSingleObject(completionEvent, INFINITE);
        CloseHandle(completionEvent);
        if (waitResult != WAIT_OBJECT_0) {
            result.error = "DirectStorage completion wait failed";
            result.bytes.clear();
            recordReadMetrics(metrics_, request, result, 0.0);
            return result;
        }

        DSTORAGE_ERROR_RECORD errorRecord{};
        queue_->RetrieveErrorRecord(&errorRecord);
        if (errorRecord.FailureCount != 0) {
            result.error = "DirectStorage queue failure count " + std::to_string(errorRecord.FailureCount) +
                ", first HRESULT 0x" + hresultHex(errorRecord.FirstFailure.HResult);
            result.bytes.clear();
            recordReadMetrics(metrics_, request, result, 0.0);
            return result;
        }

        result.ok = true;
        result.completedBytes = readBytes;
        const auto end = std::chrono::steady_clock::now();
        recordReadMetrics(metrics_, request, result, std::chrono::duration<double, std::milli>(end - start).count());
        return result;
    }

    [[nodiscard]] std::vector<StreamingIoReadResult> readBatchDirectStorage(const std::vector<StreamingIoReadRequest>& requests) {
        const auto start = std::chrono::steady_clock::now();
        std::vector<StreamingIoReadResult> results(requests.size());
        std::vector<Microsoft::WRL::ComPtr<IDStorageFile>> files(requests.size());
        std::vector<uint64_t> readBytes(requests.size(), 0ull);
        std::vector<size_t> enqueued;
        enqueued.reserve(requests.size());

        for (size_t i = 0; i < requests.size(); ++i) {
            if (!prepareDirectStorageRead(requests[i], results[i], files[i], readBytes[i])) {
                continue;
            }
            DSTORAGE_REQUEST dsRequest{};
            dsRequest.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            dsRequest.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            dsRequest.Source.File.Source = files[i].Get();
            dsRequest.Source.File.Offset = requests[i].offset;
            dsRequest.Source.File.Size = static_cast<UINT32>(readBytes[i]);
            dsRequest.Destination.Memory.Buffer = results[i].bytes.data();
            dsRequest.Destination.Memory.Size = static_cast<UINT32>(readBytes[i]);
            dsRequest.Name = requests[i].label.empty() ? "rtv streaming batch read" : requests[i].label.c_str();
            queue_->EnqueueRequest(&dsRequest);
            enqueued.push_back(i);
        }

        if (!enqueued.empty()) {
            HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (completionEvent == nullptr) {
                for (const size_t index : enqueued) {
                    results[index].error = "CreateEventW failed for DirectStorage batch completion";
                    results[index].bytes.clear();
                }
            } else {
                queue_->EnqueueSetEvent(completionEvent);
                queue_->Submit();
                const DWORD waitResult = WaitForSingleObject(completionEvent, INFINITE);
                CloseHandle(completionEvent);
                if (waitResult != WAIT_OBJECT_0) {
                    for (const size_t index : enqueued) {
                        results[index].error = "DirectStorage batch completion wait failed";
                        results[index].bytes.clear();
                    }
                } else {
                    DSTORAGE_ERROR_RECORD errorRecord{};
                    queue_->RetrieveErrorRecord(&errorRecord);
                    if (errorRecord.FailureCount != 0) {
                        const std::string error = "DirectStorage batch queue failure count " + std::to_string(errorRecord.FailureCount) +
                            ", first HRESULT 0x" + hresultHex(errorRecord.FirstFailure.HResult);
                        for (const size_t index : enqueued) {
                            results[index].error = error;
                            results[index].bytes.clear();
                        }
                    } else {
                        for (const size_t index : enqueued) {
                            results[index].ok = true;
                            results[index].completedBytes = readBytes[index];
                        }
                    }
                }
            }
        }

        const auto end = std::chrono::steady_clock::now();
        const double batchLatencyMs = std::chrono::duration<double, std::milli>(end - start).count();
        for (size_t i = 0; i < requests.size(); ++i) {
            recordReadMetrics(metrics_, requests[i], results[i], batchLatencyMs);
        }
        return results;
    }

    Microsoft::WRL::ComPtr<IDStorageFactory> factory_;
    Microsoft::WRL::ComPtr<IDStorageQueue1> queue_;
#endif

    bool available_ = false;
    std::string unavailableReason_ = "DirectStorage SDK execution backend is not linked in this build; using Win32 file-to-memory fallback";
    StreamingIoMetrics metrics_{};
};

nlohmann::json streamingIoReadResultJson(const StreamingIoReadResult& result, const StreamingIoBackend& requestedBackend) {
    return {
        {"ok", result.ok},
        {"requested_backend", requestedBackend.name()},
        {"requested_backend_available", requestedBackend.available()},
        {"requested_backend_unavailable_reason", requestedBackend.unavailableReason()},
        {"effective_backend", streamingIoBackendKindName(result.backend)},
        {"path", result.path.generic_string()},
        {"offset", result.offset},
        {"requested_bytes", result.requestedBytes},
        {"completed_bytes", result.completedBytes},
        {"fallback_reason", result.fallbackReason},
        {"error", result.error},
    };
}

} // namespace

nlohmann::json streamingIoMetricsJson(const StreamingIoMetrics& metrics) {
    return makeStreamingIoMetricsJson(metrics);
}

std::vector<StreamingIoReadResult> StreamingIoBackend::readBatch(const std::vector<StreamingIoReadRequest>& requests) {
    std::vector<StreamingIoReadResult> results;
    results.reserve(requests.size());
    for (const StreamingIoReadRequest& request : requests) {
        results.push_back(read(request));
    }
    return results;
}

namespace {

bool directStorageCompiledIn() {
#if defined(RTV_WITH_DIRECTSTORAGE) && defined(_WIN32)
    return true;
#else
    return false;
#endif
}

} // namespace

std::unique_ptr<StreamingIoBackend> makeStreamingIoBackend(const StreamingRuntimeOptions& options) {
    switch (options.ioBackend) {
    case StreamingIoBackendKind::DirectStorage:
        return std::make_unique<DirectStorageStreamingIoBackend>();
    case StreamingIoBackendKind::Mock:
        return std::make_unique<MockStreamingIoBackend>();
    case StreamingIoBackendKind::Win32:
    default:
        return std::make_unique<Win32StreamingIoBackend>();
    }
}

nlohmann::json streamingIoBackendAvailabilityJson(const StreamingRuntimeOptions& options) {
    std::unique_ptr<StreamingIoBackend> backend = makeStreamingIoBackend(options);
    const bool available = backend->available();
    const StreamingIoBackendKind effectiveBackend = available
        ? backend->kind()
        : (options.ioBackend == StreamingIoBackendKind::DirectStorage ? StreamingIoBackendKind::Win32 : backend->kind());
    return {
        {"requested_backend", streamingIoBackendKindName(options.ioBackend)},
        {"effective_backend", streamingIoBackendKindName(effectiveBackend)},
        {"requested_backend_available", available},
        {"requested_backend_unavailable_reason", backend->unavailableReason()},
        {"directstorage_requested", options.ioBackend == StreamingIoBackendKind::DirectStorage || options.directStorageEnabled},
        {"directstorage_compiled_in", directStorageCompiledIn()},
        {"directstorage_enabled", options.directStorageEnabled},
        {"force_cpu_decompress", options.forceCpuDecompress},
    };
}

int simulateStreamingIoBackendCommand(
    const std::filesystem::path& input,
    const StreamingRuntimeOptions& options,
    const std::filesystem::path& jsonOut) {
    std::unique_ptr<StreamingIoBackend> backend = makeStreamingIoBackend(options);
    StreamingIoReadRequest request;
    request.path = input;
    request.label = "streaming io probe";
    StreamingIoReadResult result = backend->read(request);

    const nlohmann::json report = {
        {"schema", "StreamingIoBackendProbeV1"},
        {"options", streamingRuntimeOptionsToJson(options)},
        {"read", streamingIoReadResultJson(result, *backend)},
        {"metrics", streamingIoMetricsJson(backend->metrics())},
    };

    if (!jsonOut.empty()) {
        std::error_code ec;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Could not create streaming I/O report directory: " << parent.string() << " (" << ec.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write streaming I/O report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return result.ok ? 0 : 1;
}

int simulateStreamingIoBatchCommand(
    const std::filesystem::path& input,
    const StreamingRuntimeOptions& options,
    uint32_t requestCount,
    uint64_t chunkBytes,
    const std::filesystem::path& jsonOut) {
    std::unique_ptr<StreamingIoBackend> backend = makeStreamingIoBackend(options);

    uint64_t fileBytes = 0;
    std::error_code ec;
    if (std::filesystem::is_regular_file(input, ec)) {
        const uintmax_t size = std::filesystem::file_size(input, ec);
        if (!ec) {
            fileBytes = static_cast<uint64_t>(size);
        }
    }
    if (fileBytes == 0) {
        std::cerr << "Streaming I/O batch input is empty or missing: " << input.string() << '\n';
        return 1;
    }

    const uint64_t effectiveChunkBytes = std::max<uint64_t>(1ull, chunkBytes);
    std::vector<StreamingIoReadRequest> requests;
    requests.reserve(requestCount);
    for (uint32_t i = 0; i < requestCount; ++i) {
        const uint64_t offset = (static_cast<uint64_t>(i) * effectiveChunkBytes) % fileBytes;
        const uint64_t remaining = fileBytes - offset;
        StreamingIoReadRequest request;
        request.path = input;
        request.offset = offset;
        request.size = std::min(effectiveChunkBytes, remaining);
        request.label = "streaming io batch " + std::to_string(i);
        requests.push_back(std::move(request));
    }

    nlohmann::json reads = nlohmann::json::array();
    bool ok = true;
    const auto benchmarkStart = std::chrono::steady_clock::now();
    const std::vector<StreamingIoReadResult> results = backend->readBatch(requests);
    const auto benchmarkEnd = std::chrono::steady_clock::now();
    for (const StreamingIoReadResult& result : results) {
        ok = ok && result.ok;
        reads.push_back(streamingIoReadResultJson(result, *backend));
    }
    const double elapsedMs = std::chrono::duration<double, std::milli>(benchmarkEnd - benchmarkStart).count();
    const StreamingIoMetrics metrics = backend->metrics();
    const double throughputMiBPerSec = elapsedMs <= 0.0
        ? 0.0
        : (static_cast<double>(metrics.bytesCompleted) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);

    const nlohmann::json report = {
        {"schema", "StreamingIoBatchProbeV1"},
        {"ok", ok},
        {"options", streamingRuntimeOptionsToJson(options)},
        {"input", input.generic_string()},
        {"request_count", requestCount},
        {"chunk_bytes", effectiveChunkBytes},
        {"backend_batch_api", true},
        {"elapsed_ms", elapsedMs},
        {"throughput_mib_per_sec", throughputMiBPerSec},
        {"metrics", streamingIoMetricsJson(metrics)},
        {"reads", reads},
    };

    if (!jsonOut.empty()) {
        std::error_code dirEc;
        const std::filesystem::path parent = jsonOut.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, dirEc);
            if (dirEc) {
                std::cerr << "Could not create streaming I/O batch report directory: " << parent.string() << " (" << dirEc.message() << ")\n";
                return 1;
            }
        }
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Could not write streaming I/O batch report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return ok ? 0 : 1;
}

} // namespace rtv
