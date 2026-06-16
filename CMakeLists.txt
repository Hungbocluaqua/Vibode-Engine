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
find_package(TIFF REQUIRED)
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

option(RTV_ENABLE_TINYOBJ_IMPORTER "Enable OBJ/MTL runtime importer through tinyobjloader when available" OFF)
find_path(TINYOBJLOADER_INCLUDE_DIRS "tiny_obj_loader.h"
    HINTS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyobjloader"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyobjloader/include")
set(RTV_TINYOBJ_IMPORTER_AVAILABLE OFF)
if(TINYOBJLOADER_INCLUDE_DIRS)
    set(RTV_TINYOBJ_IMPORTER_AVAILABLE ON)
endif()
if(RTV_ENABLE_TINYOBJ_IMPORTER AND NOT RTV_TINYOBJ_IMPORTER_AVAILABLE)
    message(FATAL_ERROR "RTV_ENABLE_TINYOBJ_IMPORTER is ON, but tiny_obj_loader.h was not found. Install tinyobjloader through vcpkg or set TINYOBJLOADER_INCLUDE_DIRS.")
endif()

option(RTV_ENABLE_ASSIMP_IMPORTER "Enable FBX importer dependency gate through Assimp when available" OFF)
find_package(assimp CONFIG QUIET)
set(RTV_ASSIMP_IMPORTER_AVAILABLE OFF)
if(TARGET assimp::assimp)
    set(RTV_ASSIMP_IMPORTER_AVAILABLE ON)
endif()
if(RTV_ENABLE_ASSIMP_IMPORTER AND NOT RTV_ASSIMP_IMPORTER_AVAILABLE)
    message(FATAL_ERROR "RTV_ENABLE_ASSIMP_IMPORTER is ON, but Assimp was not found. Install assimp through vcpkg or configure CMAKE_PREFIX_PATH/assimp_DIR.")
endif()

option(RTV_ENABLE_OPENUSD_IMPORTER "Enable USD/USDZ importer dependency gate through OpenUSD when available" OFF)
find_package(pxr CONFIG QUIET)
find_path(OPENUSD_INCLUDE_DIR "pxr/usd/usd/stage.h")
find_library(OPENUSD_USD_LIBRARY NAMES usd_usd usd USD)
set(RTV_OPENUSD_IMPORTER_AVAILABLE OFF)
set(RTV_OPENUSD_IMPORTER_TARGETS "")
if(TARGET usd)
    set(RTV_OPENUSD_IMPORTER_AVAILABLE ON)
    set(RTV_OPENUSD_IMPORTER_TARGETS usd usdGeom usdShade usdLux)
elseif(OPENUSD_INCLUDE_DIR AND OPENUSD_USD_LIBRARY)
    set(RTV_OPENUSD_IMPORTER_AVAILABLE ON)
    set(RTV_OPENUSD_IMPORTER_TARGETS ${OPENUSD_USD_LIBRARY})
endif()
if(RTV_ENABLE_OPENUSD_IMPORTER AND NOT RTV_OPENUSD_IMPORTER_AVAILABLE)
    message(FATAL_ERROR "RTV_ENABLE_OPENUSD_IMPORTER is ON, but OpenUSD headers/libraries were not found. Install OpenUSD through vcpkg or configure CMAKE_PREFIX_PATH/OPENUSD_INCLUDE_DIR/OPENUSD_USD_LIBRARY.")
endif()

