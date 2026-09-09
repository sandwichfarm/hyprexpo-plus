#define WLR_USE_UNSTABLE

#include "Dispatchers.hpp"

#include "ExpoGesture.hpp"
#include "HyprexpoConfig.hpp"
#include "HyprexpoLogic.hpp"
#include "HyprlandConfigCompat.hpp"
#include "IOverviewSession.hpp"
#include "ScrollingDiagnostics.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/keybinds/Resolver.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <lua.hpp>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <sstream>
#include <string>
#include <string_view>

static bool g_unloading                   = false;
static bool g_gestureRegistrationDisabled = false;
static bool renderingOverview             = false;
static SP<Config::Values::CStringValue> g_pCancelKeyConfig;
static SP<Config::Values::CStringValue> g_pNumberKeyModeConfig;

static SDispatchResult onExpoDispatcher(std::string arg);
static SDispatchResult onKbFocusDispatcher(std::string arg);
static SDispatchResult onKbConfirmDispatcher(std::string arg);
static SDispatchResult onKbSelectNumberDispatcher(std::string arg);
static SDispatchResult onKbSelectTokenDispatcher(std::string arg);
static SDispatchResult onKbSelectIndexDispatcher(std::string arg);
static SDispatchResult onMovePreviewWindowDispatcher(std::string arg);
static SDispatchResult onScrollingDebugDispatcher(std::string arg);
static SDispatchResult onScrollingInputTestDispatcher(std::string arg);
static SDispatchResult onScrollingMutationTestDispatcher(std::string arg);
static SDispatchResult registerExpoGesture(int fingerCount, const std::string& directionName, const std::string& action, const std::string& mods, float deltaScale, bool disableInhibit);

static std::string trimString(std::string value) {
    while (!value.empty() && std::isspace((unsigned char)value.front()))
        value.erase(value.begin());
    while (!value.empty() && std::isspace((unsigned char)value.back()))
        value.pop_back();
    return value;
}

static std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

static bool parseStrictInteger(const std::string& value, int& out) {
    const std::string trimmed = trimString(value);
    if (trimmed.empty())
        return false;

    const char* begin = trimmed.data();
    const char* end   = begin + trimmed.size();
    int         parsed = 0;
    const auto  result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
        return false;

    out = parsed;
    return true;
}

static bool isCancelKeyDisabled(const std::string& keyName) {
    const std::string key = lowerString(keyName);
    return key.empty() || key == "none" || key == "disabled" || key == "disable" || key == "off";
}

static bool keyNameMatchesKeysym(const std::string& keyName, xkb_keysym_t keysym) {
    if (keyName.empty())
        return false;

    const auto configuredKeysym = xkb_keysym_from_name(keyName.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (configuredKeysym == XKB_KEY_NoSymbol)
        return false;

    return xkb_keysym_to_lower(keysym) == xkb_keysym_to_lower(configuredKeysym);
}

static bool matchesCancelKey(xkb_keysym_t keysym) {
    std::string keyConfig = g_pCancelKeyConfig ? g_pCancelKeyConfig->value() : "escape";
    size_t      start     = 0;

    while (start <= keyConfig.size()) {
        size_t comma = keyConfig.find(',', start);
        if (comma == std::string::npos)
            comma = keyConfig.size();

        const std::string keyName = trimString(keyConfig.substr(start, comma - start));
        if (isCancelKeyDisabled(keyName))
            return false;
        if (keyNameMatchesKeysym(keyName, keysym))
            return true;

        if (comma == keyConfig.size())
            break;
        start = comma + 1;
    }

    return false;
}

bool shouldCancelOverview(const IKeyboard::SKeyEvent& event) {
    if (!overviewOpen() || event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return false;

    const auto KEYCODE  = event.keycode + 8;
    const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (KEYBOARD && KEYBOARD->m_xkbState && matchesCancelKey(xkb_state_key_get_one_sym(KEYBOARD->m_xkbState, KEYCODE)))
        return true;
    if (KEYBOARD && KEYBOARD->m_xkbSymState && matchesCancelKey(xkb_state_key_get_one_sym(KEYBOARD->m_xkbSymState, KEYCODE)))
        return true;

    return false;
}

static PHLWINDOW windowToBringFromWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return nullptr;

    const auto& windows = Desktop::windowState()->windows();
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        const auto& window = *it;
        if (!window || window->m_workspace != workspace || !window->mapped() || window->isHidden())
            continue;

        return window;
    }

    return nullptr;
}

