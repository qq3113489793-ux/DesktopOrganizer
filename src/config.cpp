#include "config.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <iterator>
#include <unordered_set>
#include <variant>

namespace {

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data;

    const Object* object() const { return std::get_if<Object>(&data); }
    const Array* array() const { return std::get_if<Array>(&data); }
    std::string string(std::string_view key, std::string fallback = {}) const {
        const auto* obj = object();
        if (!obj) return fallback;
        const auto it = obj->find(key);
        if (it == obj->end()) return fallback;
        const auto* value = std::get_if<std::string>(&it->second.data);
        return value ? *value : fallback;
    }
    int integer(std::string_view key, int fallback) const {
        const auto* obj = object();
        if (!obj) return fallback;
        const auto it = obj->find(key);
        if (it == obj->end()) return fallback;
        const auto* value = std::get_if<double>(&it->second.data);
        return value ? static_cast<int>(*value) : fallback;
    }
    bool boolean(std::string_view key, bool fallback) const {
        const auto* obj = object();
        if (!obj) return fallback;
        const auto it = obj->find(key);
        if (it == obj->end()) return fallback;
        const auto* value = std::get_if<bool>(&it->second.data);
        return value ? *value : fallback;
    }
    const JsonValue* get(std::string_view key) const {
        const auto* obj = object();
        if (!obj) return nullptr;
        const auto it = obj->find(key);
        return it == obj->end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool Parse(JsonValue& output) {
        Skip();
        if (!Value(output, 0)) return false;
        Skip();
        return position_ == input_.size();
    }

private:
    void Skip() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\n' || input_[position_] == '\r' || input_[position_] == '\t')) ++position_;
    }

    bool Consume(char expected) {
        Skip();
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool Value(JsonValue& out, std::size_t depth) {
        if (depth > 64 || ++nodeCount_ > 300000) return false;
        Skip();
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '{') return Object(out, depth + 1);
        if (input_[position_] == '[') return Array(out, depth + 1);
        if (input_[position_] == '"') {
            std::string value;
            if (!String(value)) return false;
            out.data = std::move(value);
            return true;
        }
        if (input_.substr(position_, 4) == "true") { position_ += 4; out.data = true; return true; }
        if (input_.substr(position_, 5) == "false") { position_ += 5; out.data = false; return true; }
        if (input_.substr(position_, 4) == "null") { position_ += 4; out.data = nullptr; return true; }
        return Number(out);
    }

    bool Object(JsonValue& out, std::size_t depth) {
        if (!Consume('{')) return false;
        JsonValue::Object object;
        Skip();
        if (Consume('}')) { out.data = std::move(object); return true; }
        while (true) {
            std::string key;
            if (!String(key) || !Consume(':')) return false;
            JsonValue value;
            if (!Value(value, depth)) return false;
            if (!object.emplace(std::move(key), std::move(value)).second) return false;
            Skip();
            if (Consume('}')) break;
            if (!Consume(',')) return false;
        }
        out.data = std::move(object);
        return true;
    }

    bool Array(JsonValue& out, std::size_t depth) {
        if (!Consume('[')) return false;
        JsonValue::Array array;
        Skip();
        if (Consume(']')) { out.data = std::move(array); return true; }
        while (true) {
            JsonValue value;
            if (!Value(value, depth)) return false;
            array.push_back(std::move(value));
            Skip();
            if (Consume(']')) break;
            if (!Consume(',')) return false;
        }
        out.data = std::move(array);
        return true;
    }

    bool String(std::string& out) {
        if (!Consume('"')) return false;
        out.clear();
        while (position_ < input_.size()) {
            const char ch = input_[position_++];
            if (ch == '"') return true;
            if (ch != '\\') {
                if (static_cast<unsigned char>(ch) < 0x20) return false;
                out.push_back(ch);
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > input_.size()) return false;
                const auto readCodeUnit = [&]() -> std::optional<unsigned> {
                    if (position_ + 4 > input_.size()) return std::nullopt;
                    unsigned value = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = input_[position_++];
                        value <<= 4;
                        if (digit >= '0' && digit <= '9') value += digit - '0';
                        else if (digit >= 'a' && digit <= 'f') value += digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F') value += digit - 'A' + 10;
                        else return std::nullopt;
                    }
                    return value;
                };
                const auto first = readCodeUnit();
                if (!first || (*first >= 0xdc00 && *first <= 0xdfff)) return false;
                std::wstring wide(1, static_cast<wchar_t>(*first));
                if (*first >= 0xd800 && *first <= 0xdbff) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    const auto second = readCodeUnit();
                    if (!second || *second < 0xdc00 || *second > 0xdfff) return false;
                    wide.push_back(static_cast<wchar_t>(*second));
                }
                out += ToUtf8(wide);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool Number(JsonValue& out) {
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        const size_t digits = position_;
        while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
        if (position_ == digits) return false;
        if (position_ - digits > 1 && input_[digits] == '0') return false;
        try {
            out.data = std::stod(std::string(input_.substr(start, position_ - start)));
            return true;
        } catch (...) { return false; }
    }

