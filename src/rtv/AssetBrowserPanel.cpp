#include "rtv/AssetBrowserPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/EditorPreferences.h"
#include "rtv/FileDialog.h"
#include "rtv/GpuScene.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <filesystem>
#include <algorithm>
#include <cstring>
#include <string>

namespace rtv {

namespace {

void setPathBuffer(std::array<char, 512>& buffer, const std::filesystem::path& path) {
    const std::string value = path.string();
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
}

} // namespace

void AssetBrowserPanel::invalidateThumbnails() {
    thumbnailCache_.clear();
}

void AssetBrowserPanel::loadFromPath(const std::filesystem::path& path, EditorRequests& requests) {
    std::string ext = path.extension().string();
    for (char& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (ext == ".hdr" || ext == ".exr") {
        requests.loadHdr = path;
        status_ = "Queued HDRI import/apply: " + path.string();
    } else if (ext == ".rtlevel") {
        requests.openScene = path;
        status_ = "Queued scene open: " + path.string();
    } else {
        requests.importSceneAsNewScene = path;
        status_ = "Queued Import Scene as New Scene: " + path.string();
    }
}

void AssetBrowserPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin("Content")) {
        ImGui::End();
        return;
    }

    if (state.gltfPath != nullptr && state.gltfPath->has_value() && gltfPath_[0] == '\0') {
        const std::string path = state.gltfPath->value().string();
        path.copy(gltfPath_.data(), std::min(path.size(), gltfPath_.size() - 1));
    }
    if (state.hdrPath != nullptr && state.hdrPath->has_value() && hdrPath_[0] == '\0') {
        const std::string path = state.hdrPath->value().string();
        path.copy(hdrPath_.data(), std::min(path.size(), hdrPath_.size() - 1));
    }

    ImGui::BeginGroup();
    if (ImGui::Button("Add/Import")) {
        if (auto path = openGltfFileDialog()) {
            setPathBuffer(gltfPath_, *path);
            requests.importAsset = *path;
            status_ = "Queued non-mutating Import Asset: " + path->string();
        }
    }
    ImGui::SameLine();
    static char filter[128]{};
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##contentFilter", "Search Content", filter, sizeof(filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("<")) {}
    ImGui::SameLine();
    if (ImGui::SmallButton(">")) {}
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        invalidateThumbnails();
        status_ = "Content refreshed";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("/ Content");
    ImGui::EndGroup();

    ImGui::SeparatorText("Scene Import / Compatibility");
    ImGui::TextUnformatted("Import Scene as New Scene");
    ImGui::PushItemWidth(-190.0f);
    ImGui::InputTextWithHint("##gltfPath", "C:\\path\\to\\scene.glb", gltfPath_.data(), gltfPath_.size());
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##gltf")) {
        if (auto path = openGltfFileDialog()) {
            setPathBuffer(gltfPath_, *path);
            requests.importSceneAsNewScene = *path;
            status_ = "Queued Import Scene as New Scene: " + path->string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import Scene as New Scene")) {
        std::filesystem::path path{gltfPath_.data()};
        if (!path.empty()) {
            requests.importSceneAsNewScene = path;
            status_ = "Queued Import Scene as New Scene: " + path.string();
        }
    }

    ImGui::TextUnformatted("Environment");
    ImGui::PushItemWidth(-190.0f);
    ImGui::InputTextWithHint("##hdrPath", "C:\\path\\to\\environment.hdr", hdrPath_.data(), hdrPath_.size());
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##hdr")) {
        if (auto path = openHdrFileDialog()) {
            setPathBuffer(hdrPath_, *path);
            requests.loadHdr = *path;
            status_ = "Queued HDR load: " + path->string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Import HDRI")) {
        std::filesystem::path path{hdrPath_.data()};
        if (!path.empty()) {
            requests.loadHdr = path;
            status_ = "Queued HDRI import/apply: " + path.string();
        }
    }

