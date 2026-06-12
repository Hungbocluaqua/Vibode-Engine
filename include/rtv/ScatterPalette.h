#pragma once

#include "rtv/AssetImport.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

enum class ScatterPaletteEntryKind : uint8_t {
    Mesh,
    Prefab,
    Material,
};

struct ScatterPaletteEntry {
    ScatterPaletteEntryKind kind = ScatterPaletteEntryKind::Mesh;
    AssetGuid assetGuid;
    AssetGuid materialGuid;
    float weight = 1.0f;
};

struct ScatterPaletteSettings {
    float density = 1.0f;
    float slopeMinDegrees = 0.0f;
    float slopeMaxDegrees = 60.0f;
    float heightMin = -10000.0f;
    float heightMax = 10000.0f;
    float scaleMin = 1.0f;
    float scaleMax = 1.0f;
    float yawRandomDegrees = 180.0f;
    uint32_t seed = 1;
    float spacing = 1.0f;
    float collisionRadius = 0.25f;
    bool surfaceAlignment = true;
};

struct ScatterPalette {
    std::string name;
    ScatterPaletteSettings settings;
    std::vector<ScatterPaletteEntry> entries;
};

[[nodiscard]] bool loadScatterPalette(const std::filesystem::path& path, ScatterPalette& palette, std::string* error = nullptr);
[[nodiscard]] bool saveScatterPalette(const std::filesystem::path& path, const ScatterPalette& palette, std::string* error = nullptr);
[[nodiscard]] const char* scatterPaletteEntryKindName(ScatterPaletteEntryKind kind);
[[nodiscard]] ScatterPaletteEntryKind scatterPaletteEntryKindFromString(const std::string& value);

} // namespace rtv
