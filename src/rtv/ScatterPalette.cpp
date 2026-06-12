#include "rtv/ScatterPalette.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace rtv {

namespace {

float jsonFloat(const nlohmann::json& json, const char* key, float fallback) {
    if (!json.contains(key) || !json[key].is_number()) {
        return fallback;
    }
    return json[key].get<float>();
}

uint32_t jsonUint(const nlohmann::json& json, const char* key, uint32_t fallback) {
    if (!json.contains(key) || !json[key].is_number_unsigned()) {
        return fallback;
    }
    return json[key].get<uint32_t>();
}

std::string jsonString(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_string()) {
        return {};
    }
    return json[key].get<std::string>();
}

void normalizeSettings(ScatterPaletteSettings& settings) {
    settings.density = std::clamp(settings.density, 0.0f, 10000.0f);
    settings.slopeMinDegrees = std::clamp(settings.slopeMinDegrees, 0.0f, 180.0f);
    settings.slopeMaxDegrees = std::clamp(settings.slopeMaxDegrees, 0.0f, 180.0f);
    if (settings.slopeMaxDegrees < settings.slopeMinDegrees) {
        std::swap(settings.slopeMinDegrees, settings.slopeMaxDegrees);
    }
    if (settings.heightMax < settings.heightMin) {
        std::swap(settings.heightMin, settings.heightMax);
    }
    settings.scaleMin = std::clamp(settings.scaleMin, 0.001f, 1000.0f);
    settings.scaleMax = std::clamp(settings.scaleMax, 0.001f, 1000.0f);
    if (settings.scaleMax < settings.scaleMin) {
        std::swap(settings.scaleMin, settings.scaleMax);
    }
    settings.yawRandomDegrees = std::clamp(settings.yawRandomDegrees, 0.0f, 360.0f);
    settings.spacing = std::clamp(settings.spacing, 0.001f, 10000.0f);
    settings.collisionRadius = std::clamp(settings.collisionRadius, 0.0f, 10000.0f);
}

} // namespace

const char* scatterPaletteEntryKindName(ScatterPaletteEntryKind kind) {
    switch (kind) {
    case ScatterPaletteEntryKind::Mesh: return "mesh";
    case ScatterPaletteEntryKind::Prefab: return "prefab";
    case ScatterPaletteEntryKind::Material: return "material";
    }
    return "mesh";
}

ScatterPaletteEntryKind scatterPaletteEntryKindFromString(const std::string& value) {
    if (value == "prefab") {
        return ScatterPaletteEntryKind::Prefab;
    }
    if (value == "material") {
        return ScatterPaletteEntryKind::Material;
    }
    return ScatterPaletteEntryKind::Mesh;
}

bool loadScatterPalette(const std::filesystem::path& path, ScatterPalette& palette, std::string* error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        if (error != nullptr) {
            *error = "Could not open scatter palette: " + path.string();
        }
        return false;
    }

    try {
        nlohmann::json root;
        file >> root;
        if (!root.is_object()) {
            if (error != nullptr) {
                *error = "Scatter palette root must be an object";
            }
            return false;
        }

        ScatterPalette next;
        next.name = jsonString(root, "name");
        const nlohmann::json settings = root.value("settings", nlohmann::json::object());
        next.settings.density = jsonFloat(settings, "density", next.settings.density);
        next.settings.slopeMinDegrees = jsonFloat(settings, "slopeMinDegrees", next.settings.slopeMinDegrees);
        next.settings.slopeMaxDegrees = jsonFloat(settings, "slopeMaxDegrees", next.settings.slopeMaxDegrees);
        next.settings.heightMin = jsonFloat(settings, "heightMin", next.settings.heightMin);
        next.settings.heightMax = jsonFloat(settings, "heightMax", next.settings.heightMax);
        next.settings.scaleMin = jsonFloat(settings, "scaleMin", next.settings.scaleMin);
        next.settings.scaleMax = jsonFloat(settings, "scaleMax", next.settings.scaleMax);
        next.settings.yawRandomDegrees = jsonFloat(settings, "yawRandomDegrees", next.settings.yawRandomDegrees);
        next.settings.seed = jsonUint(settings, "seed", next.settings.seed);
        next.settings.spacing = jsonFloat(settings, "spacing", next.settings.spacing);
        next.settings.collisionRadius = jsonFloat(settings, "collisionRadius", next.settings.collisionRadius);
        if (settings.contains("surfaceAlignment") && settings["surfaceAlignment"].is_boolean()) {
            next.settings.surfaceAlignment = settings["surfaceAlignment"].get<bool>();
        }
        normalizeSettings(next.settings);

        const nlohmann::json entries = root.value("entries", nlohmann::json::array());
        if (!entries.is_array()) {
            if (error != nullptr) {
                *error = "Scatter palette entries must be an array";
            }
            return false;
        }
        for (const nlohmann::json& entryJson : entries) {
            if (!entryJson.is_object()) {
                continue;
            }
            ScatterPaletteEntry entry;
            entry.kind = scatterPaletteEntryKindFromString(jsonString(entryJson, "kind"));
            entry.assetGuid = jsonString(entryJson, "assetGuid");
            entry.materialGuid = jsonString(entryJson, "materialGuid");
            entry.weight = std::clamp(jsonFloat(entryJson, "weight", 1.0f), 0.0f, 10000.0f);
            if (!entry.assetGuid.empty()) {
                next.entries.push_back(std::move(entry));
            }
        }

        palette = std::move(next);
        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = ex.what();
        }
        return false;
    }
}

bool saveScatterPalette(const std::filesystem::path& path, const ScatterPalette& palette, std::string* error) {
    ScatterPaletteSettings settings = palette.settings;
    normalizeSettings(settings);
    nlohmann::json root;
    root["schema"] = "RtScatterPaletteV1";
    root["name"] = palette.name;
    root["settings"] = {
        {"density", settings.density},
        {"slopeMinDegrees", settings.slopeMinDegrees},
        {"slopeMaxDegrees", settings.slopeMaxDegrees},
        {"heightMin", settings.heightMin},
        {"heightMax", settings.heightMax},
        {"scaleMin", settings.scaleMin},
        {"scaleMax", settings.scaleMax},
        {"yawRandomDegrees", settings.yawRandomDegrees},
        {"seed", settings.seed},
        {"spacing", settings.spacing},
        {"collisionRadius", settings.collisionRadius},
        {"surfaceAlignment", settings.surfaceAlignment},
    };
    root["entries"] = nlohmann::json::array();
    for (const ScatterPaletteEntry& entry : palette.entries) {
        root["entries"].push_back({
            {"kind", scatterPaletteEntryKindName(entry.kind)},
            {"assetGuid", entry.assetGuid},
            {"materialGuid", entry.materialGuid},
            {"weight", std::clamp(entry.weight, 0.0f, 10000.0f)},
        });
    }

    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error != nullptr) {
                *error = ec.message();
            }
            return false;
        }
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        if (error != nullptr) {
            *error = "Could not write scatter palette: " + path.string();
        }
        return false;
    }
    file << root.dump(2);
    return file.good();
}

} // namespace rtv
