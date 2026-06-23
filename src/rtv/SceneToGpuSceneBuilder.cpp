#include "rtv/SceneToGpuSceneBuilder.h"

#include "rtv/AssetManager.h"
#include "rtv/SceneRenderSettingsSync.h"
#include "rtv/SunController.h"

#include <algorithm>
#include <cmath>

namespace rtv {

namespace {

[[nodiscard]] MaterialAssetHandle effectivePrimitiveMaterial(
    const MeshRenderer& renderer,
    const MeshPrimitiveAsset& primitive,
    size_t primitiveIndex) {
    MaterialAssetHandle material = primitive.material;
    const bool hasSlot = primitiveIndex < renderer.materialSlots.size();
    const bool hasOverride = hasSlot && renderer.materialSlots[primitiveIndex].overrideMaterial.has_value();
    if (hasSlot) {
        material = renderer.materialSlots[primitiveIndex].resolvedMaterial();
    }
    if (!hasOverride && renderer.activeMaterialVariantIndex != UINT32_MAX) {
        const auto variantIt = std::find_if(
            primitive.materialVariants.begin(),
            primitive.materialVariants.end(),
            [&](const MeshPrimitiveAsset::MaterialVariant& variant) {
                return variant.variantIndex == renderer.activeMaterialVariantIndex;
            });
        if (variantIt != primitive.materialVariants.end() && variantIt->material.valid()) {
            material = variantIt->material;
        }
    }
    return material;
}

void appendSceneMaterial(SceneAsset& scene, const AssetManager* assets, MaterialAssetHandle material);

[[nodiscard]] std::vector<float> effectiveMorphWeights(const MeshRenderer& renderer, const MeshAsset& mesh) {
    return !renderer.morphWeights.empty() ? renderer.morphWeights : mesh.defaultMorphWeights;
}

[[nodiscard]] bool hasActiveSkinningPayload(const MeshAsset& mesh) {
    for (const MeshVertex& vertex : mesh.vertices) {
        const bool hasWeights = vertex.weights.x > 0.0f || vertex.weights.y > 0.0f || vertex.weights.z > 0.0f || vertex.weights.w > 0.0f;
        if (hasWeights) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] uint32_t meshTriangleCount(const MeshAsset& mesh) {
    uint64_t triangles = 0;
    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        triangles += static_cast<uint64_t>(primitive.indexCount / 3u);
    }
    return triangles > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(triangles);
}

[[nodiscard]] PathTracerRenderer::GpuSkinningSourceVertex makeGpuSkinningSourceVertex(const MeshVertex& vertex) {
    return PathTracerRenderer::GpuSkinningSourceVertex{
        .positionUvX = {vertex.position, vertex.texcoord.x},
        .normalUvY = {vertex.normal, vertex.texcoord.y},
        .tangent = vertex.tangent,
        .color = vertex.color,
        .texcoord1 = {vertex.texcoord1, 0.0f, 0.0f},
        .joints = vertex.joints,
        .weights = vertex.weights,
    };
}

[[nodiscard]] GpuLocalVertex makeGpuSkinningMorphDelta(glm::vec3 position, glm::vec3 normal, glm::vec3 tangent) {
    return GpuLocalVertex{
        .positionUvX = {position, 0.0f},
        .normalUvY = {normal, 0.0f},
        .tangent = {tangent, 0.0f},
        .color = glm::vec4{0.0f},
        .texcoord1 = glm::vec4{0.0f},
    };
}

void appendGpuSkinningMorphDeltas(
    const MeshAsset& mesh,
    const std::vector<float>& morphWeights,
    std::vector<GpuLocalVertex>& morphDeltaPayload,
    GpuSkinningInstancePlan& plan,
    AnimatedGeometryStats& stats) {
    if (!plan.morphBeforeSkinning || plan.vertexCount == 0u || !hasActiveMorphTargetWeights(mesh, morphWeights)) {
        return;
    }

    const uint64_t morphDeltaOffset = static_cast<uint64_t>(morphDeltaPayload.size());
    plan.morphDeltaOffset = morphDeltaOffset > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(morphDeltaOffset);
    plan.morphDeltaCount = plan.vertexCount;
    plan.morphWeight = 1.0f;

    std::vector<glm::vec3> positionDeltas(plan.vertexCount, glm::vec3{0.0f});
    std::vector<glm::vec3> normalDeltas(plan.vertexCount, glm::vec3{0.0f});
    std::vector<glm::vec3> tangentDeltas(plan.vertexCount, glm::vec3{0.0f});

    for (const MeshPrimitiveAsset& primitive : mesh.primitives) {
        const size_t targetCount = std::min(primitive.morphTargets.size(), morphWeights.size());
        for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
            const float weight = morphWeights[targetIndex];
            if (std::abs(weight) <= 1.0e-6f) {
                continue;
            }
            const MeshPrimitiveAsset::MorphTarget& target = primitive.morphTargets[targetIndex];
            const size_t primitiveVertexCount = static_cast<size_t>(primitive.vertexCount);
            const size_t positionCount = std::min(primitiveVertexCount, target.positionDeltas.size());
            for (size_t i = 0; i < positionCount; ++i) {
                const size_t vertexIndex = static_cast<size_t>(primitive.firstVertex) + i;
                if (vertexIndex < positionDeltas.size()) {
                    positionDeltas[vertexIndex] += target.positionDeltas[i] * weight;
                }
            }
            const size_t normalCount = std::min(primitiveVertexCount, target.normalDeltas.size());
            for (size_t i = 0; i < normalCount; ++i) {
                const size_t vertexIndex = static_cast<size_t>(primitive.firstVertex) + i;
                if (vertexIndex < normalDeltas.size()) {
                    normalDeltas[vertexIndex] += target.normalDeltas[i] * weight;
                }
            }
            const size_t tangentCount = std::min(primitiveVertexCount, target.tangentDeltas.size());
            for (size_t i = 0; i < tangentCount; ++i) {
                const size_t vertexIndex = static_cast<size_t>(primitive.firstVertex) + i;
                if (vertexIndex < tangentDeltas.size()) {
                    tangentDeltas[vertexIndex] += target.tangentDeltas[i] * weight;
                }
            }
        }
    }

