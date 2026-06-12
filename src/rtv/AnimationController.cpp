#include "rtv/AnimationController.h"

#include "rtv/NativeBinaryIO.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

namespace rtv {
namespace {

constexpr uint32_t kRtanimControllerMetadataJsonChunk = kRtanimControllerChunkMetadataJson;

void addWarning(std::vector<std::string>* warnings, std::string message) {
    if (warnings != nullptr) {
        warnings->push_back(std::move(message));
    }
}

bool readFileBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    return file.good() || size == 0;
}

bool readJsonFile(const std::filesystem::path& path, nlohmann::json& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "could not open " + path.string();
        return false;
    }
    try {
        file >> out;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
    return true;
}

bool writeJsonFile(const std::filesystem::path& path, const nlohmann::json& json, std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "could not create output directory: " + ec.message();
            return false;
        }
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        error = "could not open output " + path.string();
        return false;
    }
    file << json.dump(2);
    return true;
}

std::string trimString(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

std::string quoteCommandPath(const std::filesystem::path& path) {
    std::string value = path.string();
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

std::string readCommandOutput(const std::string& command) {
    std::string output;
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return output;
    }
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
        if (output.size() > 32768u) {
            output.resize(32768u);
            break;
        }
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

std::optional<std::filesystem::path> findGitRoot(std::filesystem::path path) {
    std::error_code ec;
    path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        path = std::filesystem::absolute(path, ec);
    }
    if (!ec && std::filesystem::is_regular_file(path, ec)) {
        path = path.parent_path();
    }
    while (!path.empty()) {
        if (std::filesystem::exists(path / ".git", ec)) {
            return path;
        }
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return std::nullopt;
}

nlohmann::json sourceControlSnapshotJson(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::weakly_canonical(path, ec);
    const std::filesystem::path comparablePath = ec ? std::filesystem::absolute(path, ec) : absolutePath;
    const bool exists = std::filesystem::exists(comparablePath, ec);
    const auto permissions = exists ? std::filesystem::status(comparablePath, ec).permissions() : std::filesystem::perms::owner_write;
    const bool readOnly = exists && (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    nlohmann::json report = {
        {"provider", "none"},
        {"providerAvailable", false},
        {"path", comparablePath.generic_string()},
        {"pathExists", exists},
        {"readOnly", readOnly},
        {"sourceControlAware", true},
        {"providerMutationsPerformed", nlohmann::json::array()},
        {"unsupportedProviderMutations", {"checkout", "revert", "lock", "submit"}},
    };
    const std::optional<std::filesystem::path> gitRoot = findGitRoot(comparablePath);
    if (!gitRoot.has_value()) {
        report["unavailableReason"] = "Path is not inside a Git repository.";
        return report;
    }
    std::filesystem::path relative = std::filesystem::relative(comparablePath, *gitRoot, ec);
    if (ec) {
        relative = comparablePath.filename();
    }
#ifdef _WIN32
    constexpr const char* stderrRedirect = " 2>NUL";
#else
    constexpr const char* stderrRedirect = " 2>/dev/null";
#endif
    const std::string status = readCommandOutput(
        "git -C " + quoteCommandPath(*gitRoot) + " status --short -- " + quoteCommandPath(relative) + stderrRedirect);
    report["provider"] = "git";
    report["providerAvailable"] = true;
    report["repositoryRoot"] = gitRoot->generic_string();
    report["repositoryRelativePath"] = relative.generic_string();
    report["focusedStatus"] = trimString(status).empty() ? std::string("Clean") : trimString(status);
    return report;
}

bool samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    std::error_code ec;
    const std::filesystem::path a = std::filesystem::weakly_canonical(lhs, ec);
    if (ec) {
        return lhs == rhs;
    }
    const std::filesystem::path b = std::filesystem::weakly_canonical(rhs, ec);
    if (ec) {
        return lhs == rhs;
    }
    return a == b;
}

nlohmann::json* controllerRoot(nlohmann::json& graph) {
    if (!graph.is_object()) {
        return nullptr;
    }
    if (!graph.contains("controller") || !graph["controller"].is_object()) {
        return nullptr;
    }
    return &graph["controller"];
}

nlohmann::json* findNamedObject(nlohmann::json& array, std::string_view name) {
    if (!array.is_array()) {
        return nullptr;
    }
    for (nlohmann::json& item : array) {
        if (item.is_object() && item.value("name", std::string{}) == name) {
            return &item;
        }
    }
    return nullptr;
}

bool applyControllerMutationOperation(nlohmann::json& graph, const nlohmann::json& operation, std::vector<std::string>& applied, std::vector<std::string>& warnings) {
    if (!operation.is_object()) {
        warnings.push_back("Skipped non-object animation controller mutation operation.");
        return false;
    }
    nlohmann::json* controller = controllerRoot(graph);
    if (controller == nullptr) {
        warnings.push_back("Animation controller mutation failed: graph has no controller object.");
        return false;
    }
    const std::string op = operation.value("op", std::string{});
    if (op == "setParameterDefault") {
        const std::string name = operation.value("name", std::string{});
        nlohmann::json* parameter = findNamedObject((*controller)["parameters"], name);
        if (parameter == nullptr || !operation.contains("value")) {
            warnings.push_back("setParameterDefault requires an existing parameter name and value.");
            return false;
        }
        (*parameter)["default"] = operation["value"];
        applied.push_back("setParameterDefault:" + name);
        return true;
    }
    if (op == "setStateSpeed" || op == "setStateLoop" || op == "setStateClip") {
        const std::string stateName = operation.value("state", std::string{});
        nlohmann::json* state = findNamedObject((*controller)["states"], stateName);
        if (state == nullptr) {
            warnings.push_back(op + " requires an existing state name.");
            return false;
        }
        if (op == "setStateSpeed") {
            if (!operation.contains("speed") || !operation["speed"].is_number()) {
                warnings.push_back("setStateSpeed requires numeric speed.");
                return false;
            }
            (*state)["speed"] = operation["speed"];
        } else if (op == "setStateLoop") {
            if (!operation.contains("loop") || !operation["loop"].is_boolean()) {
                warnings.push_back("setStateLoop requires boolean loop.");
                return false;
            }
            (*state)["loop"] = operation["loop"];
        } else {
            if (operation.contains("clipGuid")) {
                (*state)["clipGuid"] = operation["clipGuid"];
            }
            if (operation.contains("clipPath")) {
                (*state)["clipPath"] = operation["clipPath"];
            }
        }
        applied.push_back(op + ":" + stateName);
        return true;
    }
    if (op == "setDefaultState") {
        const std::string stateName = operation.value("state", std::string{});
        nlohmann::json& states = (*controller)["states"];
        nlohmann::json* state = findNamedObject(states, stateName);
        if (state == nullptr) {
            warnings.push_back("setDefaultState requires an existing state name.");
            return false;
        }
        for (nlohmann::json& item : states) {
            if (item.is_object()) {
                item["default"] = item.value("name", std::string{}) == stateName;
            }
        }
        applied.push_back("setDefaultState:" + stateName);
        return true;
    }
    if (op == "addState") {
        if (!operation.contains("state") || !operation["state"].is_object()) {
            warnings.push_back("addState requires a state object.");
            return false;
        }
        nlohmann::json state = operation["state"];
        const std::string stateName = state.value("name", std::string{});
        if (stateName.empty() || findNamedObject((*controller)["states"], stateName) != nullptr) {
            warnings.push_back("addState requires a unique non-empty state name.");
            return false;
        }
        (*controller)["states"].push_back(std::move(state));
        applied.push_back("addState:" + stateName);
        return true;
    }
    if (op == "addTransition") {
        const std::string from = operation.value("from", std::string{});
        nlohmann::json* state = findNamedObject((*controller)["states"], from);
        if (state == nullptr || !operation.contains("transition") || !operation["transition"].is_object()) {
            warnings.push_back("addTransition requires an existing from state and transition object.");
            return false;
        }
        if (!state->contains("transitions") || !(*state)["transitions"].is_array()) {
            (*state)["transitions"] = nlohmann::json::array();
        }
        (*state)["transitions"].push_back(operation["transition"]);
        applied.push_back("addTransition:" + from + "->" + operation["transition"].value("to", std::string{}));
        return true;
    }
    warnings.push_back("Unsupported animation controller mutation op: " + op);
    return false;
}

nlohmann::json normalizedControllerMutationJson(const nlohmann::json& root) {
    if (root.is_object() && root.contains("operations") && root["operations"].is_array()) {
        return root["operations"];
    }
    if (root.is_array()) {
        return root;
    }
    return nlohmann::json::array({root});
}

bool chunkRangeInside(const NativeChunkRecord& chunk, size_t fileSize) {
    return chunk.offset <= fileSize && chunk.size <= fileSize - chunk.offset;
}

const NativeChunkRecord* findChunk(const NativeAssetInspection& inspection, uint32_t type) {
    const auto it = std::find_if(inspection.chunks.begin(), inspection.chunks.end(), [&](const NativeChunkRecord& chunk) {
        return chunk.type == type;
    });
    return it != inspection.chunks.end() ? &*it : nullptr;
}

template <typename T>
bool readSingleChunk(
    const std::vector<std::byte>& bytes,
    const NativeAssetInspection& inspection,
    uint32_t type,
    T& out,
    std::vector<std::string>* warnings,
    const char* label) {
    static_assert(std::is_trivially_copyable_v<T>);
    const NativeChunkRecord* chunk = findChunk(inspection, type);
    if (chunk == nullptr) {
        addWarning(warnings, std::string("Animation controller native compact load failed: missing ") + label + " chunk.");
        return false;
    }
    if (!chunkRangeInside(*chunk, bytes.size()) || chunk->size != sizeof(T)) {
        addWarning(warnings, std::string("Animation controller native compact load failed: invalid ") + label + " chunk size.");
        return false;
    }
    std::memcpy(&out, bytes.data() + chunk->offset, sizeof(T));
    return true;
}

template <typename T>
bool readRecordChunk(
    const std::vector<std::byte>& bytes,
    const NativeAssetInspection& inspection,
    uint32_t type,
    std::vector<T>& out,
    std::vector<std::string>* warnings,
    const char* label,
    bool required) {
    static_assert(std::is_trivially_copyable_v<T>);
    const NativeChunkRecord* chunk = findChunk(inspection, type);
    if (chunk == nullptr) {
        if (required) {
            addWarning(warnings, std::string("Animation controller native compact load failed: missing ") + label + " chunk.");
            return false;
        }
        return true;
    }
    if (!chunkRangeInside(*chunk, bytes.size()) || chunk->size % sizeof(T) != 0u) {
        addWarning(warnings, std::string("Animation controller native compact load failed: invalid ") + label + " chunk size.");
        return false;
    }
    out.resize(static_cast<size_t>(chunk->size / sizeof(T)));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data() + chunk->offset, out.size() * sizeof(T));
    }
    return true;
}