set(RENDERDOC_SDK_DIR "" CACHE PATH "Path to RenderDoc SDK directory")
set(NRD_SDK_DIR "" CACHE PATH "Path to NVIDIA Real-Time Denoisers SDK/source checkout")
set(DLSS_SDK_DIR "" CACHE PATH "Path to NVIDIA DLSS/NGX SDK directory")
set(STREAMLINE_SDK_DIR "" CACHE PATH "Path to NVIDIA Streamline SDK/release directory")
if(STREAMLINE_SDK_DIR STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Streamline/include/sl.h")
    set(STREAMLINE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Streamline" CACHE PATH "Path to NVIDIA Streamline SDK/release directory" FORCE)
endif()
set(DIRECTSTORAGE_SDK_DIR "" CACHE PATH "Path to Microsoft DirectStorage SDK package root")
if(DIRECTSTORAGE_SDK_DIR STREQUAL "" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectStorage/Microsoft.Direct3D.DirectStorage.1.4.0-preview1-2603.504/native/include/dstorage.h")
    set(DIRECTSTORAGE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectStorage/Microsoft.Direct3D.DirectStorage.1.4.0-preview1-2603.504" CACHE PATH "Path to Microsoft DirectStorage SDK package root" FORCE)
endif()

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
    src/third_party/tinyexr/tinyexr_impl.cpp
    third_party/tinyexr/deps/miniz/miniz.c
    src/main.cpp
    src/rtv/AnimationClip.cpp
    src/rtv/AnimationController.cpp
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
    src/rtv/FrameWorkScheduler.cpp
    src/rtv/FrameResources.cpp
    src/rtv/GltfLoader.cpp
    src/rtv/GpuDiagnosticsPanel.cpp
    src/rtv/GpuProfiler.cpp
    src/rtv/GpuScene.cpp
    src/rtv/GpuSceneStreamingState.cpp
    src/rtv/IncrementalGpuSceneUpdateQueue.cpp
    src/rtv/GpuUploadTicket.cpp
    src/rtv/GpuValidation.cpp
    src/rtv/GraphicsPipeline.cpp
    src/rtv/HdrLoader.cpp
    src/rtv/HeadlessDiagnostics.cpp
    src/rtv/Image.cpp
    src/rtv/ImageBarrier.cpp
    src/rtv/InspectorPanel.cpp
    src/rtv/KeyBindings.cpp
    src/rtv/LightBvh.cpp
    src/rtv/MainThreadApplyTicket.cpp
    src/rtv/MaterialEditorPanel.cpp
    src/rtv/MortonCodes.cpp
    src/rtv/NativeAssetMigration.cpp
    src/rtv/NativeAssetRuntimeLoader.cpp
    src/rtv/NativeAssetStore.cpp
    src/rtv/NativeGpuAssetCache.cpp
    src/rtv/NativeBinaryIO.cpp
    src/rtv/NativeAssetCooker.cpp
    src/rtv/NativeTextureFormatPolicy.cpp
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
    src/rtv/RtpkgIO.cpp
    src/rtv/RuntimeSkeleton.cpp
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
    src/rtv/ScatterPalette.cpp
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
    src/rtv/StreamlineRuntime.cpp
    src/rtv/StreamingGpuWorkQueue.cpp
    src/rtv/StreamingIoBackend.cpp
    src/rtv/StreamingRuntime.cpp
    src/rtv/StreamingScheduler.cpp
    src/rtv/Swapchain.cpp
    src/rtv/TextureLoader.cpp
    src/rtv/TemporalSystem.cpp
    src/rtv/TopologyRebuildTicket.cpp
    src/rtv/UiOverlay.cpp
    src/rtv/UndoStack.cpp
    src/rtv/UploadContext.cpp
    src/rtv/ValidationSceneSuite.cpp
    src/rtv/VarianceEstimator.cpp
    src/rtv/ViewportPanel.cpp
    src/rtv/VulkanContext.cpp
)

target_include_directories(rtvulkan PRIVATE include ${Stb_INCLUDE_DIR} ${TINYGLTF_INCLUDE_DIRS} "${IMGUI_SOURCE_DIR}" "${IMGUI_SOURCE_DIR}/backends")
target_include_directories(rtvulkan PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyexr"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyexr/deps/miniz")
if(RTV_TINYOBJ_IMPORTER_AVAILABLE)
    target_include_directories(rtvulkan PRIVATE ${TINYOBJLOADER_INCLUDE_DIRS})
endif()
if(RTV_ASSIMP_IMPORTER_AVAILABLE)
    target_link_libraries(rtvulkan PRIVATE assimp::assimp)
endif()
if(RTV_ENABLE_OPENUSD_IMPORTER AND RTV_OPENUSD_IMPORTER_AVAILABLE)
    if(OPENUSD_INCLUDE_DIR)
        target_include_directories(rtvulkan PRIVATE ${OPENUSD_INCLUDE_DIR})
    endif()
    target_link_libraries(rtvulkan PRIVATE ${RTV_OPENUSD_IMPORTER_TARGETS})
    if(pxr_DIR)
        get_filename_component(RTV_OPENUSD_PREFIX "${pxr_DIR}/../.." ABSOLUTE)
        set(RTV_OPENUSD_SCHEMA_DIR "${RTV_OPENUSD_PREFIX}/bin/usd")
        set(RTV_OPENUSD_PLUGIN_DIR "${RTV_OPENUSD_PREFIX}/plugin/usd")
        if(EXISTS "${RTV_OPENUSD_SCHEMA_DIR}")
            add_custom_command(TARGET rtvulkan POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTV_OPENUSD_SCHEMA_DIR}" "$<TARGET_FILE_DIR:rtvulkan>/usd"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RTV_OPENUSD_PLUGIN_DIR}/plugInfo.json" "$<TARGET_FILE_DIR:rtvulkan>/usd/plugInfo.json")
        endif()
        if(EXISTS "${RTV_OPENUSD_PLUGIN_DIR}")
            add_custom_command(TARGET rtvulkan POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${RTV_OPENUSD_PLUGIN_DIR}" "$<TARGET_FILE_DIR:rtvulkan>/../plugin/usd")
        endif()
    endif()
endif()
target_compile_definitions(rtvulkan PRIVATE
    GLFW_INCLUDE_NONE
    VK_NO_PROTOTYPES
    NOMINMAX
    IMGUI_IMPL_VULKAN_USE_VOLK
    IMGUI_IMPL_VULKAN_VOLK_FILENAME=<Volk/volk.h>
    RTV_ENABLE_TINYOBJ_IMPORTER=$<BOOL:${RTV_ENABLE_TINYOBJ_IMPORTER}>
    RTV_TINYOBJ_IMPORTER_AVAILABLE=$<BOOL:${RTV_TINYOBJ_IMPORTER_AVAILABLE}>
    RTV_ENABLE_ASSIMP_IMPORTER=$<BOOL:${RTV_ENABLE_ASSIMP_IMPORTER}>
    RTV_ASSIMP_IMPORTER_AVAILABLE=$<BOOL:${RTV_ASSIMP_IMPORTER_AVAILABLE}>
    RTV_ENABLE_OPENUSD_IMPORTER=$<BOOL:${RTV_ENABLE_OPENUSD_IMPORTER}>
    RTV_OPENUSD_IMPORTER_AVAILABLE=$<BOOL:${RTV_OPENUSD_IMPORTER_AVAILABLE}>)
target_link_libraries(rtvulkan PRIVATE
    volk
    glfw
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
    imguizmo::imguizmo
    unofficial::spirv-reflect
    KTX::ktx
    TIFF::TIFF
)

if(EXISTS "${RENDERDOC_SDK_DIR}/renderdoc_app.h")
    target_compile_definitions(rtvulkan PRIVATE RTV_HAS_RENDERDOC=1)
    target_include_directories(rtvulkan PRIVATE "${RENDERDOC_SDK_DIR}")
endif()

if(WIN32 AND EXISTS "${DIRECTSTORAGE_SDK_DIR}/native/include/dstorage.h")
    set(RTV_DIRECTSTORAGE_ARCH "x64")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(RTV_DIRECTSTORAGE_ARCH "x86")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
        set(RTV_DIRECTSTORAGE_ARCH "ARM64")
    endif()
    set(RTV_DIRECTSTORAGE_LIB "${DIRECTSTORAGE_SDK_DIR}/native/lib/${RTV_DIRECTSTORAGE_ARCH}/dstorage.lib")
    set(RTV_DIRECTSTORAGE_BIN_DIR "${DIRECTSTORAGE_SDK_DIR}/native/bin/${RTV_DIRECTSTORAGE_ARCH}")
    if(EXISTS "${RTV_DIRECTSTORAGE_LIB}" AND EXISTS "${RTV_DIRECTSTORAGE_BIN_DIR}/dstorage.dll" AND EXISTS "${RTV_DIRECTSTORAGE_BIN_DIR}/dstoragecore.dll")
        target_compile_definitions(rtvulkan PRIVATE
            RTV_WITH_DIRECTSTORAGE=1
            "RTV_DIRECTSTORAGE_SDK_DIR=\"${DIRECTSTORAGE_SDK_DIR}\"")
        target_include_directories(rtvulkan PRIVATE "${DIRECTSTORAGE_SDK_DIR}/native/include")
        target_link_libraries(rtvulkan PRIVATE "${RTV_DIRECTSTORAGE_LIB}")
        foreach(RTV_DIRECTSTORAGE_RUNTIME_NAME IN ITEMS dstorage.dll dstoragecore.dll)
            add_custom_command(TARGET rtvulkan POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${RTV_DIRECTSTORAGE_BIN_DIR}/${RTV_DIRECTSTORAGE_RUNTIME_NAME}"
                    "$<TARGET_FILE_DIR:rtvulkan>/${RTV_DIRECTSTORAGE_RUNTIME_NAME}")
        endforeach()
        message(STATUS "DirectStorage SDK enabled: ${DIRECTSTORAGE_SDK_DIR} (${RTV_DIRECTSTORAGE_ARCH})")
    else()
        message(WARNING "DirectStorage SDK headers found but ${RTV_DIRECTSTORAGE_ARCH} lib/runtime files are missing under ${DIRECTSTORAGE_SDK_DIR}")
    endif()
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

set(RTV_STREAMLINE_INCLUDE_DIR "")
set(RTV_STREAMLINE_RUNTIME_DIR "")
set(RTV_STREAMLINE_RUNTIME_DLL "")
set(RTV_STREAMLINE_IMPORT_LIB "")
set(RTV_STREAMLINE_FEATURE_DLLS "")
set(RTV_STREAMLINE_MISSING_OPTIONAL_PLUGINS "")
set(RTV_STREAMLINE_HAS_DLSS OFF)
set(RTV_STREAMLINE_HAS_DLSS_RR OFF)
set(RTV_STREAMLINE_HAS_DLSS_FG OFF)
set(RTV_STREAMLINE_HAS_REFLEX OFF)
set(RTV_STREAMLINE_HAS_NIS OFF)
set(RTV_STREAMLINE_HAS_NRD OFF)
if(NOT STREAMLINE_SDK_DIR STREQUAL "")
    foreach(CANDIDATE IN ITEMS
        "${STREAMLINE_SDK_DIR}/include"
        "${STREAMLINE_SDK_DIR}/inc")
        if(EXISTS "${CANDIDATE}/sl.h" AND EXISTS "${CANDIDATE}/sl_consts.h")
            set(RTV_STREAMLINE_INCLUDE_DIR "${CANDIDATE}")
            break()
        endif()
    endforeach()

    foreach(CANDIDATE IN ITEMS
        "${STREAMLINE_SDK_DIR}/bin/x64"
        "${STREAMLINE_SDK_DIR}/bin"
        "${STREAMLINE_SDK_DIR}/lib/x64"
        "${STREAMLINE_SDK_DIR}/lib/Windows_x86_64")
        if(EXISTS "${CANDIDATE}/sl.interposer.dll")
            set(RTV_STREAMLINE_RUNTIME_DIR "${CANDIDATE}")
            set(RTV_STREAMLINE_RUNTIME_DLL "${CANDIDATE}/sl.interposer.dll")
            break()
        endif()
    endforeach()

    foreach(CANDIDATE IN ITEMS
        "${STREAMLINE_SDK_DIR}/lib/x64/sl.interposer.lib"
        "${STREAMLINE_SDK_DIR}/lib/Windows_x86_64/sl.interposer.lib"
        "${STREAMLINE_SDK_DIR}/bin/x64/sl.interposer.lib")
        if(EXISTS "${CANDIDATE}")
            set(RTV_STREAMLINE_IMPORT_LIB "${CANDIDATE}")
            break()
        endif()
    endforeach()

    foreach(PLUGIN IN ITEMS sl.dlss.dll sl.dlss_d.dll sl.dlss_g.dll sl.reflex.dll sl.nis.dll sl.nrd.dll sl.common.dll)
        if(NOT RTV_STREAMLINE_RUNTIME_DIR STREQUAL "" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/${PLUGIN}")
            list(APPEND RTV_STREAMLINE_FEATURE_DLLS "${RTV_STREAMLINE_RUNTIME_DIR}/${PLUGIN}")
        else()
            list(APPEND RTV_STREAMLINE_MISSING_OPTIONAL_PLUGINS "${PLUGIN}")
        endif()
    endforeach()

    set(RTV_STREAMLINE_HAS_DLSS OFF)
    set(RTV_STREAMLINE_HAS_DLSS_RR OFF)
    set(RTV_STREAMLINE_HAS_DLSS_FG OFF)
    set(RTV_STREAMLINE_HAS_REFLEX OFF)
    set(RTV_STREAMLINE_HAS_NIS OFF)
    set(RTV_STREAMLINE_HAS_NRD OFF)
    if(NOT RTV_STREAMLINE_INCLUDE_DIR STREQUAL "" AND NOT RTV_STREAMLINE_RUNTIME_DIR STREQUAL "")
        if(EXISTS "${RTV_STREAMLINE_INCLUDE_DIR}/sl_dlss.h" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/sl.dlss.dll")
            set(RTV_STREAMLINE_HAS_DLSS ON)
        endif()
        if(EXISTS "${RTV_STREAMLINE_INCLUDE_DIR}/sl_dlss_d.h" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/sl.dlss_d.dll")
            set(RTV_STREAMLINE_HAS_DLSS_RR ON)
        endif()
        if(EXISTS "${RTV_STREAMLINE_INCLUDE_DIR}/sl_dlss_g.h" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/sl.dlss_g.dll")
            set(RTV_STREAMLINE_HAS_DLSS_FG ON)
        endif()
        if(EXISTS "${RTV_STREAMLINE_INCLUDE_DIR}/sl_reflex.h" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/sl.reflex.dll")
            set(RTV_STREAMLINE_HAS_REFLEX ON)
        endif()
        if(EXISTS "${RTV_STREAMLINE_INCLUDE_DIR}/sl_nis.h" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/sl.nis.dll")
            set(RTV_STREAMLINE_HAS_NIS ON)
        endif()
        if(EXISTS "${RTV_STREAMLINE_INCLUDE_DIR}/sl_nrd.h" AND EXISTS "${RTV_STREAMLINE_RUNTIME_DIR}/sl.nrd.dll")
            set(RTV_STREAMLINE_HAS_NRD ON)
        endif()
    endif()

    if(NOT RTV_STREAMLINE_INCLUDE_DIR STREQUAL "")
        target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_SDK_CONFIGURED=1)
        target_include_directories(rtvulkan PRIVATE "${RTV_STREAMLINE_INCLUDE_DIR}")
        if(RTV_STREAMLINE_HAS_DLSS)
            target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_HAS_DLSS=1)
        endif()
        if(RTV_STREAMLINE_HAS_DLSS_RR)
            target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_HAS_DLSS_RR=1)
        endif()
        if(RTV_STREAMLINE_HAS_DLSS_FG)
            target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_HAS_DLSS_FG=1)
        endif()
        if(RTV_STREAMLINE_HAS_REFLEX)
            target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_HAS_REFLEX=1)
        endif()
        if(RTV_STREAMLINE_HAS_NIS)
            target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_HAS_NIS=1)
        endif()
        if(RTV_STREAMLINE_HAS_NRD)
            target_compile_definitions(rtvulkan PRIVATE RTV_STREAMLINE_HAS_NRD=1)
        endif()
    endif()

    if(NOT RTV_STREAMLINE_RUNTIME_DLL STREQUAL "")
        target_compile_definitions(rtvulkan PRIVATE
            RTV_STREAMLINE_RUNTIME_ENABLED=1
            "RTV_STREAMLINE_RUNTIME_DIR=\"${RTV_STREAMLINE_RUNTIME_DIR}\"")
        add_custom_command(TARGET rtvulkan POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RTV_STREAMLINE_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:rtvulkan>/sl.interposer.dll")
        foreach(PLUGIN_DLL IN LISTS RTV_STREAMLINE_FEATURE_DLLS)
            get_filename_component(PLUGIN_NAME "${PLUGIN_DLL}" NAME)
            add_custom_command(TARGET rtvulkan POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PLUGIN_DLL}"
                    "$<TARGET_FILE_DIR:rtvulkan>/${PLUGIN_NAME}")
        endforeach()
    endif()
endif()

message(STATUS "Streamline SDK include dir: ${RTV_STREAMLINE_INCLUDE_DIR}")
message(STATUS "Streamline runtime dll: ${RTV_STREAMLINE_RUNTIME_DLL}")
message(STATUS "Streamline import lib: ${RTV_STREAMLINE_IMPORT_LIB}")
message(STATUS "Streamline feature dlls: ${RTV_STREAMLINE_FEATURE_DLLS}")
message(STATUS "Streamline feature flags: DLSS=${RTV_STREAMLINE_HAS_DLSS}; DLSS_RR=${RTV_STREAMLINE_HAS_DLSS_RR}; DLSS_FG=${RTV_STREAMLINE_HAS_DLSS_FG}; Reflex=${RTV_STREAMLINE_HAS_REFLEX}; NIS=${RTV_STREAMLINE_HAS_NIS}; NRD=${RTV_STREAMLINE_HAS_NRD}")
message(STATUS "Streamline missing optional plugins: ${RTV_STREAMLINE_MISSING_OPTIONAL_PLUGINS}")

if(WIN32)
    target_link_libraries(rtvulkan PRIVATE Comdlg32 User32 Shell32 Dwmapi)
endif()

add_executable(rtv_bvh_benchmark
    tools/bvh_benchmark.cpp
    src/rtv/BvhBuilder.cpp
    src/rtv/BvhCollapse.cpp
    src/rtv/MortonCodes.cpp
    src/rtv/SahBuilder.cpp
)
target_include_directories(rtv_bvh_benchmark PRIVATE include)
target_link_libraries(rtv_bvh_benchmark PRIVATE glm::glm)

if(MSVC)
    target_compile_options(rtvulkan PRIVATE /W4 /permissive- /EHsc /bigobj)
    target_compile_options(rtv_bvh_benchmark PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(rtvulkan PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(rtv_bvh_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()
