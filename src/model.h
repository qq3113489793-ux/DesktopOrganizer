#pragma once

#include <windows.h>

#include <string>
#include <vector>

enum class IconSize { Small = 0, Normal = 1, Large = 2 };

// Global default style, edited from the master console. Containers with
// followGlobalStyle=true receive these values whenever the console changes
// them; other containers keep their own per-container style fields.
struct GlobalStyle {
    int opacity = 32;
    int blur = 58;
    int cornerRadius = 22;
    int tintMode = 1; // 0: auto, 1: light, 2: smoke, 3: custom
    int tintColor = 0xF3FAFC; // stored as RGB, independent from COLORREF's BGR layout
    int textColor = 0xF5F8FA; // title and item label color, stored as RGB
    bool showTitle = true;
    bool showBorder = true;
    // Global interaction option from the master console. A collapsed group
    // opened by double-click is centered on its current monitor and remembers
    // its compact home position until it collapses again.
    bool centerExpandedGroups = false;
};

struct OrganizerItem {
    std::wstring path;
    std::wstring name;
    IconSize iconSize = IconSize::Normal;
    int gridX = -1;
    int gridY = -1;
};

struct ContainerState {
    std::wstring id;
    std::wstring name = L"新建分组";
    RECT bounds{120, 120, 120, 120};
    int columns = 2;
    int rows = 2;
    // Half-step extras: the resize unit is 0.5 grid, so a container can be
    // exactly 1.5 x 1.5 cells to hold a 3 x 3 arrangement of small (0.5)
    // icons. The full half-cell extent is columns * 2 + (halfColumn ? 1 : 0).
    bool halfColumn = false;
    bool halfRow = false;
    IconSize iconSize = IconSize::Normal;
    int opacity = 32;
    int blur = 58;
    int cornerRadius = 22;
    int tintMode = 1; // 0: auto, 1: light, 2: smoke, 3: custom
    int tintColor = 0xF3FAFC; // stored as RGB, independent from COLORREF's BGR layout
    int textColor = 0xF5F8FA; // title and item label color, stored as RGB
    bool showTitle = true;
    bool showBorder = true;
    bool locked = false;
    bool collapsed = false;
    bool centeredExpansionActive = false;
    POINT collapsedHome{};
    bool followGlobalStyle = false;
    // Free movement by default: containers drag freely and snap to screen
    // edges and sibling containers. "Embed in desktop grid" is an explicit
    // per-group opt-in (right-click menu).
    bool snapToGrid = false;
    bool pushIcons = true;
    // Read-only migration field for the short-lived visible anchor format.
    // Current versions never persist or create this file.
    std::wstring legacyDesktopAnchorPath;
    std::wstring monitor;
    std::vector<OrganizerItem> items;
};