bool readStringTable(
    const std::vector<std::byte>& bytes,
    const NativeAssetInspection& inspection,
    std::string& out,
    std::vector<std::string>* warnings) {
    const NativeChunkRecord* chunk = findChunk(inspection, kRtanimControllerChunkStringTable);
    if (chunk == nullptr) {
        addWarning(warnings, "Animation controller native compact load failed: missing string table chunk.");
        return false;
    }
    if (!chunkRangeInside(*chunk, bytes.size())) {
        addWarning(warnings, "Animation controller native compact load failed: invalid string table chunk bounds.");
        return false;
    }
    out.resize(static_cast<size_t>(chunk->size));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data() + chunk->offset, out.size());
    }
    return true;
}

std::string stringFromTable(const std::string& table, uint32_t offset, uint32_t size) {
    if (size == 0u || offset > table.size() || size > table.size() - offset) {
        return {};
    }
    return table.substr(offset, size);
}

bool zeroGuid(const std::array<uint8_t, 16>& guid) {
    return std::all_of(guid.begin(), guid.end(), [](uint8_t value) { return value == 0u; });
}

std::string guidStringOrEmpty(const std::array<uint8_t, 16>& guid) {
    return zeroGuid(guid) ? std::string{} : nativeGuidToText(guid);
}

AnimationControllerParameterValue parameterValueFromJson(AnimationControllerParameterType type, const nlohmann::json& json) {
    AnimationControllerParameterValue value;
    value.type = type;
    switch (type) {
    case AnimationControllerParameterType::Bool:
        value.boolValue = json.is_boolean() ? json.get<bool>() : false;
        break;
    case AnimationControllerParameterType::Int:
        value.intValue = json.is_number_integer() ? json.get<int>() : 0;
        break;
    case AnimationControllerParameterType::Float:
        value.floatValue = json.is_number() ? json.get<float>() : 0.0f;
        break;
    case AnimationControllerParameterType::Trigger:
        value.triggerValue = json.is_boolean() ? json.get<bool>() : false;
        break;
    case AnimationControllerParameterType::Unknown:
        break;
    }
    return value;
}

nlohmann::json parameterValueJson(const AnimationControllerParameterValue& value) {
    switch (value.type) {
    case AnimationControllerParameterType::Bool: return value.boolValue;
    case AnimationControllerParameterType::Int: return value.intValue;
    case AnimationControllerParameterType::Float: return value.floatValue;
    case AnimationControllerParameterType::Trigger: return value.triggerValue;
    case AnimationControllerParameterType::Unknown: return nullptr;
    }
    return nullptr;
}

const char* nativeParameterTypeName(uint32_t type) {
    switch (static_cast<AnimationControllerParameterType>(type)) {
    case AnimationControllerParameterType::Bool: return "bool";
    case AnimationControllerParameterType::Int: return "int";
    case AnimationControllerParameterType::Float: return "float";
    case AnimationControllerParameterType::Trigger: return "trigger";
    case AnimationControllerParameterType::Unknown: break;
    }
    return "unknown";
}

nlohmann::json nativeParameterValueJson(uint32_t type, uint32_t boolValue, int32_t intValue, float floatValue, uint32_t triggerValue) {
    switch (static_cast<AnimationControllerParameterType>(type)) {
    case AnimationControllerParameterType::Bool: return boolValue != 0u;
    case AnimationControllerParameterType::Int: return intValue;
    case AnimationControllerParameterType::Float: return floatValue;
    case AnimationControllerParameterType::Trigger: return triggerValue != 0u;
    case AnimationControllerParameterType::Unknown: break;
    }
    return nullptr;
}

nlohmann::json nativePayloadJson(std::string_view payload) {
    if (payload.empty()) {
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(payload.begin(), payload.end());
    } catch (...) {
        return std::string(payload);
    }
}

bool rangeInsideVector(size_t first, size_t count, size_t size) {
    return first <= size && count <= size - first;
}

