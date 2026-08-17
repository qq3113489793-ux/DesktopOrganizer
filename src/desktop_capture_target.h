#pragma once

#include <windows.h>

#include <cwchar>
#include <iterator>

namespace desktop_capture_target {

inline bool IsDesktopHostClass(HWND window) {
    wchar_t className[128]{};
    if (!window || !GetClassNameW(window, className, static_cast<int>(std::size(className))))
        return false;
    return std::wcscmp(className, L"Progman") == 0 ||
           std::wcscmp(className, L"WorkerW") == 0;
}

// The top-level Explorer window that owns SHELLDLL_DefView is the stable
// boundary between desktop content and ordinary applications. It is not
// necessarily the same HWND used to capture an animated wallpaper.
inline HWND FindDesktopViewHost() {
    struct Search {
        HWND withDefView = nullptr;
        HWND firstWorker = nullptr;
    } search;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& result = *reinterpret_cast<Search*>(parameter);
        wchar_t className[128]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr)) {
            result.withDefView = window;
            return FALSE;
        }
        if (std::wcscmp(className, L"WorkerW") == 0 &&
            !result.firstWorker && IsWindowVisible(window)) result.firstWorker = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.withDefView) return search.withDefView;
    if (HWND shell = GetShellWindow(); IsDesktopHostClass(shell)) return shell;
    if (HWND progman = FindWindowW(L"Progman", nullptr)) return progman;
    return search.firstWorker;
}

inline HWND Find() {
    if (HWND progman = FindWindowW(L"Progman", nullptr)) return progman;

    // Newer Explorer and several animated-wallpaper hosts put the desktop on
    // a WorkerW instead of Progman. Prefer the WorkerW that owns DefView.
    struct Search {
        HWND withDefView = nullptr;
        HWND firstWorker = nullptr;
    } search;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& result = *reinterpret_cast<Search*>(parameter);
        wchar_t className[128]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr)) {
            result.withDefView = window;
            return FALSE;
        }
        if (std::wcscmp(className, L"WorkerW") == 0 &&
            !result.firstWorker && IsWindowVisible(window)) result.firstWorker = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.withDefView) return search.withDefView;

    HWND shell = GetShellWindow();
    if (IsDesktopHostClass(shell)) return shell;
    return search.firstWorker;
}

} // namespace desktop_capture_target
