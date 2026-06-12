#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace rtv {

struct RuntimeSkeletonJoint {
    int32_t index = -1;
    std::string name;
    int32_t parentIndex = -1;
    std::string parentName;
    std::array<float, 16> inverseBindMatrix{};
    bool hasInverseBindMatrix = false;
};

class RuntimeSkeleton {
public:
    [[nodiscard]] static RuntimeSkeleton fromJson(const nlohmann::json& root, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static RuntimeSkeleton loadJson(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static RuntimeSkeleton loadNativeBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static RuntimeSkeleton loadNative(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);
    [[nodiscard]] static RuntimeSkeleton load(const std::filesystem::path& path, std::vector<std::string>* warnings = nullptr);

    [[nodiscard]] bool valid() const { return !joints_.empty(); }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] int32_t skeletonRoot() const { return skeletonRoot_; }
    [[nodiscard]] const std::vector<RuntimeSkeletonJoint>& joints() const { return joints_; }

private:
    std::string name_;
    int32_t skeletonRoot_ = -1;
    std::vector<RuntimeSkeletonJoint> joints_;
};

[[nodiscard]] nlohmann::json runtimeSkeletonInspectionJson(const RuntimeSkeleton& skeleton, const std::vector<std::string>& warnings = {});
[[nodiscard]] int inspectRuntimeSkeletonCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut = {});

} // namespace rtv
