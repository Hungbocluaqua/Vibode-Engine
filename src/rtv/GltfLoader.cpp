#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "rtv/GltfLoader.h"

#include "rtv/AssetManager.h"
#include "rtv/SceneCache.h"
#include "rtv/SceneComponents.h"
#include "rtv/TextureLoader.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rtv {

namespace {

struct AccessorView {
    const uint8_t* data = nullptr;
    size_t count = 0;
    size_t byteStride = 0;
    size_t elementSize = 0;
};

[[nodiscard]] bool checkedAdd(size_t a, size_t b, size_t& out) {
    if (a > std::numeric_limits<size_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}

[[nodiscard]] bool checkedMul(size_t a, size_t b, size_t& out) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

[[nodiscard]] const char* accessorTypeName(int type) {
    switch (type) {
    case TINYGLTF_TYPE_SCALAR: return "SCALAR";
    case TINYGLTF_TYPE_VEC2: return "VEC2";
    case TINYGLTF_TYPE_VEC3: return "VEC3";
    case TINYGLTF_TYPE_VEC4: return "VEC4";
    case TINYGLTF_TYPE_MAT2: return "MAT2";
    case TINYGLTF_TYPE_MAT3: return "MAT3";
    case TINYGLTF_TYPE_MAT4: return "MAT4";
    default: return "UNKNOWN";
    }
}

[[nodiscard]] const char* componentTypeName(int componentType) {
    switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE: return "BYTE";
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return "UNSIGNED_BYTE";
    case TINYGLTF_COMPONENT_TYPE_SHORT: return "SHORT";
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return "UNSIGNED_SHORT";
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return "UNSIGNED_INT";
    case TINYGLTF_COMPONENT_TYPE_FLOAT: return "FLOAT";
    default: return "UNKNOWN";
    }
}

[[nodiscard]] bool quantizedFloatComponentType(int componentType) {
    return componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
        componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
        componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
        componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT;
}

enum class FloatAttributeComponentPolicy {
    NormalizedInteger,
    PositionOrTexcoord,
    SignedPositionDelta,
    SignedNormalizedInteger,
};

[[nodiscard]] bool signedQuantizedFloatComponentType(int componentType) {
    return componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
        componentType == TINYGLTF_COMPONENT_TYPE_SHORT;
}

[[nodiscard]] bool quantizedFloatComponentAllowed(
    int componentType,
    bool normalized,
    FloatAttributeComponentPolicy policy) {
    if (!quantizedFloatComponentType(componentType)) {
        return false;
    }
    switch (policy) {
    case FloatAttributeComponentPolicy::NormalizedInteger:
        return normalized;
    case FloatAttributeComponentPolicy::PositionOrTexcoord:
        return true;
    case FloatAttributeComponentPolicy::SignedPositionDelta:
        return signedQuantizedFloatComponentType(componentType);
    case FloatAttributeComponentPolicy::SignedNormalizedInteger:
        return normalized && signedQuantizedFloatComponentType(componentType);
    }
    return false;
}

[[nodiscard]] const char* floatAttributePolicyDescription(FloatAttributeComponentPolicy policy) {
    switch (policy) {
    case FloatAttributeComponentPolicy::NormalizedInteger:
        return "normalized integer";
    case FloatAttributeComponentPolicy::PositionOrTexcoord:
        return "quantized position/texcoord integer";
    case FloatAttributeComponentPolicy::SignedPositionDelta:
        return "signed quantized morph POSITION delta integer";
    case FloatAttributeComponentPolicy::SignedNormalizedInteger:
        return "signed normalized integer";
    }
    return "quantized integer";
}

[[nodiscard]] std::optional<AccessorView> makeAccessorView(
    const tinygltf::Model& model,
    int accessorIndex,
    int expectedType,
    int expectedComponentType,
    std::string_view label) {
    if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
        std::cerr << "glTF accessor warning: " << label << " accessor index " << accessorIndex << " is out of range\n";
        return std::nullopt;
    }

    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    if (accessor.type != expectedType) {
        std::cerr << "glTF accessor warning: " << label << " expected type " << accessorTypeName(expectedType)
                  << " but got " << accessorTypeName(accessor.type) << "; skipping attribute\n";
        return std::nullopt;
    }
    if (accessor.componentType != expectedComponentType) {
        std::cerr << "glTF accessor warning: " << label << " expected component " << componentTypeName(expectedComponentType)
                  << " but got " << componentTypeName(accessor.componentType) << "; skipping attribute\n";
        return std::nullopt;
    }
    if (accessor.bufferView < 0 || static_cast<size_t>(accessor.bufferView) >= model.bufferViews.size()) {
        std::cerr << "glTF accessor warning: " << label << " references invalid bufferView " << accessor.bufferView << "\n";
        return std::nullopt;
    }

    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
    if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size()) {
        std::cerr << "glTF accessor warning: " << label << " references invalid buffer " << view.buffer << "\n";
        return std::nullopt;
    }

    const int componentSizeInt = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
    const int componentCountInt = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
    if (componentSizeInt <= 0 || componentCountInt <= 0) {
        std::cerr << "glTF accessor warning: " << label << " has unsupported type/component combination\n";
        return std::nullopt;
    }

    const size_t componentSize = static_cast<size_t>(componentSizeInt);
    const size_t componentCount = static_cast<size_t>(componentCountInt);
    size_t elementSize = 0;
    if (!checkedMul(componentSize, componentCount, elementSize)) {
        std::cerr << "glTF accessor warning: " << label << " element size overflow\n";
        return std::nullopt;
    }

    const int strideInt = accessor.ByteStride(view);
    if (strideInt < 0) {
        std::cerr << "glTF accessor warning: " << label << " has invalid byteStride\n";
        return std::nullopt;
    }
    const size_t byteStride = strideInt > 0 ? static_cast<size_t>(strideInt) : elementSize;
    if (byteStride < elementSize) {
        std::cerr << "glTF accessor warning: " << label << " byteStride is smaller than element size\n";
        return std::nullopt;
    }

    const size_t accessorByteOffset = static_cast<size_t>(accessor.byteOffset);
    const size_t viewByteOffset = static_cast<size_t>(view.byteOffset);
    const size_t viewByteLength = static_cast<size_t>(view.byteLength);
    const tinygltf::Buffer& buffer = model.buffers[static_cast<size_t>(view.buffer)];
    const size_t bufferSize = buffer.data.size();

    size_t viewEnd = 0;
    if (!checkedAdd(viewByteOffset, viewByteLength, viewEnd) || viewEnd > bufferSize) {
        std::cerr << "glTF accessor warning: " << label << " bufferView range exceeds buffer size\n";
        return std::nullopt;
    }
    if (accessorByteOffset > viewByteLength) {
        std::cerr << "glTF accessor warning: " << label << " byteOffset exceeds bufferView length\n";
        return std::nullopt;
    }

    if (accessor.count > 0) {
        size_t lastElementOffset = 0;
        size_t lastElementEnd = 0;
        if (!checkedMul(static_cast<size_t>(accessor.count - 1), byteStride, lastElementOffset) ||
            !checkedAdd(accessorByteOffset, lastElementOffset, lastElementOffset) ||
            !checkedAdd(lastElementOffset, elementSize, lastElementEnd) ||
            lastElementEnd > viewByteLength) {
            std::cerr << "glTF accessor warning: " << label << " computed byte range exceeds bufferView length\n";
            return std::nullopt;
        }
    }

    return AccessorView{
        .data = buffer.data.data() + viewByteOffset + accessorByteOffset,
        .count = static_cast<size_t>(accessor.count),
        .byteStride = byteStride,
        .elementSize = elementSize,
    };
}

[[nodiscard]] std::optional<AccessorView> makeIndexAccessorView(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
        std::cerr << "glTF accessor warning: index accessor " << accessorIndex << " is out of range\n";
        return std::nullopt;
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    if (accessor.type != TINYGLTF_TYPE_SCALAR) {
        std::cerr << "glTF accessor warning: indices expected SCALAR but got " << accessorTypeName(accessor.type) << "; skipping primitive\n";
        return std::nullopt;
    }
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        std::cerr << "glTF accessor warning: indices use unsupported component " << componentTypeName(accessor.componentType) << "; skipping primitive\n";
        return std::nullopt;
    }
    return makeAccessorView(model, accessorIndex, TINYGLTF_TYPE_SCALAR, accessor.componentType, "indices");
}

[[nodiscard]] std::optional<AccessorView> makeFloatAttributeAccessorView(
    const tinygltf::Model& model,
    int accessorIndex,
    int expectedType,
    std::string_view label,
    FloatAttributeComponentPolicy componentPolicy = FloatAttributeComponentPolicy::NormalizedInteger) {
    if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
        std::cerr << "glTF accessor warning: " << label << " accessor index " << accessorIndex << " is out of range\n";
        return std::nullopt;
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    if (accessor.type != expectedType) {
        std::cerr << "glTF accessor warning: " << label << " expected type " << accessorTypeName(expectedType)
                  << " but got " << accessorTypeName(accessor.type) << "; skipping attribute\n";
        return std::nullopt;
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        return makeAccessorView(model, accessorIndex, expectedType, TINYGLTF_COMPONENT_TYPE_FLOAT, label);
    }
    if (quantizedFloatComponentAllowed(accessor.componentType, accessor.normalized, componentPolicy)) {
        return makeAccessorView(model, accessorIndex, expectedType, accessor.componentType, label);
    }
    std::cerr << "glTF accessor warning: " << label << " uses unsupported component "
              << componentTypeName(accessor.componentType)
              << (quantizedFloatComponentType(accessor.componentType) ? " for expected " : "")
              << (quantizedFloatComponentType(accessor.componentType) ? floatAttributePolicyDescription(componentPolicy) : "")
              << "; skipping attribute\n";
    return std::nullopt;
}

[[nodiscard]] std::optional<AccessorView> makeColorAccessorView(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
        std::cerr << "glTF accessor warning: COLOR_0 accessor index " << accessorIndex << " is out of range\n";
        return std::nullopt;
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    if (accessor.type != TINYGLTF_TYPE_VEC3 && accessor.type != TINYGLTF_TYPE_VEC4) {
        std::cerr << "glTF accessor warning: COLOR_0 expected VEC3 or VEC4 but got " << accessorTypeName(accessor.type) << "; skipping attribute\n";
        return std::nullopt;
    }
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        std::cerr << "glTF accessor warning: COLOR_0 uses unsupported component " << componentTypeName(accessor.componentType) << "; skipping attribute\n";
        return std::nullopt;
    }
    return makeAccessorView(model, accessorIndex, accessor.type, accessor.componentType, "COLOR_0");
}

[[nodiscard]] std::optional<AccessorView> makeJointAccessorView(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
        std::cerr << "glTF accessor warning: JOINTS_0 accessor index " << accessorIndex << " is out of range\n";
        return std::nullopt;
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    if (accessor.type != TINYGLTF_TYPE_VEC4) {
        std::cerr << "glTF accessor warning: JOINTS_0 expected VEC4 but got " << accessorTypeName(accessor.type) << "; skipping attribute\n";
        return std::nullopt;
    }
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        std::cerr << "glTF accessor warning: JOINTS_0 uses unsupported component " << componentTypeName(accessor.componentType) << "; skipping attribute\n";
        return std::nullopt;
    }
    return makeAccessorView(model, accessorIndex, TINYGLTF_TYPE_VEC4, accessor.componentType, "JOINTS_0");
}

[[nodiscard]] std::optional<AccessorView> makeWeightAccessorView(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size()) {
        std::cerr << "glTF accessor warning: WEIGHTS_0 accessor index " << accessorIndex << " is out of range\n";
        return std::nullopt;
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    if (accessor.type != TINYGLTF_TYPE_VEC4) {
        std::cerr << "glTF accessor warning: WEIGHTS_0 expected VEC4 but got " << accessorTypeName(accessor.type) << "; skipping attribute\n";
        return std::nullopt;
    }
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        std::cerr << "glTF accessor warning: WEIGHTS_0 uses unsupported component " << componentTypeName(accessor.componentType) << "; skipping attribute\n";
        return std::nullopt;
    }
    return makeAccessorView(model, accessorIndex, TINYGLTF_TYPE_VEC4, accessor.componentType, "WEIGHTS_0");
}

template <typename T>
[[nodiscard]] T readElement(const AccessorView& view, size_t index) {
    T value{};
    std::memcpy(&value, view.data + index * view.byteStride, sizeof(T));
    return value;
}

[[nodiscard]] uint32_t readIndexElement(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        uint16_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
        uint32_t value = 0;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }
    return *src;
}

[[nodiscard]] float readFloatAttributeComponent(const tinygltf::Accessor& accessor, const uint8_t* src, int component) {
    const size_t c = static_cast<size_t>(component);
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
        float value = 0.0f;
        std::memcpy(&value, src + sizeof(float) * c, sizeof(value));
        return value;
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE) {
        int8_t value = 0;
        std::memcpy(&value, src + sizeof(int8_t) * c, sizeof(value));
        return accessor.normalized ? std::max(-1.0f, static_cast<float>(value) / 127.0f) : static_cast<float>(value);
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
        uint8_t value = 0;
        std::memcpy(&value, src + sizeof(uint8_t) * c, sizeof(value));
        return accessor.normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT) {
        int16_t value = 0;
        std::memcpy(&value, src + sizeof(int16_t) * c, sizeof(value));
        return accessor.normalized ? std::max(-1.0f, static_cast<float>(value) / 32767.0f) : static_cast<float>(value);
    }
    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
        uint16_t value = 0;
        std::memcpy(&value, src + sizeof(uint16_t) * c, sizeof(value));
        return accessor.normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
    }
    return 0.0f;
}

[[nodiscard]] glm::vec2 readFloatAttributeVec2(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    return {
        readFloatAttributeComponent(accessor, src, 0),
        readFloatAttributeComponent(accessor, src, 1),
    };
}

[[nodiscard]] glm::vec3 readFloatAttributeVec3(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    return {
        readFloatAttributeComponent(accessor, src, 0),
        readFloatAttributeComponent(accessor, src, 1),
        readFloatAttributeComponent(accessor, src, 2),
    };
}

[[nodiscard]] glm::vec4 readFloatAttributeVec4(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    return {
        readFloatAttributeComponent(accessor, src, 0),
        readFloatAttributeComponent(accessor, src, 1),
        readFloatAttributeComponent(accessor, src, 2),
        readFloatAttributeComponent(accessor, src, 3),
    };
}

[[nodiscard]] glm::vec4 readColorElement(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    const int componentCount = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
    glm::vec4 color{1.0f};
    const int count = std::min(componentCount, 4);
    for (int c = 0; c < count; ++c) {
        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
            float value = 1.0f;
            std::memcpy(&value, src + sizeof(float) * static_cast<size_t>(c), sizeof(value));
            color[c] = value;
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            uint16_t value = 0;
            std::memcpy(&value, src + sizeof(uint16_t) * static_cast<size_t>(c), sizeof(value));
            color[c] = static_cast<float>(value) / 65535.0f;
        } else {
            color[c] = static_cast<float>(src[c]) / 255.0f;
        }
    }
    return glm::clamp(color, glm::vec4{0.0f}, glm::vec4{1.0f});
}

[[nodiscard]] glm::uvec4 readJointElement(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    glm::uvec4 joints{0u};
    for (int c = 0; c < 4; ++c) {
        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            uint16_t value = 0;
            std::memcpy(&value, src + sizeof(uint16_t) * static_cast<size_t>(c), sizeof(value));
            joints[c] = value;
        } else {
            joints[c] = src[c];
        }
    }
    return joints;
}

