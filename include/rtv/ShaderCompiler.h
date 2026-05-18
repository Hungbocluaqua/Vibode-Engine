#pragma once

#include <filesystem>
#include <vector>

namespace rtv {

class ShaderCompiler {
public:
    explicit ShaderCompiler(std::filesystem::path glslangValidatorPath);

    [[nodiscard]] std::filesystem::path compileIfNeeded(
        const std::filesystem::path& source,
        const std::filesystem::path& outputDirectory) const;
    [[nodiscard]] static std::vector<uint32_t> readSpirv(const std::filesystem::path& path);

private:
    [[nodiscard]] bool needsCompile(const std::filesystem::path& source, const std::filesystem::path& output) const;
    [[nodiscard]] std::vector<std::filesystem::path> dependenciesFor(const std::filesystem::path& source) const;

    std::filesystem::path glslangValidatorPath_;
};

} // namespace rtv
