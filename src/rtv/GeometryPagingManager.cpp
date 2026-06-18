#include "rtv/GeometryPagingManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace rtv {

// -- Mesh Cluster Paging --

void GeometryPagingManager::registerMeshClusters(AssetGuid meshGuid,
                                                   const std::vector<MeshCluster>& clusters) {
    for (const MeshCluster& c : clusters) {
        MeshCluster cluster = c;
        cluster.clusterId = nextClusterId_++;
        cluster.parentMeshGuid = meshGuid;
        clusters_.push_back(std::move(cluster));
    }
}

void GeometryPagingManager::markClusterGeometryResident(uint32_t clusterId, bool resident) {
    for (MeshCluster& cluster : clusters_) {
        if (cluster.clusterId == clusterId) {
            cluster.geometryResident = resident;
            return;
        }
    }
}

void GeometryPagingManager::markClusterBlasResident(uint32_t clusterId, bool resident) {
    for (MeshCluster& cluster : clusters_) {
        if (cluster.clusterId == clusterId) {
            cluster.blasResident = resident;
            return;
        }
    }
}

void GeometryPagingManager::updateClusterPriorities(const std::array<float, 3>& cameraPosition,
                                                      const std::array<float, 3>& cameraDirection,
                                                      float fovYRadians, float screenHeight) {
    for (MeshCluster& cluster : clusters_) {
        const float screenSize = computeClusterScreenSize(
            cluster, cameraPosition, cameraDirection, fovYRadians, screenHeight);

        // Frustum culling: check if cluster is behind the camera.
        const float cx = (cluster.boundsMin[0] + cluster.boundsMax[0]) * 0.5f;
        const float cy = (cluster.boundsMin[1] + cluster.boundsMax[1]) * 0.5f;
        const float cz = (cluster.boundsMin[2] + cluster.boundsMax[2]) * 0.5f;
        const float dx = cx - cameraPosition[0];
        const float dy = cy - cameraPosition[1];
        const float dz = cz - cameraPosition[2];
        const float dot = dx * cameraDirection[0] + dy * cameraDirection[1] + dz * cameraDirection[2];

        cluster.screenSpacePriority = screenSize;
        cluster.renderable = screenSize > 0.001f && dot > 0.0f;  // In front of camera and large enough.
    }
}

std::vector<const MeshCluster*> GeometryPagingManager::prioritizedClusters() const {
    std::vector<const MeshCluster*> result;
    result.reserve(clusters_.size());
    for (const MeshCluster& cluster : clusters_) {
        result.push_back(&cluster);
    }
    std::sort(result.begin(), result.end(),
        [](const MeshCluster* a, const MeshCluster* b) {
            return a->screenSpacePriority > b->screenSpacePriority;
        });
    return result;
}

// -- Virtual Texture Paging --

void GeometryPagingManager::registerTexturePages(AssetGuid textureGuid,
                                                   const std::vector<TexturePage>& pages) {
    for (const TexturePage& p : pages) {
        TexturePage page = p;
        page.pageId = nextPageId_++;
        page.textureGuid = textureGuid;
        pages_.push_back(std::move(page));
    }
}

void GeometryPagingManager::markPageResident(uint32_t pageId, bool resident) {
    for (TexturePage& page : pages_) {
        if (page.pageId == pageId) {
            page.resident = resident;
            return;
        }
    }
}

void GeometryPagingManager::processFeedback(const std::vector<uint32_t>& requestedPageIds,
                                              uint64_t frameSerial) {
    // Clear previous feedback flags.
    for (TexturePage& page : pages_) {
        page.feedbackRequested = false;
    }

    // Mark pages that were requested by the renderer.
    for (uint32_t pageId : requestedPageIds) {
        for (TexturePage& page : pages_) {
            if (page.pageId == pageId) {
                page.feedbackRequested = true;
                page.lastRequestFrame = frameSerial;
                // Boost priority for requested pages.
                page.priority = std::min(1.0f, page.priority + 0.3f);
                break;
            }
        }
    }

    // Decay priority for non-requested pages.
    for (TexturePage& page : pages_) {
        if (!page.feedbackRequested) {
            page.priority = std::max(0.0f, page.priority - 0.05f);
        }
    }
}