std::optional<nlohmann::json> compactControllerPayloadJson(
    const std::filesystem::path& path,
    const std::vector<std::byte>& bytes,
    const NativeAssetInspection& inspection,
    std::vector<std::string>* warnings) {
    if (findChunk(inspection, kRtanimControllerChunkPayloadHeader) == nullptr) {
        return std::nullopt;
    }

    RtanimControllerPayloadHeader header;
    if (!readSingleChunk(bytes, inspection, kRtanimControllerChunkPayloadHeader, header, warnings, "payload header")) {
        return nlohmann::json{};
    }
    if (header.version != kRtanimControllerPayloadVersion) {
        addWarning(warnings, "Animation controller native compact load failed: unsupported payload version in " + path.string());
        return nlohmann::json{};
    }

    std::string stringTable;
    if (!readStringTable(bytes, inspection, stringTable, warnings)) {
        return nlohmann::json{};
    }

    std::vector<RtanimControllerParameterRecord> parameters;
    std::vector<RtanimControllerStateRecord> states;
    std::vector<RtanimControllerTransitionRecord> transitions;
    std::vector<RtanimControllerConditionRecord> conditions;
    std::vector<RtanimControllerEventRecord> events;
    std::vector<RtanimControllerBlendTreeRecord> blendTrees;
    std::vector<RtanimControllerBlendTreeChildRecord> blendTreeChildren;
    std::vector<RtanimControllerLayerRecord> layers;
    std::vector<RtanimControllerAvatarMaskRecord> avatarMasks;
    std::vector<RtanimControllerAvatarMaskJointRecord> avatarMaskJoints;

    if (!readRecordChunk(bytes, inspection, kRtanimControllerChunkParameters, parameters, warnings, "parameters", header.parameterCount > 0u) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkStates, states, warnings, "states", header.stateCount > 0u) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkTransitions, transitions, warnings, "transitions", header.transitionCount > 0u) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkConditions, conditions, warnings, "conditions", false) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkEvents, events, warnings, "events", false) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkBlendTrees, blendTrees, warnings, "blend trees", false) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkBlendTreeChildren, blendTreeChildren, warnings, "blend tree children", false) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkLayers, layers, warnings, "layers", header.layerCount > 0u) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkAvatarMasks, avatarMasks, warnings, "avatar masks", false) ||
        !readRecordChunk(bytes, inspection, kRtanimControllerChunkAvatarMaskJoints, avatarMaskJoints, warnings, "avatar mask joints", false)) {
        return nlohmann::json{};
    }

    if (parameters.size() != header.parameterCount || states.size() != header.stateCount ||
        transitions.size() != header.transitionCount || layers.size() != header.layerCount) {
        addWarning(warnings, "Animation controller native compact load failed: payload header counts do not match record chunks in " + path.string());
        return nlohmann::json{};
    }

    auto eventArray = [&](uint32_t first, uint32_t count) {
        nlohmann::json out = nlohmann::json::array();
        if (!rangeInsideVector(first, count, events.size())) {
            return out;
        }
        for (size_t eventIndex = first; eventIndex < static_cast<size_t>(first) + count; ++eventIndex) {
            const RtanimControllerEventRecord& event = events[eventIndex];
            out.push_back({
                {"name", stringFromTable(stringTable, event.nameOffset, event.nameSize)},
                {"payload", nativePayloadJson(stringFromTable(stringTable, event.payloadOffset, event.payloadSize))},
            });
        }
        return out;
    };

    nlohmann::json parameterJson = nlohmann::json::array();
    for (const RtanimControllerParameterRecord& parameter : parameters) {
        parameterJson.push_back({
            {"name", stringFromTable(stringTable, parameter.nameOffset, parameter.nameSize)},
            {"type", nativeParameterTypeName(parameter.type)},
            {"default", nativeParameterValueJson(parameter.type, parameter.boolValue, parameter.intValue, parameter.floatValue, parameter.triggerValue)},
        });
    }

    nlohmann::json stateJson = nlohmann::json::array();
    for (size_t stateIndex = 0; stateIndex < states.size(); ++stateIndex) {
        const RtanimControllerStateRecord& state = states[stateIndex];
        nlohmann::json transitionsJson = nlohmann::json::array();
        if (rangeInsideVector(state.firstTransition, state.transitionCount, transitions.size())) {
            for (size_t transitionIndex = state.firstTransition; transitionIndex < static_cast<size_t>(state.firstTransition) + state.transitionCount; ++transitionIndex) {
                const RtanimControllerTransitionRecord& transition = transitions[transitionIndex];
                nlohmann::json conditionsJson = nlohmann::json::array();
                if (rangeInsideVector(transition.firstCondition, transition.conditionCount, conditions.size())) {
                    for (size_t conditionIndex = transition.firstCondition; conditionIndex < static_cast<size_t>(transition.firstCondition) + transition.conditionCount; ++conditionIndex) {
                        const RtanimControllerConditionRecord& condition = conditions[conditionIndex];
                        conditionsJson.push_back({
                            {"parameter", stringFromTable(stringTable, condition.parameterOffset, condition.parameterSize)},
                            {"type", nativeParameterTypeName(condition.type)},
                            {"op", stringFromTable(stringTable, condition.opOffset, condition.opSize)},
                            {"value", nativeParameterValueJson(condition.type, condition.boolValue, condition.intValue, condition.floatValue, condition.triggerValue)},
                        });
                    }
                }
                transitionsJson.push_back({
                    {"to", stringFromTable(stringTable, transition.toOffset, transition.toSize)},
                    {"exitTimeSeconds", transition.exitTimeSeconds},
                    {"conditions", conditionsJson},
                    {"events", eventArray(transition.firstEvent, transition.eventCount)},
                });
            }
        }

        nlohmann::json blendTreeJson = nullptr;
        if (state.blendTreeIndex != UINT32_MAX && state.blendTreeIndex < blendTrees.size()) {
            const RtanimControllerBlendTreeRecord& tree = blendTrees[state.blendTreeIndex];
            nlohmann::json childrenJson = nlohmann::json::array();
            if (rangeInsideVector(tree.firstChild, tree.childCount, blendTreeChildren.size())) {
                for (size_t childIndex = tree.firstChild; childIndex < static_cast<size_t>(tree.firstChild) + tree.childCount; ++childIndex) {
                    const RtanimControllerBlendTreeChildRecord& child = blendTreeChildren[childIndex];
                    childrenJson.push_back({
                        {"name", stringFromTable(stringTable, child.nameOffset, child.nameSize)},
                        {"clipGuid", guidStringOrEmpty(child.clipGuid)},
                        {"clipPath", stringFromTable(stringTable, child.clipPathOffset, child.clipPathSize)},
                        {"threshold", child.threshold},
                    });
                }
            }
            blendTreeJson = {
                {"type", stringFromTable(stringTable, tree.typeOffset, tree.typeSize)},
                {"parameter", stringFromTable(stringTable, tree.parameterOffset, tree.parameterSize)},
                {"children", childrenJson},
            };
        }

        nlohmann::json item = {
            {"name", stringFromTable(stringTable, state.nameOffset, state.nameSize)},
            {"clipGuid", guidStringOrEmpty(state.clipGuid)},
            {"clipPath", stringFromTable(stringTable, state.clipPathOffset, state.clipPathSize)},
            {"speed", state.speed},
            {"loop", state.loop != 0u},
            {"default", state.defaultState != 0u},
            {"events", eventArray(state.firstEvent, state.eventCount)},
            {"transitions", transitionsJson},
        };
        if (!blendTreeJson.is_null()) {
            item["blendTree"] = std::move(blendTreeJson);
        }
        stateJson.push_back(std::move(item));
    }

    nlohmann::json layerJson = nlohmann::json::array();
    for (const RtanimControllerLayerRecord& layer : layers) {
        layerJson.push_back({
            {"name", stringFromTable(stringTable, layer.nameOffset, layer.nameSize)},
            {"clipGuid", guidStringOrEmpty(layer.clipGuid)},
            {"clipPath", stringFromTable(stringTable, layer.clipPathOffset, layer.clipPathSize)},
            {"weight", layer.weight},
            {"additive", layer.additive != 0u},
            {"mask", stringFromTable(stringTable, layer.maskOffset, layer.maskSize)},
        });
    }

    nlohmann::json maskJson = nlohmann::json::array();
    for (const RtanimControllerAvatarMaskRecord& mask : avatarMasks) {
        nlohmann::json included = nlohmann::json::array();
        nlohmann::json excluded = nlohmann::json::array();
        if (rangeInsideVector(mask.firstIncludedJoint, mask.includedJointCount, avatarMaskJoints.size())) {
            for (size_t jointIndex = mask.firstIncludedJoint; jointIndex < static_cast<size_t>(mask.firstIncludedJoint) + mask.includedJointCount; ++jointIndex) {
                const RtanimControllerAvatarMaskJointRecord& joint = avatarMaskJoints[jointIndex];
                included.push_back(stringFromTable(stringTable, joint.nameOffset, joint.nameSize));
            }
        }
        if (rangeInsideVector(mask.firstExcludedJoint, mask.excludedJointCount, avatarMaskJoints.size())) {
            for (size_t jointIndex = mask.firstExcludedJoint; jointIndex < static_cast<size_t>(mask.firstExcludedJoint) + mask.excludedJointCount; ++jointIndex) {
                const RtanimControllerAvatarMaskJointRecord& joint = avatarMaskJoints[jointIndex];
                excluded.push_back(stringFromTable(stringTable, joint.nameOffset, joint.nameSize));
            }
        }
        maskJson.push_back({
            {"name", stringFromTable(stringTable, mask.nameOffset, mask.nameSize)},
            {"includedJoints", included},
            {"excludedJoints", excluded},
        });
    }

    return nlohmann::json{{"controller", {
        {"name", path.stem().string()},
        {"parameters", parameterJson},
        {"avatarMasks", maskJson},
        {"layers", layerJson},
        {"states", stateJson},
    }}};
}