static SDispatchResult bringWindowFromWorkspace(int64_t sourceWorkspaceID, const PHLMONITOR& destinationMonitor) {
    if (sourceWorkspaceID == WORKSPACE_INVALID)
        return {.success = false, .error = "selected workspace is empty"};

    if (!destinationMonitor || !destinationMonitor->m_activeWorkspace)
        return {.success = false, .error = "no active monitor/workspace"};

    if (sourceWorkspaceID == destinationMonitor->activeWorkspaceID())
        return {};

    PHLWORKSPACE sourceWorkspace;
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w->m_id == sourceWorkspaceID) {
            sourceWorkspace = w;
            break;
        }
    }
    if (!sourceWorkspace)
        return {.success = false, .error = "selected workspace is not open"};

    const auto window = windowToBringFromWorkspace(sourceWorkspace);
    if (!window)
        return {.success = false, .error = "selected workspace has no mapped windows"};

    Desktop::globalWindowController()->moveWindowToWorkspace(window, destinationMonitor->m_activeWorkspace);
    const auto focusState = Desktop::focusState();
    if (focusState)
        focusState->fullWindowFocus(window, Desktop::FOCUS_REASON_KEYBIND);
    window->warpCursor();
    return {};
}

// Open on every monitor, or just the one under the cursor.
static SDispatchResult openOverviews(bool allMonitors) {
    renderingOverview = true;
    bool failed = false;
    const auto open = [&failed](const PHLMONITOR& monitor) {
        if (monitor && monitor->m_activeWorkspace && !overviewForMonitor(monitor) && !createOverview(monitor))
            failed = true;
    };

    if (allMonitors) {
        for (const auto& MONITOR : State::monitorState()->monitors())
            open(MONITOR);
    } else if (const auto PMONITOR = State::monitorState()->query().vec(g_pInputManager->getMouseCoordsInternal()).run())
        open(PMONITOR);

    renderingOverview = false;
    if (failed)
        return {.success = false, .error = "failed to initialize native scrolling overview"};
    return {};
}

static bool isSingleDigitWorkspaceArg(const std::string& arg) {
    return arg.size() == 1 && arg[0] >= '1' && arg[0] <= '9';
}

static SDispatchResult changeToSingleDigitWorkspace(const std::string& arg) {
    const int workspaceID = arg[0] - '0';

    if (auto* const OV = activeOverview()) {
        if (OV->selectWorkspaceByID(workspaceID)) {
            closeOverviewsSelecting(OV);
            return {};
        }

        closeOverviews(false);
    }

    const auto change = Config::Actions::changeWorkspace(arg);
    if (!change)
        return {.success = false, .error = change.error().message};

    return {};
}

static std::string workspaceArgForKeysym(xkb_keysym_t keysym) {
    switch (keysym) {
        case XKB_KEY_0:
        case XKB_KEY_KP_0: return "0";
        case XKB_KEY_1:
        case XKB_KEY_KP_1: return "1";
        case XKB_KEY_2:
        case XKB_KEY_KP_2: return "2";
        case XKB_KEY_3:
        case XKB_KEY_KP_3: return "3";
        case XKB_KEY_4:
        case XKB_KEY_KP_4: return "4";
        case XKB_KEY_5:
        case XKB_KEY_KP_5: return "5";
        case XKB_KEY_6:
        case XKB_KEY_KP_6: return "6";
        case XKB_KEY_7:
        case XKB_KEY_KP_7: return "7";
        case XKB_KEY_8:
        case XKB_KEY_KP_8: return "8";
        case XKB_KEY_9:
        case XKB_KEY_KP_9: return "9";
        default: return "";
    }
}

