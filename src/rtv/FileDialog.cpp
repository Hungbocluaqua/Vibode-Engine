#include "rtv/FileDialog.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

#include <array>

namespace rtv {

namespace {

#if defined(_WIN32)
[[nodiscard]] std::optional<std::filesystem::path> openFileDialog(const wchar_t* title, const wchar_t* filter) {
    std::array<wchar_t, 32768> path{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = path.data();
    ofn.nMaxFile = static_cast<DWORD>(path.size());
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        return std::filesystem::path(path.data());
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> saveFileDialog(const wchar_t* title, const wchar_t* filter, const wchar_t* defaultExtension) {
    std::array<wchar_t, 32768> path{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = path.data();
    ofn.nMaxFile = static_cast<DWORD>(path.size());
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = defaultExtension;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;

    if (GetSaveFileNameW(&ofn) == TRUE) {
        return std::filesystem::path(path.data());
    }
    return std::nullopt;
}
#endif

} // namespace

std::optional<std::filesystem::path> openGltfFileDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Import Scene as New Scene", L"glTF scenes (*.gltf;*.glb)\0*.gltf;*.glb\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openHdrFileDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Open HDR Environment", L"HDR environments (*.hdr)\0*.hdr\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openSceneJsonFileDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Open Level", L"RT Level (*.rtlevel;*.json)\0*.rtlevel;*.json\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openProjectFileDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Open Project", L"RT Project (*.rtproject)\0*.rtproject\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> saveSceneJsonFileDialog() {
#if defined(_WIN32)
    return saveFileDialog(L"Save Level", L"RT Level (*.rtlevel)\0*.rtlevel\0Scene JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0", L"rtlevel");
#else
    return std::nullopt;
#endif
}

} // namespace rtv
