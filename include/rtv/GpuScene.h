#pragma once

#include "rtv/Buffer.h"
#include "rtv/BindlessResources.h"
#include "rtv/Image.h"
#include "rtv/OpacityMicromapPreprocess.h"
#include "rtv/SceneCache.h"
#include "rtv/TextureAsset.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace rtv {

class BufferUploader;
class ResourceAllocator;
class AssetManager;
struct SceneAsset;

struct alignas(16) CameraUniform {
    glm::vec4 pos{};
    glm::vec4 forward{};
    glm::vec4 right{};
    glm::vec4 up{};
    uint32_t frameCount = 0;
    uint32_t temporalFrameIndex = 0;
    float effectiveJitterScale = 0.0f;
    uint32_t cameraMoving = 0;
    float sunIntensity = 1.0f;
    float skyIntensity = 0.8f;
    float exposure = 1.0f;
    uint32_t pathTracingEnabled = 1;
    uint32_t maxBounces = 8;
    uint32_t sunlightEnabled = 1;
    uint32_t directLightingEnabled = 1;
    float fovY = 60.0f * 0.017453292519943295f;
    float sunAngularRadius = 0.00465f;
    float indirectStrength = 1.0f;
    uint32_t environmentDirectSamples = 1;
    uint32_t padding0 = 0;
    glm::vec4 jitter{}; // xy = current subpixel jitter, zw = previous subpixel jitter
    glm::vec4 atmosphere{}; // x = sun elevation, y = ReSTIR mode, z = temporal history available, w = sun azimuth
    glm::vec4 renderControls{}; // x = shadow ray bias, y = shadow distance bias, z = firefly clamp, w = RR min survival
    glm::vec4 sunDirectionIlluminance{0.0f, 0.8240f, -0.5661f, 100000.0f};
    glm::vec4 sunColorAngularRadius{1.0f, 1.0f, 1.0f, 0.00465f};
    glm::uvec4 restirGiControls{24u, 0u, 0u, 1u}; // x = temporal max age, y = half-resolution reuse, z = visibility ray budget, w = specular AA enabled
    glm::uvec4 pathTraceControls{1u, 1u, 0u, 0u}; // x = requested SPP, y = limit to 1 SPP, z = RT counters, w = caustic visibility
    glm::vec4 dofControls{0.0f, 10.0f, 0.0f, 0.0f}; // x = aperture radius, y = focus distance, z = blade count, w = bokeh rotation
    glm::vec4 motionBlurControls{0.0f, 0.0f, 1.0f, 0.0f}; // x = enabled, y = shutter open, z = shutter close, w = reserved
    glm::vec4 volumeControls{0.0f, 0.0f, 0.0f, 0.0f}; // x = enabled, y = sigma_s, z = sigma_a, w = anisotropy
};

static_assert(offsetof(CameraUniform, jitter) == 128, "CameraUniform::jitter must match std140 layout");
static_assert(offsetof(CameraUniform, atmosphere) == 144, "CameraUniform::atmosphere must match std140 layout");
static_assert(offsetof(CameraUniform, renderControls) == 160, "CameraUniform::renderControls must match std140 layout");
static_assert(offsetof(CameraUniform, sunDirectionIlluminance) == 176, "CameraUniform::sunDirectionIlluminance must match std140 layout");
static_assert(offsetof(CameraUniform, sunColorAngularRadius) == 192, "CameraUniform::sunColorAngularRadius must match std140 layout");
static_assert(offsetof(CameraUniform, restirGiControls) == 208, "CameraUniform::restirGiControls must match std140 layout");
static_assert(offsetof(CameraUniform, pathTraceControls) == 224, "CameraUniform::pathTraceControls must match std140 layout");
static_assert(offsetof(CameraUniform, dofControls) == 240, "CameraUniform::dofControls must match std140 layout");
static_assert(offsetof(CameraUniform, motionBlurControls) == 256, "CameraUniform::motionBlurControls must match std140 layout");
static_assert(offsetof(CameraUniform, volumeControls) == 272, "CameraUniform::volumeControls must match std140 layout");
static_assert(sizeof(CameraUniform) == 288, "CameraUniform size must match std140 layout");

struct MeshParamsUniform {
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t bvhNodeCount = 0;
    uint32_t materialCount = 0;
    uint32_t enabled = 0;
    uint32_t sphereCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t instanceCount = 0;
    uint32_t lightCount = 0;
    float emissiveTotalArea = 0.0f;
    uint32_t meshCount = 0;
    uint32_t localVertexCount = 0;
    uint32_t localIndexCount = 0;
    uint32_t localBvhNodeCount = 0;
    uint32_t localTriangleCount = 0;
    uint32_t tlasNodeCount = 0;
    uint32_t tlasInstanceIndexCount = 0;
    uint32_t padding2 = 0;
    uint32_t padding3 = 0;
    uint32_t padding4 = 0;
};