[[nodiscard]] glm::vec4 readWeightElement(const tinygltf::Accessor& accessor, const AccessorView& view, size_t index) {
    const uint8_t* src = view.data + index * view.byteStride;
    glm::vec4 weights{0.0f};
    for (int c = 0; c < 4; ++c) {
        if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
            float value = 0.0f;
            std::memcpy(&value, src + sizeof(float) * static_cast<size_t>(c), sizeof(value));
            weights[c] = value;
        } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            uint16_t value = 0;
            std::memcpy(&value, src + sizeof(uint16_t) * static_cast<size_t>(c), sizeof(value));
            weights[c] = static_cast<float>(value) / 65535.0f;
        } else {
            weights[c] = static_cast<float>(src[c]) / 255.0f;
        }
    }
    weights = glm::clamp(weights, glm::vec4{0.0f}, glm::vec4{1.0f});
    const float sum = weights.x + weights.y + weights.z + weights.w;
    if (sum > 1.0e-6f) {
        weights /= sum;
    }
    return weights;
}

[[nodiscard]] std::vector<std::string> morphTargetNamesForMesh(const tinygltf::Mesh& mesh) {
    std::vector<std::string> names;
    if (!mesh.extras.IsObject()) {
        return names;
    }
    const auto& extras = mesh.extras.Get<tinygltf::Value::Object>();
    const auto targetNamesIt = extras.find("targetNames");
    if (targetNamesIt == extras.end() || !targetNamesIt->second.IsArray()) {
        return names;
    }
    const auto& targetNames = targetNamesIt->second.Get<tinygltf::Value::Array>();
    names.reserve(targetNames.size());
    for (const tinygltf::Value& value : targetNames) {
        names.push_back(value.IsString() ? value.Get<std::string>() : std::string{});
    }
    return names;
}

[[nodiscard]] std::vector<float> morphWeightsFromGltf(const std::vector<double>& sourceWeights) {
    std::vector<float> weights;
    weights.reserve(sourceWeights.size());
    for (double weight : sourceWeights) {
        weights.push_back(static_cast<float>(weight));
    }
    return weights;
}

[[nodiscard]] std::vector<glm::mat4> inverseBindMatricesFromGltf(
    const tinygltf::Model& model,
    int accessorIndex,
    size_t jointCount) {
    std::vector<glm::mat4> matrices(jointCount, glm::mat4{1.0f});
    if (accessorIndex < 0 || jointCount == 0) {
        return matrices;
    }

    const auto view = makeAccessorView(
        model,
        accessorIndex,
        TINYGLTF_TYPE_MAT4,
        TINYGLTF_COMPONENT_TYPE_FLOAT,
        "inverseBindMatrices");
    if (!view.has_value()) {
        return matrices;
    }
    if (view->count < jointCount) {
        std::cerr << "glTF accessor warning: inverseBindMatrices count is shorter than skin joint count; missing entries use identity\n";
    }
    const size_t count = std::min(jointCount, view->count);
    for (size_t i = 0; i < count; ++i) {
        matrices[i] = readElement<glm::mat4>(*view, i);
    }
    return matrices;
}

[[nodiscard]] std::vector<glm::vec3> readMorphTargetDeltas(
    const tinygltf::Model& model,
    const std::map<std::string, int>& attributes,
    const char* attributeName,
    size_t vertexCount) {
    const auto attrIt = attributes.find(attributeName);
    if (attrIt == attributes.end()) {
        return {};
    }

    const std::string label = std::string("morph target ") + attributeName;
    const FloatAttributeComponentPolicy componentPolicy = std::string_view{attributeName} == "POSITION"
        ? FloatAttributeComponentPolicy::SignedPositionDelta
        : FloatAttributeComponentPolicy::SignedNormalizedInteger;
    const auto view = makeFloatAttributeAccessorView(model, attrIt->second, TINYGLTF_TYPE_VEC3, label, componentPolicy);
    if (!view.has_value()) {
        return {};
    }
    if (view->count < vertexCount) {
        std::cerr << "glTF accessor warning: " << label << " count is shorter than POSITION count; skipping morph attribute\n";
        return {};
    }

    std::vector<glm::vec3> deltas(vertexCount);
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(attrIt->second)];
    for (size_t i = 0; i < vertexCount; ++i) {
        deltas[i] = readFloatAttributeVec3(accessor, *view, i);
    }
    return deltas;
}

[[nodiscard]] glm::mat4 nodeTransform(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        glm::mat4 result{1.0f};
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                result[col][row] = static_cast<float>(node.matrix[static_cast<size_t>(col * 4 + row)]);
            }
        }
        return result;
    }

    glm::vec3 translation{0.0f};
    glm::vec3 scale{1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    if (node.translation.size() == 3) {
        translation = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2])};
    }
    if (node.scale.size() == 3) {
        scale = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2])};
    }
    if (node.rotation.size() == 4) {
        rotation = {static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])};
    }
    return glm::translate(glm::mat4{1.0f}, translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0f}, scale);
}

[[nodiscard]] bool isDataUri(const std::string& uri) {
    return uri.rfind("data:", 0) == 0;
}

[[nodiscard]] std::filesystem::path textureSourcePath(const tinygltf::Image& image, const std::filesystem::path& gltfPath) {
    if (!image.uri.empty() && !isDataUri(image.uri)) {
        return (gltfPath.parent_path() / image.uri).lexically_normal();
    }
    return gltfPath;
}

void addExternalUriDependency(
    std::vector<std::filesystem::path>& dependencies,
    std::unordered_set<std::string>& seen,
    const std::filesystem::path& gltfPath,
    const std::string& uri) {
    if (uri.empty() || isDataUri(uri)) {
        return;
    }
    const std::filesystem::path depPath = (gltfPath.parent_path() / uri).lexically_normal();
    const std::string key = depPath.string();
    if (seen.insert(key).second) {
        dependencies.push_back(depPath);
    }
}

[[nodiscard]] std::vector<std::filesystem::path> gltfExternalDependencies(const tinygltf::Model& model, const std::filesystem::path& gltfPath) {
    std::vector<std::filesystem::path> dependencies;
    dependencies.reserve(model.buffers.size() + model.images.size());
    std::unordered_set<std::string> seen;
    for (const tinygltf::Buffer& buffer : model.buffers) {
        addExternalUriDependency(dependencies, seen, gltfPath, buffer.uri);
    }
    for (const tinygltf::Image& image : model.images) {
        addExternalUriDependency(dependencies, seen, gltfPath, image.uri);
    }
    return dependencies;
}

