#include "rtv/ShaderCompiler.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <iostream>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rtv {

namespace {

std::string environmentValueOrDefault(const char* name, const char* fallback) {
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, name);
    if (value == nullptr || value[0] == '\0') {
        std::free(value);
        return fallback;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
#endif
}

bool environmentBoolEnabled(const char* name) {
    const std::string value = environmentValueOrDefault(name, "");
    if (value.empty() || value == "0" || value == "false" || value == "FALSE" || value == "off" || value == "OFF") {
        return false;
    }
    return true;
}

size_t shaderCompileThreadCount() {
    const std::string configured = environmentValueOrDefault("RTV_SHADER_COMPILE_THREADS", "");
    if (!configured.empty()) {
        try {
            const int value = std::stoi(configured);
            if (value > 0) {
                return static_cast<size_t>(value);
            }
        } catch (const std::exception&) {
            std::cerr << "Ignoring invalid RTV_SHADER_COMPILE_THREADS=" << configured << '\n';
        }
    }

    return std::max(1u, std::thread::hardware_concurrency());
}

struct PendingShaderCompiles {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_future<void>> futures;
};

PendingShaderCompiles& pendingShaderCompiles() {
    static PendingShaderCompiles pending;
    return pending;
}

std::string shaderOutputKey(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::string normalizeSignatureLineEndings(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    return value;
}

class ShaderCompileWorkerPool {
public:
    explicit ShaderCompileWorkerPool(size_t threadCount) {
        const size_t workerCount = std::max<size_t>(1, threadCount);
        workers_.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() {
                runWorker();
            });
        }
    }

    ~ShaderCompileWorkerPool() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    std::shared_future<void> submit(std::function<void()> work) {
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(work));
        std::shared_future<void> future = task->get_future().share();
        {
            std::lock_guard lock(mutex_);
            tasks_.emplace_back([task]() {
                (*task)();
            });
        }
        condition_.notify_one();
        return future;
    }

private:
    void runWorker() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

ShaderCompileWorkerPool& shaderCompileWorkerPool() {
    static ShaderCompileWorkerPool pool(shaderCompileThreadCount());
    return pool;
}

void waitForPendingShaderCompile(const std::filesystem::path& path) {
    const std::string key = shaderOutputKey(path);
    std::shared_future<void> future;
    {
        PendingShaderCompiles& pending = pendingShaderCompiles();
        std::lock_guard lock(pending.mutex);
        const auto it = pending.futures.find(key);
        if (it == pending.futures.end()) {
            return;
        }
        future = it->second;
    }

    try {
        future.get();
    } catch (...) {
        PendingShaderCompiles& pending = pendingShaderCompiles();
        std::lock_guard lock(pending.mutex);
        pending.futures.erase(key);
        throw;
    }

    PendingShaderCompiles& pending = pendingShaderCompiles();
    std::lock_guard lock(pending.mutex);
    pending.futures.erase(key);
}

} // namespace

ShaderCompiler::ShaderCompiler(std::filesystem::path glslangValidatorPath)
    : glslangValidatorPath_(std::move(glslangValidatorPath)) {}

std::filesystem::path ShaderCompiler::compileIfNeeded(
    const std::filesystem::path& source,
    const std::filesystem::path& outputDirectory) const {
    return compileIfNeeded(source, outputDirectory, std::string{}, {});
}

std::filesystem::path ShaderCompiler::compileIfNeeded(
    const std::filesystem::path& source,
    const std::filesystem::path& outputDirectory,
    const std::string& outputSuffix,
    const std::vector<std::pair<std::string, std::string>>& extraDefines) const {
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("Shader source does not exist: " + source.string());
    }

    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path output = outputPathFor(source, outputDirectory, outputSuffix);
    const std::filesystem::path signaturePath = output.string() + ".options";
    const std::string signature = compileSignature(extraDefines);
    const std::string reason = compileReasonWithSignature(source, output, signature);
    if (reason.empty()) {
        return output;
    }

    const std::string key = shaderOutputKey(output);
    {
        PendingShaderCompiles& pending = pendingShaderCompiles();
        std::lock_guard lock(pending.mutex);
        if (pending.futures.find(key) != pending.futures.end()) {
            return output;
        }
    }

    const std::string command =
        "\"\"" + glslangValidatorPath_.string() + "\" " +
        "-V --target-env vulkan1.3 " +
        compileDefineArgs(extraDefines) +
        "-o \"" + output.string() + "\" " +
        "\"" + source.string() + "\"\"";

    std::cerr << "Runtime compiling shader " << source.string()
              << " -> " << output.string()
              << " because " << reason << '\n';

    if (environmentBoolEnabled("RTV_DISABLE_RUNTIME_SHADER_COMPILE")) {
        throw std::runtime_error(
            "Runtime shader compilation is disabled by RTV_DISABLE_RUNTIME_SHADER_COMPILE, but shader is stale: " +
            source.string() + " -> " + output.string() + " because " + reason +
            ". Build the rtvulkan_shaders target before launching.");
    }

    auto task = shaderCompileWorkerPool().submit([command, source, signaturePath, signature]() {
        const int result = std::system(command.c_str());
        if (result != 0) {
            throw std::runtime_error("glslangValidator failed for " + source.string());
        }
        std::ofstream signatureFile(signaturePath, std::ios::binary);
        signatureFile << signature;
    });

    {
        PendingShaderCompiles& pending = pendingShaderCompiles();
        std::lock_guard lock(pending.mutex);
        pending.futures.emplace(key, std::move(task));
    }
    return output;
}