struct GpuMeshRecord {
    glm::uvec4 vertexIndexData{};
    glm::uvec4 primitiveData{};
    glm::uvec4 bvhData{};
    glm::uvec4 flags{};
};

struct GpuPrimitiveRecord {
    glm::uvec4 indexData{};
    glm::uvec4 metadata{};
};

struct GpuInstanceRecord {
    glm::mat4 transform{1.0f};
    glm::mat4 inverseTransform{1.0f};
    glm::mat4 normalTransform{1.0f};
    glm::mat4 prevTransform{1.0f};
    glm::uvec4 metadata{};
};

struct GpuLocalVertex {
    glm::vec4 positionUvX{};
    glm::vec4 normalUvY{};
    glm::vec4 tangent{};
};

struct GpuInstanceBoundsRecord {
    glm::vec4 bmin{};
    glm::vec4 bmax{};
};

struct GpuLightRecord {
    glm::uvec4 metadata{};
    glm::vec4 data0{};
    glm::vec4 data1{};
    glm::vec4 data2{};
    glm::vec4 data3{};
};

struct EnvParamsUniform {
    uint32_t enabled = 1;
    float intensity = 1.0f;
    float rotation = 0.0f;
    uint32_t width = 1;
    uint32_t height = 1;
    float backgroundIntensity = 0.35f;
    uint32_t procedural = 1;
    uint32_t skyCdfWidth = 256;
    float invTotalLum = 1.0f;
    uint32_t skyCdfHeight = 144;
    float pad4 = 0.0f;
    float pad5 = 0.0f;
};

enum class AccelUpdateMode : uint32_t {
    Static,
    RefitTransform,
    RefitDeform,
    Rebuild,
};

struct RayTracingMeshBuildInput {
    uint32_t meshIndex = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t primitiveOffset = 0;
    uint32_t primitiveCount = 0;
    bool containsAlphaTestedGeometry = false;
    bool containsBlendedGeometry = false;
    bool opaqueTraversalSafe = false;
    AccelUpdateMode updateMode = AccelUpdateMode::Static;
};

struct RayTracingGeometryStats {
    uint32_t opaquePrimitiveCount = 0;
    uint32_t alphaTestedPrimitiveCount = 0;
    uint32_t blendedPrimitiveCount = 0;
    uint32_t opaqueTriangleCount = 0;
    uint32_t alphaTestedTriangleCount = 0;
    uint32_t blendedTriangleCount = 0;
    uint32_t meshCountWithOnlyOpaqueGeometry = 0;
    uint32_t meshCountWithAlphaTestedGeometry = 0;
    uint32_t meshCountWithBlendedGeometry = 0;
};

struct RayTracingInstanceBuildInput {
    uint32_t instanceIndex = 0;
    uint32_t meshIndex = 0;
    glm::mat4 transform{1.0f};
    glm::mat4 previousTransform{1.0f};
    uint32_t flags = 0;
    bool visible = true;
};

class GpuScene {
public:
    GpuScene(
        ResourceAllocator& allocator,
        BufferUploader& uploader,
        const SceneAsset* importedScene = nullptr,
        const AssetManager* assets = nullptr,
        std::optional<std::filesystem::path> environmentPath = std::nullopt,
        std::optional<std::filesystem::path> sceneCachePath = std::nullopt,
        uint32_t opacityMicromapSubdivisionLevel = kDefaultOpacityMicromapSubdivisionLevel);
    ~GpuScene();

