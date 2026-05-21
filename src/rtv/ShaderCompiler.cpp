#include "rtv/ShaderCompiler.h"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <unordered_set>
#include <utility>

namespace rtv {

ShaderCompiler::ShaderCompiler(std::filesystem::path glslangValidatorPath)
    : glslangValidatorPath_(std::move(glslangValidatorPath)) {}

std::filesystem::path ShaderCompiler::compileIfNeeded(
    const std::filesystem::path& source,
    const std::filesystem::path& outputDirectory) const {
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("Shader source does not exist: " + source.string());
    }

    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path output = outputDirectory / (source.filename().string() + ".spv");
    if (!needsCompile(source, output)) {
        return output;
    }

    const std::string command =
        "\"\"" + glslangValidatorPath_.string() + "\" " +
        "-V --target-env vulkan1.3 " +
        "-o \"" + output.string() + "\" " +
        "\"" + source.string() + "\"\"";
    const int result = std::system(command.c_str());
    if (result != 0) {
        throw std::runtime_error("glslangValidator failed for " + source.string());
    }
    return output;
}

std::vector<uint32_t> ShaderCompiler::readSpirv(const std::filesystem::path& path) {
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
    if (!std::filesystem::exists(output)) {
        return true;
    }

    const auto outputTime = std::filesystem::last_write_time(output);
    if (std::filesystem::last_write_time(source) > outputTime) {
        return true;
    }
    for (const auto& dependency : dependenciesFor(source)) {
        if (std::filesystem::exists(dependency) && std::filesystem::last_write_time(dependency) > outputTime) {
            return true;
        }
    }
    return false;
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
