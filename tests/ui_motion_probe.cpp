#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct WindowSearch {
    DWORD processId = 0;
    std::wstring className;
    std::wstring title;
    std::vector<HWND> matches;
};

BOOL CALLBACK CollectTopLevel(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search.processId) return TRUE;
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    wchar_t title[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (search.className == className &&
        (search.title.empty() || search.title == title)) search.matches.push_back(window);
    return TRUE;
}

BOOL CALLBACK CollectSliders(HWND window, LPARAM parameter) {
    auto& sliders = *reinterpret_cast<std::vector<HWND>*>(parameter);
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (wcscmp(className, L"DesktopOrganizer.Slider") == 0) sliders.push_back(window);
    return TRUE;
}

struct TextControlSearch {
    const wchar_t* text = nullptr;
    HWND match = nullptr;
};

BOOL CALLBACK FindControlByText(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<TextControlSearch*>(parameter);
    wchar_t text[256]{};
    GetWindowTextW(window, text, static_cast<int>(std::size(text)));
    if (wcscmp(text, search.text) == 0) {
        search.match = window;
        return FALSE;
    }
    return TRUE;
}

std::vector<HWND> WindowsFor(DWORD processId, const wchar_t* className,
                             const wchar_t* title = nullptr) {
    WindowSearch search{processId, className, title ? title : L""};
    EnumWindows(CollectTopLevel, reinterpret_cast<LPARAM>(&search));
    return search.matches;
}

double Milliseconds(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

int WindowWidth(HWND window) {
    RECT rect{};
    return GetWindowRect(window, &rect) ? rect.right - rect.left : -1;
}

RECT WindowRect(HWND window) {
    RECT rect{};
    GetWindowRect(window, &rect);
    return rect;
}

bool SaveWindowSnapshot(HWND window, const wchar_t* path) {
    if (!window || !path || !*path) return false;
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return false;

    HDC source = GetDC(window);
    HDC memory = source ? CreateCompatibleDC(source) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(source, &info, DIB_RGB_COLORS,
                                               &pixels, nullptr, 0) : nullptr;
    if (!source || !memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (source) ReleaseDC(window, source);
        return false;
    }

    HGDIOBJ previous = SelectObject(memory, bitmap);
    PatBlt(memory, 0, 0, width, height, WHITENESS);
    const bool printed = PrintWindow(window, memory, PW_CLIENTONLY) != FALSE;
    const DWORD pixelBytes = static_cast<DWORD>(width * height * 4);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + pixelBytes;
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    if (output) {
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        output.write(static_cast<const char*>(pixels), pixelBytes);
    }
    const bool saved = printed && output.good();
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, source);
    return saved;
}

bool IsAbove(HWND candidate, HWND reference) {
    if (!candidate || !reference || candidate == reference) return false;
    for (HWND window = GetTopWindow(nullptr); window;
         window = GetWindow(window, GW_HWNDNEXT)) {
        if (window == candidate) return true;
        if (window == reference) return false;
    }
    return false;
}

HWND DesktopHost() {
    struct Search { HWND host = nullptr; } search;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto& result = *reinterpret_cast<Search*>(parameter);
        if (FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr)) {
            result.host = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (search.host) return search.host;
    if (HWND progman = FindWindowW(L"Progman", nullptr)) return progman;
    return GetShellWindow();
}

bool ToggleDesktop() {
    INPUT input[4]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LWIN;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = 'D';
    input[2].type = INPUT_KEYBOARD;
    input[2].ki.wVk = 'D';
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].type = INPUT_KEYBOARD;
    input[3].ki.wVk = VK_LWIN;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) ==
           static_cast<UINT>(std::size(input));
}

int DpiFor(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return getDpi ? static_cast<int>(getDpi(window)) : 96;
}

bool NearlyWhite(DWORD pixel) {
    return (pixel & 0xff) >= 248 && ((pixel >> 8) & 0xff) >= 248 &&
           ((pixel >> 16) & 0xff) >= 248;
}

int FindThumbCenter(HWND slider) {
    RECT client{};
    GetClientRect(slider, &client);
    const int dpi = DpiFor(slider);
    const int inset = MulDiv(10, dpi, 96);
    const int y = client.bottom / 2;
    const int width = std::max(1L, client.right);
    HDC dc = GetDC(slider);
    HDC memory = dc ? CreateCompatibleDC(dc) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -1;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS,
                                               reinterpret_cast<void**>(&pixels), nullptr, 0)
                            : nullptr;
    if (!dc || !memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (dc) ReleaseDC(slider, dc);
        return -1;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, 1, dc, 0, y, SRCCOPY);
    std::vector<std::pair<int, int>> runs;
    int runStart = -1;
    for (int x = inset; x <= client.right - inset; ++x) {
        if (NearlyWhite(pixels[x])) {
            if (runStart < 0) runStart = x;
        } else if (runStart >= 0) {
            runs.emplace_back(runStart, x - 1);
            runStart = -1;
        }
    }
    if (runStart >= 0) runs.emplace_back(runStart, client.right - inset);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(slider, dc);
    const int minimumRun = std::max(6, MulDiv(8, dpi, 96));
    const int maximumRun = std::max(minimumRun, MulDiv(22, dpi, 96));
    auto best = runs.end();
    for (auto it = runs.begin(); it != runs.end(); ++it) {
        const int length = it->second - it->first + 1;
        if (length < minimumRun || length > maximumRun) continue;
        if (best == runs.end() || length > best->second - best->first + 1) best = it;
    }
    return best == runs.end() ? -1 : (best->first + best->second) / 2;
}

LPARAM MousePosition(int x, int y) {
    return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
}