    morphDeltaPayload.reserve(morphDeltaPayload.size() + static_cast<size_t>(plan.vertexCount));
    for (uint32_t vertexIndex = 0; vertexIndex < plan.vertexCount; ++vertexIndex) {
        morphDeltaPayload.push_back(makeGpuSkinningMorphDelta(
            positionDeltas[vertexIndex],
            normalDeltas[vertexIndex],
            tangentDeltas[vertexIndex]));
    }
    const uint64_t morphDeltaBytes = static_cast<uint64_t>(plan.vertexCount) * static_cast<uint64_t>(sizeof(GpuLocalVertex));
    const uint64_t remaining = UINT64_MAX - stats.gpuSkinningMorphDeltaUploadBytes;
    stats.gpuSkinningMorphDeltaUploadBytes = morphDeltaBytes > remaining
        ? UINT64_MAX
        : stats.gpuSkinningMorphDeltaUploadBytes + morphDeltaBytes;
}

void addSaturated(uint32_t& value, uint32_t increment) {
    const uint64_t sum = static_cast<uint64_t>(value) + static_cast<uint64_t>(increment);
    value = sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
}

void addSaturated(uint64_t& value, uint64_t increment) {
    const uint64_t remaining = UINT64_MAX - value;
    value = increment > remaining ? UINT64_MAX : value + increment;
}

GpuSkinningInstancePlan recordGpuSkinningPlan(
    AnimatedGeometryStats& stats,
    const MeshAsset& mesh,
    const SceneSkinAsset& skin,
    const glm::mat4& meshWorld,
    const glm::mat4& previousMeshWorld,
    const std::vector<glm::mat4>& nodeWorldTransforms,
    const std::vector<glm::mat4>& previousNodeWorldTransforms,
    std::vector<glm::mat4>& jointMatrixPayload,
    std::vector<glm::mat4>& previousJointMatrixPayload,
    std::vector<PathTracerRenderer::GpuSkinningSourceVertex>& sourceVertexPayload,
    std::vector<GpuLocalVertex>& morphDeltaPayload,
    const std::vector<float>& morphWeights,
    uint32_t nodeIndex,
    MeshAssetHandle meshHandle,
    int32_t skinIndex,
    bool morphBeforeSkinning) {
    constexpr uint64_t kSkinnedVertexStrideBytes = sizeof(GpuLocalVertex);
    const uint32_t vertexCount = mesh.vertices.size() > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(mesh.vertices.size());
    GpuSkinningInstancePlan plan;
    plan.nodeIndex = nodeIndex;
    plan.meshHandleIndex = meshHandle.index;
    plan.skinIndex = skinIndex;
    const uint64_t jointPayloadOffset = static_cast<uint64_t>(jointMatrixPayload.size());
    plan.jointMatrixOffset = jointPayloadOffset > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(jointPayloadOffset);
    plan.jointMatrixCount = skin.joints.size() > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(skin.joints.size());
    plan.jointUploadByteOffset = jointPayloadOffset * static_cast<uint64_t>(sizeof(glm::mat4));
    plan.jointUploadBytes = static_cast<uint64_t>(plan.jointMatrixCount) * static_cast<uint64_t>(sizeof(glm::mat4));
    const uint64_t sourceVertexOffset = static_cast<uint64_t>(sourceVertexPayload.size());
    plan.sourceVertexOffset = sourceVertexOffset > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sourceVertexOffset);
    plan.currentVertexOffset = stats.gpuSkinningCurrentVertexCount;
    plan.previousVertexOffset = stats.gpuSkinningPreviousVertexCount;
    plan.vertexCount = vertexCount;
    plan.morphBeforeSkinning = morphBeforeSkinning;
    plan.cpuFallbackActive = true;