    std::string_view input_;
    size_t position_ = 0;
    std::size_t nodeCount_ = 0;
};

std::string Escape(std::wstring_view value) {
    const std::string utf8 = ToUtf8(value);
    std::string result;
    result.reserve(utf8.size() + 8);
    for (const unsigned char ch : utf8) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[7];
                sprintf_s(buffer, "\\u%04x", ch);
                result += buffer;
            } else result.push_back(static_cast<char>(ch));
        }
    }
    return result;
}

bool ReadInteger(const JsonValue& object, std::string_view key, int fallback,
                 int minimum, int maximum, int& result, bool required = false) {
    const JsonValue* value = object.get(key);
    if (!value) {
        result = fallback;
        return !required;
    }
    const auto* number = std::get_if<double>(&value->data);
    if (!number || !std::isfinite(*number) || std::trunc(*number) != *number ||
        *number < minimum || *number > maximum) return false;
    result = static_cast<int>(*number);
    return true;
}

bool ReadBoolean(const JsonValue& object, std::string_view key, bool fallback, bool& result) {
    const JsonValue* value = object.get(key);
    if (!value) {
        result = fallback;
        return true;
    }
    const auto* boolean = std::get_if<bool>(&value->data);
    if (!boolean) return false;
    result = *boolean;
    return true;
}

bool ReadWideString(const JsonValue& object, std::string_view key, std::wstring_view fallback,
                    std::size_t maximumBytes, std::wstring& result, bool required = false) {
    const JsonValue* value = object.get(key);
    if (!value) {
        result = fallback;
        return !required;
    }
    const auto* string = std::get_if<std::string>(&value->data);
    if (!string || string->size() > maximumBytes) return false;
    result = FromUtf8(*string);
    return string->empty() || !result.empty();
}

int ItemSpan(IconSize size) {
    if (size == IconSize::Small) return 1;
    if (size == IconSize::Large) return 4;
    return 2;
}

bool OccupyCells(std::vector<unsigned char>& occupied, int width, int height,
                 int x, int y, int span) {
    if (x < 0 || y < 0 || x + span > width || y + span > height) return false;
    for (int row = y; row < y + span; ++row)
        for (int column = x; column < x + span; ++column)
            if (occupied[static_cast<std::size_t>(row * width + column)]) return false;
    for (int row = y; row < y + span; ++row)
        for (int column = x; column < x + span; ++column)
            occupied[static_cast<std::size_t>(row * width + column)] = 1;
    return true;
}

