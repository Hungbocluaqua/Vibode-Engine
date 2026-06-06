cmake_minimum_required(VERSION 3.27)

project(RayTracingEngineVulkan LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)
find_package(unofficial-spirv-reflect CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

set(RENDERDOC_SDK_DIR "" CACHE PATH "Path to RenderDoc SDK directory")
set(NRD_SDK_DIR "" CACHE PATH "Path to NVIDIA Real-Time Denoisers SDK/source checkout")
set(DLSS_SDK_DIR "" CACHE PATH "Path to NVIDIA DLSS/NGX SDK directory")

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK is not set. Install the Vulkan SDK and open a new terminal.")
endif()

set(VOLK_DIR "$ENV{VULKAN_SDK}/Include/Volk")
if(NOT EXISTS "${VOLK_DIR}/volk.c")
    message(FATAL_ERROR "Volk was not found in ${VOLK_DIR}. Re-run the Vulkan SDK installer and select Volk.")
endif()

set(IMGUI_SOURCE_DIR "" CACHE PATH "Path to ImGui source directory (imgui.cpp must exist here)")
if(IMGUI_SOURCE_DIR STREQUAL "")
    file(GLOB IMGUI_CANDIDATES LIST_DIRECTORIES true "C:/Users/HomePc/vcpkg/buildtrees/imgui/src/*")
    foreach(CANDIDATE ${IMGUI_CANDIDATES})
        if(EXISTS "${CANDIDATE}/imgui.cpp")
            set(IMGUI_SOURCE_DIR "${CANDIDATE}")
            break()
        endif()
    endforeach()
endif()
if(IMGUI_SOURCE_DIR STREQUAL "" OR NOT EXISTS "${IMGUI_SOURCE_DIR}/imgui.cpp")
    message(FATAL_ERROR "ImGui source not found. Set IMGUI_SOURCE_DIR to your ImGui source path via -DIMGUI_SOURCE_DIR=...")
endif()

add_library(volk STATIC "${VOLK_DIR}/volk.c")
target_include_directories(volk PUBLIC "$ENV{VULKAN_SDK}/Include")
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)

