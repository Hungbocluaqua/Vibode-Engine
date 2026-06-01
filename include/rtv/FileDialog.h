#pragma once

#include <filesystem>
#include <optional>

namespace rtv {

[[nodiscard]] std::optional<std::filesystem::path> openGltfFileDialog();
[[nodiscard]] std::optional<std::filesystem::path> openHdrFileDialog();
[[nodiscard]] std::optional<std::filesystem::path> openSceneJsonFileDialog();
[[nodiscard]] std::optional<std::filesystem::path> openProjectFileDialog();
[[nodiscard]] std::optional<std::filesystem::path> openFolderDialog(const wchar_t* title = L"Select Folder");
[[nodiscard]] std::optional<std::filesystem::path> saveSceneJsonFileDialog();

} // namespace rtv
