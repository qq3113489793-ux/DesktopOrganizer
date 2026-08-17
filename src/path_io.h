#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace path_io {

inline std::filesystem::path ExtendedLengthPath(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::wstring value = path.wstring();
    if (value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\.\\")) return path;

    const DWORD required = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
    if (required != 0) {
        std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
        const DWORD written = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()),
                                               buffer.data(), nullptr);
        if (written != 0 && written < buffer.size()) value.assign(buffer.data(), written);
    }
    if (value.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\" + value.substr(2));
    return std::filesystem::path(L"\\\\?\\" + value);
}

inline bool CopyPathPreservingSource(const std::filesystem::path& source,
                                     const std::filesystem::path& destination,
                                     DWORD* errorCode = nullptr) {
    if (errorCode) *errorCode = ERROR_SUCCESS;
    const auto extendedSource = ExtendedLengthPath(source);
    const auto extendedDestination = ExtendedLengthPath(destination);
    const DWORD attributes = GetFileAttributesW(extendedSource.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (errorCode) *errorCode = GetLastError();
        return false;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (CopyFileW(extendedSource.c_str(), extendedDestination.c_str(), TRUE)) return true;
        if (errorCode) *errorCode = GetLastError();
        return false;
    }
    std::error_code error;
    std::filesystem::copy(extendedSource, extendedDestination,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::copy_symlinks,
                          error);
    if (errorCode) *errorCode = static_cast<DWORD>(error.value());
    return !error;
}

}  // namespace path_io
