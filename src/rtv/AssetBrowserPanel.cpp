#include "rtv/AssetBrowserPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/FileDialog.h"

#include <imgui.h>

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

void AssetBrowserPanel::draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests) {
    if (!ImGui::Begin("Asset Browser")) {
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

    ImGui::SeparatorText("Load Assets");
    ImGui::TextUnformatted("Scene");
    ImGui::PushItemWidth(-190.0f);
    ImGui::InputTextWithHint("##gltfPath", "C:\\path\\to\\scene.glb", gltfPath_.data(), gltfPath_.size());
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##gltf")) {
        if (auto path = openGltfFileDialog()) {
            setPathBuffer(gltfPath_, *path);
            requests.loadGltf = *path;
            status_ = "Queued glTF load: " + path->string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load glTF")) {
        std::filesystem::path path{gltfPath_.data()};
        if (!path.empty()) {
            requests.loadGltf = path;
            status_ = "Queued glTF load: " + path.string();
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
    if (ImGui::Button("Load HDR")) {
        std::filesystem::path path{hdrPath_.data()};
        if (!path.empty()) {
            requests.loadHdr = path;
            status_ = "Queued HDR load: " + path.string();
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
            requests.loadSceneJson = *path;
            status_ = "Queued scene load: " + path->string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Level")) {
        std::filesystem::path path{scenePath_.data()};
        if (path.empty()) {
            if (auto selected = saveSceneJsonFileDialog()) {
                path = *selected;
                setPathBuffer(scenePath_, path);
            }
        }
        if (!path.empty()) {
            requests.saveSceneJson = path;
            status_ = "Queued scene save: " + path.string();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Level")) {
        std::filesystem::path path{scenePath_.data()};
        if (!path.empty()) {
            requests.loadSceneJson = path;
            status_ = "Queued scene load: " + path.string();
        }
    }

    ImGui::SeparatorText("Loaded");
    ImGui::Text("glTF: %s", state.gltfPath != nullptr && state.gltfPath->has_value() ? state.gltfPath->value().string().c_str() : "(fallback scene)");
    ImGui::Text("HDR: %s", state.hdrPath != nullptr && state.hdrPath->has_value() ? state.hdrPath->value().string().c_str() : "(procedural)");

    if (state.importedScene != nullptr) {
        ImGui::Text("Scene nodes: %zu", state.importedScene->nodes.size());
    }

    if (state.assets != nullptr) {
        if (ImGui::TreeNodeEx("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& textures = state.assets->textures();
            for (uint32_t i = 0; i < textures.size(); ++i) {
                const TextureAsset& texture = textures[i];
                const std::string label = std::to_string(i) + ": " + (texture.name.empty() ? texture.sourcePath.filename().string() : texture.name);
                if (ImGui::Selectable(label.c_str(), selection.is(EditorSelectionKind::Asset) && selection.index() == i)) {
                    selection.selectAsset(i);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%ux%u %s", texture.width, texture.height, texture.resident ? "resident" : "cpu");
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
            }
            ImGui::TreePop();
        }
    }

    ImGui::End();
}

} // namespace rtv
