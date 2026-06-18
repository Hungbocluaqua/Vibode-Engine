#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

// A mesh cluster/page for fine-grained geometry streaming.
// Each cluster represents a contiguous range of vertices and indices
// within a parent mesh, with its own bounds, material range, and
// optional BLAS.
struct MeshCluster {
    uint32_t clusterId = 0;
    AssetGuid parentMeshGuid;

    // Byte ranges within the parent mesh's vertex/index buffers.
    uint64_t vertexByteOffset = 0;
    uint64_t vertexByteCount = 0;
    uint64_t indexByteOffset = 0;
    uint64_t indexByteCount = 0;

    // Vertex/index counts.
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;

    // Cluster bounds for screen-space priority and culling.
    std::array<float, 3> boundsMin = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> boundsMax = {0.0f, 0.0f, 0.0f};

    // Material range within the parent mesh's material slots.
    uint32_t materialStartSlot = 0;
    uint32_t materialSlotCount = 1;

    // Whether this cluster has its own compact BLAS.
    bool hasBlas = false;
    bool blasResident = false;

    // Whether this cluster's vertex/index data is GPU-resident.
    bool geometryResident = false;

    // Screen-space priority (computed per frame).
    float screenSpacePriority = 0.0f;

    // Whether the cluster should be rendered this frame.
    bool renderable = false;

    // OMM eligibility.
    bool ommEligible = false;
};

// A virtual texture page table entry.
struct TexturePage {
    uint32_t pageId = 0;
    AssetGuid textureGuid;

    // Page coordinates within the texture's mip chain.
    uint32_t mipLevel = 0;
    uint32_t pageX = 0;
    uint32_t pageY = 0;
    uint32_t pageSize = 64;  // Typical: 64x64 or 128x128.

    // Whether this page is GPU-resident.
    bool resident = false;

    // When this page was last requested.
    uint64_t lastRequestFrame = 0;

    // Screen-space priority.
    float priority = 0.0f;

    // Feedback from previous frame.
    bool feedbackRequested = false;
};

// Manages geometry paging (mesh clusters) and virtual texturing (texture pages)
// for fine-grained streaming of very large assets.
//
// Initial implementation: static opaque meshes with cluster tables.
// Virtual texturing: page tables with sparse residency hints.
//
// Longer-term (post-initial milestone):
// - Meshlet-level streaming.
// - Sparse image residency (VK_EXT_image_drm_format_modifier / sparse binding).
// - Texture tile cache with anisotropic mip priority.
// - Feedback pass to determine needed mips/pages.
class GeometryPagingManager final : private NonCopyable {
public:
    GeometryPagingManager() = default;

    // -- Mesh Cluster Paging --

    // Register a mesh with its cluster table.
    void registerMeshClusters(AssetGuid meshGuid,
                              const std::vector<MeshCluster>& clusters);

    // Mark a cluster as geometry-resident (vertex/index buffers uploaded).
    void markClusterGeometryResident(uint32_t clusterId, bool resident);

    // Mark a cluster's BLAS as resident.
    void markClusterBlasResident(uint32_t clusterId, bool resident);

    // Update per-cluster screen-space priority based on camera.
    void updateClusterPriorities(const std::array<float, 3>& cameraPosition,
                                  const std::array<float, 3>& cameraDirection,
                                  float fovYRadians, float screenHeight);

    // Get clusters sorted by priority (highest first) for streaming decisions.
    [[nodiscard]] std::vector<const MeshCluster*> prioritizedClusters() const;

    // -- Virtual Texture Paging --

    // Register a texture for virtual paging.
    void registerTexturePages(AssetGuid textureGuid,
                              const std::vector<TexturePage>& pages);

    // Mark a texture page as resident.
    void markPageResident(uint32_t pageId, bool resident);

    // Process feedback from a rendering pass to determine needed pages.
    void processFeedback(const std::vector<uint32_t>& requestedPageIds,
                          uint64_t frameSerial);

    // Get pages sorted by priority for streaming.
    [[nodiscard]] std::vector<const TexturePage*> prioritizedPages() const;

    // Query.
    [[nodiscard]] const MeshCluster* findCluster(uint32_t clusterId) const;
    [[nodiscard]] const TexturePage* findPage(uint32_t pageId) const;

    [[nodiscard]] const std::vector<MeshCluster>& clusters() const { return clusters_; }
    [[nodiscard]] const std::vector<TexturePage>& pages() const { return pages_; }

private:
    float computeClusterScreenSize(const MeshCluster& cluster,
                                    const std::array<float, 3>& cameraPos,
                                    const std::array<float, 3>& cameraDir,
                                    float fovYRadians, float screenHeight) const;

    std::vector<MeshCluster> clusters_;
    std::vector<TexturePage> pages_;
    uint32_t nextClusterId_ = 1;
    uint32_t nextPageId_ = 1;
};

[[nodiscard]] nlohmann::json geometryPagingToJson(
    const std::vector<MeshCluster>& clusters,
    const std::vector<TexturePage>& pages);

} // namespace rtv
