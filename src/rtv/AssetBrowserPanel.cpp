#include "rtv/AssetBrowserPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/EditorPreferences.h"
#include "rtv/EditorUiStyle.h"
#include "rtv/FileDialog.h"
#include "rtv/GpuScene.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <nlohmann/json.hpp>

#include <stb_image.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#endif

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace rtv {

namespace {

template <size_t N>
void setTextBuffer(std::array<char, N>& buffer, const std::string& value) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
}

template <size_t N>
void setPathBuffer(std::array<char, N>& buffer, const std::filesystem::path& path) {
    setTextBuffer(buffer, path.string());
}

std::string lowerString(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool isModelAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

bool isTextureAssetPath(const std::filesystem::path& path) {
    return editorIsTexturePath(path);
}

bool isEnvironmentAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".hdr" || ext == ".exr";
}

bool isRasterThumbnailPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr";
}

bool isSceneAssetPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".rtlevel" || ext == ".mscene";
}

bool isProjectAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".vproject";
}

bool isMaterialAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".mtl";
}

bool isIesAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".ies";
}

bool isVolumeAssetPath(const std::filesystem::path& path) {
    return lowerString(path.extension().string()) == ".vdb";
}

std::string contentKindLabel(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
        return "Folder";
    }
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".rtlevel") return "Scene";
    if (ext == ".mscene") return "Minitech Scene";
    if (ext == ".vproject") return "Project";
    if (ext == ".gltf" || ext == ".glb") return "Model";
    if (ext == ".obj") return "OBJ Model";
    if (ext == ".mtl") return "Material";
    if (isTextureAssetPath(path)) return "Texture";
    if (ext == ".hdr" || ext == ".exr") return "Environment";
    if (ext == ".ies") return "IES Profile";
    if (ext == ".vdb") return "Volume";
    if (ext == ".glsl" || ext == ".hlsl" || ext == ".spv") return "Shader";
    if (ext == ".json") return "JSON";
    return ext.empty() ? "File" : ext.substr(1);
}

std::string compactPathLabel(const std::filesystem::path& path, const char* fallback) {
    if (!path.empty()) {
        const std::filesystem::path name = path.filename();
        if (!name.empty()) {
            return name.string();
        }
        const std::filesystem::path root = path.root_name();
        if (!root.empty()) {
            return root.string();
        }
    }
    return fallback != nullptr ? fallback : "";
}

bool supportedContentPath(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) return true;
    const std::string ext = lowerString(path.extension().string());
    return ext == ".rtlevel" || ext == ".mscene" || ext == ".gltf" || ext == ".glb" || ext == ".obj" || ext == ".mtl" ||
        ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr" || ext == ".exr" ||
        ext == ".ies" || ext == ".vdb" || ext == ".vproject";
}

bool canOpenOrApplyPath(const std::filesystem::path& path) {
    return isSceneAssetPath(path) || isModelAssetPath(path) || isEnvironmentAssetPath(path);
}

void copyPathToClipboard(const std::filesystem::path& path) {
    const std::string text = path.string();
    ImGui::SetClipboardText(text.c_str());
}

void revealPathInFileBrowser(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::filesystem::path target = ec ? path : absolute;
    if (std::filesystem::is_directory(target, ec)) {
        const std::string directory = target.string();
        ShellExecuteA(nullptr, "open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        const std::string args = "/select,\"" + target.string() + "\"";
        ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
#else
    (void)path;
#endif
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec);
}

bool pathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (path.empty() || root.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(canonicalForCompare(path), canonicalForCompare(root), ec);
    if (ec) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

float assetImportProgress(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Imported && !record.missing && !record.stale) {
        return 1.0f;
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return 0.65f;
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        return 0.20f;
    }
    if (record.status == AssetImportStatus::Failed) {
        return 0.0f;
    }
    return 0.35f;
}

const char* assetImportProgressLabel(const AssetRecord& record) {
    if (record.status == AssetImportStatus::Imported && !record.missing && !record.stale) {
        return "Ready";
    }
    if (record.status == AssetImportStatus::Stale || record.stale) {
        return "Needs reimport";
    }
    if (record.status == AssetImportStatus::Missing || record.missing) {
        return "Source missing";
    }
    if (record.status == AssetImportStatus::Failed) {
        return "Failed";
    }
    return "Pending metadata";
}

ImU32 contentIconColor(const std::filesystem::path& path) {
    const EditorGlyphIcon icon = editorGlyphForPath(path);
    switch (icon) {
    case EditorGlyphIcon::Folder:
        return IM_COL32(188, 194, 204, 255);
    case EditorGlyphIcon::Texture:
    case EditorGlyphIcon::Environment:
        return IM_COL32(184, 196, 211, 255);
    case EditorGlyphIcon::Model:
    case EditorGlyphIcon::SceneFile:
        return IM_COL32(188, 199, 216, 255);
    case EditorGlyphIcon::Material:
        return IM_COL32(198, 190, 212, 255);
    case EditorGlyphIcon::IesProfile:
        return IM_COL32(210, 198, 168, 255);
    case EditorGlyphIcon::VolumeFile:
        return IM_COL32(178, 204, 198, 255);
    case EditorGlyphIcon::ShaderFile:
    case EditorGlyphIcon::ConfigFile:
        return IM_COL32(178, 188, 204, 255);
    default:
        return IM_COL32(158, 166, 178, 255);
    }
}

void drawContentGlyph(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    editorDrawIconGlyph(editorGlyphForPath(path), min, max, contentIconColor(path));
}

bool contentActionButton(const char* id, EditorGlyphIcon icon, const char* label, const char* tooltip) {
    const bool clicked = editorIconTextButton(id, icon, label);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

uint8_t toneMapHdrChannel(float value) {
    const float mapped = value <= 0.0f ? 0.0f : value / (1.0f + value);
    return static_cast<uint8_t>(std::clamp(std::pow(mapped, 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f);
}

std::filesystem::path resolveAssetRecordPath(const EditorRuntimeState& state, const std::string& value) {
    if (value.empty()) {
        return {};
    }
    std::filesystem::path path(value);
    if (path.is_absolute()) {
        return path;
    }
    if (state.project != nullptr) {
        return state.project->projectRoot / path;
    }
    if (state.assetRegistry != nullptr && !state.assetRegistry->state().path.empty()) {
        return state.assetRegistry->state().path.parent_path() / path;
    }
    return path;
}

std::string fileSizeLabel(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (ec) {
        return "Size unavailable";
    }
    const double bytes = static_cast<double>(size);
    std::ostringstream out;
    if (bytes >= 1024.0 * 1024.0) {
        out << "Size " << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024.0) {
        out << "Size " << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
    } else {
        out << "Size " << size << " B";
    }
    return out.str();
}

std::optional<nlohmann::json> readJsonFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    try {
        nlohmann::json json;
        file >> json;
        return json;
    } catch (...) {
        return std::nullopt;
    }
}

size_t jsonArraySize(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_array()) {
        return 0;
    }
    return json[key].size();
}

std::string countLabel(const char* label, size_t value) {
    return std::string(label) + " " + std::to_string(value);
}

bool samePathForOperation(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    return canonicalForCompare(lhs) == canonicalForCompare(rhs);
}

bool sceneLoadStatusIsSuccessfulCompletion(const std::string& status) {
    const std::string lower = lowerString(status);
    return lower.find("completed") != std::string::npos &&
        lower.find("failed") == std::string::npos &&
        lower.find("cancelled") == std::string::npos &&
        lower.find("error") == std::string::npos;
}

bool textureAssetMatchesPath(const TextureAsset& texture, const std::filesystem::path& path) {
    if (path.empty() || texture.sourcePath.empty()) {
        return false;
    }
    if (canonicalForCompare(texture.sourcePath) == canonicalForCompare(path)) {
        return true;
    }
    return lowerString(texture.sourcePath.filename().string()) == lowerString(path.filename().string());
}

std::optional<uint32_t> materialTextureSlotForPath(const EditorRuntimeState& state, const std::filesystem::path& path) {
    if (state.importedScene == nullptr || state.assets == nullptr || !isTextureAssetPath(path)) {
        return std::nullopt;
    }
    const SceneAsset& scene = *state.importedScene;
    for (uint32_t slot = 0; slot < scene.textures.size(); ++slot) {
        const TextureAsset* texture = state.assets->texture(scene.textures[slot]);
        if (texture != nullptr && textureAssetMatchesPath(*texture, path)) {
            return slot;
        }
    }
    return std::nullopt;
}

uint64_t fnv1a64(const std::string& text) {
    uint64_t value = 14695981039346656037ull;
    for (unsigned char ch : text) {
        value ^= static_cast<uint64_t>(ch);
        value *= 1099511628211ull;
    }
    return value;
}

std::string hex64(uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

int64_t pathWriteStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return stamp.time_since_epoch().count();
}

uintmax_t pathSizeForCache(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return 0;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

bool isGeneratedPreviewDiskCacheCandidate(const std::filesystem::path& path) {
    std::error_code ec;
    return isModelAssetPath(path) || isSceneAssetPath(path) || isProjectAssetPath(path) || isMaterialAssetPath(path) ||
        isIesAssetPath(path) || isVolumeAssetPath(path) || std::filesystem::is_directory(path, ec);
}

} // namespace

void AssetBrowserPanel::invalidateThumbnails() {
    thumbnailCache_.clear();
    sourcePreviewCache_.clear();
}

AssetBrowserPanel::CpuThumbnail& AssetBrowserPanel::thumbnailForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    CpuThumbnail& thumbnail = thumbnailCache_[key];
    if (thumbnail.attempted) {
        return thumbnail;
    }
    thumbnail.attempted = true;
    thumbnail.columns = 12;
    thumbnail.rows = 7;
    thumbnail.colors.assign(static_cast<size_t>(thumbnail.columns * thumbnail.rows), IM_COL32(32, 38, 46, 255));

    if (!isRasterThumbnailPath(path) || !std::filesystem::exists(path, ec)) {
        return thumbnail;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    const std::string filename = path.string();
    if (lowerString(path.extension().string()) == ".hdr") {
        float* data = stbi_loadf(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (data == nullptr || width <= 0 || height <= 0) {
            if (data != nullptr) {
                stbi_image_free(data);
            }
            return thumbnail;
        }
        thumbnail.width = width;
        thumbnail.height = height;
        thumbnail.available = true;
        for (int row = 0; row < thumbnail.rows; ++row) {
            for (int col = 0; col < thumbnail.columns; ++col) {
                const int sampleX = std::clamp((col * width) / thumbnail.columns + width / (thumbnail.columns * 2), 0, width - 1);
                const int sampleY = std::clamp((row * height) / thumbnail.rows + height / (thumbnail.rows * 2), 0, height - 1);
                const size_t index = (static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * 4u;
                thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(
                    toneMapHdrChannel(data[index + 0]),
                    toneMapHdrChannel(data[index + 1]),
                    toneMapHdrChannel(data[index + 2]),
                    255);
            }
        }
        stbi_image_free(data);
        return thumbnail;
    }

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (data == nullptr || width <= 0 || height <= 0) {
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return thumbnail;
    }
    thumbnail.width = width;
    thumbnail.height = height;
    thumbnail.available = true;
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const int sampleX = std::clamp((col * width) / thumbnail.columns + width / (thumbnail.columns * 2), 0, width - 1);
            const int sampleY = std::clamp((row * height) / thumbnail.rows + height / (thumbnail.rows * 2), 0, height - 1);
            const size_t index = (static_cast<size_t>(sampleY) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * 4u;
            thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)] = IM_COL32(data[index + 0], data[index + 1], data[index + 2], 255);
        }
    }
    stbi_image_free(data);
    return thumbnail;
}

bool AssetBrowserPanel::drawRasterThumbnail(const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected) {
    if (!isRasterThumbnailPath(path)) {
        return false;
    }
    CpuThumbnail& thumbnail = thumbnailForPath(path);
    if (!thumbnail.available) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 innerMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 innerMax(max.x - 4.0f, max.y - 17.0f);
    const float cellW = (innerMax.x - innerMin.x) / static_cast<float>(thumbnail.columns);
    const float cellH = (innerMax.y - innerMin.y) / static_cast<float>(thumbnail.rows);
    for (int row = 0; row < thumbnail.rows; ++row) {
        for (int col = 0; col < thumbnail.columns; ++col) {
            const ImVec2 cellMin(innerMin.x + static_cast<float>(col) * cellW, innerMin.y + static_cast<float>(row) * cellH);
            const ImVec2 cellMax(innerMin.x + static_cast<float>(col + 1) * cellW + 0.5f, innerMin.y + static_cast<float>(row + 1) * cellH + 0.5f);
            dl->AddRectFilled(cellMin, cellMax, thumbnail.colors[static_cast<size_t>(row * thumbnail.columns + col)]);
        }
    }
    dl->AddRect(innerMin, innerMax, IM_COL32(255, 255, 255, 42), 1.0f);
    const std::string badge = std::to_string(thumbnail.width) + "x" + std::to_string(thumbnail.height);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(178, 188, 202, 255), badge.c_str());
    dl->AddRect(min, max, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(54, 62, 72, 255), EditorUiMetric::cardRounding);
    return true;
}

