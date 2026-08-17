#include "desktop_anchor.h"

#include <windows.h>

#include <algorithm>
#include <fstream>

namespace desktop_anchor {
namespace {

constexpr char kMagic[] = "DESKTOP-ORGANIZER-GROUP/1\n";
constexpr wchar_t kLegacyExtension[] = L".desktoporganizer-group";

bool SameText(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool HasAnchorExtension(const std::filesystem::path& path) {
    return _wcsicmp(path.extension().c_str(), kLegacyExtension) == 0;
}

} // namespace

std::optional<std::wstring> ReadGroupId(const std::filesystem::path& path) {
    try {
        if (!HasAnchorExtension(path)) return std::nullopt;
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > 512) return std::nullopt;
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::nullopt;
        std::string payload(static_cast<size_t>(size), '\0');
        input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!input || !payload.starts_with(kMagic)) return std::nullopt;
        std::string id = payload.substr(sizeof(kMagic) - 1);
        const size_t newline = id.find('\n');
        if (newline == std::string::npos) return std::nullopt;
        id.resize(newline);
        if (id.empty() || id.size() > 128 ||
            std::any_of(id.begin(), id.end(), [](unsigned char ch) { return ch < 0x20 || ch > 0x7e; }))
            return std::nullopt;
        return std::wstring(id.begin(), id.end());
    } catch (...) {
        return std::nullopt;
    }
}

bool RemoveIfOwned(const std::filesystem::path& path, std::wstring_view groupId) {
    const auto id = ReadGroupId(path);
    if (!id || !SameText(*id, groupId)) return false;
    return DeleteFileW(path.c_str()) != FALSE;
}

} // namespace desktop_anchor