ConfigLoadResult ParseDocument(std::string_view content) {
    ConfigLoadResult result{ConfigLoadStatus::Invalid, {}};
    JsonValue root;
    if (!JsonParser(content).Parse(root) || !root.object()) return result;

    int version = 0;
    if (!ReadInteger(root, "version", 0, 1, INT_MAX, version, true)) return result;
    if (version > 2) {
        result.status = ConfigLoadStatus::FutureVersion;
        return result;
    }
    if (const JsonValue* globalValue = root.get("globalStyle")) {
        if (!globalValue->object()) return result;
        GlobalStyle style;
        if (!ReadInteger(*globalValue, "opacity", style.opacity, 15, 96, style.opacity) ||
            !ReadInteger(*globalValue, "blur", style.blur, 0, 100, style.blur) ||
            !ReadInteger(*globalValue, "cornerRadius", style.cornerRadius, 0, 48, style.cornerRadius) ||
            !ReadInteger(*globalValue, "tintMode", style.tintMode, 0, 3, style.tintMode) ||
            !ReadInteger(*globalValue, "tintColor", style.tintColor, 0, 0xFFFFFF, style.tintColor) ||
            !ReadInteger(*globalValue, "textColor", style.textColor, 0, 0xFFFFFF, style.textColor) ||
            !ReadBoolean(*globalValue, "showTitle", style.showTitle, style.showTitle) ||
            !ReadBoolean(*globalValue, "showBorder", style.showBorder, style.showBorder) ||
            !ReadBoolean(*globalValue, "centerExpandedGroups", style.centerExpandedGroups,
                         style.centerExpandedGroups)) {
            return result;
        }
        result.globalStyle = style;
    }
    const JsonValue* containersValue = root.get("containers");
    const auto* containers = containersValue ? containersValue->array() : nullptr;
    if (!containers || containers->size() > ConfigStore::MaxContainers) return result;

    std::unordered_set<std::wstring> ids;
    std::size_t totalItems = 0;
    result.containers.reserve(containers->size());
    for (const auto& value : *containers) {
        if (!value.object()) return {ConfigLoadStatus::Invalid, {}};
        ContainerState state;
        int iconSize = 1;
        int boundsLeft = 120;
        int boundsTop = 120;
        int collapsedHomeX = 0;
        int collapsedHomeY = 0;
        if (!ReadWideString(value, "id", L"", 128, state.id, true) || state.id.empty() ||
            !ids.insert(state.id).second ||
            !ReadWideString(value, "name", L"新建分组", 1024, state.name) ||
            !ReadInteger(value, "x", 120, -1000000, 1000000, boundsLeft) ||
            !ReadInteger(value, "y", 120, -1000000, 1000000, boundsTop) ||
            !ReadInteger(value, "columns", 2, 1, ConfigStore::MaxGridExtent, state.columns) ||
            !ReadInteger(value, "rows", 2, 1, ConfigStore::MaxGridExtent, state.rows) ||
            !ReadBoolean(value, "halfColumn", false, state.halfColumn) ||
            !ReadBoolean(value, "halfRow", false, state.halfRow) ||
            !ReadInteger(value, "iconSize", 1, 0, 2, iconSize) ||
            !ReadInteger(value, "opacity", 32, 15, 96, state.opacity) ||
            !ReadInteger(value, "blur", 58, 0, 100, state.blur) ||
            !ReadInteger(value, "cornerRadius", 22, 0, 48, state.cornerRadius) ||
            !ReadInteger(value, "tintMode", 1, 0, 3, state.tintMode) ||
            !ReadInteger(value, "tintColor", 0xF3FAFC, 0, 0xFFFFFF, state.tintColor) ||
            !ReadInteger(value, "textColor", 0xF5F8FA, 0, 0xFFFFFF, state.textColor) ||
            !ReadBoolean(value, "showTitle", true, state.showTitle) ||
            !ReadBoolean(value, "showBorder", true, state.showBorder) ||
            !ReadBoolean(value, "locked", false, state.locked) ||
            !ReadBoolean(value, "collapsed", false, state.collapsed) ||
            !ReadBoolean(value, "centeredExpansionActive", false, state.centeredExpansionActive) ||
            !ReadInteger(value, "collapsedHomeX", 0, -1000000, 1000000, collapsedHomeX) ||
            !ReadInteger(value, "collapsedHomeY", 0, -1000000, 1000000, collapsedHomeY) ||
            !ReadBoolean(value, "followGlobalStyle", false, state.followGlobalStyle) ||
            !ReadBoolean(value, "snapToGrid", false, state.snapToGrid) ||
            !ReadBoolean(value, "pushIcons", true, state.pushIcons) ||
            !ReadWideString(value, "desktopAnchorPath", L"", 32767, state.legacyDesktopAnchorPath) ||
            !ReadWideString(value, "monitor", L"", 1024, state.monitor)) {
            return {ConfigLoadStatus::Invalid, {}};
        }
        state.bounds.left = boundsLeft;
        state.bounds.top = boundsTop;
        state.collapsedHome = {collapsedHomeX, collapsedHomeY};
        // A saved home is meaningful only while the full group is open. Older
        // or manually edited configurations may contain an inconsistent pair;
        // clear it instead of unexpectedly moving an already compact group.
        if (state.collapsed) state.centeredExpansionActive = false;
        state.iconSize = static_cast<IconSize>(iconSize);

        const JsonValue* itemsValue = value.get("items");
        const auto* items = itemsValue ? itemsValue->array() : nullptr;
        if (itemsValue && !items) return {ConfigLoadStatus::Invalid, {}};
        const std::size_t itemCount = items ? items->size() : 0;
        if (itemCount > ConfigStore::MaxItemsPerContainer ||
            totalItems > ConfigStore::MaxTotalItems - itemCount) {
            return {ConfigLoadStatus::Invalid, {}};
        }
        totalItems += itemCount;
        state.items.reserve(itemCount);
        if (items) {
            for (const auto& itemValue : *items) {
                if (!itemValue.object()) return {ConfigLoadStatus::Invalid, {}};
                OrganizerItem item;
                int itemIconSize = 1;
                if (!ReadWideString(itemValue, "path", L"", 32767 * 4, item.path, true) || item.path.empty() ||
                    !ReadWideString(itemValue, "name", L"", 4096, item.name) ||
                    !ReadInteger(itemValue, "iconSize", 1, 0, 2, itemIconSize) ||
                    !ReadInteger(itemValue, "gridX", -1, -1, ConfigStore::MaxGridExtent * 2 + 1, item.gridX) ||
                    !ReadInteger(itemValue, "gridY", -1, -1, ConfigStore::MaxGridExtent * 2 + 1, item.gridY) ||
                    ((item.gridX == -1) != (item.gridY == -1))) {
                    return {ConfigLoadStatus::Invalid, {}};
                }
                item.iconSize = static_cast<IconSize>(itemIconSize);
                state.items.push_back(std::move(item));
            }
        }

        const int width = state.columns * 2 + (state.halfColumn ? 1 : 0);
        const int height = state.rows * 2 + (state.halfRow ? 1 : 0);
        std::vector<unsigned char> occupied(static_cast<std::size_t>(width * height), 0);
        for (const auto& item : state.items) {
            if (item.gridX >= 0 && !OccupyCells(occupied, width, height, item.gridX, item.gridY,
                                               ItemSpan(item.iconSize))) {
                return {ConfigLoadStatus::Invalid, {}};
            }
        }
        int nextCandidate[5]{};
        for (const auto& item : state.items) {
            if (item.gridX >= 0) continue;
            const int span = ItemSpan(item.iconSize);
            bool placed = false;
            for (int candidate = nextCandidate[span]; candidate < width * height; ++candidate) {
                const int x = candidate % width;
                const int y = candidate / width;
                if (!OccupyCells(occupied, width, height, x, y, span)) continue;
                nextCandidate[span] = candidate + 1;
                placed = true;
                break;
            }
            if (!placed) return {ConfigLoadStatus::Invalid, {}};
        }
        result.containers.push_back(std::move(state));
    }

    // One-time behavior migration: older configs were written while
    // snapToGrid defaulted to true, so every container carried "snapToGrid":
    // true and dragged containers were locked to the desktop icon grid. Free
    // movement with edge/sibling snapping is now the default; reset those
    // containers once and ask the caller to persist the flag so a user's own
    // opt-in is respected afterwards.
    const JsonValue* freeMoveFlag = root.get("freeMoveMigrated");
    bool freeMoveMigrated = false;
    if (freeMoveFlag) {
        const auto* flag = std::get_if<bool>(&freeMoveFlag->data);
        if (!flag) return result;
        freeMoveMigrated = *flag;
    }
    if (!freeMoveMigrated) {
        for (auto& state : result.containers) state.snapToGrid = false;
        result.freeMoveMigrated = true;
    }
    result.status = ConfigLoadStatus::Loaded;
    return result;
}

} // namespace