bool AssetBrowserPanel::drawGpuSceneTextureThumbnail(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    if (!state.uiTextures.valid()) {
        return false;
    }
    const std::optional<uint32_t> slot = materialTextureSlotForPath(state, path);
    if (!slot.has_value()) {
        return false;
    }
    const std::vector<VkDescriptorImageInfo> descriptors = state.renderer.scene().materialCombinedDescriptors();
    if (*slot >= descriptors.size()) {
        return false;
    }
    const VkDescriptorImageInfo& descriptor = descriptors[*slot];
    const VkDescriptorSet texture = state.uiTextures.texture(descriptor.imageView, descriptor.imageLayout);
    if (texture == VK_NULL_HANDLE) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 imageMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 imageMax(max.x - 4.0f, max.y - 17.0f);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), imageMin, imageMax);
    dl->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42), 1.0f);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(160, 210, 255, 255), "GPU texture");
    dl->AddRect(min, max, ImGui::GetColorU32(editorActiveRowColor()), EditorUiMetric::cardRounding);
    return true;
}

bool AssetBrowserPanel::drawStandaloneGpuAssetPreview(const EditorRuntimeState& state, const std::filesystem::path& path, ImVec2 min, ImVec2 max, bool selected) {
    uint32_t width = 0;
    uint32_t height = 0;
    const VkDescriptorSet texture = state.uiTextures.assetPreviewTexture(path, &width, &height);
    if (texture == VK_NULL_HANDLE) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    const ImVec2 imageMin(min.x + 4.0f, min.y + 4.0f);
    const ImVec2 imageMax(max.x - 4.0f, max.y - 17.0f);
    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), imageMin, imageMax);
    dl->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 42), 1.0f);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 15.0f), ImVec2(max.x - 4.0f, max.y - 4.0f), IM_COL32(12, 15, 19, 205), 1.0f);
    const std::string badge = width > 0 && height > 0
        ? "GPU preview " + std::to_string(width) + "x" + std::to_string(height)
        : std::string("GPU preview");
    dl->AddText(ImVec2(min.x + 8.0f, max.y - 15.0f), IM_COL32(160, 210, 255, 255), badge.c_str());
    dl->AddRect(min, max, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(56, 66, 82, 210), EditorUiMetric::cardRounding);
    return true;
}

std::filesystem::path AssetBrowserPanel::generatedPreviewCachePath(const std::filesystem::path& path) const {
    if (cacheRoot_.empty() || !isGeneratedPreviewDiskCacheCandidate(path)) {
        return {};
    }
    const std::filesystem::path canonical = canonicalForCompare(path);
    const std::string keyText = canonical.string() + "|" + std::to_string(pathWriteStamp(path)) + "|" + std::to_string(pathSizeForCache(path));
    return cacheRoot_ / "Editor" / "GeneratedPreviews" / (hex64(fnv1a64(keyText)) + ".json");
}