    [[nodiscard]] Buffer& vertices() { return *vertices_; }
    [[nodiscard]] Buffer& indices() { return *indices_; }
    [[nodiscard]] Buffer& bvhNodes() { return *bvhNodes_; }
    [[nodiscard]] Buffer& triangles() { return *triangles_; }
    [[nodiscard]] Buffer& materials() { return *materials_; }
    [[nodiscard]] Buffer& spheres() { return *spheres_; }
    [[nodiscard]] Buffer& meshRecords() { return *meshRecords_; }
    [[nodiscard]] Buffer& primitiveRecords() { return *primitiveRecords_; }
    [[nodiscard]] Buffer& instanceRecords() { return *instanceRecords_; }
    [[nodiscard]] Buffer& rtTriangleMaterialIds() { return *rtTriangleMaterialIds_; }
    [[nodiscard]] Buffer& lightRecords() { return *lightRecords_; }
    [[nodiscard]] Buffer& localVertices() { return *localVertices_; }
    [[nodiscard]] Buffer& localIndices() { return *localIndices_; }
    [[nodiscard]] Buffer& instanceBounds() { return *instanceBounds_; }
    [[nodiscard]] Buffer& localBvhNodes() { return *localBvhNodes_; }
    [[nodiscard]] Buffer& localTriangles() { return *localTriangles_; }
    [[nodiscard]] Buffer& tlasNodes() { return *tlasNodes_; }
    [[nodiscard]] Buffer& tlasInstanceIndices() { return *tlasInstanceIndices_; }
    [[nodiscard]] Buffer& envRows() { return *envRows_; }
    [[nodiscard]] Buffer& envCols() { return *envCols_; }
    [[nodiscard]] Buffer& meshParamsBuffer() { return *meshParamsBuffer_; }
    [[nodiscard]] Buffer& envParamsBuffer() { return *envParamsBuffer_; }
    [[nodiscard]] Buffer& lightBvhNodes() { return *lightBvhNodes_; }
    [[nodiscard]] Image& environmentImage() { return *environmentImage_; }
    [[nodiscard]] const Buffer& meshRecords() const { return *meshRecords_; }
    [[nodiscard]] const Buffer& primitiveRecords() const { return *primitiveRecords_; }
    [[nodiscard]] const Buffer& instanceRecords() const { return *instanceRecords_; }
    [[nodiscard]] const Buffer& rtTriangleMaterialIds() const { return *rtTriangleMaterialIds_; }
    [[nodiscard]] const Buffer& localVertices() const { return *localVertices_; }
    [[nodiscard]] const Buffer& localIndices() const { return *localIndices_; }
    [[nodiscard]] const Buffer& instanceBounds() const { return *instanceBounds_; }
    [[nodiscard]] const Buffer& localBvhNodes() const { return *localBvhNodes_; }
    [[nodiscard]] const Buffer& localTriangles() const { return *localTriangles_; }
    [[nodiscard]] const Buffer& tlasNodes() const { return *tlasNodes_; }
    [[nodiscard]] const Buffer& tlasInstanceIndices() const { return *tlasInstanceIndices_; }
    [[nodiscard]] VkSampler environmentSampler() const { return environmentSampler_; }
    [[nodiscard]] const std::vector<VkDescriptorImageInfo>& materialTextureDescriptors() const { return materialTextureTable_.descriptors(); }
    [[nodiscard]] std::vector<VkDescriptorImageInfo> materialCombinedDescriptors() const;
    [[nodiscard]] VkSampler materialSampler() const { return materialSampler_; }
    [[nodiscard]] const BindlessTextureTable& materialTextureTable() const { return materialTextureTable_; }
    [[nodiscard]] VkImageView materialTextureImageView(uint32_t index) const { return materialTextureTable_.imageView(index); }
    [[nodiscard]] uint32_t materialTextureCount() const { return materialTextureTable_.residentCount(); }
    [[nodiscard]] float materialTextureAnisotropy() const { return materialTextureAnisotropy_; }

    [[nodiscard]] const MeshParamsUniform& meshParams() const { return meshParams_; }
    [[nodiscard]] const EnvParamsUniform& envParams() const { return envParams_; }
    [[nodiscard]] const std::vector<RayTracingMeshBuildInput>& rayTracingMeshes() const { return rayTracingMeshes_; }
    [[nodiscard]] const std::vector<RayTracingInstanceBuildInput>& rayTracingInstances() const { return rayTracingInstances_; }
    [[nodiscard]] const RayTracingGeometryStats& rayTracingGeometryStats() const { return rayTracingGeometryStats_; }
    [[nodiscard]] const std::vector<GpuPrimitiveRecord>& primitiveRecordsCpu() const { return primitiveRecordCpu_; }
    [[nodiscard]] const OpacityMicromapCpuData& opacityMicromapData() const { return opacityMicromapData_; }

    bool setEnvironmentControls(bool enabled, float intensity, float rotation, float backgroundIntensity);
    bool setSkyCdfDimensions(uint32_t width, uint32_t height);
    bool setMaterialTextureAnisotropy(float anisotropy, uint64_t retireFrame);
    void releaseRetiredMaterialSamplers(uint64_t completedFrame);
    void loadEnvironment(BufferUploader& uploader, const std::filesystem::path& path, uint64_t retireFrame);
    bool updateImportedMaterials(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets);
    bool updateSceneLights(BufferUploader& uploader, const SceneAsset& scene, uint64_t retireFrame);
    bool updateInstanceTransforms(BufferUploader& uploader, const SceneAsset& scene, const AssetManager& assets, uint64_t retireFrame);

private:
    struct RetiredBuffer {
        std::unique_ptr<Buffer> buffer;
        uint64_t retireFrame = 0;
    };
    struct RetiredImage {
        std::unique_ptr<Image> image;
        uint64_t retireFrame = 0;
    };