AnimationController::Event controllerEventFromJson(const nlohmann::json& json) {
    AnimationController::Event event;
    if (!json.is_object()) {
        return event;
    }
    event.name = json.value("name", std::string{});
    const nlohmann::json payload = json.value("payload", nlohmann::json::object());
    event.payloadJson = payload.is_null() ? std::string{} : payload.dump();
    return event;
}

void appendControllerEvents(
    std::vector<AnimationController::Event>& events,
    const nlohmann::json& owner,
    std::vector<std::string>* warnings,
    std::string_view ownerLabel) {
    if (!owner.contains("events") || !owner["events"].is_array()) {
        return;
    }
    for (const nlohmann::json& eventJson : owner["events"]) {
        AnimationController::Event event = controllerEventFromJson(eventJson);
        if (event.name.empty()) {
            addWarning(warnings, "Animation controller skipped an unnamed event on " + std::string(ownerLabel) + ".");
            continue;
        }
        events.push_back(std::move(event));
    }
}

void routeControllerEvents(
    AnimationControllerEvaluation& evaluation,
    const std::vector<AnimationController::Event>& events) {
    for (const AnimationController::Event& event : events) {
        evaluation.routedEventNames.push_back(event.name);
        evaluation.routedEventPayloads.push_back(event.payloadJson);
    }
}

std::vector<std::string> stringArrayFromJson(const nlohmann::json& json) {
    std::vector<std::string> values;
    if (!json.is_array()) {
        return values;
    }
    for (const nlohmann::json& item : json) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

AnimationController::BlendTree blendTreeFromJson(const nlohmann::json& json, std::vector<std::string>* warnings, std::string_view stateName) {
    AnimationController::BlendTree tree;
    if (!json.is_object()) {
        return tree;
    }
    tree.type = json.value("type", std::string("1d"));
    tree.parameter = json.value("parameter", std::string{});
    if (tree.type != "1d") {
        addWarning(warnings, "Animation controller state " + std::string(stateName) + " has unsupported blend tree type: " + tree.type);
        return tree;
    }
    if (tree.parameter.empty()) {
        addWarning(warnings, "Animation controller state " + std::string(stateName) + " has a blend tree without a parameter.");
        return tree;
    }
    if (json.contains("children") && json["children"].is_array()) {
        for (const nlohmann::json& childJson : json["children"]) {
            if (!childJson.is_object()) {
                continue;
            }
            AnimationController::BlendTreeChild child;
            child.name = childJson.value("name", std::string{});
            child.clipGuid = childJson.value("clipGuid", std::string{});
            child.clipPath = childJson.value("clipPath", std::string{});
            child.threshold = childJson.value("threshold", 0.0f);
            if (child.name.empty()) {
                child.name = child.clipPath.empty() ? child.clipGuid : child.clipPath.generic_string();
            }
            if (child.clipGuid.empty() && child.clipPath.empty()) {
                addWarning(warnings, "Animation controller skipped a blend tree child without a clip on state " + std::string(stateName) + ".");
                continue;
            }
            tree.children.push_back(std::move(child));
        }
    }
    std::sort(tree.children.begin(), tree.children.end(), [](const auto& a, const auto& b) {
        return a.threshold < b.threshold;
    });
    return tree;
}

float numericParameterValue(const AnimationControllerParameterValue& value) {
    if (value.type == AnimationControllerParameterType::Float) {
        return value.floatValue;
    }
    if (value.type == AnimationControllerParameterType::Int) {
        return static_cast<float>(value.intValue);
    }
    if (value.type == AnimationControllerParameterType::Bool) {
        return value.boolValue ? 1.0f : 0.0f;
    }
    return 0.0f;
}

void selectClipForState(
    AnimationControllerEvaluation& evaluation,
    const AnimationController::State& state,
    const std::unordered_map<std::string, AnimationControllerParameterValue>& parameters) {
    evaluation.selectedClipGuid = state.clipGuid;
    evaluation.selectedClipPath = state.clipPath;
    evaluation.selectedBlendTreeChild.clear();
    evaluation.selectedBlendTreeParameterValue = 0.0f;
    evaluation.blendTreeActive = false;
    evaluation.blendFromClipGuid.clear();
    evaluation.blendFromClipPath.clear();
    evaluation.blendFromChild.clear();
    evaluation.blendToClipGuid.clear();
    evaluation.blendToClipPath.clear();
    evaluation.blendToChild.clear();
    evaluation.blendAlpha = 0.0f;
    evaluation.blendEventPolicy = "primary_clip_only";
    evaluation.blendRootMotionPolicy = "primary_clip_candidates";
    if (!state.hasBlendTree || state.blendTree.children.empty()) {
        return;
    }
    const auto parameterIt = parameters.find(state.blendTree.parameter);
    if (parameterIt == parameters.end()) {
        return;
    }
    const float parameterValue = numericParameterValue(parameterIt->second);
    evaluation.selectedBlendTreeParameterValue = parameterValue;
    const AnimationController::BlendTreeChild* selected = &state.blendTree.children.front();
    float selectedDistance = std::abs(parameterValue - selected->threshold);
    for (const AnimationController::BlendTreeChild& child : state.blendTree.children) {
        const float distance = std::abs(parameterValue - child.threshold);
        if (distance < selectedDistance) {
            selected = &child;
            selectedDistance = distance;
        }
    }
    evaluation.selectedClipGuid = selected->clipGuid;
    evaluation.selectedClipPath = selected->clipPath;
    evaluation.selectedBlendTreeChild = selected->name;

    const AnimationController::BlendTreeChild* from = &state.blendTree.children.front();
    const AnimationController::BlendTreeChild* to = from;
    float alpha = 0.0f;
    if (parameterValue <= state.blendTree.children.front().threshold || state.blendTree.children.size() == 1) {
        from = &state.blendTree.children.front();
        to = from;
    } else if (parameterValue >= state.blendTree.children.back().threshold) {
        from = &state.blendTree.children.back();
        to = from;
    } else {
        for (size_t childIndex = 1; childIndex < state.blendTree.children.size(); ++childIndex) {
            const AnimationController::BlendTreeChild& upper = state.blendTree.children[childIndex];
            if (parameterValue > upper.threshold) {
                continue;
            }
            const AnimationController::BlendTreeChild& lower = state.blendTree.children[childIndex - 1u];
            from = &lower;
            to = &upper;
            const float span = std::max(upper.threshold - lower.threshold, 1.0e-6f);
            alpha = std::clamp((parameterValue - lower.threshold) / span, 0.0f, 1.0f);
            break;
        }
    }
    evaluation.blendTreeActive = true;
    evaluation.blendFromClipGuid = from->clipGuid;
    evaluation.blendFromClipPath = from->clipPath;
    evaluation.blendFromChild = from->name;
    evaluation.blendToClipGuid = to->clipGuid;
    evaluation.blendToClipPath = to->clipPath;
    evaluation.blendToChild = to->name;
    evaluation.blendAlpha = alpha;
    if (from != to) {
        evaluation.blendEventPolicy = "active_child_clips";
        evaluation.blendRootMotionPolicy = "active_child_candidate_union";
    }
}

bool conditionPasses(
    const AnimationController::Condition& condition,
    const std::unordered_map<std::string, AnimationControllerParameterValue>& parameters) {
    const auto it = parameters.find(condition.parameter);
    if (it == parameters.end() || it->second.type != condition.value.type) {
        return false;
    }
    const AnimationControllerParameterValue& actual = it->second;
    const AnimationControllerParameterValue& expected = condition.value;
    if (actual.type == AnimationControllerParameterType::Bool) {
        if (condition.op == "equals") return actual.boolValue == expected.boolValue;
        if (condition.op == "notEquals") return actual.boolValue != expected.boolValue;
    } else if (actual.type == AnimationControllerParameterType::Trigger) {
        return (condition.op == "trigger" || condition.op == "equals") && actual.triggerValue == expected.triggerValue;
    } else if (actual.type == AnimationControllerParameterType::Int) {
        if (condition.op == "equals") return actual.intValue == expected.intValue;
        if (condition.op == "notEquals") return actual.intValue != expected.intValue;
        if (condition.op == "greater") return actual.intValue > expected.intValue;
        if (condition.op == "less") return actual.intValue < expected.intValue;
        if (condition.op == "greaterOrEqual") return actual.intValue >= expected.intValue;
        if (condition.op == "lessOrEqual") return actual.intValue <= expected.intValue;
    } else if (actual.type == AnimationControllerParameterType::Float) {
        if (condition.op == "equals") return actual.floatValue == expected.floatValue;
        if (condition.op == "notEquals") return actual.floatValue != expected.floatValue;
        if (condition.op == "greater") return actual.floatValue > expected.floatValue;
        if (condition.op == "less") return actual.floatValue < expected.floatValue;
        if (condition.op == "greaterOrEqual") return actual.floatValue >= expected.floatValue;
        if (condition.op == "lessOrEqual") return actual.floatValue <= expected.floatValue;
    }
    return false;
}

const nlohmann::json* controllerPayloadFromRoot(const nlohmann::json& root) {
    if (root.contains("controller") && root["controller"].is_object()) {
        return &root["controller"];
    }
    if (root.contains("states") && root["states"].is_array()) {
        return &root;
    }
    return nullptr;
}

} // namespace

