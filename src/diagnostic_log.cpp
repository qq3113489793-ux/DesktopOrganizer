#include "diagnostic_log.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace diagnostic_log {
namespace {

// Two 5 MiB generations keep total log use near 10 MiB. On rotation the old
// previous generation is replaced, so the directory cannot grow forever.
constexpr unsigned long long kMaximumLogBytes = 5ull * 1024ull * 1024ull;

std::mutex& LogMutex() {
    static std::mutex mutex;
    return mutex;
}

std::filesystem::path ResolvePath() {
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_LOG", overridePath,
        static_cast<DWORD>(std::size(overridePath)));
    if (overrideLength > 0 && overrideLength < std::size(overridePath))
        return std::filesystem::path(overridePath);

    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                    &localAppData)) || !localAppData) return {};
    std::filesystem::path result = std::filesystem::path(localAppData) /
                                   L"DesktopOrganizer" / L"logs" /
                                   L"DesktopOrganizer.log";
    CoTaskMemFree(localAppData);
    return result;
}

std::filesystem::path& LogPath() {
    static std::filesystem::path path = ResolvePath();
    return path;
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring OneLine(std::wstring_view value) {
    std::wstring result(value);
    for (wchar_t& character : result) {
        if (character == L'\r' || character == L'\n' || character == L'\t') character = L' ';
    }
    return result;
}

void RotateIfNeeded(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < kMaximumLogBytes) return;
    const std::filesystem::path previous = path.parent_path() / L"DesktopOrganizer.previous.log";
    MoveFileExW(path.c_str(), previous.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}  // namespace

std::filesystem::path Path() noexcept {
    try {
        return LogPath();
    } catch (...) {
        return {};
    }
}

void Write(std::wstring_view event, std::wstring_view details) noexcept {
    try {
        std::lock_guard lock(LogMutex());
        const auto path = LogPath();
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;
        RotateIfNeeded(path);

        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t prefix[160]{};
        swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu event=",
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                   now.wSecond, now.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());
        std::wstring line(prefix);
        line += OneLine(event);
        if (!details.empty()) {
            line += L" details=\"";
            line += OneLine(details);
            line += L'"';
        }
        line += L"\r\n";
        const std::string utf8 = Utf8(line);
        if (utf8.empty()) return;

        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
        // Diagnostic logging is deliberately best-effort and must never alter
        // the result of a user operation.
    }
}

}  // namespace diagnostic_log