bool AssetBrowserPanel::loadGeneratedPreviewDiskCache(const std::filesystem::path& path, SourcePreview& preview) const {
    const std::filesystem::path cachePath = generatedPreviewCachePath(path);
    if (cachePath.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec)) {
        return false;
    }
    std::optional<nlohmann::json> json = readJsonFile(cachePath);
    if (!json.has_value() || json->value("schema", std::string()) != "vibode.generatedPreview.v1") {
        return false;
    }
    const std::filesystem::path canonical = canonicalForCompare(path);
    if (json->value("sourcePath", std::string()) != canonical.string() || json->value("writeStamp", int64_t{}) != pathWriteStamp(path) ||
        json->value("sourceSize", uint64_t{}) != static_cast<uint64_t>(pathSizeForCache(path))) {
        return false;
    }

    preview.available = json->value("available", false);
    preview.loadedFromDiskCache = true;
    preview.icon = editorGlyphForPath(path);
    preview.title = json->value("title", path.filename().string());
    preview.kind = json->value("kind", contentKindLabel(path));
    preview.lines.clear();
    if (json->contains("lines") && (*json)["lines"].is_array()) {
        for (const nlohmann::json& line : (*json)["lines"]) {
            if (line.is_string()) {
                preview.lines.push_back(line.get<std::string>());
            }
        }
    }
    return preview.available;
}

void AssetBrowserPanel::saveGeneratedPreviewDiskCache(const std::filesystem::path& path, const SourcePreview& preview) const {
    const std::filesystem::path cachePath = generatedPreviewCachePath(path);
    if (cachePath.empty() || !preview.available) {
        return;
    }
    const std::filesystem::path parent = cachePath.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return;
        }
    }

    nlohmann::json json;
    json["schema"] = "vibode.generatedPreview.v1";
    json["sourcePath"] = canonicalForCompare(path).string();
    json["writeStamp"] = pathWriteStamp(path);
    json["sourceSize"] = static_cast<uint64_t>(pathSizeForCache(path));
    json["available"] = preview.available;
    json["title"] = preview.title;
    json["kind"] = preview.kind;
    json["lines"] = preview.lines;

    std::ofstream file(cachePath, std::ios::trunc);
    if (!file) {
        return;
    }
    file << json.dump(2);
}

AssetBrowserPanel::SourcePreview& AssetBrowserPanel::sourcePreviewForPath(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    const std::string key = ec ? path.string() : absolute.string();
    SourcePreview& preview = sourcePreviewCache_[key];
    if (preview.attempted) {
        return preview;
    }

    preview.attempted = true;
    if (loadGeneratedPreviewDiskCache(path, preview)) {
        return preview;
    }

    auto finishPreview = [&]() -> SourcePreview& {
        saveGeneratedPreviewDiskCache(path, preview);
        return preview;
    };

    preview.available = true;
    preview.icon = editorGlyphForPath(path);
    preview.title = path.filename().string();
    preview.kind = contentKindLabel(path);
    preview.lines.push_back(fileSizeLabel(path));

    if (std::filesystem::is_directory(path, ec)) {
        size_t folders = 0;
        size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                ++folders;
            } else if (entry.is_regular_file(ec)) {
                ++files;
            }
        }
        preview.lines.push_back(countLabel("Folders", folders));
        preview.lines.push_back(countLabel("Files", files));
        return finishPreview();
    }

    if (isModelAssetPath(path)) {
        if (lowerString(path.extension().string()) == ".gltf") {
            if (std::optional<nlohmann::json> json = readJsonFile(path)) {
                preview.lines.push_back(countLabel("Nodes", jsonArraySize(*json, "nodes")));
                preview.lines.push_back(countLabel("Meshes", jsonArraySize(*json, "meshes")));
                preview.lines.push_back(countLabel("Materials", jsonArraySize(*json, "materials")));
                preview.lines.push_back(countLabel("Textures", jsonArraySize(*json, "textures")));
                preview.lines.push_back(countLabel("Cameras", jsonArraySize(*json, "cameras")));
            } else {
                preview.lines.push_back("glTF metadata unavailable");
            }
        } else {
            preview.lines.push_back("Binary/external model preview");
            preview.lines.push_back("Import to generate prefab metadata");
        }
        return finishPreview();
    }

    if (isSceneAssetPath(path)) {
        if (std::optional<nlohmann::json> json = readJsonFile(path)) {
            const size_t entities = json->contains("entities") && (*json)["entities"].is_array() ? (*json)["entities"].size() : 0;
            size_t cameras = 0;
            size_t lights = 0;
            size_t meshes = 0;
            if (json->contains("entities") && (*json)["entities"].is_array()) {
                for (const nlohmann::json& entity : (*json)["entities"]) {
                    if (entity.contains("camera")) ++cameras;
                    if (entity.contains("light") || entity.contains("sun")) ++lights;
                    if (entity.contains("meshRenderer")) ++meshes;
                }
            }
            preview.lines.push_back(countLabel("Entities", entities));
            preview.lines.push_back(countLabel("Cameras", cameras));
            preview.lines.push_back(countLabel("Lights", lights));
            preview.lines.push_back(countLabel("Mesh renderers", meshes));
        } else {
            preview.lines.push_back("Scene metadata unavailable");
        }
        return finishPreview();
    }

    if (isProjectAssetPath(path)) {
        if (std::optional<nlohmann::json> json = readJsonFile(path)) {
            preview.title = json->value("name", preview.title);
            preview.lines.push_back("Project file .vproject");
            preview.lines.push_back("Startup " + json->value("startupScene", std::string("(none)")));
            preview.lines.push_back("Content " + json->value("contentRoot", std::string("Content")));
            preview.lines.push_back("Scenes " + json->value("scenesRoot", std::string("Scenes")));
        } else {
            preview.lines.push_back("Project metadata unavailable");
        }
        return finishPreview();
    }

    if (isMaterialAssetPath(path)) {
        std::ifstream file(path);
        size_t materials = 0;
        size_t textureRefs = 0;
        std::string line;
        while (std::getline(file, line)) {
            const std::string lower = lowerString(line);
            if (lower.rfind("newmtl ", 0) == 0) ++materials;
            if (lower.rfind("map_", 0) == 0) ++textureRefs;
        }
        preview.lines.push_back(countLabel("Materials", materials));
        preview.lines.push_back(countLabel("Texture refs", textureRefs));
        preview.lines.push_back("Swatch preview from source metadata");
        return finishPreview();
    }

    if (isIesAssetPath(path)) {
        std::ifstream file(path);
        std::string firstLine;
        std::getline(file, firstLine);
        preview.lines.push_back(firstLine.empty() ? "IES photometric profile" : firstLine.substr(0, 80));
        preview.lines.push_back("Assignable to authored light profile fields");
        return finishPreview();
    }

    if (isVolumeAssetPath(path)) {
        preview.lines.push_back("OpenVDB volume container");
        preview.lines.push_back("Metadata preview; runtime import pending");
        return finishPreview();
    }

    preview.available = supportedContentPath(path);
    if (!preview.available) {
        preview.lines.push_back("Unsupported file type");
    }
    return finishPreview();
}

