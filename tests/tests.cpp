#include "config.h"
#include "desktop_anchor.h"
#include "desktop_grid_geometry.h"
#include "desktop_item_policy.h"
#include "icon_image.h"
#include "interaction_motion.h"
#include "layout.h"
#include "path_io.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#define CHECK(expression)                                                               \
    do {                                                                                \
        if (!(expression)) {                                                            \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "    \
                      << #expression << '\n';                                           \
            return 1;                                                                   \
        }                                                                               \
    } while (false)

int main() {
    ConfigStore defaultStore;
    std::wcout << L"Default config path: " << defaultStore.Path().wstring() << L'\n';

    using desktop_item_policy::Classification;
    const std::filesystem::path desktop = L"C:\\Users\\Tester\\Desktop";
    CHECK(desktop_item_policy::Classify(desktop / L"Application.lnk", desktop,
                                        FILE_ATTRIBUTE_ARCHIVE) == Classification::AbsorbShortcut);
    CHECK(desktop_item_policy::Classify(desktop / L"Website.URL", desktop,
                                        FILE_ATTRIBUTE_NORMAL) == Classification::AbsorbShortcut);
    CHECK(desktop_item_policy::Classify(L"c:\\users\\tester\\desktop\\MixedCase.LNK", desktop,
                                        FILE_ATTRIBUTE_ARCHIVE) == Classification::AbsorbShortcut);
    CHECK(desktop_item_policy::Classify(desktop / L"Document.txt", desktop,
                                        FILE_ATTRIBUTE_ARCHIVE) == Classification::ReferenceOnly);
    CHECK(desktop_item_policy::Classify(desktop / L"Folder.lnk", desktop,
                                        FILE_ATTRIBUTE_DIRECTORY) == Classification::ReferenceOnly);
    CHECK(desktop_item_policy::Classify(desktop / L"Linked.lnk", desktop,
                                        FILE_ATTRIBUTE_REPARSE_POINT) == Classification::ReferenceOnly);
    CHECK(desktop_item_policy::Classify(desktop / L"Missing.lnk", desktop,
                                        INVALID_FILE_ATTRIBUTES) == Classification::ReferenceOnly);
    CHECK(desktop_item_policy::Classify(desktop / L"Nested" / L"Application.lnk", desktop,
                                        FILE_ATTRIBUTE_ARCHIVE) == Classification::ReferenceOnly);
    CHECK(desktop_item_policy::Classify(L"C:\\Users\\Public\\Desktop\\Application.lnk", desktop,
                                        FILE_ATTRIBUTE_ARCHIVE) == Classification::ReferenceOnly);

    CHECK(layout::Capacity(2, 2, 96, IconSize::Small) == 16);
    CHECK(layout::Capacity(2, 2, 96, IconSize::Normal) == 4);
    CHECK(layout::Capacity(2, 2, 96, IconSize::Large) == 1);
    CHECK(layout::SnapCount(80, 96) == 1);
    CHECK(layout::SnapCount(239, 96) == 2);
    CHECK(layout::SnapCount(240, 96) == 3);
    CHECK(layout::EaseOutCubic(0.0) == 0.0);
    CHECK(layout::EaseOutCubic(0.5) > 0.8);
    CHECK(layout::EaseOutCubic(1.0) == 1.0);

    // A pointer may move several physical pixels without crossing the next
    // integer setting. The thumb must still follow those pixels continuously.
    const double sliderRatioA = interaction_motion::TrackRatio(100, 10, 330);
    const double sliderRatioB = interaction_motion::TrackRatio(102, 10, 330);
    CHECK(interaction_motion::LogicalSliderValue(sliderRatioA, 0, 48) ==
          interaction_motion::LogicalSliderValue(sliderRatioB, 0, 48));
    CHECK(interaction_motion::VisualSliderX(sliderRatioA, 10, 330) !=
          interaction_motion::VisualSliderX(sliderRatioB, 10, 330));

    const interaction_motion::Rect original{10, 20, 110, 120};
    const interaction_motion::Rect expanded{10, 20, 206, 216};
    CHECK(interaction_motion::ShouldRetarget(false, original, original));
    CHECK(!interaction_motion::ShouldRetarget(true, expanded, expanded));
    CHECK(interaction_motion::ShouldRetarget(true, original, expanded));
    const auto animationStart = interaction_motion::AnimateRect(original, expanded, 0.0, 160.0);
    const auto animationMiddle = interaction_motion::AnimateRect(original, expanded, 80.0, 160.0);
    const auto animationEnd = interaction_motion::AnimateRect(original, expanded, 160.0, 160.0);
    CHECK(animationStart == original);
    CHECK(animationMiddle.right > original.right && animationMiddle.right < expanded.right);
    CHECK(animationEnd == expanded);

    // Explorer's 76x100 item slot must not become a 76x100 glass rectangle.
    // The visible tile is square, while the label remains in the slot below it.
    CHECK(desktop_grid_geometry::VisualUnit(76, 100) == 76);
    const auto collapsed = desktop_grid_geometry::MakeCollapsedLayout(76, 100, 96);
    CHECK(collapsed.tile.right - collapsed.tile.left == 68);
    CHECK(collapsed.tile.bottom - collapsed.tile.top == 68);
    CHECK(collapsed.label.top >= collapsed.tile.bottom);
    CHECK(collapsed.label.bottom <= 100);
    CHECK(desktop_grid_geometry::SnapCoordinate(114, 0, 76) == 152);
    CHECK(desktop_grid_geometry::SnapIndexWithHysteresis(42, 0, 76, 0) == 0);
    CHECK(desktop_grid_geometry::SnapIndexWithHysteresis(45, 0, 76, 0) == 1);

    int columns = 2;
    int rows = 2;
    CHECK(layout::ExpandToFit(columns, rows, 96, IconSize::Large, 4, 8, 8));
    CHECK(columns == 8 && rows == 2);
    CHECK(!layout::ExpandToFit(columns, rows, 96, IconSize::Large, 100, 4, 4));

    std::vector<IconSize> mixed{IconSize::Large, IconSize::Normal, IconSize::Small, IconSize::Small};
    std::vector<layout::Placement> placements;
    CHECK(!layout::PackItems(2, 2, mixed, &placements));
    CHECK(layout::PackItems(4, 2, mixed, &placements));
    CHECK(placements.size() == mixed.size());

    // Half-unit packing: a 1.5 x 1.5 container is 3 x 3 half cells and fits
    // exactly nine small (0.5-cell) icons, but not in a 2 x 2 half-cell box.
    std::vector<OrganizerItem> nineSmall(9, {L"p", L"P", IconSize::Small});
    CHECK(layout::ArrangeItemsInHalfUnits(3, 3, nineSmall, &placements));
    CHECK(placements.size() == 9);
    CHECK(!layout::ArrangeItemsInHalfUnits(2, 2, nineSmall, &placements));
    std::vector<OrganizerItem> twoSmall(2, {L"p", L"P", IconSize::Small});
    twoSmall[0].gridX = 0;
    twoSmall[0].gridY = 0;
    CHECK(layout::ArrangeItemsInHalfUnits(2, 2, twoSmall, &placements));
    layout::Placement halfNearest;
    CHECK(layout::FindNearestFreeInHalfUnits(2, 2, placements, 0, 1, 0, 1, halfNearest));
    CHECK(halfNearest.x == 0 && halfNearest.y == 1);

    // Coordinates from half-size icons can overlap after changing every item
    // to normal size. A uniform resize must clear them before repacking.
    std::vector<OrganizerItem> staleSmallCoordinates{
        {L"a", L"A", IconSize::Normal, 0, 0},
        {L"b", L"B", IconSize::Normal, 1, 0},
        {L"c", L"C", IconSize::Normal, 2, 0},
        {L"d", L"D", IconSize::Normal, 3, 0},
    };
    CHECK(!layout::ArrangeItems(2, 2, staleSmallCoordinates, &placements));
    for (auto& item : staleSmallCoordinates) {
        item.gridX = -1;
        item.gridY = -1;
    }
    CHECK(layout::ArrangeItems(2, 2, staleSmallCoordinates, &placements));
    CHECK(placements.size() == staleSmallCoordinates.size());

    std::vector<OrganizerItem> arrangedItems{
        {L"normal", L"Normal", IconSize::Normal, 2, 0},
        {L"small-a", L"Small A", IconSize::Small, 0, 0},
        {L"small-b", L"Small B", IconSize::Small, -1, -1},
    };
    CHECK(layout::ArrangeItems(2, 2, arrangedItems, &placements));
    CHECK(placements[0].x == 2 && placements[0].y == 0 && placements[0].span == 2);
    CHECK(placements[1].x == 0 && placements[1].y == 0);
    CHECK(placements[2].x == 1 && placements[2].y == 0);
    layout::Placement nearest;
    CHECK(layout::FindNearestFree(2, 2, placements, 1, 1, 3, 3, nearest));
    CHECK(nearest.x == 3 && nearest.y == 3);

    // Some executable icon resources contain only a tiny glyph surrounded by
    // transparent pixels. Large mode must fit the visible glyph, not preserve
    // the resource's internal empty border.
    std::vector<std::uint32_t> paddedIcon(96 * 96, 0);
    for (int y = 42; y < 54; ++y)
        for (int x = 42; x < 54; ++x)
            paddedIcon[static_cast<std::size_t>(y) * 96 + x] = 0xff20a0ffu;
    const auto fittedIcon = icon_image::FitVisiblePixels(paddedIcon, 96, 96, 96);
    const auto fittedBounds = icon_image::VisibleBounds(fittedIcon, 96, 96);
    CHECK(fittedBounds.Width() >= 84 && fittedBounds.Height() >= 84);

    const auto testDirectory = std::filesystem::temp_directory_path() / L"DesktopOrganizerTests";
    const auto configPath = testDirectory / L"config.json";
    std::error_code error;
    std::filesystem::remove_all(testDirectory, error);

    const auto anchorRoot = testDirectory / L"Desktop";
    std::filesystem::create_directories(anchorRoot, error);
    const auto legacyAnchor = anchorRoot / L"已重命名.desktoporganizer-group";
    {
        std::ofstream file(legacyAnchor, std::ios::binary | std::ios::trunc);
        file << "DESKTOP-ORGANIZER-GROUP/1\n{group-one}\n";
    }
    CHECK(desktop_anchor::ReadGroupId(legacyAnchor) ==
          std::optional<std::wstring>(L"{group-one}"));
    CHECK(!desktop_anchor::RemoveIfOwned(legacyAnchor, L"{wrong-group}"));
    CHECK(desktop_anchor::RemoveIfOwned(legacyAnchor, L"{group-one}"));
    CHECK(!std::filesystem::exists(legacyAnchor));

    ConfigStore missingStore(testDirectory / L"missing.json");
    const auto missing = missingStore.Load();
    CHECK(missing.status == ConfigLoadStatus::Missing);
    CHECK(missing.containers.empty());
    CHECK(!missingStore.IsWriteBlocked());

    ConfigStore emptyStore(testDirectory / L"empty.json");
    CHECK(emptyStore.Save({}));
    const auto empty = emptyStore.Load();
    CHECK(empty.status == ConfigLoadStatus::Loaded);
    CHECK(empty.containers.empty());
    CHECK(!emptyStore.IsWriteBlocked());

    const auto oneCellPath = testDirectory / L"one-cell.json";
    {
        std::ofstream file(oneCellPath, std::ios::binary | std::ios::trunc);
        file << R"({"version":2,"containers":[{"id":"one-cell","columns":1,"rows":1,"items":[]}]})";
    }
    const auto oneCell = ConfigStore(oneCellPath).Load();
    CHECK(oneCell.status == ConfigLoadStatus::Loaded);
    CHECK(oneCell.containers.size() == 1);
    CHECK(oneCell.containers[0].columns == 1 && oneCell.containers[0].rows == 1);

    // Configs written while snapToGrid defaulted to true migrate to free
    // movement exactly once; afterwards the persisted opt-in is respected.
    const auto legacyGridPath = testDirectory / L"legacy-grid.json";
    {
        std::ofstream file(legacyGridPath, std::ios::binary | std::ios::trunc);
        file << R"({"version":2,"containers":[{"id":"grid","columns":2,"rows":2,"snapToGrid":true,"items":[]}]})";
    }
    const auto legacyGrid = ConfigStore(legacyGridPath).Load();
    CHECK(legacyGrid.status == ConfigLoadStatus::Loaded);
    CHECK(legacyGrid.freeMoveMigrated);
    CHECK(!legacyGrid.containers[0].snapToGrid);

    const auto migratedPath = testDirectory / L"migrated.json";
    {
        std::ofstream file(migratedPath, std::ios::binary | std::ios::trunc);
        file << R"({"version":2,"freeMoveMigrated":true,"containers":[{"id":"grid","columns":2,"rows":2,"snapToGrid":true,"items":[]}]})";
    }
    const auto migrated = ConfigStore(migratedPath).Load();
    CHECK(migrated.status == ConfigLoadStatus::Loaded);
    CHECK(!migrated.freeMoveMigrated);
    CHECK(migrated.containers[0].snapToGrid);

    const auto invalidResult = [&](std::wstring_view filename, std::string_view content) {
        const auto path = testDirectory / filename;
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << content;
        }
        ConfigStore invalidStore(path);
        const ConfigLoadResult result = invalidStore.Load();
        return result.status == ConfigLoadStatus::Invalid && invalidStore.IsWriteBlocked();
    };
    CHECK(invalidResult(L"empty-id.json",
                        R"({"version":2,"containers":[{"id":"","items":[]}]})"));
    CHECK(invalidResult(L"duplicate-id.json",
                        R"({"version":2,"containers":[{"id":"same","items":[]},{"id":"same","items":[]}]})"));
    CHECK(invalidResult(L"grid-limit.json",
                        R"({"version":2,"containers":[{"id":"wide","columns":129,"items":[]}]})"));
    CHECK(invalidResult(L"overlap.json",
                        R"({"version":2,"containers":[{"id":"overlap","columns":2,"rows":2,"items":[{"path":"a","gridX":0,"gridY":0},{"path":"b","gridX":0,"gridY":0}]}]})"));
    CHECK(invalidResult(L"fractional-version.json",
                        R"({"version":2.5,"containers":[]})"));
    CHECK(invalidResult(L"leading-zero.json",
                        R"({"version":02,"containers":[]})"));
    CHECK(invalidResult(L"lone-surrogate.json",
                        R"({"version":2,"containers":[{"id":"\uD800","items":[]}]})"));
    CHECK(invalidResult(L"raw-control.json",
                        std::string("{\"version\":2,\"containers\":[{\"id\":\"bad") +
                            '\x01' + "\",\"items\":[]}]}"));

    ContainerState state;
    CHECK(state.opacity == 32);
    CHECK(state.blur == 58);
    CHECK(state.cornerRadius == 22);
    CHECK(state.tintMode == 1);
    CHECK(state.tintColor == 0xF3FAFC);
    CHECK(state.textColor == 0xF5F8FA);
    state.id = L"test-id";
    state.name = L"测试分组";
    state.columns = 3;
    state.rows = 4;
    state.collapsed = true;
    state.tintMode = 3;
    state.tintColor = 0x3A82F7;
    state.textColor = 0x112233;
    state.legacyDesktopAnchorPath = (anchorRoot / L"旧版锚点.desktoporganizer-group").wstring();
    state.items.push_back({L"C:\\测试\\文件.txt", L"文件", IconSize::Large});
    state.items[0].gridX = 2;
    state.items[0].gridY = 3;
    GlobalStyle globalStyle;
    globalStyle.textColor = 0xA1B2C3;
    globalStyle.centerExpandedGroups = true;
    ConfigStore store(configPath);
    CHECK(store.Save({state}, &globalStyle));
    CHECK(std::filesystem::exists(configPath));
    const auto loaded = store.Load();
    CHECK(loaded.status == ConfigLoadStatus::Loaded);
    CHECK(loaded.globalStyle.textColor == 0xA1B2C3);
    CHECK(loaded.globalStyle.centerExpandedGroups);
    CHECK(loaded.containers.size() == 1);
    const auto& loadedState = loaded.containers[0];
    CHECK(loadedState.id == state.id);
    CHECK(loadedState.name == state.name);
    CHECK(loadedState.columns == 3 && loadedState.rows == 4);
    CHECK(loadedState.collapsed);
    CHECK(loadedState.tintMode == 3);
    CHECK(loadedState.tintColor == 0x3A82F7);
    CHECK(loadedState.textColor == 0x112233);
    CHECK(loadedState.legacyDesktopAnchorPath.empty());
    CHECK(loadedState.items.size() == 1);
    CHECK(loadedState.items[0].path == state.items[0].path);
    CHECK(loadedState.items[0].iconSize == IconSize::Large);
    CHECK(loadedState.items[0].gridX == 2 && loadedState.items[0].gridY == 3);

    state.name = L"第二次保存";
    state.collapsed = false;
    state.centeredExpansionActive = true;
    state.collapsedHome = {-420, 735};
    CHECK(store.Save({state}));
    CHECK(std::filesystem::exists(configPath));
    CHECK(std::filesystem::exists(configPath.wstring() + L".bak"));
    const auto replaced = store.Load();
    CHECK(replaced.status == ConfigLoadStatus::Loaded);
    CHECK(replaced.containers.size() == 1 && replaced.containers[0].name == L"第二次保存");
    CHECK(replaced.containers[0].centeredExpansionActive);
    CHECK(replaced.containers[0].collapsedHome.x == -420);
    CHECK(replaced.containers[0].collapsedHome.y == 735);

    HANDLE configLock = CreateFileW(configPath.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(configLock != INVALID_HANDLE_VALUE);
    std::thread releaseConfigLock([configLock] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CloseHandle(configLock);
    });
    state.name = L"短暂占用后保存";
    const bool savedAfterShortContention = store.Save({state});
    releaseConfigLock.join();
    CHECK(savedAfterShortContention);

    const std::wstring longSegmentA(86, L'a');
    const std::wstring longSegmentB(86, L'b');
    const std::wstring longSegmentC(86, L'c');
    const auto longSource = testDirectory / L"long-source";
    const auto longRelative = std::filesystem::path(longSegmentA) / longSegmentB / longSegmentC;
    const auto longSourceDirectory = longSource / longRelative;
    const auto longDestination = testDirectory / L"long-destination";
    const auto extended = [](const std::filesystem::path& value) {
        return std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(value).wstring());
    };
    std::filesystem::create_directories(extended(longSourceDirectory), error);
    CHECK(!error);
    {
        std::ofstream payload(extended(longSourceDirectory / L"payload.txt"), std::ios::binary);
        payload << "long-path";
        CHECK(payload.good());
    }
    CHECK(path_io::CopyPathPreservingSource(longSource, longDestination));
    CHECK(std::filesystem::exists(extended(longDestination / longRelative / L"payload.txt")));
    std::filesystem::remove_all(extended(longSource), error);
    error.clear();
    std::filesystem::remove_all(extended(longDestination), error);
    error.clear();

    {
        std::ofstream corrupt(configPath, std::ios::binary | std::ios::trunc);
        corrupt << "not-json";
    }
    const auto recovered = store.Load();
    CHECK(recovered.status == ConfigLoadStatus::RecoveredBackup);
    CHECK(recovered.containers.size() == 1 && recovered.containers[0].name == L"第二次保存");
    CHECK(!store.IsWriteBlocked());

    const auto missingMainPath = testDirectory / L"missing-main.json";
    {
        std::ofstream backup(std::filesystem::path(missingMainPath.wstring() + L".bak"),
                             std::ios::binary | std::ios::trunc);
        backup << "{\"version\":2,\"containers\":[]}";
    }
    ConfigStore missingMainStore(missingMainPath);
    const auto missingMainRecovered = missingMainStore.Load();
    CHECK(missingMainRecovered.status == ConfigLoadStatus::RecoveredBackup);
    CHECK(missingMainRecovered.containers.empty());
    CHECK(!missingMainStore.IsWriteBlocked());

    const auto futurePath = testDirectory / "future.json";
    {
        std::ofstream future(futurePath, std::ios::binary | std::ios::trunc);
        future << "{\"version\":3,\"containers\":[]}";
    }
    ConfigStore futureStore(futurePath);
    CHECK(futureStore.Load().status == ConfigLoadStatus::FutureVersion);
    CHECK(futureStore.IsWriteBlocked());
    CHECK(!futureStore.Save({state}));

    ContainerState invalidWrite = state;
    invalidWrite.id.clear();
    ConfigStore invalidWriteStore(testDirectory / L"invalid-write.json");
    CHECK(!invalidWriteStore.Save({invalidWrite}));
    CHECK(!std::filesystem::exists(invalidWriteStore.Path()));

    ContainerState overlappingWrite = state;
    overlappingWrite.items.push_back(overlappingWrite.items.front());
    ConfigStore overlappingWriteStore(testDirectory / L"overlapping-write.json");
    CHECK(!overlappingWriteStore.Save({overlappingWrite}));
    CHECK(!std::filesystem::exists(overlappingWriteStore.Path()));

    std::filesystem::remove_all(testDirectory, error);
    std::cout << "All tests passed\n";
    return 0;
}