static std::string workspaceArgForKeyEvent(const IKeyboard::SKeyEvent& event) {
    if (!overviewOpen() || event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return "";

    const auto KEYCODE  = event.keycode + 8;
    const auto KEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (KEYBOARD && KEYBOARD->m_xkbState) {
        const auto arg = workspaceArgForKeysym(xkb_state_key_get_one_sym(KEYBOARD->m_xkbState, KEYCODE));
        if (!arg.empty())
            return arg;
    }

    if (KEYBOARD && KEYBOARD->m_xkbSymState) {
        const auto arg = workspaceArgForKeysym(xkb_state_key_get_one_sym(KEYBOARD->m_xkbSymState, KEYCODE));
        if (!arg.empty())
            return arg;
    }

    return "";
}

bool shouldSelectWorkspaceFromKey(const IKeyboard::SKeyEvent& event) {
    if (auto* const OV = activeOverview(); OV && OV->isSwiping())
        return false;

    const auto arg = workspaceArgForKeyEvent(event);
    if (arg.empty())
        return false;

    const auto mode = Hyprexpo::numberKeyModeFromString(g_pNumberKeyModeConfig ? g_pNumberKeyModeConfig->value() : HyprexpoConfig::NUMBER_KEY_MODE_DEFAULT);

    if (mode == Hyprexpo::ENumberKeyMode::Passthrough)
        return false;

    if (mode == Hyprexpo::ENumberKeyMode::Index) {
        const int visibleIndex = Hyprexpo::numberKeyToVisibleIndex(arg[0] - '0');
        if (visibleIndex >= 0) {
            if (auto* const OV = activeOverview(); OV && OV->onKbSelectToken(visibleIndex))
                closeOverviewsSelecting(OV);
        }
        return true;
    }

    // Zero was not handled by the legacy raw-key path. Preserve that behavior
    // in workspace mode while still allowing zero to select tile 10 in index mode.
    if (arg == "0")
        return false;

    return changeToSingleDigitWorkspace(arg).success;
}

static int luaDispatchResult(lua_State* L, const char* name, const SDispatchResult& result) {
    if (result.success)
        return 0;

    return luaL_error(L, "%s: %s", name, result.error.empty() ? "dispatcher failed" : result.error.c_str());
}

static std::string luaStringArg(lua_State* L, int index, const char* name, const char* defaultValue = "") {
    if (lua_gettop(L) < index || lua_isnil(L, index))
        return defaultValue;

    if (lua_type(L, index) == LUA_TSTRING)
        return lua_tostring(L, index);

    luaL_error(L, "%s: argument %d must be a string", name, index);
    return defaultValue;
}

static std::string luaIntegerArg(lua_State* L, int index, const char* name) {
    if (lua_gettop(L) < index || lua_isnil(L, index)) {
        luaL_error(L, "%s: argument %d must be an integer", name, index);
        return "";
    }

    if (lua_type(L, index) == LUA_TNUMBER) {
        if (!lua_isinteger(L, index)) {
            luaL_error(L, "%s: argument %d must be an integer, not a fractional number", name, index);
            return "";
        }
        return std::to_string(lua_tointeger(L, index));
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        int parsed = 0;
        const std::string value = lua_tostring(L, index);
        if (!parseStrictInteger(value, parsed)) {
            luaL_error(L, "%s: argument %d must be an integer string", name, index);
            return "";
        }
        return trimString(value);
    }

    luaL_error(L, "%s: argument %d must be an integer", name, index);
    return "";
}

static std::string luaTableStringField(lua_State* L, const char* name, const char* field, const char* defaultValue = nullptr) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (defaultValue)
            return defaultValue;
        luaL_error(L, "%s: field '%s' must be a string", name, field);
        return "";
    }

    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be a string", name, field);
        return "";
    }

    std::string value = lua_tostring(L, -1);
    lua_pop(L, 1);
    return value;
}