bool AssetBrowserPanel::drawGeneratedSourcePreview(const std::filesystem::path& path, ImVec2 min, ImVec2 max) {
    SourcePreview& preview = sourcePreviewForPath(path);
    if (!preview.available) {
        return false;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = contentIconColor(path);
    dl->AddRectFilled(min, max, IM_COL32(16, 18, 22, 255), EditorUiMetric::cardRounding);
    dl->AddRectFilled(ImVec2(min.x + 4.0f, min.y + 4.0f), ImVec2(max.x - 4.0f, min.y + 30.0f), IM_COL32(28, 34, 43, 245), 2.0f);
    editorDrawIconGlyph(preview.icon, ImVec2(min.x + 10.0f, min.y + 8.0f), ImVec2(min.x + 26.0f, min.y + 24.0f), accent);
    dl->AddText(ImVec2(min.x + 32.0f, min.y + 9.0f), IM_COL32(220, 226, 236, 255), preview.kind.c_str());

    const float diagramTop = min.y + 38.0f;
    if (isModelAssetPath(path)) {
        const ImVec2 c(min.x + 42.0f, diagramTop + 32.0f);
        const float s = 28.0f;
        dl->AddRect(ImVec2(c.x - s, c.y - s * 0.55f), ImVec2(c.x + s, c.y + s * 0.55f), accent, 2.0f, 0, 1.5f);
        dl->AddLine(ImVec2(c.x - s, c.y - s * 0.55f), ImVec2(c.x - s * 0.45f, c.y - s), accent, 1.5f);
        dl->AddLine(ImVec2(c.x + s, c.y - s * 0.55f), ImVec2(c.x + s * 0.45f, c.y - s), accent, 1.5f);
        dl->AddLine(ImVec2(c.x - s * 0.45f, c.y - s), ImVec2(c.x + s * 0.45f, c.y - s), accent, 1.5f);
    } else if (isSceneAssetPath(path) || isProjectAssetPath(path)) {
        for (int i = 0; i < 4; ++i) {
            const float x = min.x + 18.0f + static_cast<float>(i % 2) * 42.0f;
            const float y = diagramTop + 8.0f + static_cast<float>(i / 2) * 24.0f;
            dl->AddRect(ImVec2(x, y), ImVec2(x + 28.0f, y + 16.0f), accent, 2.0f, 0, 1.3f);
        }
    } else if (isMaterialAssetPath(path)) {
        dl->AddCircleFilled(ImVec2(min.x + 44.0f, diagramTop + 34.0f), 26.0f, IM_COL32(115, 92, 140, 255));
        dl->AddCircle(ImVec2(min.x + 44.0f, diagramTop + 34.0f), 26.0f, accent, 24, 1.5f);
    } else if (isIesAssetPath(path)) {
        const ImVec2 c(min.x + 48.0f, diagramTop + 42.0f);
        dl->PathLineTo(c);
        for (int i = 0; i <= 12; ++i) {
            const float t = static_cast<float>(i) / 12.0f;
            const float angle = -1.25f + t * 2.5f;
            const float radius = 14.0f + std::sin(t * 3.14159f) * 24.0f;
            dl->PathLineTo(ImVec2(c.x + std::cos(angle) * radius, c.y - std::sin(angle) * radius));
        }
        dl->PathStroke(accent, 0, 1.6f);
    } else if (isVolumeAssetPath(path)) {
        for (int i = 0; i < 4; ++i) {
            dl->AddCircle(ImVec2(min.x + 30.0f + i * 12.0f, diagramTop + 26.0f + (i % 2) * 9.0f), 18.0f, IM_COL32(130, 210, 190, 120), 20, 1.4f);
        }
    }

    float textY = min.y + 38.0f;
    const float textX = min.x + 92.0f;
    const float textMaxX = max.x - 8.0f;
    for (size_t i = 0; i < preview.lines.size() && i < 4; ++i) {
        std::string line = preview.lines[i];
        while (!line.empty() && ImGui::CalcTextSize(line.c_str()).x > textMaxX - textX) {
            line.pop_back();
        }
        if (line.size() < preview.lines[i].size() && line.size() > 3) {
            line.replace(line.size() - 3, 3, "...");
        }
        dl->AddText(ImVec2(textX, textY), i == 0 ? IM_COL32(205, 214, 226, 255) : IM_COL32(142, 151, 164, 255), line.c_str());
        textY += 17.0f;
    }
    dl->AddRect(min, max, IM_COL32(65, 76, 91, 190), EditorUiMetric::cardRounding);
    return true;
}

void AssetBrowserPanel::loadFromPath(const std::filesystem::path& path, EditorRequests& requests) {
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".hdr" || ext == ".exr") {
        requests.loadHdr = path;
        status_ = "Queued HDRI import/apply: " + path.string();
    } else if (ext == ".rtlevel") {
        requests.openScene = path;
        status_ = "Queued scene open: " + path.string();
    } else if (!compatibilityMode_ && isModelAssetPath(path)) {
        requests.importAsset = EditorImportAssetRequest{.sourcePath = path};
        recordImportOperation("Import Asset", path, currentPath_, "ImportAsset");
        status_ = "Queued Import Asset: " + path.string();
    } else {
        requests.importSceneAsNewScene = path;
        status_ = "Queued Import Scene as New Scene: " + path.string();
    }
}

void AssetBrowserPanel::recordImportOperation(
    const std::string& label,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationFolder,
    const std::string& mode,
    const AssetGuid& assetGuid) {
    for (ImportOperation& operation : importOperations_) {
        const bool sameGuid = !assetGuid.empty() && operation.assetGuid == assetGuid;
        const bool sameSource = assetGuid.empty() && samePathForOperation(operation.sourcePath, sourcePath) && operation.mode == mode;
        if ((sameGuid || sameSource) && !operation.completed && !operation.failed) {
            operation.label = label;
            operation.sourcePath = sourcePath;
            operation.destinationFolder = destinationFolder;
            operation.mode = mode;
            operation.progress = std::max(operation.progress, 0.12f);
            operation.state = "Queued";
            return;
        }
    }

    ImportOperation operation;
    operation.id = nextImportOperationId_++;
    operation.label = label;
    operation.sourcePath = sourcePath;
    operation.destinationFolder = destinationFolder;
    operation.mode = mode;
    operation.assetGuid = assetGuid;
    operation.progress = 0.12f;
    operation.state = "Queued";
    importOperations_.insert(importOperations_.begin(), std::move(operation));
    if (importOperations_.size() > 12) {
        importOperations_.resize(12);
    }
}

void AssetBrowserPanel::refreshImportOperations(const EditorRuntimeState& state) {
    if (importOperations_.empty()) {
        return;
    }
    const AssetRegistry* registry = state.assetRegistry;
    for (ImportOperation& operation : importOperations_) {
        if (operation.completed || operation.failed) {
            continue;
        }

        const AssetRecord* matchedRecord = nullptr;
        if (registry != nullptr) {
            for (const AssetRecord& record : registry->records()) {
                if (!operation.assetGuid.empty() && record.guid == operation.assetGuid) {
                    matchedRecord = &record;
                    break;
                }
                const std::filesystem::path recordSource = resolveAssetRecordPath(state, record.sourcePath);
                if (operation.assetGuid.empty() && samePathForOperation(operation.sourcePath, recordSource)) {
                    matchedRecord = &record;
                    break;
                }
            }
        }

        if (matchedRecord != nullptr) {
            operation.progress = assetImportProgress(*matchedRecord);
            operation.state = assetImportProgressLabel(*matchedRecord);
            operation.failed = matchedRecord->status == AssetImportStatus::Failed;
            operation.completed = matchedRecord->status == AssetImportStatus::Imported && !matchedRecord->missing && !matchedRecord->stale;
            if (operation.completed) {
                operation.progress = 1.0f;
                operation.state = "Completed";
            }
        } else {
            operation.progress = std::max(operation.progress, 0.35f);
            operation.state = "Handed off";
        }
    }
}