AnimationControllerParameterType animationControllerParameterTypeFromName(std::string_view type) {
    if (type == "bool") return AnimationControllerParameterType::Bool;
    if (type == "int") return AnimationControllerParameterType::Int;
    if (type == "float") return AnimationControllerParameterType::Float;
    if (type == "trigger") return AnimationControllerParameterType::Trigger;
    return AnimationControllerParameterType::Unknown;
}

const char* animationControllerParameterTypeName(AnimationControllerParameterType type) {
    switch (type) {
    case AnimationControllerParameterType::Bool: return "bool";
    case AnimationControllerParameterType::Int: return "int";
    case AnimationControllerParameterType::Float: return "float";
    case AnimationControllerParameterType::Trigger: return "trigger";
    case AnimationControllerParameterType::Unknown: return "unknown";
    }
    return "unknown";
}

AnimationController AnimationController::fromJson(const nlohmann::json& root, std::vector<std::string>* warnings) {
    AnimationController controller;
    const nlohmann::json* payload = controllerPayloadFromRoot(root);
    if (payload == nullptr) {
        addWarning(warnings, "Animation controller load failed: JSON does not contain a controller payload.");
        return controller;
    }

    controller.name_ = payload->value("name", std::string{});
    if (payload->contains("parameters") && (*payload)["parameters"].is_array()) {
        for (const nlohmann::json& item : (*payload)["parameters"]) {
            if (!item.is_object()) {
                continue;
            }
            Parameter parameter;
            parameter.name = item.value("name", std::string{});
            parameter.type = animationControllerParameterTypeFromName(item.value("type", std::string{}));
            parameter.defaultValue = parameterValueFromJson(parameter.type, item.value("default", nlohmann::json{}));
            if (parameter.name.empty() || parameter.type == AnimationControllerParameterType::Unknown) {
                addWarning(warnings, "Animation controller skipped an invalid parameter.");
                continue;
            }
            controller.parameters_.push_back(std::move(parameter));
        }
    }

    if (payload->contains("avatarMasks") && (*payload)["avatarMasks"].is_array()) {
        for (const nlohmann::json& maskJson : (*payload)["avatarMasks"]) {
            if (!maskJson.is_object()) {
                continue;
            }
            AvatarMask mask;
            mask.name = maskJson.value("name", std::string{});
            mask.includedJoints = stringArrayFromJson(maskJson.value("includedJoints", nlohmann::json::array()));
            mask.excludedJoints = stringArrayFromJson(maskJson.value("excludedJoints", nlohmann::json::array()));
            if (mask.name.empty()) {
                addWarning(warnings, "Animation controller skipped an unnamed avatar mask.");
                continue;
            }
            controller.avatarMasks_.push_back(std::move(mask));
        }
    }

    if (payload->contains("layers") && (*payload)["layers"].is_array()) {
        for (const nlohmann::json& layerJson : (*payload)["layers"]) {
            if (!layerJson.is_object()) {
                continue;
            }
            Layer layer;
            layer.name = layerJson.value("name", std::string{});
            layer.clipGuid = layerJson.value("clipGuid", std::string{});
            layer.clipPath = layerJson.value("clipPath", std::string{});
            layer.weight = layerJson.value("weight", 1.0f);
            layer.additive = layerJson.value("additive", false);
            layer.mask = layerJson.value("mask", std::string{});
            if (layer.name.empty()) {
                addWarning(warnings, "Animation controller skipped an unnamed layer.");
                continue;
            }
            controller.layers_.push_back(std::move(layer));
        }
    }

    if (payload->contains("states") && (*payload)["states"].is_array()) {
        for (const nlohmann::json& stateJson : (*payload)["states"]) {
            if (!stateJson.is_object()) {
                continue;
            }
            State state;
            state.name = stateJson.value("name", std::string{});
            state.clipGuid = stateJson.value("clipGuid", std::string{});
            state.clipPath = stateJson.value("clipPath", std::string{});
            state.speed = stateJson.value("speed", 1.0f);
            state.loop = stateJson.value("loop", true);
            state.defaultState = stateJson.value("default", false);
            if (state.name.empty()) {
                addWarning(warnings, "Animation controller skipped a state without a name.");
                continue;
            }
            if (state.defaultState && controller.initialState_.empty()) {
                controller.initialState_ = state.name;
            }
            appendControllerEvents(state.events, stateJson, warnings, state.name.empty() ? std::string_view{"state"} : std::string_view{state.name});
            if (stateJson.contains("blendTree") && stateJson["blendTree"].is_object()) {
                state.blendTree = blendTreeFromJson(stateJson["blendTree"], warnings, state.name);
                state.hasBlendTree = !state.blendTree.parameter.empty() && !state.blendTree.children.empty();
            }
            if (stateJson.contains("transitions") && stateJson["transitions"].is_array()) {
                for (const nlohmann::json& transitionJson : stateJson["transitions"]) {
                    if (!transitionJson.is_object()) {
                        continue;
                    }
                    Transition transition;
                    transition.to = transitionJson.value("to", std::string{});
                    transition.exitTimeSeconds = transitionJson.value("exitTimeSeconds", 0.0f);
                    if (transitionJson.contains("conditions") && transitionJson["conditions"].is_array()) {
                        for (const nlohmann::json& conditionJson : transitionJson["conditions"]) {
                            if (!conditionJson.is_object()) {
                                continue;
                            }
                            Condition condition;
                            condition.parameter = conditionJson.value("parameter", std::string{});
                            condition.op = conditionJson.value("op", std::string("equals"));
                            const AnimationControllerParameterType type = animationControllerParameterTypeFromName(conditionJson.value("type", std::string{}));
                            condition.value = parameterValueFromJson(type, conditionJson.value("value", nlohmann::json{}));
                            if (!condition.parameter.empty() && condition.value.type != AnimationControllerParameterType::Unknown) {
                                transition.conditions.push_back(std::move(condition));
                            }
                        }
                    }
                    appendControllerEvents(
                        transition.events,
                        transitionJson,
                        warnings,
                        transition.to.empty() ? std::string_view{"transition"} : std::string_view{transition.to});
                    if (!transition.to.empty()) {
                        state.transitions.push_back(std::move(transition));
                    }
                }
            }
            controller.states_.push_back(std::move(state));
        }
    }
    if (controller.initialState_.empty() && !controller.states_.empty()) {
        controller.initialState_ = controller.states_.front().name;
    }
    return controller;
}

