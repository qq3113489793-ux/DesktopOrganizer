#include "config.h"
#include "desktop_anchor.h"
#include "desktop_capture_target.h"
#include "desktop_grid_geometry.h"
#include "diagnostic_log.h"
#include "icon_image.h"
#include "interaction_motion.h"
#include "layout.h"
#include "live_desktop_capture.h"
#include "path_io.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#ifdef __MINGW32__
#include <initguid.h>
#endif
#include <commoncontrols.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <timeapi.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <new>
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
#include <fstream>
#endif
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr wchar_t kContainerClass[] = L"DesktopOrganizer.Container";
constexpr wchar_t kMessageClass[] = L"DesktopOrganizer.Message";
constexpr wchar_t kRenameClass[] = L"DesktopOrganizer.Rename";
constexpr wchar_t kSettingsClass[] = L"DesktopOrganizer.Settings";
constexpr wchar_t kConsoleClass[] = L"DesktopOrganizer.Console";
constexpr wchar_t kContentClass[] = L"DesktopOrganizer.Content";
constexpr wchar_t kTintClass[] = L"DesktopOrganizer.Tint";
constexpr wchar_t kDragClass[] = L"DesktopOrganizer.DragGhost";
constexpr wchar_t kHoverClass[] = L"DesktopOrganizer.HoverName";
constexpr wchar_t kSliderClass[] = L"DesktopOrganizer.Slider";
constexpr wchar_t kDesktopMenuKey[] =
    L"Software\\Classes\\DesktopBackground\\Shell\\DesktopOrganizer.NewGroup";
constexpr wchar_t kDesktopConsoleMenuKey[] =
    L"Software\\Classes\\DesktopBackground\\Shell\\DesktopOrganizer.OpenConsole";
constexpr ULONG_PTR kCopyDataNewGroup = 0x444F4E47;
constexpr ULONG_PTR kCopyDataOpenConsole = 0x444F434E;

// Payload for the "--new-group" hand-off. The cookie lets the receiver drop
// duplicate deliveries: a timed-out SendMessageTimeout still leaves the
// message queued at the main instance, so an impatient retry would otherwise
// create the group twice.
struct NewGroupRequest {
    POINT position;
    DWORD cookie;
};
constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kDissolveContainer = WM_APP + 2;
constexpr UINT kLiveBackdropMessage = WM_APP + 3;
constexpr UINT kDismissInteractionMessage = WM_APP + 4;
constexpr UINT_PTR kDesktopPlacementTimer = 5;
constexpr UINT_PTR kSaveRetryTimer = 8;
constexpr UINT_PTR kNewGroupInboxTimer = 9;
constexpr UINT_PTR kDesktopLayerRepairTimer = 11;
constexpr UINT_PTR kDesktopLayerWatchdogTimer = 12;
constexpr int kResizeBorderDip = 7;
constexpr int kResizeCornerDip = 16;
constexpr int kPaddingDip = 10;
constexpr int kTitleDip = 28;
constexpr int kSnapDip = 12;
constexpr UINT kHoverDelayMs = 180;
constexpr UINT kHoverVisualDelayMs = 36;
// WM_TIMER delivery commonly lands one scheduler quantum late under live
// wallpaper capture. Request 8 ms so interactive work still publishes near
// the display's 16 ms cadence instead of falling to roughly 30 FPS.
constexpr UINT kInteractiveFrameMs = 8;
// Slider thumbs repaint directly from pointer messages. The heavier glass
// preview is capped at display cadence so it cannot monopolize the UI thread.
constexpr UINT kStylePreviewFrameMs = 16;
constexpr double kRectAnimationDurationMs = 190.0;
// The snapped destination is deliberately quicker than the committed card:
// it should feel attached to the pointer while still gliding between grid
// steps instead of teleporting by half an icon cell.
constexpr double kResizePreviewAnimationDurationMs = 112.0;
constexpr UINT_PTR kResizePreviewTimer = 10;
constexpr UINT kSliderSetRange = WM_APP + 40;
constexpr UINT kSliderSetPosition = WM_APP + 41;
constexpr UINT kSliderGetPosition = WM_APP + 42;
// Cached pixels are premultiplied BGRA DWORDs; eight million pixels cap the
// process-wide cache at roughly 32 MiB even when many groups are present.
constexpr size_t kGlassBaseCachePixelBudget = 8u * 1024u * 1024u;
constexpr unsigned long long kMaxWallpaperDecodeBytes = 128ull * 1024ull * 1024ull;

bool IsOrganizerDesktopSurfaceClass(HWND window) {
    if (!window) return false;
    wchar_t className[128]{};
    if (!GetClassNameW(window, className, static_cast<int>(std::size(className))))
        return false;
    return wcscmp(className, kContainerClass) == 0 ||
           wcscmp(className, kTintClass) == 0 ||
           wcscmp(className, kContentClass) == 0;
}

bool IsOrganizerDesktopSurface(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId() &&
           IsOrganizerDesktopSurfaceClass(window);
}

HWND DesktopBandInsertAfter(HWND desktopHost) {
    if (!desktopHost || !IsWindow(desktopHost)) return HWND_BOTTOM;
    // Insert immediately above Explorer's desktop surface and below the
    // nearest ordinary application. Skip our own surfaces so repeated repairs
    // keep the whole organizer stack in one stable desktop band.
    HWND aboveDesktop = GetWindow(desktopHost, GW_HWNDPREV);
    while (aboveDesktop && IsOrganizerDesktopSurfaceClass(aboveDesktop))
        aboveDesktop = GetWindow(aboveDesktop, GW_HWNDPREV);
    return aboveDesktop ? aboveDesktop : HWND_TOP;
}

bool PrecedesInTopLevelZOrder(HWND window, HWND boundary) {
    if (!window || !boundary || !IsWindow(window) || !IsWindow(boundary)) return false;
    for (HWND candidate = GetTopWindow(nullptr); candidate;
         candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (candidate == window) return true;
        if (candidate == boundary) return false;
    }
    return false;
}

enum MenuId : UINT {
    MenuNew = 100,
    MenuRename,
    MenuLock,
    MenuShowTitle,
    MenuShowBorder,
    MenuAppearance,
    MenuDissolve,
    MenuCollapse,
    MenuOpenItem,
    MenuOpenLocation,
    MenuRemoveItem,
    MenuCopyPath,
    MenuSizeSmall = 120,
    MenuSizeNormal,
    MenuSizeLarge,
    MenuUnifiedSizeSmall = 130,
    MenuUnifiedSizeNormal,
    MenuUnifiedSizeLarge,
    MenuOpacity45 = 140,
    MenuOpacity65,
    MenuOpacity80,
    MenuOpacity95,
    MenuBlurOff = 160,
    MenuBlurLight,
    MenuBlurStrong,
    MenuCorner0 = 180,
    MenuCorner12,
    MenuCorner24,
    MenuShellCut = 190,
    MenuShellCopy,
    MenuShellPaste,
    MenuShellDelete,
    MenuShellProperties,
    MenuRenameFilesystem,
    TrayNew = 220,
    TrayShowAll,
    TrayHideAll,
    TrayStartup,
    TrayExit,
    TrayConsole,
    MenuConsole,
    MenuSnapGrid,
    MenuPushIcons,
    SettingOpacity = 300,
    SettingBlur,
    SettingCorner,
    SettingTitle,
    SettingBorder,
    SettingLock,
    SettingTintAuto,
    SettingTintDark,
    SettingTintLight,
    SettingTintCustom,
    SettingTintColor,
    SettingTextColor,
    ConsoleOpacity = 400,
    ConsoleBlur,
    ConsoleCorner,
    ConsoleTitle,
    ConsoleBorder,
    ConsoleGrid,
    ConsoleTintAuto,
    ConsoleTintDark,
    ConsoleTintLight,
    ConsoleTintCustom,
    ConsoleTintColor,
    ConsoleTextColor,
    ConsoleApplyAll,
    ConsoleCenterExpanded,
    // Per-group rows: name statics at ConsoleNameBase + index, follow
    // checkboxes at ConsoleFollowBase + index.
    ConsoleNameBase = 500,
    ConsoleFollowBase = 600,
    ConsoleMaxRows = 20,
};

int ScaleDip(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

std::wstring NewId() {
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t value[40]{};
    StringFromGUID2(guid, value, static_cast<int>(std::size(value)));
    return value;
}

std::wstring DisplayNameForPath(const std::wstring& path) {
    std::filesystem::path file(path);
    SHFILEINFOW shellInfo{};
    if (SHGetFileInfoW(path.c_str(), 0, &shellInfo, sizeof(shellInfo), SHGFI_DISPLAYNAME) &&
        shellInfo.szDisplayName[0] != L'\0') {
        std::wstring display = shellInfo.szDisplayName;
        if (_wcsicmp(file.extension().c_str(), L".lnk") == 0) return display;
        const std::wstring extension = file.extension().wstring();
        if (!extension.empty() && display.size() > extension.size() &&
            _wcsicmp(display.c_str() + display.size() - extension.size(), extension.c_str()) == 0) {
            display.resize(display.size() - extension.size());
        }
        return display;
    }
    std::wstring name = file.stem().wstring();
    if (name.empty()) name = file.filename().wstring();
    return name.empty() ? path : name;
}

std::filesystem::path KnownFolderPath(REFKNOWNFOLDERID folder) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    if (left.empty() || right.empty()) return false;
    return CompareStringOrdinal(left.lexically_normal().c_str(), -1,
                                right.lexically_normal().c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsShortcutFile(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return _wcsicmp(extension.c_str(), L".lnk") == 0 ||
           _wcsicmp(extension.c_str(), L".url") == 0;
}

// Resolve a .lnk to the path of the program it launches, so "open file
// location" points at the real executable's folder instead of the shortcut
// file itself. Internet shortcuts (.url) have no local target.
std::wstring ResolveShortcutTarget(const std::filesystem::path& path) {
    if (_wcsicmp(path.extension().c_str(), L".lnk") != 0) return {};
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link))))
        return {};
    IPersistFile* persisted = nullptr;
    std::wstring target;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persisted)))) {
        if (SUCCEEDED(persisted->Load(path.c_str(), STGM_READ))) {
            wchar_t buffer[32768]{};
            if (SUCCEEDED(link->GetPath(buffer, static_cast<int>(std::size(buffer)),
                                        nullptr, SLGP_UNCPRIORITY)))
                target = buffer;
        }
        persisted->Release();
    }
    link->Release();
    return target;
}

bool IsDesktopItem(const std::filesystem::path& path) {
    if (path.empty()) return false;
    const auto parent = path.parent_path();
    const auto userDesktop = KnownFolderPath(FOLDERID_Desktop);
    const auto publicDesktop = KnownFolderPath(FOLDERID_PublicDesktop);
    return SamePath(parent, userDesktop) || SamePath(parent, publicDesktop);
}

std::filesystem::path DesktopStorageRoot(const std::wstring& id,
                                         const std::filesystem::path& desktopRoot) {
    if (desktopRoot.empty()) return {};
    return desktopRoot.parent_path() / L".DesktopOrganizer" / L"Items" / id;
}

bool IsPathInStorageRoot(const std::filesystem::path& path,
                         const std::filesystem::path& root,
                         std::wstring* groupId = nullptr) {
    if (path.empty() || root.empty()) return false;
    const auto candidate = path.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    auto candidatePart = candidate.begin();
    for (auto rootPart = normalizedRoot.begin(); rootPart != normalizedRoot.end();
         ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() ||
            CompareStringOrdinal(candidatePart->c_str(), -1, rootPart->c_str(), -1, TRUE) != CSTR_EQUAL)
            return false;
    }
    size_t components = 0;
    for (; candidatePart != candidate.end(); ++candidatePart) {
        const auto& component = *candidatePart;
        if (component == L"..") return false;
        if (components == 0 && groupId) *groupId = component.wstring();
        ++components;
    }
    return components >= 2;
}

bool IsStoredItem(const std::filesystem::path& path) {
    // Managed ownership is determined by the storage root, not by the file's
    // current existence. Otherwise a missing legacy item can be mistaken for
    // an external reference and silently discarded.
    if (IsPathInStorageRoot(path, ConfigStore().Path().parent_path() / L"Items")) return true;
    const auto userDesktop = KnownFolderPath(FOLDERID_Desktop);
    const auto publicDesktop = KnownFolderPath(FOLDERID_PublicDesktop);
    return IsPathInStorageRoot(path, userDesktop.parent_path() / L".DesktopOrganizer" / L"Items") ||
           IsPathInStorageRoot(path, publicDesktop.parent_path() / L".DesktopOrganizer" / L"Items");
}

enum class PathExistence { Missing, Present, Unknown };

PathExistence QueryPathExistence(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    if (error) return PathExistence::Unknown;
    return present ? PathExistence::Present : PathExistence::Missing;
}

bool IsOrganizerWindow(HWND window) {
    const HWND root = GetAncestor(window, GA_ROOT);
    wchar_t className[128]{};
    if (!root || !GetClassNameW(root, className, static_cast<int>(std::size(className)))) return false;
    constexpr wchar_t prefix[] = L"DesktopOrganizer.";
    return wcsncmp(className, prefix, std::size(prefix) - 1) == 0;
}

bool IsDesktopWindow(HWND window) {
    for (HWND current = window; current; current = GetParent(current)) {
        wchar_t className[128]{};
        GetClassNameW(current, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
            wcscmp(className, L"SHELLDLL_DefView") == 0 ||
            wcscmp(className, L"SysListView32") == 0) return true;
    }
    return false;
}

bool IsShellDesktopPoint(POINT screen) {
    HWND hit = WindowFromPoint(screen);
    if (!hit) return false;
    if (!IsOrganizerWindow(hit)) return IsDesktopWindow(hit);

    // WindowFromPoint normally sees the drag ghost first. Walk top-level Z order,
    // but only accept the first non-organizer window that actually covers the
    // cursor; never skip an application window merely because the desktop is
    // somewhere below it.
    for (HWND candidate = GetWindow(GetAncestor(hit, GA_ROOT), GW_HWNDNEXT);
         candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (!IsWindowVisible(candidate) || IsIconic(candidate) || IsOrganizerWindow(candidate)) continue;
        RECT bounds{};
        if (!GetWindowRect(candidate, &bounds) || !PtInRect(&bounds, screen)) continue;
        return IsDesktopWindow(candidate);
    }
    return false;
}

void RemovePreparedDesktopCopy(const std::filesystem::path& path) {
    const auto userDesktop = KnownFolderPath(FOLDERID_Desktop);
    const auto publicDesktop = KnownFolderPath(FOLDERID_PublicDesktop);
    if (!SamePath(path.parent_path(), userDesktop) && !SamePath(path.parent_path(), publicDesktop)) return;
    std::error_code ignored;
    std::filesystem::remove_all(path_io::ExtendedLengthPath(path), ignored);
}

std::filesystem::path DesktopRootForStoredItem(const std::filesystem::path& path) {
    const auto userDesktop = KnownFolderPath(FOLDERID_Desktop);
    const auto publicDesktop = KnownFolderPath(FOLDERID_PublicDesktop);
    if (IsPathInStorageRoot(path, userDesktop.parent_path() / L".DesktopOrganizer" / L"Items"))
        return userDesktop;
    if (IsPathInStorageRoot(path, publicDesktop.parent_path() / L".DesktopOrganizer" / L"Items"))
        return userDesktop;
    return userDesktop;
}

void NotifyDesktopShortcutChange(LONG event, const std::filesystem::path& path) {
    // Let Explorer process only this path without synchronously flushing the
    // entire desktop directory. A full directory refresh causes a visible
    // desktop blink while an item is being dragged out of a container.
    SHChangeNotify(event, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}

bool TryPositionDesktopItem(const std::filesystem::path& path, POINT screen) {
    PIDLIST_ABSOLUTE absolute = nullptr;
    SFGAOF attributes = 0;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, &attributes)) || !absolute)
        return false;

    IShellWindows* shellWindows = nullptr;
    IDispatch* desktopDispatch = nullptr;
    IServiceProvider* serviceProvider = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* shellView = nullptr;
    IFolderView* folderView = nullptr;
    bool positioned = false;

    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                   IID_PPV_ARGS(&shellWindows)))) {
        VARIANT empty{};
        VariantInit(&empty);
        long desktopWindow = 0;
        if (SUCCEEDED(shellWindows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &desktopWindow,
                                                 SWFO_NEEDDISPATCH, &desktopDispatch)) &&
            desktopDispatch && SUCCEEDED(desktopDispatch->QueryInterface(IID_PPV_ARGS(&serviceProvider))) &&
            SUCCEEDED(serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
            SUCCEEDED(browser->QueryActiveShellView(&shellView)) && shellView &&
            SUCCEEDED(shellView->QueryInterface(IID_PPV_ARGS(&folderView))) && folderView) {
            PCUITEMID_CHILD child = ILFindLastID(absolute);
            POINT target = screen;
            // SelectAndPositionItems expects the icon's upper-left corner.
            // Translate the drop point from the cursor center, then let the
            // Shell apply its current grid/auto-arrange policy.
            target.x -= GetSystemMetrics(SM_CXICON) / 2;
            target.y -= GetSystemMetrics(SM_CYICON) / 2;
            POINT current{};
            if (SUCCEEDED(folderView->GetItemPosition(child, &current))) {
                positioned = SUCCEEDED(folderView->SelectAndPositionItems(
                    1, &child, &target, SVSI_POSITIONITEM | SVSI_TRANSLATEPT | SVSI_NOTAKEFOCUS));
            }
        }
    }

    if (folderView) folderView->Release();
    if (shellView) shellView->Release();
    if (browser) browser->Release();
    if (serviceProvider) serviceProvider->Release();
    if (desktopDispatch) desktopDispatch->Release();
    if (shellWindows) shellWindows->Release();
    CoTaskMemFree(absolute);
    return positioned;
}

struct DesktopPlacementRequest {
    std::filesystem::path path;
    POINT screen{};
    int attempts = 0;
};

struct FileIdentity {
    DWORD volumeSerial = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
};

struct ShellItemReconcileRequest {
    std::filesystem::path path;
    FileIdentity identity{};
    int attempts = 0;
};

std::optional<FileIdentity> IdentityForPath(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool read = GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    if (!read) return std::nullopt;
    return FileIdentity{information.dwVolumeSerialNumber, information.nFileIndexHigh,
                        information.nFileIndexLow};
}

bool SameIdentity(const FileIdentity& left, const FileIdentity& right) {
    return left.volumeSerial == right.volumeSerial && left.indexHigh == right.indexHigh &&
           left.indexLow == right.indexLow;
}

std::filesystem::path FindPathByIdentity(const std::filesystem::path& directory,
                                         const FileIdentity& identity) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        const auto candidate = IdentityForPath(entry.path());
        if (candidate && SameIdentity(*candidate, identity)) return entry.path();
    }
    return {};
}

std::filesystem::path UniqueDesktopPath(const std::filesystem::path& source,
                                        std::filesystem::path desktop = {}) {
    if (desktop.empty()) desktop = KnownFolderPath(FOLDERID_Desktop);
    if (desktop.empty()) return {};
    std::filesystem::path candidate = desktop / source.filename();
    const auto stem = source.stem().wstring();
    const auto extension = source.extension().wstring();
    int index = 1;
    PathExistence existence = QueryPathExistence(candidate);
    for (; existence == PathExistence::Present && index < 10000; ++index) {
        candidate = desktop / (stem + L" (" + std::to_wstring(index) + L")" + extension);
        existence = QueryPathExistence(candidate);
    }
    if (existence != PathExistence::Missing) return {};
    return candidate;
}

std::filesystem::path UniquePathInDirectory(const std::filesystem::path& directory,
                                            const std::filesystem::path& source) {
    if (directory.empty()) return {};
    std::filesystem::path candidate = directory / source.filename();
    const auto stem = source.stem().wstring();
    const auto extension = source.extension().wstring();
    int index = 1;
    PathExistence existence = QueryPathExistence(candidate);
    for (; existence == PathExistence::Present && index < 10000; ++index) {
        candidate = directory / (stem + L" (" + std::to_wstring(index) + L")" + extension);
        existence = QueryPathExistence(candidate);
    }
    if (existence != PathExistence::Missing) return {};
    return candidate;
}

bool HasCommandLineArgument(const wchar_t* wanted) {
    if (!wanted || !*wanted) return false;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (_wcsicmp(arguments[index], wanted) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

bool HasNewGroupArgument() { return HasCommandLineArgument(L"--new-group"); }
bool HasOpenConsoleArgument() { return HasCommandLineArgument(L"--open-console"); }

// Cross-integrity hand-off directory. A standard-rights second instance
// launched by Explorer's desktop context menu cannot see an elevated main
// instance's windows (UIPI), so requests are also dropped here: the LocalLow
// tree is writable for low/medium integrity processes and readable by all.
std::filesystem::path NewGroupInboxDirectory() {
    PWSTR localLow = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, KF_FLAG_CREATE, nullptr, &localLow)))
        return {};
    std::filesystem::path directory(localLow);
    CoTaskMemFree(localLow);
    return directory / L"DesktopOrganizer" / L"NewGroupInbox";
}

bool SetRegistryString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegisterDesktopContextMenu() {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) return false;

    const std::wstring executablePath(executable, length);
    // Remove the discontinued visible anchor-file integration. Existing
    // legacy files are cleaned per group after config loading below.
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.desktoporganizer-group");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\DesktopOrganizer.Group");

    HKEY menu = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kDesktopMenuKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &menu, nullptr) != ERROR_SUCCESS) return false;
    const bool labelWritten = SetRegistryString(menu, nullptr, L"新建桌面分组");
    const bool iconWritten = SetRegistryString(menu, L"Icon", L"\"" + executablePath + L"\",0");
    RegCloseKey(menu);
    if (!labelWritten || !iconWritten) return false;

    HKEY command = nullptr;
    const std::wstring commandKey = std::wstring(kDesktopMenuKey) + L"\\command";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, commandKey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &command, nullptr) != ERROR_SUCCESS) return false;
    const bool commandWritten = SetRegistryString(
        command, nullptr, L"\"" + executablePath + L"\" --new-group");
    RegCloseKey(command);
    if (!commandWritten) return false;

    HKEY consoleMenu = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kDesktopConsoleMenuKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &consoleMenu, nullptr) != ERROR_SUCCESS) return false;
    const bool consoleLabelWritten = SetRegistryString(consoleMenu, nullptr, L"打开分组总控台");
    const bool consoleIconWritten = SetRegistryString(
        consoleMenu, L"Icon", L"\"" + executablePath + L"\",0");
    RegCloseKey(consoleMenu);
    if (!consoleLabelWritten || !consoleIconWritten) return false;

    HKEY consoleCommand = nullptr;
    const std::wstring consoleCommandKey = std::wstring(kDesktopConsoleMenuKey) + L"\\command";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, consoleCommandKey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &consoleCommand, nullptr) != ERROR_SUCCESS) return false;
    const bool consoleCommandWritten = SetRegistryString(
        consoleCommand, nullptr, L"\"" + executablePath + L"\" --open-console");
    RegCloseKey(consoleCommand);
    if (!consoleCommandWritten) return false;

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

HWND FindOrganizerMessageWindow() {
    HWND target = FindWindowW(kMessageClass, L"DesktopOrganizer");
    if (!target) target = FindWindowExW(HWND_MESSAGE, nullptr, kMessageClass, L"DesktopOrganizer");
    return target;
}

bool WriteInboxRequestAndWait(std::string_view content, const wchar_t* prefix) {
    const std::filesystem::path inbox = NewGroupInboxDirectory();
    if (inbox.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(inbox, error);
    if (error) return false;
    wchar_t name[96]{};
    swprintf_s(name, L"%ls-%lu-%llu.req", prefix, GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));
    const std::filesystem::path file = inbox / name;
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << content;
    }
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (!std::filesystem::exists(file, error)) return true;
        Sleep(250);
    }
    std::filesystem::remove(file, error);
    return false;
}

bool NotifyExistingInstanceToCreateGroup(POINT screen) {
    NewGroupRequest request{screen,
                            GetCurrentProcessId() ^ static_cast<DWORD>(GetTickCount())};
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataNewGroup;
    data.cbData = sizeof(request);
    data.lpData = &request;
    // Fast path: a window message, which only works when both instances run
    // at the same integrity level. The main instance may still be busy with
    // startup work (wallpaper decode, grid probing, config loading), so wait
    // a short while for its message window and retry while it is occupied.
    for (int attempt = 0; attempt < 10; ++attempt) {
        HWND target = FindOrganizerMessageWindow();
        if (target) {
            for (int send = 0; send < 3; ++send) {
                DWORD_PTR result = 0;
                if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                        SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &result) != 0 &&
                    result != 0) {
                    return true;
                }
                if (!IsWindow(target)) break;
                Sleep(300);
            }
        }
        Sleep(100);
    }
    // Fallback: file inbox under LocalLow. This crosses the integrity
    // boundary when the main instance runs elevated (its windows are
    // invisible to us under UIPI). The receiver polls the inbox and deletes
    // each request after honoring it, which doubles as our receipt.
    const std::string content = std::to_string(screen.x) + ',' + std::to_string(screen.y) + ',' +
                                std::to_string(request.cookie);
    return WriteInboxRequestAndWait(content, L"new-group");
}

bool NotifyExistingInstanceToOpenConsole() {
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataOpenConsole;
    for (int attempt = 0; attempt < 10; ++attempt) {
        HWND target = FindOrganizerMessageWindow();
        if (target) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                    SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &result) != 0 &&
                result != 0) return true;
        }
        Sleep(100);
    }
    return WriteInboxRequestAndWait("open-console", L"open-console");
}

// Shortcuts often expose the link overlay icon instead of the target's full
// resolution icon. Resolve the target before asking the Shell image list.
std::wstring IconSourceForPath(const std::wstring& path) {
    if (_wcsicmp(std::filesystem::path(path).extension().c_str(), L".lnk") != 0) return path;

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link)))) {
        return path;
    }
    std::wstring source = path;
    IPersistFile* persisted = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persisted)))) {
        if (SUCCEEDED(persisted->Load(path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH]{};
            WIN32_FIND_DATAW data{};
            if (SUCCEEDED(link->GetPath(target, static_cast<int>(std::size(target)), &data, SLGP_RAWPATH)) &&
                target[0] != L'\0' && GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
                source = target;
            }
        }
        persisted->Release();
    }
    link->Release();
    return source;
}

HICON NormalizeIconPixels(HICON icon, int pixels) {
    if (!icon || pixels <= 0) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC sourceDc = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = pixels;
    info.bmiHeader.biHeight = -pixels;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* sourceBits = nullptr;
    HBITMAP sourceBitmap = screen
        ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &sourceBits, nullptr, 0) : nullptr;
    if (!screen || !sourceDc || !sourceBitmap || !sourceBits) {
        if (sourceBitmap) DeleteObject(sourceBitmap);
        if (sourceDc) DeleteDC(sourceDc);
        if (screen) ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HGDIOBJ previous = SelectObject(sourceDc, sourceBitmap);
    std::fill_n(static_cast<std::uint32_t*>(sourceBits), static_cast<std::size_t>(pixels) * pixels, 0u);
    DrawIconEx(sourceDc, 0, 0, icon, pixels, pixels, 0, nullptr, DI_NORMAL);
    std::vector<std::uint32_t> source(static_cast<std::uint32_t*>(sourceBits),
                                      static_cast<std::uint32_t*>(sourceBits) +
                                          static_cast<std::size_t>(pixels) * pixels);
    const icon_image::Bounds visible = icon_image::VisibleBounds(source, pixels, pixels);
    if (visible.Empty() || std::max(visible.Width(), visible.Height()) >= pixels * 7 / 10) {
        SelectObject(sourceDc, previous);
        DeleteObject(sourceBitmap);
        DeleteDC(sourceDc);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    const auto fitted = icon_image::FitVisiblePixels(source, pixels, pixels, pixels);
    SelectObject(sourceDc, previous);
    DeleteObject(sourceBitmap);
    DeleteDC(sourceDc);

    void* outputBits = nullptr;
    HBITMAP outputBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &outputBits, nullptr, 0);
    if (!outputBitmap || !outputBits) {
        if (outputBitmap) DeleteObject(outputBitmap);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    std::copy(fitted.begin(), fitted.end(), static_cast<std::uint32_t*>(outputBits));
    HBITMAP mask = CreateBitmap(pixels, pixels, 1, 1, nullptr);
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = outputBitmap;
    iconInfo.hbmMask = mask;
    HICON normalized = mask ? CreateIconIndirect(&iconInfo) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(outputBitmap);
    ReleaseDC(nullptr, screen);
    return normalized;
}

std::wstring Ellipsize(HDC dc, std::wstring text, int width) {
    SIZE size{};
    if (GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size) && size.cx <= width) return text;
    while (text.size() > 2) {
        text.resize(text.size() - 1);
        const std::wstring candidate = text + L"…";
        if (GetTextExtentPoint32W(dc, candidate.c_str(), static_cast<int>(candidate.size()), &size) && size.cx <= width) return candidate;
    }
    return L"…";
}

bool SystemUsesDarkTheme() {
    DWORD lightTheme = 1;
    DWORD size = sizeof(lightTheme);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme, &size);
    return lightTheme == 0;
}

bool VisualTestMode() {
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    return true;
#else
    return false;
#endif
}

bool VisualTestLiveCaptureEnabled() {
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    wchar_t enabled[2]{};
    return GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_ENABLE_LIVE_CAPTURE", enabled,
                                   static_cast<DWORD>(std::size(enabled))) > 0;
#else
    return true;
#endif
}

std::filesystem::path OrganizerDesktopRoot() {
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    wchar_t overridePath[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_DESKTOP", overridePath,
        static_cast<DWORD>(std::size(overridePath)));
    if (length == 0 || length >= std::size(overridePath))
        return std::filesystem::temp_directory_path() / L"DesktopOrganizer.visual-test.desktop";
    return std::filesystem::path(std::wstring(overridePath, length));
#else
    return KnownFolderPath(FOLDERID_Desktop);
#endif
}

bool UsesDarkGlass(const ContainerState& state) {
    if (state.tintMode == 3) {
        const int red = (state.tintColor >> 16) & 0xff;
        const int green = (state.tintColor >> 8) & 0xff;
        const int blue = state.tintColor & 0xff;
        return red * 54 + green * 183 + blue * 19 < 128 * 256;
    }
    return state.tintMode == 2 || (state.tintMode == 0 && SystemUsesDarkTheme());
}

bool ClientAnimationsEnabled() {
    BOOL enabled = TRUE;
    return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled != FALSE;
}

COLORREF TintColor(const ContainerState& state) {
    return RGB((state.tintColor >> 16) & 0xff, (state.tintColor >> 8) & 0xff, state.tintColor & 0xff);
}


// Optional real-time system backdrop for the glass layer.
//
// The desktop icon grid cell in DPI-independent units, mirroring Explorer's
// IconSpacing / IconVerticalSpacing (default 75). Used for collapsed size and
// for the per-group "snap to desktop grid" mode.
POINT DesktopIconCellDip() {
    POINT cell{75, 75};
    const auto read = [](const wchar_t* value, int fallback) {
        wchar_t buffer[32]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop\\WindowMetrics", value,
                         RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
            int parsed = std::abs(_wtoi(buffer));
            // Legacy shells store the spacing as -15 * pixels (e.g. -1125 = 75).
            if (parsed > 480) parsed = (parsed + 7) / 15;
            if (parsed >= 32 && parsed <= 480) return parsed;
        }
        return fallback;
    };
    cell.x = read(L"IconSpacing", 75);
    cell.y = read(L"IconVerticalSpacing", 75);
    return cell;
}

DWORD PixelFromChannels(int blue, int green, int red) {
    return static_cast<DWORD>(std::clamp(blue, 0, 255) |
                              (std::clamp(green, 0, 255) << 8) |
                              (std::clamp(red, 0, 255) << 16));
}

// Draw antialiased text into a premultiplied layered-window surface. Drawing
// directly into the destination DIB leaves its alpha channel untouched, which
// made dark user-selected colors disappear. A monochrome mask gives us stable
// coverage for every color, including pure black.
void CompositeLayeredText(DWORD* destination, size_t stride, int surfaceWidth, int surfaceHeight,
                          RECT rect, std::wstring_view text, HFONT font, UINT format,
                          COLORREF color, COLORREF shadowColor) {
    rect.left = std::clamp(rect.left, 0L, static_cast<LONG>(surfaceWidth));
    rect.right = std::clamp(rect.right, 0L, static_cast<LONG>(surfaceWidth));
    rect.top = std::clamp(rect.top, 0L, static_cast<LONG>(surfaceHeight));
    rect.bottom = std::clamp(rect.bottom, 0L, static_cast<LONG>(surfaceHeight));
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (!destination || !font || text.empty() || width <= 0 || height <= 0) return;

    HDC maskDc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD* mask = nullptr;
    HBITMAP bitmap = maskDc ? CreateDIBSection(maskDc, &info, DIB_RGB_COLORS,
                                               reinterpret_cast<void**>(&mask), nullptr, 0) : nullptr;
    if (!maskDc || !bitmap || !mask) {
        if (bitmap) DeleteObject(bitmap);
        if (maskDc) DeleteDC(maskDc);
        return;
    }
    HGDIOBJ previousBitmap = SelectObject(maskDc, bitmap);
    HGDIOBJ previousFont = SelectObject(maskDc, font);
    std::fill_n(mask, static_cast<size_t>(width) * height, 0u);
    SetBkMode(maskDc, OPAQUE);
    SetBkColor(maskDc, RGB(0, 0, 0));
    SetTextColor(maskDc, RGB(255, 255, 255));
    RECT local{0, 0, width, height};
    DrawTextW(maskDc, text.data(), static_cast<int>(text.size()), &local, format);
    GdiFlush();

    const auto composite = [&](int offsetX, int offsetY, COLORREF sourceColor, int maximumAlpha) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const DWORD maskPixel = mask[static_cast<size_t>(y) * width + x];
                const int coverage = std::max({static_cast<int>(maskPixel & 0xff),
                    static_cast<int>((maskPixel >> 8) & 0xff),
                    static_cast<int>((maskPixel >> 16) & 0xff)});
                const int alpha = coverage * maximumAlpha / 255;
                const int targetX = rect.left + x + offsetX;
                const int targetY = rect.top + y + offsetY;
                if (alpha <= 0 || targetX < 0 || targetY < 0 ||
                    targetX >= surfaceWidth || targetY >= surfaceHeight) continue;
                DWORD& target = destination[static_cast<size_t>(targetY) * stride + targetX];
                const int inverse = 255 - alpha;
                const int targetAlpha = static_cast<int>(target >> 24);
                const int outAlpha = alpha + targetAlpha * inverse / 255;
                const int blue = GetBValue(sourceColor) * alpha / 255 +
                                 static_cast<int>(target & 0xff) * inverse / 255;
                const int green = GetGValue(sourceColor) * alpha / 255 +
                                  static_cast<int>((target >> 8) & 0xff) * inverse / 255;
                const int red = GetRValue(sourceColor) * alpha / 255 +
                                static_cast<int>((target >> 16) & 0xff) * inverse / 255;
                target = PixelFromChannels(blue, green, red) |
                         (static_cast<DWORD>(outAlpha) << 24);
            }
        }
    };
    composite(1, 1, shadowColor, 155);
    composite(0, 0, color, 255);

    SelectObject(maskDc, previousFont);
    SelectObject(maskDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(maskDc);
}

struct WallpaperImage {
    int width = 0;
    int height = 0;
    std::vector<DWORD> pixels;
};

bool DecodeWallpaper(IWICImagingFactory* factory, const std::wstring& path, WallpaperImage& image) {
    if (!factory || path.empty()) return false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool decoded = false;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGR,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(converter->GetSize(&width, &height))) {
            const unsigned long long decodedBytes =
                static_cast<unsigned long long>(width) * height * sizeof(DWORD);
            if (width > 0 && height > 0 && width <= static_cast<UINT>(std::numeric_limits<int>::max()) &&
                height <= static_cast<UINT>(std::numeric_limits<int>::max()) &&
                decodedBytes <= std::numeric_limits<UINT>::max() &&
                decodedBytes <= kMaxWallpaperDecodeBytes) {
                try {
                    image.pixels.resize(static_cast<size_t>(width) * height);
                    image.width = static_cast<int>(width);
                    image.height = static_cast<int>(height);
                    decoded = SUCCEEDED(converter->CopyPixels(
                        nullptr, width * sizeof(DWORD), static_cast<UINT>(decodedBytes),
                        reinterpret_cast<BYTE*>(image.pixels.data())));
                } catch (const std::bad_alloc&) {
                    decoded = false;
                }
            }
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (!decoded) image = {};
    return decoded;
}

void DrawWallpaper(HDC dc, const WallpaperImage& image, const RECT& area,
                   DESKTOP_WALLPAPER_POSITION position) {
    if (!dc || image.width <= 0 || image.height <= 0 || area.right <= area.left || area.bottom <= area.top) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    auto draw = [&](const RECT& destination, int sourceX, int sourceY, int sourceWidth, int sourceHeight) {
        StretchDIBits(dc, destination.left, destination.top,
                      destination.right - destination.left, destination.bottom - destination.top,
                      sourceX, sourceY, sourceWidth, sourceHeight, image.pixels.data(), &info,
                      DIB_RGB_COLORS, SRCCOPY);
    };

    const int areaWidth = area.right - area.left;
    const int areaHeight = area.bottom - area.top;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    if (position == DWPOS_TILE) {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        for (int y = area.top; y < area.bottom; y += image.height) {
            for (int x = area.left; x < area.right; x += image.width) {
                const RECT destination{x, y, x + image.width, y + image.height};
                draw(destination, 0, 0, image.width, image.height);
            }
        }
        RestoreDC(dc, saved);
        return;
    }
    if (position == DWPOS_CENTER) {
        const int x = area.left + (areaWidth - image.width) / 2;
        const int y = area.top + (areaHeight - image.height) / 2;
        const RECT destination{x, y, x + image.width, y + image.height};
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
        draw(destination, 0, 0, image.width, image.height);
        RestoreDC(dc, saved);
        return;
    }
    if (position == DWPOS_FIT) {
        const double scale = std::min(static_cast<double>(areaWidth) / image.width,
                                      static_cast<double>(areaHeight) / image.height);
        const int width = std::max(1, static_cast<int>(std::lround(image.width * scale)));
        const int height = std::max(1, static_cast<int>(std::lround(image.height * scale)));
        const RECT destination{area.left + (areaWidth - width) / 2,
                               area.top + (areaHeight - height) / 2,
                               area.left + (areaWidth + width) / 2,
                               area.top + (areaHeight + height) / 2};
        draw(destination, 0, 0, image.width, image.height);
        return;
    }
    if (position == DWPOS_FILL || position == DWPOS_SPAN) {
        const double scale = std::max(static_cast<double>(areaWidth) / image.width,
                                      static_cast<double>(areaHeight) / image.height);
        const int sourceWidth = std::clamp(static_cast<int>(std::lround(areaWidth / scale)), 1, image.width);
        const int sourceHeight = std::clamp(static_cast<int>(std::lround(areaHeight / scale)), 1, image.height);
        draw(area, (image.width - sourceWidth) / 2, (image.height - sourceHeight) / 2,
             sourceWidth, sourceHeight);
        return;
    }
    draw(area, 0, 0, image.width, image.height);
}

#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
void WriteVisualBmp(const std::filesystem::path& path, const DWORD* pixels,
                    size_t stride, int width, int height) {
    if (!pixels || width <= 0 || height <= 0 || path.empty()) return;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(static_cast<size_t>(width) * height * sizeof(DWORD));
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int row = height - 1; row >= 0; --row) {
        stream.write(reinterpret_cast<const char*>(pixels + static_cast<size_t>(row) * stride),
                     static_cast<std::streamsize>(width * sizeof(DWORD)));
    }
}

void WriteVisualSnapshotIfRequested(const DWORD* pixels, size_t stride, int width, int height) {
    wchar_t outputPath[32768]{};
    const DWORD pathLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SNAPSHOT", outputPath, static_cast<DWORD>(std::size(outputPath)));
    if (pathLength == 0 || pathLength >= std::size(outputPath)) return;
    WriteVisualBmp(std::filesystem::path(outputPath), pixels, stride, width, height);
}

void WriteCompositedVisualSnapshotIfRequested(const DWORD* foreground, size_t foregroundStride,
                                               int width, int height,
                                               const std::vector<DWORD>& backdrop) {
    wchar_t outputPath[32768]{};
    const DWORD pathLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_COMPOSITED_SNAPSHOT", outputPath,
        static_cast<DWORD>(std::size(outputPath)));
    const size_t pixelCount = static_cast<size_t>(std::max(0, width)) * std::max(0, height);
    if (pathLength == 0 || pathLength >= std::size(outputPath) || !foreground ||
        width <= 0 || height <= 0 || backdrop.size() != pixelCount) return;
    std::vector<DWORD> composited(pixelCount);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t outputIndex = static_cast<size_t>(y) * width + x;
            const DWORD front = foreground[static_cast<size_t>(y) * foregroundStride + x];
            const DWORD back = backdrop[outputIndex];
            const int alpha = static_cast<int>((front >> 24) & 0xff);
            const int inverse = 255 - alpha;
            const int blue = static_cast<int>(front & 0xff) +
                (static_cast<int>(back & 0xff) * inverse + 127) / 255;
            const int green = static_cast<int>((front >> 8) & 0xff) +
                (static_cast<int>((back >> 8) & 0xff) * inverse + 127) / 255;
            const int red = static_cast<int>((front >> 16) & 0xff) +
                (static_cast<int>((back >> 16) & 0xff) * inverse + 127) / 255;
            composited[outputIndex] = PixelFromChannels(
                std::min(255, blue), std::min(255, green), std::min(255, red)) | 0xff000000;
        }
    }
    WriteVisualBmp(std::filesystem::path(outputPath), composited.data(),
                   static_cast<size_t>(width), width, height);
}
#endif

void SetOverlayPosition(HWND overlay, HWND visualAnchor, int x, int y, int width, int height, UINT flags) {
    HWND insertAfter = visualAnchor ? GetWindow(visualAnchor, GW_HWNDPREV) : nullptr;
    // Once the overlay is already directly above its anchor, GW_HWNDPREV is
    // the overlay itself. Passing the same HWND as hWnd and hWndInsertAfter
    // makes SetWindowPos fail, so later animation frames appeared frozen.
    if (insertAfter == overlay) {
        insertAfter = nullptr;
        flags |= SWP_NOZORDER;
    }
    if (!insertAfter) insertAfter = HWND_TOP;
    SetWindowPos(overlay, insertAfter, x, y, width, height, flags | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void BoxBlur(std::vector<DWORD>& pixels, int width, int height, int radius) {
    if (radius <= 0 || width <= 0 || height <= 0) return;
    radius = std::min(radius, std::max(width, height));
    const int diameter = radius * 2 + 1;
    std::vector<DWORD> temporary(pixels.size());

    for (int y = 0; y < height; ++y) {
        long blue = 0;
        long green = 0;
        long red = 0;
        const auto add = [&](DWORD pixel, int sign) {
            blue += sign * static_cast<int>(pixel & 0xff);
            green += sign * static_cast<int>((pixel >> 8) & 0xff);
            red += sign * static_cast<int>((pixel >> 16) & 0xff);
        };
        for (int offset = -radius; offset <= radius; ++offset)
            add(pixels[static_cast<size_t>(y) * width + std::clamp(offset, 0, width - 1)], 1);
        for (int x = 0; x < width; ++x) {
            temporary[static_cast<size_t>(y) * width + x] = PixelFromChannels(
                static_cast<int>(blue / diameter), static_cast<int>(green / diameter), static_cast<int>(red / diameter));
            add(pixels[static_cast<size_t>(y) * width + std::clamp(x - radius, 0, width - 1)], -1);
            add(pixels[static_cast<size_t>(y) * width + std::clamp(x + radius + 1, 0, width - 1)], 1);
        }
    }

    for (int x = 0; x < width; ++x) {
        long blue = 0;
        long green = 0;
        long red = 0;
        const auto add = [&](DWORD pixel, int sign) {
            blue += sign * static_cast<int>(pixel & 0xff);
            green += sign * static_cast<int>((pixel >> 8) & 0xff);
            red += sign * static_cast<int>((pixel >> 16) & 0xff);
        };
        for (int offset = -radius; offset <= radius; ++offset)
            add(temporary[static_cast<size_t>(std::clamp(offset, 0, height - 1)) * width + x], 1);
        for (int y = 0; y < height; ++y) {
            pixels[static_cast<size_t>(y) * width + x] = PixelFromChannels(
                static_cast<int>(blue / diameter), static_cast<int>(green / diameter), static_cast<int>(red / diameter));
            add(temporary[static_cast<size_t>(std::clamp(y - radius, 0, height - 1)) * width + x], -1);
            add(temporary[static_cast<size_t>(std::clamp(y + radius + 1, 0, height - 1)) * width + x], 1);
        }
    }
}

bool CopyBackdropRegion(HDC backdrop, const RECT& backdropBounds, const RECT& screenRect,
                        std::vector<DWORD>& output, int backdropScale = 1) {
    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;
    if (!backdrop || width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC capture = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HBITMAP bitmap = screen && capture ? CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                                reinterpret_cast<void**>(&pixels), nullptr, 0) : nullptr;
    if (!screen || !capture || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (capture) DeleteDC(capture);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ previous = SelectObject(capture, bitmap);
    backdropScale = std::max(1, backdropScale);
    BOOL copied = FALSE;
    if (backdropScale == 1) {
        copied = BitBlt(capture, 0, 0, width, height, backdrop,
                        screenRect.left - backdropBounds.left,
                        screenRect.top - backdropBounds.top, SRCCOPY);
    } else {
        const int relativeLeft = screenRect.left - backdropBounds.left;
        const int relativeTop = screenRect.top - backdropBounds.top;
        const int relativeRight = screenRect.right - backdropBounds.left;
        const int relativeBottom = screenRect.bottom - backdropBounds.top;
        const int sourceLeft = relativeLeft / backdropScale;
        const int sourceTop = relativeTop / backdropScale;
        const int sourceRight = (relativeRight + backdropScale - 1) / backdropScale;
        const int sourceBottom = (relativeBottom + backdropScale - 1) / backdropScale;
        SetStretchBltMode(capture, HALFTONE);
        SetBrushOrgEx(capture, 0, 0, nullptr);
        copied = StretchBlt(capture, 0, 0, width, height, backdrop,
                            sourceLeft, sourceTop,
                            std::max(1, sourceRight - sourceLeft),
                            std::max(1, sourceBottom - sourceTop), SRCCOPY);
    }
    GdiFlush();
    if (copied) {
        try {
            output.assign(pixels, pixels + static_cast<size_t>(width) * height);
        } catch (const std::bad_alloc&) {
            output.clear();
        }
    }
    SelectObject(capture, previous);
    DeleteObject(bitmap);
    DeleteDC(capture);
    ReleaseDC(nullptr, screen);
    return copied && output.size() == static_cast<size_t>(width) * height;
}

std::wstring MonitorDevice(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(monitor, &info) ? info.szDevice : L"";
}

RECT WorkAreaForRect(const RECT& rect) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcWork;
}

bool PromptForName(HWND owner, std::wstring& value, const ContainerState* style = nullptr);

bool RenameFilesystemItem(HWND owner, OrganizerItem& item, const ContainerState* style = nullptr) {
    const std::filesystem::path previous(item.path);
    std::wstring name = previous.filename().wstring();
    if (!PromptForName(owner, name, style) || name.empty() || name == previous.filename().wstring()) return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        MessageBoxW(owner, L"名称不能包含 \\ / : * ? \" < > |", L"重命名", MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::filesystem::path destination = previous.parent_path() / name;
    const PathExistence destinationExistence = QueryPathExistence(destination);
    if (destinationExistence == PathExistence::Unknown) {
        MessageBoxW(owner, L"无法检查目标路径，请确认磁盘或网络位置可访问。", L"重命名",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    if (destinationExistence == PathExistence::Present) {
        MessageBoxW(owner, L"已经存在同名项目。", L"重命名", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!MoveFileExW(previous.c_str(), destination.c_str(), 0)) {
        MessageBoxW(owner, L"无法重命名该项目。", L"重命名", MB_OK | MB_ICONWARNING);
        return false;
    }
    item.path = destination.wstring();
    item.name = DisplayNameForPath(destination.wstring());
    return true;
}

bool CopyPathToClipboard(HWND owner, const std::filesystem::path& path) {
    const std::wstring text = L"\"" + path.wstring() + L"\"";
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory) return false;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!copied) GlobalFree(memory);
    return copied;
}

struct SliderState {
    int minimum = 0;
    int maximum = 100;
    int value = 50;
    bool dragging = false;
    double visualRatio = 0.5;
    bool visualRatioValid = false;
};

// GetDpiForWindow only exists on Windows 10 1607+. Resolve it once and fall
// back to the DC-based system DPI so the exe still loads on older Windows.
UINT WindowDpi(HWND hwnd) {
    static const auto getDpiForWindow = [] {
        using Fn = UINT(WINAPI*)(HWND);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForWindow"))
                      : static_cast<Fn>(nullptr);
    }();
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(hwnd);
        if (dpi > 0) return dpi;
    }
    HDC dc = GetDC(hwnd);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

void SetSliderFromPoint(HWND window, SliderState& state, int x, bool finalUpdate = false) {
    RECT client{};
    GetClientRect(window, &client);
    const UINT dpi = WindowDpi(window);
    const int inset = ScaleDip(10, dpi);
    const int width = std::max(1, static_cast<int>(client.right) - inset * 2);
    const double ratio = interaction_motion::TrackRatio(x, inset, width);
    const int newValue = interaction_motion::LogicalSliderValue(
        ratio, state.minimum, state.maximum);
    const bool visualChanged = !state.visualRatioValid ||
        std::abs(state.visualRatio - ratio) > 0.0001;
    state.visualRatio = ratio;
    state.visualRatioValid = state.dragging;
    if (visualChanged) InvalidateRect(window, nullptr, FALSE);
    const bool valueChanged = newValue != state.value;
    if (valueChanged) state.value = newValue;
    if (!valueChanged && !finalUpdate) return;
    SendMessageW(GetParent(window), WM_HSCROLL,
                 MAKEWPARAM(finalUpdate ? SB_ENDSCROLL : SB_THUMBTRACK, state.value),
                 reinterpret_cast<LPARAM>(window));
}

LRESULT CALLBACK GlassSliderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SliderState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = new SliderState();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case kSliderSetRange:
        state->minimum = LOWORD(lParam); state->maximum = HIWORD(lParam); return 0;
    case kSliderSetPosition:
        state->value = std::clamp(static_cast<int>(wParam), state->minimum, state->maximum);
        if (!state->dragging) state->visualRatioValid = false;
        InvalidateRect(window, nullptr, FALSE); return 0;
    case kSliderGetPosition: return state->value;
    case WM_LBUTTONDOWN:
        SetFocus(window); SetCapture(window); state->dragging = true;
        SetSliderFromPoint(window, *state, GET_X_LPARAM(lParam)); return 0;
    case WM_MOUSEMOVE:
        if (state->dragging) SetSliderFromPoint(window, *state, GET_X_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        if (state->dragging) {
            SetSliderFromPoint(window, *state, GET_X_LPARAM(lParam), true);
            state->dragging = false;
            state->visualRatioValid = false;
            ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_LEFT || wParam == VK_DOWN) state->value = std::max(state->minimum, state->value - 1);
        else if (wParam == VK_RIGHT || wParam == VK_UP) state->value = std::min(state->maximum, state->value + 1);
        else if (wParam == VK_HOME) state->value = state->minimum;
        else if (wParam == VK_END) state->value = state->maximum;
        else if (wParam == VK_PRIOR) state->value = std::min(state->maximum, state->value + 10);
        else if (wParam == VK_NEXT) state->value = std::max(state->minimum, state->value - 10);
        else return DefWindowProcW(window, message, wParam, lParam);
        state->visualRatioValid = false;
        InvalidateRect(window, nullptr, FALSE);
        SendMessageW(GetParent(window), WM_HSCROLL, MAKEWPARAM(SB_ENDSCROLL, state->value), reinterpret_cast<LPARAM>(window));
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
        HGDIOBJ previousBitmap = bitmap ? SelectObject(buffer, bitmap) : nullptr;
        HDC draw = bitmap ? buffer : dc;
        HBRUSH background = CreateSolidBrush(RGB(244, 249, 250));
        FillRect(draw, &client, background); DeleteObject(background);
        const UINT dpi = WindowDpi(window);
        const int inset = ScaleDip(10, dpi);
        const int centerY = client.bottom / 2;
        const int usable = std::max(1, static_cast<int>(client.right) - inset * 2);
        const double ratio = state->visualRatioValid ? state->visualRatio :
            static_cast<double>(state->value - state->minimum) /
                std::max(1, state->maximum - state->minimum);
        const int thumbX = interaction_motion::VisualSliderX(ratio, inset, usable);
        HBRUSH track = CreateSolidBrush(RGB(220, 234, 236));
        HBRUSH active = CreateSolidBrush(RGB(131, 206, 219));
        HBRUSH thumb = CreateSolidBrush(RGB(255, 255, 255));
        HPEN noPen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldPen = SelectObject(draw, noPen);
        HGDIOBJ oldBrush = SelectObject(draw, track);
        RoundRect(draw, inset, centerY - ScaleDip(2, dpi), client.right - inset,
                  centerY + ScaleDip(3, dpi), ScaleDip(5, dpi), ScaleDip(5, dpi));
        SelectObject(draw, active);
        RoundRect(draw, inset, centerY - ScaleDip(2, dpi), thumbX,
                  centerY + ScaleDip(3, dpi), ScaleDip(5, dpi), ScaleDip(5, dpi));
        HPEN outline = CreatePen(PS_SOLID, 1, RGB(183, 210, 215));
        SelectObject(draw, outline); SelectObject(draw, thumb);
        const int thumbRadius = ScaleDip(8, dpi);
        Ellipse(draw, thumbX - thumbRadius, centerY - thumbRadius,
                thumbX + thumbRadius + 1, centerY + thumbRadius + 1);
        if (GetFocus() == window) {
            RECT focus{thumbX - thumbRadius - ScaleDip(3, dpi),
                       centerY - thumbRadius - ScaleDip(3, dpi),
                       thumbX + thumbRadius + ScaleDip(4, dpi),
                       centerY + thumbRadius + ScaleDip(4, dpi)};
            DrawFocusRect(draw, &focus);
        }
        SelectObject(draw, oldBrush); SelectObject(draw, oldPen);
        DeleteObject(outline); DeleteObject(noPen); DeleteObject(track); DeleteObject(active); DeleteObject(thumb);
        if (bitmap) {
            BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previousBitmap);
            DeleteObject(bitmap);
        }
        if (buffer) DeleteDC(buffer);
        EndPaint(window, &paint); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
    case WM_NCDESTROY: delete state; SetWindowLongPtrW(window, GWLP_USERDATA, 0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

class Application;
LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
Application* gMouseHookApplication = nullptr;

class ContainerWindow {
public:
    ContainerWindow(Application& app, ContainerState state) : app_(app), state_(std::move(state)) {}
    ~ContainerWindow();

    bool Create(HINSTANCE instance);
    HWND Handle() const { return window_; }
    ContainerState& State() { return state_; }
    const ContainerState& State() const { return state_; }
    void Show(bool visible);
    bool RefreshAfterExternalChange();
    void RefreshGlass();
    void ApplyGlobalStyle(const GlobalStyle& style);
    static LRESULT CALLBACK SettingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ContentProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TintProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK DragProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HoverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    friend class Application;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void UpdateMetrics();
    void SetCollapsed(bool collapsed, bool centerExpanded = false);
    int CollapsedExtent() const;
    int HalfColumns() const;
    int HalfRows() const;
    int OuterWidth(int columns) const;
    int OuterHeight(int rows) const;
    int ContentHalfUnitHeight(int rows) const;
    int IconPixels(IconSize size) const;
    bool FitsHalf(int halfColumns, int halfRows) const;
    bool LayoutItems(int columns, int rows, std::vector<layout::Placement>& placements) const;
    bool NormalizeItemPositions();
    bool FindItemPlacement(size_t index, POINT client, layout::Placement& result) const;
    bool FindFreePlacementForSpan(int span, POINT client, layout::Placement& result) const;
    bool PlaceItemAt(size_t index, POINT client);
    RECT ItemRect(size_t index) const;
    std::optional<size_t> HitItem(POINT client) const;
    bool HitVisibleSurface(POINT client) const;
    void PaintContent(HWND target);
    void UpdateShape();
    void UpdateNativeBlurLayer();
    void UpdateStateBounds();
    void EnsureVisible();
    bool EnsureCapacity(bool resizeWindow);
    void BeginPointerAction(POINT client);
    void ContinuePointerAction(POINT screen, bool ctrlHeld);
    void EndPointerAction(POINT screen, bool ctrlHeld);
    void StartItemAnimation(size_t index, POINT fromClient);
    void StepItemAnimation();
    void StartRectAnimation(const RECT& target);
    void StepRectAnimation();
    void FinishRectAnimation();
    void SnapMoveRect(RECT& rect) const;
    void SnapRectToDesktopGrid(RECT& rect) const;
    void ShowContextMenu(POINT screen, std::optional<size_t> itemIndex);
    bool ShowShellItemContextMenu(POINT screen, size_t itemIndex);
    void ReconcileShellItem(size_t itemIndex, const FileIdentity& identity);
    void ScheduleShellItemReconcile(const std::filesystem::path& path, const FileIdentity& identity);
    void ProcessShellItemReconciles();
    void ChangeItemIconSize(size_t index, IconSize size);
    void ChangeAllItemIconSize(IconSize size);
    void AddDroppedFiles(HDROP drop);
    void OpenItem(size_t index);
    void OpenItemLocation(size_t index) const;
    void UpdateTooltips();
    void ShowHoverName();
    void HideHoverName();
    void PaintHoverName(HWND target);
    void SyncContentWindow();
    void SyncDesktopLayer(bool includeContainer = true);
    void RaiseForPointerInteraction();
    void RestoreFromPointerInteraction();
    HWND TopmostSiblingSurface() const;
    void UpdateGlass();
    void SetExternalDragPreview(int span, POINT client);
    void ClearExternalDragPreview();
    void UpdateDragPlaceholder();
    void DestroyDragPlaceholder();
    void UpdateMoveTargetPreview(const RECT& freeMoveRect);
    void DestroyMoveTargetPreview();
    void UpdateResizePreview(const RECT& target, int halfColumns, int halfRows);
    void ApplyResizePreviewRect(const RECT& rect);
    void StepResizePreviewAnimation();
    void DestroyResizePreview();
    void UpdateResizeAnimationSurface(const RECT& rect);
    void DestroyResizeAnimationSurface();
    void CreateDragGhost(size_t index, POINT screen);
    void MoveDragGhost(POINT screen);
    void DestroyDragGhost();
    void OpenAppearanceSettings();
    void CreateSettingsControls(HWND window);
    void ChooseTintColor();
    void ChooseTextColor();
    void UpdateSettingsLabels();
    void ApplySettingsFromControls(bool persist);

    Application& app_;
    ContainerState state_;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    int unit_ = 96;
    int cellW_ = 75;
    int cellH_ = 75;
    int visualUnit_ = 75;
    int padding_ = 10;
    int titleHeight_ = 28;
    int resizeBorder_ = 7;
    int resizeCorner_ = 16;
    bool sizingPreview_ = false;
    bool bypassSnapOnRelease_ = false;
    bool interactionTopmost_ = false;
    bool movingFastPath_ = false;
    bool pointerActive_ = false;
    bool moving_ = false;
    bool hoverCollapsed_ = false;
    bool gestureChanged_ = false;
    int resizeEdges_ = 0;
    bool rectAnimationActive_ = false;
    RECT rectAnimationTarget_{};
    RECT rectAnimationFrom_{};
    ULONGLONG rectAnimationStarted_ = 0;
    POINT pointerStartScreen_{};
    RECT pointerStartRect_{};
    std::optional<size_t> pressedItem_;
    bool draggingItem_ = false;
    bool suppressRefresh_ = false;
    std::optional<size_t> itemAnimationIndex_;
    POINT itemAnimationStartOffset_{};
    POINT itemAnimationOffset_{};
    std::optional<layout::Placement> dragPreviewPlacement_;
    ContainerWindow* dragTargetPreview_ = nullptr;
    ULONGLONG itemAnimationStarted_ = 0;
    HWND tooltipWindow_ = nullptr;
    HWND hoverWindow_ = nullptr;
    HWND contentWindow_ = nullptr;
    HWND blurWindow_ = nullptr;
    HWND tintWindow_ = nullptr;
    HWND dragPlaceholderWindow_ = nullptr;
    HWND moveTargetWindow_ = nullptr;
    HWND resizePreviewWindow_ = nullptr;
    HWND resizeAnimationWindow_ = nullptr;
    HWND dragGhostWindow_ = nullptr;
    HWND dragGhostZAnchor_ = nullptr;
    int dragPlaceholderSpan_ = 0;
    std::optional<RECT> moveTargetRect_;
    std::optional<POINT> moveTargetCell_;
    std::optional<RECT> resizePreviewRect_;
    std::optional<RECT> resizePreviewDisplayRect_;
    RECT resizePreviewAnimationFrom_{};
    ULONGLONG resizePreviewAnimationStarted_ = 0;
    bool resizePreviewAnimationActive_ = false;
    bool resizePreviewDestroyWhenFinished_ = false;
    int resizePreviewHalfColumns_ = 0;
    int resizePreviewHalfRows_ = 0;
    int dragGhostWidth_ = 0;
    int dragGhostHeight_ = 0;
    size_t tooltipCount_ = 0;
    std::optional<size_t> hoveredItem_;
    std::optional<size_t> paintedHoverItem_;
    HFONT hoverFont_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HWND opacitySlider_ = nullptr;
    HWND blurSlider_ = nullptr;
    HWND cornerSlider_ = nullptr;
    HWND opacityValue_ = nullptr;
    HWND blurValue_ = nullptr;
    HWND cornerValue_ = nullptr;
    HWND tintColorButton_ = nullptr;
    HWND textColorButton_ = nullptr;
    std::vector<ShellItemReconcileRequest> pendingShellReconciles_;
    HFONT settingsFont_ = nullptr;
    bool settingsUpdatePending_ = false;
    bool settingsPersistPending_ = false;
};

class Application {
public:
    explicit Application(HINSTANCE instance) : instance_(instance) {}
    ~Application();

    bool Initialize(bool createDefaultContainer = true);
    int Run();
    void CreateContainer(std::optional<POINT> screenPosition = std::nullopt);
    void ScheduleDissolve(const std::wstring& id);
    void DissolveContainer(const std::wstring& id);
    bool Save();
    void InvalidateAll();
    bool RenderGlass(HWND target, const RECT& screenRect, const ContainerState& state, UINT dpi,
                     bool sizingPreview, std::optional<size_t> hoveredItem,
                     std::optional<size_t> animatedItem, POINT itemOffset, bool hideAnimatedItem,
                     bool transientFrame = false, int collapsedVisual = 0);
    ContainerWindow* FromWindow(HWND window) const;
    HICON IconForPath(const std::wstring& path, int pixels);
    HWND MessageWindow() const { return messageWindow_; }
    HWND DesktopOwner() const { return desktopOwner_; }
    HWND DesktopLayerHost() const { return desktopLayerHost_; }
    ContainerWindow* DesktopFrontContainer() const { return desktopFrontContainer_; }
    void SetDesktopFrontContainer(ContainerWindow* container);
    void PrepareCenteredExpansion(ContainerWindow* container);
    void BeginContainerInteraction(ContainerWindow* container);
    void DismissContainerInteractionAt(POINT screen);
    void DismissContainerInteractions();
    void SyncDesktopLayers();
    const std::vector<std::unique_ptr<ContainerWindow>>& Containers() const { return containers_; }
    ContainerWindow* ContainerAtPoint(POINT screen, const ContainerWindow* preferred = nullptr) const;
    bool AbsorbDesktopItem(const std::wstring& groupId, OrganizerItem& item,
                           std::filesystem::path* copiedSource = nullptr) const;
    bool MoveItemToDesktop(ContainerWindow& source, size_t index,
                           std::optional<POINT> screenPosition = std::nullopt);
    void MoveItem(ContainerWindow& source, size_t index, ContainerWindow& target, POINT targetPoint);
    // Moves the desktop icons inside `area` (virtual screen coordinates) to
    // the nearest free icon cells, so a snapped group drops cleanly into the
    // grid instead of covering icons. No-op when Explorer auto-arranges.
    void PushDesktopIconsOutOf(const RECT& area, const ContainerWindow* exclude);

    static LRESULT CALLBACK MessageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ConsoleProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void OpenConsole();
    void SetAllSnapToGrid(bool enabled);
    const GlobalStyle& GetGlobalStyle() const { return globalStyle_; }
    void ApplyGlobalStyleToFollowers();
    void RefreshConsoleRows();

    // The real desktop icon grid, measured from Explorer's desktop ListView:
    // cell pitch in physical screen pixels plus the grid phase in
    // virtual screen coordinates. Containers derive every size from this so
    // snapped and unsnapped groups share one size standard.
    struct DesktopGrid {
        int cellW = 75;
        int cellH = 75;
        int originX = 0;
        int originY = 0;
        UINT dpi = 96;
    };
    const DesktopGrid& GetDesktopGrid() const { return desktopGrid_; }
    // Re-measures the desktop grid; returns true when anything changed.
    bool MeasureDesktopGrid();

private:
    struct GlassBaseCacheEntry {
        RECT screenRect{};
        int width = 0;
        int height = 0;
        UINT dpi = 96;
        int opacity = 0;
        int blur = 0;
        int cornerRadius = 0;
        int tintMode = 0;
        int tintColor = 0;
        bool showBorder = false;
        bool collapsed = false;
        bool dark = false;
        ULONGLONG backdropGeneration = 0;
        ULONGLONG lastUsed = 0;
        std::vector<DWORD> pixels;
    };

    bool RegisterClasses();
    HWND DesktopListView() const;
    bool CaptureDesktopBackdrop();
    bool ApplyLatestLiveDesktopFrame();
    void StartLiveDesktopCapture();
    void ReleaseDesktopBackdrop();
    HDC BlurredBackdropDC(int radius, int* scaleOut = nullptr);
    void ReleaseBlurredBackdrop();
    DWORD* AcquireRenderSurface(int width, int height, HDC* dcOut);
    void ReleaseRenderSurface();
    bool TryGlassBaseCache(HWND target, const RECT& screenRect, const ContainerState& state,
                           UINT dpi, bool dark, DWORD* destination, size_t destStride,
                           size_t pixelCount);
    void StoreGlassBaseCache(HWND target, const RECT& screenRect, const ContainerState& state,
                             UINT dpi, bool dark, const DWORD* pixels, size_t sourceStride,
                             size_t pixelCount);
    void InvalidateGlassBaseCache();
    void RefreshDesktopGridMetrics();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screen);
    bool IsStartupEnabled() const;
    void SetStartupEnabled(bool enabled) const;
    void ScheduleDesktopItemPlacement(const std::filesystem::path& path, POINT screen);
    void ProcessDesktopItemPlacements();
    bool DesktopLayersNeedRepair();
    void ProcessNewGroupInbox();
    void NotifySaveFailure();
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void CreateConsoleControls(HWND window);
    void UpdateConsoleLabels();
    void ApplyConsoleFromControls(bool persist);
    void ChooseConsoleTintColor();
    void ChooseConsoleTextColor();

    HINSTANCE instance_;
    HWND messageWindow_ = nullptr;
    HWND desktopOwner_ = nullptr;
    HWND desktopLayerHost_ = nullptr;
    std::unique_ptr<LiveDesktopCapture> liveDesktopCapture_;
    LiveDesktopFrame liveDesktopFrame_;
    HDC desktopDc_ = nullptr;
    HBITMAP desktopBitmap_ = nullptr;
    HGDIOBJ desktopPreviousBitmap_ = nullptr;
    DWORD* desktopBits_ = nullptr;
    RECT desktopBounds_{};
    // Downsampled blurred copies of the latest desktop frame. Most containers
    // share the maximum-radius entry; DPI variants remain bounded below.
    struct BlurredBackdropEntry {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previousBitmap = nullptr;
        int width = 0;
        int height = 0;
        int scale = 1;
        ULONGLONG generation = 0;
        ULONGLONG lastUsed = 0;
    };
    std::map<int, BlurredBackdropEntry> blurredByRadius_;
    // Persistent render surface, reused across glass repaints so interactive
    // drag/resize frames do not allocate a fresh DIB section every time.
    HDC renderDc_ = nullptr;
    HBITMAP renderBitmap_ = nullptr;
    HGDIOBJ renderPreviousBitmap_ = nullptr;
    DWORD* renderBits_ = nullptr;
    int renderBitmapWidth_ = 0;
    int renderBitmapHeight_ = 0;
    ULONGLONG desktopBackdropGeneration_ = 0;
    size_t glassBaseCachePixels_ = 0;
    std::map<HWND, GlassBaseCacheEntry> glassBaseCaches_;
    ConfigStore config_;
    GlobalStyle globalStyle_;
    DesktopGrid desktopGrid_;
    HWND consoleWindow_ = nullptr;
    HWND consoleOpacitySlider_ = nullptr;
    HWND consoleBlurSlider_ = nullptr;
    HWND consoleCornerSlider_ = nullptr;
    HWND consoleOpacityValue_ = nullptr;
    HWND consoleBlurValue_ = nullptr;
    HWND consoleCornerValue_ = nullptr;
    HWND consoleTintColorButton_ = nullptr;
    HWND consoleTextColorButton_ = nullptr;
    HFONT consoleFont_ = nullptr;
    bool consoleUpdatePending_ = false;
    bool consolePersistPending_ = false;
    std::vector<std::unique_ptr<ContainerWindow>> containers_;
    ContainerWindow* desktopFrontContainer_ = nullptr;
    std::map<std::wstring, HICON, std::less<>> icons_;
    std::vector<DesktopPlacementRequest> pendingDesktopPlacements_;
    UINT taskbarCreatedMessage_ = 0;
    UINT shellHookMessage_ = 0;
    HHOOK mouseHook_ = nullptr;
    DWORD lastNewGroupCookie_ = 0;
    bool saveDirty_ = false;
    bool saveFailureNotified_ = false;
    bool containersShown_ = true;
    bool exiting_ = false;
};

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && gMouseHookApplication) {
        const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (mouse) {
            // Keep the hook callback short. The main UI thread owns container
            // Z-order and decides whether this screen point ends the session.
            PostMessageW(gMouseHookApplication->MessageWindow(),
                         kDismissInteractionMessage,
                         static_cast<WPARAM>(static_cast<INT_PTR>(mouse->pt.x)),
                         static_cast<LPARAM>(static_cast<INT_PTR>(mouse->pt.y)));
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

ContainerWindow::~ContainerWindow() {
    DestroyDragGhost();
    DestroyDragPlaceholder();
    DestroyMoveTargetPreview();
    DestroyResizePreview();
    DestroyResizeAnimationSurface();
    if (settingsWindow_ && IsWindow(settingsWindow_)) DestroyWindow(settingsWindow_);
    if (tooltipWindow_ && IsWindow(tooltipWindow_)) DestroyWindow(tooltipWindow_);
    if (hoverWindow_ && IsWindow(hoverWindow_)) DestroyWindow(hoverWindow_);
    if (hoverFont_) DeleteObject(hoverFont_);
    if (contentWindow_ && IsWindow(contentWindow_)) DestroyWindow(contentWindow_);
    if (tintWindow_ && IsWindow(tintWindow_)) DestroyWindow(tintWindow_);
    if (blurWindow_ && IsWindow(blurWindow_)) DestroyWindow(blurWindow_);
    if (settingsFont_) DeleteObject(settingsFont_);
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
}

bool ContainerWindow::Create(HINSTANCE instance) {
    UpdateMetrics();
    if (state_.id.empty()) state_.id = NewId();
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kContainerClass,
        state_.name.c_str(),
        WS_POPUP,
        state_.bounds.left,
        state_.bounds.top,
        OuterWidth(HalfColumns()),
        OuterHeight(HalfRows()),
        nullptr,
        nullptr,
        instance,
        this);
    if (!window_) return false;
    // The input surface is fully transparent; the per-pixel tint window below
    // handles hit testing, so the rectangular backing can never leak black.
    SetLayeredWindowAttributes(window_, 0, 0, LWA_ALPHA);
    RECT client{};
    GetClientRect(window_, &client);
    RECT screenRect{};
    GetWindowRect(window_, &screenRect);
    tintWindow_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                  kTintClass, nullptr, WS_POPUP,
                                  screenRect.left, screenRect.top, client.right, client.bottom,
                                  window_, nullptr, instance, this);
    if (!tintWindow_) return false;
    constexpr DWORD kNcRenderingPolicy = 2;
    constexpr int kNcRenderingDisabled = 1;
    constexpr DWORD kTransitionsForcedDisabled = 3;
    constexpr DWORD kWindowCornerPreference = 33;
    constexpr int kDoNotRound = 1;
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(window_, kNcRenderingPolicy, &kNcRenderingDisabled, sizeof(kNcRenderingDisabled));
    DwmSetWindowAttribute(window_, kTransitionsForcedDisabled, &disableTransitions, sizeof(disableTransitions));
    DwmSetWindowAttribute(window_, kWindowCornerPreference, &kDoNotRound, sizeof(kDoNotRound));
    DwmSetWindowAttribute(tintWindow_, kNcRenderingPolicy, &kNcRenderingDisabled, sizeof(kNcRenderingDisabled));
    DwmSetWindowAttribute(tintWindow_, kTransitionsForcedDisabled, &disableTransitions, sizeof(disableTransitions));
    DwmSetWindowAttribute(tintWindow_, kWindowCornerPreference, &kDoNotRound, sizeof(kDoNotRound));
    DragAcceptFiles(window_, TRUE);
    DragAcceptFiles(tintWindow_, TRUE);
    UpdateShape();
    EnsureCapacity(false);
    EnsureVisible();
    if (state_.snapToGrid) {
        RECT rect{};
        GetWindowRect(window_, &rect);
        SnapRectToDesktopGrid(rect);
        SetWindowPos(window_, nullptr, rect.left, rect.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    ShowWindow(blurWindow_, SW_HIDE);
    UpdateNativeBlurLayer();
    ShowWindow(tintWindow_, SW_SHOWNOACTIVATE);
    SyncContentWindow();
    UpdateWindow(window_);
    UpdateTooltips();
    return true;
}

void ContainerWindow::Show(bool visible) {
    ShowWindow(window_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (blurWindow_) {
        if (visible) UpdateNativeBlurLayer();
        else ShowWindow(blurWindow_, SW_HIDE);
    }
    if (tintWindow_) ShowWindow(tintWindow_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (contentWindow_) ShowWindow(contentWindow_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (visible) {
        SyncDesktopLayer();
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
}

void ContainerWindow::SyncContentWindow() {
    if (!window_) return;
    SyncDesktopLayer();
    if (!suppressRefresh_) {
        UpdateGlass();
        if (contentWindow_) InvalidateRect(contentWindow_, nullptr, TRUE);
    }
}

HWND ContainerWindow::TopmostSiblingSurface() const {
    for (HWND candidate = GetTopWindow(nullptr); candidate;
         candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (candidate == window_ || candidate == blurWindow_ || candidate == tintWindow_ || candidate == contentWindow_)
            continue;
        if (IsOrganizerDesktopSurface(candidate)) return candidate;
    }
    return nullptr;
}

void ContainerWindow::SyncDesktopLayer(bool includeContainer) {
    if (!window_) return;

    // A pressed container temporarily lives above normal applications. Its
    // placement must not be rebuilt into the desktop band until the pointer
    // gesture ends, otherwise a drag frame would immediately undo the raise.
    if (interactionTopmost_) return;

    // Every container remains in the ordinary desktop band. Interaction may
    // raise a group only relative to sibling groups; application windows must
    // always remain above it.
    auto restoreNoActivate = [](HWND surface) {
        if (!surface) return;
        const LONG_PTR style = GetWindowLongPtrW(surface, GWL_EXSTYLE);
        if (style & WS_EX_NOACTIVATE) return;
        SetWindowLongPtrW(surface, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
        SetWindowPos(surface, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    };
    restoreNoActivate(window_);
    restoreNoActivate(tintWindow_);
    RECT rect{};
    GetWindowRect(window_, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    // During an interactive move/resize UpdateLayeredWindow moves tintWindow_
    // together with its freshly rendered bitmap.  Moving it separately here
    // would briefly show the old bitmap at the new location.
    const bool deferTint = (pointerActive_ && (moving_ || sizingPreview_)) ||
                           rectAnimationActive_;

    const bool leavingTopmostBand = tintWindow_ &&
        (GetWindowLongPtrW(tintWindow_, GWL_EXSTYLE) & WS_EX_TOPMOST);
    auto leaveTopmostBand = [](HWND surface) {
        if (!surface ||
            !(GetWindowLongPtrW(surface, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;
        SetWindowPos(surface, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    // Clear TOPMOST left by older builds before rebuilding the desktop stack.
    leaveTopmostBand(tintWindow_);
    leaveTopmostBand(contentWindow_);
    leaveTopmostBand(blurWindow_);
    leaveTopmostBand(window_);
    ContainerWindow* front = app_.DesktopFrontContainer();
    const bool frontIsOrdinaryDesktopSurface = front && front != this &&
        front->window_ && IsWindow(front->window_);
    // An isolated refresh of an older group must stay behind the most recently
    // interacted group. Otherwise repaint/shell repair order silently
    // overwrites the user's Z-order choice.
    HWND baseAnchor = frontIsOrdinaryDesktopSurface
        ? front->window_
        : DesktopBandInsertAfter(app_.DesktopLayerHost());

    // Only the invisible input owner participates in the desktop-level Z
    // placement.  The helper surfaces are owned popups and stay above it.
    if (includeContainer) {
        SetWindowPos(window_, baseAnchor, rect.left, rect.top, width, height,
                     SWP_NOACTIVATE);
    }

    // Put every visible helper into the same desktop band as the input owner.
    // Positioning them all after baseAnchor in back-to-front creation order
    // keeps the final tint/icon surface above the invisible input window while
    // ordinary applications remain above the complete group.
    const UINT helperFlags = SWP_NOACTIVATE;
    if (blurWindow_ && !deferTint)
        SetWindowPos(blurWindow_, baseAnchor, rect.left, rect.top, width, height, helperFlags);
    if (contentWindow_)
        SetWindowPos(contentWindow_, baseAnchor, rect.left, rect.top, width, height, helperFlags);
    if (tintWindow_ && !deferTint)
        SetWindowPos(tintWindow_, baseAnchor, rect.left, rect.top, width, height, helperFlags);

    if (app_.DesktopFrontContainer() == this) {
        // Insert immediately above the highest sibling group, while keeping
        // whichever application/desktop window already precedes it above us.
        // Passing the sibling itself as hWndInsertAfter actually places this
        // group below it, which is especially visible for owned tint popups.
        if (HWND sibling = TopmostSiblingSurface();
            sibling && PrecedesInTopLevelZOrder(sibling, app_.DesktopLayerHost())) {
            HWND aboveSibling = GetWindow(sibling, GW_HWNDPREV);
            while (aboveSibling == window_ || aboveSibling == blurWindow_ ||
                   aboveSibling == contentWindow_ || aboveSibling == tintWindow_) {
                aboveSibling = GetWindow(aboveSibling, GW_HWNDPREV);
            }
            if (!aboveSibling) aboveSibling = HWND_TOP;
            SetWindowPos(window_, aboveSibling, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            // During a drag, keep the current bitmap at its existing
            // coordinates but still lift the visible glass face with its
            // owner. Skipping the Z-order update made only the transparent
            // input window rise above sibling groups.
            if (tintWindow_)
                SetWindowPos(tintWindow_, aboveSibling, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    // The glass popup is the visible and hit-tested face of the group. Owned
    // layered windows can retain the reverse internal order after leaving the
    // TOPMOST band, so on that transition keep the transparent message owner
    // behind the glass face.
    if (leavingTopmostBand && tintWindow_ && !deferTint) {
        SetWindowPos(window_, tintWindow_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Front -> back must be: tint/content -> blur -> invisible input owner.
    // hWndInsertAfter is the window that PRECEDES the positioned window, so
    // positioning blurWindow_ after tintWindow_ puts blur behind the content.
    if (blurWindow_ && tintWindow_ && IsWindowVisible(blurWindow_)) {
        SetWindowPos(blurWindow_, tintWindow_, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void ContainerWindow::RaiseForPointerInteraction() {
    if (interactionTopmost_ || !window_) return;
    interactionTopmost_ = true;
    RECT rect{};
    GetWindowRect(window_, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const UINT flags = SWP_NOACTIVATE;
    // tintWindow_ is the actual visible and hit-tested surface. The fully
    // transparent owner deliberately stays in the desktop band, while this
    // surface remains over ordinary apps until the user clicks elsewhere.
    if (blurWindow_ && IsWindowVisible(blurWindow_))
        SetWindowPos(blurWindow_, HWND_TOPMOST, rect.left, rect.top, width, height, flags);
    if (contentWindow_)
        SetWindowPos(contentWindow_, HWND_TOPMOST, rect.left, rect.top, width, height, flags);
    if (tintWindow_)
        SetWindowPos(tintWindow_, HWND_TOPMOST, rect.left, rect.top, width, height, flags);
    SetWindowPos(window_, HWND_TOP, rect.left, rect.top, width, height, flags);
}

void ContainerWindow::RestoreFromPointerInteraction() {
    if (!interactionTopmost_) return;
    interactionTopmost_ = false;
    auto leaveTopmost = [](HWND surface) {
        if (!surface || !(GetWindowLongPtrW(surface, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;
        SetWindowPos(surface, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    leaveTopmost(tintWindow_);
    leaveTopmost(contentWindow_);
    leaveTopmost(blurWindow_);
    leaveTopmost(window_);
    SyncDesktopLayer();
}

void ContainerWindow::UpdateGlass() {
    if (!window_ || !tintWindow_) return;
    RECT rect{};
    GetWindowRect(window_, &rect);
    UpdateNativeBlurLayer();
    app_.RenderGlass(tintWindow_, rect, state_, dpi_, sizingPreview_, paintedHoverItem_,
                     itemAnimationIndex_, itemAnimationOffset_, draggingItem_,
                     pointerActive_ && (moving_ || sizingPreview_),
                     state_.collapsed ? (pointerActive_ ? 2 : (hoverCollapsed_ ? 1 : 0)) : 0);
}

void ContainerWindow::UpdateDragPlaceholder() {
    if (!dragPreviewPlacement_ || !window_ || !tintWindow_) {
        DestroyDragPlaceholder();
        return;
    }

    const int halfUnit = std::max(1, unit_ / 2);
    const int halfUnitH = ContentHalfUnitHeight(state_.rows);
    const int width = state_.collapsed
        ? CollapsedExtent() : std::max(1, dragPreviewPlacement_->span * halfUnit);
    const int height = state_.collapsed
        ? CollapsedExtent() : std::max(1, dragPreviewPlacement_->span * halfUnitH);
    RECT container{};
    GetWindowRect(window_, &container);
    const int x = state_.collapsed
        ? container.left : container.left + padding_ + dragPreviewPlacement_->x * halfUnit;
    const int y = state_.collapsed
        ? container.top : container.top + titleHeight_ + dragPreviewPlacement_->y * halfUnitH;

    if (dragPlaceholderWindow_ && dragPlaceholderSpan_ == dragPreviewPlacement_->span) {
        SetWindowPos(dragPlaceholderWindow_, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW |
                         SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
        return;
    }

    DestroyDragPlaceholder();
    dragPlaceholderWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kDragClass, nullptr, WS_POPUP, x, y, width, height,
        window_, nullptr, GetModuleHandleW(nullptr), this);
    if (!dragPlaceholderWindow_) return;
    dragPlaceholderSpan_ = dragPreviewPlacement_->span;

    HDC screenDc = GetDC(nullptr);
    HDC layer = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = screenDc
        ? CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0) : nullptr;
    if (!screenDc || !layer || !bitmap || !rawPixels) {
        if (bitmap) DeleteObject(bitmap);
        if (layer) DeleteDC(layer);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        DestroyDragPlaceholder();
        return;
    }

    HGDIOBJ previous = SelectObject(layer, bitmap);
    auto* pixels = static_cast<DWORD*>(rawPixels);
    const int inset = ScaleDip(5, dpi_);
    const double radius = ScaleDip(14, dpi_);
    const int border = std::max(1, ScaleDip(1, dpi_));
    const RECT previewShape = state_.collapsed
        ? desktop_grid_geometry::MakeCollapsedLayout(width, height, dpi_).tile
        : RECT{0, 0, width, height};
    const auto coverage = [&](double px, double py, double edgeInset) {
        const double left = previewShape.left + inset + edgeInset;
        const double top = previewShape.top + inset + edgeInset;
        const double right = previewShape.right - inset - edgeInset;
        const double bottom = previewShape.bottom - inset - edgeInset;
        const double shapeWidth = right - left;
        const double shapeHeight = bottom - top;
        if (shapeWidth <= 0.0 || shapeHeight <= 0.0) return 0.0;
        const double shapeRadius = std::clamp(radius - edgeInset, 0.0,
            std::min(shapeWidth, shapeHeight) / 2.0);
        const double qx = std::abs(px - (left + right) / 2.0) - (shapeWidth / 2.0 - shapeRadius);
        const double qy = std::abs(py - (top + bottom) / 2.0) - (shapeHeight / 2.0 - shapeRadius);
        // hypot is only needed in the rounded corner octants; everywhere else
        // the Euclidean term collapses to max(qx, qy, 0), which is exact.
        const double ax = std::max(qx, 0.0);
        const double ay = std::max(qy, 0.0);
        const double euclidean = (ax > 0.0 && ay > 0.0) ? std::hypot(ax, ay) : std::max(ax, ay);
        const double distance = euclidean + std::min(std::max(qx, qy), 0.0) - shapeRadius;
        return std::clamp(0.5 - distance, 0.0, 1.0);
    };
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const double outer = coverage(column + 0.5, row + 0.5, 0.0);
            const double inner = coverage(column + 0.5, row + 0.5, border);
            const double fillWeight = 54.0 * inner;
            const double borderWeight = 150.0 * std::max(0.0, outer - inner);
            const double alphaValue = fillWeight + borderWeight;
            const DWORD alpha = static_cast<DWORD>(std::clamp(std::lround(alphaValue), 0L, 255L));
            const int blue = static_cast<int>(std::lround((244.0 * fillWeight + 215.0 * borderWeight) / 255.0));
            const int green = static_cast<int>(std::lround((236.0 * fillWeight + 198.0 * borderWeight) / 255.0));
            const int red = static_cast<int>(std::lround((189.0 * fillWeight + 112.0 * borderWeight) / 255.0));
            pixels[static_cast<size_t>(row) * width + column] =
                PixelFromChannels(blue, green, red) | (alpha << 24);
        }
    }

    POINT destination{x, y};
    POINT origin{};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(dragPlaceholderWindow_, screenDc, &destination, &size,
                        layer, &origin, 0, &blend, ULW_ALPHA);
    SelectObject(layer, previous);
    DeleteObject(bitmap);
    DeleteDC(layer);
    ReleaseDC(nullptr, screenDc);
    SetOverlayPosition(dragPlaceholderWindow_, tintWindow_, x, y, width, height,
                       SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
}

void ContainerWindow::DestroyDragPlaceholder() {
    if (dragPlaceholderWindow_ && IsWindow(dragPlaceholderWindow_)) DestroyWindow(dragPlaceholderWindow_);
    dragPlaceholderWindow_ = nullptr;
    dragPlaceholderSpan_ = 0;
}

void ContainerWindow::UpdateMoveTargetPreview(const RECT& freeMoveRect) {
    if (!state_.snapToGrid || !window_ || !tintWindow_) {
        DestroyMoveTargetPreview();
        return;
    }
    const Application::DesktopGrid& grid = app_.GetDesktopGrid();
    const int width = freeMoveRect.right - freeMoveRect.left;
    const int height = freeMoveRect.bottom - freeMoveRect.top;
    // Same pitch rule as SnapRectToDesktopGrid: never smaller than the
    // window, so the preview matches the final snapped position and wide
    // collapsed cards cannot land on top of each other.
    const int pitchX = std::max(cellW_, width);
    const int pitchY = std::max(cellH_, height);
    POINT targetCell{};
    if (moveTargetCell_) {
        targetCell.x = desktop_grid_geometry::SnapIndexWithHysteresis(
            freeMoveRect.left, grid.originX, pitchX, moveTargetCell_->x);
        targetCell.y = desktop_grid_geometry::SnapIndexWithHysteresis(
            freeMoveRect.top, grid.originY, pitchY, moveTargetCell_->y);
    } else {
        targetCell.x = desktop_grid_geometry::SnapIndex(freeMoveRect.left, grid.originX, pitchX);
        targetCell.y = desktop_grid_geometry::SnapIndex(freeMoveRect.top, grid.originY, pitchY);
    }
    const RECT work = WorkAreaForRect(freeMoveRect);
    const int minCellX = static_cast<int>(std::ceil(
        static_cast<double>(work.left - grid.originX) / pitchX));
    const int maxCellX = static_cast<int>(std::floor(
        static_cast<double>(work.right - width - grid.originX) / pitchX));
    const int minCellY = static_cast<int>(std::ceil(
        static_cast<double>(work.top - grid.originY) / pitchY));
    const int maxCellY = static_cast<int>(std::floor(
        static_cast<double>(work.bottom - height - grid.originY) / pitchY));
    if (maxCellX >= minCellX)
        targetCell.x = std::clamp<LONG>(targetCell.x, minCellX, maxCellX);
    if (maxCellY >= minCellY)
        targetCell.y = std::clamp<LONG>(targetCell.y, minCellY, maxCellY);
    moveTargetCell_ = targetCell;
    RECT target{
        grid.originX + targetCell.x * pitchX,
        grid.originY + targetCell.y * pitchY,
        0,
        0,
    };
    target.right = target.left + width;
    target.bottom = target.top + height;
    if (moveTargetRect_ && EqualRect(&*moveTargetRect_, &target) && moveTargetWindow_) return;
    moveTargetRect_ = target;

    if (moveTargetWindow_) {
        SetWindowPos(moveTargetWindow_, nullptr, target.left, target.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW |
                         SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
        return;
    }

    moveTargetWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kDragClass, nullptr, WS_POPUP, target.left, target.top, width, height,
        window_, nullptr, GetModuleHandleW(nullptr), this);
    if (!moveTargetWindow_) return;

    HDC screenDc = GetDC(nullptr);
    HDC layer = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HBITMAP bitmap = screenDc
        ? CreateDIBSection(screenDc, &info, DIB_RGB_COLORS,
                           reinterpret_cast<void**>(&pixels), nullptr, 0) : nullptr;
    if (!screenDc || !layer || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (layer) DeleteDC(layer);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        DestroyMoveTargetPreview();
        return;
    }
    HGDIOBJ previous = SelectObject(layer, bitmap);
    std::fill_n(pixels, static_cast<size_t>(width) * height, 0u);
    RECT shape = state_.collapsed
        ? RECT{0, 0, width, height}
        : RECT{ScaleDip(3, dpi_), ScaleDip(3, dpi_),
               width - ScaleDip(3, dpi_), height - ScaleDip(3, dpi_)};
    const int border = std::max(1, ScaleDip(1, dpi_));
    const double radius = std::min<double>(ScaleDip(state_.cornerRadius, dpi_),
                                           std::min(shape.right - shape.left,
                                                    shape.bottom - shape.top) / 2.0);
    const auto coverage = [&](double px, double py, double inset) {
        const double left = shape.left + inset;
        const double top = shape.top + inset;
        const double right = shape.right - inset;
        const double bottom = shape.bottom - inset;
        const double shapeRadius = std::max(0.0, radius - inset);
        const double qx = std::abs(px - (left + right) / 2.0) -
                          ((right - left) / 2.0 - shapeRadius);
        const double qy = std::abs(py - (top + bottom) / 2.0) -
                          ((bottom - top) / 2.0 - shapeRadius);
        const double ax = std::max(qx, 0.0);
        const double ay = std::max(qy, 0.0);
        const double distance = ((ax > 0.0 && ay > 0.0) ? std::hypot(ax, ay) : std::max(ax, ay)) +
                                std::min(std::max(qx, qy), 0.0) - shapeRadius;
        return std::clamp(0.5 - distance, 0.0, 1.0);
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double outer = coverage(x + 0.5, y + 0.5, 0.0);
            const double inner = coverage(x + 0.5, y + 0.5, border);
            const double fillWeight = 42.0 * inner;
            const double borderWeight = 156.0 * std::max(0.0, outer - inner);
            const int alpha = std::clamp(static_cast<int>(std::lround(
                fillWeight + borderWeight)), 0, 255);
            const int blue = static_cast<int>(std::lround(
                (244.0 * fillWeight + 215.0 * borderWeight) / 255.0));
            const int green = static_cast<int>(std::lround(
                (247.0 * fillWeight + 203.0 * borderWeight) / 255.0));
            const int red = static_cast<int>(std::lround(
                (221.0 * fillWeight + 121.0 * borderWeight) / 255.0));
            pixels[static_cast<size_t>(y) * width + x] =
                PixelFromChannels(blue, green, red) | (static_cast<DWORD>(alpha) << 24);
        }
    }
    POINT destination{target.left, target.top};
    POINT origin{};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(moveTargetWindow_, screenDc, &destination, &size,
                        layer, &origin, 0, &blend, ULW_ALPHA);
    SelectObject(layer, previous);
    DeleteObject(bitmap);
    DeleteDC(layer);
    ReleaseDC(nullptr, screenDc);
    SetOverlayPosition(moveTargetWindow_, tintWindow_, target.left, target.top, width, height,
                       SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
}

void ContainerWindow::DestroyMoveTargetPreview() {
    if (moveTargetWindow_ && IsWindow(moveTargetWindow_)) DestroyWindow(moveTargetWindow_);
    moveTargetWindow_ = nullptr;
    moveTargetRect_.reset();
    moveTargetCell_.reset();
}

void ContainerWindow::UpdateResizePreview(const RECT& target, int halfColumns, int halfRows) {
    if (!window_ || !tintWindow_) return;
    RECT current{};
    if (!resizePreviewWindow_) {
        // Begin at the real card. The target is still the snapped grid size,
        // but the after-image now has a visible path to it instead of appearing
        // one whole grid step away on the threshold-crossing mouse message.
        if (!GetWindowRect(window_, &current)) return;
        const int width = current.right - current.left;
        const int height = current.bottom - current.top;
        if (width <= 0 || height <= 0) return;
        resizePreviewWindow_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"STATIC", L"DesktopOrganizer.ResizePreview", WS_POPUP | SS_WHITERECT,
            current.left, current.top, width, height,
            window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!resizePreviewWindow_) return;
        // A faint white sheet marks the snapped destination while the real
        // glass card remains untouched. It reads as the soft after-image seen
        // during mobile folder resizing and costs almost nothing to move.
        SetLayeredWindowAttributes(resizePreviewWindow_, 0, 34, LWA_ALPHA);
    } else if (!GetWindowRect(resizePreviewWindow_, &current)) {
        return;
    }

    resizePreviewRect_ = target;
    resizePreviewHalfColumns_ = halfColumns;
    resizePreviewHalfRows_ = halfRows;
    if (EqualRect(&current, &target)) {
        resizePreviewDisplayRect_ = target;
        resizePreviewAnimationActive_ = false;
        KillTimer(window_, kResizePreviewTimer);
        ApplyResizePreviewRect(target);
        if (resizePreviewDestroyWhenFinished_) DestroyResizePreview();
        return;
    }

    // Retarget from the sheet's currently displayed rectangle. Repeated mouse
    // moves inside one snapped step never call this function, and crossing a
    // new step continues from the in-flight frame instead of restarting from
    // the real container.
    resizePreviewAnimationFrom_ = current;
    resizePreviewAnimationStarted_ = GetTickCount64();
    resizePreviewAnimationActive_ = true;
    const interaction_motion::Rect leadIn = interaction_motion::AnimateRect(
        {current.left, current.top, current.right, current.bottom},
        {target.left, target.top, target.right, target.bottom},
        static_cast<double>(kInteractiveFrameMs), kResizePreviewAnimationDurationMs);
    ApplyResizePreviewRect({leadIn.left, leadIn.top, leadIn.right, leadIn.bottom});
    SetTimer(window_, kResizePreviewTimer, kInteractiveFrameMs, nullptr);
}

void ContainerWindow::ApplyResizePreviewRect(const RECT& rect) {
    if (!resizePreviewWindow_ || !tintWindow_) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    const int radius = std::clamp(ScaleDip(state_.cornerRadius, dpi_), 0,
                                  std::min(width, height) / 2);
    HRGN region = radius > 0
        ? CreateRoundRectRgn(0, 0, width + 1, height + 1,
                             radius * 2 + 1, radius * 2 + 1)
        : CreateRectRgn(0, 0, width, height);
    if (region && !SetWindowRgn(resizePreviewWindow_, region, FALSE))
        DeleteObject(region);
    SetOverlayPosition(resizePreviewWindow_, tintWindow_, rect.left, rect.top,
                       width, height, SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    resizePreviewDisplayRect_ = rect;
    // Invalidation is enough for this solid lightweight sheet. Forcing an
    // immediate synchronous paint on every 8 ms timer tick stalls pointer
    // dispatch and was one contributor to the old uneven feel.
    RedrawWindow(resizePreviewWindow_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE);
}

void ContainerWindow::StepResizePreviewAnimation() {
    if (!resizePreviewAnimationActive_ || !resizePreviewRect_ || !window_) return;
    const ULONGLONG elapsed = GetTickCount64() - resizePreviewAnimationStarted_;
    const RECT target = *resizePreviewRect_;
    const interaction_motion::Rect sampled = interaction_motion::AnimateRect(
        {resizePreviewAnimationFrom_.left, resizePreviewAnimationFrom_.top,
         resizePreviewAnimationFrom_.right, resizePreviewAnimationFrom_.bottom},
        {target.left, target.top, target.right, target.bottom},
        static_cast<double>(elapsed), kResizePreviewAnimationDurationMs);
    RECT frame{sampled.left, sampled.top, sampled.right, sampled.bottom};
    const bool finished = static_cast<double>(elapsed) >= kResizePreviewAnimationDurationMs;
    if (finished) frame = target;
    ApplyResizePreviewRect(frame);
    if (finished) {
        resizePreviewAnimationActive_ = false;
        KillTimer(window_, kResizePreviewTimer);
        if (resizePreviewDestroyWhenFinished_) DestroyResizePreview();
    }
}

void ContainerWindow::DestroyResizePreview() {
    if (window_) KillTimer(window_, kResizePreviewTimer);
    if (resizePreviewWindow_ && IsWindow(resizePreviewWindow_))
        DestroyWindow(resizePreviewWindow_);
    resizePreviewWindow_ = nullptr;
    resizePreviewRect_.reset();
    resizePreviewDisplayRect_.reset();
    resizePreviewAnimationActive_ = false;
    resizePreviewDestroyWhenFinished_ = false;
    resizePreviewAnimationStarted_ = 0;
    resizePreviewHalfColumns_ = 0;
    resizePreviewHalfRows_ = 0;
}

void ContainerWindow::UpdateResizeAnimationSurface(const RECT& rect) {
    if (!window_ || !tintWindow_) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    if (!resizeAnimationWindow_) {
        resizeAnimationWindow_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"STATIC", L"DesktopOrganizer.ResizeAnimation", WS_POPUP | SS_WHITERECT,
            rect.left, rect.top, width, height,
            window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!resizeAnimationWindow_) return;
        SetLayeredWindowAttributes(resizeAnimationWindow_, 0, 78, LWA_ALPHA);
    }
    const int radius = std::clamp(ScaleDip(state_.cornerRadius, dpi_), 0,
                                  std::min(width, height) / 2);
    HRGN region = radius > 0
        ? CreateRoundRectRgn(0, 0, width + 1, height + 1,
                             radius * 2 + 1, radius * 2 + 1)
        : CreateRectRgn(0, 0, width, height);
    if (region && !SetWindowRgn(resizeAnimationWindow_, region, FALSE))
        DeleteObject(region);
    HWND anchor = resizePreviewWindow_ ? resizePreviewWindow_ : tintWindow_;
    SetOverlayPosition(resizeAnimationWindow_, anchor, rect.left, rect.top,
                       width, height, SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
}

void ContainerWindow::DestroyResizeAnimationSurface() {
    if (resizeAnimationWindow_ && IsWindow(resizeAnimationWindow_))
        DestroyWindow(resizeAnimationWindow_);
    resizeAnimationWindow_ = nullptr;
}

void ContainerWindow::CreateDragGhost(size_t index, POINT screen) {
    DestroyDragGhost();
    if (index >= state_.items.size() || !window_) return;
    const int iconPixels = IconPixels(state_.items[index].iconSize);
    dragGhostWidth_ = iconPixels + ScaleDip(24, dpi_);
    dragGhostHeight_ = iconPixels + ScaleDip(24, dpi_);
    dragGhostWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kDragClass, nullptr, WS_POPUP,
        screen.x - dragGhostWidth_ / 2, screen.y - dragGhostHeight_ / 2,
        dragGhostWidth_, dragGhostHeight_, window_, nullptr, GetModuleHandleW(nullptr), this);
    if (!dragGhostWindow_) return;

    HDC screenDc = GetDC(nullptr);
    HDC layer = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = dragGhostWidth_;
    info.bmiHeader.biHeight = -dragGhostHeight_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = screenDc ? CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!screenDc || !layer || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (layer) DeleteDC(layer);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        DestroyDragGhost();
        return;
    }
    HGDIOBJ previous = SelectObject(layer, bitmap);
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(dragGhostWidth_) * dragGhostHeight_, 0u);
    if (HICON icon = app_.IconForPath(state_.items[index].path, iconPixels))
        DrawIconEx(layer, (dragGhostWidth_ - iconPixels) / 2, ScaleDip(12, dpi_), icon,
                   iconPixels, iconPixels, 0, nullptr, DI_NORMAL);
    auto* pixels = static_cast<DWORD*>(bits);
    for (size_t pixel = 0; pixel < static_cast<size_t>(dragGhostWidth_) * dragGhostHeight_; ++pixel) {
        if ((pixels[pixel] & 0xff000000u) == 0 && (pixels[pixel] & 0x00ffffffu) != 0)
            pixels[pixel] |= 0xff000000u;
    }
    POINT destination{screen.x - dragGhostWidth_ / 2, screen.y - dragGhostHeight_ / 2};
    POINT origin{};
    SIZE size{dragGhostWidth_, dragGhostHeight_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 235, AC_SRC_ALPHA};
    UpdateLayeredWindow(dragGhostWindow_, screenDc, &destination, &size, layer, &origin, 0, &blend, ULW_ALPHA);
    SelectObject(layer, previous);
    DeleteObject(bitmap);
    DeleteDC(layer);
    ReleaseDC(nullptr, screenDc);
    dragGhostZAnchor_ = tintWindow_ ? tintWindow_ : window_;
    SetOverlayPosition(dragGhostWindow_, dragGhostZAnchor_, destination.x, destination.y,
                       dragGhostWidth_, dragGhostHeight_, SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
}

void ContainerWindow::MoveDragGhost(POINT screen) {
    if (!dragGhostWindow_) return;
    HWND anchor = dragTargetPreview_ && dragTargetPreview_->dragPlaceholderWindow_
        ? dragTargetPreview_->dragPlaceholderWindow_
        : (dragPlaceholderWindow_ ? dragPlaceholderWindow_ : (tintWindow_ ? tintWindow_ : window_));
    const int x = screen.x - dragGhostWidth_ / 2;
    const int y = screen.y - dragGhostHeight_ / 2;
    if (anchor != dragGhostZAnchor_) {
        dragGhostZAnchor_ = anchor;
        SetOverlayPosition(dragGhostWindow_, anchor, x, y, 0, 0,
                           SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);
    } else {
        SetWindowPos(dragGhostWindow_, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW |
                         SWP_NOSENDCHANGING | SWP_NOOWNERZORDER);
    }
}

void ContainerWindow::DestroyDragGhost() {
    if (dragGhostWindow_ && IsWindow(dragGhostWindow_)) DestroyWindow(dragGhostWindow_);
    dragGhostWindow_ = nullptr;
    dragGhostZAnchor_ = nullptr;
    dragGhostWidth_ = 0;
    dragGhostHeight_ = 0;
}

void ContainerWindow::RefreshGlass() {
    UpdateGlass();
    if (contentWindow_) InvalidateRect(contentWindow_, nullptr, TRUE);
}

// Copies the global default style into this container and refreshes every
// affected surface. Mirrors the update logic in ApplySettingsFromControls.
void ContainerWindow::ApplyGlobalStyle(const GlobalStyle& style) {
    const bool titleChanged = state_.showTitle != style.showTitle;
    const bool cornerChanged = state_.cornerRadius != style.cornerRadius;
    const bool visualChanged = state_.opacity != style.opacity || state_.blur != style.blur ||
        cornerChanged || state_.tintMode != style.tintMode ||
        state_.tintColor != style.tintColor || state_.textColor != style.textColor ||
        titleChanged || state_.showBorder != style.showBorder;
    state_.opacity = style.opacity;
    state_.blur = style.blur;
    state_.cornerRadius = style.cornerRadius;
    state_.tintMode = style.tintMode;
    state_.tintColor = style.tintColor;
    state_.textColor = style.textColor;
    state_.showTitle = style.showTitle;
    state_.showBorder = style.showBorder;
    if (!window_ || !visualChanged) return;
    // Geometry and shape changes used to trigger their own refresh and were
    // followed by RefreshGlass(), rendering every follower twice per corner
    // slider step. Suppress those nested refreshes and publish one frame.
    suppressRefresh_ = true;
    if (titleChanged) {
        UpdateMetrics();
        SetWindowPos(window_, nullptr, 0, 0, OuterWidth(HalfColumns()), OuterHeight(HalfRows()),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (cornerChanged || titleChanged) UpdateShape();
    suppressRefresh_ = false;
    RefreshGlass();
    InvalidateRect(window_, nullptr, FALSE);
}

void ContainerWindow::UpdateMetrics() {
    dpi_ = window_ ? WindowDpi(window_) : 96;
    // The desktop grid metrics stay measured for the optional "embed in
    // desktop grid" mode, but the content lattice follows the original
    // product spec: a fixed 96 DIP unit with square half-unit cells, so
    // icons always render at the classic sizes regardless of Explorer's
    // current icon pitch.
    const Application::DesktopGrid& grid = app_.GetDesktopGrid();
    cellW_ = std::max(1, grid.cellW);
    cellH_ = std::max(1, grid.cellH);
    visualUnit_ = desktop_grid_geometry::VisualUnit(cellW_, cellH_);
    unit_ = ScaleDip(96, dpi_);
    padding_ = ScaleDip(kPaddingDip, dpi_);
    titleHeight_ = state_.collapsed ? 0 : (state_.showTitle ? ScaleDip(kTitleDip, dpi_) : padding_);
    resizeBorder_ = ScaleDip(kResizeBorderDip, dpi_);
    resizeCorner_ = ScaleDip(kResizeCornerDip, dpi_);
}

// A collapsed group is a fixed square preview card.
int ContainerWindow::CollapsedExtent() const { return ScaleDip(104, dpi_); }

int ContainerWindow::HalfColumns() const {
    return state_.columns * 2 + (state_.halfColumn ? 1 : 0);
}

int ContainerWindow::HalfRows() const {
    return state_.rows * 2 + (state_.halfRow ? 1 : 0);
}

// OuterWidth/OuterHeight take HALF-grid units: a 2 x 2 container is 4 x 4,
// and a 1.5 x 1.5 container is 3 x 3.
int ContainerWindow::OuterWidth(int halfColumns) const {
    return state_.collapsed ? CollapsedExtent() : halfColumns * (unit_ / 2) + padding_ * 2;
}

int ContainerWindow::OuterHeight(int halfRows) const {
    return state_.collapsed ? CollapsedExtent() : halfRows * (unit_ / 2) + titleHeight_ + padding_;
}

int ContainerWindow::ContentHalfUnitHeight(int rows) const {
    (void)rows;
    return std::max(1, unit_ / 2);
}

// Align the complete frame to Explorer's measured grid phase.
void ContainerWindow::SnapRectToDesktopGrid(RECT& rect) const {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const Application::DesktopGrid& grid = app_.GetDesktopGrid();
    // The placement pitch must be at least as large as the window itself:
    // Explorer's horizontal icon pitch (~76 px) is smaller than a collapsed
    // card (104 px), and snapping every card into one cell made horizontally
    // arranged groups overlap each other.
    const int pitchX = std::max(cellW_, width);
    const int pitchY = std::max(cellH_, height);
    rect.left = desktop_grid_geometry::SnapCoordinate(rect.left, grid.originX, pitchX);
    rect.top = desktop_grid_geometry::SnapCoordinate(rect.top, grid.originY, pitchY);
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

void ContainerWindow::SetCollapsed(bool collapsed, bool centerExpanded) {
    if (state_.collapsed == collapsed || !window_) return;
    diagnostic_log::Write(collapsed ? L"group.collapse" : L"group.expand",
                          L"group=" + state_.name + L" id=" + state_.id +
                              L" centered=" + (centerExpanded ? L"1" : L"0"));
    if (!collapsed && centerExpanded)
        app_.PrepareCenteredExpansion(this);
    app_.SetDesktopFrontContainer(this);
    RECT previous{};
    GetWindowRect(window_, &previous);
    const bool restoreCompactHome = collapsed && state_.centeredExpansionActive;
    if (!collapsed) {
        if (centerExpanded) {
            state_.centeredExpansionActive = true;
            state_.collapsedHome = {previous.left, previous.top};
        } else {
            state_.centeredExpansionActive = false;
        }
    }
    state_.collapsed = collapsed;
    UpdateMetrics();
    RECT rect = previous;
    rect.right = rect.left + OuterWidth(HalfColumns());
    rect.bottom = rect.top + OuterHeight(HalfRows());
    if (!collapsed && centerExpanded) {
        // Center on the monitor that contains the compact group. The taskbar is
        // excluded, and oversized groups remain pinned to the work-area origin.
        const RECT work = WorkAreaForRect(previous);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        rect.left = work.left + std::max(0L, (work.right - work.left - width) / 2);
        rect.top = work.top + std::max(0L, (work.bottom - work.top - height) / 2);
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
    } else if (restoreCompactHome) {
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        rect.left = state_.collapsedHome.x;
        rect.top = state_.collapsedHome.y;
        rect.right = rect.left + width;
        rect.bottom = rect.top + height;
        // If the remembered monitor disappeared, keep the compact card visible
        // on the nearest remaining work area instead of restoring off-screen.
        const RECT work = WorkAreaForRect(rect);
        if (rect.right > work.right) OffsetRect(&rect, work.right - rect.right, 0);
        if (rect.bottom > work.bottom) OffsetRect(&rect, 0, work.bottom - rect.bottom);
        if (rect.left < work.left) OffsetRect(&rect, work.left - rect.left, 0);
        if (rect.top < work.top) OffsetRect(&rect, 0, work.top - rect.top);
        state_.centeredExpansionActive = false;
    } else if (state_.snapToGrid) {
        SnapRectToDesktopGrid(rect);
    }
    SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (collapsed && state_.snapToGrid && state_.pushIcons)
        app_.PushDesktopIconsOutOf(rect, this);
    UpdateShape();
    SyncDesktopLayer();
    UpdateTooltips();
    UpdateStateBounds();
    app_.Save();
    app_.RefreshConsoleRows();
}

int ContainerWindow::IconPixels(IconSize size) const {
    switch (size) {
    case IconSize::Small: return ScaleDip(24, dpi_);
    case IconSize::Large: return ScaleDip(96, dpi_);
    default: return ScaleDip(48, dpi_);
    }
}

bool ContainerWindow::LayoutItems(int columns, int rows, std::vector<layout::Placement>& placements) const {
    // columns/rows are whole-grid counts here; fold the half-step extras in
    // and pack directly in half-cell units.
    return layout::ArrangeItemsInHalfUnits(
        columns * 2 + (state_.halfColumn ? 1 : 0),
        rows * 2 + (state_.halfRow ? 1 : 0), state_.items, &placements);
}

bool ContainerWindow::NormalizeItemPositions() {
    std::vector<layout::Placement> placements;
    if (!LayoutItems(state_.columns, state_.rows, placements)) return false;
    for (size_t index = 0; index < state_.items.size(); ++index) {
        state_.items[index].gridX = placements[index].x;
        state_.items[index].gridY = placements[index].y;
    }
    return true;
}

bool ContainerWindow::FindItemPlacement(size_t index, POINT client, layout::Placement& result) const {
    if (index >= state_.items.size()) return false;
    std::vector<layout::Placement> placements;
    if (!LayoutItems(state_.columns, state_.rows, placements)) return false;
    const int halfUnit = std::max(1, unit_ / 2);
    const int halfUnitH = ContentHalfUnitHeight(state_.rows);
    const int span = layout::Span(state_.items[index].iconSize);
    const int desiredX = static_cast<int>(std::lround(
        static_cast<double>(client.x - padding_) / halfUnit - span / 2.0));
    const int desiredY = static_cast<int>(std::lround(
        static_cast<double>(client.y - titleHeight_) / halfUnitH - span / 2.0));
    return layout::FindNearestFreeInHalfUnits(HalfColumns(), HalfRows(), placements, index,
                                              span, desiredX, desiredY, result);
}

bool ContainerWindow::FindFreePlacementForSpan(int span, POINT client, layout::Placement& result) const {
    std::vector<layout::Placement> placements;
    if (span <= 0 || !LayoutItems(state_.columns, state_.rows, placements)) return false;
    const int halfUnit = std::max(1, unit_ / 2);
    const int halfUnitH = ContentHalfUnitHeight(state_.rows);
    const int desiredX = static_cast<int>(std::lround(
        static_cast<double>(client.x - padding_) / halfUnit - span / 2.0));
    const int desiredY = static_cast<int>(std::lround(
        static_cast<double>(client.y - titleHeight_) / halfUnitH - span / 2.0));
    return layout::FindNearestFreeInHalfUnits(HalfColumns(), HalfRows(), placements, SIZE_MAX,
                                              span, desiredX, desiredY, result);
}

bool ContainerWindow::PlaceItemAt(size_t index, POINT client) {
    layout::Placement target{};
    if (!FindItemPlacement(index, client, target)) return false;
    state_.items[index].gridX = target.x;
    state_.items[index].gridY = target.y;
    return true;
}

void ContainerWindow::SetExternalDragPreview(int span, POINT client) {
    if (state_.locked) {
        ClearExternalDragPreview();
        return;
    }
    layout::Placement next{};
    const std::optional<layout::Placement> candidate = FindFreePlacementForSpan(span, client, next)
        ? std::optional<layout::Placement>(next) : std::nullopt;
    const bool changed = candidate.has_value() != dragPreviewPlacement_.has_value() ||
                         (candidate && (candidate->x != dragPreviewPlacement_->x ||
                                        candidate->y != dragPreviewPlacement_->y ||
                                        candidate->span != dragPreviewPlacement_->span));
    if (!changed) return;
    dragPreviewPlacement_ = candidate;
    UpdateDragPlaceholder();
}

void ContainerWindow::ClearExternalDragPreview() {
    if (!dragPreviewPlacement_ && !dragPlaceholderWindow_) return;
    dragPreviewPlacement_.reset();
    DestroyDragPlaceholder();
}

bool ContainerWindow::FitsHalf(int halfColumns, int halfRows) const {
    std::vector<layout::Placement> ignored;
    return layout::ArrangeItemsInHalfUnits(halfColumns, halfRows, state_.items, &ignored);
}

RECT ContainerWindow::ItemRect(size_t index) const {
    std::vector<layout::Placement> placements;
    if (!LayoutItems(state_.columns, state_.rows, placements) || index >= placements.size()) return {};
    const auto& placement = placements[index];
    const int halfUnit = unit_ / 2;
    const int halfUnitH = ContentHalfUnitHeight(state_.rows);
    return {padding_ + placement.x * halfUnit, titleHeight_ + placement.y * halfUnitH,
            padding_ + (placement.x + placement.span) * halfUnit,
            titleHeight_ + (placement.y + placement.span) * halfUnitH};
}

std::optional<size_t> ContainerWindow::HitItem(POINT client) const {
    if (state_.collapsed) return std::nullopt;
    std::vector<layout::Placement> placements;
    if (!LayoutItems(state_.columns, state_.rows, placements)) return std::nullopt;
    const int halfUnit = unit_ / 2;
    const int halfUnitH = ContentHalfUnitHeight(state_.rows);
    for (size_t index = 0; index < placements.size(); ++index) {
        const auto& placement = placements[index];
        const RECT rect{
            padding_ + placement.x * halfUnit,
            titleHeight_ + placement.y * halfUnitH,
            padding_ + (placement.x + placement.span) * halfUnit,
            titleHeight_ + (placement.y + placement.span) * halfUnitH};
        if (PtInRect(&rect, client)) return index;
    }
    return std::nullopt;
}

bool ContainerWindow::HitVisibleSurface(POINT client) const {
    RECT bounds{};
    GetClientRect(window_, &bounds);
    const int width = bounds.right;
    const int height = bounds.bottom;
    if (client.x < 0 || client.y < 0 || client.x >= width || client.y >= height) return false;
    const auto insideRounded = [](double x, double y, double left, double top,
                                  double right, double bottom, double corner) {
        if (x < left || y < top || x >= right || y >= bottom) return false;
        corner = std::clamp(corner, 0.0, std::min(right - left, bottom - top) / 2.0);
        const double nearestX = std::clamp(x, left + corner, right - corner);
        const double nearestY = std::clamp(y, top + corner, bottom - corner);
        const double dx = x - nearestX;
        const double dy = y - nearestY;
        return dx * dx + dy * dy <= corner * corner;
    };
    if (state_.collapsed) {
        // The collapsed card follows the same corner radius setting as the
        // expanded panel so the global console's radius slider applies to it.
        const double radius = std::min<double>(ScaleDip(state_.cornerRadius, dpi_),
                                               std::min(width, height) / 2.0);
        return insideRounded(client.x + 0.5, client.y + 0.5, 0, 0, width, height, radius);
    }
    const double radius = std::min<double>(ScaleDip(state_.cornerRadius, dpi_),
                                           std::min(width, height) / 2.0);
    return insideRounded(client.x + 0.5, client.y + 0.5, 0, 0, width, height, radius);
}

void ContainerWindow::PaintContent(HWND target) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(target, &paint);
    RECT client{};
    GetClientRect(target, &client);
    // Visual content is rendered into the glass layer so icons and text are
    // composited against the real backdrop instead of a color-key matte.
    HBRUSH keyBrush = CreateSolidBrush(RGB(1, 2, 3));
    FillRect(dc, &client, keyBrush);
    DeleteObject(keyBrush);
    EndPaint(target, &paint);
}

void ContainerWindow::UpdateShape() {
    // The glass surface already supplies an antialiased per-pixel alpha mask.
    // A second HRGN clip on the invisible input window leaves a black DWM
    // backing surface exposed at large radii.
    SetWindowRgn(window_, nullptr, TRUE);
    if (contentWindow_) SetWindowRgn(contentWindow_, nullptr, TRUE);
    if (tintWindow_) SetWindowRgn(tintWindow_, nullptr, TRUE);
    UpdateNativeBlurLayer();
    if (!suppressRefresh_) UpdateGlass();
}

void ContainerWindow::UpdateNativeBlurLayer() {
    // Blur is composited into tintWindow_ with the same per-pixel rounded
    // mask. The former native Accent helper was fixed-strength and leaked a
    // rectangular DWM surface beyond large custom corner radii.
    if (blurWindow_) ShowWindow(blurWindow_, SW_HIDE);
}

void ContainerWindow::UpdateStateBounds() {
    RECT rect{};
    GetWindowRect(window_, &rect);
    state_.bounds = rect;
    state_.monitor = MonitorDevice(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
}

void ContainerWindow::EnsureVisible() {
    RECT rect{};
    GetWindowRect(window_, &rect);
    const RECT work = WorkAreaForRect(rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    int x = std::clamp(rect.left, work.left, std::max(work.left, work.right - width));
    int y = std::clamp(rect.top, work.top, std::max(work.top, work.bottom - height));
    SetWindowPos(window_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SyncContentWindow();
    UpdateStateBounds();
}

bool ContainerWindow::EnsureCapacity(bool resizeWindow) {
    if (FitsHalf(HalfColumns(), HalfRows())) return true;
    RECT current{};
    if (window_) GetWindowRect(window_, &current);
    else current = state_.bounds;
    const RECT work = WorkAreaForRect(current);
    const int availableWidth = static_cast<int>(work.right - current.left) - padding_ * 2;
    const int availableHeight = static_cast<int>(work.bottom - current.top) - titleHeight_ - padding_;
    const int halfUnit = std::max(1, unit_ / 2);
    const int maxHalfColumns = std::max(1, availableWidth / halfUnit);
    const int maxHalfRows = std::max(1, availableHeight / halfUnit);
    int halfColumns = HalfColumns();
    int halfRows = HalfRows();
    while (!FitsHalf(halfColumns, halfRows)) {
        if (halfColumns < maxHalfColumns) ++halfColumns;
        else if (halfRows < maxHalfRows) ++halfRows;
        else return false;
    }
    state_.columns = halfColumns / 2;
    state_.halfColumn = (halfColumns % 2) != 0;
    state_.rows = halfRows / 2;
    state_.halfRow = (halfRows % 2) != 0;
    if (!NormalizeItemPositions()) return false;
    if (resizeWindow && window_) {
        SetWindowPos(window_, nullptr, 0, 0, OuterWidth(halfColumns), OuterHeight(halfRows),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SyncContentWindow();
        UpdateShape();
        UpdateStateBounds();
        UpdateTooltips();
    }
    return true;
}

bool ContainerWindow::RefreshAfterExternalChange() {
    const bool fits = EnsureCapacity(true);
    UpdateTooltips();
    UpdateGlass();
    InvalidateRect(window_, nullptr, TRUE);
    return fits;
}

void ContainerWindow::BeginPointerAction(POINT client) {
    app_.BeginContainerInteraction(this);
    if (state_.locked) return;
    FinishRectAnimation();
    DestroyDragGhost();
    DestroyMoveTargetPreview();
    DestroyResizePreview();
    DestroyResizeAnimationSurface();
    if (itemAnimationIndex_) {
        KillTimer(window_, 4);
        itemAnimationIndex_.reset();
        itemAnimationOffset_ = {};
        UpdateGlass();
    }
    dragPreviewPlacement_.reset();
    DestroyDragPlaceholder();
    pressedItem_ = HitItem(client);
    pointerActive_ = true;
    ClientToScreen(window_, &client);
    pointerStartScreen_ = client;
    GetWindowRect(window_, &pointerStartRect_);
    resizeEdges_ = 0;
    moving_ = false;
    gestureChanged_ = false;
    bypassSnapOnRelease_ = false;

    if (!pressedItem_) {
        if (state_.collapsed) {
            SetCapture(window_);
            // Collapsed preview card drags as a whole.
            UpdateGlass();
            return;
        }
        RECT clientRect{};
        GetClientRect(window_, &clientRect);
        POINT local = pointerStartScreen_;
        ScreenToClient(window_, &local);
        const bool nearLeftCorner = local.x < resizeCorner_;
        const bool nearRightCorner = local.x >= clientRect.right - resizeCorner_;
        const bool nearTopCorner = local.y < resizeCorner_;
        const bool nearBottomCorner = local.y >= clientRect.bottom - resizeCorner_;
        if (local.x < resizeBorder_ || ((nearTopCorner || nearBottomCorner) && nearLeftCorner)) resizeEdges_ |= 1;
        if (local.x >= clientRect.right - resizeBorder_ || ((nearTopCorner || nearBottomCorner) && nearRightCorner)) resizeEdges_ |= 2;
        if (local.y < resizeBorder_ || ((nearLeftCorner || nearRightCorner) && nearTopCorner)) resizeEdges_ |= 4;
        if (local.y >= clientRect.bottom - resizeBorder_ || ((nearLeftCorner || nearRightCorner) && nearBottomCorner)) resizeEdges_ |= 8;
    }
    SetCapture(window_);
}

void ContainerWindow::SnapMoveRect(RECT& rect) const {
    const int threshold = ScaleDip(kSnapDip, dpi_);
    const int alignmentReach = threshold * 2;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const RECT work = WorkAreaForRect(rect);

    std::optional<int> bestDx;
    std::optional<int> bestDy;
    auto consider = [&](std::optional<int>& best, int delta) {
        if (std::abs(delta) > threshold) return;
        if (!best || std::abs(delta) < std::abs(*best)) best = delta;
    };
    auto intervalGap = [](LONG aStart, LONG aEnd, LONG bStart, LONG bEnd) {
        if (aEnd < bStart) return bStart - aEnd;
        if (bEnd < aStart) return aStart - bEnd;
        return 0L;
    };

    consider(bestDx, work.left - rect.left);
    consider(bestDx, work.right - rect.right);
    consider(bestDy, work.top - rect.top);
    consider(bestDy, work.bottom - rect.bottom);

    for (const auto& other : app_.Containers()) {
        if (other.get() == this || !IsWindowVisible(other->Handle())) continue;
        RECT target{};
        GetWindowRect(other->Handle(), &target);
        const bool overlapsVertically = rect.bottom > target.top && rect.top < target.bottom;
        const bool overlapsHorizontally = rect.right > target.left && rect.left < target.right;
        const LONG verticalGap = intervalGap(rect.top, rect.bottom, target.top, target.bottom);
        const LONG horizontalGap = intervalGap(rect.left, rect.right, target.left, target.right);

        if (overlapsVertically) {
            consider(bestDx, target.right - rect.left);
            consider(bestDx, target.left - rect.right);
        }
        if (verticalGap <= alignmentReach) {
            consider(bestDx, target.left - rect.left);
            consider(bestDx, target.right - rect.right);
        }
        if (overlapsHorizontally) {
            consider(bestDy, target.bottom - rect.top);
            consider(bestDy, target.top - rect.bottom);
        }
        if (horizontalGap <= alignmentReach) {
            consider(bestDy, target.top - rect.top);
            consider(bestDy, target.bottom - rect.bottom);
        }
    }

    OffsetRect(&rect, bestDx.value_or(0), bestDy.value_or(0));
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

void ContainerWindow::StartRectAnimation(const RECT& target) {
    if (!window_) return;
    RECT current{};
    GetWindowRect(window_, &current);
    if (EqualRect(&current, &target)) {
        if (rectAnimationActive_) {
            rectAnimationActive_ = false;
            KillTimer(window_, 3);
        }
        // Nothing to redraw: the panel already sits on the target. Skipping
        // the frame avoids a full re-render on every mouse move that stays
        // inside the same grid step while the pointer hugs a resize edge.
        return;
    }
    const interaction_motion::Rect activeTarget{
        rectAnimationTarget_.left, rectAnimationTarget_.top,
        rectAnimationTarget_.right, rectAnimationTarget_.bottom};
    const interaction_motion::Rect nextTarget{
        target.left, target.top, target.right, target.bottom};
    if (!interaction_motion::ShouldRetarget(
            rectAnimationActive_, activeTarget, nextTarget)) {
        // Mouse move messages arrive much faster than animation frames. Do not
        // restart the clock while the pointer remains inside the same snapped
        // grid step, otherwise the card never catches its target.
        return;
    }
    // Restart the ease-out from the window's current real position, so a
    // target that keeps moving (the pointer crossing several grid lines)
    // blends into one continuous motion instead of jumping per threshold.
    rectAnimationFrom_ = current;
    rectAnimationTarget_ = target;
    rectAnimationStarted_ = GetTickCount64();
    rectAnimationActive_ = true;
    // Publish a one-pixel-scale lead-in immediately. WM_TIMER can arrive one
    // scheduler quantum late; a tiny first sample removes the perceived pause
    // without introducing the old large first-frame jump.
    const interaction_motion::Rect leadIn = interaction_motion::AnimateRect(
        {current.left, current.top, current.right, current.bottom}, nextTarget,
        static_cast<double>(kInteractiveFrameMs), kRectAnimationDurationMs);
    UpdateResizeAnimationSurface(
        {leadIn.left, leadIn.top, leadIn.right, leadIn.bottom});
    SetTimer(window_, 3, kInteractiveFrameMs, nullptr);
}

void ContainerWindow::StepRectAnimation() {
    if (!rectAnimationActive_ || !window_) return;
    const ULONGLONG elapsed = GetTickCount64() - rectAnimationStarted_;
    // Time-based smoothstep: the position is a smooth function of elapsed time,
    // so an uneven WM_TIMER cadence (busy message queue while dragging) can
    // no longer make the motion stutter. The old per-tick percentage
    // approach moved 46% of the remaining gap every 16 ms tick, which read
    // as "jump, pause, jump" whenever a tick arrived late.
    const RECT target = rectAnimationTarget_;
    const interaction_motion::Rect sampled = interaction_motion::AnimateRect(
        {rectAnimationFrom_.left, rectAnimationFrom_.top,
         rectAnimationFrom_.right, rectAnimationFrom_.bottom},
        {target.left, target.top, target.right, target.bottom},
        static_cast<double>(elapsed), kRectAnimationDurationMs);
    RECT rect{sampled.left, sampled.top, sampled.right, sampled.bottom};
    const bool finished = static_cast<double>(elapsed) >= kRectAnimationDurationMs;
    if (finished) rect = target;
    // Animate only the lightweight glass sheet. Moving the real input window
    // and all of its blur/content helper windows on every tick caused regular
    // 30 ms stalls. The real container is committed once at the destination.
    UpdateResizeAnimationSurface(rect);
    if (finished) {
        sizingPreview_ = false;
        suppressRefresh_ = true;
        SetWindowPos(window_, nullptr, target.left, target.top,
                     target.right - target.left, target.bottom - target.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SyncDesktopLayer();
        suppressRefresh_ = false;
        // The moving sheet is intentionally cheap. Publish the full live-blur
        // glass and icons once, underneath it, then remove both after-images.
        UpdateGlass();
        rectAnimationActive_ = false;
        KillTimer(window_, 3);
        DestroyResizeAnimationSurface();
        DestroyResizePreview();
        UpdateTooltips();
        UpdateStateBounds();
        if (!pointerActive_) app_.Save();
    }
}

void ContainerWindow::FinishRectAnimation() {
    if (!rectAnimationActive_ || !window_) return;
    KillTimer(window_, 3);
    const RECT& target = rectAnimationTarget_;
    suppressRefresh_ = true;
    SetWindowPos(window_, nullptr, target.left, target.top,
                 target.right - target.left, target.bottom - target.top,
                  SWP_NOZORDER | SWP_NOACTIVATE);
    SyncDesktopLayer();
    suppressRefresh_ = false;
    sizingPreview_ = false;
    UpdateGlass();
    rectAnimationActive_ = false;
    DestroyResizeAnimationSurface();
    DestroyResizePreview();
    UpdateTooltips();
    UpdateStateBounds();
}

void ContainerWindow::ContinuePointerAction(POINT screen, bool ctrlHeld) {
    if (!pointerActive_) return;
    const int dx = screen.x - pointerStartScreen_.x;
    const int dy = screen.y - pointerStartScreen_.y;
    if (pressedItem_) {
        bool dragStarted = false;
        if (!draggingItem_ && std::abs(dx) + std::abs(dy) > ScaleDip(6, dpi_)) {
            draggingItem_ = true;
            dragStarted = true;
            itemAnimationIndex_ = pressedItem_;
            itemAnimationOffset_ = {};
            CreateDragGhost(*pressedItem_, screen);
        }
        if (draggingItem_) {
            POINT local = screen;
            ScreenToClient(window_, &local);
            ContainerWindow* targetPreview = app_.ContainerAtPoint(screen, this);
            if (targetPreview && targetPreview != this && !targetPreview->State().locked) {
                if (dragTargetPreview_ && dragTargetPreview_ != targetPreview)
                    dragTargetPreview_->ClearExternalDragPreview();
                dragTargetPreview_ = targetPreview;
                targetPreview->SetExternalDragPreview(
                    layout::Span(state_.items[*pressedItem_].iconSize),
                    [&] { POINT point = screen; ScreenToClient(targetPreview->Handle(), &point); return point; }());
                if (dragPreviewPlacement_) {
                    dragPreviewPlacement_.reset();
                    DestroyDragPlaceholder();
                }
            } else if (targetPreview == this) {
                if (dragTargetPreview_) {
                    dragTargetPreview_->ClearExternalDragPreview();
                    dragTargetPreview_ = nullptr;
                }
                layout::Placement preview{};
                const std::optional<layout::Placement> candidate = FindItemPlacement(*pressedItem_, local, preview)
                    ? std::optional<layout::Placement>(preview) : std::nullopt;
                const bool changed = candidate.has_value() != dragPreviewPlacement_.has_value() ||
                    (candidate && (candidate->x != dragPreviewPlacement_->x ||
                                   candidate->y != dragPreviewPlacement_->y ||
                                   candidate->span != dragPreviewPlacement_->span));
                if (changed) {
                    dragPreviewPlacement_ = candidate;
                    UpdateDragPlaceholder();
                }
            } else {
                if (dragTargetPreview_) {
                    dragTargetPreview_->ClearExternalDragPreview();
                    dragTargetPreview_ = nullptr;
                }
                if (dragPreviewPlacement_) {
                    dragPreviewPlacement_.reset();
                    DestroyDragPlaceholder();
                }
            }
            if (dragStarted) UpdateGlass();
            MoveDragGhost(screen);
        }
        return;
    }

    if (resizeEdges_ == 0 && !moving_) {
        const int dragWidth = std::max(1, GetSystemMetricsForDpi(SM_CXDRAG, dpi_));
        const int dragHeight = std::max(1, GetSystemMetricsForDpi(SM_CYDRAG, dpi_));
        if (std::abs(dx) < dragWidth && std::abs(dy) < dragHeight) return;
        moving_ = true;
        // Persist the user's last interaction instead of lifting only for one
        // frame; later shell repairs preserve this group above its siblings.
        app_.SetDesktopFrontContainer(this);
        SyncDesktopLayer();
    }

    RECT rect = pointerStartRect_;
    if (moving_) {
        bypassSnapOnRelease_ = ctrlHeld;
        OffsetRect(&rect, dx, dy);
        // Keep the already lifted window position but re-render the glass
        // every frame: the frosted backdrop (cached per generation and blur
        // radius) is re-sliced under the moving window, so the panel slides
        // over the wallpaper instead of dragging a frozen texture around.
        // Re-rendering on release only made the drag feel stiff.
        if (!ctrlHeld && !state_.snapToGrid) SnapMoveRect(rect);
        RECT current{};
        GetWindowRect(window_, &current);
        if (current.left == rect.left && current.top == rect.top) return;
        movingFastPath_ = true;
        SetWindowPos(window_, nullptr, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        movingFastPath_ = false;
        SyncDesktopLayer(false);
        UpdateGlass();
        UpdateMoveTargetPreview(rect);
        gestureChanged_ = true;
        return;
    }

    if (resizeEdges_ & 1) rect.left += dx;
    if (resizeEdges_ & 2) rect.right += dx;
    if (resizeEdges_ & 4) rect.top += dy;
    if (resizeEdges_ & 8) rect.bottom += dy;

    const int proposedWidth = std::max(OuterWidth(1), static_cast<int>(rect.right - rect.left));
    const int proposedHeight = std::max(OuterHeight(1), static_cast<int>(rect.bottom - rect.top));
    // Resize snaps in 0.5-grid steps: the half-unit pitch lets the container
    // land on sizes like 1.5 x 1.5 (a 3 x 3 arrangement of small icons).
    const int halfUnit = std::max(1, unit_ / 2);
    int halfColumns = layout::SnapCount(proposedWidth - padding_ * 2, halfUnit);
    int halfRows = layout::SnapCount(proposedHeight - titleHeight_ - padding_, halfUnit);

    const auto fitsResizedGrid = [&](int candidateColumns, int candidateRows) {
        const int shiftX = (resizeEdges_ & 1) ? candidateColumns - HalfColumns() : 0;
        const int shiftY = (resizeEdges_ & 4) ? candidateRows - HalfRows() : 0;
        std::vector<OrganizerItem> shifted = state_.items;
        for (auto& item : shifted) {
            if (item.gridX >= 0) {
                item.gridX += shiftX;
                if (item.gridX < 0) return false;
            }
            if (item.gridY >= 0) {
                item.gridY += shiftY;
                if (item.gridY < 0) return false;
            }
        }
        std::vector<layout::Placement> ignored;
        return layout::ArrangeItemsInHalfUnits(
            candidateColumns, candidateRows, shifted, &ignored);
    };

    if (!fitsResizedGrid(halfColumns, halfRows)) {
        if ((resizeEdges_ & 3) && !(resizeEdges_ & 12)) {
            while (!fitsResizedGrid(halfColumns, halfRows)) ++halfColumns;
        } else if ((resizeEdges_ & 12) && !(resizeEdges_ & 3)) {
            while (!fitsResizedGrid(halfColumns, halfRows)) ++halfRows;
        } else {
            halfColumns = HalfColumns();
            halfRows = HalfRows();
        }
    }

    const int width = OuterWidth(halfColumns);
    const int height = OuterHeight(halfRows);
    if (resizeEdges_ & 1) rect.left = pointerStartRect_.right - width;
    else rect.right = pointerStartRect_.left + width;
    if (resizeEdges_ & 4) rect.top = pointerStartRect_.bottom - height;
    else rect.bottom = pointerStartRect_.top + height;

    const RECT work = WorkAreaForRect(rect);
    if (rect.left < work.left || rect.top < work.top || rect.right > work.right || rect.bottom > work.bottom) return;
    const bool changedGrid = halfColumns != HalfColumns() || halfRows != HalfRows();
    sizingPreview_ = changedGrid;
    gestureChanged_ = changedGrid;
    if (!changedGrid) {
        // Crossing back over the original threshold should retract the sheet
        // through the same short motion, not make it disappear in one frame.
        if (resizePreviewWindow_) {
            resizePreviewDestroyWhenFinished_ = true;
            UpdateResizePreview(pointerStartRect_, HalfColumns(), HalfRows());
        }
        return;
    }
    resizePreviewDestroyWhenFinished_ = false;
    const bool previewChanged = !resizePreviewRect_ ||
        !EqualRect(&*resizePreviewRect_, &rect) ||
        resizePreviewHalfColumns_ != halfColumns ||
        resizePreviewHalfRows_ != halfRows;
    if (previewChanged) UpdateResizePreview(rect, halfColumns, halfRows);
}

void ContainerWindow::EndPointerAction(POINT screen, bool ctrlHeld) {
    if (!pointerActive_) return;
    bypassSnapOnRelease_ = ctrlHeld;
    const bool movedContainer = moving_ && !pressedItem_ && gestureChanged_;
    const bool resizedContainer = sizingPreview_ && gestureChanged_ &&
                                  resizePreviewRect_.has_value();
    if (movedContainer || resizedContainer || (pressedItem_ && draggingItem_)) {
        diagnostic_log::Write(
            pressedItem_ && draggingItem_ ? L"item.drag.end" :
                (resizedContainer ? L"group.resize.end" : L"group.move.end"),
            L"group=" + state_.name + L" id=" + state_.id +
                L" x=" + std::to_wstring(screen.x) + L" y=" + std::to_wstring(screen.y) +
                L" ctrl=" + (ctrlHeld ? L"1" : L"0"));
    }
    // Mark the gesture complete before releasing capture. ReleaseCapture sends
    // WM_CAPTURECHANGED synchronously, which must not discard the pending item.
    pointerActive_ = false;
    ReleaseCapture();
    if (pressedItem_ && draggingItem_) {
        KillTimer(window_, 4);
        DestroyDragGhost();
        itemAnimationIndex_.reset();
        itemAnimationOffset_ = {};
        dragPreviewPlacement_.reset();
        DestroyDragPlaceholder();
        if (dragTargetPreview_) {
            dragTargetPreview_->ClearExternalDragPreview();
            dragTargetPreview_ = nullptr;
        }
        ContainerWindow* target = app_.ContainerAtPoint(screen, this);
        if (target) {
            POINT local = screen;
            ScreenToClient(target->Handle(), &local);
            app_.MoveItem(*this, *pressedItem_, *target, local);
        } else if (IsShellDesktopPoint(screen)) {
            app_.MoveItemToDesktop(*this, *pressedItem_, screen);
        }
        UpdateGlass();
    }
    if (resizedContainer) {
        const int gridShiftX = (resizeEdges_ & 1)
            ? resizePreviewHalfColumns_ - HalfColumns() : 0;
        const int gridShiftY = (resizeEdges_ & 4)
            ? resizePreviewHalfRows_ - HalfRows() : 0;
        for (auto& item : state_.items) {
            if (item.gridX >= 0) item.gridX += gridShiftX;
            if (item.gridY >= 0) item.gridY += gridShiftY;
        }
        state_.columns = resizePreviewHalfColumns_ / 2;
        state_.halfColumn = (resizePreviewHalfColumns_ % 2) != 0;
        state_.rows = resizePreviewHalfRows_ / 2;
        state_.halfRow = (resizePreviewHalfRows_ % 2) != 0;
        StartRectAnimation(*resizePreviewRect_);
    }
    moving_ = false;
    movingFastPath_ = false;
    resizeEdges_ = 0;
    pressedItem_.reset();
    draggingItem_ = false;
    dragPreviewPlacement_.reset();
    DestroyDragPlaceholder();
    sizingPreview_ = rectAnimationActive_;
    if (!rectAnimationActive_ && !resizePreviewAnimationActive_) DestroyResizePreview();
    if (movedContainer && state_.snapToGrid && !bypassSnapOnRelease_) {
        RECT snapped{};
        if (moveTargetRect_) snapped = *moveTargetRect_;
        else {
            GetWindowRect(window_, &snapped);
            SnapRectToDesktopGrid(snapped);
        }
        SetWindowPos(window_, nullptr, snapped.left, snapped.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SyncDesktopLayer(false);
    }
    DestroyMoveTargetPreview();
    // The group now sits on the desktop grid: push any covered desktop
    // icons to the nearest free cells so nothing stays hidden underneath.
    if ((movedContainer || resizedContainer) && state_.collapsed &&
        state_.snapToGrid && state_.pushIcons) {
        RECT area{};
        if (rectAnimationActive_) area = rectAnimationTarget_;
        else GetWindowRect(window_, &area);
        app_.PushDesktopIconsOutOf(area, this);
    }
    // Mouse release ends the gesture, while the container remains available
    // above applications for follow-up clicks. The session ends on an
    // outside click, foreground-window switch, or interrupted capture.
    if (rectAnimationActive_) {
        // The ease-out keeps running after release; commit the final geometry
        // immediately so the saved state matches where the panel lands.
        state_.bounds = rectAnimationTarget_;
        state_.monitor = MonitorDevice(MonitorFromRect(&state_.bounds, MONITOR_DEFAULTTONEAREST));
    } else {
        UpdateStateBounds();
    }
    if (movedContainer || resizedContainer) app_.Save();
    if (!rectAnimationActive_) UpdateTooltips();
    if (movedContainer || (resizedContainer && !rectAnimationActive_)) UpdateGlass();
    else if (state_.collapsed) UpdateGlass();
    bypassSnapOnRelease_ = false;
}

void ContainerWindow::StartItemAnimation(size_t index, POINT fromClient) {
    if (index >= state_.items.size()) return;
    dragPreviewPlacement_.reset();
    DestroyDragPlaceholder();
    if (state_.collapsed) {
        itemAnimationIndex_.reset();
        itemAnimationOffset_ = {};
        KillTimer(window_, 4);
        UpdateGlass();
        return;
    }
    const RECT target = ItemRect(index);
    const POINT targetCenter{(target.left + target.right) / 2, (target.top + target.bottom) / 2};
    itemAnimationIndex_ = index;
    itemAnimationStartOffset_ = {fromClient.x - targetCenter.x, fromClient.y - targetCenter.y};
    itemAnimationOffset_ = itemAnimationStartOffset_;
    if (!ClientAnimationsEnabled()) {
        KillTimer(window_, 4);
        itemAnimationIndex_.reset();
        itemAnimationStartOffset_ = {};
        itemAnimationOffset_ = {};
        UpdateGlass();
        return;
    }
    itemAnimationStarted_ = GetTickCount64();
    if (itemAnimationStartOffset_.x == 0 && itemAnimationStartOffset_.y == 0) {
        itemAnimationIndex_.reset();
        itemAnimationOffset_ = {};
        KillTimer(window_, 4);
        UpdateGlass();
        return;
    }
    SetTimer(window_, 4, 16, nullptr);
    UpdateGlass();
}

void ContainerWindow::StepItemAnimation() {
    if (!itemAnimationIndex_ || !window_) return;
    constexpr ULONGLONG kDurationMs = 160;
    const ULONGLONG elapsed = GetTickCount64() - itemAnimationStarted_;
    const double progress = std::min(1.0, static_cast<double>(elapsed) / kDurationMs);
    const double eased = layout::EaseOutCubic(progress);
    const double remaining = 1.0 - eased;
    itemAnimationOffset_.x = static_cast<LONG>(std::lround(itemAnimationStartOffset_.x * remaining));
    itemAnimationOffset_.y = static_cast<LONG>(std::lround(itemAnimationStartOffset_.y * remaining));
    UpdateGlass();
    if (progress >= 1.0) {
        itemAnimationIndex_.reset();
        itemAnimationOffset_ = {};
        KillTimer(window_, 4);
        UpdateGlass();
    }
}

void ContainerWindow::OpenItem(size_t index) {
    if (index >= state_.items.size()) return;
    const auto& path = state_.items[index].path;
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        diagnostic_log::Write(L"item.open.failed",
                              L"group=" + state_.name + L" path=" + path +
                                  L" shellCode=" + std::to_wstring(result));
        MessageBoxW(window_, L"无法打开该项目，文件可能已经移动或删除。", L"桌面收纳", MB_OK | MB_ICONWARNING);
        return;
    }
    diagnostic_log::Write(L"item.open.succeeded",
                          L"group=" + state_.name + L" path=" + path);
    // Only a group that was opened through the optional centered interaction
    // owns a compact home to return to. Ordinary expanded groups keep their
    // existing behavior and remain open after launching an item.
    if (!state_.collapsed && state_.centeredExpansionActive &&
        app_.GetGlobalStyle().centerExpandedGroups) {
        SetCollapsed(true);
    }
}

void ContainerWindow::OpenItemLocation(size_t index) const {
    if (index >= state_.items.size()) return;
    const std::wstring& stored = state_.items[index].path;
    // For shortcuts, reveal the target program's own folder (e.g. the .exe
    // location), not the shortcut file's location. Everything else reveals
    // the item itself; a broken path falls back to the stored reference.
    std::wstring target = stored;
    if (IsShortcutFile(stored)) {
        const std::wstring resolved = ResolveShortcutTarget(stored);
        if (!resolved.empty()) target = resolved;
    }
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) target = stored;
    const std::wstring command = L"/select,\"" + target + L"\"";
    ShellExecuteW(window_, L"open", L"explorer.exe", command.c_str(), nullptr, SW_SHOWNORMAL);
}

void ContainerWindow::ChangeItemIconSize(size_t index, IconSize size) {
    if (index >= state_.items.size() || state_.items[index].iconSize == size) return;
    const RECT previousRect = ItemRect(index);
    const POINT previousCenter{(previousRect.left + previousRect.right) / 2,
                               (previousRect.top + previousRect.bottom) / 2};
    const IconSize previous = state_.items[index].iconSize;
    const int previousX = state_.items[index].gridX;
    const int previousY = state_.items[index].gridY;
    state_.items[index].iconSize = size;
    state_.items[index].gridX = -1;
    state_.items[index].gridY = -1;
    if (!EnsureCapacity(true)) {
        state_.items[index].iconSize = previous;
        state_.items[index].gridX = previousX;
        state_.items[index].gridY = previousY;
        MessageBoxW(window_, L"当前屏幕空间不足，无法放大图标。", L"桌面收纳", MB_OK | MB_ICONINFORMATION);
        return;
    }
    PlaceItemAt(index, previousCenter);
    StartItemAnimation(index, previousCenter);
    UpdateTooltips();
    app_.Save();
    UpdateGlass();
}

void ContainerWindow::ChangeAllItemIconSize(IconSize size) {
    if (state_.items.empty()) return;
    bool changed = false;
    for (const auto& item : state_.items) {
        if (item.iconSize != size) {
            changed = true;
            break;
        }
    }
    if (!changed) return;

    struct PreviousLayout {
        IconSize size;
        int gridX;
        int gridY;
    };
    std::vector<PreviousLayout> previousLayout;
    previousLayout.reserve(state_.items.size());
    for (auto& item : state_.items) {
        previousLayout.push_back({item.iconSize, item.gridX, item.gridY});
        item.iconSize = size;
        // A uniform resize is a fresh packing operation. Old coordinates may
        // have been valid for a different span and can falsely block space.
        item.gridX = -1;
        item.gridY = -1;
    }

    if (!EnsureCapacity(true)) {
        for (size_t index = 0; index < state_.items.size(); ++index) {
            state_.items[index].iconSize = previousLayout[index].size;
            state_.items[index].gridX = previousLayout[index].gridX;
            state_.items[index].gridY = previousLayout[index].gridY;
        }
        MessageBoxW(window_, L"当前屏幕空间不足，无法统一放大图标。",
                    L"桌面收纳", MB_OK | MB_ICONINFORMATION);
        UpdateTooltips();
        UpdateGlass();
        return;
    }

    NormalizeItemPositions();
    UpdateTooltips();
    app_.Save();
    UpdateGlass();
}

void ContainerWindow::UpdateTooltips() {
    HideHoverName();
    hoveredItem_.reset();
    if (paintedHoverItem_) {
        paintedHoverItem_.reset();
        UpdateGlass();
    }
    if (!tooltipWindow_) return;
    for (size_t index = 0; index < tooltipCount_; ++index) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window_;
        tool.uId = static_cast<UINT_PTR>(index + 1);
        SendMessageW(tooltipWindow_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    tooltipCount_ = state_.items.size();
    if (state_.collapsed) {
        tooltipCount_ = 0;
        return;
    }
    for (size_t index = 0; index < tooltipCount_; ++index) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
        tool.hwnd = window_;
        tool.uId = static_cast<UINT_PTR>(index + 1);
        tool.rect = ItemRect(index);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltipWindow_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void ContainerWindow::HideHoverName() {
    KillTimer(window_, 2);
    KillTimer(window_, 7);
    if (hoverWindow_ && IsWindow(hoverWindow_)) DestroyWindow(hoverWindow_);
    hoverWindow_ = nullptr;
}

void ContainerWindow::ShowHoverName() {
    if (!hoveredItem_ || *hoveredItem_ >= state_.items.size() || !tintWindow_) return;
    const size_t itemIndex = *hoveredItem_;
    HideHoverName();
    hoveredItem_ = itemIndex;

    if (!hoverFont_) {
        hoverFont_ = CreateFontW(-ScaleDip(13, dpi_), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI Variable Text");
    }
    const std::wstring& name = state_.items[itemIndex].name;
    HDC screen = GetDC(nullptr);
    SIZE text{};
    HGDIOBJ previous = screen && hoverFont_ ? SelectObject(screen, hoverFont_) : nullptr;
    if (screen) GetTextExtentPoint32W(screen, name.c_str(), static_cast<int>(name.size()), &text);
    if (screen && previous) SelectObject(screen, previous);
    if (screen) ReleaseDC(nullptr, screen);

    const int textWidth = static_cast<int>(text.cx);
    const int width = std::clamp(textWidth + ScaleDip(24, dpi_), ScaleDip(64, dpi_),
                                 ScaleDip(420, dpi_));
    const int height = ScaleDip(32, dpi_);
    POINT cursor{};
    GetCursorPos(&cursor);
    RECT pointRect{cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    const RECT work = WorkAreaForRect(pointRect);
    int x = cursor.x + ScaleDip(14, dpi_);
    int y = cursor.y + ScaleDip(18, dpi_);
    if (x + width > work.right) x = work.right - width;
    if (y + height > work.bottom) y = cursor.y - height - ScaleDip(10, dpi_);
    x = std::max(x, static_cast<int>(work.left));
    y = std::max(y, static_cast<int>(work.top));

    // Owning the popup from the visible tint surface keeps it above this
    // container's glass while preserving the complete group below ordinary
    // application windows.
    hoverWindow_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kHoverClass, name.c_str(), WS_POPUP,
        x, y, width, height, tintWindow_, nullptr, GetModuleHandleW(nullptr), this);
    if (!hoverWindow_) return;
    SetLayeredWindowAttributes(hoverWindow_, 0, 238, LWA_ALPHA);
    const int radius = ScaleDip(9, dpi_);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                     radius * 2, radius * 2);
    if (region && !SetWindowRgn(hoverWindow_, region, FALSE)) DeleteObject(region);
    ShowWindow(hoverWindow_, SW_SHOWNOACTIVATE);
    HWND aboveGroup = GetWindow(tintWindow_, GW_HWNDPREV);
    while (aboveGroup == window_ || aboveGroup == blurWindow_ ||
           aboveGroup == contentWindow_ || aboveGroup == tintWindow_) {
        aboveGroup = GetWindow(aboveGroup, GW_HWNDPREV);
    }
    if (!aboveGroup) aboveGroup = HWND_TOP;
    SetWindowPos(hoverWindow_, aboveGroup, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(hoverWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

void ContainerWindow::PaintHoverName(HWND target) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(target, &paint);
    RECT client{};
    GetClientRect(target, &client);
    const bool dark = UsesDarkGlass(state_);
    HBRUSH background = CreateSolidBrush(dark ? RGB(32, 34, 38) : RGB(250, 252, 253));
    FillRect(dc, &client, background);
    DeleteObject(background);
    HPEN border = CreatePen(PS_SOLID, 1, dark ? RGB(91, 96, 104) : RGB(205, 212, 218));
    HGDIOBJ previousPen = SelectObject(dc, border);
    HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, client.right, client.bottom,
              ScaleDip(18, dpi_), ScaleDip(18, dpi_));
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dark ? RGB(247, 248, 250) : RGB(30, 34, 40));
    HGDIOBJ previousFont = hoverFont_ ? SelectObject(dc, hoverFont_) : nullptr;
    RECT textRect{ScaleDip(10, dpi_), 0, client.right - ScaleDip(10, dpi_), client.bottom};
    wchar_t text[512]{};
    GetWindowTextW(target, text, static_cast<int>(std::size(text)));
    DrawTextW(dc, text, -1, &textRect,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (previousFont) SelectObject(dc, previousFont);
    EndPaint(target, &paint);
}

void ContainerWindow::OpenAppearanceSettings() {
    if (settingsWindow_ && IsWindow(settingsWindow_)) {
        ShowWindow(settingsWindow_, SW_RESTORE);
        SetForegroundWindow(settingsWindow_);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = SettingsProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = CreateSolidBrush(RGB(244, 249, 250));
        cls.lpszClassName = kSettingsClass;
        registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return;
    RECT owner{};
    GetWindowRect(window_, &owner);
    settingsWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, kSettingsClass, L"外观设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, owner.right + ScaleDip(14, dpi_), owner.top,
        ScaleDip(390, dpi_), ScaleDip(455, dpi_), window_, nullptr, GetModuleHandleW(nullptr), this);
    if (settingsWindow_) {
        constexpr DWORD kBackdropType = 38;
        constexpr int kMica = 2;
        constexpr DWORD kCornerPreference = 33;
        constexpr int kRound = 2;
        DwmSetWindowAttribute(settingsWindow_, kBackdropType, &kMica, sizeof(kMica));
        DwmSetWindowAttribute(settingsWindow_, kCornerPreference, &kRound, sizeof(kRound));
        ShowWindow(settingsWindow_, SW_SHOW);
        SetForegroundWindow(settingsWindow_);
    }
}

void ContainerWindow::CreateSettingsControls(HWND window) {
    if (settingsFont_) DeleteObject(settingsFont_);
    settingsFont_ = CreateFontW(-ScaleDip(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    const auto createLabel = [&](const wchar_t* text, int y, int width = 220) {
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, ScaleDip(24, dpi_), ScaleDip(y, dpi_),
                             ScaleDip(width, dpi_), ScaleDip(22, dpi_), window, nullptr, nullptr, nullptr);
    };
    createLabel(L"清透外观", 20);
    createLabel(L"通透度", 60);
    opacityValue_ = createLabel(L"", 60, 330);
    SetWindowPos(opacityValue_, nullptr, ScaleDip(305, dpi_), ScaleDip(60, dpi_), ScaleDip(54, dpi_), ScaleDip(22, dpi_), SWP_NOZORDER);
    opacitySlider_ = CreateWindowW(kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   ScaleDip(20, dpi_), ScaleDip(84, dpi_), ScaleDip(340, dpi_), ScaleDip(28, dpi_),
                                   window, reinterpret_cast<HMENU>(SettingOpacity), nullptr, nullptr);
    SendMessageW(opacitySlider_, kSliderSetRange, 0, MAKELONG(4, 85));
    SendMessageW(opacitySlider_, kSliderSetPosition, 100 - state_.opacity, 0);

    createLabel(L"背景模糊（实时磨砂）", 122);
    blurValue_ = createLabel(L"—", 122, 330);
    SetWindowPos(blurValue_, nullptr, ScaleDip(305, dpi_), ScaleDip(122, dpi_), ScaleDip(54, dpi_), ScaleDip(22, dpi_), SWP_NOZORDER);
    blurSlider_ = CreateWindowW(kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                ScaleDip(20, dpi_), ScaleDip(146, dpi_), ScaleDip(340, dpi_), ScaleDip(28, dpi_),
                                window, reinterpret_cast<HMENU>(SettingBlur), nullptr, nullptr);
    SendMessageW(blurSlider_, kSliderSetRange, 0, MAKELONG(0, 100));
    SendMessageW(blurSlider_, kSliderSetPosition, state_.blur, 0);
    EnableWindow(blurSlider_, TRUE);

    createLabel(L"圆角", 184);
    cornerValue_ = createLabel(L"", 184, 330);
    SetWindowPos(cornerValue_, nullptr, ScaleDip(305, dpi_), ScaleDip(184, dpi_), ScaleDip(54, dpi_), ScaleDip(22, dpi_), SWP_NOZORDER);
    cornerSlider_ = CreateWindowW(kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  ScaleDip(20, dpi_), ScaleDip(208, dpi_), ScaleDip(340, dpi_), ScaleDip(28, dpi_),
                                  window, reinterpret_cast<HMENU>(SettingCorner), nullptr, nullptr);
    SendMessageW(cornerSlider_, kSliderSetRange, 0, MAKELONG(0, 48));
    SendMessageW(cornerSlider_, kSliderSetPosition, state_.cornerRadius, 0);

    createLabel(L"玻璃色调", 246);
    HWND tintAuto = CreateWindowW(L"BUTTON", L"自动", WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                  ScaleDip(24, dpi_), ScaleDip(270, dpi_), ScaleDip(76, dpi_), ScaleDip(26, dpi_),
                                  window, reinterpret_cast<HMENU>(SettingTintAuto), nullptr, nullptr);
    HWND tintDark = CreateWindowW(L"BUTTON", L"夜色", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                  ScaleDip(112, dpi_), ScaleDip(270, dpi_), ScaleDip(76, dpi_), ScaleDip(26, dpi_),
                                  window, reinterpret_cast<HMENU>(SettingTintDark), nullptr, nullptr);
    HWND tintLight = CreateWindowW(L"BUTTON", L"冰蓝", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   ScaleDip(200, dpi_), ScaleDip(270, dpi_), ScaleDip(76, dpi_), ScaleDip(26, dpi_),
                                   window, reinterpret_cast<HMENU>(SettingTintLight), nullptr, nullptr);
    HWND tintCustom = CreateWindowW(L"BUTTON", L"自定义", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                    ScaleDip(286, dpi_), ScaleDip(270, dpi_), ScaleDip(76, dpi_), ScaleDip(26, dpi_),
                                    window, reinterpret_cast<HMENU>(SettingTintCustom), nullptr, nullptr);
    SendMessageW(tintAuto, BM_SETCHECK, state_.tintMode == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tintLight, BM_SETCHECK, state_.tintMode == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tintDark, BM_SETCHECK, state_.tintMode == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tintCustom, BM_SETCHECK, state_.tintMode == 3 ? BST_CHECKED : BST_UNCHECKED, 0);

    tintColorButton_ = CreateWindowW(L"BUTTON", L"选择背景颜色", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     ScaleDip(24, dpi_), ScaleDip(306, dpi_), ScaleDip(140, dpi_), ScaleDip(30, dpi_),
                                     window, reinterpret_cast<HMENU>(SettingTintColor), nullptr, nullptr);
    textColorButton_ = CreateWindowW(L"BUTTON", L"选择文字颜色", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     ScaleDip(178, dpi_), ScaleDip(306, dpi_), ScaleDip(184, dpi_), ScaleDip(30, dpi_),
                                     window, reinterpret_cast<HMENU>(SettingTextColor), nullptr, nullptr);

    HWND title = CreateWindowW(L"BUTTON", L"显示标题", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               ScaleDip(24, dpi_), ScaleDip(352, dpi_), ScaleDip(100, dpi_), ScaleDip(26, dpi_),
                               window, reinterpret_cast<HMENU>(SettingTitle), nullptr, nullptr);
    HWND border = CreateWindowW(L"BUTTON", L"显示细边框", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                ScaleDip(138, dpi_), ScaleDip(352, dpi_), ScaleDip(112, dpi_), ScaleDip(26, dpi_),
                                window, reinterpret_cast<HMENU>(SettingBorder), nullptr, nullptr);
    HWND lock = CreateWindowW(L"BUTTON", L"锁定容器", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              ScaleDip(264, dpi_), ScaleDip(352, dpi_), ScaleDip(100, dpi_), ScaleDip(26, dpi_),
                              window, reinterpret_cast<HMENU>(SettingLock), nullptr, nullptr);
    SendMessageW(title, BM_SETCHECK, state_.showTitle ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(border, BM_SETCHECK, state_.showBorder ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(lock, BM_SETCHECK, state_.locked ? BST_CHECKED : BST_UNCHECKED, 0);

    EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        SetWindowTheme(child, L"Explorer", nullptr);
        return TRUE;
    }, reinterpret_cast<LPARAM>(settingsFont_));
    UpdateSettingsLabels();
}

void ContainerWindow::UpdateSettingsLabels() {
    if (!settingsWindow_) return;
    const std::wstring opacity = std::to_wstring(SendMessageW(opacitySlider_, kSliderGetPosition, 0, 0)) + L"%";
    const std::wstring corner = std::to_wstring(SendMessageW(cornerSlider_, kSliderGetPosition, 0, 0)) + L" px";
    SetWindowTextW(opacityValue_, opacity.c_str());
    SetWindowTextW(blurValue_, (std::to_wstring(SendMessageW(blurSlider_, kSliderGetPosition, 0, 0)) + L"%").c_str());
    SetWindowTextW(cornerValue_, corner.c_str());
}

void ContainerWindow::ChooseTintColor() {
    static COLORREF customColors[16]{
        RGB(246, 249, 255), RGB(238, 247, 255), RGB(246, 240, 255), RGB(255, 242, 248),
        RGB(238, 252, 247), RGB(255, 249, 232), RGB(232, 240, 255), RGB(244, 244, 244)};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = settingsWindow_;
    chooser.rgbResult = TintColor(state_);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&chooser)) return;
    state_.tintColor = (GetRValue(chooser.rgbResult) << 16) |
                       (GetGValue(chooser.rgbResult) << 8) | GetBValue(chooser.rgbResult);
    state_.tintMode = 3;
    state_.followGlobalStyle = false;
    SendDlgItemMessageW(settingsWindow_, SettingTintAuto, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(settingsWindow_, SettingTintDark, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(settingsWindow_, SettingTintLight, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(settingsWindow_, SettingTintCustom, BM_SETCHECK, BST_CHECKED, 0);
    InvalidateRect(tintColorButton_, nullptr, TRUE);
    UpdateGlass();
    app_.Save();
    app_.RefreshConsoleRows();
}

void ContainerWindow::ChooseTextColor() {
    static COLORREF customColors[16]{
        RGB(245, 248, 250), RGB(255, 255, 255), RGB(35, 49, 54), RGB(0, 0, 0),
        RGB(73, 112, 255), RGB(96, 210, 190), RGB(255, 188, 92), RGB(244, 116, 143)};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = settingsWindow_;
    chooser.rgbResult = RGB((state_.textColor >> 16) & 0xff,
                            (state_.textColor >> 8) & 0xff,
                            state_.textColor & 0xff);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&chooser)) return;
    state_.textColor = (GetRValue(chooser.rgbResult) << 16) |
                       (GetGValue(chooser.rgbResult) << 8) | GetBValue(chooser.rgbResult);
    state_.followGlobalStyle = false;
    InvalidateRect(textColorButton_, nullptr, TRUE);
    UpdateGlass();
    app_.Save();
    app_.RefreshConsoleRows();
}

void ContainerWindow::ApplySettingsFromControls(bool persist) {
    if (!settingsWindow_) return;
    // Editing style fields from the per-container panel takes manual control:
    // the group stops following the global default style.
    state_.followGlobalStyle = false;
    const int oldOpacity = state_.opacity;
    const int oldBlur = state_.blur;
    const int oldCorner = state_.cornerRadius;
    const int oldTintMode = state_.tintMode;
    const bool oldBorder = state_.showBorder;
    const bool oldLocked = state_.locked;
    const bool oldTitle = state_.showTitle;
    state_.opacity = 100 - static_cast<int>(SendMessageW(opacitySlider_, kSliderGetPosition, 0, 0));
    state_.blur = static_cast<int>(SendMessageW(blurSlider_, kSliderGetPosition, 0, 0));
    state_.cornerRadius = static_cast<int>(SendMessageW(cornerSlider_, kSliderGetPosition, 0, 0));
    if (SendDlgItemMessageW(settingsWindow_, SettingTintAuto, BM_GETCHECK, 0, 0) == BST_CHECKED) state_.tintMode = 0;
    else if (SendDlgItemMessageW(settingsWindow_, SettingTintLight, BM_GETCHECK, 0, 0) == BST_CHECKED) state_.tintMode = 1;
    else if (SendDlgItemMessageW(settingsWindow_, SettingTintDark, BM_GETCHECK, 0, 0) == BST_CHECKED) state_.tintMode = 2;
    else state_.tintMode = 3;
    state_.showTitle = SendDlgItemMessageW(settingsWindow_, SettingTitle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state_.showBorder = SendDlgItemMessageW(settingsWindow_, SettingBorder, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state_.locked = SendDlgItemMessageW(settingsWindow_, SettingLock, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (oldTitle != state_.showTitle) {
        UpdateMetrics();
        SetWindowPos(window_, nullptr, 0, 0, OuterWidth(HalfColumns()), OuterHeight(HalfRows()),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (oldOpacity != state_.opacity || oldBlur != state_.blur || oldTintMode != state_.tintMode ||
        oldBorder != state_.showBorder)
        UpdateGlass();
    if (oldCorner != state_.cornerRadius) UpdateShape();
    UpdateSettingsLabels();
    if (oldTitle != state_.showTitle || oldBorder != state_.showBorder || oldLocked != state_.locked ||
        oldCorner != state_.cornerRadius || oldTintMode != state_.tintMode)
        InvalidateRect(window_, nullptr, TRUE);
    if (persist) {
        diagnostic_log::Write(L"group.style.changed",
                              L"group=" + state_.name + L" opacity=" +
                                  std::to_wstring(state_.opacity) + L" blur=" +
                                  std::to_wstring(state_.blur) + L" corner=" +
                                  std::to_wstring(state_.cornerRadius) + L" tintMode=" +
                                  std::to_wstring(state_.tintMode));
        app_.Save();
    }
    app_.RefreshConsoleRows();
}

void ContainerWindow::ReconcileShellItem(size_t itemIndex, const FileIdentity& identity) {
    if (itemIndex >= state_.items.size()) return;
    const std::filesystem::path previous(state_.items[itemIndex].path);
    if (QueryPathExistence(previous) != PathExistence::Missing) return;

    const auto renamed = FindPathByIdentity(previous.parent_path(), identity);
    if (!renamed.empty()) {
        state_.items[itemIndex].path = renamed.wstring();
        state_.items[itemIndex].name = DisplayNameForPath(renamed.wstring());
    } else {
        state_.items.erase(state_.items.begin() + static_cast<ptrdiff_t>(itemIndex));
    }
    RefreshAfterExternalChange();
    app_.Save();
}

void ContainerWindow::ScheduleShellItemReconcile(const std::filesystem::path& path,
                                                  const FileIdentity& identity) {
    pendingShellReconciles_.push_back({path, identity, 0});
    SetTimer(window_, 6, 250, nullptr);
}

void ContainerWindow::ProcessShellItemReconciles() {
    for (auto it = pendingShellReconciles_.begin(); it != pendingShellReconciles_.end();) {
        if (QueryPathExistence(it->path) != PathExistence::Missing) {
            if (++it->attempts >= 40) it = pendingShellReconciles_.erase(it);
            else ++it;
            continue;
        }
        const auto item = std::find_if(state_.items.begin(), state_.items.end(), [&](const OrganizerItem& candidate) {
            return SamePath(candidate.path, it->path);
        });
        if (item != state_.items.end()) {
            ReconcileShellItem(static_cast<size_t>(item - state_.items.begin()), it->identity);
        }
        it = pendingShellReconciles_.erase(it);
    }
    if (pendingShellReconciles_.empty()) KillTimer(window_, 6);
}

bool ContainerWindow::ShowShellItemContextMenu(POINT screen, size_t itemIndex) {
    if (itemIndex >= state_.items.size()) return false;
    const std::filesystem::path itemPath(state_.items[itemIndex].path);

    PIDLIST_ABSOLUTE absolute = nullptr;
    IShellFolder* parent = nullptr;
    IContextMenu* contextMenu = nullptr;
    std::vector<std::pair<std::string, UINT>> shellCommands;

    SFGAOF attributes = 0;
    PCUITEMID_CHILD child = nullptr;
    constexpr UINT kShellFirst = 0x1000;
    constexpr UINT kShellLast = 0x6fff;
    if (SUCCEEDED(SHParseDisplayName(itemPath.c_str(), nullptr, &absolute, 0, &attributes)) && absolute &&
        SUCCEEDED(SHBindToParent(absolute, IID_PPV_ARGS(&parent), &child)) && parent && child &&
        SUCCEEDED(parent->GetUIObjectOf(window_, 1, &child, IID_IContextMenu, nullptr,
                                        reinterpret_cast<void**>(&contextMenu))) && contextMenu) {
        HMENU queriedMenu = CreatePopupMenu();
        if (queriedMenu) {
            const HRESULT query = contextMenu->QueryContextMenu(
                queriedMenu, 0, kShellFirst, kShellLast, CMF_NORMAL | CMF_EXPLORE);
            if (SUCCEEDED(query)) {
                const UINT commandCount = static_cast<UINT>(HRESULT_CODE(query));
                for (UINT offset = 0; offset < commandCount; ++offset) {
                    char verb[128]{};
                    if (SUCCEEDED(contextMenu->GetCommandString(
                            offset, GCS_VERBA, nullptr, verb, static_cast<UINT>(std::size(verb)))) && verb[0]) {
                        shellCommands.emplace_back(verb, offset);
                    }
                }
            }
            DestroyMenu(queriedMenu);
        }
    }

    const auto shellOffset = [&](const char* wanted) -> std::optional<UINT> {
        const auto found = std::find_if(shellCommands.begin(), shellCommands.end(), [&](const auto& entry) {
            return _stricmp(entry.first.c_str(), wanted) == 0;
        });
        if (found == shellCommands.end()) return std::nullopt;
        return found->second;
    };
    const auto cut = shellOffset("cut");
    const auto copy = shellOffset("copy");
    const auto paste = shellOffset("paste");
    const auto remove = shellOffset("delete");
    const auto properties = shellOffset("properties");
    const auto enabledFlag = [](const std::optional<UINT>& command) -> UINT {
        return command ? MF_STRING : MF_STRING | MF_GRAYED | MF_DISABLED;
    };

    HMENU menu = CreatePopupMenu();
    HMENU size = CreatePopupMenu();
    if (!menu || !size) {
        if (size) DestroyMenu(size);
        if (menu) DestroyMenu(menu);
        if (contextMenu) contextMenu->Release();
        if (parent) parent->Release();
        if (absolute) CoTaskMemFree(absolute);
        return false;
    }

    const IconSize currentSize = state_.items[itemIndex].iconSize;
    AppendMenuW(size, MF_STRING | (currentSize == IconSize::Small ? MF_CHECKED : 0),
                MenuSizeSmall, L"小（正常的一半）");
    AppendMenuW(size, MF_STRING | (currentSize == IconSize::Normal ? MF_CHECKED : 0),
                MenuSizeNormal, L"正常");
    AppendMenuW(size, MF_STRING | (currentSize == IconSize::Large ? MF_CHECKED : 0),
                MenuSizeLarge, L"大（正常的两倍）");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(size), L"图标大小");
    AppendMenuW(menu, MF_STRING, MenuOpenLocation, L"打开文件所在位置");
    AppendMenuW(menu, MF_STRING, MenuRemoveItem, L"移出容器");
    AppendMenuW(menu, MF_STRING, MenuNew, L"新建组");
    AppendMenuW(menu, MF_STRING, MenuCopyPath, L"复制文件地址");
    AppendMenuW(menu, MF_STRING, MenuRename, L"修改显示名称");
    AppendMenuW(menu, MF_STRING, MenuRenameFilesystem, L"重命名磁盘文件…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, enabledFlag(cut), MenuShellCut, L"剪切");
    AppendMenuW(menu, enabledFlag(copy), MenuShellCopy, L"复制");
    AppendMenuW(menu, enabledFlag(paste), MenuShellPaste, L"粘贴");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, enabledFlag(remove), MenuShellDelete, L"删除");
    AppendMenuW(menu, enabledFlag(properties), MenuShellProperties, L"属性");

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command != 0) {
        diagnostic_log::Write(L"item.menu.command",
                              L"group=" + state_.name + L" command=" +
                                  std::to_wstring(command) + L" path=" + itemPath.wstring());
    }

    if (command == MenuRemoveItem) {
        app_.MoveItemToDesktop(*this, itemIndex);
    } else if (command == MenuOpenLocation) {
        OpenItemLocation(itemIndex);
    } else if (command == MenuCopyPath) CopyPathToClipboard(window_, itemPath);
    else if (command == MenuRename) {
        std::wstring displayName = state_.items[itemIndex].name;
        if (PromptForName(window_, displayName, &state_) && !displayName.empty()) {
            state_.items[itemIndex].name = std::move(displayName);
            RefreshAfterExternalChange();
            app_.Save();
        }
    }
    else if (command == MenuRenameFilesystem) {
        const OrganizerItem previous = state_.items[itemIndex];
        if (RenameFilesystemItem(window_, state_.items[itemIndex], &state_)) {
            RefreshAfterExternalChange();
            if (!app_.Save()) {
                const std::filesystem::path renamed(state_.items[itemIndex].path);
                if (MoveFileExW(renamed.c_str(), previous.path.c_str(), 0)) {
                    state_.items[itemIndex] = previous;
                    RefreshAfterExternalChange();
                    app_.Save();
                } else {
                    MessageBoxW(window_,
                                L"配置保存失败，且无法回滚磁盘重命名。\n程序将保留新路径并继续重试保存。",
                                L"重命名", MB_OK | MB_ICONWARNING);
                }
            }
        }
    }
    else if (command == MenuSizeSmall) ChangeItemIconSize(itemIndex, IconSize::Small);
    else if (command == MenuSizeNormal) ChangeItemIconSize(itemIndex, IconSize::Normal);
    else if (command == MenuSizeLarge) ChangeItemIconSize(itemIndex, IconSize::Large);
    else if (command == MenuNew) app_.CreateContainer(screen);
    else {
        std::optional<UINT> selectedShellCommand;
        if (command == MenuShellCut) selectedShellCommand = cut;
        else if (command == MenuShellCopy) selectedShellCommand = copy;
        else if (command == MenuShellPaste) selectedShellCommand = paste;
        else if (command == MenuShellDelete) selectedShellCommand = remove;
        else if (command == MenuShellProperties) selectedShellCommand = properties;

        if (contextMenu && selectedShellCommand) {
            const bool reconcile = command == MenuShellCut || command == MenuShellDelete;
            const auto identity = reconcile ? IdentityForPath(itemPath) : std::nullopt;
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_ASYNCOK;
            invoke.hwnd = window_;
            invoke.lpVerb = MAKEINTRESOURCEA(*selectedShellCommand);
            invoke.lpVerbW = MAKEINTRESOURCEW(*selectedShellCommand);
            invoke.nShow = SW_SHOWNORMAL;
            if (SUCCEEDED(contextMenu->InvokeCommand(
                    reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke))) && identity) {
                ReconcileShellItem(itemIndex, *identity);
                if (QueryPathExistence(itemPath) == PathExistence::Present)
                    ScheduleShellItemReconcile(itemPath, *identity);
            }
        }
    }

    if (contextMenu) contextMenu->Release();
    if (parent) parent->Release();
    if (absolute) CoTaskMemFree(absolute);
    SyncDesktopLayer();
    return true;
}

LRESULT CALLBACK ContainerWindow::SettingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ContainerWindow* self = reinterpret_cast<ContainerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ContainerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE: self->settingsWindow_ = window; self->CreateSettingsControls(window); return 0;
    case WM_HSCROLL:
        self->UpdateSettingsLabels();
        {
        const bool timerAlreadyRunning = self->settingsUpdatePending_;
        self->settingsUpdatePending_ = true;
        self->settingsPersistPending_ = self->settingsPersistPending_ || LOWORD(wParam) == SB_ENDSCROLL;
        if (!timerAlreadyRunning) SetTimer(window, 1, kStylePreviewFrameMs, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wParam == 1 && self->settingsUpdatePending_) {
            KillTimer(window, 1);
            const bool persist = self->settingsPersistPending_;
            self->settingsUpdatePending_ = false;
            self->settingsPersistPending_ = false;
            self->ApplySettingsFromControls(persist);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            if (LOWORD(wParam) == SettingTintColor) self->ChooseTintColor();
            else if (LOWORD(wParam) == SettingTextColor) self->ChooseTextColor();
            else self->ApplySettingsFromControls(true);
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw && (draw->CtlID == SettingTintColor || draw->CtlID == SettingTextColor)) {
            RECT rect = draw->rcItem;
            HBRUSH background = CreateSolidBrush(RGB(244, 249, 250));
            FillRect(draw->hDC, &rect, background);
            DeleteObject(background);
            RECT swatch{rect.left + ScaleDip(8, self->dpi_), rect.top + ScaleDip(6, self->dpi_),
                        rect.left + ScaleDip(30, self->dpi_), rect.bottom - ScaleDip(6, self->dpi_)};
            const COLORREF swatchColor = draw->CtlID == SettingTintColor
                ? TintColor(self->state_)
                : RGB((self->state_.textColor >> 16) & 0xff,
                      (self->state_.textColor >> 8) & 0xff,
                      self->state_.textColor & 0xff);
            HBRUSH color = CreateSolidBrush(swatchColor);
            FillRect(draw->hDC, &swatch, color);
            DeleteObject(color);
            HBRUSH swatchBorder = CreateSolidBrush(RGB(183, 210, 215));
            FrameRect(draw->hDC, &swatch, swatchBorder);
            DeleteObject(swatchBorder);
            RECT text{swatch.right + ScaleDip(8, self->dpi_), rect.top, rect.right - ScaleDip(5, self->dpi_), rect.bottom};
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, RGB(38, 59, 65));
            const wchar_t* label = draw->CtlID == SettingTintColor
                ? L"背景颜色" : L"文字颜色";
            DrawTextW(draw->hDC, label, -1, &text,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
            HBRUSH outline = CreateSolidBrush(RGB(214, 231, 234));
            FrameRect(draw->hDC, &rect, outline);
            DeleteObject(outline);
            if (draw->itemState & ODS_FOCUS) {
                InflateRect(&rect, -ScaleDip(3, self->dpi_), -ScaleDip(3, self->dpi_));
                DrawFocusRect(draw->hDC, &rect);
            }
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(244, 249, 250));
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(38, 59, 65));
        SetDCBrushColor(reinterpret_cast<HDC>(wParam), RGB(244, 249, 250));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_CLOSE:
        KillTimer(window, 1);
        if (self->settingsUpdatePending_) self->ApplySettingsFromControls(true);
        self->settingsUpdatePending_ = false;
        self->settingsPersistPending_ = false;
        DestroyWindow(window); return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(window, 1);
        self->settingsUpdatePending_ = false;
        self->settingsPersistPending_ = false;
        self->settingsWindow_ = nullptr;
        if (self->settingsFont_) {
            DeleteObject(self->settingsFont_);
            self->settingsFont_ = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ContainerWindow::ShowContextMenu(POINT screen, std::optional<size_t> itemIndex) {
    if (itemIndex && ShowShellItemContextMenu(screen, *itemIndex)) return;
    HMENU menu = CreatePopupMenu();
    if (itemIndex) {
        AppendMenuW(menu, MF_STRING, MenuOpenItem, L"打开");
        AppendMenuW(menu, MF_STRING, MenuOpenLocation, L"打开文件所在位置");
        HMENU size = CreatePopupMenu();
        const IconSize currentSize = state_.items[*itemIndex].iconSize;
        AppendMenuW(size, MF_STRING | (currentSize == IconSize::Small ? MF_CHECKED : 0), MenuSizeSmall, L"小（正常的一半）");
        AppendMenuW(size, MF_STRING | (currentSize == IconSize::Normal ? MF_CHECKED : 0), MenuSizeNormal, L"正常");
        AppendMenuW(size, MF_STRING | (currentSize == IconSize::Large ? MF_CHECKED : 0), MenuSizeLarge, L"大（正常的两倍）");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(size), L"图标大小");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuRemoveItem, L"移出容器");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuNew, L"新建组");
    } else {
        AppendMenuW(menu, MF_STRING, MenuCollapse, state_.collapsed ? L"展开分组" : L"收起为分组图标");
        HMENU unifiedSize = CreatePopupMenu();
        std::optional<IconSize> commonSize;
        if (!state_.items.empty()) {
            commonSize = state_.items.front().iconSize;
            for (const auto& item : state_.items) {
                if (item.iconSize != *commonSize) {
                    commonSize.reset();
                    break;
                }
            }
        }
        AppendMenuW(unifiedSize, MF_STRING | (commonSize == IconSize::Small ? MF_CHECKED : 0),
                    MenuUnifiedSizeSmall, L"小（正常的一半）");
        AppendMenuW(unifiedSize, MF_STRING | (commonSize == IconSize::Normal ? MF_CHECKED : 0),
                    MenuUnifiedSizeNormal, L"正常");
        AppendMenuW(unifiedSize, MF_STRING | (commonSize == IconSize::Large ? MF_CHECKED : 0),
                    MenuUnifiedSizeLarge, L"大（正常的两倍）");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(unifiedSize), L"统一图标大小");
        AppendMenuW(menu, MF_STRING, MenuAppearance, L"外观设置…");
        AppendMenuW(menu, MF_STRING, MenuConsole, L"总控制台…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuRename, L"重命名");
        AppendMenuW(menu, MF_STRING | (state_.showTitle ? MF_CHECKED : 0), MenuShowTitle, L"显示标题");
        AppendMenuW(menu, MF_STRING | (state_.showBorder ? MF_CHECKED : 0), MenuShowBorder, L"显示边框");
        AppendMenuW(menu, MF_STRING | (state_.locked ? MF_CHECKED : 0), MenuLock, state_.locked ? L"解除锁定" : L"锁定位置和大小");
        AppendMenuW(menu, MF_STRING | (state_.snapToGrid ? MF_CHECKED : 0), MenuSnapGrid,
                    L"嵌入桌面网格（收起后占一格）");
        AppendMenuW(menu, MF_STRING | (state_.pushIcons ? MF_CHECKED : 0), MenuPushIcons,
                    L"收起时挤开桌面图标");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, MenuNew, L"新建组");
        AppendMenuW(menu, MF_STRING, MenuDissolve,
                    state_.items.empty() ? L"删除空分组…" : L"一键解散分组…");
    }

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(menu);
    SyncDesktopLayer();
    if (command != 0) {
        diagnostic_log::Write(L"group.menu.command",
                              L"group=" + state_.name + L" id=" + state_.id +
                                  L" command=" + std::to_wstring(command));
    }
    switch (command) {
    case MenuOpenItem: if (itemIndex) OpenItem(*itemIndex); break;
    case MenuOpenLocation: if (itemIndex) OpenItemLocation(*itemIndex); break;
    case MenuRemoveItem:
        if (itemIndex && *itemIndex < state_.items.size()) {
            app_.MoveItemToDesktop(*this, *itemIndex);
        }
        break;
    case MenuSizeSmall: if (itemIndex) ChangeItemIconSize(*itemIndex, IconSize::Small); break;
    case MenuSizeNormal: if (itemIndex) ChangeItemIconSize(*itemIndex, IconSize::Normal); break;
    case MenuSizeLarge: if (itemIndex) ChangeItemIconSize(*itemIndex, IconSize::Large); break;
    case MenuUnifiedSizeSmall: ChangeAllItemIconSize(IconSize::Small); break;
    case MenuUnifiedSizeNormal: ChangeAllItemIconSize(IconSize::Normal); break;
    case MenuUnifiedSizeLarge: ChangeAllItemIconSize(IconSize::Large); break;
    case MenuAppearance: OpenAppearanceSettings(); break;
    case MenuConsole: app_.OpenConsole(); break;
    case MenuOpacity45: state_.opacity = 45; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuOpacity65: state_.opacity = 65; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuOpacity80: state_.opacity = 80; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuOpacity95: state_.opacity = 95; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuBlurOff: state_.blur = 0; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuBlurLight: state_.blur = 40; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuBlurStrong: state_.blur = 80; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuCorner0: state_.cornerRadius = 0; state_.followGlobalStyle = false; UpdateShape(); app_.Save(); break;
    case MenuCorner12: state_.cornerRadius = 12; state_.followGlobalStyle = false; UpdateShape(); app_.Save(); break;
    case MenuCorner24: state_.cornerRadius = 24; state_.followGlobalStyle = false; UpdateShape(); app_.Save(); break;
    case MenuRename:
        if (PromptForName(window_, state_.name, &state_)) { SetWindowTextW(window_, state_.name.c_str()); UpdateGlass(); app_.Save(); app_.RefreshConsoleRows(); }
        break;
    case MenuShowTitle: {
        const int oldHeight = OuterHeight(HalfRows());
        state_.showTitle = !state_.showTitle;
        state_.followGlobalStyle = false;
        UpdateMetrics();
        const int newHeight = OuterHeight(HalfRows());
        SetWindowPos(window_, nullptr, 0, 0, OuterWidth(HalfColumns()), newHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        (void)oldHeight;
        UpdateShape(); app_.Save(); InvalidateRect(window_, nullptr, TRUE);
        break;
    }
    case MenuShowBorder: state_.showBorder = !state_.showBorder; state_.followGlobalStyle = false; UpdateGlass(); app_.Save(); break;
    case MenuLock: state_.locked = !state_.locked; app_.Save(); InvalidateRect(window_, nullptr, TRUE); break;
    case MenuSnapGrid: {
        state_.snapToGrid = !state_.snapToGrid;
        UpdateMetrics();
        RECT snapped{};
        GetWindowRect(window_, &snapped);
        snapped.right = snapped.left + OuterWidth(HalfColumns());
        snapped.bottom = snapped.top + OuterHeight(HalfRows());
        if (state_.snapToGrid) SnapRectToDesktopGrid(snapped);
        SetWindowPos(window_, nullptr, snapped.left, snapped.top,
                     snapped.right - snapped.left, snapped.bottom - snapped.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (state_.collapsed && state_.snapToGrid && state_.pushIcons)
            app_.PushDesktopIconsOutOf(snapped, this);
        UpdateShape();
        UpdateGlass();
        InvalidateRect(window_, nullptr, TRUE);
        UpdateStateBounds();
        app_.Save();
        break;
    }
    case MenuPushIcons: {
        state_.pushIcons = !state_.pushIcons;
        if (state_.collapsed && state_.pushIcons && state_.snapToGrid) {
            RECT area{};
            GetWindowRect(window_, &area);
            app_.PushDesktopIconsOutOf(area, this);
        }
        app_.Save();
        break;
    }
    case MenuCollapse: SetCollapsed(!state_.collapsed); break;
    case MenuNew: app_.CreateContainer(screen); break;
    case MenuDissolve: {
        const bool empty = state_.items.empty();
        const wchar_t* prompt = empty
            ? L"删除这个空分组？"
            : L"一键解散这个分组？\n\n从桌面收纳进来的项目会全部恢复到桌面，"
              L"并由 Windows 按当前桌面排列规则放置。\n真实文件不会被删除。";
        const wchar_t* title = empty ? L"删除空分组" : L"一键解散分组";
        if (MessageBoxW(window_, prompt, title,
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            app_.ScheduleDissolve(state_.id);
        break;
    }
    default: break;
    }
}

void ContainerWindow::AddDroppedFiles(HDROP drop) {
    // Absorb-and-collect: desktop items (shortcuts, files and folders) are
    // copied into the group storage and then removed from the desktop, so
    // the desktop icon disappears for good while the group keeps the item.
    // Dropping out restores it to the desktop. Items dropped from anywhere
    // outside the desktop become plain references and are never moved.
    const ContainerState originalState = state_;
    const size_t originalCount = state_.items.size();
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pendingAbsorptions;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    diagnostic_log::Write(L"items.drop.begin",
                          L"group=" + state_.name + L" id=" + state_.id +
                              L" count=" + std::to_wstring(count));
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        if (path.empty()) continue;
        const auto duplicate = std::find_if(state_.items.begin(), state_.items.end(), [&](const OrganizerItem& item) {
            return CompareStringOrdinal(item.path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (duplicate != state_.items.end()) continue;
        state_.items.push_back({path, DisplayNameForPath(path)});
    }
    DragFinish(drop);
    if (!EnsureCapacity(true)) {
        diagnostic_log::Write(L"items.drop.failed",
                              L"group=" + state_.name + L" reason=insufficient-capacity");
        state_.items.resize(originalCount);
        MessageBoxW(window_, L"当前屏幕空间不足，无法加入这些项目。", L"桌面收纳", MB_OK | MB_ICONINFORMATION);
        UpdateTooltips();
        UpdateGlass();
        return;
    }
    for (size_t index = originalCount; index < state_.items.size(); ++index) {
        std::filesystem::path copiedSource;
        // A failed absorption degrades to attribute hiding, not failure.
        if (app_.AbsorbDesktopItem(state_.id, state_.items[index], &copiedSource) &&
            !copiedSource.empty()) {
            pendingAbsorptions.emplace_back(copiedSource, state_.items[index].path);
        }
    }
    if (!app_.Save()) {
        for (const auto& [source, copy] : pendingAbsorptions) {
            std::error_code ignored;
            std::filesystem::remove_all(path_io::ExtendedLengthPath(copy), ignored);
        }
        state_ = originalState;
        EnsureCapacity(true);
        diagnostic_log::Write(L"items.drop.failed",
                              L"group=" + state_.name + L" reason=config-save");
        MessageBoxW(window_, L"无法保存分组配置，桌面项目没有被移动。", L"桌面收纳", MB_OK | MB_ICONWARNING);
    } else {
        size_t deleteFailures = 0;
        for (const auto& [source, copy] : pendingAbsorptions) {
            std::error_code error;
            const auto removed = std::filesystem::remove_all(path_io::ExtendedLengthPath(source), error);
            if (!error && removed > 0) NotifyDesktopShortcutChange(SHCNE_DELETE, source);
            else ++deleteFailures;
        }
        if (deleteFailures) {
            diagnostic_log::Write(L"items.drop.partial",
                                  L"group=" + state_.name + L" added=" +
                                      std::to_wstring(state_.items.size() - originalCount) +
                                      L" deleteFailures=" + std::to_wstring(deleteFailures));
            MessageBoxW(window_,
                        L"项目已收纳，但部分桌面原件未能删除。\n它们是安全的重复项，可手动清理。",
                        L"桌面收纳", MB_OK | MB_ICONINFORMATION);
        }
        if (!deleteFailures) {
            diagnostic_log::Write(L"items.drop.succeeded",
                                  L"group=" + state_.name + L" added=" +
                                      std::to_wstring(state_.items.size() - originalCount));
        }
    }
    UpdateTooltips();
    UpdateGlass();
}

LRESULT CALLBACK ContainerWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ContainerWindow* self = reinterpret_cast<ContainerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ContainerWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ContainerWindow::ContentProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ContainerWindow* self = reinterpret_cast<ContainerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ContainerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    if (!IsWindowEnabled(self->window_) &&
        (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_LBUTTONDOWN ||
         message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONUP ||
         message == WM_DROPFILES)) return 0;
    if (message == WM_PAINT) { self->PaintContent(window); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(self->window_, &point);
        return self->HitVisibleSurface(point) ? HTCLIENT : HTTRANSPARENT;
    }
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return SendMessageW(self->window_, message, wParam, lParam);
    }
    if (message == WM_MOUSELEAVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_LBUTTONDBLCLK || message == WM_RBUTTONUP || message == WM_SETCURSOR || message == WM_DROPFILES)
        return SendMessageW(self->window_, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ContainerWindow::TintProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ContainerWindow* self = reinterpret_cast<ContainerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ContainerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    if (!IsWindowEnabled(self->window_) &&
        (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE || message == WM_LBUTTONDOWN ||
         message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONUP ||
         message == WM_DROPFILES)) return 0;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(self->window_, &point);
        return self->HitVisibleSurface(point) ? HTCLIENT : HTTRANSPARENT;
    }
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return SendMessageW(self->window_, message, wParam, lParam);
    }
    if (message == WM_MOUSELEAVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_LBUTTONDBLCLK || message == WM_RBUTTONUP || message == WM_SETCURSOR ||
        message == WM_DROPFILES)
        return SendMessageW(self->window_, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ContainerWindow::DragProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ContainerWindow::HoverProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    ContainerWindow* self = reinterpret_cast<ContainerWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ContainerWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    if (message == WM_PAINT) {
        self->PaintHoverName(window);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ContainerWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        EndPaint(window_, &paint);
        if (contentWindow_) InvalidateRect(contentWindow_, nullptr, TRUE);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<RECT*>(lParam);
        dpi_ = HIWORD(wParam);
        if (hoverFont_) {
            DeleteObject(hoverFont_);
            hoverFont_ = nullptr;
        }
        UpdateMetrics();
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, OuterWidth(HalfColumns()), OuterHeight(HalfRows()), SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateShape();
        return 0;
    }
    case WM_SIZE:
        if (!suppressRefresh_ && !movingFastPath_) {
            SyncDesktopLayer();
            UpdateShape();
            UpdateTooltips();
        }
        return 0;
    case WM_DISPLAYCHANGE: EnsureVisible(); return 0;
    case WM_SETTINGCHANGE: return 0;
    case WM_LBUTTONDOWN: HideHoverName(); BeginPointerAction({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}); return 0;
    case WM_MOUSEMOVE:
        if (pointerActive_) {
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(window_, &screen);
            ContinuePointerAction(screen, (wParam & MK_CONTROL) != 0);
        } else {
            if (state_.collapsed && !hoverCollapsed_) {
                hoverCollapsed_ = true;
                UpdateGlass();
            }
            POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto item = HitItem(client);
            if (item != hoveredItem_) {
                HideHoverName();
                hoveredItem_ = item;
                if (hoveredItem_) SetTimer(window_, 2, kHoverDelayMs, nullptr);
                SetTimer(window_, 7, kHoverVisualDelayMs, nullptr);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        HideHoverName();
        if (hoverCollapsed_) {
            hoverCollapsed_ = false;
            if (state_.collapsed) UpdateGlass();
        }
        if (hoveredItem_) {
            hoveredItem_.reset();
            SetTimer(window_, 7, kHoverVisualDelayMs, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wParam == 2) { KillTimer(window_, 2); ShowHoverName(); return 0; }
        if (wParam == 7) {
            KillTimer(window_, 7);
            if (paintedHoverItem_ != hoveredItem_) {
                paintedHoverItem_ = hoveredItem_;
                UpdateGlass();
            }
            return 0;
        }
        if (wParam == 3) { StepRectAnimation(); return 0; }
        if (wParam == 4) { StepItemAnimation(); return 0; }
        if (wParam == 6) { ProcessShellItemReconciles(); return 0; }
        if (wParam == kResizePreviewTimer) { StepResizePreviewAnimation(); return 0; }
        break;
    case WM_LBUTTONUP: {
        POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window_, &screen);
        EndPointerAction(screen, (wParam & MK_CONTROL) != 0);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pointerActive_) {
            const bool hadDragPreview = draggingItem_;
            const bool hadSizingPreview = sizingPreview_ && gestureChanged_ &&
                                          resizePreviewRect_.has_value();
            const bool hadMove = moving_ && !pressedItem_ && gestureChanged_;
            const int interruptedGridShiftX = hadSizingPreview && (resizeEdges_ & 1)
                ? resizePreviewHalfColumns_ - HalfColumns() : 0;
            const int interruptedGridShiftY = hadSizingPreview && (resizeEdges_ & 4)
                ? resizePreviewHalfRows_ - HalfRows() : 0;
            pointerActive_ = false;
            moving_ = false;
            movingFastPath_ = false;
            resizeEdges_ = 0;
            pressedItem_.reset();
            draggingItem_ = false;
            if (hadSizingPreview) {
                for (auto& item : state_.items) {
                    if (item.gridX >= 0) item.gridX += interruptedGridShiftX;
                    if (item.gridY >= 0) item.gridY += interruptedGridShiftY;
                }
                state_.columns = resizePreviewHalfColumns_ / 2;
                state_.halfColumn = (resizePreviewHalfColumns_ % 2) != 0;
                state_.rows = resizePreviewHalfRows_ / 2;
                state_.halfRow = (resizePreviewHalfRows_ % 2) != 0;
                StartRectAnimation(*resizePreviewRect_);
            }
            sizingPreview_ = rectAnimationActive_;
            if (!rectAnimationActive_ && !resizePreviewAnimationActive_) DestroyResizePreview();
            if (hadDragPreview) {
                KillTimer(window_, 4);
                DestroyDragGhost();
                itemAnimationIndex_.reset();
                itemAnimationOffset_ = {};
                dragPreviewPlacement_.reset();
                DestroyDragPlaceholder();
                if (dragTargetPreview_) {
                    dragTargetPreview_->ClearExternalDragPreview();
                    dragTargetPreview_ = nullptr;
                }
            }
            if (hadSizingPreview || hadMove) {
                if (hadMove && state_.snapToGrid && !bypassSnapOnRelease_) {
                    RECT snapped{};
                    if (moveTargetRect_) snapped = *moveTargetRect_;
                    else {
                        GetWindowRect(window_, &snapped);
                        SnapRectToDesktopGrid(snapped);
                    }
                    SetWindowPos(window_, nullptr, snapped.left, snapped.top, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                DestroyMoveTargetPreview();
                if (hadMove) SyncDesktopLayer();
                if (rectAnimationActive_) {
                    state_.bounds = rectAnimationTarget_;
                    state_.monitor = MonitorDevice(MonitorFromRect(&state_.bounds, MONITOR_DEFAULTTONEAREST));
                } else {
                    UpdateStateBounds();
                }
                if (state_.collapsed && state_.snapToGrid && state_.pushIcons) {
                    RECT area{};
                    GetWindowRect(window_, &area);
                    app_.PushDesktopIconsOutOf(area, this);
                }
            }
            if (!rectAnimationActive_) {
                UpdateTooltips();
                UpdateGlass();
            }
            if (hadSizingPreview || hadMove) app_.Save();
            bypassSnapOnRelease_ = false;
            // Capture can be taken by Alt+Tab, a system gesture, or another
            // window before WM_LBUTTONUP arrives. Do not leave this group in
            // the temporary application-topmost band in that case.
            RestoreFromPointerInteraction();
        }
        DestroyMoveTargetPreview();
        return 0;
    case WM_LBUTTONDBLCLK: {
        if (state_.collapsed) {
            SetCollapsed(false, app_.GetGlobalStyle().centerExpandedGroups);
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (const auto item = HitItem(point)) OpenItem(*item);
        else {
            // Double-clicking real empty space or the title/name collapses the
            // group, just like a folder. Resize edges stay reserved for
            // resizing so an edge double-click never misfires.
            RECT client{};
            GetClientRect(window_, &client);
            const bool onResizeEdge = point.x < resizeBorder_ || point.x >= client.right - resizeBorder_ ||
                                      point.y < resizeBorder_ || point.y >= client.bottom - resizeBorder_;
            if (!onResizeEdge) SetCollapsed(true);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        HideHoverName();
        POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const auto item = HitItem(client);
        ClientToScreen(window_, &client);
        ShowContextMenu(client, item);
        return 0;
    }
    case WM_DROPFILES: AddDroppedFiles(reinterpret_cast<HDROP>(wParam)); return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && header->hwndFrom == tooltipWindow_ && header->code == TTN_GETDISPINFOW) {
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            const size_t index = static_cast<size_t>(header->idFrom - 1);
            if (index < state_.items.size()) info->lpszText = const_cast<wchar_t*>(state_.items[index].name.c_str());
            return 0;
        }
        break;
    }
    case WM_SETCURSOR:
        if (!state_.locked && LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window_, &point);
            RECT client{};
            GetClientRect(window_, &client);
            const bool nearLeftCorner = point.x < resizeCorner_;
            const bool nearRightCorner = point.x >= client.right - resizeCorner_;
            const bool nearTopCorner = point.y < resizeCorner_;
            const bool nearBottomCorner = point.y >= client.bottom - resizeCorner_;
            const bool left = point.x < resizeBorder_ || ((nearTopCorner || nearBottomCorner) && nearLeftCorner);
            const bool right = point.x >= client.right - resizeBorder_ || ((nearTopCorner || nearBottomCorner) && nearRightCorner);
            const bool top = point.y < resizeBorder_ || ((nearLeftCorner || nearRightCorner) && nearTopCorner);
            const bool bottom = point.y >= client.bottom - resizeBorder_ || ((nearLeftCorner || nearRightCorner) && nearBottomCorner);
            LPCWSTR cursor = IDC_ARROW;
            if ((left && top) || (right && bottom)) cursor = IDC_SIZENWSE;
            else if ((right && top) || (left && bottom)) cursor = IDC_SIZENESW;
            else if (left || right) cursor = IDC_SIZEWE;
            else if (top || bottom) cursor = IDC_SIZENS;
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window_, &point);
        return HitVisibleSurface(point) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_DESTROY: window_ = nullptr; return 0;
    default: return DefWindowProcW(window_, message, wParam, lParam);
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

Application::~Application() {
    if (gMouseHookApplication == this) gMouseHookApplication = nullptr;
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (liveDesktopCapture_) liveDesktopCapture_->Stop();
    if (messageWindow_) DeregisterShellHookWindow(messageWindow_);
    RemoveTrayIcon();
    desktopFrontContainer_ = nullptr;
    containers_.clear();
    for (auto& [path, icon] : icons_) if (icon) DestroyIcon(icon);
    ReleaseDesktopBackdrop();
    ReleaseRenderSurface();
    timeEndPeriod(1);
}

DWORD* Application::AcquireRenderSurface(int width, int height, HDC* dcOut) {
    if (dcOut) *dcOut = nullptr;
    if (width <= 0 || height <= 0) return nullptr;
    if (renderDc_ && (width > renderBitmapWidth_ || height > renderBitmapHeight_))
        ReleaseRenderSurface();
    if (!renderDc_) {
        HDC screen = GetDC(nullptr);
        HDC layer = screen ? CreateCompatibleDC(screen) : nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!screen || !layer || !bitmap || !bits) {
            if (bitmap) DeleteObject(bitmap);
            if (layer) DeleteDC(layer);
            if (screen) ReleaseDC(nullptr, screen);
            return nullptr;
        }
        ReleaseDC(nullptr, screen);
        renderPreviousBitmap_ = SelectObject(layer, bitmap);
        renderDc_ = layer;
        renderBitmap_ = bitmap;
        renderBits_ = static_cast<DWORD*>(bits);
        renderBitmapWidth_ = width;
        renderBitmapHeight_ = height;
    }
    if (dcOut) *dcOut = renderDc_;
    return renderBits_;
}

void Application::ReleaseRenderSurface() {
    if (renderDc_ && renderPreviousBitmap_) SelectObject(renderDc_, renderPreviousBitmap_);
    if (renderBitmap_) DeleteObject(renderBitmap_);
    if (renderDc_) DeleteDC(renderDc_);
    renderDc_ = nullptr;
    renderBitmap_ = nullptr;
    renderPreviousBitmap_ = nullptr;
    renderBits_ = nullptr;
    renderBitmapWidth_ = 0;
    renderBitmapHeight_ = 0;
}

void Application::ReleaseDesktopBackdrop() {
    InvalidateGlassBaseCache();
    ReleaseBlurredBackdrop();
    if (desktopDc_ && desktopPreviousBitmap_) SelectObject(desktopDc_, desktopPreviousBitmap_);
    if (desktopBitmap_) DeleteObject(desktopBitmap_);
    if (desktopDc_) DeleteDC(desktopDc_);
    desktopDc_ = nullptr;
    desktopBitmap_ = nullptr;
    desktopPreviousBitmap_ = nullptr;
    desktopBits_ = nullptr;
    desktopBounds_ = {};
}

void Application::ReleaseBlurredBackdrop() {
    for (auto& [radius, entry] : blurredByRadius_) {
        if (entry.dc && entry.previousBitmap) SelectObject(entry.dc, entry.previousBitmap);
        if (entry.bitmap) DeleteObject(entry.bitmap);
        if (entry.dc) DeleteDC(entry.dc);
    }
    blurredByRadius_.clear();
}

// The frosted look needs the whole backdrop blurred, not just the slice
// under one window. Blur once per backdrop generation and blur radius so
// interactive move/resize never pays for a full-window BoxBlur per frame.
HDC Application::BlurredBackdropDC(int radius, int* scaleOut) {
    if (scaleOut) *scaleOut = 1;
    if (!desktopDc_ || radius <= 0) return desktopDc_;
    const int fullWidth = desktopBounds_.right - desktopBounds_.left;
    const int fullHeight = desktopBounds_.bottom - desktopBounds_.top;
    if (fullWidth <= 0 || fullHeight <= 0) return desktopDc_;
    // A frosted surface intentionally discards high-frequency detail. Blurring
    // a quarter-resolution backdrop is visually equivalent at these radii and
    // keeps animated wallpapers from monopolizing the UI thread.
    constexpr int kBackdropScale = 4;
    const int width = (fullWidth + kBackdropScale - 1) / kBackdropScale;
    const int height = (fullHeight + kBackdropScale - 1) / kBackdropScale;

    auto found = blurredByRadius_.find(radius);
    if (found != blurredByRadius_.end() && found->second.dc &&
        found->second.generation == desktopBackdropGeneration_ &&
        found->second.width == width && found->second.height == height &&
        found->second.scale == kBackdropScale) {
        found->second.lastUsed = GetTickCount64();
        if (scaleOut) *scaleOut = found->second.scale;
        return found->second.dc;
    }

    // Keep the number of cached radii bounded; evict the least recently used
    // before inserting a new radius (a fresh entry has lastUsed == 0 and
    // would otherwise evict itself).
    if (found == blurredByRadius_.end()) {
        constexpr size_t kMaxBlurredRadii = 4;
        while (blurredByRadius_.size() >= kMaxBlurredRadii) {
            auto oldest = blurredByRadius_.begin();
            for (auto it = std::next(oldest); it != blurredByRadius_.end(); ++it) {
                if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
            }
            if (oldest->second.dc && oldest->second.previousBitmap)
                SelectObject(oldest->second.dc, oldest->second.previousBitmap);
            if (oldest->second.bitmap) DeleteObject(oldest->second.bitmap);
            if (oldest->second.dc) DeleteDC(oldest->second.dc);
            blurredByRadius_.erase(oldest);
        }
    }

    auto& entry = blurredByRadius_[radius];
    if (entry.dc && entry.previousBitmap) SelectObject(entry.dc, entry.previousBitmap);
    if (entry.bitmap) DeleteObject(entry.bitmap);
    if (entry.dc) DeleteDC(entry.dc);
    entry = {};

    HDC screen = GetDC(nullptr);
    HDC layer = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!screen || !layer || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (layer) DeleteDC(layer);
        if (screen) ReleaseDC(nullptr, screen);
        blurredByRadius_.erase(radius);
        return desktopDc_;
    }
    HGDIOBJ previous = SelectObject(layer, bitmap);
    SetStretchBltMode(layer, HALFTONE);
    SetBrushOrgEx(layer, 0, 0, nullptr);
    StretchBlt(layer, 0, 0, width, height, desktopDc_, 0, 0,
               fullWidth, fullHeight, SRCCOPY);
    std::vector<DWORD> pixels(static_cast<DWORD*>(bits),
                              static_cast<DWORD*>(bits) + static_cast<size_t>(width) * height);
    const int scaledRadius = std::max(1, (radius + kBackdropScale - 1) / kBackdropScale);
    BoxBlur(pixels, width, height, scaledRadius);
    BoxBlur(pixels, width, height, std::max(1, scaledRadius / 2));
    std::copy(pixels.begin(), pixels.end(), static_cast<DWORD*>(bits));
    ReleaseDC(nullptr, screen);

    entry.dc = layer;
    entry.bitmap = bitmap;
    entry.previousBitmap = previous;
    entry.width = width;
    entry.height = height;
    entry.scale = kBackdropScale;
    entry.generation = desktopBackdropGeneration_;
    entry.lastUsed = GetTickCount64();
    if (scaleOut) *scaleOut = entry.scale;
    return entry.dc;
}

void Application::InvalidateGlassBaseCache() {
    glassBaseCaches_.clear();
    glassBaseCachePixels_ = 0;
}

bool Application::TryGlassBaseCache(HWND target, const RECT& screenRect,
                                    const ContainerState& state, UINT dpi, bool dark,
                                    DWORD* destination, size_t destStride, size_t pixelCount) {
    const auto found = glassBaseCaches_.find(target);
    if (found == glassBaseCaches_.end()) return false;
    auto& entry = found->second;
    if (!EqualRect(&entry.screenRect, &screenRect) ||
        entry.width != screenRect.right - screenRect.left ||
        entry.height != screenRect.bottom - screenRect.top || entry.dpi != dpi ||
        entry.opacity != state.opacity || entry.blur != state.blur ||
        entry.cornerRadius != state.cornerRadius || entry.tintMode != state.tintMode ||
        entry.tintColor != state.tintColor || entry.showBorder != state.showBorder ||
        entry.collapsed != state.collapsed || entry.dark != dark ||
        entry.backdropGeneration != desktopBackdropGeneration_ ||
        entry.pixels.size() != pixelCount) {
        glassBaseCachePixels_ -= entry.pixels.size();
        glassBaseCaches_.erase(found);
        return false;
    }
    // Cached rows are packed; the destination bitmap may use a wider stride.
    const size_t rowPixels = static_cast<size_t>(entry.width);
    const int rows = entry.height;
    if (destStride == rowPixels) {
        std::copy(entry.pixels.begin(), entry.pixels.end(), destination);
    } else {
        for (int y = 0; y < rows; ++y) {
            std::copy_n(entry.pixels.data() + static_cast<size_t>(y) * rowPixels, rowPixels,
                        destination + static_cast<size_t>(y) * destStride);
        }
    }
    entry.lastUsed = GetTickCount64();
    return true;
}

void Application::StoreGlassBaseCache(HWND target, const RECT& screenRect,
                                      const ContainerState& state, UINT dpi, bool dark,
                                      const DWORD* pixels, size_t sourceStride, size_t pixelCount) {
    const auto existing = glassBaseCaches_.find(target);
    if (existing != glassBaseCaches_.end()) {
        glassBaseCachePixels_ -= existing->second.pixels.size();
        glassBaseCaches_.erase(existing);
    }
    if (pixelCount == 0 || pixelCount > kGlassBaseCachePixelBudget) return;
    while (!glassBaseCaches_.empty() &&
           glassBaseCachePixels_ > kGlassBaseCachePixelBudget - pixelCount) {
        auto oldest = glassBaseCaches_.begin();
        for (auto it = std::next(oldest); it != glassBaseCaches_.end(); ++it) {
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        }
        glassBaseCachePixels_ -= oldest->second.pixels.size();
        glassBaseCaches_.erase(oldest);
    }

    GlassBaseCacheEntry entry;
    entry.screenRect = screenRect;
    entry.width = screenRect.right - screenRect.left;
    entry.height = screenRect.bottom - screenRect.top;
    entry.dpi = dpi;
    entry.opacity = state.opacity;
    entry.blur = state.blur;
    entry.cornerRadius = state.cornerRadius;
    entry.tintMode = state.tintMode;
    entry.tintColor = state.tintColor;
    entry.showBorder = state.showBorder;
    entry.collapsed = state.collapsed;
    entry.dark = dark;
    entry.backdropGeneration = desktopBackdropGeneration_;
    entry.lastUsed = GetTickCount64();
    try {
        entry.pixels.resize(pixelCount);
        const size_t rowPixels = static_cast<size_t>(entry.width);
        if (sourceStride == rowPixels) {
            std::copy_n(pixels, pixelCount, entry.pixels.data());
        } else {
            for (int y = 0; y < entry.height; ++y) {
                std::copy_n(pixels + static_cast<size_t>(y) * sourceStride, rowPixels,
                            entry.pixels.data() + static_cast<size_t>(y) * rowPixels);
            }
        }
        glassBaseCaches_.emplace(target, std::move(entry));
        glassBaseCachePixels_ += pixelCount;
    } catch (const std::bad_alloc&) {
        // Rendering must still succeed when the optional cache cannot grow.
    }
}

bool Application::CaptureDesktopBackdrop() {
    RECT bounds{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    bounds.right = bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.bottom = bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return false;

    HDC screen = GetDC(nullptr);
    HDC capture = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO captureInfo{};
    captureInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    captureInfo.bmiHeader.biWidth = bounds.right - bounds.left;
    captureInfo.bmiHeader.biHeight = -(bounds.bottom - bounds.top);
    captureInfo.bmiHeader.biPlanes = 1;
    captureInfo.bmiHeader.biBitCount = 32;
    captureInfo.bmiHeader.biCompression = BI_RGB;
    DWORD* captureBits = nullptr;
    HBITMAP bitmap = screen ? CreateDIBSection(screen, &captureInfo, DIB_RGB_COLORS,
                                               reinterpret_cast<void**>(&captureBits), nullptr, 0) : nullptr;
    if (!screen || !capture || !bitmap || !captureBits) {
        if (bitmap) DeleteObject(bitmap);
        if (capture) DeleteDC(capture);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ previous = SelectObject(capture, bitmap);
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    wchar_t syntheticBackdrop[2]{};
    if (GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_TEST_BACKDROP", syntheticBackdrop,
                                static_cast<DWORD>(std::size(syntheticBackdrop))) > 0) {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int checker = ((x / 18) + (y / 14)) & 1;
                const int blue = std::clamp(42 + (x * 173 / std::max(1, width - 1)) + checker * 34, 0, 255);
                const int green = std::clamp(58 + (y * 151 / std::max(1, height - 1)) + checker * 22, 0, 255);
                const int red = std::clamp(214 - (x * 91 / std::max(1, width - 1)) + checker * 28, 0, 255);
                captureBits[static_cast<size_t>(y) * width + x] = PixelFromChannels(blue, green, red);
            }
        }
        ReleaseDC(nullptr, screen);
        ReleaseDesktopBackdrop();
        desktopDc_ = capture;
        desktopBitmap_ = bitmap;
        desktopPreviousBitmap_ = previous;
        desktopBits_ = captureBits;
        desktopBounds_ = bounds;
        ++desktopBackdropGeneration_;
        return true;
    }
#endif
    COLORREF background = GetSysColor(COLOR_DESKTOP);
    IDesktopWallpaper* wallpaper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wallpaper)))) {
        wallpaper->GetBackgroundColor(&background);
    }
    RECT localBounds{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(capture, &localBounds, backgroundBrush);
    DeleteObject(backgroundBrush);

    IWICImagingFactory* factory = nullptr;
    bool drewWallpaper = false;
    if (wallpaper && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
        wallpaper->GetPosition(&position);
        UINT monitorCount = 0;
        wallpaper->GetMonitorDevicePathCount(&monitorCount);
        std::map<std::wstring, WallpaperImage, std::less<>> decoded;
        auto drawPath = [&](const std::wstring& path, const RECT& area,
                            DESKTOP_WALLPAPER_POSITION drawPosition) {
            if (path.empty()) return;
            auto [it, inserted] = decoded.try_emplace(path);
            if (inserted && !DecodeWallpaper(factory, path, it->second)) {
                decoded.erase(it);
                return;
            }
            DrawWallpaper(capture, it->second, area, drawPosition);
            drewWallpaper = true;
        };

        bool drewSpan = false;
        for (UINT index = 0; index < monitorCount; ++index) {
            LPWSTR monitorId = nullptr;
            LPWSTR path = nullptr;
            if (FAILED(wallpaper->GetMonitorDevicePathAt(index, &monitorId)) || !monitorId) continue;
            RECT monitor{};
            if (position == DWPOS_SPAN) {
                if (!drewSpan && SUCCEEDED(wallpaper->GetWallpaper(nullptr, &path)) && path) {
                    drawPath(path, localBounds, DWPOS_FILL);
                    drewSpan = true;
                }
            } else if (SUCCEEDED(wallpaper->GetMonitorRECT(monitorId, &monitor)) &&
                       SUCCEEDED(wallpaper->GetWallpaper(monitorId, &path)) && path) {
                OffsetRect(&monitor, -bounds.left, -bounds.top);
                drawPath(path, monitor, position);
            }
            if (path) CoTaskMemFree(path);
            CoTaskMemFree(monitorId);
        }
        factory->Release();
    }
    if (wallpaper) wallpaper->Release();
    // WIC cannot decode slideshows, Spotlight images, or solid-color setups.
    // Without a real backdrop the glass renders as a flat solid panel, so
    // fall back to capturing the shell window (or the raw screen) instead.
    if (!drewWallpaper) {
        HWND shell = GetShellWindow();
        HWND desktopList = nullptr;
        if (shell) {
            EnumChildWindows(shell, [](HWND child, LPARAM data) {
                wchar_t className[64]{};
                GetClassNameW(child, className, static_cast<int>(std::size(className)));
                if (wcscmp(className, L"SysListView32") == 0) {
                    *reinterpret_cast<HWND*>(data) = child;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&desktopList));
        }
        const bool desktopIconsVisible = desktopList && IsWindowVisible(desktopList);
        if (desktopIconsVisible) ShowWindow(desktopList, SW_HIDE);
        const BOOL printed = shell ? PrintWindow(shell, capture, 2) : FALSE;
        if (!printed) {
            BitBlt(capture, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                   screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);
        }
        if (desktopIconsVisible) ShowWindow(desktopList, SW_SHOWNOACTIVATE);
    }
    ReleaseDC(nullptr, screen);

    ReleaseDesktopBackdrop();
    desktopDc_ = capture;
    desktopBitmap_ = bitmap;
    desktopPreviousBitmap_ = previous;
    desktopBits_ = captureBits;
    desktopBounds_ = bounds;
    ++desktopBackdropGeneration_;
    return true;
}

bool Application::ApplyLatestLiveDesktopFrame() {
    if (!liveDesktopCapture_ || !liveDesktopCapture_->TakeLatestFrame(liveDesktopFrame_)) return false;
    const int width = liveDesktopFrame_.width;
    const int height = liveDesktopFrame_.height;
    if (width <= 0 || height <= 0) return false;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (liveDesktopFrame_.pixels.size() != pixelCount) return false;

    const bool reusable = desktopDc_ && desktopBitmap_ && desktopBits_ &&
        desktopBounds_.left == liveDesktopFrame_.bounds.left &&
        desktopBounds_.top == liveDesktopFrame_.bounds.top &&
        desktopBounds_.right - desktopBounds_.left == width &&
        desktopBounds_.bottom - desktopBounds_.top == height;
    if (!reusable) {
        ReleaseDesktopBackdrop();
        HDC screen = GetDC(nullptr);
        HDC capture = screen ? CreateCompatibleDC(screen) : nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        DWORD* bits = nullptr;
        HBITMAP bitmap = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                                   reinterpret_cast<void**>(&bits), nullptr, 0) : nullptr;
        if (!screen || !capture || !bitmap || !bits) {
            if (bitmap) DeleteObject(bitmap);
            if (capture) DeleteDC(capture);
            if (screen) ReleaseDC(nullptr, screen);
            return false;
        }
        desktopPreviousBitmap_ = SelectObject(capture, bitmap);
        desktopDc_ = capture;
        desktopBitmap_ = bitmap;
        desktopBits_ = bits;
        desktopBounds_ = liveDesktopFrame_.bounds;
        desktopBounds_.right = desktopBounds_.left + width;
        desktopBounds_.bottom = desktopBounds_.top + height;
        ReleaseDC(nullptr, screen);
    } else {
        InvalidateGlassBaseCache();
        ReleaseBlurredBackdrop();
    }
    std::copy_n(liveDesktopFrame_.pixels.data(), pixelCount, desktopBits_);
    GdiFlush();
    ++desktopBackdropGeneration_;
    return true;
}

void Application::StartLiveDesktopCapture() {
    if (!VisualTestLiveCaptureEnabled() || !messageWindow_) return;
    if (!liveDesktopCapture_) liveDesktopCapture_ = std::make_unique<LiveDesktopCapture>();
    desktopOwner_ = desktop_capture_target::Find();
    if (desktopOwner_) liveDesktopCapture_->Start(desktopOwner_, messageWindow_, kLiveBackdropMessage);
}

bool Application::RenderGlass(HWND target, const RECT& screenRect, const ContainerState& state, UINT dpi,
                              bool sizingPreview, std::optional<size_t> hoveredItem,
                              std::optional<size_t> animatedItem, POINT itemOffset, bool hideAnimatedItem,
                              bool transientFrame, int collapsedVisual) {
    // Hover and press visual states moved back to the original look, which
    // has no per-item highlight overlay; keep the parameters for symmetry.
    (void)hoveredItem;
    (void)collapsedVisual;
    (void)transientFrame;
    // Optional frame profiling: set DESKTOP_ORGANIZER_PROFILE to a log path.
    FILE* profileLog = [] {
        static FILE* log = [] {
            wchar_t path[32768]{};
            const DWORD n = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_PROFILE", path,
                                                    static_cast<DWORD>(std::size(path)));
            if (n == 0 || n >= std::size(path)) return static_cast<FILE*>(nullptr);
            FILE* f = nullptr;
            _wfopen_s(&f, path, L"ab");
            return f;
        }();
        return log;
    }();
    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    const auto profNow = [&] {
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    };
    const auto profMs = [&](LONGLONG from, LONGLONG to) {
        return static_cast<double>(to - from) * 1000.0 / static_cast<double>(qpf.QuadPart);
    };
    const LONGLONG profT0 = profileLog ? profNow() : 0;
    if (!target) return false;
    // The container is a per-pixel glass surface built from the latest Explorer
    // desktop-composition frame. Windows Graphics Capture supplies live
    // wallpapers without including our separate top-level organizer windows;
    // the static wallpaper decoder remains the compatibility fallback.
    const int width = screenRect.right - screenRect.left;
    const int height = screenRect.bottom - screenRect.top;
    if (width <= 0 || height <= 0) return false;
    if (static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height) >
        std::numeric_limits<size_t>::max() / sizeof(DWORD)) return false;

    HDC screen = GetDC(nullptr);
    HDC layer = nullptr;
    DWORD* source = AcquireRenderSurface(width, height, &layer);
    if (!screen || !layer || !source) {
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }
    const size_t stride = static_cast<size_t>(renderBitmapWidth_);
    const bool dark = UsesDarkGlass(state);
    const LONGLONG profT1 = profileLog ? profNow() : 0;

    const COLORREF customTint = TintColor(state);
    const int tintBlue = state.tintMode == 3 ? GetBValue(customTint) : (dark ? 43 : 252);
    const int tintGreen = state.tintMode == 3 ? GetGValue(customTint) : (dark ? 38 : 248);
    const int tintRed = state.tintMode == 3 ? GetRValue(customTint) : (dark ? 25 : 234);
    // The opacity slider controls only the glass color wash. Blur strength is
    // independent and maps to a real pixel radius below.
    const int tintAlpha = std::clamp(MulDiv(state.opacity, 255, 100), 0, 255);
    std::vector<DWORD> sharpBackdrop;
    std::vector<DWORD> blurredBackdrop;
    const int maximumBlurRadius = std::max(1, ScaleDip(24, dpi));
    if (state.blur > 0 && !desktopDc_) CaptureDesktopBackdrop();
    int blurredBackdropScale = 1;
    HDC maximumBlurDc = state.blur > 0
        ? BlurredBackdropDC(maximumBlurRadius, &blurredBackdropScale) : nullptr;
    const bool hasSharpBackdrop = state.blur > 0 && desktopDc_ &&
        CopyBackdropRegion(desktopDc_, desktopBounds_, screenRect, sharpBackdrop);
    const bool hasBlurredBackdrop = state.blur > 0 && maximumBlurDc &&
        CopyBackdropRegion(maximumBlurDc, desktopBounds_, screenRect,
                           blurredBackdrop, blurredBackdropScale);
    // Mix between the current sharp frame and one shared maximum-blur frame.
    // The ease-out curve preserves fine control at 1% while making middle
    // slider values feel more like physical frosted glass. All containers,
    // even with different percentages, share the same expensive blur pass.
    const int blurPercent = std::clamp(state.blur, 0, 100);
    const int blurMix = hasBlurredBackdrop
        ? std::clamp(MulDiv(blurPercent * (300 - blurPercent), 255, 20000), 0, 255) : 0;
    const int sharpMix = 255 - blurMix;
    // The whole frame is one rounded glass sheet in both states: collapsed
    // groups are a fixed 104 DIP square card showing the four-item preview.
    // Both states share the user's corner radius so the global console's
    // radius slider visibly applies to collapsed cards as well.
    const RECT shapeRect{0, 0, width, height};
    const double radius = std::min<double>(ScaleDip(state.cornerRadius, dpi),
                                           std::min(width, height) / 2.0);
    const double borderWidth = state.showBorder ? std::max(1, ScaleDip(1, dpi)) : 0;
    const auto roundedRectCoverage = [](double px, double py, double left, double top,
                                        double right, double bottom, double shapeRadius) {
        const double shapeWidth = right - left;
        const double shapeHeight = bottom - top;
        if (shapeWidth <= 0.0 || shapeHeight <= 0.0) return 0.0;
        shapeRadius = std::clamp(shapeRadius, 0.0, std::min(shapeWidth, shapeHeight) / 2.0);
        const double qx = std::abs(px - (left + right) / 2.0) - (shapeWidth / 2.0 - shapeRadius);
        const double qy = std::abs(py - (top + bottom) / 2.0) - (shapeHeight / 2.0 - shapeRadius);
        // hypot is only needed in the rounded corner octants; everywhere else
        // the Euclidean term collapses to max(qx, qy, 0), which is exact.
        const double ax = std::max(qx, 0.0);
        const double ay = std::max(qy, 0.0);
        const double euclidean = (ax > 0.0 && ay > 0.0) ? std::hypot(ax, ay) : std::max(ax, ay);
        const double distance = euclidean + std::min(std::max(qx, qy), 0.0) - shapeRadius;
        return std::clamp(0.5 - distance, 0.0, 1.0);
    };
    const auto roundedCoverage = [&](double px, double py, double inset) {
        const double shapeRadius = std::max(0.0, radius - inset);
        return roundedRectCoverage(px, py, shapeRect.left + inset, shapeRect.top + inset,
                                   shapeRect.right - inset, shapeRect.bottom - inset, shapeRadius);
    };

    // With blur disabled this remains a genuinely transparent tint. With blur
    // enabled we paint a freshly captured and blurred copy of the real desktop
    // into the same rounded per-pixel surface, then mix the chosen glass tint.
    GdiFlush();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int washBlue = tintBlue;
            int washGreen = tintGreen;
            int washRed = tintRed;
            if (!dark && state.tintMode != 3) {
                const int horizontal = std::max(1, width - 1);
                const int lavender = (width - 1 - x) * 92 / horizontal;
                const int mint = x * 96 / horizontal;
                washBlue = (washBlue * (255 - lavender) + 255 * lavender + 127) / 255;
                washGreen = (washGreen * (255 - lavender) + 215 * lavender + 127) / 255;
                washRed = (washRed * (255 - lavender) + 205 * lavender + 127) / 255;
                washBlue = (washBlue * (255 - mint) + 237 * mint + 127) / 255;
                washGreen = (washGreen * (255 - mint) + 249 * mint + 127) / 255;
                washRed = (washRed * (255 - mint) + 192 * mint + 127) / 255;
            }

            const double outerCoverage = roundedCoverage(x + 0.5, y + 0.5, 0.0);
            int blue = washBlue;
            int green = washGreen;
            int red = washRed;
            int alpha = static_cast<int>(std::lround(outerCoverage * tintAlpha));
            if (hasBlurredBackdrop) {
                const size_t backdropIndex = static_cast<size_t>(y) * width + x;
                DWORD backdrop = blurredBackdrop[backdropIndex];
                if (hasSharpBackdrop && sharpBackdrop.size() == blurredBackdrop.size()) {
                    const DWORD sharp = sharpBackdrop[backdropIndex];
                    const int mixedBlue = (static_cast<int>(sharp & 0xff) * sharpMix +
                                           static_cast<int>(backdrop & 0xff) * blurMix + 127) / 255;
                    const int mixedGreen = (static_cast<int>((sharp >> 8) & 0xff) * sharpMix +
                                            static_cast<int>((backdrop >> 8) & 0xff) * blurMix + 127) / 255;
                    const int mixedRed = (static_cast<int>((sharp >> 16) & 0xff) * sharpMix +
                                          static_cast<int>((backdrop >> 16) & 0xff) * blurMix + 127) / 255;
                    backdrop = PixelFromChannels(mixedBlue, mixedGreen, mixedRed);
                }
                const int backdropBlue = static_cast<int>(backdrop & 0xff);
                const int backdropGreen = static_cast<int>((backdrop >> 8) & 0xff);
                const int backdropRed = static_cast<int>((backdrop >> 16) & 0xff);
                blue = (backdropBlue * (255 - tintAlpha) + washBlue * tintAlpha + 127) / 255;
                green = (backdropGreen * (255 - tintAlpha) + washGreen * tintAlpha + 127) / 255;
                red = (backdropRed * (255 - tintAlpha) + washRed * tintAlpha + 127) / 255;
                alpha = static_cast<int>(std::lround(outerCoverage * 255.0));
            }
            if (borderWidth > 0.0) {
                const double innerCoverage = roundedCoverage(x + 0.5, y + 0.5, borderWidth);
                int borderMix = static_cast<int>(std::lround(
                    std::clamp(outerCoverage - innerCoverage, 0.0, 1.0) * 255.0));
                // When a copied backdrop makes the layered surface opaque,
                // keep the border's effective opacity equal to the transparent
                // no-blur path. Otherwise the border flashes fully white as
                // soon as blur changes from 0% to 1%.
                if (hasBlurredBackdrop) borderMix = borderMix * tintAlpha / 255;
                const int borderBlue = dark ? 150 : 255;
                const int borderGreen = dark ? 142 : 255;
                const int borderRed = dark ? 110 : 255;
                blue = (blue * (255 - borderMix) + borderBlue * borderMix + 127) / 255;
                green = (green * (255 - borderMix) + borderGreen * borderMix + 127) / 255;
                red = (red * (255 - borderMix) + borderRed * borderMix + 127) / 255;
            }
            // Store premultiplied BGRA for AC_SRC_OVER.
            blue = blue * alpha / 255;
            green = green * alpha / 255;
            red = red * alpha / 255;
            source[static_cast<size_t>(y) * stride + x] =
                PixelFromChannels(blue, green, red) | (static_cast<DWORD>(alpha) << 24);
        }
    }
    const LONGLONG profT2 = profileLog ? profNow() : 0;

    // Draw labels and Shell icons on top of the translucent tint. The user can
    // choose any label color; use a contrasting shadow to preserve readability
    // over both bright wallpapers and dark live content.
    SetBkMode(layer, TRANSPARENT);
    const COLORREF labelColor = RGB((state.textColor >> 16) & 0xff,
                                    (state.textColor >> 8) & 0xff,
                                    state.textColor & 0xff);
    const int labelLuminance = (GetRValue(labelColor) * 299 +
                                GetGValue(labelColor) * 587 +
                                GetBValue(labelColor) * 114) / 1000;
    const COLORREF shadowColor = labelLuminance >= 140 ? RGB(18, 18, 18) : RGB(250, 252, 253);
    const auto drawLabelText = [&](HDC dc, const std::wstring& text, const RECT& rect, UINT format) {
        HFONT font = reinterpret_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
        CompositeLayeredText(source, stride, width, height, rect, text, font, format,
                             labelColor, shadowColor);
    };
    LOGFONTW fontInfo{};
    fontInfo.lfHeight = -ScaleDip(12, dpi);
    fontInfo.lfWeight = FW_NORMAL;
    fontInfo.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(fontInfo.lfFaceName, L"Microsoft YaHei UI");
    HFONT labelFont = CreateFontIndirectW(&fontInfo);
    HGDIOBJ previousFont = SelectObject(layer, labelFont);

    const int padding = ScaleDip(kPaddingDip, dpi);
    const int titleHeight = state.collapsed ? 0 : (state.showTitle ? ScaleDip(kTitleDip, dpi) : padding);
    if (state.collapsed) {
        const int previewPixels = ScaleDip(24, dpi);
        const int previewGap = ScaleDip(8, dpi);
        const int previewWidth = previewPixels * 2 + previewGap;
        const int previewLeft = (width - previewWidth) / 2;
        const int previewTop = ScaleDip(10, dpi);
        if (state.items.empty()) {
            if (HICON icon = IconForPath(L"C:\\Windows", ScaleDip(44, dpi)))
                DrawIconEx(layer, (width - ScaleDip(44, dpi)) / 2, previewTop,
                           icon, ScaleDip(44, dpi), ScaleDip(44, dpi), 0, nullptr, DI_NORMAL);
        } else {
            for (size_t index = 0; index < std::min<size_t>(4, state.items.size()); ++index) {
                const int column = static_cast<int>(index % 2);
                const int row = static_cast<int>(index / 2);
                if (HICON icon = IconForPath(state.items[index].path, previewPixels))
                    DrawIconEx(layer, previewLeft + column * (previewPixels + previewGap),
                               previewTop + row * (previewPixels + previewGap), icon,
                               previewPixels, previewPixels, 0, nullptr, DI_NORMAL);
            }
        }
        RECT nameRect{ScaleDip(5, dpi), height - ScaleDip(30, dpi),
                      width - ScaleDip(5, dpi), height - ScaleDip(5, dpi)};
        std::wstring name = Ellipsize(layer, state.name, nameRect.right - nameRect.left);
        drawLabelText(layer, name, nameRect,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else if (state.showTitle) {
        fontInfo.lfHeight = -ScaleDip(13, dpi);
        fontInfo.lfWeight = FW_NORMAL;
        HFONT titleFont = CreateFontIndirectW(&fontInfo);
        SelectObject(layer, titleFont);
        const auto gridText = [](int whole, bool half) {
            std::wstring text = std::to_wstring(whole);
            if (half) text += L".5";
            return text;
        };
        const std::wstring title = sizingPreview
            ? gridText(state.columns, state.halfColumn) + L"列 × " +
                  gridText(state.rows, state.halfRow) + L"行"
            : state.name;
        RECT titleRect{padding, 0, width - padding, titleHeight};
        drawLabelText(layer, title, titleRect,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(layer, labelFont);
        DeleteObject(titleFont);
    }

    std::vector<layout::Placement> placements;
    if (!state.collapsed && layout::ArrangeItemsInHalfUnits(
            state.columns * 2 + (state.halfColumn ? 1 : 0),
            state.rows * 2 + (state.halfRow ? 1 : 0),
            state.items, &placements)) {
        const int unit = ScaleDip(96, dpi);
        const int halfUnit = std::max(1, unit / 2);
        for (size_t index = 0; index < state.items.size() && index < placements.size(); ++index) {
            const auto& item = state.items[index];
            const auto& placement = placements[index];
            RECT area{padding + placement.x * halfUnit, titleHeight + placement.y * halfUnit,
                      padding + (placement.x + placement.span) * halfUnit,
                      titleHeight + (placement.y + placement.span) * halfUnit};
            if (animatedItem && *animatedItem == index) OffsetRect(&area, itemOffset.x, itemOffset.y);
            const int areaWidth = area.right - area.left;
            const int areaHeight = area.bottom - area.top;
            const int textHeight = item.iconSize == IconSize::Small ? 0 : ScaleDip(20, dpi);
            const int textGap = textHeight == 0 ? 0 : ScaleDip(4, dpi);
            int iconPixels = item.iconSize == IconSize::Small ? ScaleDip(24, dpi)
                : (item.iconSize == IconSize::Large ? ScaleDip(96, dpi) : ScaleDip(48, dpi));
            iconPixels = std::min(iconPixels, std::max(16, areaWidth - ScaleDip(12, dpi)));
            const int iconX = area.left + (areaWidth - iconPixels) / 2;
            const int iconY = area.top + std::max(3, (areaHeight - iconPixels - textGap - textHeight) / 2);
            const bool hiddenByDrag = hideAnimatedItem && animatedItem && *animatedItem == index;
            if (!hiddenByDrag) {
                if (HICON icon = IconForPath(item.path, iconPixels))
                    DrawIconEx(layer, iconX, iconY, icon, iconPixels, iconPixels, 0, nullptr, DI_NORMAL);
                if (item.iconSize != IconSize::Small) {
                    RECT textRect{area.left + 3, iconY + iconPixels + textGap,
                                  area.right - 3, iconY + iconPixels + textGap + textHeight};
                    std::wstring name = Ellipsize(layer, item.name, textRect.right - textRect.left);
                    drawLabelText(layer, name, textRect,
                                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
                }
            }
        }
    }

    SelectObject(layer, previousFont);
    DeleteObject(labelFont);

#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    WriteVisualSnapshotIfRequested(source, stride, width, height);
    wchar_t compositedPath[2]{};
    if (GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_COMPOSITED_SNAPSHOT",
                                compositedPath, static_cast<DWORD>(std::size(compositedPath))) > 0) {
        if (!desktopDc_) CaptureDesktopBackdrop();
        std::vector<DWORD> testBackdrop;
        if (CopyBackdropRegion(desktopDc_, desktopBounds_, screenRect, testBackdrop))
            WriteCompositedVisualSnapshotIfRequested(source, stride, width, height, testBackdrop);
    }
#endif

    POINT destination{screenRect.left, screenRect.top};
    POINT origin{};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiFlush();
    const LONGLONG profT3 = profileLog ? profNow() : 0;
    const BOOL updated = UpdateLayeredWindow(target, screen, &destination, &size, layer, &origin, 0, &blend, ULW_ALPHA);
    if (profileLog) {
        const LONGLONG profT4 = profNow();
        fprintf(profileLog, "frame %dx%d filter=1 surface=%.2f loop=%.2f content=%.2f ulw=%.2f total=%.2f\n",
                width, height,
                profMs(profT0, profT1), profMs(profT1, profT2), profMs(profT2, profT3),
                profMs(profT3, profT4), profMs(profT0, profT4));
        fflush(profileLog);
    }
    ReleaseDC(nullptr, screen);
    return updated == TRUE;
}

bool Application::RegisterClasses() {
    WNDCLASSEXW container{};
    container.cbSize = sizeof(container);
    container.style = CS_DBLCLKS;
    container.lpfnWndProc = ContainerWindow::WindowProc;
    container.hInstance = instance_;
    container.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    container.lpszClassName = kContainerClass;
    if (!RegisterClassExW(&container) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW content{};
    content.cbSize = sizeof(content);
    content.style = CS_DBLCLKS;
    content.lpfnWndProc = ContainerWindow::ContentProc;
    content.hInstance = instance_;
    content.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    content.lpszClassName = kContentClass;
    if (!RegisterClassExW(&content) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW tint{};
    tint.cbSize = sizeof(tint);
    tint.style = CS_DBLCLKS;
    tint.lpfnWndProc = ContainerWindow::TintProc;
    tint.hInstance = instance_;
    tint.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tint.lpszClassName = kTintClass;
    if (!RegisterClassExW(&tint) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW drag{};
    drag.cbSize = sizeof(drag);
    drag.lpfnWndProc = ContainerWindow::DragProc;
    drag.hInstance = instance_;
    drag.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    drag.lpszClassName = kDragClass;
    if (!RegisterClassExW(&drag) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW hover{};
    hover.cbSize = sizeof(hover);
    hover.lpfnWndProc = ContainerWindow::HoverProc;
    hover.hInstance = instance_;
    hover.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hover.lpszClassName = kHoverClass;
    if (!RegisterClassExW(&hover) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW slider{};
    slider.cbSize = sizeof(slider);
    slider.lpfnWndProc = GlassSliderProc;
    slider.hInstance = instance_;
    slider.hCursor = LoadCursorW(nullptr, IDC_HAND);
    slider.hbrBackground = CreateSolidBrush(RGB(244, 249, 250));
    slider.lpszClassName = kSliderClass;
    if (!RegisterClassExW(&slider) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW message{};
    message.cbSize = sizeof(message);
    message.lpfnWndProc = MessageProc;
    message.hInstance = instance_;
    message.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    message.lpszClassName = kMessageClass;
    if (!RegisterClassExW(&message) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW console{};
    console.cbSize = sizeof(console);
    console.lpfnWndProc = ConsoleProc;
    console.hInstance = instance_;
    console.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    console.hbrBackground = CreateSolidBrush(RGB(244, 249, 250));
    console.lpszClassName = kConsoleClass;
    if (!RegisterClassExW(&console) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    return true;
}

void Application::RefreshDesktopGridMetrics() {
    if (!MeasureDesktopGrid()) return;
    for (const auto& container : containers_) {
        if (!container->Handle()) continue;
        container->UpdateMetrics();
        RECT rect{};
        GetWindowRect(container->Handle(), &rect);
        rect.right = rect.left + container->OuterWidth(container->HalfColumns());
        rect.bottom = rect.top + container->OuterHeight(container->HalfRows());
        if (container->state_.snapToGrid) container->SnapRectToDesktopGrid(rect);
        SetWindowPos(container->Handle(), nullptr, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        container->UpdateShape();
        container->UpdateStateBounds();
        container->RefreshGlass();
        InvalidateRect(container->Handle(), nullptr, TRUE);
    }
}

HWND Application::DesktopListView() const {
    HWND defView = desktopOwner_
        ? FindWindowExW(desktopOwner_, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!defView) {
        for (HWND worker = nullptr;
             (worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr;) {
            defView = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView) break;
        }
    }
    return defView ? FindWindowExW(defView, nullptr, L"SysListView32", nullptr) : nullptr;
}

void Application::PushDesktopIconsOutOf(const RECT& area, const ContainerWindow* exclude) {
    HWND list = DesktopListView();
    if (!list) return;
    // Auto-arranged desktops reflow on their own; manual moves would be lost.
    if (GetWindowLongPtrW(list, GWL_STYLE) & 0x0100 /*LVS_AUTOARRANGE*/) return;

    const DesktopGrid grid = desktopGrid_;
    if (grid.cellW < 8 || grid.cellH < 8) return;
    POINT listOrigin{};
    ClientToScreen(list, &listOrigin);
    RECT clientRect{};
    GetClientRect(list, &clientRect);
    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (clientW < grid.cellW || clientH < grid.cellH) return;

    // Valid cell index range: the cell's top-left must stay inside the
    // ListView client area.
    const int kMinX = static_cast<int>(std::ceil(
        static_cast<double>(listOrigin.x - grid.originX) / grid.cellW));
    const int kMaxX = static_cast<int>(std::floor(
        static_cast<double>(listOrigin.x + clientW - grid.cellW - grid.originX) / grid.cellW));
    const int kMinY = static_cast<int>(std::ceil(
        static_cast<double>(listOrigin.y - grid.originY) / grid.cellH));
    const int kMaxY = static_cast<int>(std::floor(
        static_cast<double>(listOrigin.y + clientH - grid.cellH - grid.originY) / grid.cellH));
    if (kMaxX < kMinX || kMaxY < kMinY) return;

    const auto cellRange = [&](const RECT& rect, int& x0, int& y0, int& x1, int& y1) {
        x0 = static_cast<int>(std::lround(
            static_cast<double>(rect.left - grid.originX) / grid.cellW));
        y0 = static_cast<int>(std::lround(
            static_cast<double>(rect.top - grid.originY) / grid.cellH));
        // Round up: a 104 px collapsed card spans two 76 px columns, and
        // rounding to one cell would leave a desktop icon hidden underneath.
        x1 = x0 + std::max(1, static_cast<int>(std::ceil(
            static_cast<double>(rect.right - rect.left) / grid.cellW)));
        y1 = y0 + std::max(1, static_cast<int>(std::ceil(
            static_cast<double>(rect.bottom - rect.top) / grid.cellH)));
    };

    // Reserved cells: the dropped group's own footprint plus every other
    // snapped group, so pushed icons never land under another group.
    std::set<std::pair<int, int>> reserved;
    {
        int x0, y0, x1, y1;
        cellRange(area, x0, y0, x1, y1);
        for (int kx = x0; kx < x1; ++kx)
            for (int ky = y0; ky < y1; ++ky) reserved.insert({kx, ky});
        for (const auto& container : containers_) {
            if (container.get() == exclude || !container->Handle() ||
                !IsWindowVisible(container->Handle()))
                continue;
            const ContainerState& other = container->State();
            if (!other.collapsed || !other.snapToGrid) continue;
            RECT rect{};
            GetWindowRect(container->Handle(), &rect);
            cellRange(rect, x0, y0, x1, y1);
            for (int kx = x0; kx < x1; ++kx)
                for (int ky = y0; ky < y1; ++ky) reserved.insert({kx, ky});
        }
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(list, &pid);
    HANDLE process = pid
        ? OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid)
        : nullptr;
    if (!process) return;
    LPVOID remote = VirtualAllocEx(process, nullptr, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return;
    }

    struct IconCell {
        int index;
        int kx;
        int ky;
    };
    std::vector<IconCell> icons;
    std::set<std::pair<int, int>> occupied;
    const int count = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
    SIZE_T copied = 0;
    for (int i = 0; i < count; ++i) {
        SendMessageW(list, 0x1010 /*LVM_GETITEMPOSITION*/, static_cast<WPARAM>(i),
                     reinterpret_cast<LPARAM>(remote));
        POINT pos{};
        if (!ReadProcessMemory(process, remote, &pos, sizeof(pos), &copied) ||
            copied != sizeof(pos))
            continue;
        const int vx = pos.x + listOrigin.x;
        const int vy = pos.y + listOrigin.y;
        const int kx = static_cast<int>(std::lround(
            static_cast<double>(vx - grid.originX) / grid.cellW));
        const int ky = static_cast<int>(std::lround(
            static_cast<double>(vy - grid.originY) / grid.cellH));
        icons.push_back({i, kx, ky});
        occupied.insert({kx, ky});
    }

    const int footX0 = static_cast<int>(std::lround(
        static_cast<double>(area.left - grid.originX) / grid.cellW));
    const int footY0 = static_cast<int>(std::lround(
        static_cast<double>(area.top - grid.originY) / grid.cellH));
    const int footX1 = footX0 + std::max(1, static_cast<int>(std::lround(
        static_cast<double>(area.right - area.left) / grid.cellW)));
    const int footY1 = footY0 + std::max(1, static_cast<int>(std::lround(
        static_cast<double>(area.bottom - area.top) / grid.cellH)));

    for (auto& icon : icons) {
        if (icon.kx < footX0 || icon.kx >= footX1 || icon.ky < footY0 || icon.ky >= footY1)
            continue;
        // Expanding-ring search for the nearest free, unreserved cell.
        const int maxRadius = std::max(kMaxX - kMinX, kMaxY - kMinY) + 1;
        int bestKx = 0;
        int bestKy = 0;
        int bestDist = INT_MAX;
        bool found = false;
        for (int ring = 1; ring <= maxRadius && !found; ++ring) {
            for (int ky = icon.ky - ring; ky <= icon.ky + ring && !found; ++ky) {
                for (int kx = icon.kx - ring; kx <= icon.kx + ring; ++kx) {
                    if (std::max(std::abs(kx - icon.kx), std::abs(ky - icon.ky)) != ring) continue;
                    if (kx < kMinX || kx > kMaxX || ky < kMinY || ky > kMaxY) continue;
                    const std::pair<int, int> cell{kx, ky};
                    if (occupied.count(cell) || reserved.count(cell)) continue;
                    const int dist = std::abs(kx - icon.kx) + std::abs(ky - icon.ky);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestKx = kx;
                        bestKy = ky;
                        found = true;
                    }
                }
            }
        }
        if (!found) continue;
        occupied.erase({icon.kx, icon.ky});
        occupied.insert({bestKx, bestKy});
        const LONG clientX = static_cast<LONG>(grid.originX + bestKx * grid.cellW - listOrigin.x);
        const LONG clientY = static_cast<LONG>(grid.originY + bestKy * grid.cellH - listOrigin.y);
        SendMessageW(list, 0x100F /*LVM_SETITEMPOSITION*/, static_cast<WPARAM>(icon.index),
                     MAKELPARAM(clientX, clientY));
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
}

bool Application::MeasureDesktopGrid() {
    DesktopGrid grid;
    // Registry values are only a fallback: on this machine IconSpacing reads
    // -1128 (~75 px) while the real desktop ListView reports a 76x100 pitch,
    // so the live ListView always wins when it can be reached.
    const POINT fallback = DesktopIconCellDip();
    grid.cellW = std::clamp(static_cast<int>(fallback.x), 32, 512);
    grid.cellH = std::clamp(static_cast<int>(fallback.y), 32, 512);

    HWND list = DesktopListView();
    DWORD pid = 0;
    if (list) GetWindowThreadProcessId(list, &pid);
    HANDLE process = pid
        ? OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid)
        : nullptr;
    if (process) {
        LPVOID remote = VirtualAllocEx(process, nullptr, 4096, MEM_COMMIT, PAGE_READWRITE);
        if (remote) {
            SIZE_T copied = 0;
            // LVM_GETITEMSPACING (wParam=FALSE) returns the small-icon cell
            // pitch, both as a packed DWORD and through the POINT out-param.
            const DWORD packed = static_cast<DWORD>(SendMessageW(
                list, 0x1033 /*LVM_GETITEMSPACING*/, 0, reinterpret_cast<LPARAM>(remote)));
            POINT pitch{};
            if (ReadProcessMemory(process, remote, &pitch, sizeof(pitch), &copied) &&
                copied == sizeof(pitch)) {
                const int w = pitch.x > 0 ? pitch.x
                                          : static_cast<int>(static_cast<short>(LOWORD(packed)));
                const int h = pitch.y > 0 ? pitch.y
                                          : static_cast<int>(static_cast<short>(HIWORD(packed)));
                if (w >= 32 && w <= 512 && h >= 32 && h <= 512) {
                    grid.cellW = w;
                    grid.cellH = h;
                }
            }
            // Grid phase: icon left/top positions share the same residue mod
            // the pitch, which is exactly the grid origin in virtual coords.
            const int count = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
            POINT listOrigin{};
            ClientToScreen(list, &listOrigin);
            const auto positiveMod = [](int value, int pitchValue) {
                const int safePitch = std::max(1, pitchValue);
                const int r = value % safePitch;
                return r < 0 ? r + safePitch : r;
            };
            grid.originX = positiveMod(listOrigin.x, grid.cellW);
            grid.originY = positiveMod(listOrigin.y, grid.cellH);
            if (count > 0) {
                std::map<int, int> phaseX, phaseY;
                int sampled = 0;
                for (int i = 0; i < count && sampled < 64; ++i) {
                    SendMessageW(list, 0x1010 /*LVM_GETITEMPOSITION*/, static_cast<WPARAM>(i),
                                 reinterpret_cast<LPARAM>(remote));
                    POINT pos{};
                    if (!ReadProcessMemory(process, remote, &pos, sizeof(pos), &copied) ||
                        copied != sizeof(pos))
                        continue;
                    const int vx = pos.x + listOrigin.x;
                    const int vy = pos.y + listOrigin.y;
                    const auto mod = [](int value, int pitchValue) {
                        if (pitchValue <= 0) return 0;
                        const int r = value % pitchValue;
                        return r < 0 ? r + pitchValue : r;
                    };
                    ++phaseX[mod(vx, grid.cellW)];
                    ++phaseY[mod(vy, grid.cellH)];
                    ++sampled;
                }
                const auto bestPhase = [](const std::map<int, int>& phases) {
                    int best = 0;
                    int bestCount = -1;
                    for (const auto& [phase, hits] : phases) {
                        if (hits > bestCount) {
                            bestCount = hits;
                            best = phase;
                        }
                    }
                    return best;
                };
                if (sampled >= 1) {
                    grid.originX = bestPhase(phaseX);
                    grid.originY = bestPhase(phaseY);
                }
            }
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        }
        const UINT listDpi = list ? GetDpiForWindow(list) : 0;
        grid.dpi = listDpi >= 96 ? listDpi : 96;
        CloseHandle(process);
    }

    const bool changed = grid.cellW != desktopGrid_.cellW || grid.cellH != desktopGrid_.cellH ||
                         grid.originX != desktopGrid_.originX ||
                         grid.originY != desktopGrid_.originY || grid.dpi != desktopGrid_.dpi;
    desktopGrid_ = grid;
    return changed;
}

bool Application::Initialize(bool createDefaultContainer) {
    diagnostic_log::Write(L"application.start",
                          L"version=development visualTest=" +
                              std::wstring(VisualTestMode() ? L"1" : L"0"));
    // 1 ms timer resolution so the 7 ms drag/animation timers can actually
    // reach ~144 Hz instead of the default 15.6 ms scheduler tick.
    timeBeginPeriod(1);
    if (!RegisterClasses()) return false;
    // A hidden top-level window receives Explorer's registered broadcast
    // messages; HWND_MESSAGE windows deliberately do not receive them.
    // Create it BEFORE the slow startup work (wallpaper decode, desktop grid
    // probing, config loading) so a second instance launched from the
    // desktop context menu can find it and deliver "--new-group" right away
    // instead of seeing "main program still starting".
    messageWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kMessageClass,
                                     L"DesktopOrganizer", WS_POPUP,
                                     0, 0, 0, 0, nullptr, nullptr, instance_, this);
    if (!messageWindow_) return false;
    gMouseHookApplication = this;
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, instance_, 0);
    if (!mouseHook_) {
        gMouseHookApplication = nullptr;
        return false;
    }
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    shellHookMessage_ = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(messageWindow_);
    // Stale inbox requests belong to senders that already gave up; discard
    // them instead of creating surprise groups after a restart.
    {
        std::error_code ignored;
        const std::filesystem::path inbox = NewGroupInboxDirectory();
        for (std::filesystem::directory_iterator it(inbox, ignored), end;
             !ignored && it != end; it.increment(ignored)) {
            if (it->path().extension() == L".req")
                std::filesystem::remove(it->path(), ignored);
        }
    }
    SetTimer(messageWindow_, kNewGroupInboxTimer, 500, nullptr);
    SetTimer(messageWindow_, kDesktopLayerWatchdogTimer, 1000, nullptr);
    desktopOwner_ = FindWindowW(L"Progman", nullptr);
    desktopLayerHost_ = desktop_capture_target::FindDesktopViewHost();
    StartLiveDesktopCapture();
    MeasureDesktopGrid();
    if (!VisualTestMode()) {
        RegisterDesktopContextMenu();
        AddTrayIcon();
    }

    ConfigLoadResult configResult = config_.Load();
    diagnostic_log::Write(L"config.load",
                          L"status=" + std::to_wstring(static_cast<int>(configResult.status)) +
                              L" containers=" + std::to_wstring(configResult.containers.size()));
    if (!configResult.IsUsable() && configResult.status != ConfigLoadStatus::Missing) {
        MessageBoxW(nullptr,
                    L"配置文件无法安全读取，或来自更高版本。程序不会覆盖它，请先检查 config.json 和 config.json.bak。",
                    L"桌面收纳", MB_OK | MB_ICONERROR);
        return false;
    }
    globalStyle_ = configResult.globalStyle;
    if (configResult.status == ConfigLoadStatus::Missing) {
        if (createDefaultContainer) CreateContainer();
    } else {
        bool removedLegacyAnchors = false;
        bool absorbedAny = false;
        const std::filesystem::path desktopRoot = OrganizerDesktopRoot();
        std::set<std::wstring> groupIds;
        for (const auto& state : configResult.containers) groupIds.insert(state.id);
        for (auto& state : configResult.containers) {
            if (!state.legacyDesktopAnchorPath.empty()) {
                const std::filesystem::path legacyPath(state.legacyDesktopAnchorPath);
                if (!desktopRoot.empty() && SamePath(legacyPath.parent_path(), desktopRoot) &&
                    desktop_anchor::RemoveIfOwned(legacyPath, state.id)) {
                    NotifyDesktopShortcutChange(SHCNE_DELETE, legacyPath);
                }
                state.legacyDesktopAnchorPath.clear();
                removedLegacyAnchors = true;
            }
            // References left on the desktop by the pure-reference builds
            // are absorbed on first start: the user expects collected items
            // to live inside the group storage with the desktop icon gone.
            if (!VisualTestMode()) {
                for (auto& item : state.items) {
                    if (IsStoredItem(item.path) || !IsDesktopItem(item.path)) continue;
                    std::filesystem::path copiedSource;
                    if (AbsorbDesktopItem(state.id, item, &copiedSource) && !copiedSource.empty()) {
                        std::error_code removeError;
                        const auto removed = std::filesystem::remove_all(
                            path_io::ExtendedLengthPath(copiedSource), removeError);
                        if (removeError || removed == 0) {
                            // The desktop original could not be removed:
                            // roll the storage copy back and keep the
                            // reference pointing at the original.
                            std::filesystem::remove_all(
                                path_io::ExtendedLengthPath(item.path), removeError);
                            item.path = copiedSource.wstring();
                        } else {
                            NotifyDesktopShortcutChange(SHCNE_DELETE, copiedSource);
                            absorbedAny = true;
                        }
                    }
                }
            }
            auto container = std::make_unique<ContainerWindow>(*this, std::move(state));
            if (!container->Create(instance_)) continue;
            containers_.push_back(std::move(container));
        }
        // A user may have renamed an anchor while the discontinued integration
        // was active. Remove only files whose signed payload belongs to one of
        // the loaded groups; unrelated desktop files are never touched.
        if (!desktopRoot.empty()) {
            std::error_code scanError;
            size_t scanned = 0;
            for (const auto& entry : std::filesystem::directory_iterator(desktopRoot, scanError)) {
                if (scanError || ++scanned > 4096) break;
                const auto groupId = desktop_anchor::ReadGroupId(entry.path());
                if (!groupId || !groupIds.contains(*groupId)) continue;
                if (desktop_anchor::RemoveIfOwned(entry.path(), *groupId)) {
                    NotifyDesktopShortcutChange(SHCNE_DELETE, entry.path());
                    removedLegacyAnchors = true;
                }
            }
        }
        if (removedLegacyAnchors || configResult.freeMoveMigrated || absorbedAny) Save();
    }
    // Re-snap grid-embedded groups on every start so layout rule changes
    // (like the non-overlapping collapsed pitch) apply to existing groups
    // without a manual nudge.
    RefreshDesktopGridMetrics();
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    // Keep production startup unchanged while allowing automated visual and
    // latency probes to target the otherwise tray-only console window.
    wchar_t openConsole[2]{};
    if (GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_OPEN_CONSOLE", openConsole,
                                static_cast<DWORD>(std::size(openConsole))) > 0) {
        OpenConsole();
    }
#endif
    return true;
}

int Application::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool handledBySettings = false;
        for (const auto& container : containers_) {
            if (container->settingsWindow_ && IsWindowVisible(container->settingsWindow_) &&
                IsDialogMessageW(container->settingsWindow_, &message)) {
                handledBySettings = true;
                break;
            }
        }
        if (!handledBySettings && consoleWindow_ && IsWindowVisible(consoleWindow_) &&
            IsDialogMessageW(consoleWindow_, &message)) {
            handledBySettings = true;
        }
        if (handledBySettings) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void Application::SetDesktopFrontContainer(ContainerWindow* container) {
    desktopFrontContainer_ = container;
}

void Application::PrepareCenteredExpansion(ContainerWindow* container) {
    for (const auto& candidate : containers_) {
        if (candidate.get() == container || candidate->state_.collapsed ||
            !candidate->state_.centeredExpansionActive) continue;
        candidate->SetCollapsed(true);
    }
}

void Application::BeginContainerInteraction(ContainerWindow* container) {
    if (!container) return;
    for (const auto& candidate : containers_) {
        if (candidate.get() != container)
            candidate->RestoreFromPointerInteraction();
    }
    SetDesktopFrontContainer(container);
    container->RaiseForPointerInteraction();
}

void Application::DismissContainerInteractionAt(POINT screen) {
    if (ContainerAtPoint(screen)) return;
    DismissContainerInteractions();
}

void Application::DismissContainerInteractions() {
    for (const auto& container : containers_)
        container->RestoreFromPointerInteraction();
}

void Application::SyncDesktopLayers() {
    if (HWND liveHost = desktop_capture_target::FindDesktopViewHost(); liveHost)
        desktopLayerHost_ = liveHost;

    // Recover the front group from below the desktop boundary first. Other
    // groups use it as their sibling anchor, so repairing them before it can
    // preserve the entire stack on the wrong side of SysListView32.
    if (desktopFrontContainer_ && IsWindowVisible(desktopFrontContainer_->Handle()))
        desktopFrontContainer_->SyncDesktopLayer();
    for (const auto& container : containers_) {
        if (container.get() == desktopFrontContainer_ ||
            !IsWindowVisible(container->Handle())) continue;
        container->SyncDesktopLayer();
    }
    // The sibling passes above may change relative group order. Reassert the
    // most recently used group without allowing a below-desktop sibling to
    // become its anchor.
    if (desktopFrontContainer_ && IsWindowVisible(desktopFrontContainer_->Handle()))
        desktopFrontContainer_->SyncDesktopLayer();
}

bool Application::DesktopLayersNeedRepair() {
    if (!containersShown_ || containers_.empty()) return false;
    HWND liveHost = desktop_capture_target::FindDesktopViewHost();
    if (!liveHost) return false;
    desktopLayerHost_ = liveHost;
    for (const auto& container : containers_) {
        if (!IsWindowVisible(container->window_) ||
            !IsWindowVisible(container->tintWindow_) ||
            !PrecedesInTopLevelZOrder(container->window_, liveHost) ||
            !PrecedesInTopLevelZOrder(container->tintWindow_, liveHost)) return true;
    }
    return false;
}

void Application::CreateContainer(std::optional<POINT> screenPosition) {
    ContainerState state;
    state.id = NewId();
    // New groups follow the global default style so console changes reach
    // them immediately; the per-container panel can detach them later.
    state.followGlobalStyle = true;
    state.opacity = globalStyle_.opacity;
    state.blur = globalStyle_.blur;
    state.cornerRadius = globalStyle_.cornerRadius;
    state.tintMode = globalStyle_.tintMode;
    state.tintColor = globalStyle_.tintColor;
    state.textColor = globalStyle_.textColor;
    state.showTitle = globalStyle_.showTitle;
    state.showBorder = globalStyle_.showBorder;
    POINT position = screenPosition.value_or(POINT{120 + static_cast<LONG>(containers_.size() * 28), 120 + static_cast<LONG>(containers_.size() * 28)});
    state.bounds.left = position.x;
    state.bounds.top = position.y;
    auto container = std::make_unique<ContainerWindow>(*this, std::move(state));
    if (container->Create(instance_)) {
        const std::wstring id = container->State().id;
        containers_.push_back(std::move(container));
        const bool saved = Save();
        diagnostic_log::Write(saved ? L"group.create.succeeded" : L"group.create.failed",
                              L"id=" + id + L" x=" + std::to_wstring(position.x) +
                                  L" y=" + std::to_wstring(position.y));
        RefreshConsoleRows();
    }
}

void Application::ScheduleDissolve(const std::wstring& id) {
    PostMessageW(messageWindow_, kDissolveContainer, 0,
                 reinterpret_cast<LPARAM>(new std::wstring(id)));
}

void Application::DissolveContainer(const std::wstring& id) {
    const auto it = std::find_if(containers_.begin(), containers_.end(), [&](const auto& item) { return item->State().id == id; });
    if (it == containers_.end()) return;
    ContainerWindow* container = it->get();
    diagnostic_log::Write(L"group.dissolve.begin",
                          L"group=" + container->State().name + L" id=" + id +
                              L" items=" + std::to_wstring(container->State().items.size()));
    struct PreparedCopy { std::filesystem::path source; std::filesystem::path destination; };
    std::vector<PreparedCopy> copies;
    size_t missingManaged = 0;
    for (const auto& item : container->State().items) {
        if (!IsStoredItem(item.path)) continue;
        const std::filesystem::path source(item.path);
        const PathExistence existence = QueryPathExistence(source);
        if (existence == PathExistence::Unknown) {
            diagnostic_log::Write(L"group.dissolve.failed",
                                  L"id=" + id + L" reason=query-path path=" + source.wstring());
            for (const auto& copy : copies) RemovePreparedDesktopCopy(copy.destination);
            MessageBoxW(container->Handle(), L"无法访问部分托管项目，分组未解散。",
                        L"桌面收纳", MB_OK | MB_ICONWARNING);
            return;
        }
        if (existence == PathExistence::Missing) {
            ++missingManaged;
            continue;
        }
        const auto destination = UniqueDesktopPath(source, DesktopRootForStoredItem(source));
        DWORD copyError = ERROR_SUCCESS;
        if (destination.empty() ||
            !path_io::CopyPathPreservingSource(source, destination, &copyError)) {
            diagnostic_log::Write(L"group.dissolve.failed",
                                  L"id=" + id + L" reason=copy error=" +
                                      std::to_wstring(copyError) + L" source=" + source.wstring() +
                                      L" destination=" + destination.wstring());
            for (const auto& copy : copies) RemovePreparedDesktopCopy(copy.destination);
            MessageBoxW(container->Handle(), L"托管项目无法安全复制到桌面，分组未解散。",
                        L"桌面收纳", MB_OK | MB_ICONWARNING);
            return;
        }
        copies.push_back({source, destination});
    }

    const size_t index = static_cast<size_t>(it - containers_.begin());
    const bool removedWasDesktopFront = desktopFrontContainer_ == container;
    if (removedWasDesktopFront) desktopFrontContainer_ = nullptr;
    std::unique_ptr<ContainerWindow> removed = std::move(*it);
    containers_.erase(it);
    RefreshConsoleRows();
    if (!Save()) {
        diagnostic_log::Write(L"group.dissolve.failed",
                              L"id=" + id + L" reason=config-save");
        containers_.insert(containers_.begin() + static_cast<ptrdiff_t>(index), std::move(removed));
        if (removedWasDesktopFront) desktopFrontContainer_ = containers_[index].get();
        for (const auto& copy : copies) RemovePreparedDesktopCopy(copy.destination);
        RefreshConsoleRows();
        return;
    }

    size_t duplicateCount = 0;
    for (const auto& copy : copies) {
        NotifyDesktopShortcutChange(SHCNE_CREATE, copy.destination);
        std::error_code error;
        const auto removedCount = std::filesystem::remove_all(
            path_io::ExtendedLengthPath(copy.source), error);
        if (error || removedCount == 0) ++duplicateCount;
        else NotifyDesktopShortcutChange(SHCNE_DELETE, copy.source);
    }
    if (missingManaged || duplicateCount) {
        const std::wstring message = std::wstring(L"分组已解散。") +
            (missingManaged ? L"\n其中有失效的托管记录已移除。" : L"") +
            (duplicateCount ? L"\n部分旧托管副本未能清理，但桌面副本已安全保留。" : L"");
        MessageBoxW(nullptr, message.c_str(), L"桌面收纳", MB_OK | MB_ICONINFORMATION);
    }
    diagnostic_log::Write(L"group.dissolve.succeeded",
                          L"id=" + id + L" restored=" + std::to_wstring(copies.size()) +
                              L" missing=" + std::to_wstring(missingManaged) +
                              L" cleanupFailures=" + std::to_wstring(duplicateCount));
}

bool Application::Save() {
    if (exiting_) return true;
    std::vector<ContainerState> states;
    states.reserve(containers_.size());
    for (const auto& container : containers_) states.push_back(container->State());
    const bool wasDirty = saveDirty_;
    if (config_.Save(states, &globalStyle_)) {
        saveDirty_ = false;
        saveFailureNotified_ = false;
        if (messageWindow_) KillTimer(messageWindow_, kSaveRetryTimer);
        if (wasDirty) diagnostic_log::Write(L"config.save.recovered");
        return true;
    }
    const ConfigSaveFailure failure = config_.LastSaveFailure();
    diagnostic_log::Write(L"config.save.failed",
                          L"stage=" + std::to_wstring(static_cast<int>(failure.stage)) +
                              L" error=" + std::to_wstring(failure.error) +
                              L" replaceError=" + std::to_wstring(failure.replaceError) +
                              L" containers=" + std::to_wstring(states.size()));
    saveDirty_ = true;
    if (messageWindow_) SetTimer(messageWindow_, kSaveRetryTimer, 2000, nullptr);
    NotifySaveFailure();
    return false;
}

void Application::NotifySaveFailure() {
    if (saveFailureNotified_ || VisualTestMode()) return;
    saveFailureNotified_ = true;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = messageWindow_;
    data.uID = kTrayId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    wcscpy_s(data.szInfoTitle, L"桌面收纳未能保存");
    wcscpy_s(data.szInfo, L"配置写入失败，程序会自动重试。退出前必须保存成功。");
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Application::InvalidateAll() {
    for (const auto& container : containers_) container->RefreshGlass();
}

ContainerWindow* Application::FromWindow(HWND window) const {
    while (window) {
        const auto* className = kContainerClass;
        wchar_t actual[128]{};
        GetClassNameW(window, actual, static_cast<int>(std::size(actual)));
        if (wcscmp(actual, className) == 0) return reinterpret_cast<ContainerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        window = GetParent(window);
    }
    return nullptr;
}

ContainerWindow* Application::ContainerAtPoint(POINT screen, const ContainerWindow* preferred) const {
    auto contains = [&](const ContainerWindow* container) {
        if (!container || !IsWindowVisible(container->Handle())) return false;
        POINT local = screen;
        ScreenToClient(container->Handle(), &local);
        return container->HitVisibleSurface(local);
    };
    if (contains(preferred)) return const_cast<ContainerWindow*>(preferred);
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it) {
        if (contains(it->get())) return it->get();
    }
    return nullptr;
}

HICON Application::IconForPath(const std::wstring& path, int pixels) {
    constexpr int kShellLarge = 0;
    constexpr int kShellExtraLarge = 2;
    constexpr int kShellJumbo = 4;
    const int imageListSize = pixels <= 32 ? kShellLarge : (pixels <= 64 ? kShellExtraLarge : kShellJumbo);
    const std::wstring cacheKey = path + L"\n" + std::to_wstring(imageListSize);
    const auto cached = icons_.find(cacheKey);
    if (cached != icons_.end()) return cached->second;
    const std::wstring iconSource = IconSourceForPath(path);

    SHFILEINFOW info{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(iconSource.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX)) {
        using SHGetImageListFn = HRESULT(WINAPI*)(int, REFIID, void**);
        static const auto getImageList = reinterpret_cast<SHGetImageListFn>(
            GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetImageList"));
        IImageList* imageList = nullptr;
        if (getImageList && SUCCEEDED(getImageList(imageListSize, IID_IImageList,
                                                   reinterpret_cast<void**>(&imageList)))) {
            imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
            imageList->Release();
        }
    }
    if (!icon) {
        const DWORD_PTR result = SHGetFileInfoW(iconSource.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON);
        icon = result ? info.hIcon : CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    }
    if (HICON normalized = NormalizeIconPixels(icon, pixels)) {
        DestroyIcon(icon);
        icon = normalized;
    }
    icons_.emplace(cacheKey, icon);
    return icon;
}

void Application::MoveItem(ContainerWindow& source, size_t index, ContainerWindow& target, POINT targetPoint) {
    if (index >= source.State().items.size() || target.State().locked) return;
    const std::wstring movedPath = source.State().items[index].path;
    diagnostic_log::Write(L"item.move.begin",
                          L"from=" + source.State().name + L" to=" + target.State().name +
                              L" path=" + movedPath);
    const ContainerState sourceBefore = source.State();
    const std::optional<ContainerState> targetBefore = &source == &target
        ? std::nullopt : std::optional<ContainerState>(target.State());
    size_t animatedIndex = index;
    if (&source == &target) {
        if (!source.PlaceItemAt(index, targetPoint)) return;
        source.RefreshAfterExternalChange();
    } else {
        OrganizerItem item = source.State().items[index];
        item.gridX = -1;
        item.gridY = -1;
        target.State().items.push_back(std::move(item));
        const size_t destination = target.State().items.size() - 1;
        if (!target.RefreshAfterExternalChange()) {
            target.State().items.pop_back();
            target.RefreshAfterExternalChange();
            MessageBoxW(target.Handle(), L"目标容器没有足够的屏幕空间。", L"桌面收纳", MB_OK | MB_ICONINFORMATION);
            return;
        }
        target.PlaceItemAt(destination, targetPoint);
        target.RefreshAfterExternalChange();
        animatedIndex = destination;
        source.State().items.erase(source.State().items.begin() + static_cast<ptrdiff_t>(index));
        source.RefreshAfterExternalChange();
    }
    if (!Save()) {
        source.State() = sourceBefore;
        if (targetBefore) target.State() = *targetBefore;
        source.RefreshAfterExternalChange();
        if (targetBefore) target.RefreshAfterExternalChange();
        diagnostic_log::Write(L"item.move.failed", L"path=" + movedPath + L" reason=config-save");
        return;
    }
    if (&source == &target) source.StartItemAnimation(animatedIndex, targetPoint);
    else target.StartItemAnimation(animatedIndex, targetPoint);
    diagnostic_log::Write(L"item.move.succeeded",
                          L"from=" + source.State().name + L" to=" + target.State().name +
                              L" path=" + movedPath);
}

bool Application::AbsorbDesktopItem(const std::wstring& groupId, OrganizerItem& item,
                                    std::filesystem::path* copiedSource) const {
    if (copiedSource) copiedSource->clear();
    // Every desktop item is absorbed into the group storage: the desktop
    // icon disappears for good (independent of Explorer visibility settings)
    // because the original leaves the desktop directory. Dropping the item
    // out again restores it to the desktop. Items dropped from outside the
    // desktop stay plain references and are never touched.
    const std::filesystem::path source(item.path);
    if (!IsDesktopItem(source)) return true;
    const DWORD attributes = GetFileAttributesW(source.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return true;

    const auto storage = DesktopStorageRoot(groupId, source.parent_path());
    std::error_code error;
    std::filesystem::create_directories(storage, error);
    if (error) return false;
    SetFileAttributesW(storage.parent_path().parent_path().c_str(),
                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    const auto destination = UniquePathInDirectory(storage, source);
    if (destination.empty()) return false;
    if (!path_io::CopyPathPreservingSource(source, destination))
        return false;
    item.path = destination.wstring();
    if (copiedSource) *copiedSource = source;
    return true;
}

void Application::ScheduleDesktopItemPlacement(const std::filesystem::path& path, POINT screen) {    if (!messageWindow_ || path.empty()) return;
    const auto duplicate = std::find_if(
        pendingDesktopPlacements_.begin(), pendingDesktopPlacements_.end(),
        [&](const DesktopPlacementRequest& request) { return SamePath(request.path, path); });
    if (duplicate != pendingDesktopPlacements_.end()) {
        duplicate->screen = screen;
        duplicate->attempts = 0;
    } else {
        pendingDesktopPlacements_.push_back(DesktopPlacementRequest{path, screen, 0});
    }
    SetTimer(messageWindow_, kDesktopPlacementTimer, 16, nullptr);
}

void Application::ProcessDesktopItemPlacements() {
    if (pendingDesktopPlacements_.empty()) {
        KillTimer(messageWindow_, kDesktopPlacementTimer);
        return;
    }

    for (auto it = pendingDesktopPlacements_.begin(); it != pendingDesktopPlacements_.end();) {
        if (TryPositionDesktopItem(it->path, it->screen) || ++it->attempts >= 30) {
            it = pendingDesktopPlacements_.erase(it);
        } else {
            ++it;
        }
    }
    if (pendingDesktopPlacements_.empty()) KillTimer(messageWindow_, kDesktopPlacementTimer);
}

void Application::ProcessNewGroupInbox() {
    // Honor requests dropped by a lower-integrity second instance that could
    // not reach us through WM_COPYDATA. Each file is deleted after handling;
    // the sender treats the disappearance as its receipt. Duplicate cookies
    // are dropped so a retried request never creates two groups.
    const std::filesystem::path inbox = NewGroupInboxDirectory();
    if (inbox.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(inbox, error);
    for (std::filesystem::directory_iterator it(inbox, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || it->path().extension() != L".req") continue;
        int x = 0;
        int y = 0;
        unsigned long cookie = 0;
        {
            std::ifstream stream(it->path(), std::ios::binary);
            std::string content;
            content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            if (content == "open-console") {
                diagnostic_log::Write(L"console.open.request", L"source=desktop-menu-inbox");
                OpenConsole();
            } else if (sscanf_s(content.c_str(), "%d,%d,%lu", &x, &y, &cookie) == 3) {
                if (cookie == 0 || cookie != lastNewGroupCookie_) {
                    lastNewGroupCookie_ = cookie;
                    CreateContainer(POINT{x, y});
                }
            }
        } // The stream must close before the file can be deleted below.
        std::error_code removeError;
        std::filesystem::remove(it->path(), removeError);
    }
}

bool Application::MoveItemToDesktop(ContainerWindow& source, size_t index,
                                    std::optional<POINT> screenPosition) {
    if (index >= source.State().items.size()) return false;
    const ContainerState before = source.State();
    const OrganizerItem item = source.State().items[index];
    diagnostic_log::Write(L"item.restore.begin",
                          L"group=" + source.State().name + L" path=" + item.path);
    const bool managed = IsStoredItem(item.path);
    const std::filesystem::path managedSource(item.path);
    std::filesystem::path desktopCopy;
    const PathExistence managedExistence = managed
        ? QueryPathExistence(managedSource) : PathExistence::Missing;
    if (managedExistence == PathExistence::Unknown) {
        diagnostic_log::Write(L"item.restore.failed",
                              L"path=" + item.path + L" reason=query-path");
        MessageBoxW(source.Handle(), L"无法访问托管项目，操作已取消。",
                    L"桌面收纳", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (managed && managedExistence == PathExistence::Present) {
        desktopCopy = UniqueDesktopPath(managedSource, DesktopRootForStoredItem(managedSource));
        DWORD copyError = ERROR_SUCCESS;
        if (desktopCopy.empty() ||
            !path_io::CopyPathPreservingSource(managedSource, desktopCopy, &copyError)) {
            diagnostic_log::Write(L"item.restore.failed",
                                  L"path=" + item.path + L" reason=copy error=" +
                                      std::to_wstring(copyError));
            MessageBoxW(source.Handle(), L"项目无法安全复制到桌面，仍保留在分组中。",
                        L"桌面收纳", MB_OK | MB_ICONWARNING);
            return false;
        }
    } else if (managed) {
        if (MessageBoxW(source.Handle(),
                        L"托管原件已不存在。是否只移除这条失效记录？",
                        L"桌面收纳", MB_YESNO | MB_ICONWARNING) != IDYES) return false;
    }
    source.State().items.erase(source.State().items.begin() + static_cast<ptrdiff_t>(index));
    source.RefreshAfterExternalChange();
    if (!Save()) {
        source.State() = before;
        source.RefreshAfterExternalChange();
        if (!desktopCopy.empty()) RemovePreparedDesktopCopy(desktopCopy);
        diagnostic_log::Write(L"item.restore.failed",
                              L"path=" + item.path + L" reason=config-save");
        return false;
    }
    if (!desktopCopy.empty()) {
        NotifyDesktopShortcutChange(SHCNE_CREATE, desktopCopy);
        std::error_code error;
        const auto removedCount = std::filesystem::remove_all(
            path_io::ExtendedLengthPath(managedSource), error);
        if (!error && removedCount > 0) NotifyDesktopShortcutChange(SHCNE_DELETE, managedSource);
        if (screenPosition) ScheduleDesktopItemPlacement(desktopCopy, *screenPosition);
        if (error || removedCount == 0) {
            MessageBoxW(source.Handle(),
                        L"项目已恢复到桌面，但旧托管副本暂未能清理。数据不会丢失。",
                        L"桌面收纳", MB_OK | MB_ICONINFORMATION);
        }
    }
    diagnostic_log::Write(L"item.restore.succeeded",
                          L"group=" + source.State().name + L" path=" + item.path +
                              L" desktop=" + desktopCopy.wstring());
    return true;
}

void Application::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = messageWindow_;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    HICON ownedIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_DESKTOP_ORGANIZER),
                                                    IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    data.hIcon = ownedIcon ? ownedIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"桌面收纳");
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    if (ownedIcon) DestroyIcon(ownedIcon);
}

void Application::RemoveTrayIcon() {
    if (!messageWindow_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = messageWindow_;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void Application::ShowTrayMenu(POINT screen) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TrayNew, L"新建容器");
    AppendMenuW(menu, MF_STRING, TrayConsole, L"总控制台");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayShowAll, L"显示全部");
    AppendMenuW(menu, MF_STRING, TrayHideAll, L"隐藏全部");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), TrayStartup, L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, TrayExit, L"退出");
    SetForegroundWindow(messageWindow_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, messageWindow_, nullptr);
    DestroyMenu(menu);
    if (command == TrayNew) CreateContainer(screen);
    else if (command == TrayConsole) OpenConsole();
    else if (command == TrayShowAll) {
        containersShown_ = true;
        for (const auto& item : containers_) item->Show(true);
    }
    else if (command == TrayHideAll) {
        containersShown_ = false;
        for (const auto& item : containers_) item->Show(false);
    }
    else if (command == TrayStartup) SetStartupEnabled(!IsStartupEnabled());
    else if (command == TrayExit) {
        if (!Save()) {
            MessageBoxW(messageWindow_,
                        L"配置仍未能保存，已取消退出。\n\n请检查磁盘空间和配置目录权限后再试。",
                        L"桌面收纳", MB_OK | MB_ICONWARNING);
            return;
        }
        exiting_ = true;
        RemoveTrayIcon();
        PostQuitMessage(0);
    }
}

bool Application::IsStartupEnabled() const {
    wchar_t value[2048]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"DesktopOrganizer", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

void Application::SetStartupEnabled(bool enabled) const {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        const std::wstring command = L"\"" + std::wstring(executable) + L"\"";
        RegSetValueExW(key, L"DesktopOrganizer", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"DesktopOrganizer");
    }
    RegCloseKey(key);
}

LRESULT CALLBACK Application::MessageProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Application* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Application*>(create->lpCreateParams);
        self->messageWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Application::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kDismissInteractionMessage) {
        POINT screen{
            static_cast<LONG>(static_cast<INT_PTR>(wParam)),
            static_cast<LONG>(static_cast<INT_PTR>(lParam))};
        DismissContainerInteractionAt(screen);
        return 0;
    }
    if (shellHookMessage_ && message == shellHookMessage_) {
        const int shellEvent = static_cast<int>(wParam);
        if (shellEvent == HSHELL_WINDOWACTIVATED ||
            shellEvent == HSHELL_RUDEAPPACTIVATED) {
            HWND activated = reinterpret_cast<HWND>(lParam);
            bool pointerActionActive = false;
            for (const auto& container : containers_) {
                if (container->pointerActive_) {
                    pointerActionActive = true;
                    break;
                }
            }
            if (!pointerActionActive && !IsOrganizerDesktopSurface(activated))
                DismissContainerInteractions();
        }
        // Win+D and the taskbar's Show Desktop command rearrange ordinary
        // top-level windows. Repair the organizer surfaces immediately after
        // every shell activation/minimize transition instead of waiting for a
        // later repaint to make them reappear.
        if (DesktopLayersNeedRepair())
            diagnostic_log::Write(L"desktop.layer.repair",
                                  L"reason=shell-event event=" + std::to_wstring(shellEvent));
        SyncDesktopLayers();
        // Show Desktop performs more than one Z-order pass. Repair once now
        // for responsiveness and once after the shell animation settles.
        SetTimer(messageWindow_, kDesktopLayerRepairTimer, 80, nullptr);
        return 0;
    }
    if (taskbarCreatedMessage_ && message == taskbarCreatedMessage_) {
        desktopOwner_ = desktop_capture_target::Find();
        desktopLayerHost_ = desktop_capture_target::FindDesktopViewHost();
        StartLiveDesktopCapture();
        if (!VisualTestMode()) AddTrayIcon();
        ReleaseDesktopBackdrop();
        RefreshDesktopGridMetrics();
        if (containersShown_)
            for (const auto& container : containers_) container->Show(true);
        SyncDesktopLayers();
        return 0;
    }
    if (message == WM_DISPLAYCHANGE) {
        desktopOwner_ = desktop_capture_target::Find();
        desktopLayerHost_ = desktop_capture_target::FindDesktopViewHost();
        StartLiveDesktopCapture();
        ReleaseDesktopBackdrop();
        RefreshDesktopGridMetrics();
        InvalidateAll();
        return 0;
    }
    if (message == kLiveBackdropMessage) {
        if (ApplyLatestLiveDesktopFrame()) InvalidateAll();
        return 0;
    }
    if (message == WM_THEMECHANGED || message == WM_DWMCOLORIZATIONCOLORCHANGED ||
        message == WM_SYSCOLORCHANGE || message == WM_SETTINGCHANGE) {
        ReleaseDesktopBackdrop();
        if (message == WM_SETTINGCHANGE) RefreshDesktopGridMetrics();
        InvalidateAll();
        return 0;
    }
    if (message == WM_TIMER && wParam == kDesktopPlacementTimer) {
        ProcessDesktopItemPlacements();
        return 0;
    }
    if (message == WM_TIMER && wParam == kDesktopLayerRepairTimer) {
        KillTimer(messageWindow_, kDesktopLayerRepairTimer);
        SyncDesktopLayers();
        return 0;
    }
    if (message == WM_TIMER && wParam == kDesktopLayerWatchdogTimer) {
        if (DesktopLayersNeedRepair()) {
            diagnostic_log::Write(L"desktop.layer.repair", L"reason=watchdog");
            for (const auto& container : containers_) container->Show(true);
            SyncDesktopLayers();
        }
        return 0;
    }
    if (message == WM_TIMER && wParam == kNewGroupInboxTimer) {
        ProcessNewGroupInbox();
        return 0;
    }
    if (message == WM_TIMER && wParam == kSaveRetryTimer) {
        if (saveDirty_) Save();
        else KillTimer(messageWindow_, kSaveRetryTimer);
        return 0;
    }
    if (message == WM_COPYDATA) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (data && data->dwData == kCopyDataOpenConsole) {
            diagnostic_log::Write(L"console.open.request", L"source=desktop-menu-message");
            OpenConsole();
            return TRUE;
        }
        if (data && data->dwData == kCopyDataNewGroup && data->lpData) {
            POINT position{};
            DWORD cookie = 0;
            bool valid = false;
            if (data->cbData == sizeof(NewGroupRequest)) {
                const auto& request = *static_cast<const NewGroupRequest*>(data->lpData);
                position = request.position;
                cookie = request.cookie;
                valid = true;
            } else if (data->cbData == sizeof(POINT)) {
                // Older sender payload without a dedupe cookie.
                position = *static_cast<const POINT*>(data->lpData);
                valid = true;
            }
            if (valid && (cookie == 0 || cookie != lastNewGroupCookie_)) {
                lastNewGroupCookie_ = cookie;
                CreateContainer(position);
            }
            return TRUE;
        }
        return FALSE;
    }
    if (message == kTrayMessage) {
        if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            containersShown_ = true;
            for (const auto& item : containers_) item->Show(true);
        }
        return 0;
    }
    if (message == kDissolveContainer) {
        std::unique_ptr<std::wstring> id(reinterpret_cast<std::wstring*>(lParam));
        DissolveContainer(*id);
        return 0;
    }
    return DefWindowProcW(messageWindow_, message, wParam, lParam);
}

namespace {

struct RenameContext {
    std::wstring value;
    bool accepted = false;
    HWND edit = nullptr;
    HFONT titleFont = nullptr;
    HFONT controlFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH editBrush = nullptr;
    UINT dpi = 96;
    int opacity = 32;
    int cornerRadius = 22;
    COLORREF tint = RGB(247, 252, 253);
    bool dark = false;
};

constexpr int kRenameOk = 101;
constexpr int kRenameCancel = IDCANCEL;

HFONT CreateUiFont(int pixels, int weight) {
    LOGFONTW info{};
    info.lfHeight = -pixels;
    info.lfWeight = weight;
    info.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(info.lfFaceName, L"Microsoft YaHei UI");
    return CreateFontIndirectW(&info);
}

void DrawRenameButton(const DRAWITEMSTRUCT& item, const RenameContext& context) {
    const bool primary = item.CtlID == kRenameOk;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF background = primary
        ? (context.dark ? RGB(41, 93, 104) : RGB(207, 239, 244))
        : (context.dark ? RGB(42, 54, 59) : RGB(238, 246, 247));
    if (pressed) background = primary
        ? (context.dark ? RGB(35, 78, 88) : RGB(169, 221, 231))
        : (context.dark ? RGB(34, 45, 49) : RGB(216, 233, 236));
    HBRUSH brush = CreateSolidBrush(background);
    const COLORREF border = primary
        ? (context.dark ? RGB(65, 118, 128) : RGB(165, 217, 227))
        : (context.dark ? RGB(57, 73, 79) : RGB(214, 231, 234));
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    const int buttonRadius = ScaleDip(9, context.dpi);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom,
              buttonRadius * 2, buttonRadius * 2);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(145, 158, 162)
        : (primary ? (context.dark ? RGB(234, 249, 252) : RGB(23, 103, 123))
                   : (context.dark ? RGB(213, 228, 231) : RGB(75, 98, 104))));
    HGDIOBJ oldFont = SelectObject(item.hDC, context.controlFont);
    RECT text = item.rcItem;
    DrawTextW(item.hDC, primary ? L"保存" : L"取消", -1, &text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(item.hDC, &focus);
    }
}

void PaintRenamePanel(HWND window, RenameContext& context, HDC dc) {
    RECT rect{};
    GetClientRect(window, &rect);
    const COLORREF base = context.tint;
    HBRUSH brush = CreateSolidBrush(base);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, context.dark ? RGB(57, 80, 87) : RGB(212, 231, 235));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int radius = std::clamp(context.cornerRadius, 8, 28);
    RoundRect(dc, 0, 0, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, context.dark ? RGB(240, 248, 249) : RGB(32, 54, 60));
    HGDIOBJ oldFont = SelectObject(dc, context.titleFont);
    RECT title{ScaleDip(24, context.dpi), ScaleDip(16, context.dpi),
               rect.right - ScaleDip(24, context.dpi), ScaleDip(48, context.dpi)};
    DrawTextW(dc, L"重命名", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, oldFont);

    RECT editFrame{ScaleDip(23, context.dpi), ScaleDip(53, context.dpi),
                   ScaleDip(337, context.dpi), ScaleDip(91, context.dpi)};
    HBRUSH editBackground = CreateSolidBrush(context.dark ? RGB(24, 33, 38) : RGB(255, 255, 255));
    HPEN editBorder = CreatePen(PS_SOLID, 1, context.dark ? RGB(66, 92, 100) : RGB(201, 224, 229));
    oldBrush = SelectObject(dc, editBackground);
    oldPen = SelectObject(dc, editBorder);
    RoundRect(dc, editFrame.left, editFrame.top, editFrame.right, editFrame.bottom,
              ScaleDip(12, context.dpi), ScaleDip(12, context.dpi));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(editBorder);
    DeleteObject(editBackground);
}

LRESULT CALLBACK RenameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    RenameContext* context = reinterpret_cast<RenameContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<RenameContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    if (!context) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintRenamePanel(window, *context, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CREATE:
        context->titleFont = CreateUiFont(ScaleDip(15, context->dpi), FW_SEMIBOLD);
        context->controlFont = CreateUiFont(ScaleDip(14, context->dpi), FW_NORMAL);
        context->backgroundBrush = CreateSolidBrush(context->tint);
        context->editBrush = CreateSolidBrush(context->dark ? RGB(24, 33, 38) : RGB(255, 255, 255));
        context->edit = CreateWindowExW(0, L"EDIT", context->value.c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        ScaleDip(32, context->dpi), ScaleDip(60, context->dpi),
                                        ScaleDip(296, context->dpi), ScaleDip(26, context->dpi),
                                        window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        if (!context->edit) return -1;
        SendMessageW(context->edit, WM_SETFONT, reinterpret_cast<WPARAM>(context->controlFont), TRUE);
        CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                      ScaleDip(248, context->dpi), ScaleDip(104, context->dpi),
                      ScaleDip(88, context->dpi), ScaleDip(34, context->dpi),
                      window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenameOk)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      ScaleDip(152, context->dpi), ScaleDip(104, context->dpi),
                      ScaleDip(88, context->dpi), ScaleDip(34, context->dpi),
                      window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenameCancel)), nullptr, nullptr);
        SendMessageW(context->edit, EM_SETSEL, 0, -1);
        SetFocus(context->edit);
        return 0;
    case WM_DRAWITEM:
        if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            item && (item->CtlID == kRenameOk || item->CtlID == kRenameCancel)) {
            DrawRenameButton(*item, *context);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == kRenameOk) {
            const int length = GetWindowTextLengthW(context->edit);
            std::wstring value(length + 1, L'\0');
            GetWindowTextW(context->edit, value.data(), length + 1);
            value.resize(length);
            if (!value.empty()) { context->value = std::move(value); context->accepted = true; DestroyWindow(window); }
            return 0;
        }
        if (LOWORD(wParam) == kRenameCancel) { DestroyWindow(window); return 0; }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        if (wParam == VK_RETURN) { SendMessageW(window, WM_COMMAND, kRenameOk, 0); return 0; }
        break;
    case WM_CTLCOLOREDIT:
        SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
        SetBkColor(reinterpret_cast<HDC>(wParam), context->dark ? RGB(24, 33, 38) : RGB(255, 255, 255));
        SetTextColor(reinterpret_cast<HDC>(wParam), context->dark ? RGB(234, 244, 246) : RGB(37, 58, 64));
        return reinterpret_cast<LRESULT>(context->editBrush ? context->editBrush : GetStockObject(WHITE_BRUSH));
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), context->dark ? RGB(240, 248, 249) : RGB(32, 54, 60));
        return reinterpret_cast<LRESULT>(context->backgroundBrush ? context->backgroundBrush : GetStockObject(NULL_BRUSH));
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (context->titleFont) DeleteObject(context->titleFont);
        if (context->controlFont) DeleteObject(context->controlFont);
        if (context->backgroundBrush) DeleteObject(context->backgroundBrush);
        if (context->editBrush) DeleteObject(context->editBrush);
        context->titleFont = nullptr;
        context->controlFont = nullptr;
        context->backgroundBrush = nullptr;
        context->editBrush = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool PromptForName(HWND owner, std::wstring& value, const ContainerState* style) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = RenameProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = kRenameClass;
        registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return false;
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    RenameContext context{value};
    if (style) {
        context.opacity = style->opacity;
        context.cornerRadius = style->cornerRadius;
        context.dark = UsesDarkGlass(*style);
        context.tint = style->tintMode == 3 ? TintColor(*style)
            : (context.dark ? RGB(25, 38, 43) : RGB(247, 252, 253));
    }
    const UINT dpi = WindowDpi(owner);
    context.dpi = dpi;
    const int width = ScaleDip(360, dpi);
    const int height = ScaleDip(158, dpi);
    RECT work = WorkAreaForRect(ownerRect);
    int left = std::clamp(ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2,
                          work.left, work.right - width);
    int top = std::clamp(ownerRect.top + ScaleDip(28, dpi), work.top, work.bottom - height);
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
                                  kRenameClass, L"重命名容器",
                                  WS_POPUP,
                                  left, top, width, height,
                                  owner, nullptr, GetModuleHandleW(nullptr), &context);
    if (!dialog) return false;
    const int radius = std::clamp(ScaleDip(context.cornerRadius, dpi), ScaleDip(8, dpi),
                                  std::min(width, height) / 2);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
    if (!SetWindowRgn(dialog, region, TRUE)) DeleteObject(region);
    constexpr DWORD kCornerPreference = 33;
    constexpr DWORD kRound = 2;
    DwmSetWindowAttribute(dialog, kCornerPreference, &kRound, sizeof(kRound));
    EnableWindow(owner, FALSE);
    SetWindowPos(dialog, HWND_TOP, left, top, width, height,
                 SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    SetForegroundWindow(dialog);
    SetActiveWindow(dialog);
    SetFocus(context.edit);
    MSG message{};
    bool receivedQuit = false;
    int quitCode = 0;
    while (IsWindow(dialog)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                receivedQuit = true;
                quitCode = static_cast<int>(message.wParam);
            }
            if (IsWindow(dialog)) DestroyWindow(dialog);
            break;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    if (receivedQuit) PostQuitMessage(quitCode);
    if (context.accepted) value = std::move(context.value);
    return context.accepted;
}

} // namespace

// ---- Master console: global default style + per-group follow toggles ----

void Application::OpenConsole() {
    diagnostic_log::Write(L"console.open");
    if (consoleWindow_ && IsWindow(consoleWindow_)) {
        ShowWindow(consoleWindow_, SW_RESTORE);
        SetForegroundWindow(consoleWindow_);
        return;
    }
    const UINT dpi = WindowDpi(messageWindow_);
    const size_t rows = std::min(containers_.size(), static_cast<size_t>(ConsoleMaxRows));
    const int width = ScaleDip(400, dpi);
    // Client rows start below the interaction options; the window frame adds
    // a caption on top.
    const int height = ScaleDip(536 + static_cast<int>(rows) * 26, dpi);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int left = work.left + static_cast<int>(std::max(0L, (work.right - work.left - width) / 2));
    const int top = work.top + static_cast<int>(std::max(0L, (work.bottom - work.top - height) / 4));
    // A normal app window in the visual-test build is discoverable by the
    // UI automation harness; production keeps the compact tool-window style.
    const DWORD consoleExStyle = VisualTestMode() ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
    CreateWindowExW(consoleExStyle, kConsoleClass, L"总控制台",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                    left, top, width, height, nullptr, nullptr, instance_, this);
    if (consoleWindow_) {
        constexpr DWORD kBackdropType = 38;
        constexpr int kMica = 2;
        constexpr DWORD kCornerPreference = 33;
        constexpr int kRound = 2;
        DwmSetWindowAttribute(consoleWindow_, kBackdropType, &kMica, sizeof(kMica));
        DwmSetWindowAttribute(consoleWindow_, kCornerPreference, &kRound, sizeof(kRound));
        ShowWindow(consoleWindow_, SW_SHOW);
        SetForegroundWindow(consoleWindow_);
    }
}

void Application::CreateConsoleControls(HWND window) {
    consoleWindow_ = window;
    const UINT dpi = WindowDpi(window);
    if (consoleFont_) DeleteObject(consoleFont_);
    consoleFont_ = CreateFontW(-ScaleDip(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
    const auto createLabel = [&](const wchar_t* text, int y, int width = 220) {
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, ScaleDip(24, dpi), ScaleDip(y, dpi),
                             ScaleDip(width, dpi), ScaleDip(22, dpi), window, nullptr, nullptr, nullptr);
    };
    createLabel(L"全局默认样式", 16);

    createLabel(L"通透度", 48);
    consoleOpacityValue_ = createLabel(L"", 48, 330);
    SetWindowPos(consoleOpacityValue_, nullptr, ScaleDip(315, dpi), ScaleDip(48, dpi), ScaleDip(54, dpi), ScaleDip(22, dpi), SWP_NOZORDER);
    consoleOpacitySlider_ = CreateWindowW(kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                          ScaleDip(20, dpi), ScaleDip(72, dpi), ScaleDip(350, dpi), ScaleDip(28, dpi),
                                          window, reinterpret_cast<HMENU>(ConsoleOpacity), nullptr, nullptr);
    SendMessageW(consoleOpacitySlider_, kSliderSetRange, 0, MAKELONG(4, 85));
    SendMessageW(consoleOpacitySlider_, kSliderSetPosition, 100 - globalStyle_.opacity, 0);

    createLabel(L"背景模糊（实时磨砂）", 110);
    consoleBlurValue_ = createLabel(L"—", 110, 330);
    SetWindowPos(consoleBlurValue_, nullptr, ScaleDip(315, dpi), ScaleDip(110, dpi), ScaleDip(54, dpi), ScaleDip(22, dpi), SWP_NOZORDER);
    consoleBlurSlider_ = CreateWindowW(kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       ScaleDip(20, dpi), ScaleDip(134, dpi), ScaleDip(350, dpi), ScaleDip(28, dpi),
                                       window, reinterpret_cast<HMENU>(ConsoleBlur), nullptr, nullptr);
    SendMessageW(consoleBlurSlider_, kSliderSetRange, 0, MAKELONG(0, 100));
    SendMessageW(consoleBlurSlider_, kSliderSetPosition, globalStyle_.blur, 0);
    EnableWindow(consoleBlurSlider_, TRUE);

    createLabel(L"圆角", 172);
    consoleCornerValue_ = createLabel(L"", 172, 330);
    SetWindowPos(consoleCornerValue_, nullptr, ScaleDip(315, dpi), ScaleDip(172, dpi), ScaleDip(54, dpi), ScaleDip(22, dpi), SWP_NOZORDER);
    consoleCornerSlider_ = CreateWindowW(kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         ScaleDip(20, dpi), ScaleDip(196, dpi), ScaleDip(350, dpi), ScaleDip(28, dpi),
                                         window, reinterpret_cast<HMENU>(ConsoleCorner), nullptr, nullptr);
    SendMessageW(consoleCornerSlider_, kSliderSetRange, 0, MAKELONG(0, 48));
    SendMessageW(consoleCornerSlider_, kSliderSetPosition, globalStyle_.cornerRadius, 0);

    createLabel(L"玻璃色调", 234);
    HWND tintAuto = CreateWindowW(L"BUTTON", L"自动", WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON,
                                  ScaleDip(24, dpi), ScaleDip(258, dpi), ScaleDip(76, dpi), ScaleDip(26, dpi),
                                  window, reinterpret_cast<HMENU>(ConsoleTintAuto), nullptr, nullptr);
    HWND tintDark = CreateWindowW(L"BUTTON", L"夜色", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                  ScaleDip(112, dpi), ScaleDip(258, dpi), ScaleDip(76, dpi), ScaleDip(26, dpi),
                                  window, reinterpret_cast<HMENU>(ConsoleTintDark), nullptr, nullptr);
    HWND tintLight = CreateWindowW(L"BUTTON", L"冰蓝", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   ScaleDip(200, dpi), ScaleDip(258, dpi), ScaleDip(76, dpi), ScaleDip(26, dpi),
                                   window, reinterpret_cast<HMENU>(ConsoleTintLight), nullptr, nullptr);
    HWND tintCustom = CreateWindowW(L"BUTTON", L"自定义", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                    ScaleDip(286, dpi), ScaleDip(258, dpi), ScaleDip(76, dpi), ScaleDip(26, dpi),
                                    window, reinterpret_cast<HMENU>(ConsoleTintCustom), nullptr, nullptr);
    SendMessageW(tintAuto, BM_SETCHECK, globalStyle_.tintMode == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tintLight, BM_SETCHECK, globalStyle_.tintMode == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tintDark, BM_SETCHECK, globalStyle_.tintMode == 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tintCustom, BM_SETCHECK, globalStyle_.tintMode == 3 ? BST_CHECKED : BST_UNCHECKED, 0);

    consoleTintColorButton_ = CreateWindowW(L"BUTTON", L"选择背景颜色", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                            ScaleDip(24, dpi), ScaleDip(294, dpi), ScaleDip(140, dpi), ScaleDip(30, dpi),
                                            window, reinterpret_cast<HMENU>(ConsoleTintColor), nullptr, nullptr);
    consoleTextColorButton_ = CreateWindowW(L"BUTTON", L"选择字体颜色", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                            ScaleDip(178, dpi), ScaleDip(294, dpi), ScaleDip(184, dpi), ScaleDip(30, dpi),
                                            window, reinterpret_cast<HMENU>(ConsoleTextColor), nullptr, nullptr);

    HWND title = CreateWindowW(L"BUTTON", L"显示标题", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               ScaleDip(24, dpi), ScaleDip(338, dpi), ScaleDip(100, dpi), ScaleDip(26, dpi),
                               window, reinterpret_cast<HMENU>(ConsoleTitle), nullptr, nullptr);
    HWND border = CreateWindowW(L"BUTTON", L"显示细边框", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                ScaleDip(138, dpi), ScaleDip(338, dpi), ScaleDip(112, dpi), ScaleDip(26, dpi),
                                window, reinterpret_cast<HMENU>(ConsoleBorder), nullptr, nullptr);
    SendMessageW(title, BM_SETCHECK, globalStyle_.showTitle ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(border, BM_SETCHECK, globalStyle_.showBorder ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND grid = CreateWindowW(L"BUTTON", L"嵌入桌面网格（对齐桌面图标）",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              ScaleDip(24, dpi), ScaleDip(366, dpi), ScaleDip(280, dpi), ScaleDip(26, dpi),
                              window, reinterpret_cast<HMENU>(ConsoleGrid), nullptr, nullptr);
    bool allSnapToGrid = !containers_.empty();
    for (const auto& container : containers_) {
        if (!container->State().snapToGrid) {
            allSnapToGrid = false;
            break;
        }
    }
    SendMessageW(grid, BM_SETCHECK, allSnapToGrid ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND centerExpanded = CreateWindowW(
        L"BUTTON", L"双击展开时居中，打开项目后自动收回",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        ScaleDip(24, dpi), ScaleDip(394, dpi), ScaleDip(338, dpi), ScaleDip(26, dpi),
        window, reinterpret_cast<HMENU>(ConsoleCenterExpanded), nullptr, nullptr);
    SendMessageW(centerExpanded, BM_SETCHECK,
                 globalStyle_.centerExpandedGroups ? BST_CHECKED : BST_UNCHECKED, 0);

    CreateWindowW(L"BUTTON", L"应用到全部分组", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                  ScaleDip(24, dpi), ScaleDip(422, dpi), ScaleDip(160, dpi), ScaleDip(30, dpi),
                  window, reinterpret_cast<HMENU>(ConsoleApplyAll), nullptr, nullptr);

    createLabel(L"分组样式（勾选 = 跟随全局样式）", 468, 340);

    EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        SetWindowTheme(child, L"Explorer", nullptr);
        return TRUE;
    }, reinterpret_cast<LPARAM>(consoleFont_));
    UpdateConsoleLabels();
    RefreshConsoleRows();
}

void Application::SetAllSnapToGrid(bool enabled) {
    // Console-wide toggle for "embed in desktop grid": mirrors the per-group
    // context menu switch for every group at once and snaps their positions
    // when enabled, so the change is visible immediately.
    for (const auto& container : containers_) {
        ContainerState& state = container->State();
        if (state.snapToGrid == enabled || !container->Handle()) continue;
        state.snapToGrid = enabled;
        container->UpdateMetrics();
        RECT snapped{};
        GetWindowRect(container->Handle(), &snapped);
        snapped.right = snapped.left + container->OuterWidth(container->HalfColumns());
        snapped.bottom = snapped.top + container->OuterHeight(container->HalfRows());
        if (enabled) container->SnapRectToDesktopGrid(snapped);
        SetWindowPos(container->Handle(), nullptr, snapped.left, snapped.top,
                     snapped.right - snapped.left, snapped.bottom - snapped.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (state.collapsed && enabled && state.pushIcons)
            PushDesktopIconsOutOf(snapped, container.get());
        container->UpdateShape();
        container->UpdateGlass();
        InvalidateRect(container->Handle(), nullptr, TRUE);
        container->UpdateStateBounds();
    }
    Save();
    RefreshConsoleRows();
}

void Application::RefreshConsoleRows() {    if (!consoleWindow_) return;
    for (int index = 0; index < ConsoleMaxRows; ++index) {
        if (HWND name = GetDlgItem(consoleWindow_, ConsoleNameBase + index)) DestroyWindow(name);
        if (HWND follow = GetDlgItem(consoleWindow_, ConsoleFollowBase + index)) DestroyWindow(follow);
    }
    const UINT dpi = WindowDpi(consoleWindow_);
    const size_t rows = std::min(containers_.size(), static_cast<size_t>(ConsoleMaxRows));
    for (size_t index = 0; index < rows; ++index) {
        const ContainerState& state = containers_[index]->State();
        HWND name = CreateWindowW(L"STATIC", state.name.c_str(), WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                  ScaleDip(24, dpi), ScaleDip(496 + static_cast<int>(index) * 26, dpi),
                                  ScaleDip(224, dpi), ScaleDip(20, dpi),
                                  consoleWindow_, reinterpret_cast<HMENU>(ConsoleNameBase + index), nullptr, nullptr);
        HWND follow = CreateWindowW(L"BUTTON", L"跟随全局", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    ScaleDip(262, dpi), ScaleDip(494 + static_cast<int>(index) * 26, dpi),
                                    ScaleDip(100, dpi), ScaleDip(22, dpi),
                                    consoleWindow_, reinterpret_cast<HMENU>(ConsoleFollowBase + index), nullptr, nullptr);
        SendMessageW(follow, BM_SETCHECK, state.followGlobalStyle ? BST_CHECKED : BST_UNCHECKED, 0);
        if (consoleFont_) {
            SendMessageW(name, WM_SETFONT, reinterpret_cast<WPARAM>(consoleFont_), TRUE);
            SendMessageW(follow, WM_SETFONT, reinterpret_cast<WPARAM>(consoleFont_), TRUE);
        }
        SetWindowTheme(follow, L"Explorer", nullptr);
    }
    RECT rect{};
    GetWindowRect(consoleWindow_, &rect);
    const int height = ScaleDip(536 + static_cast<int>(rows) * 26, dpi);
    if (rect.bottom - rect.top != height) {
        SetWindowPos(consoleWindow_, nullptr, 0, 0, rect.right - rect.left, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Application::UpdateConsoleLabels() {
    if (!consoleWindow_) return;
    const std::wstring opacity = std::to_wstring(SendMessageW(consoleOpacitySlider_, kSliderGetPosition, 0, 0)) + L"%";
    const std::wstring corner = std::to_wstring(SendMessageW(consoleCornerSlider_, kSliderGetPosition, 0, 0)) + L" px";
    SetWindowTextW(consoleOpacityValue_, opacity.c_str());
    SetWindowTextW(consoleBlurValue_, (std::to_wstring(SendMessageW(consoleBlurSlider_, kSliderGetPosition, 0, 0)) + L"%").c_str());
    SetWindowTextW(consoleCornerValue_, corner.c_str());
}

void Application::ApplyGlobalStyleToFollowers() {
    for (const auto& container : containers_) {
        if (container->State().followGlobalStyle) container->ApplyGlobalStyle(globalStyle_);
    }
}

void Application::ApplyConsoleFromControls(bool persist) {
    if (!consoleWindow_) return;
    globalStyle_.opacity = 100 - static_cast<int>(SendMessageW(consoleOpacitySlider_, kSliderGetPosition, 0, 0));
    globalStyle_.blur = static_cast<int>(SendMessageW(consoleBlurSlider_, kSliderGetPosition, 0, 0));
    globalStyle_.cornerRadius = static_cast<int>(SendMessageW(consoleCornerSlider_, kSliderGetPosition, 0, 0));
    if (SendDlgItemMessageW(consoleWindow_, ConsoleTintAuto, BM_GETCHECK, 0, 0) == BST_CHECKED) globalStyle_.tintMode = 0;
    else if (SendDlgItemMessageW(consoleWindow_, ConsoleTintLight, BM_GETCHECK, 0, 0) == BST_CHECKED) globalStyle_.tintMode = 1;
    else if (SendDlgItemMessageW(consoleWindow_, ConsoleTintDark, BM_GETCHECK, 0, 0) == BST_CHECKED) globalStyle_.tintMode = 2;
    else globalStyle_.tintMode = 3;
    globalStyle_.showTitle = SendDlgItemMessageW(consoleWindow_, ConsoleTitle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    globalStyle_.showBorder = SendDlgItemMessageW(consoleWindow_, ConsoleBorder, BM_GETCHECK, 0, 0) == BST_CHECKED;
    globalStyle_.centerExpandedGroups =
        SendDlgItemMessageW(consoleWindow_, ConsoleCenterExpanded, BM_GETCHECK, 0, 0) == BST_CHECKED;
    UpdateConsoleLabels();
    ApplyGlobalStyleToFollowers();
    if (persist) {
        diagnostic_log::Write(L"global.style.changed",
                              L"opacity=" + std::to_wstring(globalStyle_.opacity) +
                                  L" blur=" + std::to_wstring(globalStyle_.blur) +
                                  L" corner=" + std::to_wstring(globalStyle_.cornerRadius) +
                                  L" tintMode=" + std::to_wstring(globalStyle_.tintMode) +
                                  L" centered=" +
                                  (globalStyle_.centerExpandedGroups ? L"1" : L"0"));
        Save();
    }
}

void Application::ChooseConsoleTintColor() {
    static COLORREF customColors[16]{
        RGB(246, 249, 255), RGB(238, 247, 255), RGB(246, 240, 255), RGB(255, 242, 248),
        RGB(238, 252, 247), RGB(255, 249, 232), RGB(232, 240, 255), RGB(244, 244, 244)};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = consoleWindow_;
    chooser.rgbResult = RGB((globalStyle_.tintColor >> 16) & 0xff,
                            (globalStyle_.tintColor >> 8) & 0xff, globalStyle_.tintColor & 0xff);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&chooser)) return;
    globalStyle_.tintColor = (GetRValue(chooser.rgbResult) << 16) |
                             (GetGValue(chooser.rgbResult) << 8) | GetBValue(chooser.rgbResult);
    globalStyle_.tintMode = 3;
    SendDlgItemMessageW(consoleWindow_, ConsoleTintAuto, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(consoleWindow_, ConsoleTintDark, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(consoleWindow_, ConsoleTintLight, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(consoleWindow_, ConsoleTintCustom, BM_SETCHECK, BST_CHECKED, 0);
    InvalidateRect(consoleTintColorButton_, nullptr, TRUE);
    ApplyGlobalStyleToFollowers();
    Save();
}

void Application::ChooseConsoleTextColor() {
    static COLORREF customColors[16]{
        RGB(245, 248, 250), RGB(255, 255, 255), RGB(35, 49, 54), RGB(0, 0, 0),
        RGB(73, 112, 255), RGB(96, 210, 190), RGB(255, 188, 92), RGB(244, 116, 143)};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = consoleWindow_;
    chooser.rgbResult = RGB((globalStyle_.textColor >> 16) & 0xff,
                            (globalStyle_.textColor >> 8) & 0xff,
                            globalStyle_.textColor & 0xff);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&chooser)) return;
    globalStyle_.textColor = (GetRValue(chooser.rgbResult) << 16) |
                             (GetGValue(chooser.rgbResult) << 8) | GetBValue(chooser.rgbResult);
    InvalidateRect(consoleTextColorButton_, nullptr, TRUE);
    ApplyGlobalStyleToFollowers();
    Save();
}

LRESULT CALLBACK Application::ConsoleProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Application* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE: self->CreateConsoleControls(window); return 0;
    case WM_HSCROLL:
        self->UpdateConsoleLabels();
        {
            const bool timerAlreadyRunning = self->consoleUpdatePending_;
            self->consoleUpdatePending_ = true;
            self->consolePersistPending_ = self->consolePersistPending_ || LOWORD(wParam) == SB_ENDSCROLL;
            if (!timerAlreadyRunning) SetTimer(window, 1, kStylePreviewFrameMs, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wParam == 1 && self->consoleUpdatePending_) {
            KillTimer(window, 1);
            const bool persist = self->consolePersistPending_;
            self->consoleUpdatePending_ = false;
            self->consolePersistPending_ = false;
            self->ApplyConsoleFromControls(persist);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            const UINT id = LOWORD(wParam);
            if (id == ConsoleTintColor) {
                self->ChooseConsoleTintColor();
            } else if (id == ConsoleTextColor) {
                self->ChooseConsoleTextColor();
            } else if (id == ConsoleApplyAll) {
                for (const auto& container : self->containers_) container->State().followGlobalStyle = true;
                self->ApplyGlobalStyleToFollowers();
                self->RefreshConsoleRows();
                self->Save();
            } else if (id == ConsoleGrid) {
                const bool enabled = SendDlgItemMessageW(window, ConsoleGrid, BM_GETCHECK, 0, 0) == BST_CHECKED;
                self->SetAllSnapToGrid(enabled);
            } else if (id >= ConsoleFollowBase && id < ConsoleFollowBase + ConsoleMaxRows) {
                const size_t index = static_cast<size_t>(id - ConsoleFollowBase);
                if (index < self->containers_.size()) {
                    ContainerWindow* container = self->containers_[index].get();
                    const bool follow = SendMessageW(reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    container->State().followGlobalStyle = follow;
                    if (follow) container->ApplyGlobalStyle(self->globalStyle_);
                    self->Save();
                }
            } else {
                self->ApplyConsoleFromControls(true);
            }
            return 0;
        }
        break;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw && (draw->CtlID == ConsoleTintColor || draw->CtlID == ConsoleTextColor)) {
            const UINT dpi = WindowDpi(window);
            RECT rect = draw->rcItem;
            HBRUSH background = CreateSolidBrush(RGB(244, 249, 250));
            FillRect(draw->hDC, &rect, background);
            DeleteObject(background);
            RECT swatch{rect.left + ScaleDip(8, dpi), rect.top + ScaleDip(6, dpi),
                        rect.left + ScaleDip(30, dpi), rect.bottom - ScaleDip(6, dpi)};
            const int storedColor = draw->CtlID == ConsoleTintColor
                ? self->globalStyle_.tintColor : self->globalStyle_.textColor;
            HBRUSH color = CreateSolidBrush(RGB((storedColor >> 16) & 0xff,
                                                (storedColor >> 8) & 0xff,
                                                storedColor & 0xff));
            FillRect(draw->hDC, &swatch, color);
            DeleteObject(color);
            HBRUSH swatchBorder = CreateSolidBrush(RGB(183, 210, 215));
            FrameRect(draw->hDC, &swatch, swatchBorder);
            DeleteObject(swatchBorder);
            RECT text{swatch.right + ScaleDip(8, dpi), rect.top, rect.right - ScaleDip(5, dpi), rect.bottom};
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, RGB(38, 59, 65));
            const wchar_t* label = draw->CtlID == ConsoleTintColor
                ? L"背景颜色" : L"字体颜色";
            DrawTextW(draw->hDC, label, -1, &text,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
            HBRUSH outline = CreateSolidBrush(RGB(214, 231, 234));
            FrameRect(draw->hDC, &rect, outline);
            DeleteObject(outline);
            if (draw->itemState & ODS_FOCUS) {
                InflateRect(&rect, -ScaleDip(3, dpi), -ScaleDip(3, dpi));
                DrawFocusRect(draw->hDC, &rect);
            }
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), OPAQUE);
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(244, 249, 250));
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(38, 59, 65));
        SetDCBrushColor(reinterpret_cast<HDC>(wParam), RGB(244, 249, 250));
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    case WM_CLOSE:
        KillTimer(window, 1);
        if (self->consoleUpdatePending_) self->ApplyConsoleFromControls(true);
        self->consoleUpdatePending_ = false;
        self->consolePersistPending_ = false;
        DestroyWindow(window);
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(window, 1);
        self->consoleUpdatePending_ = false;
        self->consolePersistPending_ = false;
        self->consoleWindow_ = nullptr;
        self->consoleOpacitySlider_ = nullptr;
        self->consoleBlurSlider_ = nullptr;
        self->consoleCornerSlider_ = nullptr;
        self->consoleOpacityValue_ = nullptr;
        self->consoleBlurValue_ = nullptr;
        self->consoleCornerValue_ = nullptr;
        self->consoleTintColorButton_ = nullptr;
        if (self->consoleFont_) {
            DeleteObject(self->consoleFont_);
            self->consoleFont_ = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// Resolve DPI APIs dynamically so the exe still starts on Windows versions
// that lack SetProcessDpiAwarenessContext (added in Windows 10 1703).
void EnableDpiAwareness() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto setContext = reinterpret_cast<SetContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            if (setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
    }
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        using SetAwarenessFn = HRESULT(WINAPI*)(int);
        if (auto setAwareness = reinterpret_cast<SetAwarenessFn>(
                GetProcAddress(shcore, "SetProcessDpiAwareness"))) {
            constexpr int kProcessPerMonitorDpiAware = 2;
            if (SUCCEEDED(setAwareness(kProcessPerMonitorDpiAware))) return;
        }
    }
    SetProcessDPIAware();
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const bool createFromDesktopMenu = HasNewGroupArgument();
    const bool openConsoleFromDesktopMenu = HasOpenConsoleArgument();
    POINT requestedPosition{};
    if (createFromDesktopMenu) GetCursorPos(&requestedPosition);
    const wchar_t* mutexName = VisualTestMode()
        ? L"Local\\DesktopOrganizer.SingleInstance.visual-test"
        : L"Local\\DesktopOrganizer.SingleInstance.v1";
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, mutexName);
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        bool delivered = true;
        if (createFromDesktopMenu) delivered = NotifyExistingInstanceToCreateGroup(requestedPosition);
        else if (openConsoleFromDesktopMenu) delivered = NotifyExistingInstanceToOpenConsole();
        if (!delivered) {
            MessageBoxW(nullptr, L"主程序正在启动，请稍后再试一次。",
                        L"桌面收纳", MB_OK | MB_ICONINFORMATION);
        }
        if (singleInstance) CloseHandle(singleInstance);
        return 0;
    }
    EnableDpiAwareness();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        CloseHandle(singleInstance);
        return 1;
    }
    Application app(instance);
    if (!app.Initialize(!createFromDesktopMenu)) {
        MessageBoxW(nullptr, L"程序初始化失败。", L"桌面收纳", MB_OK | MB_ICONERROR);
        CoUninitialize();
        CloseHandle(singleInstance);
        return 1;
    }
    if (createFromDesktopMenu) app.CreateContainer(requestedPosition);
    if (openConsoleFromDesktopMenu) app.OpenConsole();
    const int result = app.Run();
    CoUninitialize();
    CloseHandle(singleInstance);
    return result;
}