    void createCornellBox(BufferUploader& uploader);
    void createImportedScene(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets);
    void createImportedSceneFromCache(BufferUploader& uploader, const CachedScene& cached);
    void createDefaultMaterialTexture(BufferUploader& uploader);
    void createImportedMaterialTextures(BufferUploader& uploader, const SceneAsset& importedScene, const AssetManager& assets);
    void createCachedMaterialTextures(BufferUploader& uploader, const CachedScene& cached);
    void createEnvironment(BufferUploader& uploader);
    void uploadEnvironmentParams();
    void uploadLightRecords(BufferUploader& uploader, std::vector<GpuLightRecord> lightRecords, float totalWeight, uint64_t retireFrame = 0);
    void uploadLightBvh(BufferUploader& uploader, const std::vector<GpuLightRecord>& lightRecords, uint64_t retireFrame = 0);
    void destroyMaterialTextureSamplers();
    void retireMaterialTextureSampler(VkSampler sampler, uint64_t retireFrame);
    void retireBuffer(std::unique_ptr<Buffer> buffer, uint64_t retireFrame);
    void retireImage(std::unique_ptr<Image> image, uint64_t retireFrame);
    void retireEnvironmentResources(uint64_t retireFrame);
    void recreateMaterialTextureSamplers(uint64_t retireFrame);
    void rebuildMaterialSamplerDescriptors(uint32_t slotCount);
    [[nodiscard]] static uint32_t textureSlotIndexFor(const SceneAsset& scene, TextureAssetHandle texture, uint32_t maxSlots);

    ResourceAllocator& allocator_;
    std::optional<std::filesystem::path> environmentPath_;
    std::optional<std::filesystem::path> sceneCachePath_;
    uint32_t opacityMicromapSubdivisionLevel_ = kDefaultOpacityMicromapSubdivisionLevel;
    std::unique_ptr<Buffer> vertices_;
    std::unique_ptr<Buffer> indices_;
    std::unique_ptr<Buffer> bvhNodes_;
    std::unique_ptr<Buffer> triangles_;
    std::unique_ptr<Buffer> materials_;
    std::unique_ptr<Buffer> spheres_;
    std::unique_ptr<Buffer> meshRecords_;
    std::unique_ptr<Buffer> primitiveRecords_;
    std::unique_ptr<Buffer> instanceRecords_;
    std::unique_ptr<Buffer> rtTriangleMaterialIds_;
    std::unique_ptr<Buffer> lightRecords_;
    std::unique_ptr<Buffer> localVertices_;
    std::unique_ptr<Buffer> localIndices_;
    std::unique_ptr<Buffer> instanceBounds_;
    std::unique_ptr<Buffer> localBvhNodes_;
    std::unique_ptr<Buffer> localTriangles_;
    std::unique_ptr<Buffer> tlasNodes_;
    std::unique_ptr<Buffer> tlasInstanceIndices_;
    std::unique_ptr<Buffer> envRows_;
    std::unique_ptr<Buffer> envCols_;
    std::unique_ptr<Buffer> meshParamsBuffer_;
    std::unique_ptr<Buffer> envParamsBuffer_;
    std::unique_ptr<Buffer> lightBvhNodes_;
    std::unique_ptr<Image> environmentImage_;
    BindlessTextureTable materialTextureTable_;
    VkSampler environmentSampler_ = VK_NULL_HANDLE;
    VkSampler materialSampler_ = VK_NULL_HANDLE;
    std::vector<VkSampler> materialTextureSamplers_;
    struct RetiredMaterialSampler {
        VkSampler sampler = VK_NULL_HANDLE;
        uint64_t retireFrame = 0;
    };
    std::vector<RetiredMaterialSampler> retiredMaterialSamplers_;
    std::vector<RetiredBuffer> retiredBuffers_;
    std::vector<RetiredImage> retiredImages_;
    std::vector<TextureSamplerDesc> materialTextureSamplerDescs_;
    TextureSamplerDesc materialSamplerDesc_{};
    float materialTextureAnisotropy_ = 8.0f;
    MeshParamsUniform meshParams_{};
    EnvParamsUniform envParams_{};
    std::vector<RayTracingMeshBuildInput> rayTracingMeshes_;
    std::vector<RayTracingInstanceBuildInput> rayTracingInstances_;
    RayTracingGeometryStats rayTracingGeometryStats_{};
    std::vector<GpuPrimitiveRecord> primitiveRecordCpu_;
    OpacityMicromapCpuData opacityMicromapData_{};
    std::vector<GpuLightRecord> emissiveLightRecords_;
    std::vector<GpuInstanceRecord> instanceRecordCpu_;
};

} // namespace rtv