AnimationController AnimationController::loadJson(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::ifstream file(path);
    if (!file.is_open()) {
        addWarning(warnings, "Animation controller load failed: could not open " + path.string());
        return {};
    }
    try {
        nlohmann::json root;
        file >> root;
        return fromJson(root, warnings);
    } catch (const std::exception& ex) {
        addWarning(warnings, std::string("Animation controller load failed: ") + ex.what());
    }
    return {};
}

AnimationController AnimationController::loadNativeBytes(const std::filesystem::path& pathHint, const std::vector<std::byte>& bytes, std::vector<std::string>* warnings) {
    NativeAssetReader reader;
    const NativeAssetInspection inspection = reader.inspectBytes(pathHint, bytes, true);
    if (!inspection.ok) {
        addWarning(warnings, inspection.errors.empty()
            ? "Animation controller native load failed: invalid native asset " + pathHint.string()
            : "Animation controller native load failed: " + inspection.errors.front().message);
        return {};
    }
    if (static_cast<NativeAssetKind>(inspection.header.assetKind) != NativeAssetKind::AnimationController) {
        addWarning(warnings, "Animation controller native load failed: asset is not .rtanimcontroller " + pathHint.string());
        return {};
    }
    if (std::optional<nlohmann::json> compactPayload = compactControllerPayloadJson(pathHint, bytes, inspection, warnings); compactPayload.has_value()) {
        if (!compactPayload->is_object() || compactPayload->empty()) {
            return {};
        }
        return fromJson(*compactPayload, warnings);
    }
    const auto chunkIt = std::find_if(inspection.chunks.begin(), inspection.chunks.end(), [](const NativeChunkRecord& chunk) {
        return chunk.type == kRtanimControllerMetadataJsonChunk;
    });
    if (chunkIt == inspection.chunks.end() || !chunkRangeInside(*chunkIt, bytes.size())) {
        addWarning(warnings, "Animation controller native load failed: missing metadata JSON chunk in " + pathHint.string());
        return {};
    }
    std::string metadata;
    metadata.resize(static_cast<size_t>(chunkIt->size));
    if (!metadata.empty()) {
        std::memcpy(metadata.data(), bytes.data() + chunkIt->offset, metadata.size());
    }
    try {
        return fromJson(nlohmann::json::parse(metadata), warnings);
    } catch (const std::exception& ex) {
        addWarning(warnings, std::string("Animation controller native load failed: ") + ex.what());
    }
    return {};
}

AnimationController AnimationController::loadNative(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes)) {
        addWarning(warnings, "Animation controller load failed: could not open native " + path.string());
        return {};
    }
    return loadNativeBytes(path, bytes, warnings);
}

AnimationController AnimationController::load(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    if (nativeAssetKindFromExtension(path) == NativeAssetKind::AnimationController) {
        return loadNative(path, warnings);
    }
    return loadJson(path, warnings);
}

std::unordered_map<std::string, AnimationControllerParameterValue> AnimationController::defaultParameters() const {
    std::unordered_map<std::string, AnimationControllerParameterValue> result;
    for (const Parameter& parameter : parameters_) {
        result[parameter.name] = parameter.defaultValue;
    }
    return result;
}

const AnimationController::State* AnimationController::state(std::string_view name) const {
    const auto it = std::find_if(states_.begin(), states_.end(), [&](const State& state) {
        return state.name == name;
    });
    return it == states_.end() ? nullptr : &*it;
}

AnimationControllerEvaluation AnimationController::evaluate(
    std::string_view currentState,
    float timeInStateSeconds,
    const std::unordered_map<std::string, AnimationControllerParameterValue>& parameters) const {
    AnimationControllerEvaluation result;
    result.previousState = currentState.empty() ? initialState_ : std::string(currentState);
    result.state = result.previousState;
    const State* source = state(result.previousState);
    if (source == nullptr) {
        source = state(initialState_);
        result.previousState = initialState_;
        result.state = initialState_;
    }
    if (source == nullptr) {
        return result;
    }
    selectClipForState(result, *source, parameters);
    for (const Transition& transition : source->transitions) {
        if (timeInStateSeconds < transition.exitTimeSeconds) {
            continue;
        }
        bool passed = true;
        for (const Condition& condition : transition.conditions) {
            if (!conditionPasses(condition, parameters)) {
                passed = false;
                break;
            }
        }
        if (!passed || state(transition.to) == nullptr) {
            continue;
        }
        result.state = transition.to;
        result.transitionTo = transition.to;
        result.transitioned = true;
        routeControllerEvents(result, transition.events);
        if (const State* target = state(result.state)) {
            routeControllerEvents(result, target->events);
            selectClipForState(result, *target, parameters);
        }
        for (const Condition& condition : transition.conditions) {
            if (condition.value.type == AnimationControllerParameterType::Trigger) {
                result.consumedTriggers.push_back(condition.parameter);
            }
        }
        break;
    }
    return result;
}