bool ActivateProbeWindow(HWND window) {
    const HWND foreground = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground
        ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                          AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    if (attached) AttachThreadInput(currentThread, foregroundThread, FALSE);
    if (GetForegroundWindow() == window) return true;

    // A user-initiated Alt gesture grants the same foreground transition that
    // the production window receives from a real double-click. Keep Alt down
    // only around SetForegroundWindow so no menu mode remains active.
    INPUT alt[2]{};
    alt[0].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = VK_MENU;
    alt[1].type = INPUT_KEYBOARD;
    alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt[0], sizeof(INPUT));
    SetForegroundWindow(window);
    SetActiveWindow(window);
    SendInput(1, &alt[1], sizeof(INPUT));
    if (GetForegroundWindow() == window) return true;

    // Foreground-lock policy can still reject the API-only transition after
    // Show Desktop or another process has consumed the last user gesture.
    // A real click is the behavior being verified, so use SendInput as the
    // final activation path instead of granting the probe special privileges.
    RECT rect{};
    POINT cursor{};
    GetWindowRect(window, &rect);
    GetCursorPos(&cursor);
    // Make the synthetic click target unambiguous even when a full-screen or
    // maximized window currently owns the foreground. This temporary TOPMOST
    // state belongs only to the probe guard and is removed immediately.
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetCursorPos(rect.left + std::min(48L, std::max(8L, (rect.right - rect.left) / 2)),
                 rect.top + std::min(48L, std::max(8L, (rect.bottom - rect.top) / 2)));
    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetCursorPos(cursor.x, cursor.y);
    return GetForegroundWindow() == window;
}

HWND TintOwnedBy(DWORD processId, HWND container);