std::vector<const TexturePage*> GeometryPagingManager::prioritizedPages() const {
    std::vector<const TexturePage*> result;
    result.reserve(pages_.size());
    for (const TexturePage& page : pages_) {
        result.push_back(&page);
    }
    std::sort(result.begin(), result.end(),
        [](const TexturePage* a, const TexturePage* b) {
            return a->priority > b->priority;
        });
    return result;
}

// -- Query --

const MeshCluster* GeometryPagingManager::findCluster(uint32_t clusterId) const {
    for (const MeshCluster& cluster : clusters_) {
        if (cluster.clusterId == clusterId) return &cluster;
    }
    return nullptr;
}

const TexturePage* GeometryPagingManager::findPage(uint32_t pageId) const {
    for (const TexturePage& page : pages_) {
        if (page.pageId == pageId) return &page;
    }
    return nullptr;
}

float GeometryPagingManager::computeClusterScreenSize(const MeshCluster& cluster,
                                                        const std::array<float, 3>& cameraPos,
                                                        const std::array<float, 3>& cameraDir,
                                                        float fovYRadians,
                                                        float screenHeight) const {
    // NOTE: This is intentionally duplicated from HlodStreamingManager::computeScreenSize
    // to avoid a cross-dependency between HLOD and geometry paging managers. Keep in sync.
    const float cx = (cluster.boundsMin[0] + cluster.boundsMax[0]) * 0.5f;
    const float cy = (cluster.boundsMin[1] + cluster.boundsMax[1]) * 0.5f;
    const float cz = (cluster.boundsMin[2] + cluster.boundsMax[2]) * 0.5f;

    const float extX = cluster.boundsMax[0] - cluster.boundsMin[0];
    const float extY = cluster.boundsMax[1] - cluster.boundsMin[1];
    const float extZ = cluster.boundsMax[2] - cluster.boundsMin[2];
    const float radius = std::sqrt(extX * extX + extY * extY + extZ * extZ) * 0.5f;

    const float dx = cx - cameraPos[0];
    const float dy = cy - cameraPos[1];
    const float dz = cz - cameraPos[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (distance <= radius) return 1.0f;

    // Project sphere onto screen (same formula as HlodStreamingManager).
    (void)cameraDir;
    const float pixelSize = (radius / distance) *
        (screenHeight / (2.0f * std::tan(fovYRadians * 0.5f)));
    return pixelSize / screenHeight;
}

nlohmann::json geometryPagingToJson(const std::vector<MeshCluster>& clusters,
                                      const std::vector<TexturePage>& pages) {
    nlohmann::json clustersJson = nlohmann::json::array();
    for (const MeshCluster& c : clusters) {
        clustersJson.push_back({
            {"cluster_id", c.clusterId},
            {"parent_mesh_guid", c.parentMeshGuid},
            {"vertex_count", c.vertexCount},
            {"index_count", c.indexCount},
            {"geometry_resident", c.geometryResident},
            {"blas_resident", c.blasResident},
            {"screen_space_priority", c.screenSpacePriority},
            {"renderable", c.renderable},
            {"omm_eligible", c.ommEligible},
        });
    }

    nlohmann::json pagesJson = nlohmann::json::array();
    for (const TexturePage& p : pages) {
        pagesJson.push_back({
            {"page_id", p.pageId},
            {"texture_guid", p.textureGuid},
            {"mip_level", p.mipLevel},
            {"page_x", p.pageX},
            {"page_y", p.pageY},
            {"resident", p.resident},
            {"priority", p.priority},
            {"feedback_requested", p.feedbackRequested},
        });
    }

    return nlohmann::json{
        {"clusters", clustersJson},
        {"pages", pagesJson},
    };
}

} // namespace rtv
