#include "rtv/FileDialog.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
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

std::optional<std::filesystem::path> openImportAssetFileDialog() {
#if defined(_WIN32)
    const wchar_t* filter =
#if RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE && RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
        L"Importable assets (*.gltf;*.glb;*.obj;*.fbx;*.usd;*.usda;*.usdc;*.usdz;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr)\0*.gltf;*.glb;*.obj;*.fbx;*.usd;*.usda;*.usdc;*.usdz;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr\0"
        L"glTF models (*.gltf;*.glb)\0*.gltf;*.glb\0"
        L"OBJ models (*.obj)\0*.obj\0"
        L"FBX models (*.fbx)\0*.fbx\0"
        L"USD stages (*.usd;*.usda;*.usdc;*.usdz)\0*.usd;*.usda;*.usdc;*.usdz\0"
#elif RTV_ENABLE_ASSIMP_IMPORTER && RTV_ASSIMP_IMPORTER_AVAILABLE
        L"Importable assets (*.gltf;*.glb;*.obj;*.fbx;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr)\0*.gltf;*.glb;*.obj;*.fbx;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr\0"
        L"glTF models (*.gltf;*.glb)\0*.gltf;*.glb\0"
        L"OBJ models (*.obj)\0*.obj\0"
        L"FBX models (*.fbx)\0*.fbx\0"
#elif RTV_ENABLE_OPENUSD_IMPORTER && RTV_OPENUSD_IMPORTER_AVAILABLE
        L"Importable assets (*.gltf;*.glb;*.obj;*.usd;*.usda;*.usdc;*.usdz;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr)\0*.gltf;*.glb;*.obj;*.usd;*.usda;*.usdc;*.usdz;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr\0"
        L"glTF models (*.gltf;*.glb)\0*.gltf;*.glb\0"
        L"OBJ models (*.obj)\0*.obj\0"
        L"USD stages (*.usd;*.usda;*.usdc;*.usdz)\0*.usd;*.usda;*.usdc;*.usdz\0"
#else
        L"Importable assets (*.gltf;*.glb;*.obj;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr)\0*.gltf;*.glb;*.obj;*.mtl;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2;*.hdr;*.exr\0"
        L"glTF models (*.gltf;*.glb)\0*.gltf;*.glb\0"
        L"OBJ models (*.obj)\0*.obj\0"
#endif
        L"MTL material libraries (*.mtl)\0*.mtl\0"
        L"Textures (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2\0"
        L"HDR environments (*.hdr;*.exr)\0*.hdr;*.exr\0"
        L"All files (*.*)\0*.*\0\0";

    return openFileDialog(
        L"Import Asset",
        filter);
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openTextureFileDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Import Texture", L"Textures (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds;*.ktx;*.ktx2\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openHdrFileDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Import HDR Environment", L"HDR environments (*.hdr;*.exr)\0*.hdr;*.exr\0All files (*.*)\0*.*\0\0");
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
    return openFileDialog(L"Open Vibode Project", L"Vibode Project (*.vproject)\0*.vproject\0Legacy RT Project (*.rtproject)\0*.rtproject\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openNativeTextureTargetSetLibraryDialog() {
#if defined(_WIN32)
    return openFileDialog(L"Import Native Texture Target Set Library", L"Target Set Library (*.json)\0*.json\0All files (*.*)\0*.*\0\0");
#else
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> openFolderDialog(const wchar_t* title) {
#if defined(_WIN32)
    BROWSEINFOW browse{};
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (item == nullptr) {
        return std::nullopt;
    }
    std::array<wchar_t, MAX_PATH> path{};
    const BOOL ok = SHGetPathFromIDListW(item, path.data());
    CoTaskMemFree(item);
    if (ok == TRUE) {
        return std::filesystem::path(path.data());
    }
    return std::nullopt;
#else
    (void)title;
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

std::optional<std::filesystem::path> saveNativeTextureTargetSetLibraryDialog() {
#if defined(_WIN32)
    return saveFileDialog(L"Export Native Texture Target Set Library", L"Target Set Library (*.json)\0*.json\0All files (*.*)\0*.*\0\0", L"json");
#else
    return std::nullopt;
#endif
}

} // namespace rtv
