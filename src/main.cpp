#include "rtv/Application.h"
#include "rtv/RendererBackend.h"
#include "rtv/RendererDebug.h"

#include <exception>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    try {
        uint32_t maxFrames = 0;
        rtv::RendererDebugView debugView = rtv::RendererDebugView::Beauty;
        bool debugViewProvided = false;
        std::optional<std::filesystem::path> gltfPath;
        std::optional<std::filesystem::path> hdrPath;
        std::optional<std::filesystem::path> scenePath;
        std::optional<bool> denoiserOverride;
        rtv::RendererBackend backend = rtv::RendererBackend::Auto;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--frames" && i + 1 < argc) {
                maxFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (std::string_view(argv[i]) == "--debug-view" && i + 1 < argc) {
                debugView = rtv::parseRendererDebugView(argv[++i]);
                debugViewProvided = true;
            } else if (std::string_view(argv[i]) == "--gltf" && i + 1 < argc) {
                gltfPath = std::filesystem::path(argv[++i]);
            } else if (std::string_view(argv[i]) == "--hdr" && i + 1 < argc) {
                hdrPath = std::filesystem::path(argv[++i]);
            } else if ((std::string_view(argv[i]) == "--scene" || std::string_view(argv[i]) == "--rtlevel") && i + 1 < argc) {
                scenePath = std::filesystem::path(argv[++i]);
            } else if (std::string_view(argv[i]) == "--backend" && i + 1 < argc) {
                backend = rtv::parseRendererBackend(argv[++i]);
            } else if (std::string_view(argv[i]) == "--denoiser" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                denoiserOverride = !(value == "off" || value == "false" || value == "0");
            }
        }

        rtv::Application app(debugView, gltfPath, hdrPath, backend, scenePath, denoiserOverride, debugViewProvided);
        app.run(maxFrames);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
