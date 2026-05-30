#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

class AssetManager;
struct CachedScene;
struct SceneAsset;

constexpr uint32_t kDefaultOpacityMicromapSubdivisionLevel = 2u;

enum class OpacityMicromapCpuState : uint8_t {
    Transparent = 0,
    Opaque = 1,
    Unknown = 2,
    Mixed = 3,
};

struct OpacityMicromapPrimitiveCpuData {
    uint32_t meshIndex = 0;
    uint32_t primitiveIndex = 0;
    uint32_t materialIndex = 0;
    int32_t alphaTextureIndex = -1;
    uint32_t firstIndex = 0;
    uint32_t subdivisionLevel = kDefaultOpacityMicromapSubdivisionLevel;
    uint32_t triangleCount = 0;
    uint32_t stateOffset = 0;
    uint32_t stateCount = 0;
    uint32_t opaqueCount = 0;
    uint32_t transparentCount = 0;
    uint32_t unknownCount = 0;
    uint32_t mixedCount = 0;
};

struct OpacityMicromapPreprocessStats {
    uint32_t subdivisionLevel = kDefaultOpacityMicromapSubdivisionLevel;
    uint32_t eligiblePrimitiveCount = 0;
    uint32_t generatedPrimitiveCount = 0;
    uint32_t alphaTexturePrimitiveCount = 0;
    uint32_t constantAlphaPrimitiveCount = 0;
    uint32_t cacheEntryCount = 0;
    uint32_t cacheHitCount = 0;
    uint32_t totalTriangleCount = 0;
    uint32_t microTriangleCount = 0;
    uint32_t opaqueCount = 0;
    uint32_t transparentCount = 0;
    uint32_t unknownCount = 0;
    uint32_t mixedCount = 0;
    uint64_t dataBytes = 0;
    double preprocessingMs = 0.0;
    uint32_t validationErrorCount = 0;
    std::vector<std::string> warnings;
};

struct OpacityMicromapCpuData {
    OpacityMicromapPreprocessStats stats{};
    std::vector<OpacityMicromapPrimitiveCpuData> primitives;
    std::vector<uint8_t> microTriangleStates;
};

[[nodiscard]] OpacityMicromapCpuData generateOpacityMicromapData(
    const SceneAsset& scene,
    const AssetManager& assets,
    uint32_t subdivisionLevel = kDefaultOpacityMicromapSubdivisionLevel);

[[nodiscard]] OpacityMicromapCpuData generateOpacityMicromapData(
    const CachedScene& cached,
    uint32_t subdivisionLevel = kDefaultOpacityMicromapSubdivisionLevel);

bool writeOpacityMicromapDebugImages(const OpacityMicromapCpuData& data, const std::filesystem::path& directory);

} // namespace rtv
