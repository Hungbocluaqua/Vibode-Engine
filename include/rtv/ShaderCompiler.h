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
    [[nodiscard]] static std::filesystem::path outputPathFor(
        const std::filesystem::path& source,
        const std::filesystem::path& outputDirectory,
        const std::string& outputSuffix = {});
    [[nodiscard]] bool needsCompile(const std::filesystem::path& source, const std::filesystem::path& output) const;
    [[nodiscard]] bool needsCompile(
        const std::filesystem::path& source,
        const std::filesystem::path& output,
        const std::vector<std::pair<std::string, std::string>>& extraDefines) const;
    [[nodiscard]] std::string compileReason(const std::filesystem::path& source, const std::filesystem::path& output) const;
    [[nodiscard]] static std::vector<uint32_t> readSpirv(const std::filesystem::path& path);
    [[nodiscard]] std::vector<std::filesystem::path> dependenciesFor(const std::filesystem::path& source) const;

private:
    [[nodiscard]] bool needsCompileWithSignature(
        const std::filesystem::path& source,
        const std::filesystem::path& output,
        const std::string& signature) const;
    [[nodiscard]] std::string compileReasonWithSignature(
        const std::filesystem::path& source,
        const std::filesystem::path& output,
        const std::string& signature) const;
    [[nodiscard]] std::string compileSignature(const std::vector<std::pair<std::string, std::string>>& extraDefines = {}) const;
    [[nodiscard]] std::string compileDefineArgs(const std::vector<std::pair<std::string, std::string>>& extraDefines = {}) const;

    std::filesystem::path glslangValidatorPath_;
};

} // namespace rtv