    ++stats.gpuSkinningCandidateInstanceCount;
    ++stats.gpuSkinningCpuFallbackInstanceCount;
    ++stats.gpuSkinningDispatchRecordCount;
    if (morphBeforeSkinning) {
        ++stats.gpuSkinningMorphPreSkinInstanceCount;
    }
    const glm::mat4 inverseMeshWorld = glm::inverse(meshWorld);
    const glm::mat4 inversePreviousMeshWorld = glm::inverse(previousMeshWorld);
    for (size_t jointIndex = 0; jointIndex < skin.joints.size(); ++jointIndex) {
        const uint32_t jointNodeIndex = skin.joints[jointIndex];
        const glm::mat4 jointWorld = jointNodeIndex < nodeWorldTransforms.size()
            ? nodeWorldTransforms[jointNodeIndex]
            : glm::mat4{1.0f};
        const glm::mat4 previousJointWorld = jointNodeIndex < previousNodeWorldTransforms.size()
            ? previousNodeWorldTransforms[jointNodeIndex]
            : jointWorld;
        const glm::mat4 inverseBind = jointIndex < skin.inverseBindMatrices.size()
            ? skin.inverseBindMatrices[jointIndex]
            : glm::mat4{1.0f};
        jointMatrixPayload.push_back(inverseMeshWorld * jointWorld * inverseBind);
        previousJointMatrixPayload.push_back(inversePreviousMeshWorld * previousJointWorld * inverseBind);
    }
    for (const MeshVertex& vertex : mesh.vertices) {
        sourceVertexPayload.push_back(makeGpuSkinningSourceVertex(vertex));
    }
    appendGpuSkinningMorphDeltas(mesh, morphWeights, morphDeltaPayload, plan, stats);
    addSaturated(stats.gpuSkinningJointMatrixCount, static_cast<uint32_t>(skin.joints.size()));
    addSaturated(stats.gpuSkinningJointUploadBytes, plan.jointUploadBytes);
    addSaturated(stats.gpuSkinningPreviousJointUploadBytes, plan.jointUploadBytes);
    addSaturated(stats.gpuSkinningSourceVertexUploadBytes, static_cast<uint64_t>(vertexCount) * static_cast<uint64_t>(sizeof(PathTracerRenderer::GpuSkinningSourceVertex)));
    addSaturated(stats.gpuSkinningCurrentVertexCount, vertexCount);
    addSaturated(stats.gpuSkinningPreviousVertexCount, vertexCount);
    addSaturated(stats.gpuSkinningCurrentVertexBufferBytes, static_cast<uint64_t>(vertexCount) * kSkinnedVertexStrideBytes);
    addSaturated(stats.gpuSkinningPreviousVertexBufferBytes, static_cast<uint64_t>(vertexCount) * kSkinnedVertexStrideBytes);
    return plan;
}

void recordAnimatedGeometryClassification(
    AnimatedGeometryStats& stats,
    const MeshAsset& mesh,
    SceneUpdateKind updateKind,
    bool needsMorphRuntimeMesh,
    bool needsSkinRuntimeMesh,
    bool needsRuntimeMesh,
    bool materialOverrideRuntimeMesh) {
    ++stats.meshInstanceCount;
    addSaturated(stats.totalPrimitiveCount, static_cast<uint32_t>(mesh.primitives.size()));
    addSaturated(stats.totalTriangleCount, meshTriangleCount(mesh));

    if (needsMorphRuntimeMesh) {
        ++stats.morphDeformingInstanceCount;
    }
    if (needsSkinRuntimeMesh) {
        ++stats.skinnedDeformingInstanceCount;
    }
    if (needsMorphRuntimeMesh && needsSkinRuntimeMesh) {
        ++stats.combinedMorphSkinnedInstanceCount;
    }
    if (needsMorphRuntimeMesh || needsSkinRuntimeMesh) {
        ++stats.deformingInstanceCount;
    } else if (updateKind == SceneUpdateKind::TransformOnly) {
        ++stats.transformOnlyCandidateInstanceCount;
    } else {
        ++stats.staticMeshInstanceCount;
    }
    if (needsRuntimeMesh) {
        ++stats.runtimeMeshInstanceCount;
    }
    if (materialOverrideRuntimeMesh) {
        ++stats.materialOverrideRuntimeMeshInstanceCount;
    }
}

[[nodiscard]] std::vector<glm::mat4> sceneNodeWorldTransforms(const SceneAsset& scene, bool previous = false) {
    std::vector<glm::mat4> world(scene.nodes.size(), glm::mat4{1.0f});
    std::vector<uint8_t> computed(scene.nodes.size(), 0u);
    auto compute = [&](auto&& self, size_t nodeIndex) -> glm::mat4 {
        if (nodeIndex >= scene.nodes.size()) {
            return glm::mat4{1.0f};
        }
        if (computed[nodeIndex] != 0u) {
            return world[nodeIndex];
        }
        const SceneNodeAsset& node = scene.nodes[nodeIndex];
        glm::mat4 result = previous && node.previousTransformValid ? node.previousTransform : node.transform;
        if (node.parent >= 0 && static_cast<size_t>(node.parent) < scene.nodes.size()) {
            result = self(self, static_cast<size_t>(node.parent)) * result;
        }
        world[nodeIndex] = result;
        computed[nodeIndex] = 1u;
        return result;
    };
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        compute(compute, i);
    }
    return world;
}

