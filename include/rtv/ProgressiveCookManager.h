#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/AssetRegistry.h"
#include "rtv/EntityId.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rtv {

// Generation stages for progressive import/cook.
// Stages are monotonically increasing; a later stage atomically
// replaces all earlier stages once validated.
enum class CookGenerationStage : uint8_t {
    // Generation not yet started.
    None = 0,
    // Metadata extracted: GUIDs, hierarchy, bounds, cameras, lights.
    Metadata = 1,
    // Hierarchy expanded with transforms, material constants loaded.
    HierarchyAndMaterials = 2,
    // Preview proxies generated, lowest texture mips cooked.
    ProxiesAndLowMips = 3,
    // Full mesh payloads cooked and validated.
    FullMesh = 4,
    // Full textures, animation, BLAS metadata complete.
    Complete = 5,
};

// An import session tracks progressive cook of a source asset.
// Each generation is an atomic set of artifacts.
struct ImportSession {
    // Unique session ID assigned at drag/drop time.
    uint64_t sessionId = 0;

    // Source file being imported.
    std::filesystem::path sourcePath;

    // Current highest completed stage.
    CookGenerationStage completedStage = CookGenerationStage::None;

    // Current active (published) stage that runtime streaming consumes.
    CookGenerationStage publishedStage = CookGenerationStage::None;

    // Generation artifact directories. Each directory contains the outputs
    // for that stage. When a later stage validates, the registry pointer
    // advances and older generations may be cleaned up.
    struct Generation {
        CookGenerationStage stage = CookGenerationStage::None;
        std::filesystem::path directory;
        bool validated = false;
        uint64_t cookedAssetCount = 0;
        uint64_t cookedBytes = 0;
        std::string manifestChecksum;
    };
    std::vector<Generation> generations;

    // Root GUID published into the asset registry.
    AssetGuid rootGuid;

    // Whether the import was cancelled.
    bool cancelled = false;

    // Whether the import encountered a fatal error.
    bool failed = false;
    std::string failureReason;

    // Time tracking (in seconds since session start).
    double startTime = 0.0;
    double stageCompletedAt[6] = {};  // indexed by CookGenerationStage

    // User-facing label for progress display.
    std::string label;

    // Whether the import session is associated with a live placement.
    // If the user deletes the placed shell during import, the session
    // may continue as import-only or cancel per user preference.
    bool placementActive = false;
    EntityId placementRootEntity;

    // Number of outstanding cook jobs for the current stage.
    uint32_t outstandingJobs = 0;
};

// Manages progressive import session lifecycle and generation-based publishing.
//
// Key behaviors:
// - Each import session has a stable session ID and multiple generations.
// - Generations are published atomically: the registry points to the newest
//   validated generation. Runtime streaming consumes the active generation.
// - Failed later stages do not invalidate already-usable earlier generations.
// - Older generations are cleaned up by policy (keep N recent, remove stale).
// - Cancellation is safe: partial writes are in temp directories, and the
//   registry is only updated on atomic publish.
class ProgressiveCookManager final : private NonCopyable {
public:
    ProgressiveCookManager() = default;

    // Start a new import session for a source file.
    [[nodiscard]] uint64_t startSession(const std::filesystem::path& sourcePath,
                                         const std::string& label,
                                         const AssetGuid& rootGuid,
                                         const std::filesystem::path& tempRoot);

    // Signal that a cook stage has completed and its artifacts are ready
    // for validation and possible publish.
    void stageCompleted(uint64_t sessionId, CookGenerationStage stage,
                        const std::filesystem::path& generationDir,
                        uint64_t assetCount, uint64_t bytes,
                        const std::string& manifestChecksum);

    // Validate a completed stage and, if valid, atomically publish it
    // as the new active generation.
    [[nodiscard]] bool validateAndPublish(uint64_t sessionId, CookGenerationStage stage);

    // Roll back to a previous valid generation (e.g. if validation fails).
    [[nodiscard]] bool rollback(uint64_t sessionId, CookGenerationStage targetStage);

    // Cancel an import session. Cleans up temp files.
    void cancelSession(uint64_t sessionId);

    // Mark the session as failed with a reason.
    void failSession(uint64_t sessionId, const std::string& reason);

    // Clean up stale generations older than keepCount.
    void cleanupStaleGenerations(uint64_t sessionId, uint32_t keepCount);

    // Query sessions.
    [[nodiscard]] const ImportSession* findSession(uint64_t sessionId) const;
    [[nodiscard]] const ImportSession* findSessionByRootGuid(AssetGuid rootGuid) const;
    [[nodiscard]] const ImportSession* findSessionByPlacement(EntityId entityId) const;

    [[nodiscard]] const std::vector<ImportSession>& sessions() const { return sessions_; }

    // Get the active generation directory that runtime streaming should consume.
    [[nodiscard]] std::filesystem::path activeGenerationPath(uint64_t sessionId) const;

    // Settings.
    struct Settings {
        bool keepAllGenerations = false;
        uint32_t maxGenerationsToKeep = 3;
        std::filesystem::path cookCacheRoot;
    };

    void setSettings(const Settings& settings) { settings_ = settings; }
    [[nodiscard]] const Settings& settings() const { return settings_; }

private:
    Settings settings_;
    std::vector<ImportSession> sessions_;
    uint64_t nextSessionId_ = 1;
};

[[nodiscard]] const char* cookGenerationStageName(CookGenerationStage stage);
[[nodiscard]] nlohmann::json importSessionToJson(const ImportSession& session);

} // namespace rtv