nlohmann::json animationControllerTransparentJson(const AnimationController& controller) {
    nlohmann::json parameters = nlohmann::json::array();
    for (const AnimationController::Parameter& parameter : controller.parameters()) {
        parameters.push_back({
            {"name", parameter.name},
            {"type", animationControllerParameterTypeName(parameter.type)},
            {"default", parameterValueJson(parameter.defaultValue)},
        });
    }

    auto eventsJson = [](const std::vector<AnimationController::Event>& events) {
        nlohmann::json out = nlohmann::json::array();
        for (const AnimationController::Event& event : events) {
            out.push_back({
                {"name", event.name},
                {"payload", nativePayloadJson(event.payloadJson)},
            });
        }
        return out;
    };

    nlohmann::json avatarMasks = nlohmann::json::array();
    for (const AnimationController::AvatarMask& mask : controller.avatarMasks()) {
        avatarMasks.push_back({
            {"name", mask.name},
            {"includedJoints", mask.includedJoints},
            {"excludedJoints", mask.excludedJoints},
        });
    }

    nlohmann::json layers = nlohmann::json::array();
    for (const AnimationController::Layer& layer : controller.layers()) {
        layers.push_back({
            {"name", layer.name},
            {"clipGuid", layer.clipGuid},
            {"clipPath", layer.clipPath.generic_string()},
            {"weight", layer.weight},
            {"additive", layer.additive},
            {"mask", layer.mask},
        });
    }

    nlohmann::json states = nlohmann::json::array();
    for (const AnimationController::State& state : controller.states()) {
        nlohmann::json transitions = nlohmann::json::array();
        for (const AnimationController::Transition& transition : state.transitions) {
            nlohmann::json conditions = nlohmann::json::array();
            for (const AnimationController::Condition& condition : transition.conditions) {
                conditions.push_back({
                    {"parameter", condition.parameter},
                    {"type", animationControllerParameterTypeName(condition.value.type)},
                    {"op", condition.op},
                    {"value", parameterValueJson(condition.value)},
                });
            }
            transitions.push_back({
                {"to", transition.to},
                {"exitTimeSeconds", transition.exitTimeSeconds},
                {"conditions", conditions},
                {"events", eventsJson(transition.events)},
            });
        }

        nlohmann::json stateJson = {
            {"name", state.name},
            {"clipGuid", state.clipGuid},
            {"clipPath", state.clipPath.generic_string()},
            {"speed", state.speed},
            {"loop", state.loop},
            {"default", state.defaultState},
            {"events", eventsJson(state.events)},
            {"transitions", transitions},
        };
        if (state.hasBlendTree) {
            nlohmann::json children = nlohmann::json::array();
            for (const AnimationController::BlendTreeChild& child : state.blendTree.children) {
                children.push_back({
                    {"name", child.name},
                    {"clipGuid", child.clipGuid},
                    {"clipPath", child.clipPath.generic_string()},
                    {"threshold", child.threshold},
                });
            }
            stateJson["blendTree"] = {
                {"type", state.blendTree.type},
                {"parameter", state.blendTree.parameter},
                {"children", children},
            };
        }
        states.push_back(std::move(stateJson));
    }

    return {
        {"assetType", "AnimationController"},
        {"schema", "TransparentAnimationControllerMetadataV1"},
        {"controller", {
            {"name", controller.name()},
            {"parameters", parameters},
            {"avatarMasks", avatarMasks},
            {"layers", layers},
            {"states", states},
        }},
    };
}

nlohmann::json animationControllerInspectionJson(const AnimationController& controller, const std::vector<std::string>& warnings) {
    nlohmann::json parameters = nlohmann::json::array();
    for (const AnimationController::Parameter& parameter : controller.parameters()) {
        parameters.push_back({
            {"name", parameter.name},
            {"type", animationControllerParameterTypeName(parameter.type)},
            {"default", parameterValueJson(parameter.defaultValue)},
        });
    }
    nlohmann::json states = nlohmann::json::array();
    for (const AnimationController::State& state : controller.states()) {
        nlohmann::json transitions = nlohmann::json::array();
        for (const AnimationController::Transition& transition : state.transitions) {
            nlohmann::json events = nlohmann::json::array();
            for (const AnimationController::Event& event : transition.events) {
                events.push_back({
                    {"name", event.name},
                    {"payload", event.payloadJson.empty() ? nlohmann::json{} : nlohmann::json::parse(event.payloadJson)},
                });
            }
            transitions.push_back({
                {"to", transition.to},
                {"exitTimeSeconds", transition.exitTimeSeconds},
                {"conditionCount", transition.conditions.size()},
                {"eventCount", transition.events.size()},
                {"events", events},
            });
        }
        nlohmann::json events = nlohmann::json::array();
        for (const AnimationController::Event& event : state.events) {
            events.push_back({
                {"name", event.name},
                {"payload", event.payloadJson.empty() ? nlohmann::json{} : nlohmann::json::parse(event.payloadJson)},
            });
        }
        nlohmann::json blendTree;
        if (state.hasBlendTree) {
            nlohmann::json children = nlohmann::json::array();
            for (const AnimationController::BlendTreeChild& child : state.blendTree.children) {
                children.push_back({
                    {"name", child.name},
                    {"clipGuid", child.clipGuid},
                    {"clipPath", child.clipPath.generic_string()},
                    {"threshold", child.threshold},
                });
            }
            blendTree = {
                {"type", state.blendTree.type},
                {"parameter", state.blendTree.parameter},
                {"childCount", state.blendTree.children.size()},
                {"children", children},
            };
        } else {
            blendTree = nullptr;
        }
        states.push_back({
            {"name", state.name},
            {"clipGuid", state.clipGuid},
            {"clipPath", state.clipPath.generic_string()},
            {"speed", state.speed},
            {"loop", state.loop},
            {"default", state.defaultState},
            {"eventCount", state.events.size()},
            {"events", events},
            {"blendTree", blendTree},
            {"transitions", transitions},
        });
    }
    nlohmann::json layers = nlohmann::json::array();
    for (const AnimationController::Layer& layer : controller.layers()) {
        layers.push_back({
            {"name", layer.name},
            {"clipGuid", layer.clipGuid},
            {"clipPath", layer.clipPath.generic_string()},
            {"weight", layer.weight},
            {"additive", layer.additive},
            {"mask", layer.mask},
        });
    }
    nlohmann::json avatarMasks = nlohmann::json::array();
    for (const AnimationController::AvatarMask& mask : controller.avatarMasks()) {
        avatarMasks.push_back({
            {"name", mask.name},
            {"includedJoints", mask.includedJoints},
            {"excludedJoints", mask.excludedJoints},
        });
    }
    const auto defaults = controller.defaultParameters();
    const AnimationControllerEvaluation evaluation = controller.evaluate(controller.initialState(), 0.0f, defaults);
    return {
        {"schema", "AnimationControllerInspectionV1"},
        {"ok", controller.valid()},
        {"name", controller.name()},
        {"initialState", controller.initialState()},
        {"parameterCount", controller.parameters().size()},
        {"stateCount", controller.states().size()},
        {"layerCount", controller.layers().size()},
        {"avatarMaskCount", controller.avatarMasks().size()},
        {"parameters", parameters},
        {"layers", layers},
        {"avatarMasks", avatarMasks},
        {"states", states},
        {"defaultEvaluation", {
            {"previousState", evaluation.previousState},
            {"state", evaluation.state},
            {"transitioned", evaluation.transitioned},
            {"transitionTo", evaluation.transitionTo},
            {"consumedTriggers", evaluation.consumedTriggers},
            {"routedEventNames", evaluation.routedEventNames},
            {"routedEventPayloads", evaluation.routedEventPayloads},
            {"selectedClipGuid", evaluation.selectedClipGuid},
            {"selectedClipPath", evaluation.selectedClipPath.generic_string()},
            {"selectedBlendTreeChild", evaluation.selectedBlendTreeChild},
            {"selectedBlendTreeParameterValue", evaluation.selectedBlendTreeParameterValue},
            {"blendTreeActive", evaluation.blendTreeActive},
            {"blendFromClipGuid", evaluation.blendFromClipGuid},
            {"blendFromClipPath", evaluation.blendFromClipPath.generic_string()},
            {"blendFromChild", evaluation.blendFromChild},
            {"blendToClipGuid", evaluation.blendToClipGuid},
            {"blendToClipPath", evaluation.blendToClipPath.generic_string()},
            {"blendToChild", evaluation.blendToChild},
            {"blendAlpha", evaluation.blendAlpha},
            {"blendEventPolicy", evaluation.blendEventPolicy},
            {"blendRootMotionPolicy", evaluation.blendRootMotionPolicy},
        }},
        {"warnings", warnings},
    };
}