void applySkinningPayload(
    MeshAsset& mesh,
    const SceneSkinAsset& skin,
    const glm::mat4& meshWorld,
    const std::vector<glm::mat4>& nodeWorldTransforms) {
    if (!hasActiveSkinningPayload(mesh) || skin.joints.empty()) {
        return;
    }

    const glm::mat4 inverseMeshWorld = glm::inverse(meshWorld);
    std::vector<glm::mat4> jointMatrices(skin.joints.size(), glm::mat4{1.0f});
    for (size_t jointIndex = 0; jointIndex < skin.joints.size(); ++jointIndex) {
        const uint32_t nodeIndex = skin.joints[jointIndex];
        const glm::mat4 jointWorld = nodeIndex < nodeWorldTransforms.size()
            ? nodeWorldTransforms[nodeIndex]
            : glm::mat4{1.0f};
        const glm::mat4 inverseBind = jointIndex < skin.inverseBindMatrices.size()
            ? skin.inverseBindMatrices[jointIndex]
            : glm::mat4{1.0f};
        jointMatrices[jointIndex] = inverseMeshWorld * jointWorld * inverseBind;
    }

    const std::vector<MeshVertex> sourceVertices = mesh.vertices;
    for (size_t vertexIndex = 0; vertexIndex < sourceVertices.size(); ++vertexIndex) {
        const MeshVertex& source = sourceVertices[vertexIndex];
        glm::vec3 position{0.0f};
        glm::vec3 normal{0.0f};
        glm::vec3 tangent{0.0f};
        float totalWeight = 0.0f;

        for (int influence = 0; influence < 4; ++influence) {
            const float weight = source.weights[influence];
            const uint32_t jointIndex = source.joints[influence];
            if (weight <= 0.0f || jointIndex >= jointMatrices.size()) {
                continue;
            }
            const glm::mat4& skinMatrix = jointMatrices[jointIndex];
            position += glm::vec3(skinMatrix * glm::vec4(source.position, 1.0f)) * weight;
            normal += glm::mat3(skinMatrix) * source.normal * weight;
            tangent += glm::mat3(skinMatrix) * glm::vec3(source.tangent) * weight;
            totalWeight += weight;
        }

        if (totalWeight <= 1.0e-6f) {
            continue;
        }
        MeshVertex& target = mesh.vertices[vertexIndex];
        target.position = position;
        const float normalLen2 = glm::dot(normal, normal);
        target.normal = normalLen2 > 1.0e-10f ? normal / std::sqrt(normalLen2) : source.normal;
        tangent -= target.normal * glm::dot(target.normal, tangent);
        const float tangentLen2 = glm::dot(tangent, tangent);
        target.tangent = glm::vec4{
            tangentLen2 > 1.0e-10f ? tangent / std::sqrt(tangentLen2) : glm::vec3(source.tangent),
            source.tangent.w < 0.0f ? -1.0f : 1.0f,
        };
    }
}