add_executable(rtvulkan
    "${IMGUI_SOURCE_DIR}/imgui.cpp"
    "${IMGUI_SOURCE_DIR}/imgui_draw.cpp"
    "${IMGUI_SOURCE_DIR}/imgui_tables.cpp"
    "${IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
    "${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    src/third_party/imgui/imgui_impl_vulkan.cpp
    src/main.cpp
    src/rtv/AnimationClip.cpp
    src/rtv/Application.cpp
    src/rtv/AccelerationStructure.cpp
    src/rtv/AsyncSceneLoader.cpp
    src/rtv/AssetImport.cpp
    src/rtv/AssetRegistry.cpp
    src/rtv/AssetManager.cpp
    src/rtv/AssetBrowserPanel.cpp
    src/rtv/AtmosphereLutSystem.cpp
    src/rtv/AtmosphereModel.cpp
    src/rtv/AtmosphereSamplingSystem.cpp
    src/rtv/BatchUploader.cpp
    src/rtv/BindlessResources.cpp
    src/rtv/CameraBookmark.cpp
    src/rtv/Buffer.cpp
    src/rtv/BufferUploader.cpp
    src/rtv/BvhBuilder.cpp
    src/rtv/BvhCollapse.cpp
    src/rtv/CameraController.cpp
    src/rtv/CommandSystem.cpp
    src/rtv/ComputePipeline.cpp
    src/rtv/DebugProfilerPanel.cpp
    src/rtv/DescriptorAllocator.cpp
    src/rtv/DescriptorLayoutCache.cpp
    src/rtv/DescriptorSet.cpp
    src/rtv/DescriptorWriter.cpp
    src/rtv/DiagnosticImageExport.cpp
    src/rtv/DiagnosticTools.cpp
    src/rtv/EditorDockspace.cpp
    src/rtv/EditorCommands.cpp
    src/rtv/EditorLayer.cpp
    src/rtv/EditorLog.cpp
    src/rtv/EditorPanels.cpp
    src/rtv/EditorPreferences.cpp
    src/rtv/EditorSelection.cpp
    src/rtv/EditorTimeline.cpp
    src/rtv/Entity.cpp
    src/rtv/EnvironmentImportanceSampler.cpp
    src/rtv/FileDialog.cpp
    src/rtv/FrameResources.cpp
    src/rtv/GltfLoader.cpp
    src/rtv/GpuDiagnosticsPanel.cpp
    src/rtv/GpuProfiler.cpp
    src/rtv/GpuScene.cpp
    src/rtv/GpuValidation.cpp
    src/rtv/GraphicsPipeline.cpp
    src/rtv/HdrLoader.cpp
    src/rtv/HeadlessDiagnostics.cpp
    src/rtv/Image.cpp
    src/rtv/ImageBarrier.cpp
    src/rtv/InspectorPanel.cpp
    src/rtv/KeyBindings.cpp
    src/rtv/LightBvh.cpp
    src/rtv/MaterialEditorPanel.cpp
    src/rtv/MortonCodes.cpp
    src/rtv/NotificationManager.cpp
    src/rtv/OpacityMicromapPreprocess.cpp
    src/rtv/ParallelBvhBuilder.cpp
    src/rtv/PathTracerRenderer.cpp
    src/rtv/PhysicalCamera.cpp
    src/rtv/PipelineBuilder.cpp
    src/rtv/PipelineCache.cpp
    src/rtv/PipelineDemo.cpp
    src/rtv/Prefab.cpp
    src/rtv/Project.cpp
    src/rtv/RenderSettingsPanel.cpp
    src/rtv/RenderGraph.cpp
    src/rtv/RenderGraphDump.cpp
    src/rtv/RendererDebug.cpp
    src/rtv/RayTracingPipeline.cpp
    src/rtv/RayTracingScene.cpp
    src/rtv/ResourceAllocator.cpp
    src/rtv/ResourceDemo.cpp
    src/rtv/SahBuilder.cpp
    src/rtv/SceneComponents.cpp
    src/rtv/SceneDocument.cpp
    src/rtv/SceneEventBus.cpp
    src/rtv/SceneHierarchyPanel.cpp
    src/rtv/SceneOperations.cpp
    src/rtv/SceneRenderSettingsSync.cpp
    src/rtv/SceneStatsPanel.cpp
    src/rtv/SceneRegistry.cpp
    src/rtv/SceneToGpuSceneBuilder.cpp
    src/rtv/SceneUpdateRouter.cpp
    src/rtv/SkySystem.cpp
    src/rtv/SunController.cpp
    src/rtv/SceneCache.cpp
    src/rtv/ShaderCompiler.cpp
    src/rtv/ShaderModule.cpp
    src/rtv/ShaderReflection.cpp
    src/rtv/Swapchain.cpp
    src/rtv/TextureLoader.cpp
    src/rtv/TemporalSystem.cpp
    src/rtv/UiOverlay.cpp
    src/rtv/UndoStack.cpp
    src/rtv/UploadContext.cpp
    src/rtv/ValidationSceneSuite.cpp
    src/rtv/VarianceEstimator.cpp
    src/rtv/ViewportPanel.cpp
    src/rtv/VulkanContext.cpp
)

target_include_directories(rtvulkan PRIVATE include ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} "${IMGUI_SOURCE_DIR}" "${IMGUI_SOURCE_DIR}/backends")
target_compile_definitions(rtvulkan PRIVATE GLFW_INCLUDE_NONE VK_NO_PROTOTYPES NOMINMAX IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_IMPL_VULKAN_VOLK_FILENAME=<Volk/volk.h>)
target_link_libraries(rtvulkan PRIVATE
    volk
    glfw
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
    imguizmo::imguizmo
    unofficial::spirv-reflect
    KTX::ktx
)