void addDependency(CachedScene& cached, const std::filesystem::path& path, std::unordered_set<std::string>& seen) {
    if (path.empty()) {
        return;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    const std::string key = normalized.string();
    if (!seen.insert(key).second) {
        return;
    }
    if (!std::filesystem::exists(normalized) || !std::filesystem::is_regular_file(normalized)) {
        cached.dependencies.push_back(FileDependency{
            .path = key,
            .size = std::numeric_limits<uint64_t>::max(),
            .mtime = 0,
        });
        return;
    }
    cached.dependencies.push_back(FileDependency{
        .path = key,
        .size = static_cast<uint64_t>(std::filesystem::file_size(normalized)),
        .mtime = SceneCache::fileMtime(normalized),
    });
}

[[nodiscard]] bool dependenciesValid(const CachedScene& cached) {
    for (const FileDependency& dep : cached.dependencies) {
        const std::filesystem::path path = dep.path;
        if (dep.size == std::numeric_limits<uint64_t>::max()) {
            if (std::filesystem::exists(path)) {
                return false;
            }
            continue;
        }
        if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
            return false;
        }
        if (static_cast<uint64_t>(std::filesystem::file_size(path)) != dep.size) {
            return false;
        }
        if (SceneCache::fileMtime(path) != dep.mtime) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isKtx2Data(const std::vector<uint8_t>& data) {
    constexpr uint8_t ktx2Magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    return data.size() >= sizeof(ktx2Magic) && std::memcmp(data.data(), ktx2Magic, sizeof(ktx2Magic)) == 0;
}

[[nodiscard]] bool isKtx2Bytes(const unsigned char* bytes, int size) {
    constexpr uint8_t ktx2Magic[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    return bytes != nullptr && size >= static_cast<int>(sizeof(ktx2Magic)) &&
        std::memcmp(bytes, ktx2Magic, sizeof(ktx2Magic)) == 0;
}

[[nodiscard]] uint32_t readKtx2U32(const unsigned char* bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

[[nodiscard]] bool isKtx2Path(const std::string& uri) {
    if (uri.empty()) return false;
    const std::string lower = [&] {
        std::string s = uri;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }();
    return lower.ends_with(".ktx2");
}

[[nodiscard]] bool isDdsBytes(const unsigned char* bytes, int size) {
    return bytes != nullptr && size >= 4 && std::memcmp(bytes, "DDS ", 4u) == 0;
}

[[nodiscard]] bool isDdsPath(const std::string& uri) {
    if (uri.empty()) return false;
    std::string lower = uri;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.ends_with(".dds");
}

[[nodiscard]] uint32_t readDdsU32(const unsigned char* bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

[[nodiscard]] std::pair<int, int> readDdsDimensions(const unsigned char* bytes, int size) {
    if (!isDdsBytes(bytes, size) || size < 128) {
        return {1, 1};
    }
    return {
        static_cast<int>(std::max(readDdsU32(bytes, 16), 1u)),
        static_cast<int>(std::max(readDdsU32(bytes, 12), 1u)),
    };
}

[[nodiscard]] bool isHdrBytes(const unsigned char* bytes, int size) {
    constexpr char radianceMagic[] = "#?RADIANCE";
    constexpr char rgbeMagic[] = "#?RGBE";
    return bytes != nullptr && size >= 7 &&
        (std::memcmp(bytes, radianceMagic, sizeof(radianceMagic) - 1) == 0 ||
         std::memcmp(bytes, rgbeMagic, sizeof(rgbeMagic) - 1) == 0);
}

[[nodiscard]] bool isHdrPath(const std::string& uri) {
    if (uri.empty()) return false;
    std::string lower = uri;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.ends_with(".hdr") || lower.ends_with(".rgbe");
}

[[nodiscard]] std::pair<int, int> readRadianceDimensions(const unsigned char* bytes, int size) {
    if (bytes == nullptr || size <= 0) {
        return {1, 1};
    }
    const std::string header(reinterpret_cast<const char*>(bytes), static_cast<size_t>(size));
    const size_t marker = header.find("-Y ");
    if (marker == std::string::npos) {
        return {1, 1};
    }
    std::istringstream dims(header.substr(marker));
    std::string yToken;
    std::string xToken;
    int height = 1;
    int width = 1;
    if (!(dims >> yToken >> height >> xToken >> width) || yToken != "-Y" || xToken != "+X") {
        return {1, 1};
    }
    return {std::max(width, 1), std::max(height, 1)};
}

bool loadImageDataPreservingKtx2(
    tinygltf::Image* image,
    const int imageIndex,
    std::string* error,
    std::string* warning,
    int reqWidth,
    int reqHeight,
    const unsigned char* bytes,
    int size,
    void* userData) {
    const bool ktx2 = image != nullptr &&
        (image->mimeType == "image/ktx2" || isKtx2Path(image->uri) || isKtx2Bytes(bytes, size));
    const bool dds = image != nullptr &&
        (image->mimeType == "image/vnd-ms.dds" || isDdsPath(image->uri) || isDdsBytes(bytes, size));
    const bool hdr = image != nullptr &&
        (image->mimeType == "image/vnd.radiance" || image->mimeType == "image/hdr" || isHdrPath(image->uri) || isHdrBytes(bytes, size));
    if (!ktx2 && !dds && !hdr) {
        return tinygltf::LoadImageData(image, imageIndex, error, warning, reqWidth, reqHeight, bytes, size, userData);
    }

    image->image.assign(bytes, bytes + size);
    if (ktx2) {
        image->width = isKtx2Bytes(bytes, size) ? static_cast<int>(readKtx2U32(bytes, 20)) : 1;
        image->height = isKtx2Bytes(bytes, size) ? static_cast<int>(readKtx2U32(bytes, 24)) : 1;
    } else if (dds) {
        const auto [width, height] = readDdsDimensions(bytes, size);
        image->width = width;
        image->height = height;
    } else {
        const auto [width, height] = readRadianceDimensions(bytes, size);
        image->width = width;
        image->height = height;
    }
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    return true;
}

[[nodiscard]] NativeTextureColorSpace gltfNativeTextureColorSpace(NativeTextureRole role) {
    switch (role) {
    case NativeTextureRole::BaseColor:
    case NativeTextureRole::Emissive:
        return NativeTextureColorSpace::Srgb;
    case NativeTextureRole::EnvironmentHdr:
        return NativeTextureColorSpace::HdrLinear;
    default:
        return NativeTextureColorSpace::Linear;
    }
}

[[nodiscard]] TextureAsset textureFromImage(
    const tinygltf::Image& image,
    const std::filesystem::path& gltfPath,
    const NativeTextureFormatSupport& nativeTextureFormatSupport,
    NativeTextureRole nativeTextureRole) {
    TextureAsset texture;
    texture.name = image.name;
    texture.sourcePath = textureSourcePath(image, gltfPath);
    texture.width = static_cast<uint32_t>(std::max(image.width, 1));
    texture.height = static_cast<uint32_t>(std::max(image.height, 1));
    texture.channels = 4;
    const NativeTextureColorSpace nativeTextureColorSpace = gltfNativeTextureColorSpace(nativeTextureRole);

    if (isKtx2Path(image.uri) || isKtx2Data(image.image)) {
        std::filesystem::path ktxPath;
        if (isKtx2Path(image.uri)) {
            ktxPath = texture.sourcePath;
        }
        try {
            TextureData td = ktxPath.empty() && isKtx2Data(image.image)
                ? TextureLoader::loadKtx2(image.image.data(), image.image.size(), nativeTextureFormatSupport, nativeTextureRole, nativeTextureColorSpace)
                : TextureLoader::loadKtx2(ktxPath.string(), nativeTextureFormatSupport, nativeTextureRole, nativeTextureColorSpace);
            texture.width = static_cast<uint32_t>(td.width);
            texture.height = static_cast<uint32_t>(td.height);
            texture.channels = 4;
            texture.mipLevels = td.mipLevels;
            texture.isCompressed = td.isCompressed;
            texture.linearColorSpace = td.linearColorSpace;
            texture.format = td.format;
            texture.compressedFormat = td.compressedFormat;
            texture.sourceContainerKind = std::move(td.sourceContainerKind);
            texture.nativePayloadSource = std::move(td.nativePayloadSource);
            texture.sourceContainerPreserved = td.sourceContainerPreserved;
            texture.sourceContainerTranscoded = td.sourceContainerTranscoded;
            texture.rgba8 = std::move(td.pixels);
            texture.mipData = std::move(td.mipData);
            texture.fallback = false;
            return texture;
        } catch (const std::runtime_error& e) {
            std::cerr << "KTX2 texture load failed: " << e.what() << ", falling back\n";
        }
    }

    const bool externalImage = !texture.sourcePath.empty() && texture.sourcePath != gltfPath && std::filesystem::exists(texture.sourcePath);
    if (externalImage) {
        try {
            TextureData td = TextureLoader::load(texture.sourcePath.string(), nativeTextureFormatSupport, nativeTextureRole, nativeTextureColorSpace);
            texture.width = static_cast<uint32_t>(td.width);
            texture.height = static_cast<uint32_t>(td.height);
            texture.channels = 4;
            texture.mipLevels = td.mipLevels;
            texture.isCompressed = td.isCompressed;
            texture.linearColorSpace = td.linearColorSpace;
            texture.format = td.format;
            texture.compressedFormat = td.compressedFormat;
            texture.sourceContainerKind = std::move(td.sourceContainerKind);
            texture.nativePayloadSource = std::move(td.nativePayloadSource);
            texture.sourceContainerPreserved = td.sourceContainerPreserved;
            texture.sourceContainerTranscoded = td.sourceContainerTranscoded;
            texture.rgba8 = std::move(td.pixels);
            texture.mipData = std::move(td.mipData);
            texture.fallback = false;
            return texture;
        } catch (const std::runtime_error& e) {
            std::cerr << "High-precision texture load failed: " << e.what() << ", trying decoded glTF image data\n";
        }
    }

    const size_t pixelCount = static_cast<size_t>(texture.width) * texture.height;
    const size_t expectedRgbaBytes = pixelCount * 4u;
    const size_t componentCount = static_cast<size_t>(std::max(image.component, 0));

    const bool canDecodeFloat =
        image.bits == 32 &&
        image.pixel_type == TINYGLTF_COMPONENT_TYPE_FLOAT &&
        image.component > 0 &&
        image.component <= 4 &&
        image.image.size() >= pixelCount * componentCount * sizeof(float);
    if (canDecodeFloat) {
        texture.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        texture.linearColorSpace = true;
        texture.rgba8.resize(pixelCount * 4u * sizeof(float));
        for (size_t i = 0; i < pixelCount; ++i) {
            const size_t src = i * componentCount;
            const size_t dst = i * 4u;
            float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (size_t c = 0; c < componentCount; ++c) {
                std::memcpy(&values[c], image.image.data() + (src + c) * sizeof(float), sizeof(float));
            }
            if (componentCount == 1) {
                values[1] = values[0];
                values[2] = values[0];
            }
            std::memcpy(texture.rgba8.data() + dst * sizeof(float), values, sizeof(values));
        }
        return texture;
    }

    const bool canDecode16Bit =
        image.bits == 16 &&
        image.component > 0 &&
        image.component <= 4 &&
        image.image.size() >= pixelCount * componentCount * sizeof(uint16_t);
    if (canDecode16Bit) {
        texture.format = VK_FORMAT_R16G16B16A16_UNORM;
        texture.rgba8.resize(pixelCount * 4u * sizeof(uint16_t));
        for (size_t i = 0; i < pixelCount; ++i) {
            const size_t src = i * componentCount;
            const size_t dst = i * 4u;
            uint16_t values[4] = {0u, 0u, 0u, 65535u};
            for (size_t c = 0; c < componentCount; ++c) {
                std::memcpy(&values[c], image.image.data() + (src + c) * sizeof(uint16_t), sizeof(uint16_t));
            }
            if (componentCount == 1) {
                values[1] = values[0];
                values[2] = values[0];
            }
            std::memcpy(texture.rgba8.data() + dst * sizeof(uint16_t), values, sizeof(values));
        }
        return texture;
    }

    const bool canDecode8Bit =
        image.bits == 8 &&
        image.component > 0 &&
        image.component <= 4 &&
        image.image.size() >= pixelCount * static_cast<size_t>(image.component);

    if (!canDecode8Bit) {
        texture.width = 1;
        texture.height = 1;
        texture.fallback = true;
        texture.rgba8 = {255, 255, 255, 255};
        std::cerr << "glTF texture fallback: "
                  << (texture.sourcePath.empty() ? std::string("<embedded>") : texture.sourcePath.string())
                  << " could not be decoded as 8-bit RGBA data; using white.\n";
        return texture;
    }

    texture.format = VK_FORMAT_R8G8B8A8_UNORM;
    texture.rgba8.resize(expectedRgbaBytes);
    if (image.component == 4 && image.image.size() == expectedRgbaBytes) {
        texture.rgba8 = image.image;
    } else {
        for (size_t i = 0; i < pixelCount; ++i) {
            const size_t src = i * static_cast<size_t>(image.component);
            const size_t dst = i * 4u;
            texture.rgba8[dst + 0] = image.image[src + 0];
            texture.rgba8[dst + 1] = image.component > 1 ? image.image[src + 1] : image.image[src + 0];
            texture.rgba8[dst + 2] = image.component > 2 ? image.image[src + 2] : image.image[src + 0];
            texture.rgba8[dst + 3] = image.component > 3 ? image.image[src + 3] : 255;
        }
    }
    return texture;
}

[[nodiscard]] TextureFilter textureFilterFromGltf(int filter) {
    switch (filter) {
    case TINYGLTF_TEXTURE_FILTER_NEAREST:
    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST:
    case TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR:
        return TextureFilter::Nearest;
    case TINYGLTF_TEXTURE_FILTER_LINEAR:
    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST:
    case TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR:
    default:
        return TextureFilter::Linear;
    }
}

[[nodiscard]] TextureWrap textureWrapFromGltf(int wrap) {
    switch (wrap) {
    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
        return TextureWrap::ClampToEdge;
    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
        return TextureWrap::MirroredRepeat;
    case TINYGLTF_TEXTURE_WRAP_REPEAT:
    default:
        return TextureWrap::Repeat;
    }
}

[[nodiscard]] TextureSamplerDesc samplerFromGltf(const tinygltf::Model& model, const tinygltf::Texture& texture) {
    TextureSamplerDesc desc{};
    if (texture.sampler < 0 || static_cast<size_t>(texture.sampler) >= model.samplers.size()) {
        return desc;
    }

    const tinygltf::Sampler& sampler = model.samplers[static_cast<size_t>(texture.sampler)];
    desc.minFilter = textureFilterFromGltf(sampler.minFilter);
    desc.magFilter = textureFilterFromGltf(sampler.magFilter);
    desc.wrapS = textureWrapFromGltf(sampler.wrapS);
    desc.wrapT = textureWrapFromGltf(sampler.wrapT);
    return desc;
}

[[nodiscard]] int textureImageSourceIndexFromGltf(
    const tinygltf::Texture& texture,
    size_t imageCount,
    size_t textureIndex) {
    auto extensionSource = [&](const char* extensionName) -> std::optional<int> {
        const auto extIt = texture.extensions.find(extensionName);
        if (extIt == texture.extensions.end() || !extIt->second.IsObject()) {
            return std::nullopt;
        }
        const auto& extObject = extIt->second.Get<tinygltf::Value::Object>();
        const auto sourceIt = extObject.find("source");
        if (sourceIt != extObject.end() && sourceIt->second.IsInt()) {
            const int source = sourceIt->second.Get<int>();
            if (source >= 0 && static_cast<size_t>(source) < imageCount) {
                return source;
            }
            std::cerr << "glTF texture warning: texture " << textureIndex
                      << " has " << extensionName << " source " << source
                      << " outside the image array; using core source fallback\n";
        } else {
            std::cerr << "glTF texture warning: texture " << textureIndex
                      << " has " << extensionName << " without an integer source; using core source fallback\n";
        }
        return std::nullopt;
    };

    if (const std::optional<int> source = extensionSource("KHR_texture_basisu")) {
        return *source;
    }
    if (const std::optional<int> source = extensionSource("MSFT_texture_dds")) {
        return *source;
    }
    return texture.source;
}

[[nodiscard]] bool supportedGltfExtension(const std::string& name) {
    return name == "KHR_materials_clearcoat" ||
        name == "KHR_materials_transmission" ||
        name == "KHR_materials_ior" ||
        name == "KHR_materials_specular" ||
        name == "KHR_materials_pbrSpecularGlossiness" ||
        name == "KHR_materials_volume" ||
        name == "KHR_materials_dispersion" ||
        name == "KHR_materials_sheen" ||
        name == "KHR_materials_iridescence" ||
        name == "KHR_materials_emissive_strength" ||
        name == "KHR_materials_anisotropy" ||
        name == "KHR_materials_unlit" ||
        name == "KHR_materials_variants" ||
        name == "KHR_mesh_quantization" ||
        name == "KHR_texture_basisu" ||
        name == "MSFT_texture_dds" ||
        name == "KHR_lights_punctual" ||
        name == "KHR_texture_transform";
}

[[nodiscard]] std::vector<std::string> unsupportedRequiredGltfExtensions(const tinygltf::Model& model) {
    std::vector<std::string> unsupported;
    for (const std::string& name : model.extensionsRequired) {
        if (!supportedGltfExtension(name)) {
            unsupported.push_back(name);
        }
    }
    return unsupported;
}

[[nodiscard]] std::string joinExtensionNames(const std::vector<std::string>& names) {
    std::ostringstream out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << names[i];
    }
    return out.str();
}

void reportGltfExtensionDiagnostics(const tinygltf::Model& model) {
    for (const std::string& name : model.extensionsRequired) {
        if (!supportedGltfExtension(name)) {
            std::cerr << "glTF required extension warning: unsupported required extension '" << name << "'\n";
        }
    }
    for (const std::string& name : model.extensionsUsed) {
        if (!supportedGltfExtension(name)) {
            std::cerr << "glTF extension warning: unsupported extension '" << name << "'\n";
        }
    }
}

struct PunctualLightJsonFallback {
    std::vector<tinygltf::Light> lights;
    std::vector<int> nodeLightIndices;
};

[[nodiscard]] bool hasGltfExtensionUsed(const tinygltf::Model& model, std::string_view name) {
    return std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), name) != model.extensionsUsed.end() ||
        std::find(model.extensionsRequired.begin(), model.extensionsRequired.end(), name) != model.extensionsRequired.end();
}

[[nodiscard]] bool isGltfJsonPath(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    return extension == ".gltf";
}

[[nodiscard]] std::optional<nlohmann::json> readGltfJsonDocument(const std::filesystem::path& path) {
    if (!isGltfJsonPath(path)) {
        return std::nullopt;
    }
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    try {
        nlohmann::json doc;
        in >> doc;
        return doc;
    } catch (const std::exception& e) {
        std::cerr << "glTF light warning: failed to parse JSON light fallback for "
                  << path.string() << ": " << e.what() << "\n";
    }
    return std::nullopt;
}

[[nodiscard]] const nlohmann::json* jsonObjectMember(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(key);
    return it != object.end() ? &*it : nullptr;
}

[[nodiscard]] double jsonNumberMember(const nlohmann::json& object, const char* key, double fallback) {
    const nlohmann::json* value = jsonObjectMember(object, key);
    return value != nullptr && value->is_number() ? value->get<double>() : fallback;
}

[[nodiscard]] std::string jsonStringMember(const nlohmann::json& object, const char* key, std::string fallback = {}) {
    const nlohmann::json* value = jsonObjectMember(object, key);
    return value != nullptr && value->is_string() ? value->get<std::string>() : fallback;
}

[[nodiscard]] std::vector<double> jsonVec3Member(const nlohmann::json& object, const char* key, std::vector<double> fallback) {
    const nlohmann::json* value = jsonObjectMember(object, key);
    if (value == nullptr || !value->is_array() || value->size() < 3) {
        return fallback;
    }
    std::vector<double> result(3, 1.0);
    for (size_t i = 0; i < 3; ++i) {
        if (!(*value)[i].is_number()) {
            return fallback;
        }
        result[i] = (*value)[i].get<double>();
    }
    return result;
}

[[nodiscard]] PunctualLightJsonFallback punctualLightFallbackFromGltfJson(
    const std::filesystem::path& path,
    size_t nodeCount) {
    PunctualLightJsonFallback fallback;
    fallback.nodeLightIndices.assign(nodeCount, -1);

    const std::optional<nlohmann::json> doc = readGltfJsonDocument(path);
    if (!doc.has_value() || !doc->is_object()) {
        return fallback;
    }

    const nlohmann::json* extensions = jsonObjectMember(*doc, "extensions");
    const nlohmann::json* punctual = extensions != nullptr ? jsonObjectMember(*extensions, "KHR_lights_punctual") : nullptr;
    const nlohmann::json* lights = punctual != nullptr ? jsonObjectMember(*punctual, "lights") : nullptr;
    if (lights != nullptr && lights->is_array()) {
        fallback.lights.reserve(lights->size());
        for (const nlohmann::json& item : *lights) {
            if (!item.is_object()) {
                continue;
            }
            tinygltf::Light light;
            light.name = jsonStringMember(item, "name");
            light.type = jsonStringMember(item, "type", "point");
            light.color = jsonVec3Member(item, "color", {1.0, 1.0, 1.0});
            light.intensity = jsonNumberMember(item, "intensity", 1.0);
            light.range = jsonNumberMember(item, "range", 0.0);
            if (const nlohmann::json* spot = jsonObjectMember(item, "spot")) {
                light.spot.innerConeAngle = jsonNumberMember(*spot, "innerConeAngle", light.spot.innerConeAngle);
                light.spot.outerConeAngle = jsonNumberMember(*spot, "outerConeAngle", light.spot.outerConeAngle);
            }
            fallback.lights.push_back(std::move(light));
        }
    }

    const nlohmann::json* nodes = jsonObjectMember(*doc, "nodes");
    if (nodes != nullptr && nodes->is_array()) {
        const size_t count = std::min(nodeCount, nodes->size());
        for (size_t i = 0; i < count; ++i) {
            const nlohmann::json& node = (*nodes)[i];
            const nlohmann::json* nodeExtensions = jsonObjectMember(node, "extensions");
            const nlohmann::json* nodePunctual = nodeExtensions != nullptr ? jsonObjectMember(*nodeExtensions, "KHR_lights_punctual") : nullptr;
            const nlohmann::json* light = nodePunctual != nullptr ? jsonObjectMember(*nodePunctual, "light") : nullptr;
            if (light != nullptr && light->is_number_integer()) {
                fallback.nodeLightIndices[i] = light->get<int>();
            }
        }
    }

    return fallback;
}

[[nodiscard]] MaterialAsset materialFromGltf(const tinygltf::Material& source, const std::vector<TextureAssetHandle>& textures) {
    auto textureHandle = [&](int textureIndex) {
        if (textureIndex >= 0 && static_cast<size_t>(textureIndex) < textures.size()) {
            return textures[static_cast<size_t>(textureIndex)];
        }
        return TextureAssetHandle{};
    };
    auto extensionObject = [&](std::string_view name) -> const tinygltf::Value* {
        const auto it = source.extensions.find(std::string{name});
        if (it == source.extensions.end() || !it->second.IsObject()) {
            return nullptr;
        }
        return &it->second;
    };
    auto valueMember = [](const tinygltf::Value& object, const char* key) -> const tinygltf::Value* {
        if (!object.IsObject()) {
            return nullptr;
        }
        const auto& values = object.Get<tinygltf::Value::Object>();
        const auto it = values.find(key);
        return it != values.end() ? &it->second : nullptr;
    };
    auto numberMember = [&](const tinygltf::Value& object, const char* key, double fallback) {
        const tinygltf::Value* value = valueMember(object, key);
        return value != nullptr && value->IsNumber() ? value->GetNumberAsDouble() : fallback;
    };
    auto intMember = [&](const tinygltf::Value& object, const char* key, int fallback) {
        const tinygltf::Value* value = valueMember(object, key);
        return value != nullptr && value->IsInt() ? value->Get<int>() : fallback;
    };
    auto vec3Member = [&](const tinygltf::Value& object, const char* key, glm::vec3 fallback) {
        const tinygltf::Value* value = valueMember(object, key);
        if (value == nullptr || !value->IsArray()) {
            return fallback;
        }
        const auto& array = value->Get<tinygltf::Value::Array>();
        if (array.size() < 3 || !array[0].IsNumber() || !array[1].IsNumber() || !array[2].IsNumber()) {
            return fallback;
        }
        return glm::vec3{
            static_cast<float>(array[0].GetNumberAsDouble()),
            static_cast<float>(array[1].GetNumberAsDouble()),
            static_cast<float>(array[2].GetNumberAsDouble()),
        };
    };
    auto vec4Member = [&](const tinygltf::Value& object, const char* key, glm::vec4 fallback) {
        const tinygltf::Value* value = valueMember(object, key);
        if (value == nullptr || !value->IsArray()) {
            return fallback;
        }
        const auto& array = value->Get<tinygltf::Value::Array>();
        if (array.size() < 4 || !array[0].IsNumber() || !array[1].IsNumber() || !array[2].IsNumber() || !array[3].IsNumber()) {
            return fallback;
        }
        return glm::vec4{
            static_cast<float>(array[0].GetNumberAsDouble()),
            static_cast<float>(array[1].GetNumberAsDouble()),
            static_cast<float>(array[2].GetNumberAsDouble()),
            static_cast<float>(array[3].GetNumberAsDouble()),
        };
    };
    auto textureHandleMember = [&](const tinygltf::Value& object, const char* key) {
        const tinygltf::Value* textureInfo = valueMember(object, key);
        if (textureInfo == nullptr || !textureInfo->IsObject()) {
            return TextureAssetHandle{};
        }
        return textureHandle(intMember(*textureInfo, "index", -1));
    };
    auto textureTransformFromObject = [&](const tinygltf::Value& textureTransform, int texCoordFallback) {
        TextureTransformAsset transform{};
        transform.enabled = 1u;
        if (const tinygltf::Value* offset = valueMember(textureTransform, "offset"); offset != nullptr && offset->IsArray()) {
            const auto& array = offset->Get<tinygltf::Value::Array>();
            if (array.size() >= 2 && array[0].IsNumber() && array[1].IsNumber()) {
                transform.offset = {static_cast<float>(array[0].GetNumberAsDouble()), static_cast<float>(array[1].GetNumberAsDouble())};
            }
        }
        if (const tinygltf::Value* scale = valueMember(textureTransform, "scale"); scale != nullptr && scale->IsArray()) {
            const auto& array = scale->Get<tinygltf::Value::Array>();
            if (array.size() >= 2 && array[0].IsNumber() && array[1].IsNumber()) {
                transform.scale = {static_cast<float>(array[0].GetNumberAsDouble()), static_cast<float>(array[1].GetNumberAsDouble())};
            }
        }
        transform.rotation = static_cast<float>(numberMember(textureTransform, "rotation", 0.0));
        transform.texCoord = static_cast<uint32_t>(std::max(intMember(textureTransform, "texCoord", texCoordFallback), 0));
        return transform;
    };
    auto textureTransformFromInfo = [&](const auto& textureInfo) {
        TextureTransformAsset transform{};
        transform.texCoord = static_cast<uint32_t>(std::max(textureInfo.texCoord, 0));
        const auto extIt = textureInfo.extensions.find("KHR_texture_transform");
        if (extIt == textureInfo.extensions.end() || !extIt->second.IsObject()) {
            return transform;
        }
        return textureTransformFromObject(extIt->second, textureInfo.texCoord);
    };
    auto textureTransformMember = [&](const tinygltf::Value& object, const char* key) {
        const tinygltf::Value* textureInfo = valueMember(object, key);
        if (textureInfo == nullptr || !textureInfo->IsObject()) {
            return TextureTransformAsset{};
        }
        TextureTransformAsset transform{};
        transform.texCoord = static_cast<uint32_t>(std::max(intMember(*textureInfo, "texCoord", 0), 0));
        const tinygltf::Value* extensions = valueMember(*textureInfo, "extensions");
        if (extensions == nullptr || !extensions->IsObject()) {
            return transform;
        }
        const tinygltf::Value* textureTransform = valueMember(*extensions, "KHR_texture_transform");
        if (textureTransform == nullptr || !textureTransform->IsObject()) {
            return transform;
        }
        return textureTransformFromObject(*textureTransform, intMember(*textureInfo, "texCoord", 0));
    };

    auto textureInfoHasTransform = [&](const TextureTransformAsset& transform) {
        return transform.enabled != 0u;
    };

    auto textureInfoAnyTransform = [&](std::initializer_list<TextureTransformAsset> transforms) {
        return std::any_of(transforms.begin(), transforms.end(), textureInfoHasTransform);
    };

    auto assignExtensionTextureTransform = [&](TextureTransformAsset& destination, const tinygltf::Value& object, const char* key) {
        destination = textureTransformMember(object, key);
    };

    MaterialAsset material;
    material.name = source.name;
    const auto& pbr = source.pbrMetallicRoughness;
    if (pbr.baseColorFactor.size() == 4) {
        material.baseColorFactor = {
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3]),
        };
    }
    material.metallicFactor = static_cast<float>(pbr.metallicFactor);
    material.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
    material.alphaCutoff = static_cast<float>(source.alphaCutoff);
    material.doubleSided = source.doubleSided ? 1u : 0u;
    if (source.alphaMode == "MASK") {
        material.alphaMode = 1u;
    } else if (source.alphaMode == "BLEND") {
        material.alphaMode = 2u;
    }
    if (source.emissiveFactor.size() == 3) {
        material.emissiveFactor = {
            static_cast<float>(source.emissiveFactor[0]),
            static_cast<float>(source.emissiveFactor[1]),
            static_cast<float>(source.emissiveFactor[2]),
        };
    }
    material.baseColorTexture = textureHandle(pbr.baseColorTexture.index);
    material.metallicRoughnessTexture = textureHandle(pbr.metallicRoughnessTexture.index);
    material.normalTexture = textureHandle(source.normalTexture.index);
    material.emissiveTexture = textureHandle(source.emissiveTexture.index);
    material.occlusionTexture = textureHandle(source.occlusionTexture.index);
    material.occlusionStrength = static_cast<float>(std::clamp(source.occlusionTexture.strength, 0.0, 1.0));
    material.baseColorTextureTransform = textureTransformFromInfo(pbr.baseColorTexture);
    material.metallicRoughnessTextureTransform = textureTransformFromInfo(pbr.metallicRoughnessTexture);
    material.normalTextureTransform = textureTransformFromInfo(source.normalTexture);
    material.emissiveTextureTransform = textureTransformFromInfo(source.emissiveTexture);
    material.occlusionTextureTransform = textureTransformFromInfo(source.occlusionTexture);

    if (const tinygltf::Value* ext = extensionObject("KHR_materials_pbrSpecularGlossiness")) {
        const float glossinessFactor = static_cast<float>(std::clamp(numberMember(*ext, "glossinessFactor", 1.0), 0.0, 1.0));
        material.materialWorkflow = kMaterialWorkflowSpecularGlossiness;
        material.baseColorFactor = vec4Member(*ext, "diffuseFactor", glm::vec4{1.0f});
        material.baseColorTexture = textureHandleMember(*ext, "diffuseTexture");
        assignExtensionTextureTransform(material.baseColorTextureTransform, *ext, "diffuseTexture");
        material.metallicFactor = 0.0f;
        material.roughnessFactor = std::clamp(1.0f - glossinessFactor, 0.0f, 1.0f);
        material.hasSpecular = 1u;
        material.specularFactor = 1.0f;
        material.specularColorFactor = vec3Member(*ext, "specularFactor", glm::vec3{1.0f});
        material.specularTexture = textureHandleMember(*ext, "specularGlossinessTexture");
        assignExtensionTextureTransform(material.specularTextureTransform, *ext, "specularGlossinessTexture");
        material.specularTextureAlphaMode = kMaterialSpecularTextureAlphaGlossiness;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_ior")) {
        material.hasIor = 1u;
        material.iorFactor = static_cast<float>(numberMember(*ext, "ior", 1.5));
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_emissive_strength")) {
        material.hasEmissiveStrength = 1u;
        material.emissiveStrength = static_cast<float>(numberMember(*ext, "emissiveStrength", 1.0));
        material.emissiveFactor *= material.emissiveStrength;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_clearcoat")) {
        material.hasClearcoat = 1u;
        material.clearcoatFactor = static_cast<float>(numberMember(*ext, "clearcoatFactor", 0.0));
        material.clearcoatRoughnessFactor = static_cast<float>(numberMember(*ext, "clearcoatRoughnessFactor", 0.0));
        material.clearcoatTexture = textureHandleMember(*ext, "clearcoatTexture");
        material.clearcoatRoughnessTexture = textureHandleMember(*ext, "clearcoatRoughnessTexture");
        material.clearcoatNormalTexture = textureHandleMember(*ext, "clearcoatNormalTexture");
        assignExtensionTextureTransform(material.clearcoatTextureTransform, *ext, "clearcoatTexture");
        assignExtensionTextureTransform(material.clearcoatRoughnessTextureTransform, *ext, "clearcoatRoughnessTexture");
        assignExtensionTextureTransform(material.clearcoatNormalTextureTransform, *ext, "clearcoatNormalTexture");
        material.shaderCompatibilityMask |= kMaterialClosureFlagClearcoat;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_transmission")) {
        material.hasTransmission = 1u;
        material.transmissionFactor = static_cast<float>(numberMember(*ext, "transmissionFactor", 0.0));
        material.transmissionTexture = textureHandleMember(*ext, "transmissionTexture");
        assignExtensionTextureTransform(material.transmissionTextureTransform, *ext, "transmissionTexture");
        material.shaderCompatibilityMask |= kMaterialClosureFlagTransmission;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_volume")) {
        material.hasVolume = 1u;
        material.volumeThicknessFactor = static_cast<float>(numberMember(*ext, "thicknessFactor", 0.0));
        material.volumeThicknessTexture = textureHandleMember(*ext, "thicknessTexture");
        material.volumeAttenuationDistance = static_cast<float>(numberMember(*ext, "attenuationDistance", 0.0));
        material.volumeAttenuationColor = vec3Member(*ext, "attenuationColor", glm::vec3{1.0f});
        assignExtensionTextureTransform(material.volumeThicknessTextureTransform, *ext, "thicknessTexture");
        material.shaderCompatibilityMask |= kMaterialClosureFlagVolume;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_dispersion")) {
        material.hasDispersion = 1u;
        material.dispersionFactor = std::max(0.0f, static_cast<float>(numberMember(*ext, "dispersion", 0.0)));
        if (material.dispersionFactor > 0.0f) {
            material.shaderCompatibilityMask |= kMaterialClosureFlagDispersion;
        }
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_specular")) {
        material.hasSpecular = 1u;
        material.specularFactor = static_cast<float>(numberMember(*ext, "specularFactor", 1.0));
        material.specularColorFactor = vec3Member(*ext, "specularColorFactor", glm::vec3{1.0f});
        material.specularTexture = textureHandleMember(*ext, "specularTexture");
        material.specularColorTexture = textureHandleMember(*ext, "specularColorTexture");
        assignExtensionTextureTransform(material.specularTextureTransform, *ext, "specularTexture");
        assignExtensionTextureTransform(material.specularColorTextureTransform, *ext, "specularColorTexture");
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_sheen")) {
        material.hasSheen = 1u;
        material.sheenColorFactor = vec3Member(*ext, "sheenColorFactor", glm::vec3{0.0f});
        material.sheenRoughnessFactor = static_cast<float>(numberMember(*ext, "sheenRoughnessFactor", 0.0));
        material.sheenColorTexture = textureHandleMember(*ext, "sheenColorTexture");
        material.sheenRoughnessTexture = textureHandleMember(*ext, "sheenRoughnessTexture");
        assignExtensionTextureTransform(material.sheenColorTextureTransform, *ext, "sheenColorTexture");
        assignExtensionTextureTransform(material.sheenRoughnessTextureTransform, *ext, "sheenRoughnessTexture");
        material.shaderCompatibilityMask |= kMaterialClosureFlagSheen;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_iridescence")) {
        material.hasIridescence = 1u;
        material.iridescenceFactor = static_cast<float>(numberMember(*ext, "iridescenceFactor", 0.0));
        material.iridescenceIor = static_cast<float>(numberMember(*ext, "iridescenceIor", 1.3));
        material.iridescenceThicknessMinimum = static_cast<float>(numberMember(*ext, "iridescenceThicknessMinimum", 100.0));
        material.iridescenceThicknessMaximum = static_cast<float>(numberMember(*ext, "iridescenceThicknessMaximum", 400.0));
        material.iridescenceTexture = textureHandleMember(*ext, "iridescenceTexture");
        material.iridescenceThicknessTexture = textureHandleMember(*ext, "iridescenceThicknessTexture");
        assignExtensionTextureTransform(material.iridescenceTextureTransform, *ext, "iridescenceTexture");
        assignExtensionTextureTransform(material.iridescenceThicknessTextureTransform, *ext, "iridescenceThicknessTexture");
        material.shaderCompatibilityMask |= kMaterialClosureFlagThinFilm;
    }
    if (const tinygltf::Value* ext = extensionObject("KHR_materials_anisotropy")) {
        material.hasAnisotropy = 1u;
        material.anisotropyStrength = static_cast<float>(numberMember(*ext, "anisotropyStrength", 0.0));
        material.anisotropyRotation = static_cast<float>(numberMember(*ext, "anisotropyRotation", 0.0));
        material.anisotropyTexture = textureHandleMember(*ext, "anisotropyTexture");
        assignExtensionTextureTransform(material.anisotropyTextureTransform, *ext, "anisotropyTexture");
    }
    if (extensionObject("KHR_materials_unlit") != nullptr) {
        material.shaderCompatibilityMask |= kMaterialClosureFlagUnlit;
    }
    for (const auto& [name, extensionValue] : source.extensions) {
        if (!extensionValue.IsObject()) {
            continue;
        }
        if (name == "KHR_materials_clearcoat" || name == "KHR_materials_transmission" ||
            name == "KHR_materials_ior" || name == "KHR_materials_specular" ||
            name == "KHR_materials_volume" || name == "KHR_materials_dispersion" ||
            name == "KHR_materials_sheen" || name == "KHR_materials_emissive_strength" ||
            name == "KHR_materials_iridescence" || name == "KHR_materials_anisotropy" ||
            name == "KHR_materials_unlit" || name == "KHR_materials_pbrSpecularGlossiness") {
            continue;
        }
        std::cerr << "glTF material extension warning: material '" << (source.name.empty() ? "(unnamed)" : source.name)
                  << "' uses unsupported extension '" << name << "'\n";
    }

    const bool hasTextureTransform =
        textureInfoAnyTransform({
            material.baseColorTextureTransform,
            material.metallicRoughnessTextureTransform,
            material.normalTextureTransform,
            material.emissiveTextureTransform,
            material.occlusionTextureTransform,
            material.clearcoatTextureTransform,
            material.clearcoatRoughnessTextureTransform,
            material.clearcoatNormalTextureTransform,
            material.transmissionTextureTransform,
            material.volumeThicknessTextureTransform,
            material.specularTextureTransform,
            material.specularColorTextureTransform,
            material.sheenColorTextureTransform,
            material.sheenRoughnessTextureTransform,
            material.iridescenceTextureTransform,
            material.iridescenceThicknessTextureTransform,
            material.anisotropyTextureTransform,
        });
    if (material.hasIor != 0u || material.hasClearcoat != 0u || material.hasTransmission != 0u ||
        material.hasVolume != 0u || material.hasDispersion != 0u || material.hasSpecular != 0u || material.hasSheen != 0u || material.hasEmissiveStrength != 0u ||
        material.hasIridescence != 0u || material.hasAnisotropy != 0u ||
        (material.shaderCompatibilityMask & kMaterialClosureFlagUnlit) != 0u || hasTextureTransform) {
        std::cout << "glTF material extensions: material '"
                  << (source.name.empty() ? "(unnamed)" : source.name)
                  << "' ior=" << material.iorFactor
                  << " clearcoat=" << material.clearcoatFactor
                  << " transmission=" << material.transmissionFactor
                  << " volumeThickness=" << material.volumeThicknessFactor
                  << " volumeAttenuationDistance=" << material.volumeAttenuationDistance
                  << " dispersion=" << material.dispersionFactor
                  << " specular=" << material.specularFactor
                  << " sheenRoughness=" << material.sheenRoughnessFactor
                  << " iridescence=" << material.iridescenceFactor
                  << " emissiveStrength=" << material.emissiveStrength
                  << " anisotropy=" << material.anisotropyStrength
                  << " unlit=" << (((material.shaderCompatibilityMask & kMaterialClosureFlagUnlit) != 0u) ? "yes" : "no")
                  << " textureTransform=" << (hasTextureTransform ? "yes" : "no")
                  << '\n';
    }
    return material;
}

