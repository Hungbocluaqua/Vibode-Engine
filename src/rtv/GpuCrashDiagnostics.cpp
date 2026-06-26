#include "rtv/GpuCrashDiagnostics.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#if defined(RTV_HAS_NSIGHT_AFTERMATH)
#include <GFSDK_Aftermath_GpuCrashDump.h>
#endif

namespace rtv {
namespace {

std::filesystem::path g_outputDirectory;
std::mutex g_dumpMutex;
GpuCrashDiagnosticsStatus g_status{
#if defined(RTV_HAS_NSIGHT_AFTERMATH)
    .buildAvailable = true,
#else
    .buildAvailable = false,
#endif
};

std::string uniqueDumpStem(const char* kind) {
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return std::string(kind) + "_" + std::to_string(ticks);
}

void writeBinaryDump(const char* kind, const char* extension, const void* data, uint32_t size) {
    std::scoped_lock lock(g_dumpMutex);
    std::filesystem::create_directories(g_outputDirectory);
    const std::filesystem::path path = g_outputDirectory / (uniqueDumpStem(kind) + extension);
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    output.flush();
    if (std::string_view(extension) == ".nv-gpudmp") {
        ++g_status.dumpCount;
    } else if (std::string_view(extension) == ".nvdbg") {
        ++g_status.shaderDebugInfoCount;
    }
    std::cerr << "Nsight Aftermath wrote " << path.string() << '\n';
}

uint32_t countFilesWithExtension(const std::filesystem::path& directory, const char* extension) {
    if (directory.empty() || !std::filesystem::exists(directory)) {
        return 0u;
    }
    uint32_t count = 0;
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == extension) {
            ++count;
        }
    }
    return count;
}

#if defined(RTV_HAS_NSIGHT_AFTERMATH)

void GFSDK_AFTERMATH_CALL gpuCrashDumpCallback(const void* data, uint32_t size, void*) {
    writeBinaryDump("gpu_crash", ".nv-gpudmp", data, size);
}

void GFSDK_AFTERMATH_CALL shaderDebugInfoCallback(const void* data, uint32_t size, void*) {
    writeBinaryDump("shader_debug", ".nvdbg", data, size);
}

void GFSDK_AFTERMATH_CALL crashDumpDescriptionCallback(
    PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addValue,
    void*) {
    addValue(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "rtvulkan");
    addValue(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "diagnostics-v1");
}
#endif

} // namespace

GpuCrashDiagnostics::~GpuCrashDiagnostics() {
#if defined(RTV_HAS_NSIGHT_AFTERMATH)
    if (enabled_) {
        GFSDK_Aftermath_DisableGpuCrashDumps();
    }
#endif
}

bool GpuCrashDiagnostics::enable(const std::filesystem::path& outputDirectory) {
#if defined(RTV_HAS_NSIGHT_AFTERMATH)
    recordGpuCrashDiagnosticsRequest(true, outputDirectory);
    g_outputDirectory = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(g_outputDirectory);
    const GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
        gpuCrashDumpCallback,
        shaderDebugInfoCallback,
        crashDumpDescriptionCallback,
        nullptr,
        nullptr);
    enabled_ = result == GFSDK_Aftermath_Result_Success;
    {
        std::scoped_lock lock(g_dumpMutex);
        g_status.enabled = enabled_;
        g_status.enableResult = static_cast<uint32_t>(result);
        g_status.unavailableReason = enabled_ ? std::string{} : "GFSDK_Aftermath_EnableGpuCrashDumps failed";
        g_status.dumpCount = countFilesWithExtension(g_outputDirectory, ".nv-gpudmp");
        g_status.shaderDebugInfoCount = countFilesWithExtension(g_outputDirectory, ".nvdbg");
    }
    if (!enabled_) {
        std::cerr << "Warning: Nsight Aftermath initialization failed with result "
                  << static_cast<uint32_t>(result) << '\n';
    }
    return enabled_;
#else
    recordGpuCrashDiagnosticsRequest(true, outputDirectory);
    {
        std::scoped_lock lock(g_dumpMutex);
        g_status.enabled = false;
        g_status.unavailableReason = "Nsight Aftermath was not available at build time";
    }
    std::cerr << "Warning: GPU crash dumps requested, but Nsight Aftermath was not available at build time.\n";
    return false;
#endif
}

void recordGpuCrashDiagnosticsRequest(bool requested, const std::filesystem::path& outputDirectory) {
    std::scoped_lock lock(g_dumpMutex);
    g_status.requested = requested;
    g_status.outputDirectory = outputDirectory.empty()
        ? std::filesystem::path{}
        : std::filesystem::absolute(outputDirectory);
    if (!requested) {
        g_status.enabled = false;
        g_status.enableResult = 0u;
        g_status.unavailableReason.clear();
    } else if (!g_status.buildAvailable) {
        g_status.unavailableReason = "Nsight Aftermath was not available at build time";
    }
    g_outputDirectory = g_status.outputDirectory;
    g_status.dumpCount = countFilesWithExtension(g_status.outputDirectory, ".nv-gpudmp");
    g_status.shaderDebugInfoCount = countFilesWithExtension(g_status.outputDirectory, ".nvdbg");
}

GpuCrashDiagnosticsStatus gpuCrashDiagnosticsStatus() {
    std::scoped_lock lock(g_dumpMutex);
    g_status.dumpCount = countFilesWithExtension(g_status.outputDirectory, ".nv-gpudmp");
    g_status.shaderDebugInfoCount = countFilesWithExtension(g_status.outputDirectory, ".nvdbg");
    return g_status;
}

} // namespace rtv