if(EXISTS "${RENDERDOC_SDK_DIR}/renderdoc_app.h")
    target_compile_definitions(rtvulkan PRIVATE RTV_HAS_RENDERDOC=1)
    target_include_directories(rtvulkan PRIVATE "${RENDERDOC_SDK_DIR}")
endif()

if(EXISTS "${NRD_SDK_DIR}/Include/NRD.h" OR EXISTS "${NRD_SDK_DIR}/Include/NRDDescs.h" OR EXISTS "${NRD_SDK_DIR}/Include/NRDIntegration.hpp")
    target_compile_definitions(rtvulkan PRIVATE RTV_NRD_SDK_CONFIGURED=1)
    target_include_directories(rtvulkan PRIVATE "${NRD_SDK_DIR}/Include")
    if(EXISTS "${NRD_SDK_DIR}/Integration/NRDIntegration.hpp")
        target_include_directories(rtvulkan PRIVATE "${NRD_SDK_DIR}/Integration")
    endif()
    if(EXISTS "${NRD_SDK_DIR}/CMakeLists.txt")
        set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
        set(NRD_NRI OFF CACHE BOOL "" FORCE)
        set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "" FORCE)
        set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
        set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
        set(NRD_SUPPORTS_QUAD_INTRINSICS OFF CACHE BOOL "" FORCE)
        set(NRD_NORMAL_ENCODING "4" CACHE STRING "" FORCE)
        set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE)
        add_subdirectory("${NRD_SDK_DIR}" "${CMAKE_BINARY_DIR}/nrd" EXCLUDE_FROM_ALL)
        target_link_libraries(rtvulkan PRIVATE NRD)
        target_compile_definitions(rtvulkan PRIVATE RTV_NRD_RUNTIME_ENABLED=1)
    endif()
endif()

if(EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_vk.h" OR EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx.h" OR EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_helpers_vk.h")
    target_compile_definitions(rtvulkan PRIVATE RTV_DLSS_SDK_CONFIGURED=1)
    target_include_directories(rtvulkan PRIVATE "${DLSS_SDK_DIR}/include")
    set(DLSS_NGX_LIB_RELEASE "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    set(DLSS_NGX_LIB_DEBUG "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib")
    set(DLSS_RUNTIME_DIR "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel")
    set(DLSS_RUNTIME_DLL "${DLSS_RUNTIME_DIR}/nvngx_dlss.dll")
    if(WIN32 AND EXISTS "${DLSS_NGX_LIB_RELEASE}" AND EXISTS "${DLSS_NGX_LIB_DEBUG}" AND EXISTS "${DLSS_RUNTIME_DLL}")
        target_compile_definitions(rtvulkan PRIVATE
            RTV_DLSS_NGX_ENABLED=1
            "RTV_DLSS_RUNTIME_DIR=\"${DLSS_RUNTIME_DIR}\"")
        target_link_libraries(rtvulkan PRIVATE
            "$<$<CONFIG:Debug>:${DLSS_NGX_LIB_DEBUG}>"
            "$<$<NOT:$<CONFIG:Debug>>:${DLSS_NGX_LIB_RELEASE}>")
        foreach(DLSS_RUNTIME_NAME IN ITEMS nvngx_dlss.dll nvngx_dlssd.dll nvngx_dlssg.dll)
            if(EXISTS "${DLSS_RUNTIME_DIR}/${DLSS_RUNTIME_NAME}")
                add_custom_command(TARGET rtvulkan POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${DLSS_RUNTIME_DIR}/${DLSS_RUNTIME_NAME}"
                        "$<TARGET_FILE_DIR:rtvulkan>/${DLSS_RUNTIME_NAME}")
            endif()
        endforeach()
    endif()
endif()

if(WIN32)
    target_link_libraries(rtvulkan PRIVATE Comdlg32 User32 Shell32 Dwmapi)
endif()

if(MSVC)
    target_compile_options(rtvulkan PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(rtvulkan PRIVATE -Wall -Wextra -Wpedantic)
endif()