void finalizePrimitiveVertexFrames(
    MeshAsset& mesh,
    uint32_t firstVertex,
    uint32_t vertexCount,
    uint32_t firstIndex,
    uint32_t indexCount,
    bool hasNormals,
    bool hasTangents,
    bool hasTexcoords) {
    if (!hasNormals) {
        for (uint32_t i = 0; i < vertexCount; ++i) {
            mesh.vertices[firstVertex + i].normal = glm::vec3{0.0f};
        }
        for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
            const uint32_t i0 = mesh.indices[firstIndex + i + 0];
            const uint32_t i1 = mesh.indices[firstIndex + i + 1];
            const uint32_t i2 = mesh.indices[firstIndex + i + 2];
            const glm::vec3 p0 = mesh.vertices[i0].position;
            const glm::vec3 p1 = mesh.vertices[i1].position;
            const glm::vec3 p2 = mesh.vertices[i2].position;
            const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
            mesh.vertices[i0].normal += faceNormal;
            mesh.vertices[i1].normal += faceNormal;
            mesh.vertices[i2].normal += faceNormal;
        }
    }

    std::vector<glm::vec3> tangentAccum(vertexCount, glm::vec3{0.0f});
    std::vector<glm::vec3> bitangentAccum(vertexCount, glm::vec3{0.0f});
    if (!hasTangents && hasTexcoords) {
        for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
            const uint32_t i0 = mesh.indices[firstIndex + i + 0];
            const uint32_t i1 = mesh.indices[firstIndex + i + 1];
            const uint32_t i2 = mesh.indices[firstIndex + i + 2];
            const glm::vec3 p0 = mesh.vertices[i0].position;
            const glm::vec3 p1 = mesh.vertices[i1].position;
            const glm::vec3 p2 = mesh.vertices[i2].position;
            const glm::vec2 uv0 = mesh.vertices[i0].texcoord;
            const glm::vec2 uv1 = mesh.vertices[i1].texcoord;
            const glm::vec2 uv2 = mesh.vertices[i2].texcoord;
            const glm::vec3 e1 = p1 - p0;
            const glm::vec3 e2 = p2 - p0;
            const glm::vec2 duv1 = uv1 - uv0;
            const glm::vec2 duv2 = uv2 - uv0;
            const float det = duv1.x * duv2.y - duv1.y * duv2.x;
            if (std::abs(det) <= 1.0e-8f) {
                continue;
            }
            const glm::vec3 tangent = (e1 * duv2.y - e2 * duv1.y) / det;
            const glm::vec3 bitangent = (e2 * duv1.x - e1 * duv2.x) / det;
            tangentAccum[i0 - firstVertex] += tangent;
            tangentAccum[i1 - firstVertex] += tangent;
            tangentAccum[i2 - firstVertex] += tangent;
            bitangentAccum[i0 - firstVertex] += bitangent;
            bitangentAccum[i1 - firstVertex] += bitangent;
            bitangentAccum[i2 - firstVertex] += bitangent;
        }
    }

    for (uint32_t i = 0; i < vertexCount; ++i) {
        MeshVertex& vertex = mesh.vertices[firstVertex + i];
        const float normalLen2 = glm::dot(vertex.normal, vertex.normal);
        vertex.normal = normalLen2 > 1.0e-12f ? vertex.normal / std::sqrt(normalLen2) : glm::vec3{0.0f, 1.0f, 0.0f};

        if (!hasTangents) {
            glm::vec3 tangent = hasTexcoords ? tangentAccum[i] : glm::vec3{0.0f};
            tangent -= vertex.normal * glm::dot(vertex.normal, tangent);
            const float tangentLen2 = glm::dot(tangent, tangent);
            if (tangentLen2 <= 1.0e-12f) {
                const glm::vec3 helper = std::abs(vertex.normal.y) < 0.95f ? glm::vec3{0.0f, 1.0f, 0.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
                tangent = glm::normalize(glm::cross(helper, vertex.normal));
            } else {
                tangent /= std::sqrt(tangentLen2);
            }
            const glm::vec3 bitangent = hasTexcoords ? bitangentAccum[i] : glm::vec3{0.0f};
            const float handedness = glm::dot(glm::cross(vertex.normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
            vertex.tangent = glm::vec4{tangent, handedness};
        }
    }
}

} // namespace

GltfLoader::GltfLoader(AssetManager& assets)
    : assets_(assets) {}

SceneAsset GltfLoader::load(const std::filesystem::path& path) {
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(loadImageDataPreservingKtx2, nullptr);
    tinygltf::Model model;
    std::string error;
    std::string warning;
    const bool binary = path.extension() == ".glb";
    const bool ok = binary
        ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
        : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
    if (!warning.empty()) {
        std::cerr << "glTF load warning: " << warning << '\n';
    }
    if (!ok) {
        throw std::runtime_error("glTF load failed: " + path.string() + " " + error);
    }
    reportGltfExtensionDiagnostics(model);
    const std::vector<std::string> unsupportedRequired = unsupportedRequiredGltfExtensions(model);
    if (!unsupportedRequired.empty()) {
        throw std::runtime_error(
            "glTF load blocked: unsupported required extension(s): " + joinExtensionNames(unsupportedRequired));
    }
    lastExternalDependencies_ = gltfExternalDependencies(model, path);

    SceneAsset scene;
    scene.name = path.filename().string();
    scene.sourcePath = path;

    std::vector<bool> colorTextureUse(model.textures.size(), false);
    std::vector<bool> dataTextureUse(model.textures.size(), false);
    std::vector<NativeTextureRole> nativeTextureRoles(model.textures.size(), NativeTextureRole::Unknown);
    auto extensionTextureIndex = [](const tinygltf::Material& material, const char* extensionName, const char* textureName) -> int {
        const auto extIt = material.extensions.find(extensionName);
        if (extIt == material.extensions.end() || !extIt->second.IsObject()) {
            return -1;
        }
        const auto& extObject = extIt->second.Get<tinygltf::Value::Object>();
        const auto texIt = extObject.find(textureName);
        if (texIt == extObject.end() || !texIt->second.IsObject()) {
            return -1;
        }
        const auto& texObject = texIt->second.Get<tinygltf::Value::Object>();
        const auto indexIt = texObject.find("index");
        return indexIt != texObject.end() && indexIt->second.IsInt() ? indexIt->second.Get<int>() : -1;
    };
    auto markTextureUse = [&](int textureIndex, std::vector<bool>& use) {
        if (textureIndex >= 0 && static_cast<size_t>(textureIndex) < use.size()) {
            use[static_cast<size_t>(textureIndex)] = true;
        }
    };
    auto rolePriority = [](NativeTextureRole role) -> int {
        switch (role) {
        case NativeTextureRole::Normal: return 6;
        case NativeTextureRole::BaseColor: return 5;
        case NativeTextureRole::Emissive: return 5;
        case NativeTextureRole::MetallicRoughness: return 4;
        case NativeTextureRole::Occlusion: return 3;
        case NativeTextureRole::Opacity: return 3;
        case NativeTextureRole::Data: return 2;
        case NativeTextureRole::Unknown: return 0;
        default: return 1;
        }
    };
    auto markTextureRole = [&](int textureIndex, NativeTextureRole role) {
        if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= nativeTextureRoles.size()) {
            return;
        }
        NativeTextureRole& current = nativeTextureRoles[static_cast<size_t>(textureIndex)];
        if (rolePriority(role) > rolePriority(current)) {
            current = role;
        }
    };
    for (const tinygltf::Material& sourceMaterial : model.materials) {
        const auto& pbr = sourceMaterial.pbrMetallicRoughness;
        markTextureUse(pbr.baseColorTexture.index, colorTextureUse);
        markTextureRole(pbr.baseColorTexture.index, NativeTextureRole::BaseColor);
        markTextureUse(sourceMaterial.emissiveTexture.index, colorTextureUse);
        markTextureRole(sourceMaterial.emissiveTexture.index, NativeTextureRole::Emissive);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_pbrSpecularGlossiness", "diffuseTexture"), colorTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_pbrSpecularGlossiness", "diffuseTexture"), NativeTextureRole::BaseColor);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture"), NativeTextureRole::Data);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_sheen", "sheenColorTexture"), colorTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_sheen", "sheenColorTexture"), NativeTextureRole::BaseColor);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_specular", "specularColorTexture"), colorTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_specular", "specularColorTexture"), NativeTextureRole::BaseColor);
        markTextureUse(pbr.metallicRoughnessTexture.index, dataTextureUse);
        markTextureRole(pbr.metallicRoughnessTexture.index, NativeTextureRole::MetallicRoughness);
        markTextureUse(sourceMaterial.normalTexture.index, dataTextureUse);
        markTextureRole(sourceMaterial.normalTexture.index, NativeTextureRole::Normal);
        markTextureUse(sourceMaterial.occlusionTexture.index, dataTextureUse);
        markTextureRole(sourceMaterial.occlusionTexture.index, NativeTextureRole::Occlusion);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_clearcoat", "clearcoatTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_clearcoat", "clearcoatTexture"), NativeTextureRole::Data);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_clearcoat", "clearcoatRoughnessTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_clearcoat", "clearcoatRoughnessTexture"), NativeTextureRole::Roughness);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_clearcoat", "clearcoatNormalTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_clearcoat", "clearcoatNormalTexture"), NativeTextureRole::Normal);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_transmission", "transmissionTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_transmission", "transmissionTexture"), NativeTextureRole::Opacity);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_volume", "thicknessTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_volume", "thicknessTexture"), NativeTextureRole::Data);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_specular", "specularTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_specular", "specularTexture"), NativeTextureRole::Data);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_sheen", "sheenRoughnessTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_sheen", "sheenRoughnessTexture"), NativeTextureRole::Roughness);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_iridescence", "iridescenceTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_iridescence", "iridescenceTexture"), NativeTextureRole::Data);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_iridescence", "iridescenceThicknessTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_iridescence", "iridescenceThicknessTexture"), NativeTextureRole::Data);
        markTextureUse(extensionTextureIndex(sourceMaterial, "KHR_materials_anisotropy", "anisotropyTexture"), dataTextureUse);
        markTextureRole(extensionTextureIndex(sourceMaterial, "KHR_materials_anisotropy", "anisotropyTexture"), NativeTextureRole::Data);
    }

    std::vector<TextureAssetHandle> textureHandles;
    textureHandles.reserve(model.textures.size());
    for (size_t textureIndex = 0; textureIndex < model.textures.size(); ++textureIndex) {
        const tinygltf::Texture& sourceTexture = model.textures[textureIndex];
        TextureAsset texture;
        const int sourceImageIndex = textureImageSourceIndexFromGltf(sourceTexture, model.images.size(), textureIndex);
        if (sourceImageIndex >= 0 && static_cast<size_t>(sourceImageIndex) < model.images.size()) {
            texture = textureFromImage(
                model.images[static_cast<size_t>(sourceImageIndex)],
                path,
                nativeTextureFormatSupport_,
                nativeTextureRoles[textureIndex]);
        }
        texture.name = texture.name.empty() ? sourceTexture.name : texture.name;
        if (texture.name.empty() && !texture.sourcePath.empty() && texture.sourcePath != path) {
            texture.name = texture.sourcePath.stem().string();
        }
        texture.srgb = colorTextureUse[textureIndex] && !dataTextureUse[textureIndex] && !texture.linearColorSpace;
        texture.sampler = samplerFromGltf(model, sourceTexture);
        textureHandles.push_back(assets_.addTexture(std::move(texture)));
    }
    scene.textures = textureHandles;

    std::vector<MaterialAssetHandle> materialHandles;
    materialHandles.reserve(model.materials.size());
    for (const tinygltf::Material& sourceMaterial : model.materials) {
        materialHandles.push_back(assets_.addMaterial(materialFromGltf(sourceMaterial, textureHandles)));
    }
    if (materialHandles.empty()) {
        materialHandles.push_back(assets_.addMaterial(MaterialAsset{.name = "default"}));
    }
    scene.materials = materialHandles;

    if (const auto variantsExtIt = model.extensions.find("KHR_materials_variants");
        variantsExtIt != model.extensions.end() && variantsExtIt->second.IsObject()) {
        const auto& extObject = variantsExtIt->second.Get<tinygltf::Value::Object>();
        const auto variantsIt = extObject.find("variants");
        if (variantsIt != extObject.end() && variantsIt->second.IsArray()) {
            const auto& variants = variantsIt->second.Get<tinygltf::Value::Array>();
            scene.materialVariants.reserve(variants.size());
            for (size_t variantIndex = 0; variantIndex < variants.size(); ++variantIndex) {
                std::string name = "Variant " + std::to_string(variantIndex);
                if (variants[variantIndex].IsObject()) {
                    const auto& variantObject = variants[variantIndex].Get<tinygltf::Value::Object>();
                    const auto nameIt = variantObject.find("name");
                    if (nameIt != variantObject.end() && nameIt->second.IsString()) {
                        name = nameIt->second.Get<std::string>();
                    }
                }
                scene.materialVariants.push_back(std::move(name));
            }
        }
    }

    std::vector<MeshAssetHandle> meshHandles;
    meshHandles.reserve(model.meshes.size());
    for (const tinygltf::Mesh& sourceMesh : model.meshes) {
        MeshAsset mesh;
        mesh.name = sourceMesh.name;
        mesh.defaultMorphWeights = morphWeightsFromGltf(sourceMesh.weights);
        const std::vector<std::string> morphTargetNames = morphTargetNamesForMesh(sourceMesh);

        for (const tinygltf::Primitive& primitive : sourceMesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                continue;
            }

            const uint32_t firstVertex = static_cast<uint32_t>(mesh.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
            const auto positionIt = primitive.attributes.find("POSITION");
            if (positionIt == primitive.attributes.end()) {
                continue;
            }

            const auto posView = makeFloatAttributeAccessorView(
                model,
                positionIt->second,
                TINYGLTF_TYPE_VEC3,
                "POSITION",
                FloatAttributeComponentPolicy::PositionOrTexcoord);
            if (!posView.has_value()) {
                continue;
            }
            const tinygltf::Accessor& positionAccessor = model.accessors[static_cast<size_t>(positionIt->second)];
            const size_t vertexCount = posView->count;
            if (vertexCount == 0 || vertexCount > std::numeric_limits<uint32_t>::max()) {
                std::cerr << "glTF primitive warning: POSITION vertex count is invalid; skipping primitive\n";
                continue;
            }
            if (mesh.vertices.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - vertexCount) {
                std::cerr << "glTF primitive warning: mesh vertex count would exceed 32-bit index range; skipping primitive\n";
                continue;
            }

            std::vector<uint32_t> decodedIndices;
            if (primitive.indices >= 0) {
                const auto indexView = makeIndexAccessorView(model, primitive.indices);
                if (!indexView.has_value()) {
                    continue;
                }
                const tinygltf::Accessor& indexAccessor = model.accessors[static_cast<size_t>(primitive.indices)];
                decodedIndices.reserve(indexView->count);
                bool indicesValid = true;
                for (size_t i = 0; i < indexView->count; ++i) {
                    const uint32_t index = readIndexElement(indexAccessor, *indexView, i);
                    if (index >= vertexCount) {
                        std::cerr << "glTF primitive warning: index " << index << " exceeds vertex count " << vertexCount << "; skipping primitive\n";
                        indicesValid = false;
                        break;
                    }
                    decodedIndices.push_back(firstVertex + index);
                }
                if (!indicesValid) {
                    continue;
                }
            } else {
                decodedIndices.reserve(vertexCount);
                for (uint32_t i = 0; i < static_cast<uint32_t>(vertexCount); ++i) {
                    decodedIndices.push_back(firstVertex + i);
                }
            }

            mesh.vertices.resize(mesh.vertices.size() + vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                mesh.vertices[firstVertex + i].position = readFloatAttributeVec3(positionAccessor, *posView, i);
            }

            const auto normalIt = primitive.attributes.find("NORMAL");
            bool hasNormals = false;
            if (normalIt != primitive.attributes.end()) {
                const auto normalView = makeFloatAttributeAccessorView(
                    model,
                    normalIt->second,
                    TINYGLTF_TYPE_VEC3,
                    "NORMAL",
                    FloatAttributeComponentPolicy::SignedNormalizedInteger);
                if (normalView.has_value() && normalView->count >= vertexCount) {
                    const tinygltf::Accessor& normalAccessor = model.accessors[static_cast<size_t>(normalIt->second)];
                    hasNormals = true;
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].normal = readFloatAttributeVec3(normalAccessor, *normalView, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: NORMAL count is shorter than POSITION count; generating normals\n";
                }
            }

            const auto uvIt = primitive.attributes.find("TEXCOORD_0");
            bool hasTexcoords = false;
            if (uvIt != primitive.attributes.end()) {
                const auto uvView = makeFloatAttributeAccessorView(
                    model,
                    uvIt->second,
                    TINYGLTF_TYPE_VEC2,
                    "TEXCOORD_0",
                    FloatAttributeComponentPolicy::PositionOrTexcoord);
                if (uvView.has_value() && uvView->count >= vertexCount) {
                    const tinygltf::Accessor& uvAccessor = model.accessors[static_cast<size_t>(uvIt->second)];
                    hasTexcoords = true;
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].texcoord = readFloatAttributeVec2(uvAccessor, *uvView, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: TEXCOORD_0 count is shorter than POSITION count; using default UVs\n";
                }
            }

            const auto uv1It = primitive.attributes.find("TEXCOORD_1");
            if (uv1It != primitive.attributes.end()) {
                const auto uv1View = makeFloatAttributeAccessorView(
                    model,
                    uv1It->second,
                    TINYGLTF_TYPE_VEC2,
                    "TEXCOORD_1",
                    FloatAttributeComponentPolicy::PositionOrTexcoord);
                if (uv1View.has_value() && uv1View->count >= vertexCount) {
                    const tinygltf::Accessor& uv1Accessor = model.accessors[static_cast<size_t>(uv1It->second)];
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].texcoord1 = readFloatAttributeVec2(uv1Accessor, *uv1View, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: TEXCOORD_1 count is shorter than POSITION count; using default UV1s\n";
                }
            }

            const auto tangentIt = primitive.attributes.find("TANGENT");
            bool hasTangents = false;
            if (tangentIt != primitive.attributes.end()) {
                const auto tangentView = makeFloatAttributeAccessorView(
                    model,
                    tangentIt->second,
                    TINYGLTF_TYPE_VEC4,
                    "TANGENT",
                    FloatAttributeComponentPolicy::SignedNormalizedInteger);
                if (tangentView.has_value() && tangentView->count >= vertexCount) {
                    const tinygltf::Accessor& tangentAccessor = model.accessors[static_cast<size_t>(tangentIt->second)];
                    hasTangents = true;
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].tangent = readFloatAttributeVec4(tangentAccessor, *tangentView, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: TANGENT count is shorter than POSITION count; generating tangents\n";
                }
            }

            const auto colorIt = primitive.attributes.find("COLOR_0");
            if (colorIt != primitive.attributes.end()) {
                const auto colorView = makeColorAccessorView(model, colorIt->second);
                if (colorView.has_value() && colorView->count >= vertexCount) {
                    const tinygltf::Accessor& colorAccessor = model.accessors[static_cast<size_t>(colorIt->second)];
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].color = readColorElement(colorAccessor, *colorView, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: COLOR_0 count is shorter than POSITION count; using default vertex color\n";
                }
            }

            const auto jointsIt = primitive.attributes.find("JOINTS_0");
            if (jointsIt != primitive.attributes.end()) {
                const auto jointsView = makeJointAccessorView(model, jointsIt->second);
                if (jointsView.has_value() && jointsView->count >= vertexCount) {
                    const tinygltf::Accessor& jointsAccessor = model.accessors[static_cast<size_t>(jointsIt->second)];
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].joints = readJointElement(jointsAccessor, *jointsView, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: JOINTS_0 count is shorter than POSITION count; using default joints\n";
                }
            }

            const auto weightsIt = primitive.attributes.find("WEIGHTS_0");
            if (weightsIt != primitive.attributes.end()) {
                const auto weightsView = makeWeightAccessorView(model, weightsIt->second);
                if (weightsView.has_value() && weightsView->count >= vertexCount) {
                    const tinygltf::Accessor& weightsAccessor = model.accessors[static_cast<size_t>(weightsIt->second)];
                    for (size_t i = 0; i < vertexCount; ++i) {
                        mesh.vertices[firstVertex + i].weights = readWeightElement(weightsAccessor, *weightsView, i);
                    }
                } else {
                    std::cerr << "glTF accessor warning: WEIGHTS_0 count is shorter than POSITION count; using default weights\n";
                }
            }
            mesh.indices.insert(mesh.indices.end(), decodedIndices.begin(), decodedIndices.end());

            MeshPrimitiveAsset prim;
            prim.firstVertex = firstVertex;
            prim.vertexCount = static_cast<uint32_t>(vertexCount);
            prim.firstIndex = firstIndex;
            prim.indexCount = static_cast<uint32_t>(mesh.indices.size()) - firstIndex;
            prim.material = primitive.material >= 0 && static_cast<size_t>(primitive.material) < materialHandles.size()
                ? materialHandles[static_cast<size_t>(primitive.material)]
                : materialHandles.front();
            if (!primitive.targets.empty()) {
                prim.morphTargets.reserve(primitive.targets.size());
                for (size_t targetIndex = 0; targetIndex < primitive.targets.size(); ++targetIndex) {
                    MeshPrimitiveAsset::MorphTarget target;
                    target.name = targetIndex < morphTargetNames.size() && !morphTargetNames[targetIndex].empty()
                        ? morphTargetNames[targetIndex]
                        : "Target_" + std::to_string(targetIndex);
                    target.positionDeltas = readMorphTargetDeltas(model, primitive.targets[targetIndex], "POSITION", vertexCount);
                    target.normalDeltas = readMorphTargetDeltas(model, primitive.targets[targetIndex], "NORMAL", vertexCount);
                    target.tangentDeltas = readMorphTargetDeltas(model, primitive.targets[targetIndex], "TANGENT", vertexCount);
                    if (target.positionDeltas.empty() && target.normalDeltas.empty() && target.tangentDeltas.empty()) {
                        std::cerr << "glTF primitive warning: morph target " << targetIndex
                                  << " has no supported POSITION/NORMAL/TANGENT delta attributes; skipping target\n";
                        continue;
                    }
                    prim.morphTargets.push_back(std::move(target));
                }
            }
            if (const auto variantsExtIt = primitive.extensions.find("KHR_materials_variants");
                variantsExtIt != primitive.extensions.end() && variantsExtIt->second.IsObject()) {
                const auto& extObject = variantsExtIt->second.Get<tinygltf::Value::Object>();
                const auto mappingsIt = extObject.find("mappings");
                if (mappingsIt != extObject.end() && mappingsIt->second.IsArray()) {
                    const auto& mappings = mappingsIt->second.Get<tinygltf::Value::Array>();
                    for (const tinygltf::Value& mappingValue : mappings) {
                        if (!mappingValue.IsObject()) {
                            continue;
                        }
                        const auto& mapping = mappingValue.Get<tinygltf::Value::Object>();
                        const auto materialIt = mapping.find("material");
                        const auto variantsIt = mapping.find("variants");
                        if (materialIt == mapping.end() || !materialIt->second.IsInt() ||
                            variantsIt == mapping.end() || !variantsIt->second.IsArray()) {
                            continue;
                        }
                        const int materialIndex = materialIt->second.Get<int>();
                        if (materialIndex < 0 || static_cast<size_t>(materialIndex) >= materialHandles.size()) {
                            continue;
                        }
                        const auto& variants = variantsIt->second.Get<tinygltf::Value::Array>();
                        for (const tinygltf::Value& variantValue : variants) {
                            if (!variantValue.IsInt()) {
                                continue;
                            }
                            const int variantIndex = variantValue.Get<int>();
                            if (variantIndex < 0) {
                                continue;
                            }
                            const uint32_t variantIndexU = static_cast<uint32_t>(variantIndex);
                            prim.materialVariants.push_back(MeshPrimitiveAsset::MaterialVariant{
                                .variantIndex = variantIndexU,
                                .variantName = variantIndexU < scene.materialVariants.size()
                                    ? scene.materialVariants[variantIndexU]
                                    : "Variant " + std::to_string(variantIndexU),
                                .material = materialHandles[static_cast<size_t>(materialIndex)],
                            });
                        }
                    }
                }
            }
            updatePrimitiveAlphaClassification(prim, assets_.material(prim.material));
            finalizePrimitiveVertexFrames(mesh, firstVertex, prim.vertexCount, firstIndex, prim.indexCount, hasNormals, hasTangents, hasTexcoords);
            mesh.primitives.push_back(prim);
        }

        meshHandles.push_back(assets_.addMesh(std::move(mesh)));
    }
    scene.meshes = meshHandles;

    scene.nodes.reserve(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const tinygltf::Node& sourceNode = model.nodes[i];
        SceneNodeAsset node;
        node.name = sourceNode.name;
        node.transform = nodeTransform(sourceNode);
        node.morphWeights = morphWeightsFromGltf(sourceNode.weights);
        if (sourceNode.mesh >= 0 && static_cast<size_t>(sourceNode.mesh) < meshHandles.size()) {
            node.mesh = meshHandles[static_cast<size_t>(sourceNode.mesh)];
        }
        if (sourceNode.skin >= 0 && static_cast<size_t>(sourceNode.skin) < model.skins.size()) {
            node.skinIndex = sourceNode.skin;
        }
        if (sourceNode.camera >= 0 && static_cast<size_t>(sourceNode.camera) < model.cameras.size()) {
            const tinygltf::Camera& sourceCamera = model.cameras[static_cast<size_t>(sourceNode.camera)];
            node.hasCamera = true;
            if (sourceCamera.type == "perspective") {
                node.cameraProjection = 0u;
                node.cameraYfov = static_cast<float>(sourceCamera.perspective.yfov);
                node.cameraAspectRatio = sourceCamera.perspective.aspectRatio > 0.0
                    ? static_cast<float>(sourceCamera.perspective.aspectRatio)
                    : 0.0f;
                node.cameraNear = static_cast<float>(sourceCamera.perspective.znear);
                node.cameraFar = sourceCamera.perspective.zfar > 0.0
                    ? static_cast<float>(sourceCamera.perspective.zfar)
                    : 1000.0f;
            } else if (sourceCamera.type == "orthographic") {
                node.cameraProjection = 1u;
                node.cameraOrthoXmag = sourceCamera.orthographic.xmag > 0.0
                    ? static_cast<float>(sourceCamera.orthographic.xmag)
                    : 1.0f;
                node.cameraOrthoYmag = sourceCamera.orthographic.ymag > 0.0
                    ? static_cast<float>(sourceCamera.orthographic.ymag)
                    : 1.0f;
                node.cameraNear = static_cast<float>(sourceCamera.orthographic.znear);
                node.cameraFar = sourceCamera.orthographic.zfar > 0.0
                    ? static_cast<float>(sourceCamera.orthographic.zfar)
                    : 1000.0f;
            } else {
                std::cerr << "glTF camera warning: camera '" << sourceCamera.name
                          << "' has unsupported type '" << sourceCamera.type << "'; preserving default perspective metadata\n";
            }
        }
        for (int child : sourceNode.children) {
            node.children.push_back(static_cast<uint32_t>(child));
        }
        scene.nodes.push_back(std::move(node));
    }
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        for (uint32_t child : scene.nodes[i].children) {
            if (child < scene.nodes.size()) {
                scene.nodes[child].parent = static_cast<int32_t>(i);
            }
        }
    }

    scene.skins.reserve(model.skins.size());
    for (const tinygltf::Skin& sourceSkin : model.skins) {
        SceneSkinAsset skin;
        skin.name = sourceSkin.name;
        skin.skeletonRoot = sourceSkin.skeleton >= 0 && static_cast<size_t>(sourceSkin.skeleton) < scene.nodes.size()
            ? sourceSkin.skeleton
            : -1;
        skin.joints.reserve(sourceSkin.joints.size());
        for (int jointNode : sourceSkin.joints) {
            if (jointNode >= 0 && static_cast<size_t>(jointNode) < scene.nodes.size()) {
                skin.joints.push_back(static_cast<uint32_t>(jointNode));
            }
        }
        skin.inverseBindMatrices = inverseBindMatricesFromGltf(model, sourceSkin.inverseBindMatrices, skin.joints.size());
        scene.skins.push_back(std::move(skin));
    }

    std::vector<glm::mat4> nodeWorldTransforms(scene.nodes.size(), glm::mat4{1.0f});
    std::vector<uint8_t> nodeWorldComputed(scene.nodes.size(), 0u);
    auto nodeWorldTransform = [&](auto&& self, size_t nodeIndex) -> glm::mat4 {
        if (nodeIndex >= scene.nodes.size()) {
            return glm::mat4{1.0f};
        }
        if (nodeWorldComputed[nodeIndex] != 0u) {
            return nodeWorldTransforms[nodeIndex];
        }

        const SceneNodeAsset& node = scene.nodes[nodeIndex];
        glm::mat4 world = node.transform;
        if (node.parent >= 0 && static_cast<size_t>(node.parent) < scene.nodes.size()) {
            world = self(self, static_cast<size_t>(node.parent)) * node.transform;
        }
        nodeWorldTransforms[nodeIndex] = world;
        nodeWorldComputed[nodeIndex] = 1u;
        return world;
    };
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        nodeWorldTransform(nodeWorldTransform, i);
    }

    const bool usesPunctualLights = hasGltfExtensionUsed(model, "KHR_lights_punctual");
    const PunctualLightJsonFallback punctualLightJsonFallback = usesPunctualLights
        ? punctualLightFallbackFromGltfJson(path, model.nodes.size())
        : PunctualLightJsonFallback{};
    const std::vector<tinygltf::Light>& punctualLights = !model.lights.empty()
        ? model.lights
        : punctualLightJsonFallback.lights;

    auto nodePunctualLightIndex = [&](const tinygltf::Node& node, size_t nodeIndex) -> int {
        if (node.light >= 0) {
            return node.light;
        }
        const auto extIt = node.extensions.find("KHR_lights_punctual");
        if (extIt != node.extensions.end() && extIt->second.IsObject()) {
            const auto& object = extIt->second.Get<tinygltf::Value::Object>();
            const auto lightIt = object.find("light");
            if (lightIt != object.end() && lightIt->second.IsInt()) {
                return lightIt->second.Get<int>();
            }
        }
        return nodeIndex < punctualLightJsonFallback.nodeLightIndices.size()
            ? punctualLightJsonFallback.nodeLightIndices[nodeIndex]
            : -1;
    };
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        const tinygltf::Node& sourceNode = model.nodes[ni];
        const int lightIndex = nodePunctualLightIndex(sourceNode, ni);
        if (lightIndex < 0 || static_cast<size_t>(lightIndex) >= punctualLights.size()) {
            continue;
        }

        const tinygltf::Light& srcLight = punctualLights[static_cast<size_t>(lightIndex)];
        SceneLightAsset light;
        light.color = srcLight.color.size() >= 3
            ? glm::vec3(static_cast<float>(srcLight.color[0]),
                         static_cast<float>(srcLight.color[1]),
                         static_cast<float>(srcLight.color[2]))
            : glm::vec3(1.0f);
        light.intensity = static_cast<float>(srcLight.intensity);
        light.type = srcLight.type == "directional" ? static_cast<uint32_t>(LightType::Directional)
                   : srcLight.type == "spot" ? static_cast<uint32_t>(LightType::Spot)
                   : static_cast<uint32_t>(LightType::Point);
        light.enabled = true;
        if (srcLight.type == "directional") {
            light.sizeOrRadius = 0.0f;
        } else if (srcLight.range > 0.0) {
            light.sizeOrRadius = static_cast<float>(srcLight.range);
        }
        if (srcLight.type == "spot") {
            light.innerConeRadians = static_cast<float>(std::max(srcLight.spot.innerConeAngle, 0.0));
            light.outerConeRadians = static_cast<float>(std::max(srcLight.spot.outerConeAngle, srcLight.spot.innerConeAngle));
        }
        light.transform = ni < nodeWorldTransforms.size() ? nodeWorldTransforms[ni] : nodeTransform(sourceNode);
        light.nodeIndex = static_cast<int32_t>(ni);
        scene.lights.push_back(light);
    }
    if (usesPunctualLights && scene.lights.empty()) {
        const size_t jsonNodeLightRefCount = std::count_if(
            punctualLightJsonFallback.nodeLightIndices.begin(),
            punctualLightJsonFallback.nodeLightIndices.end(),
            [](int lightIndex) { return lightIndex >= 0; });
        const size_t tinyNodeLightRefCount = std::count_if(
            model.nodes.begin(),
            model.nodes.end(),
            [](const tinygltf::Node& node) { return node.light >= 0; });
        std::cerr << "glTF light warning: KHR_lights_punctual was declared but no scene lights were created"
                  << " tiny_lights=" << model.lights.size()
                  << " tiny_node_refs=" << tinyNodeLightRefCount
                  << " json_lights=" << punctualLightJsonFallback.lights.size()
                  << " json_node_refs=" << jsonNodeLightRefCount
                  << " nodes=" << model.nodes.size() << "\n";
    }

    const int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (sceneIndex >= 0 && static_cast<size_t>(sceneIndex) < model.scenes.size()) {
        for (int root : model.scenes[static_cast<size_t>(sceneIndex)].nodes) {
            scene.rootNodes.push_back(static_cast<uint32_t>(root));
        }
    }
    return scene;
}

