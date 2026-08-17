#pragma once

#include <filesystem>
#include <windows.h>

namespace desktop_item_policy {

enum class Classification {
    ReferenceOnly,
    AbsorbShortcut,
};

inline bool SamePath(const std::filesystem::path& left,
                     const std::filesystem::path& right) {
    if (left.empty() || right.empty()) return false;
    return CompareStringOrdinal(left.lexically_normal().c_str(), -1,
                                right.lexically_normal().c_str(), -1, TRUE) == CSTR_EQUAL;
}

inline Classification Classify(const std::filesystem::path& path,
                               const std::filesystem::path& userDesktop,
                               DWORD attributes) {
    if (path.empty() || userDesktop.empty() || attributes == INVALID_FILE_ATTRIBUTES)
        return Classification::ReferenceOnly;
    if (!SamePath(path.parent_path(), userDesktop))
        return Classification::ReferenceOnly;
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return Classification::ReferenceOnly;

    const std::wstring extension = path.extension().wstring();
    if (_wcsicmp(extension.c_str(), L".lnk") != 0 &&
        _wcsicmp(extension.c_str(), L".url") != 0)
        return Classification::ReferenceOnly;

    return Classification::AbsorbShortcut;
}

} // namespace desktop_item_policy
