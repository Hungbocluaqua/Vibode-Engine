#include "rtv/AssetBrowserPanel.h"

#include "rtv/AssetManager.h"
#include "rtv/EditorPreferences.h"
#include "rtv/FileDialog.h"
#include "rtv/GpuScene.h"

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace rtv {

namespace {

void setPathBuffer(std::array<char, 512>& buffer, const std::filesystem::path& path) {
    const std::string value = path.string();
    std::fill(buffer.begin(), buffer.end(), '\0');
    std::memcpy(buffer.data(), value.data(), std::min(value.size(), buffer.size() - 1));
}

std::string lowerString(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool isSceneImportPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".gltf" || ext == ".glb";
}

bool isEnvironmentPath(const std::filesystem::path& path) {
    const std::string ext = lowerString(path.extension().string());
    return ext == ".hdr" || ext == ".exr";
}

std::string contentKindLabel(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
        return "Folder";
    }
    const std::string ext = lowerString(path.extension().string());
    if (ext == ".rtlevel") return "Scene";
    if (ext == ".gltf" || ext == ".glb") return "glTF Scene";
    if (ext == ".hdr" || ext == ".exr") return "Environment";
    if (ext == ".json") return "JSON";
    return ext.empty() ? "File" : ext.substr(1);
}

std::filesystem::path canonicalForCompare(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return canonical;
    }
    return std::filesystem::absolute(path, ec);
}

} // namespace

void AssetBrowserPanel::invalidateThumbnails() {
    thumbnailCache_.clear();
}