SceneAsset GltfLoader::loadWithCache(const std::filesystem::path& path) {
    const std::filesystem::path cachePath = SceneCache::cachePathFor(path);

    if (useCache_ && SceneCache::isCacheValid(path, cachePath)) {
        const auto cacheReadStart = std::chrono::high_resolution_clock::now();
        auto cached = SceneCache::load(cachePath);
        const auto cacheReadEnd = std::chrono::high_resolution_clock::now();
        const double cacheReadMs = std::chrono::duration<double, std::milli>(cacheReadEnd - cacheReadStart).count();
        if (cached.has_value()) {
            uint64_t actualGltfMtime = SceneCache::fileMtime(path);
            if (cached->sourceMtime != actualGltfMtime) {
                cached.reset();
            }
            if (cached.has_value() && !dependenciesValid(*cached)) {
                cached.reset();
            }

            if (cached.has_value()) {
                const auto rebuildStart = std::chrono::high_resolution_clock::now();

                SceneAsset scene;
                scene.name = cached->name;
                scene.sourcePath = path;

                for (const auto& cachedTex : cached->textures) {
                    TextureAsset texture;
                    texture.name = cachedTex.name;
                    texture.sourcePath = cachedTex.sourcePath.empty() ? path : std::filesystem::path(cachedTex.sourcePath);
                    texture.width = cachedTex.width;
                    texture.height = cachedTex.height;
                    texture.channels = cachedTex.channels;
                    texture.mipLevels = cachedTex.mipLevels;
                    texture.srgb = cachedTex.srgb;
                    texture.fallback = cachedTex.fallback;
                    texture.isCompressed = cachedTex.isCompressed;
                    texture.linearColorSpace = cachedTex.linearColorSpace;
                    texture.format = static_cast<VkFormat>(cachedTex.format);
                    texture.compressedFormat = static_cast<VkFormat>(cachedTex.compressedFormat);
                    texture.rgba8 = cachedTex.rgba8;
                    texture.mipData = cachedTex.mipData;
                    texture.sampler.minFilter = static_cast<TextureFilter>(cachedTex.minFilter);
                    texture.sampler.magFilter = static_cast<TextureFilter>(cachedTex.magFilter);
                    texture.sampler.wrapS = static_cast<TextureWrap>(cachedTex.wrapS);
                    texture.sampler.wrapT = static_cast<TextureWrap>(cachedTex.wrapT);
                    scene.textures.push_back(assets_.addTexture(std::move(texture)));
                }

                auto textureHandleFor = [&](int32_t index) -> TextureAssetHandle {
                    if (index < 0 || static_cast<size_t>(index) >= scene.textures.size()) {
                        return TextureAssetHandle{};
                    }
                    return scene.textures[static_cast<size_t>(index)];
                };

                for (const auto& cachedMat : cached->materials) {
                    MaterialAsset material;
                    material.name = cachedMat.name;
                    material.baseColorFactor = cachedMat.baseColorFactor;
                    material.emissiveFactor = cachedMat.emissiveFactor;
                    material.metallicFactor = cachedMat.metallicFactor;
                    material.roughnessFactor = cachedMat.roughnessFactor;
                    material.iorFactor = cachedMat.iorFactor;
                    material.alphaCutoff = cachedMat.alphaCutoff;
                    material.alphaMode = cachedMat.alphaMode;
                    material.doubleSided = cachedMat.doubleSided;
                    material.hasIor = cachedMat.hasIor;
                    material.hasClearcoat = cachedMat.hasClearcoat;
                    material.clearcoatFactor = cachedMat.clearcoatFactor;
                    material.clearcoatRoughnessFactor = cachedMat.clearcoatRoughnessFactor;
                    material.hasTransmission = cachedMat.hasTransmission;
                    material.transmissionFactor = cachedMat.transmissionFactor;
                    material.hasVolume = cachedMat.hasVolume;
                    material.volumeThicknessFactor = cachedMat.volumeThicknessFactor;
                    material.volumeAttenuationDistance = cachedMat.volumeAttenuationDistance;
                    material.volumeAttenuationColor = cachedMat.volumeAttenuationColor;
                    material.hasDispersion = cachedMat.hasDispersion;
                    material.dispersionFactor = cachedMat.dispersionFactor;
                    material.hasSpecular = cachedMat.hasSpecular;
                    material.specularFactor = cachedMat.specularFactor;
                    material.specularColorFactor = cachedMat.specularColorFactor;
                    material.hasSheen = cachedMat.hasSheen;
                    material.sheenColorFactor = cachedMat.sheenColorFactor;
                    material.sheenRoughnessFactor = cachedMat.sheenRoughnessFactor;
                    material.hasIridescence = cachedMat.hasIridescence;
                    material.iridescenceFactor = cachedMat.iridescenceFactor;
                    material.iridescenceIor = cachedMat.iridescenceIor;
                    material.iridescenceThicknessMinimum = cachedMat.iridescenceThicknessMinimum;
                    material.iridescenceThicknessMaximum = cachedMat.iridescenceThicknessMaximum;
                    material.hasEmissiveStrength = cachedMat.hasEmissiveStrength;
                    material.emissiveStrength = cachedMat.emissiveStrength;
                    material.hasAnisotropy = cachedMat.hasAnisotropy;
                    material.anisotropyStrength = cachedMat.anisotropyStrength;
                    material.anisotropyRotation = cachedMat.anisotropyRotation;
                    material.occlusionStrength = cachedMat.occlusionStrength;
                    material.useConductorOptics = cachedMat.useConductorOptics;
                    material.conductorEta = cachedMat.conductorEta;
                    material.conductorK = cachedMat.conductorK;
                    material.baseColorTexture = textureHandleFor(cachedMat.baseColorTextureIndex);
                    material.normalTexture = textureHandleFor(cachedMat.normalTextureIndex);
                    material.metallicRoughnessTexture = textureHandleFor(cachedMat.metallicRoughnessTextureIndex);
                    material.emissiveTexture = textureHandleFor(cachedMat.emissiveTextureIndex);
                    material.clearcoatTexture = textureHandleFor(cachedMat.clearcoatTextureIndex);
                    material.clearcoatRoughnessTexture = textureHandleFor(cachedMat.clearcoatRoughnessTextureIndex);
                    material.clearcoatNormalTexture = textureHandleFor(cachedMat.clearcoatNormalTextureIndex);
                    material.transmissionTexture = textureHandleFor(cachedMat.transmissionTextureIndex);
                    material.volumeThicknessTexture = textureHandleFor(cachedMat.volumeThicknessTextureIndex);
                    material.specularTexture = textureHandleFor(cachedMat.specularTextureIndex);
                    material.specularColorTexture = textureHandleFor(cachedMat.specularColorTextureIndex);
                    material.sheenColorTexture = textureHandleFor(cachedMat.sheenColorTextureIndex);
                    material.sheenRoughnessTexture = textureHandleFor(cachedMat.sheenRoughnessTextureIndex);
                    material.iridescenceTexture = textureHandleFor(cachedMat.iridescenceTextureIndex);
                    material.iridescenceThicknessTexture = textureHandleFor(cachedMat.iridescenceThicknessTextureIndex);
                    material.anisotropyTexture = textureHandleFor(cachedMat.anisotropyTextureIndex);
                    material.occlusionTexture = textureHandleFor(cachedMat.occlusionTextureIndex);
                    material.opacityTexture = textureHandleFor(cachedMat.opacityTextureIndex);
                    material.heightTexture = textureHandleFor(cachedMat.heightTextureIndex);
                    material.heightScale = cachedMat.heightScale;
                    material.baseColorTextureTransform = cachedMat.baseColorTextureTransform;
                    material.metallicRoughnessTextureTransform = cachedMat.metallicRoughnessTextureTransform;
                    material.normalTextureTransform = cachedMat.normalTextureTransform;
                    material.emissiveTextureTransform = cachedMat.emissiveTextureTransform;
                    material.occlusionTextureTransform = cachedMat.occlusionTextureTransform;
                    material.clearcoatTextureTransform = cachedMat.clearcoatTextureTransform;
                    material.clearcoatRoughnessTextureTransform = cachedMat.clearcoatRoughnessTextureTransform;
                    material.clearcoatNormalTextureTransform = cachedMat.clearcoatNormalTextureTransform;
                    material.transmissionTextureTransform = cachedMat.transmissionTextureTransform;
                    material.volumeThicknessTextureTransform = cachedMat.volumeThicknessTextureTransform;
                    material.specularTextureTransform = cachedMat.specularTextureTransform;
                    material.specularColorTextureTransform = cachedMat.specularColorTextureTransform;
                    material.sheenColorTextureTransform = cachedMat.sheenColorTextureTransform;
                    material.sheenRoughnessTextureTransform = cachedMat.sheenRoughnessTextureTransform;
                    material.iridescenceTextureTransform = cachedMat.iridescenceTextureTransform;
                    material.iridescenceThicknessTextureTransform = cachedMat.iridescenceThicknessTextureTransform;
                    material.anisotropyTextureTransform = cachedMat.anisotropyTextureTransform;
                    material.materialWorkflow = cachedMat.materialWorkflow;
                    material.normalMapConvention = cachedMat.normalMapConvention;
                    material.specularTextureAlphaMode = cachedMat.specularTextureAlphaMode;
                    material.shaderCompatibilityMask = cachedMat.shaderCompatibilityMask;
                    scene.materials.push_back(assets_.addMaterial(std::move(material)));
                }
                scene.materialVariants = cached->materialVariants;

                for (const auto& cachedMesh : cached->meshes) {
                    MeshAsset mesh;
                    mesh.name = cachedMesh.name;
                    mesh.vertices = cachedMesh.vertices;
                    mesh.indices = cachedMesh.indices;
                    mesh.defaultMorphWeights = cachedMesh.defaultMorphWeights;
                    for (const auto& cachedPrim : cachedMesh.primitives) {
                        MeshPrimitiveAsset prim;
                        prim.firstVertex = cachedPrim.firstVertex;
                        prim.vertexCount = cachedPrim.vertexCount;
                        prim.firstIndex = cachedPrim.firstIndex;
                        prim.indexCount = cachedPrim.indexCount;
                        prim.morphTargets = cachedPrim.morphTargets;
                        if (cachedPrim.materialIndex >= 0 && static_cast<size_t>(cachedPrim.materialIndex) < scene.materials.size()) {
                            prim.material = scene.materials[static_cast<size_t>(cachedPrim.materialIndex)];
                        } else if (!scene.materials.empty()) {
                            prim.material = scene.materials.front();
                        }
                        for (const auto& cachedVariant : cachedPrim.materialVariants) {
                            if (cachedVariant.materialIndex >= 0 && static_cast<size_t>(cachedVariant.materialIndex) < scene.materials.size()) {
                                prim.materialVariants.push_back(MeshPrimitiveAsset::MaterialVariant{
                                    .variantIndex = cachedVariant.variantIndex,
                                    .variantName = cachedVariant.variantName,
                                    .material = scene.materials[static_cast<size_t>(cachedVariant.materialIndex)],
                                });
                            }
                        }
                        updatePrimitiveAlphaClassification(prim, assets_.material(prim.material));
                        mesh.primitives.push_back(prim);
                    }
                    scene.meshes.push_back(assets_.addMesh(std::move(mesh)));
                }

                for (uint32_t i = 0; i < cached->nodes.size(); ++i) {
                    const CachedNodeData& cachedNode = cached->nodes[i];
                    SceneNodeAsset node;
                    node.name = cachedNode.name;
                    node.transform = cachedNode.transform;
                    node.morphWeights = cachedNode.morphWeights;
                    node.skinIndex = cachedNode.skinIndex;
                    node.parent = cachedNode.parentIndex;
                    node.children = cachedNode.children;
                    node.hasCamera = cachedNode.hasCamera != 0;
                    node.cameraProjection = cachedNode.cameraProjection;
                    node.cameraYfov = cachedNode.cameraYfov;
                    node.cameraAspectRatio = cachedNode.cameraAspectRatio;
                    node.cameraOrthoXmag = cachedNode.cameraOrthoXmag;
                    node.cameraOrthoYmag = cachedNode.cameraOrthoYmag;
                    node.cameraNear = cachedNode.cameraNear;
                    node.cameraFar = cachedNode.cameraFar;
                    if (cachedNode.meshIndex >= 0 && static_cast<size_t>(cachedNode.meshIndex) < scene.meshes.size()) {
                        node.mesh = scene.meshes[static_cast<size_t>(cachedNode.meshIndex)];
                    }
                    scene.nodes.push_back(std::move(node));
                }
                scene.skins.reserve(cached->skins.size());
                for (const CachedSkinData& cachedSkin : cached->skins) {
                    scene.skins.push_back(SceneSkinAsset{
                        .name = cachedSkin.name,
                        .skeletonRoot = cachedSkin.skeletonRoot,
                        .joints = cachedSkin.joints,
                        .inverseBindMatrices = cachedSkin.inverseBindMatrices,
                    });
                }
                scene.rootNodes = cached->rootNodes;
                for (const CachedSceneLightData& cachedLight : cached->sceneLights) {
                    SceneLightAsset light;
                    light.type = cachedLight.type;
                    light.transform = cachedLight.transform;
                    light.color = cachedLight.color;
                    light.intensity = cachedLight.intensity;
                    light.sizeOrRadius = cachedLight.sizeOrRadius;
                    light.innerConeRadians = cachedLight.innerConeRadians;
                    light.outerConeRadians = cachedLight.outerConeRadians;
                    light.enabled = cachedLight.enabled != 0;
                    light.nodeIndex = cachedLight.nodeIndex;
                    scene.lights.push_back(light);
                }

                const auto rebuildEnd = std::chrono::high_resolution_clock::now();
                const double rebuildMs = std::chrono::duration<double, std::milli>(rebuildEnd - rebuildStart).count();
                std::cout << "Scene cache hit: read_ms=" << cacheReadMs
                          << " rebuild_assets_ms=" << rebuildMs
                          << " name=" << cached->name << "\n";
                return scene;
            }
        }
    }

    const auto totalStart = std::chrono::high_resolution_clock::now();
    const auto parseStart = std::chrono::high_resolution_clock::now();
    SceneAsset scene = load(path);
    const auto parseEnd = std::chrono::high_resolution_clock::now();
    const double parseMs = std::chrono::duration<double, std::milli>(parseEnd - parseStart).count();

    if (!cacheWritesEnabled_) {
        const auto totalEnd = std::chrono::high_resolution_clock::now();
        const double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        std::cout << "glTF loaded (cache write skipped): parse_ms=" << parseMs
                  << " total_ms=" << totalMs << "\n";
        return scene;
    }

    const auto cacheBuildStart = std::chrono::high_resolution_clock::now();
    CachedScene cached = buildCachedScene(path, scene, lastExternalDependencies_);
    const auto cacheBuildEnd = std::chrono::high_resolution_clock::now();
    const double cacheBuildMs = std::chrono::duration<double, std::milli>(cacheBuildEnd - cacheBuildStart).count();
    const auto cacheSaveStart = std::chrono::high_resolution_clock::now();
    const bool cacheSaved = SceneCache::save(cachePath, cached);
    const auto cacheSaveEnd = std::chrono::high_resolution_clock::now();
    const double cacheSaveMs = std::chrono::duration<double, std::milli>(cacheSaveEnd - cacheSaveStart).count();
    if (cacheSaved) {
        std::cout << "Scene cache created: " << cachePath.string() << "\n";
    }
    const auto totalEnd = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
    std::cout << "glTF loaded (uncached): parse_ms=" << parseMs
              << " cache_build_ms=" << cacheBuildMs
              << " cache_save_ms=" << cacheSaveMs
              << " total_ms=" << totalMs << "\n";
    return scene;
}

