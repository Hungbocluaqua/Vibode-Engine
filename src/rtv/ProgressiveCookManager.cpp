#include "rtv/ProgressiveCookManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

namespace rtv {

uint64_t ProgressiveCookManager::startSession(const std::filesystem::path& sourcePath,
                                                const std::string& label,
                                                const AssetGuid& rootGuid,
                                                const std::filesystem::path& tempRoot) {
    ImportSession session;
    session.sessionId = nextSessionId_++;
    session.sourcePath = sourcePath;
    session.label = label;
    session.rootGuid = rootGuid;
    session.startTime = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Create temp root for this session's generations.
    if (!tempRoot.empty()) {
        std::filesystem::create_directories(tempRoot / ("session_" + std::to_string(session.sessionId)));
    }

    sessions_.push_back(std::move(session));
    return sessions_.back().sessionId;
}

void ProgressiveCookManager::stageCompleted(uint64_t sessionId, CookGenerationStage stage,
                                              const std::filesystem::path& generationDir,
                                              uint64_t assetCount, uint64_t bytes,
                                              const std::string& manifestChecksum) {
    ImportSession* session = const_cast<ImportSession*>(findSession(sessionId));
    if (session == nullptr || session->cancelled || session->failed) {
        return;
    }

    // Record generation completion.
    ImportSession::Generation gen;
    gen.stage = stage;
    gen.directory = generationDir;
    gen.validated = false;  // Validation happens in validateAndPublish.
    gen.cookedAssetCount = assetCount;
    gen.cookedBytes = bytes;
    gen.manifestChecksum = manifestChecksum;

    // Remove any previous generation at this stage (re-cook).
    session->generations.erase(
        std::remove_if(session->generations.begin(), session->generations.end(),
            [stage](const ImportSession::Generation& g) { return g.stage == stage; }),
        session->generations.end());

    session->generations.push_back(std::move(gen));
    session->completedStage = std::max(session->completedStage, stage);
    session->stageCompletedAt[static_cast<size_t>(stage)] =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - session->startTime;
}

bool ProgressiveCookManager::validateAndPublish(uint64_t sessionId, CookGenerationStage stage) {
    ImportSession* session = const_cast<ImportSession*>(findSession(sessionId));
    if (session == nullptr || session->cancelled || session->failed) {
        return false;
    }

    // Find the generation for this stage.
    auto genIt = std::find_if(session->generations.begin(), session->generations.end(),
        [stage](const ImportSession::Generation& g) { return g.stage == stage; });
    if (genIt == session->generations.end()) {
        return false;
    }

    // Basic validation: generation directory must exist and have assets.
    if (!std::filesystem::exists(genIt->directory) || genIt->cookedAssetCount == 0) {
        return false;
    }

    // Mark validated and publish.
    genIt->validated = true;

    // Only advance published stage if this is the next sequential stage
    // or higher (allows skipping stages if higher ones are valid).
    if (stage > session->publishedStage) {
        session->publishedStage = stage;
    }

    return true;
}

bool ProgressiveCookManager::rollback(uint64_t sessionId, CookGenerationStage targetStage) {
    ImportSession* session = const_cast<ImportSession*>(findSession(sessionId));
    if (session == nullptr) {
        return false;
    }

    // Find a validated generation at or before the target stage.
    const CookGenerationStage* bestStage = nullptr;
    for (const ImportSession::Generation& gen : session->generations) {
        if (gen.validated && gen.stage <= targetStage) {
            if (bestStage == nullptr || gen.stage > *bestStage) {
                bestStage = &gen.stage;
            }
        }
    }

    if (bestStage != nullptr) {
        session->publishedStage = *bestStage;
        return true;
    }

    // No validated generation at this level; roll back to metadata if available.
    for (const ImportSession::Generation& gen : session->generations) {
        if (gen.validated && gen.stage == CookGenerationStage::Metadata) {
            session->publishedStage = CookGenerationStage::Metadata;
            return true;
        }
    }

    return false;
}

void ProgressiveCookManager::cancelSession(uint64_t sessionId) {
    ImportSession* session = const_cast<ImportSession*>(findSession(sessionId));
    if (session != nullptr) {
        session->cancelled = true;
    }
}

void ProgressiveCookManager::failSession(uint64_t sessionId, const std::string& reason) {
    ImportSession* session = const_cast<ImportSession*>(findSession(sessionId));
    if (session != nullptr) {
        session->failed = true;
        session->failureReason = reason;
    }
}

void ProgressiveCookManager::cleanupStaleGenerations(uint64_t sessionId, uint32_t keepCount) {
    ImportSession* session = const_cast<ImportSession*>(findSession(sessionId));
    if (session == nullptr || session->generations.size() <= keepCount) {
        return;
    }

    if (settings_.keepAllGenerations) {
        return;
    }

    // Sort by stage ascending, keep the most recent `keepCount`.
    std::sort(session->generations.begin(), session->generations.end(),
        [](const ImportSession::Generation& a, const ImportSession::Generation& b) {
            return static_cast<uint8_t>(a.stage) < static_cast<uint8_t>(b.stage);
        });

    const uint32_t toKeep = std::max(keepCount, settings_.maxGenerationsToKeep);
    // Find the published generation so we never remove it.
    auto publishedIt = std::find_if(session->generations.begin(), session->generations.end(),
        [session](const ImportSession::Generation& g) { return g.stage == session->publishedStage; });
    const bool publishedFound = publishedIt != session->generations.end();

    while (session->generations.size() > toKeep) {
        // Scan for the oldest generation that is NOT the published one.
        bool removed = false;
        for (auto it = session->generations.begin(); it != session->generations.end(); ++it) {
            if (!publishedFound || it->stage != session->publishedStage) {
                // Clean up directory.
                if (!it->directory.empty() && std::filesystem::exists(it->directory)) {
                    std::error_code ec;
                    std::filesystem::remove_all(it->directory, ec);
                }
                session->generations.erase(it);
                removed = true;
                break;  // Iterator invalidated; restart while loop.
            }
        }
        if (!removed) {
            break;  // Only the published generation remains; don't remove it.
        }
    }
}

const ImportSession* ProgressiveCookManager::findSession(uint64_t sessionId) const {
    for (const ImportSession& session : sessions_) {
        if (session.sessionId == sessionId) {
            return &session;
        }
    }
    return nullptr;
}

const ImportSession* ProgressiveCookManager::findSessionByRootGuid(AssetGuid rootGuid) const {
    for (const ImportSession& session : sessions_) {
        if (session.rootGuid == rootGuid) {
            return &session;
        }
    }
    return nullptr;
}

const ImportSession* ProgressiveCookManager::findSessionByPlacement(EntityId entityId) const {
    for (const ImportSession& session : sessions_) {
        if (session.placementActive && session.placementRootEntity == entityId) {
            return &session;
        }
    }
    return nullptr;
}

std::filesystem::path ProgressiveCookManager::activeGenerationPath(uint64_t sessionId) const {
    const ImportSession* session = findSession(sessionId);
    if (session == nullptr || session->publishedStage == CookGenerationStage::None) {
        return {};
    }

    // Find the published generation's directory.
    for (const ImportSession::Generation& gen : session->generations) {
        if (gen.stage == session->publishedStage && gen.validated) {
            return gen.directory;
        }
    }
    return {};
}

const char* cookGenerationStageName(CookGenerationStage stage) {
    switch (stage) {
    case CookGenerationStage::None: return "none";
    case CookGenerationStage::Metadata: return "metadata";
    case CookGenerationStage::HierarchyAndMaterials: return "hierarchy_and_materials";
    case CookGenerationStage::ProxiesAndLowMips: return "proxies_and_low_mips";
    case CookGenerationStage::FullMesh: return "full_mesh";
    case CookGenerationStage::Complete: return "complete";
    }
    return "unknown";
}

nlohmann::json importSessionToJson(const ImportSession& session) {
    nlohmann::json gens = nlohmann::json::array();
    for (const ImportSession::Generation& gen : session.generations) {
        gens.push_back({
            {"stage", cookGenerationStageName(gen.stage)},
            {"directory", gen.directory.generic_string()},
            {"validated", gen.validated},
            {"cooked_asset_count", gen.cookedAssetCount},
            {"cooked_bytes", gen.cookedBytes},
            {"manifest_checksum", gen.manifestChecksum},
        });
    }

    return nlohmann::json{
        {"session_id", session.sessionId},
        {"source_path", session.sourcePath.generic_string()},
        {"label", session.label},
        {"completed_stage", cookGenerationStageName(session.completedStage)},
        {"published_stage", cookGenerationStageName(session.publishedStage)},
        {"root_guid", session.rootGuid},
        {"cancelled", session.cancelled},
        {"failed", session.failed},
        {"failure_reason", session.failureReason},
        {"placement_active", session.placementActive},
        {"outstanding_jobs", session.outstandingJobs},
        {"generations", gens},
    };
}

} // namespace rtv