static int luaTableIntegerField(lua_State* L, const char* name, const char* field) {
    lua_getfield(L, 1, field);
    if (!lua_isinteger(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be an integer", name, field);
        return 0;
    }

    const int value = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return value;
}

static float luaTableFloatField(lua_State* L, const char* name, const char* field, float defaultValue) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return defaultValue;
    }

    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be a number", name, field);
        return defaultValue;
    }

    const float value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool luaTableBoolField(lua_State* L, const char* name, const char* field, bool defaultValue) {
    lua_getfield(L, 1, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return defaultValue;
    }

    if (lua_type(L, -1) != LUA_TBOOLEAN) {
        lua_pop(L, 1);
        luaL_error(L, "%s: field '%s' must be a boolean", name, field);
        return defaultValue;
    }

    const bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static int luaExpo(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.expo", onExpoDispatcher(luaStringArg(L, 1, "hyprexpo.expo", "toggle")));
}

static int luaKbFocus(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_focus", onKbFocusDispatcher(luaStringArg(L, 1, "hyprexpo.kb_focus")));
}

static int luaKbConfirm(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_confirm", onKbConfirmDispatcher(""));
}

static int luaKbSelectNumber(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_selectn", onKbSelectNumberDispatcher(luaIntegerArg(L, 1, "hyprexpo.kb_selectn")));
}

static int luaKbSelectToken(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_select", onKbSelectTokenDispatcher(luaStringArg(L, 1, "hyprexpo.kb_select")));
}

static int luaKbSelectIndex(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.kb_selecti", onKbSelectIndexDispatcher(luaIntegerArg(L, 1, "hyprexpo.kb_selecti")));
}

static int luaMoveWindow(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.move_window", onMovePreviewWindowDispatcher(luaStringArg(L, 1, "hyprexpo.move_window")));
}

static int luaScrollingDebug(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.scrolling_debug", onScrollingDebugDispatcher(luaStringArg(L, 1, "hyprexpo.scrolling_debug")));
}

static int luaScrollingInputTest(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.scrolling_input_test", onScrollingInputTestDispatcher(luaStringArg(L, 1, "hyprexpo.scrolling_input_test")));
}

static int luaScrollingMutationTest(lua_State* L) {
    return luaDispatchResult(L, "hyprexpo.scrolling_mutation_test", onScrollingMutationTestDispatcher(luaStringArg(L, 1, "hyprexpo.scrolling_mutation_test")));
}

static int luaGesture(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    const int         fingers        = luaTableIntegerField(L, "hyprexpo.gesture", "fingers");
    const std::string direction      = luaTableStringField(L, "hyprexpo.gesture", "direction");
    const std::string action         = luaTableStringField(L, "hyprexpo.gesture", "action", "expo");
    const std::string mods           = luaTableStringField(L, "hyprexpo.gesture", "mods", "");
    const float       scale          = luaTableFloatField(L, "hyprexpo.gesture", "scale", 1.0F);
    const bool        disableInhibit = luaTableBoolField(L, "hyprexpo.gesture", "disable_inhibit", false);

    return luaDispatchResult(L, "hyprexpo.gesture", registerExpoGesture(fingers, direction, action, mods, scale, disableInhibit));
}

static SDispatchResult onExpoDispatcher(std::string arg) {
    const auto COMMAND = Hyprexpo::parseExpoCommand(lowerString(trimString(arg)));

    arg                     = COMMAND.command;
    const bool ALL_MONITORS = COMMAND.allMonitors;

    if (auto* const OV = activeOverview(); OV && OV->isSwiping())
        return {.success = false, .error = "already swiping"};

    if (isSingleDigitWorkspaceArg(arg))
        return changeToSingleDigitWorkspace(arg);

    if (arg == "select") {
        if (auto* const OV = overviewForGlobalPoint(g_pInputManager->getMouseCoordsInternal())) {
            if (OV->selectHoveredWorkspace())
                closeOverviewsSelecting(OV);
        }
        return {};
    }

    if (arg == "bring") {
        if (auto* const OV = overviewForGlobalPoint(g_pInputManager->getMouseCoordsInternal())) {
            if (!OV->selectHoveredWorkspace())
                return {.success = false, .error = "no workspace under pointer"};
            const auto DESTINATION = OV->monitor();
            const auto result = bringWindowFromWorkspace(OV->selectedWorkspaceID(), DESTINATION);
            closeOverviews(false);
            return result;
        }
        return {};
    }

    if (arg == "toggle") {
        if (overviewOpen())
            closeOverviews(false);
        else
            return openOverviews(ALL_MONITORS);
        return {};
    }

    if (arg == "cancel") {
        closeOverviews(false);
        return {};
    }

    if (arg == "off" || arg == "close" || arg == "disable") {
        closeOverviews(false);
        return {};
    }

    if (ALL_MONITORS && (arg == "on" || arg == "enable")) {
        return openOverviews(true);
    }

    if (overviewOpen())
        return {};

    return openOverviews(ALL_MONITORS);
}

static SDispatchResult registerExpoGesture(int fingerCount, const std::string& directionName, const std::string& action, const std::string& mods, float deltaScale, bool disableInhibit) {
    // PLUGIN_EXIT reloads Lua config before dlclose, so block every registration path.
    if (g_unloading || g_gestureRegistrationDisabled)
        return {};

    if (fingerCount <= 1 || fingerCount >= 10)
        return {.success = false, .error = std::format("invalid fingers '{}', expected 2-9", fingerCount)};

    const auto direction = g_pTrackpadGestures->dirForString(directionName);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("invalid direction '{}'", directionName)};

    Input::ModifierMask modMask = Input::HL_MODIFIER_NONE;
    if (!mods.empty())
        modMask = Keybinds::modMaskFromString(mods);

    deltaScale = std::clamp(deltaScale, 0.1F, 10.F);

    std::expected<void, std::string> result;
    if (action == "expo")
        result = g_pTrackpadGestures->addGesture(makeUnique<CExpoGesture>(EExpoGestureAction::Expo), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "cancel")
        result = g_pTrackpadGestures->addGesture(makeUnique<CExpoGesture>(EExpoGestureAction::Cancel), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "unset")
        result = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    else
        return {.success = false, .error = std::format("invalid action '{}', expected expo|cancel|unset", action)};

    if (!result)
        return {.success = false, .error = result.error()};

    return {};
}