void AssetBrowserPanel::drawImportOperations() {
    if (importOperations_.empty()) {
        return;
    }

    ImGui::SeparatorText("Import Queue");
    ImGui::TextDisabled("Queued and recent asset import operations");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Completed")) {
        importOperations_.erase(
            std::remove_if(importOperations_.begin(), importOperations_.end(), [](const ImportOperation& operation) {
                return operation.completed || operation.failed;
            }),
            importOperations_.end());
    }

    if (ImGui::BeginTable("ContentImportOperations", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Operation");
        ImGui::TableSetupColumn("Asset");
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::progressColumnWidth);
        ImGui::TableHeadersRow();
        for (const ImportOperation& operation : importOperations_) {
            ImGui::PushID(static_cast<int>(operation.id));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(operation.label.c_str());
            ImGui::TableSetColumnIndex(1);
            const std::string assetName = operation.sourcePath.empty()
                ? operation.assetGuid
                : operation.sourcePath.filename().string();
            ImGui::TextUnformatted(assetName.empty() ? "(asset)" : assetName.c_str());
            if (!operation.destinationFolder.empty()) {
                ImGui::TextDisabled("-> %s", operation.destinationFolder.generic_string().c_str());
            }
            ImGui::TableSetColumnIndex(2);
            if (operation.failed) {
                ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.32f, 1.0f), "%s", operation.state.c_str());
            } else if (operation.completed) {
                ImGui::TextColored(ImVec4(0.48f, 0.82f, 0.55f, 1.0f), "%s", operation.state.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.55f, 0.72f, 0.95f, 1.0f), "%s", operation.state.c_str());
            }
            ImGui::TableSetColumnIndex(3);
            ImGui::ProgressBar(std::clamp(operation.progress, 0.0f, 1.0f), ImVec2(-FLT_MIN, 0.0f), operation.completed ? "Done" : operation.state.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::prepareImportDialog(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationFolder, int mode) {
    setPathBuffer(importSourcePath_, sourcePath);
    std::string destination = "Models";
    if (!destinationFolder.empty()) {
        destination = relativeImportDestination(destinationFolder);
        if (destination.empty()) {
            destination = ".";
        }
    }
    setTextBuffer(importDestinationFolder_, destination);
    importMode_ = mode;
    openImportSettings_ = true;
}

void AssetBrowserPanel::syncBrowserRoot(const EditorRuntimeState& state) {
    std::filesystem::path root;
    std::filesystem::path defaultPath;
    compatibilityMode_ = state.project == nullptr;
    if (state.project != nullptr) {
        root = state.project->projectRoot;
        defaultPath = state.project->contentRoot;
        contentRoot_ = canonicalForCompare(state.project->contentRoot);
        scenesRoot_ = canonicalForCompare(state.project->scenesRoot);
        savedRoot_ = canonicalForCompare(state.project->savedRoot);
        cacheRoot_ = canonicalForCompare(state.project->cacheRoot);
    } else if (state.scenePath != nullptr && state.scenePath->has_value()) {
        root = state.scenePath->value().parent_path();
        defaultPath = root;
        contentRoot_.clear();
        scenesRoot_.clear();
        savedRoot_.clear();
        cacheRoot_.clear();
    } else {
        root = std::filesystem::current_path();
        defaultPath = root;
        contentRoot_.clear();
        scenesRoot_.clear();
        savedRoot_.clear();
        cacheRoot_.clear();
    }

    if (root.empty()) {
        root = std::filesystem::current_path();
    }
    if (defaultPath.empty()) {
        defaultPath = root;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::filesystem::create_directories(defaultPath, ec);
    root = canonicalForCompare(root);
    defaultPath = canonicalForCompare(defaultPath);
    if (browserRoot_ != root) {
        browserRoot_ = root;
        currentPath_ = pathIsWithin(defaultPath, browserRoot_) ? defaultPath : root;
        selectedPath_.clear();
        selectedRecordGuid_.clear();
        backStack_.clear();
        forwardStack_.clear();
    } else if (currentPath_.empty()) {
        currentPath_ = root;
    }
}

void AssetBrowserPanel::navigateTo(const std::filesystem::path& path, bool addHistory) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_directory(path, ec)) {
        return;
    }
    const std::filesystem::path next = canonicalForCompare(path);
    if (next == currentPath_) {
        return;
    }
    if (addHistory && !currentPath_.empty()) {
        backStack_.push_back(currentPath_);
        forwardStack_.clear();
    }
    currentPath_ = next;
    selectedPath_.clear();
    selectedRecordGuid_.clear();
}

bool AssetBrowserPanel::shouldShowPath(const std::filesystem::path& path) const {
    const std::string filter = lowerString(search_.data());
    if (filter.empty()) {
        return true;
    }
    return lowerString(path.filename().string()).find(filter) != std::string::npos;
}

std::string AssetBrowserPanel::relativeContentPath(const std::filesystem::path& path) const {
    if (browserRoot_.empty() || path.empty()) {
        return path.string();
    }
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, browserRoot_, ec);
    return ec ? path.string() : relative.generic_string();
}

std::string AssetBrowserPanel::relativeImportDestination(const std::filesystem::path& path) const {
    if (!contentRoot_.empty() && pathIsWithin(path, contentRoot_)) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(path, contentRoot_, ec);
        return ec ? path.string() : relative.generic_string();
    }
    return relativeContentPath(path);
}

void AssetBrowserPanel::drawPathContextMenu(const std::filesystem::path& path, bool isDirectory, EditorRequests& requests) {
    if (isDirectory) {
        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Open Folder")) {
            navigateTo(path);
        }
        editorGlyphMenuItem(EditorGlyphIcon::Add, "New Folder", false);
        if (editorGlyphMenuItem(EditorGlyphIcon::Import, "Import Here...", !compatibilityMode_)) {
            if (auto source = openGltfFileDialog()) {
                prepareImportDialog(*source, path, 0);
            }
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Import and Place Here...", !compatibilityMode_)) {
            if (auto source = openGltfFileDialog()) {
                prepareImportDialog(*source, path, 1);
            }
        }
        ImGui::Separator();
        if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
            copyPathToClipboard(path);
            status_ = "Copied path: " + path.string();
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer")) {
            revealPathInFileBrowser(path);
        }
        if (editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Refresh")) {
            invalidateThumbnails();
            status_ = "Content refreshed";
        }
        return;
    }

    const bool canOpen = canOpenOrApplyPath(path);
    const bool canImport = !compatibilityMode_ && isModelAssetPath(path);
    if (editorGlyphMenuItem(editorGlyphForPath(path), "Open / Apply", canOpen)) {
        loadFromPath(path, requests);
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Import, "Import Asset...", canImport)) {
        prepareImportDialog(path, currentPath_, 0);
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Add, "Import and Place...", canImport)) {
        prepareImportDialog(path, currentPath_, 1);
    }
    editorGlyphMenuItem(EditorGlyphIcon::Details, "Preview", false);
    editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Reimport", false);
    ImGui::Separator();
    if (editorGlyphMenuItem(EditorGlyphIcon::Command, "Copy Path")) {
        copyPathToClipboard(path);
        status_ = "Copied path: " + path.string();
    }
    if (editorGlyphMenuItem(EditorGlyphIcon::Folder, "Show in Explorer")) {
        revealPathInFileBrowser(path);
    }
    editorGlyphMenuItem(EditorGlyphIcon::Trash, "Delete", false);
}