CachedScene GltfLoader::buildCachedScene(const std::filesystem::path& path, const SceneAsset& scene, const std::vector<std::filesystem::path>& externalDependencies) {
    CachedScene cached;
    cached.name = scene.name;

    std::unordered_map<uint32_t, int32_t> textureIndexByHandle;
    textureIndexByHandle.reserve(scene.textures.size());
    for (int32_t i = 0; i < static_cast<int32_t>(scene.textures.size()); ++i) {
        textureIndexByHandle.emplace(scene.textures[static_cast<size_t>(i)].index, i);
    }

    std::unordered_map<uint32_t, int32_t> materialIndexByHandle;
    materialIndexByHandle.reserve(scene.materials.size());
    for (int32_t i = 0; i < static_cast<int32_t>(scene.materials.size()); ++i) {
        materialIndexByHandle.emplace(scene.materials[static_cast<size_t>(i)].index, i);
    }

    std::unordered_map<uint32_t, int32_t> meshIndexByHandle;
    meshIndexByHandle.reserve(scene.meshes.size());
    for (int32_t i = 0; i < static_cast<int32_t>(scene.meshes.size()); ++i) {
        meshIndexByHandle.emplace(scene.meshes[static_cast<size_t>(i)].index, i);
    }

    const auto getTextureIndex = [&](TextureAssetHandle handle) -> int32_t {
        if (!handle.valid()) {
            return -1;
        }
        const auto it = textureIndexByHandle.find(handle.index);
        return it != textureIndexByHandle.end() ? it->second : -1;
    };

    const auto getMaterialIndex = [&](MaterialAssetHandle handle) -> int32_t {
        if (!handle.valid()) {
            return -1;
        }
        const auto it = materialIndexByHandle.find(handle.index);
        return it != materialIndexByHandle.end() ? it->second : -1;
    };

    const auto getMeshIndex = [&](MeshAssetHandle handle) -> int32_t {
        if (!handle.valid()) {
            return -1;
        }
        const auto it = meshIndexByHandle.find(handle.index);
        return it != meshIndexByHandle.end() ? it->second : -1;
    };

    for (TextureAssetHandle handle : scene.textures) {
        const TextureAsset* texture = assets_.texture(handle);
        if (texture == nullptr) {
            continue;
        }
        CachedTextureData cachedTex;
        cachedTex.name = texture->name;
        cachedTex.sourcePath = texture->sourcePath.string();
        cachedTex.width = texture->width;
        cachedTex.height = texture->height;
        cachedTex.channels = texture->channels;
        cachedTex.mipLevels = texture->mipLevels;
        cachedTex.srgb = texture->srgb;
        cachedTex.fallback = texture->fallback;
        cachedTex.isCompressed = texture->isCompressed;
        cachedTex.linearColorSpace = texture->linearColorSpace;
        cachedTex.format = static_cast<uint32_t>(texture->format);
        cachedTex.compressedFormat = static_cast<uint32_t>(texture->compressedFormat);
        cachedTex.rgba8 = texture->rgba8;
        cachedTex.mipData = texture->mipData;
        cachedTex.minFilter = static_cast<uint32_t>(texture->sampler.minFilter);
        cachedTex.magFilter = static_cast<uint32_t>(texture->sampler.magFilter);
        cachedTex.wrapS = static_cast<uint32_t>(texture->sampler.wrapS);
        cachedTex.wrapT = static_cast<uint32_t>(texture->sampler.wrapT);
        cached.textures.push_back(std::move(cachedTex));
    }

    for (MaterialAssetHandle handle : scene.materials) {
        const MaterialAsset* material = assets_.material(handle);
        if (material == nullptr) {
            continue;
        }
        CachedMaterialData cachedMat;
        cachedMat.name = material->name;
        cachedMat.baseColorFactor = material->baseColorFactor;
        cachedMat.emissiveFactor = material->emissiveFactor;
        cachedMat.metallicFactor = material->metallicFactor;
        cachedMat.roughnessFactor = material->roughnessFactor;
        cachedMat.iorFactor = material->iorFactor;
        cachedMat.alphaCutoff = material->alphaCutoff;
        cachedMat.alphaMode = material->alphaMode;
        cachedMat.doubleSided = material->doubleSided;
        cachedMat.hasIor = material->hasIor;
        cachedMat.hasClearcoat = material->hasClearcoat;
        cachedMat.clearcoatFactor = material->clearcoatFactor;
        cachedMat.clearcoatRoughnessFactor = material->clearcoatRoughnessFactor;
        cachedMat.hasTransmission = material->hasTransmission;
        cachedMat.transmissionFactor = material->transmissionFactor;
        cachedMat.hasVolume = material->hasVolume;
        cachedMat.volumeThicknessFactor = material->volumeThicknessFactor;
        cachedMat.volumeAttenuationDistance = material->volumeAttenuationDistance;
        cachedMat.volumeAttenuationColor = material->volumeAttenuationColor;
        cachedMat.hasDispersion = material->hasDispersion;
        cachedMat.dispersionFactor = material->dispersionFactor;
        cachedMat.hasSpecular = material->hasSpecular;
        cachedMat.specularFactor = material->specularFactor;
        cachedMat.specularColorFactor = material->specularColorFactor;
        cachedMat.hasSheen = material->hasSheen;
        cachedMat.sheenColorFactor = material->sheenColorFactor;
        cachedMat.sheenRoughnessFactor = material->sheenRoughnessFactor;
        cachedMat.hasIridescence = material->hasIridescence;
        cachedMat.iridescenceFactor = material->iridescenceFactor;
        cachedMat.iridescenceIor = material->iridescenceIor;
        cachedMat.iridescenceThicknessMinimum = material->iridescenceThicknessMinimum;
        cachedMat.iridescenceThicknessMaximum = material->iridescenceThicknessMaximum;
        cachedMat.hasEmissiveStrength = material->hasEmissiveStrength;
        cachedMat.emissiveStrength = material->emissiveStrength;
        cachedMat.hasAnisotropy = material->hasAnisotropy;
        cachedMat.anisotropyStrength = material->anisotropyStrength;
        cachedMat.anisotropyRotation = material->anisotropyRotation;
        cachedMat.occlusionStrength = material->occlusionStrength;
        cachedMat.useConductorOptics = material->useConductorOptics;
        cachedMat.conductorEta = material->conductorEta;
        cachedMat.conductorK = material->conductorK;
        cachedMat.baseColorTextureIndex = getTextureIndex(material->baseColorTexture);
        cachedMat.normalTextureIndex = getTextureIndex(material->normalTexture);
        cachedMat.metallicRoughnessTextureIndex = getTextureIndex(material->metallicRoughnessTexture);
        cachedMat.emissiveTextureIndex = getTextureIndex(material->emissiveTexture);
        cachedMat.clearcoatTextureIndex = getTextureIndex(material->clearcoatTexture);
        cachedMat.clearcoatRoughnessTextureIndex = getTextureIndex(material->clearcoatRoughnessTexture);
        cachedMat.clearcoatNormalTextureIndex = getTextureIndex(material->clearcoatNormalTexture);
        cachedMat.transmissionTextureIndex = getTextureIndex(material->transmissionTexture);
        cachedMat.volumeThicknessTextureIndex = getTextureIndex(material->volumeThicknessTexture);
        cachedMat.specularTextureIndex = getTextureIndex(material->specularTexture);
        cachedMat.specularColorTextureIndex = getTextureIndex(material->specularColorTexture);
        cachedMat.sheenColorTextureIndex = getTextureIndex(material->sheenColorTexture);
        cachedMat.sheenRoughnessTextureIndex = getTextureIndex(material->sheenRoughnessTexture);
        cachedMat.iridescenceTextureIndex = getTextureIndex(material->iridescenceTexture);
        cachedMat.iridescenceThicknessTextureIndex = getTextureIndex(material->iridescenceThicknessTexture);
        cachedMat.anisotropyTextureIndex = getTextureIndex(material->anisotropyTexture);
        cachedMat.occlusionTextureIndex = getTextureIndex(material->occlusionTexture);
        cachedMat.opacityTextureIndex = getTextureIndex(material->opacityTexture);
        cachedMat.heightTextureIndex = getTextureIndex(material->heightTexture);
        cachedMat.heightScale = material->heightScale;
        cachedMat.baseColorTextureTransform = material->baseColorTextureTransform;
        cachedMat.metallicRoughnessTextureTransform = material->metallicRoughnessTextureTransform;
        cachedMat.normalTextureTransform = material->normalTextureTransform;
        cachedMat.emissiveTextureTransform = material->emissiveTextureTransform;
        cachedMat.occlusionTextureTransform = material->occlusionTextureTransform;
        cachedMat.clearcoatTextureTransform = material->clearcoatTextureTransform;
        cachedMat.clearcoatRoughnessTextureTransform = material->clearcoatRoughnessTextureTransform;
        cachedMat.clearcoatNormalTextureTransform = material->clearcoatNormalTextureTransform;
        cachedMat.transmissionTextureTransform = material->transmissionTextureTransform;
        cachedMat.volumeThicknessTextureTransform = material->volumeThicknessTextureTransform;
        cachedMat.specularTextureTransform = material->specularTextureTransform;
        cachedMat.specularColorTextureTransform = material->specularColorTextureTransform;
        cachedMat.sheenColorTextureTransform = material->sheenColorTextureTransform;
        cachedMat.sheenRoughnessTextureTransform = material->sheenRoughnessTextureTransform;
        cachedMat.iridescenceTextureTransform = material->iridescenceTextureTransform;
        cachedMat.iridescenceThicknessTextureTransform = material->iridescenceThicknessTextureTransform;
        cachedMat.anisotropyTextureTransform = material->anisotropyTextureTransform;
        cachedMat.materialWorkflow = material->materialWorkflow;
        cachedMat.normalMapConvention = material->normalMapConvention;
        cachedMat.specularTextureAlphaMode = material->specularTextureAlphaMode;
        cachedMat.shaderCompatibilityMask = material->shaderCompatibilityMask;
        cached.materials.push_back(std::move(cachedMat));
    }

    for (MeshAssetHandle handle : scene.meshes) {
        const MeshAsset* mesh = assets_.mesh(handle);
        if (mesh == nullptr) {
            continue;
        }
        CachedMeshData cachedMesh;
        cachedMesh.name = mesh->name;
        cachedMesh.vertices = mesh->vertices;
        cachedMesh.indices = mesh->indices;
        cachedMesh.defaultMorphWeights = mesh->defaultMorphWeights;
        for (const MeshPrimitiveAsset& prim : mesh->primitives) {
            CachedPrimitiveData cachedPrim;
            cachedPrim.firstVertex = prim.firstVertex;
            cachedPrim.vertexCount = prim.vertexCount;
            cachedPrim.firstIndex = prim.firstIndex;
            cachedPrim.indexCount = prim.indexCount;
            cachedPrim.materialIndex = getMaterialIndex(prim.material);
            cachedPrim.morphTargets = prim.morphTargets;
            for (const auto& variant : prim.materialVariants) {
                cachedPrim.materialVariants.push_back(CachedPrimitiveData::MaterialVariant{
                    .variantIndex = variant.variantIndex,
                    .variantName = variant.variantName,
                    .materialIndex = getMaterialIndex(variant.material),
                });
            }
            cachedMesh.primitives.push_back(std::move(cachedPrim));
        }
        cached.meshes.push_back(std::move(cachedMesh));
    }

    cached.materialVariants = scene.materialVariants;

    for (const SceneNodeAsset& node : scene.nodes) {
        CachedNodeData cachedNode;
        cachedNode.name = node.name;
        cachedNode.transform = node.transform;
        cachedNode.meshIndex = getMeshIndex(node.mesh);
        cachedNode.morphWeights = node.morphWeights;
        cachedNode.skinIndex = node.skinIndex;
        cachedNode.hasCamera = node.hasCamera ? 1u : 0u;
        cachedNode.cameraProjection = node.cameraProjection;
        cachedNode.cameraYfov = node.cameraYfov;
        cachedNode.cameraAspectRatio = node.cameraAspectRatio;
        cachedNode.cameraOrthoXmag = node.cameraOrthoXmag;
        cachedNode.cameraOrthoYmag = node.cameraOrthoYmag;
        cachedNode.cameraNear = node.cameraNear;
        cachedNode.cameraFar = node.cameraFar;
        cachedNode.parentIndex = node.parent;
        cachedNode.children = node.children;
        cached.nodes.push_back(std::move(cachedNode));
    }

    cached.skins.reserve(scene.skins.size());
    for (const SceneSkinAsset& skin : scene.skins) {
        cached.skins.push_back(CachedSkinData{
            .name = skin.name,
            .skeletonRoot = skin.skeletonRoot,
            .joints = skin.joints,
            .inverseBindMatrices = skin.inverseBindMatrices,
        });
    }

    for (const SceneLightAsset& light : scene.lights) {
        CachedSceneLightData cachedLight;
        cachedLight.type = light.type;
        cachedLight.transform = light.transform;
        cachedLight.color = light.color;
        cachedLight.intensity = light.intensity;
        cachedLight.sizeOrRadius = light.sizeOrRadius;
        cachedLight.innerConeRadians = light.innerConeRadians;
        cachedLight.outerConeRadians = light.outerConeRadians;
        cachedLight.enabled = light.enabled ? 1u : 0u;
        cachedLight.nodeIndex = light.nodeIndex;
        cached.sceneLights.push_back(std::move(cachedLight));
    }

    cached.rootNodes = scene.rootNodes;
    cached.sourceMtime = SceneCache::fileMtime(path);

    std::unordered_set<std::string> dependencyPaths;
    for (const std::filesystem::path& dependency : externalDependencies) {
        addDependency(cached, dependency, dependencyPaths);
    }
    for (TextureAssetHandle handle : scene.textures) {
        const TextureAsset* texture = assets_.texture(handle);
        if (texture == nullptr || texture->sourcePath.empty() || texture->sourcePath == path) {
            continue;
        }
        addDependency(cached, texture->sourcePath, dependencyPaths);
    }

    return cached;
}

} // namespace rtv
