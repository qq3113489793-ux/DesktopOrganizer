#pragma once

#include <filesystem>
#include <string_view>

namespace diagnostic_log {

void Write(std::wstring_view event, std::wstring_view details = {}) noexcept;
std::filesystem::path Path() noexcept;

}  // namespace diagnostic_log