void AssetBrowserPanel::loadFromPath(const std::filesystem::path& path, EditorRequests& requests) {
    const std::string ext = lowerString(path.extension().string());
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

void AssetBrowserPanel::syncBrowserRoot(const EditorRuntimeState& state) {
    std::filesystem::path root;
    compatibilityMode_ = state.project == nullptr;
    if (state.project != nullptr) {
        root = state.project->contentRoot;
    } else if (state.scenePath != nullptr && state.scenePath->has_value()) {
        root = state.scenePath->value().parent_path();
    } else {
        root = std::filesystem::current_path();
    }

    if (root.empty()) {
        root = std::filesystem::current_path();
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    root = canonicalForCompare(root);
    if (browserRoot_ != root) {
        browserRoot_ = root;
        currentPath_ = root;
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

void AssetBrowserPanel::drawFolderTree(const std::filesystem::path& path) {
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
    const bool open = ImGui::TreeNodeEx(path.filename().empty() ? path.string().c_str() : path.filename().string().c_str(), flags);
    if (ImGui::IsItemClicked()) {
        navigateTo(path);
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
            drawFolderTree(child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::drawPathList(const EditorRuntimeState& state, EditorRequests& requests) {
    (void)state;
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
        const float cellWidth = 104.0f;
        const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth));
        ImGui::Columns(columns, "ContentGrid", false);
        for (const auto& entry : entries) {
            const std::filesystem::path path = entry.path();
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            ImGui::Button(entry.is_directory(ec) ? "[Folder]" : "[File]", ImVec2(84.0f, 52.0f));
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
            ImGui::TextWrapped("%s%s", selected ? "> " : "", path.filename().string().c_str());
            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
        return;
    }

    if (ImGui::BeginTable("ContentPathList", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Path");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();
        for (const auto& entry : entries) {
            const std::filesystem::path path = entry.path();
            const bool isDir = entry.is_directory(ec);
            const bool selected = selectedPath_ == path;
            ImGui::PushID(path.string().c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string name = (isDir ? "[D] " : "") + path.filename().string();
            if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
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
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(contentKindLabel(path).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(relativeContentPath(path).c_str());
            ImGui::TableSetColumnIndex(3);
            if (!isDir && (lowerString(path.extension().string()) == ".rtlevel" || isSceneImportPath(path) || isEnvironmentPath(path))) {
                if (ImGui::SmallButton("Open/Apply")) {
                    loadFromPath(path, requests);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
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
    if (ImGui::BeginTable("AssetRegistryRecords", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("GUID");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Imported");
        ImGui::TableSetupColumn("Deps");
        ImGui::TableSetupColumn("Refs");
        ImGui::TableSetupColumn("Missing/Stale");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (const AssetRecord& record : records) {
            ImGui::PushID(record.guid.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(assetTypeName(record.type));
            ImGui::TableSetColumnIndex(1);
            const char* name = record.displayName.empty() ? "(unnamed)" : record.displayName.c_str();
            if (ImGui::Selectable(name, selectedRecordGuid_ == record.guid, ImGuiSelectableFlags_SpanAllColumns)) {
                selectedRecordGuid_ = record.guid;
                selectedPath_.clear();
            }
            if (record.type == AssetType::Prefab && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                std::array<char, 128> guidPayload{};
                record.guid.copy(guidPayload.data(), std::min(record.guid.size(), guidPayload.size() - 1));
                ImGui::SetDragDropPayload("PREFAB_ASSET", guidPayload.data(), guidPayload.size());
                ImGui::Text("Prefab %s", name);
                ImGui::EndDragDropSource();
            }
            if (record.type == AssetType::Prefab && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Place Prefab")) {
                    requests.placeAsset = record.guid;
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
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void AssetBrowserPanel::drawDetails(const EditorRuntimeState& state) {
    ImGui::SeparatorText("Details");
    if (state.project != nullptr) {
        ImGui::Text("Project: %s", state.project->name.c_str());
        ImGui::Text("Content Root: %s", state.project->contentRoot.string().c_str());
    } else {
        ImGui::TextDisabled("No project open. Content is in compatibility mode.");
    }
    ImGui::Text("Current Folder: %s", currentPath_.empty() ? "(none)" : relativeContentPath(currentPath_).c_str());
    if (!selectedPath_.empty()) {
        ImGui::Text("Selected: %s", selectedPath_.filename().string().c_str());
        ImGui::Text("Kind: %s", contentKindLabel(selectedPath_).c_str());
        ImGui::TextWrapped("Path: %s", relativeContentPath(selectedPath_).c_str());
    }
    if (!selectedRecordGuid_.empty() && state.assetRegistry != nullptr) {
        for (const AssetRecord& record : state.assetRegistry->records()) {
            if (record.guid != selectedRecordGuid_) {
                continue;
            }
            ImGui::Text("Asset: %s", record.displayName.empty() ? "(unnamed)" : record.displayName.c_str());
            ImGui::Text("GUID: %s", record.guid.c_str());
            ImGui::Text("Type: %s", assetTypeName(record.type));
            ImGui::TextWrapped("Source: %s", record.sourcePath.c_str());
            ImGui::TextWrapped("Imported: %s", record.importedPath.c_str());
            ImGui::TextWrapped("Cache: %s", record.cachePath.c_str());
            ImGui::Text("Dependencies: %zu", record.dependencies.size());
            ImGui::Text("References: %zu", record.references.size());
            ImGui::Text("Status: %s%s%s", assetImportStatusName(record.status), record.missing ? " missing" : "", record.stale ? " stale" : "");
            break;
        }
    }
    ImGui::Text("Scene: %s", state.scenePath != nullptr && state.scenePath->has_value() ? state.scenePath->value().string().c_str() : "Untitled / not saved");
    ImGui::Text("Source scene import: %s", state.gltfPath != nullptr && state.gltfPath->has_value() ? state.gltfPath->value().string().c_str() : "(none)");
    ImGui::Text("HDRI: %s", state.hdrPath != nullptr && state.hdrPath->has_value() ? state.hdrPath->value().string().c_str() : "(procedural)");
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
            status_ = "Queued non-mutating Import Asset: " + requests.importAsset->sourcePath.string();
        } else {
            requests.importAndPlace = request.sourcePath;
            status_ = "Queued Import and Place: " + request.sourcePath.string();
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
    syncBrowserRoot(state);

    ImGui::BeginGroup();
    if (ImGui::Button("Add/Import")) {
        if (compatibilityMode_) {
            requests.showProjectManager = true;
            status_ = "Open or create a project before importing reusable assets.";
        } else if (auto path = openGltfFileDialog()) {
            setPathBuffer(gltfPath_, *path);
            setPathBuffer(importSourcePath_, *path);
            if (importDestinationFolder_[0] == '\0') {
                const char* defaultDestination = "Models";
                std::memcpy(importDestinationFolder_.data(), defaultDestination, std::strlen(defaultDestination));
            }
            openImportSettings_ = true;
            ImGui::OpenPopup("Import Settings");
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##contentFilter", "Search Content", search_.data(), search_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("<") && !backStack_.empty()) {
        forwardStack_.push_back(currentPath_);
        const std::filesystem::path previous = backStack_.back();
        backStack_.pop_back();
        navigateTo(previous, false);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">") && !forwardStack_.empty()) {
        backStack_.push_back(currentPath_);
        const std::filesystem::path next = forwardStack_.back();
        forwardStack_.pop_back();
        navigateTo(next, false);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        invalidateThumbnails();
        status_ = "Content refreshed";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(gridView_ ? "List" : "Grid")) {
        gridView_ = !gridView_;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(showDetails_ ? "Hide Details" : "Details")) {
        showDetails_ = !showDetails_;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("/ %s", currentPath_.empty() ? "Content" : relativeContentPath(currentPath_).c_str());
    ImGui::EndGroup();
    drawImportSettingsDialog(requests);

    if (compatibilityMode_) {
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
            "Compatibility mode: open or create a project before importing reusable assets.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Project Manager")) {
            requests.showProjectManager = true;
        }
    }

    if (!browserRoot_.empty()) {
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(currentPath_, browserRoot_, relativeError);
        if (ImGui::SmallButton("Content")) {
            navigateTo(browserRoot_);
        }
        std::filesystem::path accum = browserRoot_;
        for (const auto& part : relativeError ? std::filesystem::path{} : relative) {
            const std::string partString = part.string();
            if (partString == "." || partString.empty()) {
                continue;
            }
            accum /= part;
            ImGui::SameLine();
            ImGui::TextDisabled("/");
            ImGui::SameLine();
            if (ImGui::SmallButton(partString.c_str())) {
                navigateTo(accum);
            }
        }
    }

    const float browserHeight = std::max(260.0f, ImGui::GetContentRegionAvail().y * 0.46f);
    if (ImGui::BeginChild("ContentBrowser", ImVec2(0.0f, browserHeight), true)) {
        const float detailsWidth = showDetails_ ? 300.0f : 0.0f;
        const float treeWidth = 210.0f;
        ImGui::BeginChild("ContentFolders", ImVec2(treeWidth, 0.0f), true);
        if (!browserRoot_.empty()) {
            drawFolderTree(browserRoot_);
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
        ImGui::BeginChild("ContentItems", ImVec2(-(detailsWidth + (showDetails_ ? ImGui::GetStyle().ItemSpacing.x : 0.0f)), 0.0f), true);
        drawPathList(state, requests);
        drawRegistryTable(state, requests);
        ImGui::EndChild();
        if (showDetails_) {
            ImGui::SameLine();
            ImGui::BeginChild("ContentDetails", ImVec2(detailsWidth, 0.0f), true);
            drawDetails(state);
            if (state.editorPrefs != nullptr && !selectedPath_.empty()) {
                if (ImGui::SmallButton("Add Selected to Favorites")) {
                    state.editorPrefs->addFavorite(selectedPath_);
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();

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
