#pragma once

#include "model.h"

#include <cstddef>
#include <filesystem>
#include <vector>

enum class ConfigLoadStatus {
    Missing,
    Loaded,
    RecoveredBackup,
    Invalid,
    FutureVersion,
    IsolationError,
};

enum class ConfigSaveStage {
    None,
    Blocked,
    Limits,
    CreateDirectory,
    Validate,
    RemoveTemporary,
    OpenTemporary,
    WriteTemporary,
    ReplaceFile,
};

struct ConfigSaveFailure {
    ConfigSaveStage stage = ConfigSaveStage::None;
    unsigned long error = 0;
    unsigned long replaceError = 0;
};

struct ConfigLoadResult {
    ConfigLoadStatus status = ConfigLoadStatus::Missing;
    std::vector<ContainerState> containers;
    GlobalStyle globalStyle;
    // One-time behavior migration: configs written while snapToGrid defaulted
    // to true had every container stuck on the desktop grid. The loader resets
    // them to free movement once and asks the caller to persist the flag.
    bool freeMoveMigrated = false;

    bool IsUsable() const {
        return status == ConfigLoadStatus::Loaded ||
               status == ConfigLoadStatus::RecoveredBackup;
    }
};

class ConfigStore {
public:
    static constexpr std::size_t MaxConfigBytes = 8 * 1024 * 1024;
    static constexpr std::size_t MaxContainers = 256;
    static constexpr std::size_t MaxItemsPerContainer = 4096;
    static constexpr std::size_t MaxTotalItems = 65536;
    static constexpr int MaxGridExtent = 128;

    ConfigStore();
    explicit ConfigStore(std::filesystem::path path);
    ConfigLoadResult Load() const;
    bool Save(const std::vector<ContainerState>& containers, const GlobalStyle* globalStyle = nullptr) const;
    const std::filesystem::path& Path() const { return path_; }
    bool IsWriteBlocked() const { return isolationError_ || writeBlocked_; }
    ConfigSaveFailure LastSaveFailure() const { return lastSaveFailure_; }

private:
    std::filesystem::path path_;
    bool isolationError_ = false;
    mutable bool writeBlocked_ = false;
    mutable bool preserveBackupOnNextSave_ = false;
    mutable ConfigSaveFailure lastSaveFailure_;
};