void disableExpoGestureRegistration() {
    g_gestureRegistrationDisabled = true;
}

static void reportGestureConfigError(const std::string& error) {
    Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] {}", error);
    HyprlandAPI::addNotification(PHANDLE, "[hyprexpo] " + error, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

void syncExpoGestureFromConfig() {
    if (g_unloading || g_gestureRegistrationDisabled)
        return;

    const int         FINGERS = (int)CompatHyprlandAPI::intValue("plugin:hyprexpo:gesture_fingers");
    const std::string DIR     = CompatHyprlandAPI::stringValue("plugin:hyprexpo:gesture_direction");

    const auto DECISION = Hyprexpo::evaluateGestureSync({
        .fingers        = FINGERS,
        .direction      = DIR,
        .directionValid = g_pTrackpadGestures && g_pTrackpadGestures->dirForString(DIR) != TRACKPAD_GESTURE_DIR_NONE,
    });

    if (!DECISION.error.empty()) {
        reportGestureConfigError(DECISION.error);
        return;
    }

    if (!DECISION.registerGesture)
        return;

    if (const auto RESULT = registerExpoGesture(FINGERS, DIR, "expo", "", 1.F, false); !RESULT.success)
        reportGestureConfigError(RESULT.error);
}

static SDispatchResult onKbFocusDispatcher(std::string arg) {
    auto* const OV = activeOverview();
    if (!OV)
        return {};

    if (arg == "left" || arg == "right" || arg == "up" || arg == "down") {
        OV->onKbMoveFocus(arg);
        return {};
    }

    return {.success = false, .error = "invalid arg. expected left|right|up|down"};
}

static SDispatchResult onKbConfirmDispatcher(std::string arg) {
    auto* const OV = activeOverview();
    if (!OV)
        return {};

    if (OV->onKbConfirm())
        closeOverviewsSelecting(OV);
    return {};
}

static SDispatchResult onKbSelectNumberDispatcher(std::string arg) {
    auto* const OV = activeOverview();
    if (!OV)
        return {};

    arg = trimString(arg);
    if (arg.empty())
        return {.success = false, .error = "missing number"};

    int num = -1;
    if (!parseStrictInteger(arg, num))
        return {.success = false, .error = "invalid number"};

    if (OV->onKbSelectNumber(num))
        closeOverviewsSelecting(OV);
    return {};
}

static SDispatchResult onKbSelectTokenDispatcher(std::string arg) {
    auto* const OV = activeOverview();
    if (!OV)
        return {};
    arg = trimString(arg);
    if (!OV->selectVisibleToken(arg))
        return {.success = false, .error = "no visible workspace for token"};
    closeOverviewsSelecting(OV);
    return {};
}

static SDispatchResult onKbSelectIndexDispatcher(std::string arg) {
    auto* const OV = activeOverview();
    if (!OV)
        return {};
    arg = trimString(arg);
    int idx = -1;
    if (!parseStrictInteger(arg, idx))
        idx = -1;
    if (idx <= 0)
        return {.success = false, .error = "invalid index (expected >= 1)"};
    // convert to 0-based visible index
    if (OV->onKbSelectToken(idx - 1))
        closeOverviewsSelecting(OV);
    return {};
}

static SDispatchResult onMovePreviewWindowDispatcher(std::string arg) {
    auto* const OV = activeOverview();
    if (!OV)
        return {.success = false, .error = "overview is not open"};

    std::istringstream stream{trimString(arg)};
    size_t             source = 0;
    size_t             target = 0;
    if (!(stream >> source >> target))
        return {.success = false, .error = "expected 1-based source and target visible tile indices, optionally followed by a window address"};

    if (source == 0 || target == 0)
        return {.success = false, .error = "indices are 1-based"};

    std::string windowAddress;
    stream >> windowAddress;

    PHLWINDOW window;
    if (!windowAddress.empty()) {
        if (!windowAddress.starts_with("address:"))
            windowAddress = "address:" + windowAddress;

        window = Desktop::viewState()->query().selector(windowAddress).runWindow();
        if (!window)
            return {.success = false, .error = "window address did not match a mapped window"};
    }

    if (!OV->moveWindowBetweenVisibleIndices(source - 1, target - 1, window))
        return {.success = false, .error = "failed to move preview window"};

    return {};
}

static SDispatchResult onScrollingDebugDispatcher(std::string arg) {
    const auto emission = Hyprexpo::Scrolling::buildReadDiagnostic(arg);
    if (!emission.validRequest)
        return {.success = false, .error = emission.error};

    Log::logger->log(Log::INFO, Log::logFnName(), "HYPREXPO_SCROLLING_DIAGNOSTIC {}", emission.json);
    if (!emission.success)
        return {.success = false, .error = emission.error};
    return {};
}

static SDispatchResult onScrollingInputTestDispatcher(std::string arg) {
    if (g_unloading)
        return {.success = false, .error = "plugin is unloading"};
    if (CompatHyprlandAPI::intValue("plugin:hyprexpo:scrolling_input_debug") == 0)
        return {.success = false, .error = "scrolling input diagnostics are disabled"};
    auto* const OV = activeOverview();
    if (!OV)
        return {.success = false, .error = "overview is not open"};

    const auto emission = OV->injectScrollingInput(arg);
    if (!emission)
        return {.success = false, .error = emission.error()};
    Log::logger->log(Log::INFO, Log::logFnName(), "HYPREXPO_SCROLLING_INPUT {}", *emission);
    return {};
}

static SDispatchResult onScrollingMutationTestDispatcher(std::string arg) {
    if (g_unloading)
        return {.success = false, .error = "plugin is unloading"};
    if (CompatHyprlandAPI::intValue("plugin:hyprexpo:scrolling_input_debug") == 0)
        return {.success = false, .error = "scrolling mutation diagnostics are disabled"};
    auto* const OV = activeOverview();
    if (!OV)
        return {.success = false, .error = "overview is not open"};

    const auto result = Hyprexpo::Scrolling::runNativeMutationTest(arg, OV->sessionGeneration());
    if (!result)
        return {.success = false, .error = result.error()};
    const auto diagnostic = Hyprexpo::Scrolling::mutationDiagnosticJson(*result);
    Log::logger->log(result->outcome == Hyprexpo::Scrolling::EMutationOutcome::RollbackFailed ? Log::ERR : Log::INFO, Log::logFnName(),
                     "HYPREXPO_SCROLLING_MUTATION {}", diagnostic);
    if (result->outcome == Hyprexpo::Scrolling::EMutationOutcome::RollbackFailed || result->outcome == Hyprexpo::Scrolling::EMutationOutcome::Rejected)
        return {.success = false, .error = result->error};
    OV->onConfigReload();
    return {};
}

SP<Config::Values::CStringValue> createCancelKeyConfig() {
    g_pCancelKeyConfig = makeShared<Config::Values::CStringValue>("plugin:hyprexpo:cancel_key", "cancel key", HyprexpoConfig::CANCEL_KEY_DEFAULT);
    return g_pCancelKeyConfig;
}

SP<Config::Values::CStringValue> createNumberKeyModeConfig() {
    g_pNumberKeyModeConfig =
        makeShared<Config::Values::CStringValue>("plugin:hyprexpo:number_key_mode", "raw number-key handling: workspace, index, or passthrough", HyprexpoConfig::NUMBER_KEY_MODE_DEFAULT);
    return g_pNumberKeyModeConfig;
}

void resetDispatcherRuntime() {
    g_unloading = true;
    g_pCancelKeyConfig.reset();
    g_pNumberKeyModeConfig.reset();
}

bool isRenderingOverview() {
    return renderingOverview;
}

void registerHyprexpoDispatchers() {
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:expo", onExpoDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_focus", onKbFocusDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_confirm", onKbConfirmDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_selectn", onKbSelectNumberDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_select", onKbSelectTokenDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:kb_selecti", onKbSelectIndexDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:move_window", onMovePreviewWindowDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:scrolling_debug", onScrollingDebugDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:scrolling_input_test", onScrollingInputTestDispatcher);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprexpo:scrolling_mutation_test", onScrollingMutationTestDispatcher);

    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "expo", luaExpo);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_focus", luaKbFocus);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_confirm", luaKbConfirm);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_selectn", luaKbSelectNumber);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_select", luaKbSelectToken);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "kb_selecti", luaKbSelectIndex);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "move_window", luaMoveWindow);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "scrolling_debug", luaScrollingDebug);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "scrolling_input_test", luaScrollingInputTest);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "scrolling_mutation_test", luaScrollingMutationTest);
    HyprlandAPI::addLuaFunction(PHANDLE, "hyprexpo", "gesture", luaGesture);
}