ConfigStore::ConfigStore() {
#ifdef DESKTOP_ORGANIZER_VISUAL_TEST
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_CONFIG", overridePath, static_cast<DWORD>(std::size(overridePath)));
    if (overrideLength > 0 && overrideLength < std::size(overridePath)) {
        path_ = overridePath;
        return;
    }
    // The visual-test executable is isolated even when launched by a UI
    // harness that cannot inject environment variables.
    path_ = std::filesystem::temp_directory_path() / L"desktop-organizer-smooth-test.json";
    return;
#endif
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
        path_ = std::filesystem::path(localAppData) / L"DesktopOrganizer" / L"config.json";
        CoTaskMemFree(localAppData);
    } else {
        path_ = std::filesystem::current_path() / L"config.json";
    }
}

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}

ConfigLoadResult ConfigStore::Load() const {
    writeBlocked_ = isolationError_;
    preserveBackupOnNextSave_ = false;
    if (isolationError_) return {ConfigLoadStatus::IsolationError, {}};

    enum class ReadStatus { Missing, Invalid, Loaded };
    const auto readFile = [](const std::filesystem::path& file, std::string& content) {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error) {
            return error == std::errc::no_such_file_or_directory
                       ? ReadStatus::Missing : ReadStatus::Invalid;
        }
        if (size > ConfigStore::MaxConfigBytes) return ReadStatus::Invalid;
        std::ifstream stream(file, std::ios::binary);
        if (!stream) return ReadStatus::Invalid;
        content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        return stream.bad() ? ReadStatus::Invalid : ReadStatus::Loaded;
    };

    std::string content;
    const ReadStatus mainRead = readFile(path_, content);

    ConfigLoadResult mainResult = mainRead == ReadStatus::Loaded
                                      ? ParseDocument(content)
                                      : ConfigLoadResult{ConfigLoadStatus::Invalid, {}};
    if (mainResult.status == ConfigLoadStatus::Loaded) return mainResult;
    if (mainResult.status == ConfigLoadStatus::FutureVersion) {
        writeBlocked_ = true;
        return mainResult;
    }

    const std::filesystem::path backup = path_.wstring() + L".bak";
    std::string backupContent;
    const ReadStatus backupRead = readFile(backup, backupContent);
    if (backupRead == ReadStatus::Loaded) {
        ConfigLoadResult backupResult = ParseDocument(backupContent);
        if (backupResult.status == ConfigLoadStatus::Loaded) {
            backupResult.status = ConfigLoadStatus::RecoveredBackup;
            preserveBackupOnNextSave_ = true;
            return backupResult;
        }
        if (backupResult.status == ConfigLoadStatus::FutureVersion) {
            writeBlocked_ = true;
            return backupResult;
        }
    }

    if (mainRead == ReadStatus::Missing && backupRead == ReadStatus::Missing) {
        return {ConfigLoadStatus::Missing, {}};
    }
    writeBlocked_ = true;
    return {ConfigLoadStatus::Invalid, {}};
}