void applyRendererMaterialBindings(
    SceneGpuBuildResult& result,
    const std::vector<const Entity*>& entities,
    AssetManager* assets) {
    if (assets == nullptr) {
        return;
    }

    const size_t count = std::min(result.sceneAsset.nodes.size(), entities.size());
    std::vector<uint8_t> classifiedNodes(result.sceneAsset.nodes.size(), 0u);
    std::vector<glm::mat4> nodeWorldTransforms;
    std::vector<glm::mat4> previousNodeWorldTransforms;
    bool worldTransformsComputed = false;
    auto ensureWorldTransforms = [&]() {
        if (worldTransformsComputed) {
            return;
        }
        nodeWorldTransforms = sceneNodeWorldTransforms(result.sceneAsset);
        previousNodeWorldTransforms = sceneNodeWorldTransforms(result.sceneAsset, true);
        worldTransformsComputed = true;
    };
    for (size_t nodeIndex = 0; nodeIndex < count; ++nodeIndex) {
        const Entity* entity = entities[nodeIndex];
        if (entity == nullptr || !entity->meshRenderer.has_value()) {
            continue;
        }

        const MeshRenderer& renderer = *entity->meshRenderer;
        const MeshAsset* sourceMesh = assets->mesh(renderer.mesh);
        if (sourceMesh == nullptr || sourceMesh->primitives.empty()) {
            ++result.animatedGeometry.missingMeshInstanceCount;
            classifiedNodes[nodeIndex] = 1u;
            continue;
        }
        classifiedNodes[nodeIndex] = 1u;

        bool needsRuntimeMesh = false;
        const std::vector<float> morphWeights = effectiveMorphWeights(renderer, *sourceMesh);
        const bool needsMorphRuntimeMesh = hasActiveMorphTargetWeights(*sourceMesh, morphWeights);
        const int32_t skinIndex = result.sceneAsset.nodes[nodeIndex].skinIndex;
        const bool needsSkinRuntimeMesh = skinIndex >= 0 &&
            static_cast<size_t>(skinIndex) < result.sceneAsset.skins.size() &&
            hasActiveSkinningPayload(*sourceMesh);
        if (needsSkinRuntimeMesh) {
            ensureWorldTransforms();
            result.gpuSkinningPlan.push_back(recordGpuSkinningPlan(
                result.animatedGeometry,
                *sourceMesh,
                result.sceneAsset.skins[static_cast<size_t>(skinIndex)],
                nodeWorldTransforms[nodeIndex],
                previousNodeWorldTransforms[nodeIndex],
                nodeWorldTransforms,
                previousNodeWorldTransforms,
                result.gpuSkinningJointMatrices,
                result.gpuSkinningPreviousJointMatrices,
                result.gpuSkinningSourceVertices,
                result.gpuSkinningMorphDeltas,
                morphWeights,
                static_cast<uint32_t>(nodeIndex),
                renderer.mesh,
                skinIndex,
                needsMorphRuntimeMesh));
        }
        needsRuntimeMesh = needsMorphRuntimeMesh;
        needsRuntimeMesh = needsRuntimeMesh || needsSkinRuntimeMesh;
        bool materialOverrideRuntimeMesh = false;
        std::vector<MaterialAssetHandle> effectiveMaterials;
        effectiveMaterials.reserve(sourceMesh->primitives.size());
        for (size_t primitiveIndex = 0; primitiveIndex < sourceMesh->primitives.size(); ++primitiveIndex) {
            const MeshPrimitiveAsset& primitive = sourceMesh->primitives[primitiveIndex];
            const MaterialAssetHandle material = effectivePrimitiveMaterial(renderer, primitive, primitiveIndex);
            effectiveMaterials.push_back(material);
            if (material.valid() && material.index != primitive.material.index) {
                needsRuntimeMesh = true;
                materialOverrideRuntimeMesh = true;
            }
            appendSceneMaterial(result.sceneAsset, assets, material);
        }
        recordAnimatedGeometryClassification(
            result.animatedGeometry,
            *sourceMesh,
            result.updateKind,
            needsMorphRuntimeMesh,
            needsSkinRuntimeMesh,
            needsRuntimeMesh,
            materialOverrideRuntimeMesh);

        if (!needsRuntimeMesh) {
            continue;
        }

        MeshAsset runtimeMesh = *sourceMesh;
        runtimeMesh.name = sourceMesh->name.empty()
            ? "runtime mesh"
            : sourceMesh->name + " (runtime mesh)";
        for (size_t primitiveIndex = 0; primitiveIndex < runtimeMesh.primitives.size(); ++primitiveIndex) {
            MeshPrimitiveAsset& primitive = runtimeMesh.primitives[primitiveIndex];
            const MaterialAssetHandle material = effectiveMaterials[primitiveIndex];
            if (material.valid()) {
                primitive.material = material;
                updatePrimitiveAlphaClassification(primitive, assets->material(material));
            }
        }
        if (needsMorphRuntimeMesh) {
            applyMorphTargetWeights(runtimeMesh, morphWeights);
        }
        if (needsSkinRuntimeMesh) {
            ensureWorldTransforms();
            applySkinningPayload(
                runtimeMesh,
                result.sceneAsset.skins[static_cast<size_t>(skinIndex)],
                nodeWorldTransforms[nodeIndex],
                nodeWorldTransforms);
        }

        const MeshAssetHandle runtimeHandle = assets->addMesh(std::move(runtimeMesh));
        result.sceneAsset.meshes.push_back(runtimeHandle);
        result.sceneAsset.nodes[nodeIndex].mesh = runtimeHandle;
        result.sceneAsset.nodes[nodeIndex].skinIndex = -1;
    }

    for (size_t nodeIndex = 0; nodeIndex < result.sceneAsset.nodes.size(); ++nodeIndex) {
        if (classifiedNodes[nodeIndex] != 0u) {
            continue;
        }
        const SceneNodeAsset& node = result.sceneAsset.nodes[nodeIndex];
        if (!node.mesh.valid()) {
            continue;
        }
        const MeshAsset* sourceMesh = assets->mesh(node.mesh);
        if (sourceMesh == nullptr || sourceMesh->primitives.empty()) {
            ++result.animatedGeometry.missingMeshInstanceCount;
            continue;
        }
        const std::vector<float> morphWeights = !node.morphWeights.empty() ? node.morphWeights : sourceMesh->defaultMorphWeights;
        const bool needsMorphRuntimeMesh = hasActiveMorphTargetWeights(*sourceMesh, morphWeights);
        const bool needsSkinRuntimeMesh = node.skinIndex >= 0 &&
            static_cast<size_t>(node.skinIndex) < result.sceneAsset.skins.size() &&
            hasActiveSkinningPayload(*sourceMesh);
        if (needsSkinRuntimeMesh) {
            ensureWorldTransforms();
            result.gpuSkinningPlan.push_back(recordGpuSkinningPlan(
                result.animatedGeometry,
                *sourceMesh,
                result.sceneAsset.skins[static_cast<size_t>(node.skinIndex)],
                nodeWorldTransforms[nodeIndex],
                previousNodeWorldTransforms[nodeIndex],
                nodeWorldTransforms,
                previousNodeWorldTransforms,
                result.gpuSkinningJointMatrices,
                result.gpuSkinningPreviousJointMatrices,
                result.gpuSkinningSourceVertices,
                result.gpuSkinningMorphDeltas,
                morphWeights,
                static_cast<uint32_t>(nodeIndex),
                node.mesh,
                node.skinIndex,
                needsMorphRuntimeMesh));
        }
        recordAnimatedGeometryClassification(
            result.animatedGeometry,
            *sourceMesh,
            result.updateKind,
            needsMorphRuntimeMesh,
            needsSkinRuntimeMesh,
            needsMorphRuntimeMesh || needsSkinRuntimeMesh,
            false);
    }

    if (result.animatedGeometry.meshInstanceCount == 0 && !result.sceneAsset.meshes.empty()) {
        for (MeshAssetHandle meshHandle : result.sceneAsset.meshes) {
            const MeshAsset* sourceMesh = assets->mesh(meshHandle);
            if (sourceMesh == nullptr || sourceMesh->primitives.empty()) {
                ++result.animatedGeometry.missingMeshInstanceCount;
                continue;
            }
            const bool needsMorphRuntimeMesh = hasActiveMorphTargetWeights(*sourceMesh, sourceMesh->defaultMorphWeights);
            recordAnimatedGeometryClassification(
                result.animatedGeometry,
                *sourceMesh,
                result.updateKind,
                needsMorphRuntimeMesh,
                false,
                needsMorphRuntimeMesh,
                false);
        }
    }
}