bool ProbeCenteredExpansion(DWORD processId, HWND console, HWND container) {
    TextControlSearch search{L"双击展开时居中，打开项目后自动收回"};
    EnumChildWindows(console, FindControlByText, reinterpret_cast<LPARAM>(&search));
    if (!search.match) {
        std::cout << "center_expand error=checkbox-not-found\n";
        return false;
    }
    if (SendMessageW(search.match, BM_GETCHECK, 0, 0) != BST_CHECKED)
        SendMessageW(search.match, BM_CLICK, 0, 0);

    RECT initial{};
    GetWindowRect(container, &initial);
    RECT client{};
    GetClientRect(container, &client);
    const int emptyX = client.right / 2;
    const int emptyY = client.bottom / 2;
    SendMessageW(container, WM_LBUTTONDBLCLK, MK_LBUTTON, MousePosition(emptyX, emptyY));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    RECT compact{};
    GetWindowRect(container, &compact);

    HWND guard = CreateWindowExW(0, L"STATIC", L"UiProbeCenterGuard",
                                 WS_OVERLAPPEDWINDOW, 20, 20, 420, 260,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!guard) {
        std::cout << "center_expand error=guard-create-failed\n";
        return false;
    }
    ShowWindow(guard, SW_SHOWNORMAL);
    SetWindowPos(guard, HWND_TOP, 20, 20, 420, 260,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));

    SendMessageW(container, WM_LBUTTONDBLCLK, MK_LBUTTON,
                 MousePosition((compact.right - compact.left) / 2,
                               (compact.bottom - compact.top) / 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    RECT expanded{};
    GetWindowRect(container, &expanded);
    HWND tint = TintOwnedBy(processId, container);
    const bool expandedContainerBelowApp = IsAbove(guard, container);
    const bool expandedTintBelowApp = tint && IsAbove(guard, tint);
    const bool expandedBelowApp = expandedContainerBelowApp && expandedTintBelowApp;
    const bool expandedContainerTopmost =
        (GetWindowLongPtrW(container, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool expandedTintTopmost = tint &&
        (GetWindowLongPtrW(tint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    HMONITOR monitor = MonitorFromRect(&compact, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int expectedX = info.rcWork.left +
        (info.rcWork.right - info.rcWork.left - (expanded.right - expanded.left)) / 2;
    const int expectedY = info.rcWork.top +
        (info.rcWork.bottom - info.rcWork.top - (expanded.bottom - expanded.top)) / 2;

    GetClientRect(container, &client);
    SendMessageW(container, WM_LBUTTONDBLCLK, MK_LBUTTON,
                 MousePosition(client.right / 2, client.bottom / 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    RECT restored{};
    GetWindowRect(container, &restored);
    const bool restoredBelowApp = tint && IsAbove(guard, container) &&
                                  IsAbove(guard, tint);
    const bool restoredTintTopmost = tint &&
        (GetWindowLongPtrW(tint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool centered = std::abs(expanded.left - expectedX) <= 1 &&
                          std::abs(expanded.top - expectedY) <= 1;
    const bool returned = restored.left == compact.left && restored.top == compact.top;
    const bool resized = compact.right - compact.left < initial.right - initial.left;
    std::cout << "center_expand centered=" << centered
              << " returned=" << returned
              << " compacted=" << resized
              << " expanded_below_app=" << expandedBelowApp
              << " expanded_container_below_app=" << expandedContainerBelowApp
              << " expanded_tint_below_app=" << expandedTintBelowApp
              << " expanded_container_topmost=" << expandedContainerTopmost
              << " expanded_tint_topmost=" << expandedTintTopmost
              << " restored_below_app=" << restoredBelowApp
              << " restored_tint_topmost=" << restoredTintTopmost
              << " compact=" << compact.left << ',' << compact.top
              << " expanded=" << expanded.left << ',' << expanded.top
              << " expected=" << expectedX << ',' << expectedY
              << " restored=" << restored.left << ',' << restored.top << "\n";

    // Leave the fixture normally expanded for the remaining probes.
    if (SendMessageW(search.match, BM_GETCHECK, 0, 0) == BST_CHECKED)
        SendMessageW(search.match, BM_CLICK, 0, 0);
    SendMessageW(container, WM_LBUTTONDBLCLK, MK_LBUTTON,
                 MousePosition((restored.right - restored.left) / 2,
                               (restored.bottom - restored.top) / 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::cout << "center_expand normal_tint_topmost="
              << (tint && (GetWindowLongPtrW(tint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
              << " normal_tint_below_app=" << (tint && IsAbove(guard, tint)) << "\n";
    DestroyWindow(guard);
    return centered && returned && resized && expandedBelowApp &&
           !expandedContainerTopmost && !expandedTintTopmost && restoredBelowApp;
}

bool ProbeCenteredExpansionSequence(DWORD processId) {
    const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (containers.size() != 3) {
        std::cout << "center_sequence error=expected-three-containers actual="
                  << containers.size() << "\n";
        return false;
    }
    HWND first = nullptr;
    HWND second = nullptr;
    HWND third = nullptr;
    for (HWND container : containers) {
        wchar_t title[128]{};
        GetWindowTextW(container, title, static_cast<int>(std::size(title)));
        if (wcscmp(title, L"First") == 0) first = container;
        else if (wcscmp(title, L"Second") == 0) second = container;
        else if (wcscmp(title, L"Third") == 0) third = container;
    }
    if (!first || !second || !third) {
        std::cout << "center_sequence error=container-not-found\n";
        return false;
    }

    const RECT firstHome = WindowRect(first);
    const RECT secondHome = WindowRect(second);
    const RECT thirdHome = WindowRect(third);
    const auto open = [](HWND container) {
        RECT client{};
        GetClientRect(container, &client);
        SendMessageW(container, WM_LBUTTONDBLCLK, MK_LBUTTON,
                     MousePosition(client.right / 2, client.bottom / 2));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    const auto compactAt = [](HWND container, const RECT& home) {
        const RECT current = WindowRect(container);
        return current.left == home.left && current.top == home.top &&
               current.right - current.left == home.right - home.left &&
               current.bottom - current.top == home.bottom - home.top;
    };
    const auto expanded = [](HWND container, const RECT& home) {
        const RECT current = WindowRect(container);
        return current.right - current.left > home.right - home.left &&
               current.bottom - current.top > home.bottom - home.top;
    };

    open(first);
    const bool firstOnly = expanded(first, firstHome) &&
                           compactAt(second, secondHome) && compactAt(third, thirdHome);
    open(second);
    const bool secondOnly = compactAt(first, firstHome) && expanded(second, secondHome) &&
                            compactAt(third, thirdHome);
    open(third);
    const bool thirdOnly = compactAt(first, firstHome) && compactAt(second, secondHome) &&
                           expanded(third, thirdHome);

    std::cout << "center_sequence first_only=" << firstOnly
              << " second_only=" << secondOnly
              << " third_only=" << thirdOnly << "\n";
    return firstOnly && secondOnly && thirdOnly;
}

bool DragContainerBy(HWND container, int dx, int dy) {
    RECT client{};
    GetClientRect(container, &client);
    const int x = client.right / 2;
    const int y = client.bottom / 2;
    SendMessageW(container, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(x, y));
    SendMessageW(container, WM_MOUSEMOVE, MK_LBUTTON, MousePosition(x + dx, y + dy));
    SendMessageW(container, WM_LBUTTONUP, 0, MousePosition(x + dx, y + dy));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return true;
}

bool ProbeContainerEdgeAlignment(DWORD processId) {
    const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (containers.size() != 2) {
        std::cout << "edge_align error=expected-two-containers actual="
                  << containers.size() << "\n";
        return false;
    }
    HWND anchor = nullptr;
    HWND moved = nullptr;
    for (HWND container : containers) {
        wchar_t title[128]{};
        GetWindowTextW(container, title, static_cast<int>(std::size(title)));
        if (wcscmp(title, L"Anchor") == 0) anchor = container;
        else if (wcscmp(title, L"Moved") == 0) moved = container;
    }
    if (!anchor || !moved) {
        std::cout << "edge_align error=container-not-found\n";
        return false;
    }

    SetWindowPos(anchor, nullptr, 320, 220, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const RECT anchorRect = WindowRect(anchor);
    const RECT movedRect = WindowRect(moved);
    const int movedWidth = movedRect.right - movedRect.left;
    const int movedHeight = movedRect.bottom - movedRect.top;

    // The cards are vertically near but do not overlap. Moving within the
    // magnetic threshold should align their left vertical edges only.
    SetWindowPos(moved, nullptr, anchorRect.left + 16, anchorRect.bottom + 18,
                 movedWidth, movedHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    DragContainerBy(moved, -8, 0);
    const RECT vertical = WindowRect(moved);
    const bool verticalEdgeAligned = vertical.left == anchorRect.left;

    // The cards are horizontally near but do not overlap. Apply the same rule
    // independently to their top horizontal edges.
    SetWindowPos(moved, nullptr, anchorRect.right + 18, anchorRect.top + 16,
                 movedWidth, movedHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    DragContainerBy(moved, 0, -8);
    const RECT horizontal = WindowRect(moved);
    const bool horizontalEdgeAligned = horizontal.top == anchorRect.top;

    std::cout << "edge_align vertical=" << verticalEdgeAligned
              << " horizontal=" << horizontalEdgeAligned
              << " anchor=" << anchorRect.left << ',' << anchorRect.top
              << " vertical_result=" << vertical.left << ',' << vertical.top
              << " horizontal_result=" << horizontal.left << ',' << horizontal.top
              << "\n";
    return verticalEdgeAligned && horizontalEdgeAligned;
}

bool ProbeHoverName(DWORD processId, const wchar_t* capturePath = nullptr) {
    const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (containers.size() != 1) {
        std::cout << "hover_name error=expected-one-container actual="
                  << containers.size() << "\n";
        return false;
    }
    const HWND container = containers.front();
    const int dpi = DpiFor(container);
    // ui-motion.json places its first half-size item at half-grid (2, 0).
    const int x = MulDiv(10 + 96 + 24, dpi, 96);
    const int y = MulDiv(28 + 24, dpi, 96);
    SendMessageW(container, WM_MOUSEMOVE, 0, MousePosition(x, y));
    std::this_thread::sleep_for(std::chrono::milliseconds(260));

    const auto hovers = WindowsFor(processId, L"DesktopOrganizer.HoverName");
    const HWND tint = TintOwnedBy(processId, container);
    const HWND hover = hovers.size() == 1 ? hovers.front() : nullptr;
    wchar_t title[256]{};
    if (hover) GetWindowTextW(hover, title, static_cast<int>(std::size(title)));
    const bool visible = hover && IsWindowVisible(hover);
    const bool ownedByTint = hover && GetWindow(hover, GW_OWNER) == tint;
    const bool aboveContainer = hover && tint && IsAbove(hover, tint) &&
                                IsAbove(hover, container);
    const bool named = wcscmp(title, L"缩小后仍需显示的文件夹名称") == 0;
    const bool captured = !capturePath || SaveWindowSnapshot(hover, capturePath);

    SendMessageW(container, WM_MOUSELEAVE, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const bool dismissed = WindowsFor(processId, L"DesktopOrganizer.HoverName").empty();
    std::wcout << L"hover_name visible=" << visible
               << L" owned_by_tint=" << ownedByTint
               << L" above_container=" << aboveContainer
               << L" named=" << named
               << L" captured=" << captured
               << L" dismissed=" << dismissed
               << L" text=" << title << L"\n";
    return visible && ownedByTint && aboveContainer && named && captured && dismissed;
}

bool ProbeCtrlBypassesSnap(DWORD processId) {
    const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (containers.size() != 1) {
        std::cout << "ctrl_snap error=expected-one-container actual="
                  << containers.size() << "\n";
        return false;
    }
    const HWND container = containers.front();
    const RECT before = WindowRect(container);
    RECT client{};
    GetClientRect(container, &client);
    const int x = client.right / 2;
    const int y = client.bottom / 2;
    constexpr int kMoveX = 31;
    constexpr int kMoveY = 19;
    SendMessageW(container, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(x, y));
    SendMessageW(container, WM_MOUSEMOVE, MK_LBUTTON | MK_CONTROL,
                 MousePosition(x + kMoveX, y + kMoveY));
    SendMessageW(container, WM_LBUTTONUP, MK_CONTROL,
                 MousePosition(x + kMoveX, y + kMoveY));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const RECT after = WindowRect(container);
    const bool keptRawPosition = std::abs(after.left - (before.left + kMoveX)) <= 1 &&
                                 std::abs(after.top - (before.top + kMoveY)) <= 1;
    std::cout << "ctrl_snap bypassed=" << keptRawPosition
              << " before=" << before.left << ',' << before.top
              << " after=" << after.left << ',' << after.top << "\n";
    return keptRawPosition;
}

bool ProbeLeftResize(HWND container) {
    RECT initial{};
    GetWindowRect(container, &initial);
    RECT client{};
    GetClientRect(container, &client);
    const int startX = 2;
    const int y = client.bottom / 2;
    SendMessageW(container, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(startX, y));
    for (int step = 1; step <= 48; ++step) {
        const int x = startX + static_cast<int>(std::lround(100.0 * step / 48.0));
        SendMessageW(container, WM_MOUSEMOVE, MK_LBUTTON, MousePosition(x, y));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    SendMessageW(container, WM_LBUTTONUP, 0, MousePosition(startX + 100, y));
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    RECT resized{};
    GetWindowRect(container, &resized);
    const int widthDelta = (initial.right - initial.left) - (resized.right - resized.left);
    const bool rightStayed = std::abs(resized.right - initial.right) <= 1;
    const bool removedLeadingCell = widthDelta >= 90 && resized.left > initial.left;
    std::cout << "left_resize removed_leading_cell=" << removedLeadingCell
              << " right_stayed=" << rightStayed
              << " initial=" << initial.left << ',' << initial.right
              << " resized=" << resized.left << ',' << resized.right
              << " width_delta=" << widthDelta << "\n";
    return removedLeadingCell && rightStayed;
}

bool ProbeDesktopBand(DWORD processId, HWND container) {
    const wchar_t* guardClass = L"DesktopOrganizer.UiProbeGuard";
    WNDCLASSW registration{};
    registration.lpfnWndProc = DefWindowProcW;
    registration.hInstance = GetModuleHandleW(nullptr);
    registration.lpszClassName = guardClass;
    RegisterClassW(&registration);
    HWND guard = CreateWindowExW(0, guardClass, L"UiProbeGuard", WS_OVERLAPPEDWINDOW,
                                 20, 20, 420, 260, nullptr, nullptr,
                                 registration.hInstance, nullptr);
    if (!guard) {
        std::cout << "desktop_band error=guard-create-failed\n";
        return false;
    }
    ShowWindow(guard, SW_SHOWNORMAL);
    SetWindowPos(guard, HWND_TOP, 20, 20, 420, 260, SWP_SHOWWINDOW);

    const RECT before = WindowRect(container);
    const auto messages = WindowsFor(processId, L"DesktopOrganizer.Message");
    const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (messages.size() == 1 && taskbarCreated)
        SendMessageW(messages.front(), taskbarCreated, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const RECT after = WindowRect(container);
    const auto tints = WindowsFor(processId, L"DesktopOrganizer.Tint");
    HWND desktop = DesktopHost();
    const bool positionStable = EqualRect(&before, &after) != FALSE;
    const bool containerInBand = desktop && IsAbove(container, desktop) && IsAbove(guard, container);
    bool tintInBand = tints.size() == 1 && IsAbove(tints.front(), container) &&
                      IsAbove(guard, tints.front());
    bool tintAligned = false;
    if (tints.size() == 1) {
        const RECT tint = WindowRect(tints.front());
        tintAligned = EqualRect(&after, &tint) != FALSE;
    }
    bool showDesktopStable = true;
    int hiddenOrBehindCount = 0;
    int restoredAboveGuardCount = 0;
    int containerAboveGuardCount = 0;
    int tintAboveGuardCount = 0;
    for (int cycle = 0; cycle < 4; ++cycle) {
        if (!ToggleDesktop()) {
            showDesktopStable = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        desktop = DesktopHost();
        const bool visibleOnDesktop = IsWindowVisible(container) &&
            tints.size() == 1 && IsWindowVisible(tints.front()) &&
            desktop && IsAbove(container, desktop) && IsAbove(tints.front(), desktop);
        if (!visibleOnDesktop) ++hiddenOrBehindCount;

        ToggleDesktop();
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        ShowWindow(guard, SW_RESTORE);
        SetWindowPos(guard, HWND_TOP, 20, 20, 420, 260,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(guard);
        // Shell hook delivery and the app's settling repair are asynchronous.
        // The user-visible contract is that the desktop card is corrected
        // before the 180 ms Show Desktop animation finishes, not within the
        // same instruction as an unrelated app's SetWindowPos call.
        std::this_thread::sleep_for(std::chrono::milliseconds(140));
        const bool containerBelowApp = IsAbove(guard, container);
        const bool tintBelowApp = tints.size() == 1 && IsAbove(guard, tints.front());
        if (!containerBelowApp) ++containerAboveGuardCount;
        if (!tintBelowApp) ++tintAboveGuardCount;
        const bool restoredBelowApp = containerBelowApp && tintBelowApp;
        if (!restoredBelowApp) ++restoredAboveGuardCount;
        const RECT cycled = WindowRect(container);
        if (!EqualRect(&before, &cycled)) showDesktopStable = false;
    }
    showDesktopStable = showDesktopStable && hiddenOrBehindCount == 0 &&
                        restoredAboveGuardCount == 0;

    std::cout << "desktop_band position_stable=" << positionStable
              << " container_in_band=" << containerInBand
              << " tint_in_band=" << tintInBand
              << " tint_aligned=" << tintAligned
              << " show_desktop_stable=" << showDesktopStable
              << " hidden_or_behind=" << hiddenOrBehindCount
              << " restored_above_app=" << restoredAboveGuardCount
              << " container_above_app=" << containerAboveGuardCount
              << " tint_above_app=" << tintAboveGuardCount
              << " desktop=" << reinterpret_cast<std::uintptr_t>(desktop)
              << " guard=" << reinterpret_cast<std::uintptr_t>(guard) << "\n";
    DestroyWindow(guard);
    UnregisterClassW(guardClass, registration.hInstance);
    return positionStable && containerInBand && tintInBand && tintAligned &&
           showDesktopStable;
}

HWND TintOwnedBy(DWORD processId, HWND container) {
    const auto tints = WindowsFor(processId, L"DesktopOrganizer.Tint");
    for (HWND tint : tints)
        if (GetWindow(tint, GW_OWNER) == container) return tint;
    return nullptr;
}

bool ProbeAllDesktopVisibility(DWORD processId) {
    const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (containers.empty()) {
        std::cout << "desktop_visibility error=no-containers\n";
        return false;
    }
    if (HWND tint = TintOwnedBy(processId, containers.front())) {
        RECT client{};
        GetClientRect(tint, &client);
        const LPARAM center = MousePosition(client.right / 2, client.bottom / 2);
        SendMessageW(tint, WM_LBUTTONDOWN, MK_LBUTTON, center);
        SendMessageW(tint, WM_LBUTTONUP, 0, center);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    if (!ToggleDesktop()) {
        std::cout << "desktop_visibility error=toggle-failed\n";
        return false;
    }

    bool passed = true;
    const int delays[]{40, 80, 180, 400, 800};
    int elapsed = 0;
    for (const int delay : delays) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay - elapsed));
        elapsed = delay;
        HWND desktop = DesktopHost();
        int invalid = 0;
        int invisible = 0;
        int behindDesktop = 0;
        int hitByOtherProcess = 0;
        wchar_t firstHitClass[128]{};
        for (HWND container : containers) {
            HWND tint = TintOwnedBy(processId, container);
            RECT rect = WindowRect(container);
            POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
            HWND hit = WindowFromPoint(center);
            DWORD hitProcess = 0;
            GetWindowThreadProcessId(hit, &hitProcess);
            const bool visible = tint && IsWindowVisible(container) && IsWindowVisible(tint) &&
                                 !IsIconic(container) && !IsIconic(tint);
            const bool aboveDesktop = desktop && IsAbove(container, desktop) &&
                                      IsAbove(tint, desktop);
            const bool hitOrganizer = hitProcess == processId;
            if (!visible) ++invisible;
            if (!aboveDesktop) ++behindDesktop;
            if (!hitOrganizer) {
                ++hitByOtherProcess;
                if (!firstHitClass[0])
                    GetClassNameW(hit, firstHitClass, static_cast<int>(std::size(firstHitClass)));
            }
            if (!visible || !aboveDesktop) ++invalid;
        }
        std::cout << "desktop_visibility elapsed_ms=" << elapsed
                  << " invalid=" << invalid
                  << " invisible=" << invisible
                  << " behind_desktop=" << behindDesktop
                  << " hit_other=" << hitByOtherProcess
                  << " first_hit_class=";
        std::wcout << firstHitClass;
        std::cout
                  << " total=" << containers.size() << "\n";
        if (invalid != 0) passed = false;
    }

    const bool restored = ToggleDesktop();
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    return passed && restored;
}

bool ProbeDraggedContainerZOrder(DWORD processId) {
    const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (containers.size() != 2) {
        std::cout << "drag_z error=expected-two-containers actual="
                  << containers.size() << "\n";
        return false;
    }
    HWND dragged = nullptr;
    HWND other = nullptr;
    for (HWND container : containers) {
        wchar_t title[128]{};
        GetWindowTextW(container, title, static_cast<int>(std::size(title)));
        if (wcscmp(title, L"Dragged") == 0) dragged = container;
        else if (wcscmp(title, L"Other") == 0) other = container;
    }
    HWND draggedTint = TintOwnedBy(processId, dragged);
    HWND otherTint = TintOwnedBy(processId, other);
    if (!dragged || !other || !draggedTint || !otherTint) {
        std::cout << "drag_z error=surface-not-found\n";
        return false;
    }

    HWND guard = CreateWindowExW(0, L"STATIC", L"UiProbeDragGuard",
                                 WS_OVERLAPPEDWINDOW, 20, 20, 360, 220,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(guard, SW_SHOWNORMAL);
    SetWindowPos(guard, HWND_TOP, 20, 20, 360, 220, SWP_SHOWWINDOW);
    SetForegroundWindow(guard);
    std::this_thread::sleep_for(std::chrono::milliseconds(140));

    RECT client{};
    GetClientRect(dragged, &client);
    const int x = client.right / 2;
    const int y = std::max(12L, client.bottom / 2);
    // The glass tint is both the visible surface and the real hit-test target;
    // send messages there instead of directly to its transparent owner.
    SendMessageW(draggedTint, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(x, y));
    SendMessageW(draggedTint, WM_MOUSEMOVE, MK_LBUTTON, MousePosition(x + 24, y + 18));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool duringContainerAboveSibling = IsAbove(dragged, other);
    const bool duringTintAboveSibling = IsAbove(draggedTint, otherTint);
    const bool duringAboveSibling = duringTintAboveSibling && duringContainerAboveSibling;
    const bool duringTintAboveApp = IsAbove(draggedTint, guard);
    const bool duringTintTopmost =
        (GetWindowLongPtrW(draggedTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

    SendMessageW(draggedTint, WM_LBUTTONUP, 0, MousePosition(x + 24, y + 18));
    std::this_thread::sleep_for(std::chrono::milliseconds(140));
    const bool afterContainerAboveSibling = IsAbove(dragged, other);
    const bool afterTintAboveSibling = IsAbove(draggedTint, otherTint);
    const bool afterAboveSibling = afterTintAboveSibling && afterContainerAboveSibling;
    const bool afterTintAboveApp = IsAbove(draggedTint, guard);
    const bool afterTintTopmost =
        (GetWindowLongPtrW(draggedTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

    // Starting work in another container transfers the temporary topmost
    // session instead of leaving every previously touched group above apps.
    RECT otherClient{};
    GetClientRect(other, &otherClient);
    const int otherX = otherClient.right / 2;
    const int otherY = std::max(12L, otherClient.bottom / 2);
    SendMessageW(otherTint, WM_LBUTTONDOWN, MK_LBUTTON,
                 MousePosition(otherX, otherY));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool switchedOtherTopmost =
        (GetWindowLongPtrW(otherTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool switchedDraggedTopmost =
        (GetWindowLongPtrW(draggedTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool switchedOtherAboveDragged = IsAbove(otherTint, draggedTint);
    SendMessageW(otherTint, WM_LBUTTONUP, 0, MousePosition(otherX, otherY));

    // A real click on the foreground application must end the container's
    // temporary work session even when that application was already focused.
    RECT guardRect{};
    GetWindowRect(guard, &guardRect);
    POINT priorCursor{};
    GetCursorPos(&priorCursor);
    SetCursorPos(guardRect.left + 48, guardRect.top + 48);
    INPUT appClick[2]{};
    appClick[0].type = INPUT_MOUSE;
    appClick[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    appClick[1].type = INPUT_MOUSE;
    appClick[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(static_cast<UINT>(std::size(appClick)), appClick, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SetCursorPos(priorCursor.x, priorCursor.y);
    const bool clickRestoredBelowApp =
        IsAbove(guard, draggedTint) && IsAbove(guard, dragged) &&
        IsAbove(guard, otherTint) && IsAbove(guard, other);
    const bool clickTintTopmost =
        (GetWindowLongPtrW(draggedTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 ||
        (GetWindowLongPtrW(otherTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

    // Exercise the exact cleanup branch used when Alt+Tab or a system gesture
    // transfers capture. A process cannot take another process's capture in a
    // deterministic probe, so dispatch the same User32 notification directly.
    SendMessageW(draggedTint, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(x, y));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool interruptionStartedTopmost =
        (GetWindowLongPtrW(draggedTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    SendMessageW(dragged, WM_CAPTURECHANGED, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const bool interruptionRestoredBelowApp =
        IsAbove(guard, draggedTint) && IsAbove(guard, dragged);
    const bool interruptionTintTopmost =
        (GetWindowLongPtrW(draggedTint, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    std::cout << "drag_z during_above_sibling=" << duringAboveSibling
              << " during_container_above_sibling=" << duringContainerAboveSibling
              << " during_tint_above_sibling=" << duringTintAboveSibling
              << " during_visible_above_app=" << duringTintAboveApp
              << " during_tint_above_app=" << duringTintAboveApp
              << " during_tint_topmost=" << duringTintTopmost
              << " after_above_sibling=" << afterAboveSibling
              << " after_container_above_sibling=" << afterContainerAboveSibling
              << " after_tint_above_sibling=" << afterTintAboveSibling
              << " after_visible_above_app=" << afterTintAboveApp
              << " after_tint_above_app=" << afterTintAboveApp
              << " after_tint_topmost=" << afterTintTopmost
              << " switched_other_topmost=" << switchedOtherTopmost
              << " switched_dragged_topmost=" << switchedDraggedTopmost
              << " switched_other_above_dragged=" << switchedOtherAboveDragged
              << " click_restored_below_app=" << clickRestoredBelowApp
              << " click_tint_topmost=" << clickTintTopmost
              << " interruption_started_topmost=" << interruptionStartedTopmost
              << " interruption_restored_below_app=" << interruptionRestoredBelowApp
              << " interruption_tint_topmost=" << interruptionTintTopmost << "\n";
    DestroyWindow(guard);
    return duringAboveSibling && duringTintAboveApp && duringTintTopmost &&
           afterAboveSibling && afterTintAboveApp && afterTintTopmost &&
           switchedOtherTopmost && !switchedDraggedTopmost &&
           switchedOtherAboveDragged &&
           clickRestoredBelowApp && !clickTintTopmost &&
           interruptionStartedTopmost && interruptionRestoredBelowApp &&
           !interruptionTintTopmost;
}

void ProbeSlider(HWND slider, const char* name) {
    RECT client{};
    GetClientRect(slider, &client);
    const int dpi = DpiFor(slider);
    const int inset = MulDiv(10, dpi, 96);
    const int y = client.bottom / 2;
    RedrawWindow(slider, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    const int start = FindThumbCenter(slider);
    if (start < 0) {
        std::cout << "slider " << name << " error=thumb-not-found\n";
        return;
    }
    int travel = std::min(MulDiv(150, dpi, 96),
                          static_cast<int>(client.right) - inset - start - 2);
    if (travel < MulDiv(60, dpi, 96))
        travel = -std::min(MulDiv(150, dpi, 96), start - inset - 2);
    constexpr int steps = 96;
    constexpr double durationMs = 800.0;
    std::vector<int> rendered;
    std::vector<double> timestamps;
    std::vector<double> handlers;
    rendered.reserve(steps + 1);
    timestamps.reserve(steps + 1);
    SendMessageW(slider, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(start, y));
    const auto begun = Clock::now();
    for (int step = 0; step <= steps; ++step) {
        const int requested = start + static_cast<int>(std::lround(
            static_cast<double>(travel) * step / steps));
        const auto before = Clock::now();
        SendMessageW(slider, WM_MOUSEMOVE, MK_LBUTTON, MousePosition(requested, y));
        const auto after = Clock::now();
        handlers.push_back(Milliseconds(before, after));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        rendered.push_back(FindThumbCenter(slider));
        timestamps.push_back(Milliseconds(begun, Clock::now()));
        const auto deadline = begun + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(durationMs * (step + 1) / steps));
        std::this_thread::sleep_until(deadline);
    }
    SendMessageW(slider, WM_MOUSEMOVE, MK_LBUTTON, MousePosition(start, y));
    SendMessageW(slider, WM_LBUTTONUP, 0, MousePosition(start, y));

    std::set<int> unique(rendered.begin(), rendered.end());
    double longestFrozen = 0.0;
    double frozenFrom = timestamps.empty() ? 0.0 : timestamps.front();
    int maximumJump = 0;
    for (size_t index = 1; index < rendered.size(); ++index) {
        maximumJump = std::max(maximumJump, std::abs(rendered[index] - rendered[index - 1]));
        if (rendered[index] != rendered[index - 1]) {
            longestFrozen = std::max(longestFrozen, timestamps[index] - frozenFrom);
            frozenFrom = timestamps[index];
        }
    }
    if (!timestamps.empty()) longestFrozen = std::max(longestFrozen, timestamps.back() - frozenFrom);
    std::sort(handlers.begin(), handlers.end());
    const size_t p95Index = handlers.empty() ? 0 : std::min(
        handlers.size() - 1, static_cast<size_t>(std::floor(handlers.size() * 0.95)));
    std::cout << "slider " << name
              << " rendered_positions=" << unique.size()
              << " longest_frozen_ms=" << longestFrozen
              << " max_jump_px=" << maximumJump
              << " handler_p95_ms=" << (handlers.empty() ? 0.0 : handlers[p95Index])
              << " handler_max_ms=" << (handlers.empty() ? 0.0 : handlers.back()) << "\n";
}

bool ProbeResize(DWORD processId, HWND container) {
    RECT client{};
    GetClientRect(container, &client);
    const int startX = client.right - 2;
    const int y = client.bottom / 2;
    const int initialWidth = WindowWidth(container);
    std::vector<std::pair<double, int>> previewFrames;
    int previousPreviewWidth = -1;
    const auto dragBegun = Clock::now();
    SendMessageW(container, WM_LBUTTONDOWN, MK_LBUTTON, MousePosition(startX, y));
    // Cross several snapped grid thresholds slowly enough to observe whether
    // the destination sheet jumps a whole cell or supplies intermediate frames.
    for (int step = 1; step <= 96; ++step) {
        const int x = startX + static_cast<int>(std::lround(144.0 * step / 96.0));
        SendMessageW(container, WM_MOUSEMOVE, MK_LBUTTON, MousePosition(x, y));
        const auto previews = WindowsFor(
            processId, L"Static", L"DesktopOrganizer.ResizePreview");
        const int previewWidth = previews.size() == 1 ? WindowWidth(previews.front()) : -1;
        if (previewWidth > 0 && previewWidth != previousPreviewWidth) {
            previewFrames.emplace_back(Milliseconds(dragBegun, Clock::now()), previewWidth);
            previousPreviewWidth = previewWidth;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const int dragWidth = WindowWidth(container);
    std::set<int> distinctPreviewWidths;
    int previewMaximumJump = 0;
    double previewMaximumInterval = 0.0;
    for (size_t index = 0; index < previewFrames.size(); ++index) {
        distinctPreviewWidths.insert(previewFrames[index].second);
        if (index == 0) continue;
        previewMaximumJump = std::max(
            previewMaximumJump,
            std::abs(previewFrames[index].second - previewFrames[index - 1].second));
        previewMaximumInterval = std::max(
            previewMaximumInterval,
            previewFrames[index].first - previewFrames[index - 1].first);
    }
    std::cout << "resize_preview distinct_widths=" << distinctPreviewWidths.size()
              << " changed_frames=" << previewFrames.size()
              << " max_interval_ms=" << previewMaximumInterval
              << " max_jump_px=" << previewMaximumJump
              << " container_width_during_drag=" << dragWidth << "\n";
    std::cout << "resize_preview_frames";
    for (const auto& [timestamp, width] : previewFrames)
        std::cout << ' ' << static_cast<int>(std::lround(timestamp)) << ':' << width;
    std::cout << "\n";
    SendMessageW(container, WM_LBUTTONUP, 0, MousePosition(startX + 144, y));
    auto animationWindows = WindowsFor(processId, L"Static", L"DesktopOrganizer.ResizeAnimation");
    HWND animation = animationWindows.size() == 1 ? animationWindows.front() : nullptr;
    if (!animation) {
        WindowSearch all{processId, L"", L""};
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
            auto& result = *reinterpret_cast<WindowSearch*>(parameter);
            DWORD ownerProcess = 0;
            GetWindowThreadProcessId(window, &ownerProcess);
            if (ownerProcess != result.processId) return TRUE;
            wchar_t className[128]{}, title[256]{};
            GetClassNameW(window, className, static_cast<int>(std::size(className)));
            GetWindowTextW(window, title, static_cast<int>(std::size(title)));
            std::wcerr << L"probe-window class=" << className << L" title=" << title << L"\n";
            return TRUE;
        }, reinterpret_cast<LPARAM>(&all));
    }

    const auto begun = Clock::now();
    std::vector<std::pair<double, int>> frames;
    int previous = -1;
    while (Milliseconds(begun, Clock::now()) < 350.0) {
        int width = animation ? WindowWidth(animation) : -1;
        if (width < 0) width = WindowWidth(container);
        if (width != previous) {
            frames.emplace_back(Milliseconds(begun, Clock::now()), width);
            previous = width;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    double maximumInterval = 0.0;
    int maximumJump = 0;
    for (size_t index = 1; index < frames.size(); ++index) {
        maximumInterval = std::max(maximumInterval, frames[index].first - frames[index - 1].first);
        maximumJump = std::max(maximumJump, std::abs(frames[index].second - frames[index - 1].second));
    }
    std::cout << "resize initial_width=" << initialWidth
              << " drag_width=" << dragWidth
              << " final_width=" << (frames.empty() ? -1 : frames.back().second)
              << " distinct_frames=" << frames.size()
              << " max_interval_ms=" << maximumInterval
              << " max_jump_px=" << maximumJump << "\n";
    std::cout << "resize_frames";
    for (const auto& [timestamp, width] : frames)
        std::cout << ' ' << static_cast<int>(std::lround(timestamp)) << ':' << width;
    std::cout << "\n";
    // A snapped half-cell is about 48 px in this fixture. The former direct
    // SetWindowPos implementation produced only three widths and 48 px jumps.
    // Require several actual intermediate frames without moving the real card.
    return distinctPreviewWidths.size() >= 8 && previewMaximumJump < 24 &&
           dragWidth == initialWidth;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && wcscmp(argv[1], L"--desktop-visibility") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        return ProbeAllDesktopVisibility(processId) ? 0 : 12;
    }
    if (argc == 3 && wcscmp(argv[1], L"--drag-z") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        return ProbeDraggedContainerZOrder(processId) ? 0 : 6;
    }
    if (argc == 3 && wcscmp(argv[1], L"--center-z") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        const auto consoles = WindowsFor(processId, L"DesktopOrganizer.Console");
        const auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
        if (consoles.size() != 1 || containers.size() != 1) return 3;
        return ProbeCenteredExpansion(processId, consoles.front(), containers.front()) ? 0 : 7;
    }
    if (argc == 3 && wcscmp(argv[1], L"--center-sequence") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        return ProbeCenteredExpansionSequence(processId) ? 0 : 11;
    }
    if (argc == 3 && wcscmp(argv[1], L"--align") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        return ProbeContainerEdgeAlignment(processId) ? 0 : 8;
    }
    if ((argc == 3 || argc == 4) && wcscmp(argv[1], L"--hover-name") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        return ProbeHoverName(processId, argc == 4 ? argv[3] : nullptr) ? 0 : 9;
    }
    if (argc == 3 && wcscmp(argv[1], L"--ctrl-no-snap") == 0) {
        const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        return ProbeCtrlBypassesSnap(processId) ? 0 : 10;
    }
    if (argc != 2) {
        std::wcerr << L"usage: UiMotionProbe <process-id>\n";
        return 2;
    }
    const DWORD processId = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    auto consoles = WindowsFor(processId, L"DesktopOrganizer.Console");
    auto containers = WindowsFor(processId, L"DesktopOrganizer.Container");
    if (consoles.size() != 1 || containers.size() != 1) {
        std::wcerr << L"expected one console and one container, got "
                   << consoles.size() << L" and " << containers.size() << L"\n";
        return 3;
    }
    std::vector<HWND> sliders;
    EnumChildWindows(consoles.front(), CollectSliders, reinterpret_cast<LPARAM>(&sliders));
    std::sort(sliders.begin(), sliders.end(), [](HWND left, HWND right) {
        RECT a{}, b{};
        GetWindowRect(left, &a);
        GetWindowRect(right, &b);
        return a.top < b.top;
    });
    if (sliders.size() < 3) {
        std::wcerr << L"expected three sliders, got " << sliders.size() << L"\n";
        return 4;
    }
    const char* names[]{"opacity", "blur", "corner"};
    for (size_t index = 0; index < 3; ++index) {
        ProbeSlider(sliders[index], names[index]);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool centerPassed = ProbeCenteredExpansion(processId, consoles.front(), containers.front());
    const bool leftResizePassed = ProbeLeftResize(containers.front());
    const bool resizePassed = ProbeResize(processId, containers.front());
    const bool desktopBandPassed = ProbeDesktopBand(processId, containers.front());
    return centerPassed && leftResizePassed && desktopBandPassed && resizePassed ? 0 : 5;
}