    if (!status_.empty()) {
        ImGui::TextWrapped("%s", status_.c_str());
    }
    if (state.sceneLoadingStatus != nullptr && !state.sceneLoadingStatus->empty()) {
        ImGui::TextWrapped("%s", state.sceneLoadingStatus->c_str());
    }

    ImGui::SeparatorText("Scene Document");
    ImGui::PushItemWidth(-260.0f);
    ImGui::InputTextWithHint("##sceneJsonPath", "C:\\path\\to\\scene.rtlevel", scenePath_.data(), scenePath_.size());
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##sceneJson")) {
        if (auto path = openSceneJsonFileDialog()) {
            setPathBuffer(scenePath_, *path);
            requests.openScene = *path;
            status_ = "Queued scene open: " + path->string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Scene")) {
        std::filesystem::path path{scenePath_.data()};
        if (path.empty()) {
            if (state.scenePath != nullptr && state.scenePath->has_value()) {
                path = **state.scenePath;
                setPathBuffer(scenePath_, path);
            }
        }
        if (path.empty()) {
            if (auto selected = saveSceneJsonFileDialog()) {
                path = *selected;
                setPathBuffer(scenePath_, path);
            }
        }
        if (!path.empty()) {
            requests.saveScene = path;
            status_ = "Queued scene save: " + path.string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Scene")) {
        std::filesystem::path path{scenePath_.data()};
        if (!path.empty()) {
            requests.openScene = path;
            status_ = "Queued scene open: " + path.string();
        }
    }

    if (state.editorPrefs != nullptr) {
        auto& prefs = *state.editorPrefs;

        if (!prefs.favoriteFiles.empty()) {
            if (ImGui::TreeNodeEx("Favorites", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.favoriteFiles.size(); ++i) {
                    const auto& fav = prefs.favoriteFiles[i];
                    const std::filesystem::path favPath(fav);
                    ImGui::PushID(static_cast<int>(i));
                    const std::string label = favPath.filename().string();
                    if (ImGui::Selectable(label.c_str(), false)) {
                        status_ = "Loading favorite: " + fav;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", fav.c_str());
                    }
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Load")) {
                            loadFromPath(favPath, requests);
                        }
                        if (ImGui::MenuItem("Remove from Favorites")) {
                            requests.removeFavorite = fav;
                        }
                        ImGui::EndPopup();
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        loadFromPath(favPath, requests);
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        if (!prefs.recentFiles.empty()) {
            if (ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (size_t i = 0; i < prefs.recentFiles.size(); ++i) {
                    const auto& rec = prefs.recentFiles[i];
                    const std::filesystem::path recPath(rec);
                    ImGui::PushID(static_cast<int>(i + 1000));
                    if (ImGui::Selectable(recPath.filename().string().c_str(), false)) {
                        loadFromPath(recPath, requests);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", rec.c_str());
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }

        if (ImGui::SmallButton("Add Current to Favorites")) {
            if (state.gltfPath != nullptr && state.gltfPath->has_value()) {
                prefs.addFavorite(state.gltfPath->value());
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add Current Environment to Favorites")) {
            if (state.hdrPath != nullptr && state.hdrPath->has_value()) {
                prefs.addFavorite(state.hdrPath->value());
            }
        }
    }

    ImGui::SeparatorText("Details");
    if (state.project != nullptr) {
        ImGui::Text("Project: %s", state.project->name.c_str());
        ImGui::Text("Content Root: %s", state.project->contentRoot.string().c_str());
    } else {
        ImGui::TextDisabled("No project open. Content is in compatibility mode.");
    }
    ImGui::Text("Scene: %s", state.scenePath != nullptr && state.scenePath->has_value() ? state.scenePath->value().string().c_str() : "Untitled / not saved");
    ImGui::Text("Source scene import: %s", state.gltfPath != nullptr && state.gltfPath->has_value() ? state.gltfPath->value().string().c_str() : "(none)");
    ImGui::Text("HDRI: %s", state.hdrPath != nullptr && state.hdrPath->has_value() ? state.hdrPath->value().string().c_str() : "(procedural)");

    if (state.assetRegistry != nullptr) {
        const AssetRegistry& registry = *state.assetRegistry;
        ImGui::SeparatorText("Asset Registry");
        ImGui::Text("Registry: %s%s",
            registry.state().path.empty() ? "(none)" : registry.state().path.string().c_str(),
            registry.dirty() ? " *" : "");
        const auto& records = registry.records();
        if (records.empty()) {
            ImGui::TextDisabled("No registry records yet. Import Asset will populate this in the next milestone.");
        } else if (ImGui::BeginTable("AssetRegistryRecords", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("GUID");
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();
            for (const AssetRecord& record : records) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(assetTypeName(record.type));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(record.displayName.empty() ? "(unnamed)" : record.displayName.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(record.guid.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(record.sourcePath.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(assetImportStatusName(record.status));
            }
            ImGui::EndTable();
        }
    }

    if (state.importedScene != nullptr) {
        ImGui::Text("Scene nodes: %zu", state.importedScene->nodes.size());
    }

    if (state.assets != nullptr) {
        if (ImGui::TreeNodeEx("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& textures = state.assets->textures();
            const GpuScene& gpuScene = state.renderer.scene();
            const float thumbSize = 56.0f;
            for (uint32_t i = 0; i < textures.size(); ++i) {
                const TextureAsset& texture = textures[i];
                ImGui::PushID(static_cast<int>(i));

                const bool hasThumbnail = texture.resident && i < gpuScene.materialTextureCount();
                if (hasThumbnail && thumbnailCache_.find(i) == thumbnailCache_.end()) {
                    const VkImageView view = gpuScene.materialTextureImageView(i);
                    if (view != VK_NULL_HANDLE) {
                        thumbnailCache_[i] = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                }

                if (hasThumbnail) {
                    auto cacheIt = thumbnailCache_.find(i);
                    if (cacheIt != thumbnailCache_.end() && cacheIt->second != VK_NULL_HANDLE) {
                        ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(cacheIt->second)),
                                     ImVec2(thumbSize, thumbSize));
                        ImGui::SameLine();
                    }
                }

                const std::string label = std::to_string(i) + ": " + (texture.name.empty() ? texture.sourcePath.filename().string() : texture.name);
                if (ImGui::Selectable(label.c_str(), selection.is(EditorSelectionKind::Asset) && selection.index() == i)) {
                    selection.selectAsset(i);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%ux%u %s", texture.width, texture.height, texture.resident ? "resident" : "cpu");
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Meshes", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& meshes = state.assets->meshes();
            for (uint32_t i = 0; i < meshes.size(); ++i) {
                const MeshAsset& mesh = meshes[i];
                const std::string label = std::to_string(i) + ": " + (mesh.name.empty() ? "mesh" : mesh.name);
                if (ImGui::Selectable(label.c_str(), selection.is(EditorSelectionKind::Asset) && selection.index() == i)) {
                    selection.selectAsset(i);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%zu vertices  %zu primitives", mesh.vertices.size(), mesh.primitives.size());
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNodeEx("Materials", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& materials = state.assets->materials();
            for (uint32_t i = 0; i < materials.size(); ++i) {
                const MaterialAsset& material = materials[i];
                const std::string label = std::to_string(i) + ": " + (material.name.empty() ? "material" : material.name);
                if (ImGui::Selectable(label.c_str(), selection.is(EditorSelectionKind::Material) && selection.index() == i)) {
                    selection.selectMaterial(i);
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("MATERIAL", &i, sizeof(uint32_t));
                    ImGui::Text("Material %u", i);
                    ImGui::EndDragDropSource();
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::End();
}

} // namespace rtv
