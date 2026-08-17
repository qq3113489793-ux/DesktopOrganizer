#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace desktop_anchor {

std::optional<std::wstring> ReadGroupId(const std::filesystem::path& path);
bool RemoveIfOwned(const std::filesystem::path& path, std::wstring_view groupId);

} // namespace desktop_anchor