int inspectAnimationControllerCommand(const std::filesystem::path& path, const std::filesystem::path& jsonOut) {
    std::vector<std::string> warnings;
    const AnimationController controller = AnimationController::load(path, &warnings);
    const nlohmann::json report = animationControllerInspectionJson(controller, warnings);
    if (!jsonOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(jsonOut.parent_path(), ec);
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Failed to write animation controller inspection: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return controller.valid() ? 0 : 1;
}

int exportAnimationControllerCommand(
    const std::filesystem::path& path,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& jsonOut) {
    std::vector<std::string> warnings;
    const AnimationController controller = AnimationController::load(path, &warnings);
    const nlohmann::json exported = animationControllerTransparentJson(controller);
    bool wroteOutput = false;
    if (controller.valid()) {
        if (!outputPath.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            std::ofstream file(outputPath);
            if (!file.is_open()) {
                warnings.push_back("Failed to write exported animation controller graph: " + outputPath.string());
            } else {
                file << exported.dump(2);
                wroteOutput = true;
            }
        } else {
            std::cout << exported.dump(2) << '\n';
            wroteOutput = true;
        }
    }

    std::vector<std::string> roundTripWarnings;
    const AnimationController roundTrip = AnimationController::fromJson(exported, &roundTripWarnings);
    nlohmann::json report = {
        {"schema", "AnimationControllerGraphExportReportV1"},
        {"ok", controller.valid() && wroteOutput && roundTrip.valid()},
        {"sourcePath", path.generic_string()},
        {"outputPath", outputPath.empty() ? std::string{} : outputPath.generic_string()},
        {"sourceValid", controller.valid()},
        {"exported", wroteOutput},
        {"roundTripValid", roundTrip.valid()},
        {"transparentSchema", "TransparentAnimationControllerMetadataV1"},
        {"graphSaveEmissionImplemented", true},
        {"sourceControlReviewable", true},
        {"sourceControlCheckoutImplemented", false},
        {"editorGraphAuthoringImplemented", false},
        {"parameterCount", controller.parameters().size()},
        {"stateCount", controller.states().size()},
        {"layerCount", controller.layers().size()},
        {"avatarMaskCount", controller.avatarMasks().size()},
        {"warnings", warnings},
        {"roundTripWarnings", roundTripWarnings},
    };
    if (!jsonOut.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(jsonOut.parent_path(), ec);
        std::ofstream file(jsonOut);
        if (!file.is_open()) {
            std::cerr << "Failed to write animation controller export report: " << jsonOut.string() << '\n';
            return 1;
        }
        file << report.dump(2);
    } else if (!outputPath.empty()) {
        std::cout << report.dump(2) << '\n';
    }
    return report.value("ok", false) ? 0 : 1;
}

int mutateAnimationControllerCommand(
    const std::filesystem::path& path,
    const std::filesystem::path& mutationPath,
    const std::filesystem::path& outputPath,
    const std::filesystem::path& jsonOut,
    bool dryRun,
    bool forceOverwrite) {
    std::vector<std::string> warnings;
    std::vector<std::string> applied;
    const AnimationController controller = AnimationController::load(path, &warnings);
    nlohmann::json graph = animationControllerTransparentJson(controller);

    nlohmann::json mutationRoot = nlohmann::json::object();
    std::string readError;
    bool mutationLoaded = false;
    if (!mutationPath.empty()) {
        mutationLoaded = readJsonFile(mutationPath, mutationRoot, readError);
        if (!mutationLoaded) {
            warnings.push_back("Animation controller mutation file load failed: " + readError);
        }
    } else {
        warnings.push_back("Animation controller mutation requires --controller-mutation-json <path>.");
    }

    size_t attemptedOperationCount = 0;
    size_t appliedOperationCount = 0;
    if (controller.valid() && mutationLoaded) {
        const nlohmann::json operations = normalizedControllerMutationJson(mutationRoot);
        attemptedOperationCount = operations.size();
        for (const nlohmann::json& operation : operations) {
            if (applyControllerMutationOperation(graph, operation, applied, warnings)) {
                ++appliedOperationCount;
            }
        }
    }

    std::vector<std::string> roundTripWarnings;
    const AnimationController roundTrip = AnimationController::fromJson(graph, &roundTripWarnings);
    const bool roundTripValid = roundTrip.valid();
    const bool outputRequested = !outputPath.empty();
    const bool sourceOverwrite = outputRequested && samePath(path, outputPath);
    const bool outputExists = outputRequested && std::filesystem::exists(outputPath);
    const nlohmann::json sourceControl = sourceControlSnapshotJson(outputRequested ? outputPath : path);
    const bool outputReadOnly = sourceControl.value("readOnly", false);
    std::vector<std::string> blockers;
    if (!controller.valid()) {
        blockers.push_back("Source controller is invalid.");
    }
    if (!mutationLoaded) {
        blockers.push_back("Mutation JSON was not loaded.");
    }
    if (attemptedOperationCount == 0u) {
        blockers.push_back("Mutation JSON contains no operations.");
    }
    if (attemptedOperationCount != appliedOperationCount) {
        blockers.push_back("One or more mutation operations were not applied.");
    }
    if (!roundTripValid) {
        blockers.push_back("Mutated graph did not round-trip through the AnimationController parser.");
    }
    if (!dryRun && !outputRequested) {
        blockers.push_back("Non-dry-run mutation requires --controller-output <path>.");
    }
    if (!dryRun && outputExists && !forceOverwrite) {
        blockers.push_back("Output already exists; pass --controller-force-overwrite after reviewing the source-control report.");
    }
    if (!dryRun && sourceOverwrite && !forceOverwrite) {
        blockers.push_back("In-place source mutation requires --controller-force-overwrite.");
    }
    if (!dryRun && outputReadOnly) {
        blockers.push_back("Output path is read-only or not checkout/editable by the current process.");
    }

    bool wroteOutput = false;
    std::string writeError;
    if (!dryRun && blockers.empty() && outputRequested) {
        wroteOutput = writeJsonFile(outputPath, graph, writeError);
        if (!wroteOutput) {
            warnings.push_back("Failed to write mutated animation controller graph: " + writeError);
            blockers.push_back("Output write failed.");
        }
    }

    const bool ok = controller.valid() && mutationLoaded && attemptedOperationCount > 0u &&
        attemptedOperationCount == appliedOperationCount && roundTripValid && blockers.empty() &&
        (dryRun || wroteOutput);
    nlohmann::json report = {
        {"schema", "AnimationControllerGraphMutationReportV1"},
        {"ok", ok},
        {"sourcePath", path.generic_string()},
        {"mutationPath", mutationPath.empty() ? std::string{} : mutationPath.generic_string()},
        {"outputPath", outputPath.empty() ? std::string{} : outputPath.generic_string()},
        {"dryRun", dryRun},
        {"forceOverwrite", forceOverwrite},
        {"sourceValid", controller.valid()},
        {"mutationLoaded", mutationLoaded},
        {"attemptedOperationCount", attemptedOperationCount},
        {"appliedOperationCount", appliedOperationCount},
        {"appliedOperations", applied},
        {"roundTripValid", roundTripValid},
        {"written", wroteOutput},
        {"sourceOverwrite", sourceOverwrite},
        {"sourceControlAware", true},
        {"sourceControlCheckoutAware", true},
        {"sourceControlProviderMutationImplemented", false},
        {"sourceControlReviewable", true},
        {"sourceControl", sourceControl},
        {"transparentSchema", "TransparentAnimationControllerMetadataV1"},
        {"graphMutationImplemented", true},
        {"editorGraphAuthoringImplemented", false},
        {"blockers", blockers},
        {"warnings", warnings},
        {"roundTripWarnings", roundTripWarnings},
        {"mutatedGraph", dryRun ? graph : nlohmann::json()},
    };
    if (!jsonOut.empty()) {
        std::string reportError;
        if (!writeJsonFile(jsonOut, report, reportError)) {
            std::cerr << "Failed to write animation controller mutation report: " << reportError << '\n';
            return 1;
        }
    } else {
        std::cout << report.dump(2) << '\n';
    }
    return ok ? 0 : 1;
}

} // namespace rtv