void AssetBrowserPanel::drawFolderTree(const std::filesystem::path& path, EditorRequests& requests) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return;
    }
    const bool selected = canonicalForCompare(path) == canonicalForCompare(currentPath_);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    bool hasChildren = false;
    for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec)) {
            hasChildren = true;
            break;
        }
    }
    if (!hasChildren) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    ImGui::PushID(path.string().c_str());
    const std::string treeLabel = editorGlyphLabel(path.filename().empty() ? path.string() : path.filename().string());
    editorDrawPreRowBand(EditorUiMetric::contentRowHeight);
    editorPushRowSelectionStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, editorRowFramePadding(EditorUiMetric::contentRowHeight));
    const bool open = ImGui::TreeNodeEx(treeLabel.c_str(), flags);
    ImGui::PopStyleVar();
    editorPopRowSelectionStyle();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() + 2.0f;
    const float iconY = rowMin.y + std::max(0.0f, (rowMax.y - rowMin.y - 16.0f) * 0.5f);
    drawContentGlyph(path, ImVec2(iconX, iconY), ImVec2(iconX + 16.0f, iconY + 16.0f));
    if (ImGui::IsItemClicked()) {
        navigateTo(path);
    }
    if (ImGui::BeginPopupContextItem("FolderContext")) {
        selectedPath_ = path;
        selectedRecordGuid_.clear();
        drawPathContextMenu(path, true, requests);
        ImGui::EndPopup();
    }
    if (open) {
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory(ec)) {
                children.push_back(entry.path());
            }
        }
        std::sort(children.begin(), children.end());
        for (const auto& child : children) {
            drawFolderTree(child, requests);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::drawPathList(const EditorRuntimeState& state, EditorRequests& requests) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    if (!currentPath_.empty() && std::filesystem::is_directory(currentPath_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(currentPath_, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (shouldShowPath(entry.path())) {
                entries.push_back(entry);
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        std::error_code errorA;
        std::error_code errorB;
        const bool aDir = a.is_directory(errorA);
        const bool bDir = b.is_directory(errorB);
        if (aDir != bDir) {
            return aDir > bDir;
        }
        return lowerString(a.path().filename().string()) < lowerString(b.path().filename().string());
    });

    if (gridView_) {
        const float cellWidth = EditorUiMetric::contentGridCellWidth;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth));
        ImGui::Columns(columns, "ContentGrid", false);
        for (const auto& entry : entries) {
            const std::filesystem::path path = entry.path();
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            const ImVec2 thumbSize(EditorUiMetric::contentGridThumbWidth, EditorUiMetric::contentGridThumbHeight);
            ImGui::InvisibleButton("ContentGridThumb", thumbSize);
            const ImVec2 thumbMin = ImGui::GetItemRectMin();
            const ImVec2 thumbMax = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(thumbMin, thumbMax, selected ? ImGui::GetColorU32(editorSelectedRowColor()) : IM_COL32(24, 27, 32, 255), EditorUiMetric::cardRounding);
            dl->AddRect(thumbMin, thumbMax, selected ? ImGui::GetColorU32(editorActiveRowColor()) : IM_COL32(54, 62, 72, 255), EditorUiMetric::cardRounding);
            if (!drawGpuSceneTextureThumbnail(state, path, thumbMin, thumbMax) &&
                !drawStandaloneGpuAssetPreview(state, path, thumbMin, thumbMax, selected) &&
                !drawRasterThumbnail(path, thumbMin, thumbMax, selected)) {
                drawContentGlyph(
                    path,
                    ImVec2(thumbMin.x + thumbSize.x * 0.34f, thumbMin.y + thumbSize.y * 0.22f),
                    ImVec2(thumbMax.x - thumbSize.x * 0.34f, thumbMax.y - thumbSize.y * 0.22f));
            }
            if (ImGui::IsItemClicked()) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.is_directory(ec)) {
                    navigateTo(path);
                } else {
                    loadFromPath(path, requests);
                }
            }
            if (ImGui::BeginPopupContextItem("PathContext")) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                drawPathContextMenu(path, entry.is_directory(ec), requests);
                ImGui::EndPopup();
            }
            ImGui::TextWrapped("%s%s", selected ? "> " : "", path.filename().string().c_str());
            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 0.0f));
    if (ImGui::BeginTable("ContentPathListCompact", 1, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Asset");
        for (const auto& entry : entries) {
            const std::filesystem::path path = entry.path();
            const bool isDir = entry.is_directory(ec);
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const ImVec2 nameCursor = ImGui::GetCursorScreenPos();
            const std::string name = editorGlyphLabel(path.filename().string());
            editorPushRowSelectionStyle();
            if (ImGui::Selectable(
                    name.c_str(),
                    selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2(0.0f, EditorUiMetric::contentRowHeight))) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (isDir) {
                        navigateTo(path);
                    } else {
                        loadFromPath(path, requests);
                    }
                }
            }
            editorPopRowSelectionStyle();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float iconY = nameCursor.y + std::max(0.0f, (itemMax.y - nameCursor.y - 16.0f) * 0.5f);
            drawContentGlyph(path, ImVec2(nameCursor.x + 2.0f, iconY), ImVec2(nameCursor.x + 18.0f, iconY + 16.0f));
            if (ImGui::BeginPopupContextItem("PathContext")) {
                selectedPath_ = path;
                selectedRecordGuid_.clear();
                drawPathContextMenu(path, isDir, requests);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void AssetBrowserPanel::drawRegistryTable(const EditorRuntimeState& state, EditorRequests& requests) {
    if (state.assetRegistry == nullptr) {
        return;
    }
    const AssetRegistry& registry = *state.assetRegistry;
    ImGui::SeparatorText("Asset Registry");
    ImGui::Text("Registry: %s%s",
        registry.state().path.empty() ? "(none)" : registry.state().path.string().c_str(),
        registry.dirty() ? " *" : "");
    const auto& records = registry.records();
    if (records.empty()) {
        ImGui::TextDisabled("No registry records yet. Import Asset will populate this in the next milestone.");
        return;
    }
    if (ImGui::BeginTable("AssetRegistryRecords", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("GUID");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Imported");
        ImGui::TableSetupColumn("Deps");
        ImGui::TableSetupColumn("Refs");
        ImGui::TableSetupColumn("Missing/Stale");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, EditorUiMetric::progressColumnWidth);
        ImGui::TableHeadersRow();
        for (const AssetRecord& record : records) {
            ImGui::PushID(record.guid.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(assetTypeName(record.type));
            ImGui::TableSetColumnIndex(1);
            const char* name = record.displayName.empty() ? "(unnamed)" : record.displayName.c_str();
            editorPushRowSelectionStyle();
            if (ImGui::Selectable(name, selectedRecordGuid_ == record.guid, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, EditorUiMetric::contentRowHeight))) {
                selectedRecordGuid_ = record.guid;
                selectedPath_.clear();
            }
            editorPopRowSelectionStyle();
            if (record.type == AssetType::Prefab && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                std::array<char, 128> guidPayload{};
                record.guid.copy(guidPayload.data(), std::min(record.guid.size(), guidPayload.size() - 1));
                ImGui::SetDragDropPayload("PREFAB_ASSET", guidPayload.data(), guidPayload.size());
                ImGui::Text("Prefab %s", name);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem()) {
                if (record.type == AssetType::Prefab && editorGlyphMenuItem(EditorGlyphIcon::Add, "Place Prefab")) {
                    requests.placeAsset = record.guid;
                }
                const bool canReimport = !record.sourcePath.empty() && std::filesystem::exists(record.sourcePath);
                if (editorGlyphMenuItem(EditorGlyphIcon::Refresh, "Reimport", canReimport)) {
                    requests.reimportAsset = record.guid;
                    recordImportOperation("Reimport Asset", resolveAssetRecordPath(state, record.sourcePath), {}, "Reimport", record.guid);
                    status_ = "Queued reimport: " + record.displayName;
                }
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(record.guid.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(record.sourcePath.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(record.importedPath.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%zu", record.dependencies.size());
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%zu", record.references.size());
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%s%s", record.missing ? "missing" : "ok", record.stale ? " / stale" : "");
            ImGui::TableSetColumnIndex(8);
            ImGui::TextUnformatted(assetImportStatusName(record.status));
            ImGui::TableSetColumnIndex(9);
            ImGui::ProgressBar(assetImportProgress(record), ImVec2(-FLT_MIN, 0.0f), assetImportProgressLabel(record));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::drawDetails(const EditorRuntimeState& state, EditorRequests& requests) {
    ImGui::SeparatorText("Details");
    if (!selectedPath_.empty()) {
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewWidth = std::min(ImGui::GetContentRegionAvail().x, EditorUiMetric::assetPreviewMaxWidth);
        const ImVec2 previewSize(previewWidth, EditorUiMetric::assetPreviewHeight);
        ImGui::InvisibleButton("AssetPreview", previewSize);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(20, 23, 27, 255), 4.0f);
        drawList->AddRect(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(55, 62, 72, 255), 4.0f);
        const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
        const bool hasGpuPreview = drawGpuSceneTextureThumbnail(state, selectedPath_, previewPos, previewMax);
        const bool hasStandaloneGpuPreview = hasGpuPreview ? false : drawStandaloneGpuAssetPreview(state, selectedPath_, previewPos, previewMax, false);
        const bool hasRasterPreview = (hasGpuPreview || hasStandaloneGpuPreview) ? false : drawRasterThumbnail(selectedPath_, previewPos, previewMax, false);
        if (!hasGpuPreview && !hasStandaloneGpuPreview && !hasRasterPreview) {
            const bool hasGeneratedPreview = drawGeneratedSourcePreview(selectedPath_, previewPos, previewMax);
            if (!hasGeneratedPreview) {
                const ImVec2 previewIconSize(34.0f, 34.0f);
                drawContentGlyph(
                    selectedPath_,
                    ImVec2(previewPos.x + previewSize.x * 0.5f - previewIconSize.x * 0.5f, previewPos.y + 28.0f),
                    ImVec2(previewPos.x + previewSize.x * 0.5f + previewIconSize.x * 0.5f, previewPos.y + 28.0f + previewIconSize.y));
                const std::string previewKind = contentKindLabel(selectedPath_);
                const ImVec2 kindSize = ImGui::CalcTextSize(previewKind.c_str());
                drawList->AddText(
                    ImVec2(previewPos.x + previewSize.x * 0.5f - kindSize.x * 0.5f, previewPos.y + 64.0f),
                    IM_COL32(130, 137, 148, 255),
                    previewKind.c_str());
            }
        }
        ImGui::Text("Selected: %s", selectedPath_.filename().string().c_str());
        ImGui::Text("Kind: %s", contentKindLabel(selectedPath_).c_str());
        ImGui::TextWrapped("Path: %s", relativeContentPath(selectedPath_).c_str());
        const bool isDirectory = std::filesystem::is_directory(selectedPath_);
        if (isDirectory) {
            if (contentActionButton("OpenFolder", EditorGlyphIcon::Folder, "Open Folder", "Open this folder in Content")) {
                navigateTo(selectedPath_);
            }
            ImGui::SameLine();
            if (compatibilityMode_) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ImportHere", EditorGlyphIcon::Import, "Import Here", "Import a model asset into this folder")) {
                if (auto source = openGltfFileDialog()) {
                    prepareImportDialog(*source, selectedPath_, 0);
                }
            }
            if (compatibilityMode_) {
                ImGui::EndDisabled();
            }
        } else {
            const bool canOpen = canOpenOrApplyPath(selectedPath_);
            if (!canOpen) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("OpenApply", EditorGlyphIcon::File, "Open / Apply", "Open the selected asset or apply it to the scene")) {
                loadFromPath(selectedPath_, requests);
            }
            if (!canOpen) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            const bool canImport = !compatibilityMode_ && isModelAssetPath(selectedPath_);
            if (!canImport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ImportAsset", EditorGlyphIcon::Import, "Import Asset", "Import this model into the project asset registry")) {
                prepareImportDialog(selectedPath_, currentPath_, 0);
            }
            ImGui::SameLine();
            if (contentActionButton("PlaceAsset", EditorGlyphIcon::Add, "Place", "Import and place this model in the current scene")) {
                prepareImportDialog(selectedPath_, currentPath_, 1);
            }
            if (!canImport) {
                ImGui::EndDisabled();
            }
        }
        if (contentActionButton("CopyPath", EditorGlyphIcon::Command, "Copy Path", "Copy the selected asset path to the clipboard")) {
            copyPathToClipboard(selectedPath_);
            status_ = "Copied path: " + selectedPath_.string();
        }
        ImGui::SameLine();
        if (contentActionButton("ShowInExplorer", EditorGlyphIcon::Folder, "Show in Explorer", "Reveal the selected asset in Explorer")) {
            revealPathInFileBrowser(selectedPath_);
        }
        if (!supportedContentPath(selectedPath_)) {
            ImGui::TextDisabled("No supported files selected");
        }
    } else if (selectedRecordGuid_.empty()) {
        ImGui::TextDisabled("No supported files selected");
    }
    const bool hasDetailsSelection = !selectedPath_.empty() || !selectedRecordGuid_.empty();
    if (!selectedRecordGuid_.empty() && state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.guid != selectedRecordGuid_) {
                continue;
            }
            std::filesystem::path recordThumbnail = resolveAssetRecordPath(state, record.thumbnailPath);
            if (recordThumbnail.empty() && (record.type == AssetType::Texture || record.type == AssetType::HDRI)) {
                recordThumbnail = resolveAssetRecordPath(state, record.importedPath.empty() ? record.sourcePath : record.importedPath);
            }
            std::filesystem::path recordPreviewSource = recordThumbnail;
            if (recordPreviewSource.empty()) {
                recordPreviewSource = resolveAssetRecordPath(state, record.importedPath.empty() ? record.sourcePath : record.importedPath);
            }
            if (!recordPreviewSource.empty()) {
                const ImVec2 previewPos = ImGui::GetCursorScreenPos();
                const float previewWidth = std::min(ImGui::GetContentRegionAvail().x, EditorUiMetric::assetPreviewMaxWidth);
                const ImVec2 previewSize(previewWidth, EditorUiMetric::assetPreviewHeight);
                ImGui::InvisibleButton("AssetRecordPreview", previewSize);
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(20, 23, 27, 255), 4.0f);
                drawList->AddRect(previewPos, ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y), IM_COL32(55, 62, 72, 255), 4.0f);
                const ImVec2 previewMax(previewPos.x + previewSize.x, previewPos.y + previewSize.y);
                if (!drawGpuSceneTextureThumbnail(state, recordPreviewSource, previewPos, previewMax) &&
                    !drawStandaloneGpuAssetPreview(state, recordPreviewSource, previewPos, previewMax, false) &&
                    !drawRasterThumbnail(recordPreviewSource, previewPos, previewMax, false) &&
                    !drawGeneratedSourcePreview(recordPreviewSource, previewPos, previewMax)) {
                    const ImVec2 previewIconSize(34.0f, 34.0f);
                    editorDrawIconGlyph(
                        record.type == AssetType::HDRI ? EditorGlyphIcon::Environment : EditorGlyphIcon::Texture,
                        ImVec2(previewPos.x + previewSize.x * 0.5f - previewIconSize.x * 0.5f, previewPos.y + 32.0f),
                        ImVec2(previewPos.x + previewSize.x * 0.5f + previewIconSize.x * 0.5f, previewPos.y + 32.0f + previewIconSize.y),
                        IM_COL32(105, 170, 230, 255));
                    drawList->AddText(ImVec2(previewPos.x + 10.0f, previewPos.y + previewSize.y - 22.0f), IM_COL32(130, 137, 148, 255), "Thumbnail unavailable");
                }
            }
            ImGui::Text("Asset: %s", record.displayName.empty() ? "(unnamed)" : record.displayName.c_str());
            ImGui::Text("GUID: %s", record.guid.c_str());
            ImGui::Text("Type: %s", assetTypeName(record.type));
            ImGui::TextWrapped("Thumbnail: %s", record.thumbnailPath.empty() ? "(none)" : record.thumbnailPath.c_str());
            ImGui::TextWrapped("Source: %s", record.sourcePath.c_str());
            ImGui::TextWrapped("Imported: %s", record.importedPath.c_str());
            ImGui::TextWrapped("Cache: %s", record.cachePath.c_str());
            ImGui::Text("Dependencies: %zu", record.dependencies.size());
            ImGui::Text("References: %zu", record.references.size());
            ImGui::Text("Status: %s%s%s", assetImportStatusName(record.status), record.missing ? " missing" : "", record.stale ? " stale" : "");
            ImGui::ProgressBar(assetImportProgress(record), ImVec2(-FLT_MIN, 0.0f), assetImportProgressLabel(record));
            const bool canReimport = !record.sourcePath.empty() && std::filesystem::exists(record.sourcePath);
            if (!canReimport) {
                ImGui::BeginDisabled();
            }
            if (contentActionButton("ReimportRecord", EditorGlyphIcon::Refresh, "Reimport", "Queue this asset for reimport")) {
                requests.reimportAsset = record.guid;
                recordImportOperation("Reimport Asset", resolveAssetRecordPath(state, record.sourcePath), {}, "Reimport", record.guid);
                status_ = "Queued reimport: " + record.displayName;
            }
            if (!canReimport) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("Source unavailable");
            }
            if (record.type == AssetType::Prefab) {
                ImGui::SameLine();
                if (contentActionButton("PlacePrefab", EditorGlyphIcon::Add, "Place Prefab", "Place this prefab in the current scene")) {
                    requests.placeAsset = record.guid;
                }
            }
            break;
        }
    }
    if (hasDetailsSelection) {
        ImGui::SeparatorText("Context");
        if (state.project != nullptr) {
            ImGui::Text("Project: %s", state.project->name.c_str());
            ImGui::TextWrapped("Content Root: %s", state.project->contentRoot.string().c_str());
        } else {
            ImGui::TextDisabled("Project: none");
        }
        ImGui::Text("Current Folder: %s", currentPath_.empty() ? "(none)" : relativeContentPath(currentPath_).c_str());
    }
}

void AssetBrowserPanel::drawImportSettingsDialog(EditorRequests& requests) {
    if (openImportSettings_) {
        ImGui::OpenPopup("Import Settings");
        openImportSettings_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const char* modes[] = {"Import Asset", "Import and Place"};
    ImGui::Combo("Mode", &importMode_, modes, IM_ARRAYSIZE(modes));
    ImGui::InputText("Source", importSourcePath_.data(), importSourcePath_.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::InputTextWithHint("Destination Folder", "Models", importDestinationFolder_.data(), importDestinationFolder_.size());
    ImGui::SeparatorText("Hierarchy");
    ImGui::Checkbox("Preserve hierarchy", &importSettings_.preserveHierarchy);
    ImGui::Checkbox("Import materials", &importSettings_.importMaterials);
    ImGui::Checkbox("Import textures", &importSettings_.importTextures);
    ImGui::Checkbox("Import cameras", &importSettings_.importCameras);
    ImGui::Checkbox("Import lights", &importSettings_.importLights);
    ImGui::SeparatorText("Geometry / Cache");
    ImGui::Checkbox("Generate tangents", &importSettings_.generateTangents);
    ImGui::Checkbox("Build BLAS cache", &importSettings_.buildBlasCache);
    ImGui::InputFloat("Unit scale", &importSettings_.unitScale, 0.1f, 1.0f, "%.3f");
    static int coordinateMode = 0;
    const char* coordinateModes[] = {"None", "glTF Y-Up to Engine", "Z-Up to Engine"};
    if (ImGui::Combo("Coordinate conversion", &coordinateMode, coordinateModes, IM_ARRAYSIZE(coordinateModes))) {
        importSettings_.coordinateConversion = coordinateModes[coordinateMode];
    }

    if (ImGui::Button("Import")) {
        EditorImportAssetRequest request;
        request.sourcePath = std::filesystem::path(importSourcePath_.data());
        request.destinationFolder = std::filesystem::path(importDestinationFolder_.data());
        request.mode = importMode_ == 0 ? "ImportAsset" : "ImportAndPlace";
        request.settings = importSettings_;
        if (importMode_ == 0) {
            requests.importAsset = std::move(request);
            recordImportOperation("Import Asset", requests.importAsset->sourcePath, requests.importAsset->destinationFolder, requests.importAsset->mode);
            status_ = "Queued non-mutating Import Asset: " + requests.importAsset->sourcePath.string();
        } else {
            recordImportOperation("Import and Place", request.sourcePath, request.destinationFolder, request.mode);
            const std::filesystem::path sourcePath = request.sourcePath;
            requests.importAndPlace = std::move(request);
            status_ = "Queued Import and Place: " + sourcePath.string();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        status_ = "Import Asset cancelled";
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AssetBrowserPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    (void)selection;
    if (!ImGui::Begin(EditorDockWindowTitle::Content)) {
        ImGui::End();
        return;
    }

    syncBrowserRoot(state);
    refreshImportOperations(state);

    ImGui::BeginGroup();
    if (editorIconButton("ContentAdd", EditorGlyphIcon::Add, false)) {
        ImGui::OpenPopup("ContentAddMenu");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Add or import content");
    }
    if (ImGui::BeginPopup("ContentAddMenu")) {
        ImGui::MenuItem("New Folder", nullptr, false, false);
        if (ImGui::MenuItem("New Scene", nullptr, false, true)) {
            requests.newScene = true;
        }
        ImGui::MenuItem("New Material", nullptr, false, false);
        ImGui::Separator();
        const bool canImportAssets = !compatibilityMode_;
        if (ImGui::MenuItem("Import Asset...", nullptr, false, canImportAssets)) {
            if (auto path = openGltfFileDialog()) {
                prepareImportDialog(*path, {}, 0);
            }
        }
        if (ImGui::MenuItem("Import Into Scene...", nullptr, false, canImportAssets)) {
            if (auto path = openGltfFileDialog()) {
                prepareImportDialog(*path, {}, 1);
            }
        }
        ImGui::MenuItem("Import Texture...", nullptr, false, false);
        if (ImGui::MenuItem("Import HDRI...")) {
            if (auto path = openHdrFileDialog()) {
                requests.loadHdr = *path;
                status_ = "Queued HDRI import/apply: " + path->string();
            }
        }
        ImGui::MenuItem("Import IES Profile...", nullptr, false, false);
        ImGui::MenuItem("Browse Filesystem...", nullptr, false, false);
        if (compatibilityMode_) {
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project Manager")) {
                requests.showProjectManager = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##contentFilter", "Filter in selected folder...", search_.data(), search_.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(backStack_.empty());
    if (editorIconButton("ContentBack", EditorGlyphIcon::Back, false)) {
        forwardStack_.push_back(currentPath_);
        const std::filesystem::path previous = backStack_.back();
        backStack_.pop_back();
        navigateTo(previous, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Back");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(forwardStack_.empty());
    if (editorIconButton("ContentForward", EditorGlyphIcon::Forward, false)) {
        backStack_.push_back(currentPath_);
        const std::filesystem::path next = forwardStack_.back();
        forwardStack_.pop_back();
        navigateTo(next, false);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Forward");
    }
    showDetails_ = true;

    if (!browserRoot_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::PushID("ContentBreadcrumb");
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(currentPath_, browserRoot_, relativeError);
        const std::string rootLabel = compatibilityMode_ ? compactPathLabel(browserRoot_, "Workspace") : "Project";
        if (ImGui::SmallButton(rootLabel.c_str())) {
            navigateTo(browserRoot_);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", browserRoot_.string().c_str());
        }
        std::filesystem::path accum = browserRoot_;
        for (const auto& part : relativeError ? std::filesystem::path{} : relative) {
            const std::string partString = part.string();
            if (partString == "." || partString.empty()) {
                continue;
            }
            accum /= part;
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
            ImGui::PushID(partString.c_str());
            if (ImGui::SmallButton(partString.c_str())) {
                navigateTo(accum);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndGroup();
    drawImportSettingsDialog(requests);

    const std::string sceneLoadStatus = state.sceneLoadingStatus != nullptr ? *state.sceneLoadingStatus : std::string{};
    const bool hasSceneLoadStatus = !sceneLoadStatus.empty();
    const bool sceneLoadCompleted = !state.sceneLoadRunning && hasSceneLoadStatus && sceneLoadStatusIsSuccessfulCompletion(sceneLoadStatus);
    const bool showSceneLoadBanner = state.sceneLoadRunning || (hasSceneLoadStatus && !sceneLoadCompleted);
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }
    if (showSceneLoadBanner) {
        const float progress = std::clamp(state.sceneLoadProgress, 0.0f, 1.0f);
        ImGui::ProgressBar(progress, ImVec2(std::min(360.0f, ImGui::GetContentRegionAvail().x), 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.sceneLoadRunning ? "Import / load in progress" : "Last import / load status");
        ImGui::TextWrapped("%s", sceneLoadStatus.c_str());
    }

    const float browserHeight = ImGui::GetContentRegionAvail().y;
    if (browserHeight > ImGui::GetTextLineHeightWithSpacing()) {
        const float browserWidth = ImGui::GetContentRegionAvail().x;
        const float sectionSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float treeWidth = std::clamp(
            browserWidth * EditorUiMetric::contentTreePanelRatio,
            EditorUiMetric::contentTreeMinWidth,
            EditorUiMetric::contentTreeMaxWidth);
        float detailsWidth = showDetails_
            ? std::clamp(
                browserWidth * EditorUiMetric::contentDetailsPanelRatio,
                EditorUiMetric::contentDetailsMinWidth,
                EditorUiMetric::contentDetailsMaxWidth)
            : 0.0f;
        if (showDetails_) {
            const float maxDetailsWidth = browserWidth - treeWidth - EditorUiMetric::contentListMinWidth - (sectionSpacing * 2.0f);
            detailsWidth = std::max(0.0f, std::min(detailsWidth, maxDetailsWidth));
        }
        ImGui::BeginChild("ContentFolders", ImVec2(treeWidth, 0.0f), true);
        if (!browserRoot_.empty()) {
            drawFolderTree(browserRoot_, requests);
        }
        if (state.editorPrefs != nullptr) {
            auto& prefs = *state.editorPrefs;
            if (!prefs.favoriteFiles.empty() && ImGui::TreeNodeEx("Favorites", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.favoriteFiles.size(); ++i) {
                    const std::filesystem::path favPath(prefs.favoriteFiles[i]);
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(favPath.filename().string().c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedPath_ = favPath;
                        selectedRecordGuid_.clear();
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            loadFromPath(favPath, requests);
                        }
                    }
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Load")) {
                            loadFromPath(favPath, requests);
                        }
                        if (ImGui::MenuItem("Remove from Favorites")) {
                            requests.removeFavorite = prefs.favoriteFiles[i];
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            if (!prefs.recentFiles.empty() && ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.recentFiles.size(); ++i) {
                    const std::filesystem::path recPath(prefs.recentFiles[i]);
                    ImGui::PushID(static_cast<int>(i + 1000));
                    if (ImGui::Selectable(recPath.filename().string().c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selectedPath_ = recPath;
                        selectedRecordGuid_.clear();
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            loadFromPath(recPath, requests);
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("ContentItems", ImVec2(-(detailsWidth + (showDetails_ ? sectionSpacing : 0.0f)), 0.0f), true);
        drawPathList(state, requests);
        ImGui::EndChild();
        if (showDetails_) {
            ImGui::SameLine();
            ImGui::BeginChild("ContentDetails", ImVec2(detailsWidth, 0.0f), true);
            drawDetails(state, requests);
            if (state.editorPrefs != nullptr && !selectedPath_.empty()) {
                if (ImGui::SmallButton("Add Selected to Favorites")) {
                    state.editorPrefs->addFavorite(selectedPath_);
                }
            }
            ImGui::EndChild();
        }
    }

    ImGui::End();
}

} // namespace rtv