void appendSceneTexture(SceneAsset& scene, const AssetManager* assets, TextureAssetHandle texture) {
    if (texture.valid() && (assets == nullptr || assets->texture(texture) != nullptr)) {
        scene.textures.push_back(texture);
    }
}

void appendMaterialTextures(SceneAsset& scene, const AssetManager* assets, const MaterialAsset& material) {
    appendSceneTexture(scene, assets, material.baseColorTexture);
    appendSceneTexture(scene, assets, material.normalTexture);
    appendSceneTexture(scene, assets, material.metallicRoughnessTexture);
    appendSceneTexture(scene, assets, material.emissiveTexture);
    appendSceneTexture(scene, assets, material.clearcoatTexture);
    appendSceneTexture(scene, assets, material.clearcoatRoughnessTexture);
    appendSceneTexture(scene, assets, material.clearcoatNormalTexture);
    appendSceneTexture(scene, assets, material.transmissionTexture);
    appendSceneTexture(scene, assets, material.volumeThicknessTexture);
    appendSceneTexture(scene, assets, material.specularTexture);
    appendSceneTexture(scene, assets, material.specularColorTexture);
    appendSceneTexture(scene, assets, material.sheenColorTexture);
    appendSceneTexture(scene, assets, material.sheenRoughnessTexture);
    appendSceneTexture(scene, assets, material.iridescenceTexture);
    appendSceneTexture(scene, assets, material.iridescenceThicknessTexture);
    appendSceneTexture(scene, assets, material.anisotropyTexture);
    appendSceneTexture(scene, assets, material.occlusionTexture);
    appendSceneTexture(scene, assets, material.opacityTexture);
    appendSceneTexture(scene, assets, material.heightTexture);
}

void appendSceneMaterial(SceneAsset& scene, const AssetManager* assets, MaterialAssetHandle material) {
    if (!material.valid() || (assets != nullptr && assets->material(material) == nullptr)) {
        return;
    }
    scene.materials.push_back(material);
}

void deduplicateSceneHandles(SceneAsset& scene) {
    std::sort(scene.textures.begin(), scene.textures.end(), [](TextureAssetHandle a, TextureAssetHandle b) { return a.index < b.index; });
    scene.textures.erase(
        std::unique(scene.textures.begin(), scene.textures.end(), [](TextureAssetHandle a, TextureAssetHandle b) { return a.index == b.index; }),
        scene.textures.end());

    std::sort(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index < b.index; });
    scene.materials.erase(
        std::unique(scene.materials.begin(), scene.materials.end(), [](MaterialAssetHandle a, MaterialAssetHandle b) { return a.index == b.index; }),
        scene.materials.end());

    std::sort(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index < b.index; });
    scene.meshes.erase(
        std::unique(scene.meshes.begin(), scene.meshes.end(), [](MeshAssetHandle a, MeshAssetHandle b) { return a.index == b.index; }),
        scene.meshes.end());
}

void collectSceneMaterialTextures(SceneAsset& scene, const AssetManager* assets) {
    if (assets == nullptr) {
        deduplicateSceneHandles(scene);
        return;
    }
    for (MaterialAssetHandle material : scene.materials) {
        if (const MaterialAsset* asset = assets->material(material)) {
            appendMaterialTextures(scene, assets, *asset);
        }
    }
    deduplicateSceneHandles(scene);
}

} // namespace