std::filesystem::path ShaderCompiler::outputPathFor(
    const std::filesystem::path& source,
    const std::filesystem::path& outputDirectory,
    const std::string& outputSuffix) {
    return outputDirectory / (source.filename().string() + outputSuffix + ".spv");
}

std::vector<uint32_t> ShaderCompiler::readSpirv(const std::filesystem::path& path) {
    waitForPendingShaderCompile(path);

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open SPIR-V file: " + path.string());
    }

    const std::streamsize byteSize = file.tellg();
    if (byteSize <= 0 || byteSize % 4 != 0) {
        throw std::runtime_error("Invalid SPIR-V byte size: " + path.string());
    }
    file.seekg(0);

    std::vector<uint32_t> spirv(static_cast<size_t>(byteSize) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(spirv.data()), byteSize);
    return spirv;
}

bool ShaderCompiler::needsCompile(const std::filesystem::path& source, const std::filesystem::path& output) const {
    return needsCompileWithSignature(source, output, compileSignature());
}

bool ShaderCompiler::needsCompile(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::vector<std::pair<std::string, std::string>>& extraDefines) const {
    return needsCompileWithSignature(source, output, compileSignature(extraDefines));
}

bool ShaderCompiler::needsCompileWithSignature(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::string& signature) const {
    return !compileReasonWithSignature(source, output, signature).empty();
}

std::string ShaderCompiler::compileReason(const std::filesystem::path& source, const std::filesystem::path& output) const {
    return compileReasonWithSignature(source, output, compileSignature());
}

std::string ShaderCompiler::compileReasonWithSignature(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    const std::string& signature) const {
    if (!std::filesystem::exists(output)) {
        return "missing output " + output.string();
    }

    const std::filesystem::path signaturePath = output.string() + ".options";
    std::ifstream signatureFile(signaturePath, std::ios::binary);
    std::string storedSignature;
    if (signatureFile) {
        storedSignature.assign(
            std::istreambuf_iterator<char>(signatureFile),
            std::istreambuf_iterator<char>());
    }
    if (normalizeSignatureLineEndings(storedSignature) != normalizeSignatureLineEndings(signature)) {
        if (!std::filesystem::exists(signaturePath)) {
            return "missing compile signature " + signaturePath.string();
        }
        return "compile signature changed " + signaturePath.string();
    }

    const auto outputTime = std::filesystem::last_write_time(output);
    if (std::filesystem::last_write_time(source) > outputTime) {
        return "source is newer " + source.string();
    }
    for (const auto& dependency : dependenciesFor(source)) {
        if (std::filesystem::exists(dependency) && std::filesystem::last_write_time(dependency) > outputTime) {
            return "include is newer " + dependency.string();
        }
    }
    return {};
}

std::string ShaderCompiler::compileSignature(const std::vector<std::pair<std::string, std::string>>& extraDefines) const {
    auto hasExtraDefine = [&](const char* name) {
        return std::any_of(extraDefines.begin(), extraDefines.end(), [&](const auto& define) {
            return define.first == name;
        });
    };
    std::string signature = "RTV_USE_DIMENSIONED_SAMPLER=" + environmentValueOrDefault("RTV_USE_DIMENSIONED_SAMPLER", "1") +
        "\nRTV_DENOISER_SHARED_TILE=" + environmentValueOrDefault("RTV_DENOISER_SHARED_TILE", "1") + "\n";
    if (!hasExtraDefine("RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT")) {
        signature += "RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT=" +
            environmentValueOrDefault("RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT", "0") + "\n";
    }
    for (const auto& [name, value] : extraDefines) {
        signature += name + "=" + value + "\n";
    }
    return signature;
}

std::string ShaderCompiler::compileDefineArgs(const std::vector<std::pair<std::string, std::string>>& extraDefines) const {
    auto hasExtraDefine = [&](const char* name) {
        return std::any_of(extraDefines.begin(), extraDefines.end(), [&](const auto& define) {
            return define.first == name;
        });
    };
    auto defineArg = [](const char* name, const char* fallback) {
        const std::string finalValue = environmentValueOrDefault(name, fallback);
        return std::string("-D") + name + "=" + finalValue + " ";
    };

    std::string args = defineArg("RTV_USE_DIMENSIONED_SAMPLER", "1") +
        defineArg("RTV_DENOISER_SHARED_TILE", "1");
    if (!hasExtraDefine("RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT")) {
        args += defineArg("RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT", "0");
    }
    for (const auto& [name, value] : extraDefines) {
        args += "-D" + name + "=" + value + " ";
    }
    return args;
}

std::vector<std::filesystem::path> ShaderCompiler::dependenciesFor(const std::filesystem::path& source) const {
    std::vector<std::filesystem::path> deps;
    std::unordered_set<std::string> visited;
    std::regex includePattern(R"shader(^\s*#include\s+"([^"]+)")shader");

    auto visit = [&](const std::filesystem::path& current, auto&& visitSelf) -> void {
        const std::filesystem::path normalized = std::filesystem::weakly_canonical(current);
        const std::string key = normalized.string();
        if (!visited.insert(key).second) {
            return;
        }

        std::ifstream file(current);
        if (!file) {
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            std::smatch match;
            if (std::regex_search(line, match, includePattern)) {
                const std::filesystem::path dependency = current.parent_path() / match[1].str();
                deps.push_back(dependency);
                visitSelf(dependency, visitSelf);
            }
        }
    };

    visit(source, visit);
    return deps;
}

} // namespace rtv