bool ConfigStore::Save(const std::vector<ContainerState>& containers, const GlobalStyle* globalStyle) const {
    lastSaveFailure_ = {};
    if (writeBlocked_ || isolationError_) {
        lastSaveFailure_.stage = ConfigSaveStage::Blocked;
        return false;
    }
    if (containers.size() > MaxContainers) {
        lastSaveFailure_.stage = ConfigSaveStage::Limits;
        return false;
    }
    std::size_t totalItems = 0;
    for (const auto& state : containers) {
        if (state.items.size() > MaxItemsPerContainer ||
            totalItems > MaxTotalItems - state.items.size()) {
            lastSaveFailure_.stage = ConfigSaveStage::Limits;
            return false;
        }
        totalItems += state.items.size();
    }
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        lastSaveFailure_ = {ConfigSaveStage::CreateDirectory,
                            static_cast<unsigned long>(error.value()), 0};
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    std::ostringstream stream;

    stream << "{\n  \"version\": 2,\n"
           << "  \"freeMoveMigrated\": true,\n";
    if (globalStyle) {
        stream << "  \"globalStyle\": {\"opacity\": " << globalStyle->opacity
               << ", \"blur\": " << globalStyle->blur
               << ", \"cornerRadius\": " << globalStyle->cornerRadius
               << ", \"tintMode\": " << globalStyle->tintMode
               << ", \"tintColor\": " << globalStyle->tintColor
               << ", \"textColor\": " << globalStyle->textColor
               << ", \"showTitle\": " << (globalStyle->showTitle ? "true" : "false")
               << ", \"showBorder\": " << (globalStyle->showBorder ? "true" : "false")
               << ", \"centerExpandedGroups\": "
               << (globalStyle->centerExpandedGroups ? "true" : "false") << "},\n";
    }
    stream << "  \"containers\": [\n";
    for (size_t index = 0; index < containers.size(); ++index) {
        const auto& state = containers[index];
        stream << "    {\n"
               << "      \"id\": \"" << Escape(state.id) << "\",\n"
               << "      \"name\": \"" << Escape(state.name) << "\",\n"
               << "      \"x\": " << state.bounds.left << ", \"y\": " << state.bounds.top << ",\n"
               << "      \"columns\": " << state.columns << ", \"rows\": " << state.rows << ",\n"
               << "      \"halfColumn\": " << (state.halfColumn ? "true" : "false")
               << ", \"halfRow\": " << (state.halfRow ? "true" : "false") << ",\n"
               << "      \"iconSize\": " << static_cast<int>(state.iconSize) << ",\n"
               << "      \"opacity\": " << state.opacity << ", \"blur\": " << state.blur << ",\n"
               << "      \"cornerRadius\": " << state.cornerRadius << ",\n"
               << "      \"tintMode\": " << state.tintMode << ",\n"
               << "      \"tintColor\": " << state.tintColor << ",\n"
               << "      \"textColor\": " << state.textColor << ",\n"
               << "      \"showTitle\": " << (state.showTitle ? "true" : "false") << ",\n"
               << "      \"showBorder\": " << (state.showBorder ? "true" : "false") << ",\n"
               << "      \"locked\": " << (state.locked ? "true" : "false") << ",\n"
               << "      \"collapsed\": " << (state.collapsed ? "true" : "false") << ",\n"
               << "      \"centeredExpansionActive\": "
               << (state.centeredExpansionActive ? "true" : "false") << ",\n"
               << "      \"collapsedHomeX\": " << state.collapsedHome.x
               << ", \"collapsedHomeY\": " << state.collapsedHome.y << ",\n"
               << "      \"followGlobalStyle\": " << (state.followGlobalStyle ? "true" : "false") << ",\n"
               << "      \"snapToGrid\": " << (state.snapToGrid ? "true" : "false") << ",\n"
               << "      \"pushIcons\": " << (state.pushIcons ? "true" : "false") << ",\n"
               << "      \"monitor\": \"" << Escape(state.monitor) << "\",\n"
               << "      \"items\": [";
        for (size_t itemIndex = 0; itemIndex < state.items.size(); ++itemIndex) {
            const auto& item = state.items[itemIndex];
            if (itemIndex) stream << ',';
            stream << "\n        {\"path\": \"" << Escape(item.path) << "\", \"name\": \"" << Escape(item.name)
                   << "\", \"iconSize\": " << static_cast<int>(item.iconSize)
                   << ", \"gridX\": " << item.gridX << ", \"gridY\": " << item.gridY << "}";
        }
        if (!state.items.empty()) stream << '\n' << "      ";
        stream << "]\n    }" << (index + 1 < containers.size() ? "," : "") << '\n';
    }
    stream << "  ]\n}\n";
    const std::string serialized = stream.str();
    if (serialized.size() > MaxConfigBytes ||
        ParseDocument(serialized).status != ConfigLoadStatus::Loaded) {
        lastSaveFailure_.stage = ConfigSaveStage::Validate;
        return false;
    }
    std::filesystem::remove(temporary, error);
    if (error) {
        lastSaveFailure_ = {ConfigSaveStage::RemoveTemporary,
                            static_cast<unsigned long>(error.value()), 0};
        return false;
    }
    error.clear();
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        lastSaveFailure_ = {ConfigSaveStage::OpenTemporary, GetLastError(), 0};
        return false;
    }
    file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    file.close();
    if (!file) {
        lastSaveFailure_ = {ConfigSaveStage::WriteTemporary, GetLastError(), 0};
        return false;
    }

    const std::filesystem::path backup = path_.wstring() + L".bak";
    constexpr DWORD retryDelaysMs[] = {0, 12, 24, 48, 80};
    DWORD replaceError = ERROR_SUCCESS;
    DWORD moveError = ERROR_SUCCESS;
    const auto isTransient = [](DWORD value) {
        return value == ERROR_ACCESS_DENIED || value == ERROR_SHARING_VIOLATION ||
               value == ERROR_LOCK_VIOLATION || value == ERROR_USER_MAPPED_FILE ||
               value == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
               value == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
    };
    for (size_t attempt = 0; attempt < std::size(retryDelaysMs); ++attempt) {
        if (retryDelaysMs[attempt] != 0) Sleep(retryDelaysMs[attempt]);
        error.clear();
        if (std::filesystem::exists(path_, error)) {
            const wchar_t* backupPath = preserveBackupOnNextSave_ ? nullptr : backup.c_str();
            if (ReplaceFileW(path_.c_str(), temporary.c_str(), backupPath,
                             REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
                preserveBackupOnNextSave_ = false;
                return true;
            }
            replaceError = GetLastError();
        }

        if (MoveFileExW(temporary.c_str(), path_.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            preserveBackupOnNextSave_ = false;
            return true;
        }
        moveError = GetLastError();
        if (!isTransient(moveError) && !isTransient(replaceError)) break;
    }
    lastSaveFailure_ = {ConfigSaveStage::ReplaceFile, moveError, replaceError};
    std::filesystem::remove(temporary, error);
    return false;
}