SceneGpuBuildResult SceneToGpuSceneBuilder::build(
    const SceneDocument& document,
    AssetManager* assets,
    const RendererSettings& currentSettings) const {
    SceneGpuBuildResult result;
    result.updateKind = document.pendingUpdate();
    result.sceneAsset = document.toSceneAsset();
    result.rendererSettings = currentSettings;

    const std::vector<const Entity*> entities = document.registry().entities();
    applyRendererMaterialBindings(result, entities, assets);
    collectSceneMaterialTextures(result.sceneAsset, assets);

    const RenderSettings& render = document.renderSettings();
    const Environment& environment = document.environment();
    result.rendererSettings.renderPreset = render.renderPreset;
    result.rendererSettings.pathTracingEnabled = render.pathTracingEnabled;
    result.rendererSettings.cameraJitterEnabled = render.cameraJitterEnabled;
    result.rendererSettings.directLightingEnabled = render.directLightingEnabled;
    result.rendererSettings.maxBounces = render.maxBounces;
    result.rendererSettings.environmentDirectSamples = render.environmentDirectSamples;
    result.rendererSettings.toneMapper = render.toneMapper;
    result.rendererSettings.exposure = render.exposure;
    result.rendererSettings.gamma = render.gamma;
    result.rendererSettings.contrast = render.contrast;
    result.rendererSettings.saturation = render.saturation;
    result.rendererSettings.brightness = render.brightness;
    result.rendererSettings.whitePoint = render.whitePoint;
    result.rendererSettings.autoExposureEnabled = render.autoExposureEnabled;
    result.rendererSettings.targetLuminance = render.targetLuminance;
    result.rendererSettings.minExposure = render.minExposure;
    result.rendererSettings.maxExposure = render.maxExposure;
    result.rendererSettings.adaptationSpeed = render.adaptationSpeed;
    result.rendererSettings.histogramMinLogLuminance = render.histogramMinLogLuminance;
    result.rendererSettings.histogramMaxLogLuminance = render.histogramMaxLogLuminance;
    result.rendererSettings.histogramLowPercentile = render.histogramLowPercentile;
    result.rendererSettings.histogramHighPercentile = render.histogramHighPercentile;
    result.rendererSettings.histogramTargetPercentile = render.histogramTargetPercentile;
    result.rendererSettings.skyIntensity = render.skyIntensity;
    result.rendererSettings.rayleighScaleHeight = render.rayleighScaleHeight;
    result.rendererSettings.mieScaleHeight = render.mieScaleHeight;
    result.rendererSettings.mieAnisotropy = render.mieAnisotropy;
    result.rendererSettings.groundAlbedo = render.groundAlbedo;
    result.rendererSettings.indirectStrength = render.indirectStrength;
    result.rendererSettings.restirMode = render.restirMode;
    result.rendererSettings.restirGiEnabled = render.restirGiEnabled;
    result.rendererSettings.denoiserEnabled = render.denoiserEnabled;
    result.rendererSettings.denoiserBackend = render.denoiserBackend;
    result.rendererSettings.denoiseWhileMoving = render.denoiseWhileMoving;
    result.rendererSettings.samplesPerPixel = render.samplesPerPixel;
    result.rendererSettings.limitSamplesPerPixel = render.limitSamplesPerPixel;
    result.rendererSettings.atrousIterations = render.atrousIterations;
    result.rendererSettings.denoiserStrength = render.denoiserStrength;
    result.rendererSettings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
    result.rendererSettings.momentValidityThreshold = render.momentValidityThreshold;
    result.rendererSettings.taaEnabled = render.taaEnabled;
    result.rendererSettings.temporalUpscaler = render.temporalUpscaler;
    result.rendererSettings.dlssFrameGenerationEnabled = render.dlssFrameGenerationEnabled;
    result.rendererSettings.dlssRayReconstructionEnabled = render.dlssRayReconstructionEnabled;
    result.rendererSettings.streamlineReflexEnabled = render.streamlineReflexEnabled;
    result.rendererSettings.dlssSharpeningStrength = render.dlssSharpeningStrength;
    result.rendererSettings.taaFeedback = render.taaFeedback;
    result.rendererSettings.taaMotionFeedback = render.taaMotionFeedback;
    result.rendererSettings.taaReactiveFeedback = render.taaReactiveFeedback;
    result.rendererSettings.taaSharpeningStrength = render.taaSharpeningStrength;
    result.rendererSettings.debugView = render.debugView;
    result.rendererSettings.shadowRayBias = render.shadowRayBias;
    result.rendererSettings.shadowDistanceBias = render.shadowDistanceBias;
    result.rendererSettings.fireflyClamp = render.fireflyClamp;
    result.rendererSettings.restirGiTemporalMaxAge = render.restirGiTemporalMaxAge;
    result.rendererSettings.restirGiSpatialRounds = render.restirGiSpatialRounds;
    result.rendererSettings.restirGiSpatialRadius = render.restirGiSpatialRadius;
    result.rendererSettings.restirGiDepthThresholdScale = render.restirGiDepthThresholdScale;
    result.rendererSettings.restirGiSpatialCompatibilityThreshold = render.restirGiSpatialCompatibilityThreshold;
    result.rendererSettings.restirGiHalfResolution = render.restirGiHalfResolution;
    result.rendererSettings.restirGiVisibilityRayBudget = render.restirGiVisibilityRayBudget;
    result.rendererSettings.restirGiFinalStabilizationEnabled = render.restirGiFinalStabilizationEnabled;
    result.rendererSettings.restirGiActiveTileMaskMode = render.restirGiActiveTileMaskMode;
    result.rendererSettings.restirHistoryCopyMode = render.restirHistoryCopyMode;
    result.rendererSettings.adaptiveQualityMode = render.adaptiveQualityMode;
    result.rendererSettings.adaptiveGpuFrameTargetMs = render.adaptiveGpuFrameTargetMs;
    result.rendererSettings.materialTextureAnisotropy = render.materialTextureAnisotropy;
    result.rendererSettings.specularAaEnabled = render.specularAaEnabled;
    result.rendererSettings.opacityMicromapsEnabled = render.opacityMicromapsEnabled;
    result.rendererSettings.usePhysicalCamera = render.usePhysicalCamera;
    result.rendererSettings.physicalAperture = render.physicalAperture;
    result.rendererSettings.physicalShutterSeconds = render.physicalShutterSeconds;
    result.rendererSettings.physicalIso = render.physicalIso;
    result.rendererSettings.physicalExposureCompensation = render.physicalExposureCompensation;
    result.rendererSettings.dofApertureRadius = render.dofApertureRadius;
    result.rendererSettings.dofFocusDistance = render.dofFocusDistance;
    result.rendererSettings.dofBladeCount = render.dofBladeCount;
    result.rendererSettings.dofBokehRotation = render.dofBokehRotation;
    result.rendererSettings.motionBlurEnabled = render.motionBlurEnabled;
    result.rendererSettings.motionBlurShutterOpen = render.motionBlurShutterOpen;
    result.rendererSettings.motionBlurShutterClose = render.motionBlurShutterClose;
    result.rendererSettings.homogeneousVolumeEnabled = render.homogeneousVolumeEnabled;
    result.rendererSettings.homogeneousVolumeScattering = render.homogeneousVolumeScattering;
    result.rendererSettings.homogeneousVolumeAbsorption = render.homogeneousVolumeAbsorption;
    result.rendererSettings.homogeneousVolumeAnisotropy = render.homogeneousVolumeAnisotropy;
    result.rendererSettings.mneeCausticsEnabled = render.mneeCausticsEnabled;
    result.rendererSettings.environmentEnabled = environment.enabled;
    result.rendererSettings.environmentIntensity = environment.intensity;
    result.rendererSettings.environmentRotation = environment.rotation;
    result.rendererSettings.environmentBackgroundIntensity = environment.backgroundIntensity;
    result.rendererSettings.renderResolutionScale = render.resolutionScale;
    SunController::applyToRendererSettings(document, result.rendererSettings);
    applySceneWorldComponentsToRendererSettings(document, result.rendererSettings);

    auto appendInstanceEntity = [&](uint32_t nodeIndex) {
        if (nodeIndex < result.sceneAsset.nodes.size() &&
            nodeIndex < entities.size() &&
            result.sceneAsset.nodes[nodeIndex].mesh.valid()) {
            result.instanceEntities.push_back(entities[nodeIndex]->id);
        }
    };
    auto visitNode = [&](auto&& self, uint32_t nodeIndex) -> void {
        if (nodeIndex >= result.sceneAsset.nodes.size()) {
            return;
        }
        appendInstanceEntity(nodeIndex);
        for (uint32_t child : result.sceneAsset.nodes[nodeIndex].children) {
            self(self, child);
        }
    };
    if (!result.sceneAsset.rootNodes.empty()) {
        for (uint32_t root : result.sceneAsset.rootNodes) {
            visitNode(visitNode, root);
        }
    } else {
        for (uint32_t i = 0; i < result.sceneAsset.nodes.size(); ++i) {
            if (result.sceneAsset.nodes[i].parent < 0) {
                visitNode(visitNode, i);
            }
        }
    }

    result.accumulationReason = accumulationReasonFor(result.updateKind);
    result.requiresRendererRebuild = result.updateKind == SceneUpdateKind::TopologyChanged;
    return result;
}

AccumulationResetReason SceneToGpuSceneBuilder::accumulationReasonFor(SceneUpdateKind kind) {
    switch (kind) {
    case SceneUpdateKind::None: return AccumulationResetReason::Manual;
    case SceneUpdateKind::MaterialOnly: return AccumulationResetReason::MaterialChanged;
    case SceneUpdateKind::TransformOnly: return AccumulationResetReason::SceneChanged;
    case SceneUpdateKind::LightOnly: return AccumulationResetReason::LightingChanged;
    case SceneUpdateKind::EnvironmentOnly: return AccumulationResetReason::EnvironmentChanged;
    case SceneUpdateKind::CameraOnly: return AccumulationResetReason::CameraMoved;
    case SceneUpdateKind::VisibilityOnly: return AccumulationResetReason::Manual;
    case SceneUpdateKind::TopologyChanged: return AccumulationResetReason::SceneChanged;
    case SceneUpdateKind::RendererSettingsOnly: return AccumulationResetReason::RenderSettingsChanged;
    }
    return AccumulationResetReason::Manual;
}

} // namespace rtv
