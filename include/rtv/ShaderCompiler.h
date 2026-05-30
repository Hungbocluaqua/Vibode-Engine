#pragma once

#include <filesystem>
#include <utility>
#include <string>
#include <vector>

namespace rtv {

class ShaderCompiler {
public:
    explicit ShaderCompiler(std::filesystem::path glslangValidatorPath);

    [[nodiscard]] std::filesystem::path compileIfNeeded(
        const std::filesystem::path& source,
        const std::filesystem::path& outputDirectory) const;
    [[nodiscard]] std::filesystem::path compileIfNeeded(
        const std::filesystem::path& source,
        const std::filesystem::path& outputDirectory,
        const std::string& outputSuffix,
        const std::vector<std::pair<std::string, std::string>>& extraDefines) const;
    [[nodiscard]] bool needsCompile(const std::filesystem::path& source, const std::filesystem::path& output) const;
    [[nodiscard]] static std::vector<uint32_t> readSpirv(const std::filesystem::path& path);

private:
    [[nodiscard]] std::vector<std::filesystem::path> dependenciesFor(const std::filesystem::path& source) const;
    [[nodiscard]] bool needsCompileWithSignature(
        const std::filesystem::path& source,
        const std::filesystem::path& output,
        const std::string& signature) const;
    [[nodiscard]] std::string compileSignature(const std::vector<std::pair<std::string, std::string>>& extraDefines = {}) const;
    [[nodiscard]] std::string compileDefineArgs(const std::vector<std::pair<std::string, std::string>>& extraDefines = {}) const;

    std::filesystem::path glslangValidatorPath_;
};

} // namespace rtv
