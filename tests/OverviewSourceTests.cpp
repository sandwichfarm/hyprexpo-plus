#include <fstream>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
    if (condition)
        return;

    ++failures;
    std::cerr << "FAIL: " << label << '\n';
}

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file)
        return {};

    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string extractFunction(const std::string& source, const std::string& signature) {
    for (auto start = source.find(signature); start != std::string::npos; start = source.find(signature, start + 1)) {
        const auto open = source.find('{', start);
        if (open == std::string::npos)
            return {};

        // Ignore forward declarations.
        const auto semicolon = source.find(';', start);
        if (semicolon != std::string::npos && semicolon < open)
            continue;

        int depth = 0;
        for (size_t i = open; i < source.size(); ++i) {
            if (source[i] == '{')
                ++depth;
            else if (source[i] == '}') {
                --depth;
                if (depth == 0)
                    return source.substr(start, i - start + 1);
            }
        }

        return {};
    }

    return {};
}

size_t countOccurrences(const std::string& source, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = source.find(needle); pos != std::string::npos; pos = source.find(needle, pos + needle.size()))
        ++count;
    return count;
}

void expectContains(const std::string& source, const std::string& token, const std::string& label) {
    expect(source.find(token) != std::string::npos, label);
}

void expectAbsent(const std::string& source, const std::string& token, const std::string& label) {
    expect(source.find(token) == std::string::npos, label);
}

void expectOrder(const std::string& source, const std::string& first, const std::string& second, const std::string& label) {
    const auto firstPosition = source.find(first);
    const auto secondPosition = source.find(second);
    expect(firstPosition != std::string::npos && secondPosition != std::string::npos && firstPosition < secondPosition, label);
}

void expectLastOrder(const std::string& source, const std::string& first, const std::string& second, const std::string& label) {
    const auto firstPosition  = source.find(first);
    const auto secondPosition = source.rfind(second);
    expect(firstPosition != std::string::npos && secondPosition != std::string::npos && firstPosition < secondPosition, label);
}

}

int main() {
    const auto lifecycleSource = readFile("src/main.cpp");
    expect(lifecycleSource.find("m_events.monitor.removed.listen") != std::string::npos &&
               lifecycleSource.find("destroyOverview(OV);") != std::string::npos,
           "disconnecting an output unregisters its overview without waiting for a frame on that output");
    const auto source = readFile("src/Overview.cpp");
    const auto overviewHeader = readFile("src/Overview.hpp");
    expect(!source.empty(), "src/Overview.cpp can be read from repo root");

    const auto function = extractFunction(source, "void removeOverview(");
    expect(!function.empty(), "removeOverview function exists");

    const auto lockPos     = function.find("const auto MON = OV->monitor();");
    const auto resetPos    = function.find("destroyOverview(OV);");
    const auto damagePos   = function.find("g_pHyprRenderer->damageMonitor(MON);");
    const auto schedulePos = function.find("MON->scheduleFrame();");

    expect(lockPos != std::string::npos, "removeOverview captures monitor before teardown");
    expect(resetPos != std::string::npos, "removeOverview destroys active overview");
    expect(damagePos != std::string::npos, "removeOverview damages monitor after teardown");
    expect(schedulePos != std::string::npos, "removeOverview schedules a frame after teardown");
    expect(lockPos < resetPos, "monitor is captured before overview reset");
    expect(resetPos < damagePos, "monitor damage happens after overview reset");
    expect(damagePos < schedulePos, "frame scheduling follows monitor damage");

    const auto mainSource = readFile("src/main.cpp");
    expect(!mainSource.empty(), "src/main.cpp can be read from repo root");
    expect(mainSource.find("const Time::steady_tp& now") != std::string::npos, "render hook uses the Hyprland 0.56 time-point ABI");
    expect(mainSource.find("_ZN7Monitor8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE") != std::string::npos,
           "damage hook uses the Hyprland 0.56 namespaced monitor symbol");
    expect(mainSource.find("_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE") == std::string::npos,
           "damage hook no longer uses the pre-0.56 monitor symbol");

    const auto configSource = readFile("src/PluginConfig.cpp");
    expect(configSource.find("plugin:hyprexpo:drag_drop_enable") != std::string::npos,
           "drag/drop enable configuration is registered");
    expect(source.find("plugin:hyprexpo:drag_drop_enable") != std::string::npos,
           "drag/drop enable configuration has a compatibility default");
    expect(source.find("if (**PDRAGDROPENABLE && TARGET)\n                TARGET->beginWindowDrag();") != std::string::npos,
           "drag/drop enable configuration gates drag start");
    expect(source.find("if (**PDRAGDROPENABLE && SOURCE)") != std::string::npos && source.find("SOURCE->finishWindowDrag()") != std::string::npos,
           "drag/drop enable configuration gates drag completion");

    const auto dispatchersSource = readFile("src/Dispatchers.cpp");
    expect(!dispatchersSource.empty(), "src/Dispatchers.cpp can be read from repo root");
    const auto gestureHeader = readFile("src/ExpoGesture.hpp");
    expect(!gestureHeader.empty(), "src/ExpoGesture.hpp can be read from repo root");
    const auto gestureSource = readFile("src/ExpoGesture.cpp");
    expect(!gestureSource.empty(), "src/ExpoGesture.cpp can be read from repo root");
    expect(!overviewHeader.empty(), "src/Overview.hpp can be read from repo root");
    const auto expoDispatcher = extractFunction(dispatchersSource, "static SDispatchResult onExpoDispatcher(std::string arg) {");
    expect(!expoDispatcher.empty(), "expo dispatcher function exists");

    const auto numberKeyDispatcher = extractFunction(dispatchersSource, "static SDispatchResult changeToSingleDigitWorkspace(const std::string& arg) {");
    expect(!numberKeyDispatcher.empty(), "number-key dispatcher function exists");
    expect(numberKeyDispatcher.find("OV->selectWorkspaceByID(workspaceID)") != std::string::npos,
           "workspace-mode raw number keys preserve workspace-ID selection");

    const auto rawNumberSelection = extractFunction(dispatchersSource, "bool shouldSelectWorkspaceFromKey(const IKeyboard::SKeyEvent& event) {");
    expect(!rawNumberSelection.empty(), "raw number-key selection function exists");
    expect(rawNumberSelection.find("g_pNumberKeyModeConfig") != std::string::npos,
           "raw number-key handling reads the dedicated retained mode");
    expect(rawNumberSelection.find("g_pNumberKeyModeConfig->value()") != std::string::npos,
           "raw number-key handling reads the retained V2 string value");
    expect(rawNumberSelection.find("HyprlandAPI::getConfigValue") == std::string::npos,
           "raw number-key handling does not dereference the deprecated config API");
    expect(configSource.find("addConfigValue(createNumberKeyModeConfig())") != std::string::npos,
           "number-key mode registration retains the V2 config value");
    expect(dispatchersSource.find("g_pNumberKeyModeConfig.reset()") != std::string::npos,
           "number-key mode releases its retained V2 config value during teardown");
    const auto passthroughMode = rawNumberSelection.find("ENumberKeyMode::Passthrough");
    const auto indexMode       = rawNumberSelection.find("ENumberKeyMode::Index", passthroughMode);
    const auto workspaceMode   = rawNumberSelection.find("return changeToSingleDigitWorkspace(arg).success;", indexMode);
    expect(passthroughMode != std::string::npos && indexMode != std::string::npos &&
               rawNumberSelection.substr(passthroughMode, indexMode - passthroughMode).find("return false;") != std::string::npos,
           "passthrough mode leaves raw number keys uncancelled");
    expect(indexMode != std::string::npos && workspaceMode != std::string::npos &&
               rawNumberSelection.substr(indexMode, workspaceMode - indexMode).find("OV->onKbSelectToken(visibleIndex)") != std::string::npos &&
               rawNumberSelection.substr(indexMode, workspaceMode - indexMode).find("return true;") != std::string::npos,
           "index mode selects from active-overview visible tile positions");
    expect(workspaceMode != std::string::npos,
           "workspace mode retains the legacy global-workspace fallback");
    expect(rawNumberSelection.find("if (arg == \"0\")") != std::string::npos,
           "workspace mode preserves legacy zero-key passthrough");
    expect(mainSource.find("if (shouldSelectWorkspaceFromKey(event))\n            info.cancelled = true;") != std::string::npos,
           "raw key events are cancelled only when the mode-specific handler consumes them");

    const auto interactionSource = readFile("src/OverviewInteraction.cpp");
    expect(!interactionSource.empty(), "src/OverviewInteraction.cpp can be read from repo root");

    const auto enterSubmap = extractFunction(interactionSource, "void enterOverviewSubmap(bool& submapActive) {");
    expect(!enterSubmap.empty(), "keyboard navigation submap entry function exists");
    const auto captureSubmapPos = enterSubmap.find("g_previousSubmap = Keybinds::mgr()->currentSubmap();");
    const auto firstSubmapRef   = enterSubmap.find("if (g_submapRefs++ == 0)");
    const auto shareSubmapPos   = captureSubmapPos;
    const auto enterSubmapPos   = enterSubmap.find("Config::Actions::setSubmap(\"hyprexpo\");");
    const auto activateGuardPos = enterSubmap.find("submapActive = true;");
    expect(captureSubmapPos != std::string::npos, "submap entry captures the exact active Hyprland submap");
    expect(firstSubmapRef != std::string::npos && firstSubmapRef < captureSubmapPos,
           "only the first overview in a multi-monitor session captures the active submap");
    expect(shareSubmapPos != std::string::npos && captureSubmapPos == shareSubmapPos,
           "the first overview publishes the captured submap for the whole overview session");
    expect(enterSubmapPos != std::string::npos, "submap entry switches to the hyprexpo navigation submap");
    expect(activateGuardPos != std::string::npos, "submap entry marks the navigation submap active");
    expect(captureSubmapPos < enterSubmapPos, "active submap capture happens before entering hyprexpo");
    expect(enterSubmapPos < activateGuardPos, "submap-active guard is set only after entering hyprexpo");

    const auto resetSubmap = extractFunction(interactionSource, "void leaveOverviewSubmap(bool& submapActive) {");
    expect(!resetSubmap.empty(), "keyboard navigation submap reset function exists");
    const auto lastSubmapRef    = resetSubmap.find("if (--g_submapRefs <= 0)");
    const auto sharedRestorePos = resetSubmap.find("Config::Actions::setSubmap(g_previousSubmap);");
    const auto restoreSubmapPos = resetSubmap.find("Config::Actions::setSubmap(g_previousSubmap);");
    const auto clearGuardPos    = resetSubmap.find("submapActive = false;");
    expect(lastSubmapRef != std::string::npos && sharedRestorePos != std::string::npos && lastSubmapRef < sharedRestorePos,
           "only the last overview restores the session-global captured submap");
    expect(restoreSubmapPos != std::string::npos, "submap reset restores the exact captured submap");
    expect(resetSubmap.find("Config::Actions::setSubmap(\"reset\");") == std::string::npos,
           "submap reset never hardcodes Hyprland's default submap");
    expect(clearGuardPos != std::string::npos, "submap reset clears the active guard");
    expect(sharedRestorePos == restoreSubmapPos && restoreSubmapPos < clearGuardPos,
           "shared captured submap restoration happens before clearing the active guard");

    const auto numberSelection = extractFunction(interactionSource, "bool COverview::onKbSelectNumber(int num) {");
    expect(!numberSelection.empty(), "workspace-number dispatcher selection function exists");
    expect(numberSelection.find("selectWorkspaceByID(num)") != std::string::npos,
           "kb_selectn remains workspace-ID based");
    expect(numberSelection.find("number_key_mode") == std::string::npos && numberSelection.find("numberKeyToVisibleIndex") == std::string::npos,
           "kb_selectn semantics do not depend on the raw number-key mode");

    const auto workspaceMoveHandler = extractFunction(interactionSource, "void COverview::onWindowMoveToWorkspace(");
    expect(!workspaceMoveHandler.empty(), "workspace-move close handler exists");
    const auto monitorRelevancePos = workspaceMoveHandler.find("const bool movedOnOverviewMonitor");
    const auto classifierPos       = workspaceMoveHandler.find("Hyprexpo::shouldAbortOverviewCloseForWorkspaceMove((window->m_state & Desktop::View::WINDOW_STATE_PINNED) != Desktop::View::WINDOW_STATE_NONE, movedOnOverviewMonitor)");
    const auto abortFlagPos        = workspaceMoveHandler.find("externalWorkspaceMoveDuringClose = true;");
    const auto workspaceDamagePos  = workspaceMoveHandler.find("damage();", abortFlagPos);
    const auto scheduleFramePos    = workspaceMoveHandler.find("monitor->scheduleFrame();", workspaceDamagePos);
    expect(monitorRelevancePos != std::string::npos && classifierPos != std::string::npos && monitorRelevancePos < classifierPos,
           "workspace-move handler classifies pin state after computing monitor relevance");
    expect(classifierPos != std::string::npos && abortFlagPos != std::string::npos && classifierPos < abortFlagPos,
           "workspace-move handler rejects pinned events before mutating close state");
    expect(abortFlagPos != std::string::npos && workspaceDamagePos != std::string::npos && scheduleFramePos != std::string::npos && abortFlagPos < workspaceDamagePos && workspaceDamagePos < scheduleFramePos,
           "accepted external moves retain flag, damage, and frame scheduling in order");

    const auto numberDispatcher = extractFunction(dispatchersSource, "static SDispatchResult onKbSelectNumberDispatcher(std::string arg) {");
    const auto indexDispatcher  = extractFunction(dispatchersSource, "static SDispatchResult onKbSelectIndexDispatcher(std::string arg) {");
    expect(numberDispatcher.find("if (OV->onKbSelectNumber(num))") != std::string::npos && numberDispatcher.find("closeOverviewsSelecting(OV);") != std::string::npos,
           "kb_selectn closes every overview only after successful workspace-number selection");
    expect(indexDispatcher.find("if (OV->onKbSelectToken(idx - 1))") != std::string::npos && indexDispatcher.find("closeOverviewsSelecting(OV);") != std::string::npos,
           "kb_selecti closes every overview only after successful visible-index selection");

    const auto toggleStart = expoDispatcher.find("if (arg == \"toggle\")");
    const auto cancelStart = expoDispatcher.find("if (arg == \"cancel\")", toggleStart);
    const auto toggleBlock = toggleStart == std::string::npos || cancelStart == std::string::npos ? std::string{} : expoDispatcher.substr(toggleStart, cancelStart - toggleStart);
    expect(toggleBlock.find("closeOverviews(false);") != std::string::npos, "plain toggle close does not select a fallback workspace");

    const auto offStart = expoDispatcher.find("if (arg == \"off\" || arg == \"close\" || arg == \"disable\")");
    const auto offEnd   = expoDispatcher.find("\n    if (overviewOpen())\n        return {};", offStart);
    const auto offBlock = offStart == std::string::npos || offEnd == std::string::npos ? std::string{} : expoDispatcher.substr(offStart, offEnd - offStart);
    expect(offBlock.find("closeOverviews(false);") != std::string::npos, "plain off and close commands do not select a fallback workspace");

    const auto enableAllStart = expoDispatcher.find("if (ALL_MONITORS && (arg == \"on\" || arg == \"enable\"))");
    const auto alreadyOpen    = expoDispatcher.find("if (overviewOpen())", enableAllStart);
    expect(enableAllStart != std::string::npos && alreadyOpen != std::string::npos && enableAllStart < alreadyOpen,
           "on all and enable all fill missing monitor entries before the already-open early return");
    expect(toggleBlock.find("openOverviews(ALL_MONITORS);") != std::string::npos && toggleBlock.find("closeOverviews(false);") != std::string::npos,
           "toggle all preserves close-when-any-open semantics");

    const auto selectStart = expoDispatcher.find("if (arg == \"select\")");
    const auto bringStart  = expoDispatcher.find("if (arg == \"bring\")", selectStart);
    const auto toggleAfterBring = expoDispatcher.find("if (arg == \"toggle\")", bringStart);
    const auto selectBlock = selectStart == std::string::npos || bringStart == std::string::npos ? std::string{} : expoDispatcher.substr(selectStart, bringStart - selectStart);
    const auto bringBlock  = bringStart == std::string::npos || toggleAfterBring == std::string::npos ? std::string{} : expoDispatcher.substr(bringStart, toggleAfterBring - bringStart);
    expect(selectBlock.find("overviewForGlobalPoint(") != std::string::npos && selectBlock.find("activeOverview()") == std::string::npos,
           "pointer select resolves the overview under the cursor instead of keyboard ownership");
    expect(bringBlock.find("overviewForGlobalPoint(") != std::string::npos && bringBlock.find("activeOverview()") == std::string::npos,
           "pointer bring resolves the overview under the cursor instead of keyboard ownership");
    expect(bringBlock.find("const auto DESTINATION = OV->monitor();") != std::string::npos &&
               bringBlock.find("bringWindowFromWorkspace(OV->selectedWorkspaceID(), DESTINATION)") != std::string::npos,
           "bring passes the cursor overview's monitor as the explicit destination");

    const auto bringWindow = extractFunction(dispatchersSource, "static SDispatchResult bringWindowFromWorkspace(int64_t sourceWorkspaceID, const PHLMONITOR& destinationMonitor) {");
    expect(!bringWindow.empty() && bringWindow.find("destinationMonitor->m_activeWorkspace") != std::string::npos,
           "bring moves onto the explicitly chosen cursor monitor workspace");
    expect(bringWindow.find("focusState ? focusState->monitor()") == std::string::npos && bringWindow.find("State::monitorState()->query()") == std::string::npos,
           "bring never recomputes its destination from keyboard focus or cursor fallback");

    const auto activeFunction = extractFunction(source, "IOverviewSession* activeOverview() {");
    expect(activeFunction.find("g_keyboardOverviewMonitor.lock()") != std::string::npos && activeFunction.find("overviewForMonitor(KEYBOARD)") != std::string::npos,
           "active overview honors a persistent explicit keyboard owner before compositor focus");
    expect(source.find("bool overviewRegistered(const IOverviewSession* overview)") != std::string::npos,
           "registry exposes exact overview liveness for delayed callbacks");

    const auto moveFocus = extractFunction(interactionSource, "bool COverview::moveFocus(int dx, int dy) {");
    const auto kbMove    = extractFunction(interactionSource, "bool COverview::onKbMoveFocus(const std::string& dir) {");
    expect(!moveFocus.empty() && moveFocus.find("return true;") != std::string::npos && moveFocus.find("return false;") != std::string::npos,
           "local focus movement reports whether it found a tile");
    expect(kbMove.find("moveOverviewFocusAcrossMonitors(this") != std::string::npos,
           "cross-monitor focus runs only after local movement fails");
    expect(source.find("Hyprexpo::selectDirectionalTile(") != std::string::npos && source.find("g_keyboardOverviewMonitor = TARGET->monitor();") != std::string::npos,
           "cross-monitor focus uses pure geometry and persists destination keyboard ownership");

    expect(source.find("void closeOverviewsSelecting(IOverviewSession* selecting)") != std::string::npos &&
               dispatchersSource.find("static void closeOverviewsSelecting(") == std::string::npos,
           "one registry coordinator owns selector and peer closure");
    expect(interactionSource.find("bool COverview::selectHoveredWorkspace()") != std::string::npos,
           "pointer and touch selection can distinguish success from an invalid tile");

    const auto resetDrag = extractFunction(source, "void resetOverviewDrag(");
    expect(!resetDrag.empty(), "registry provides one centralized idempotent drag reset");
    expect(resetDrag.find("transitionOverviewDrag(") != std::string::npos && resetDrag.find("\"left_ptr\"") != std::string::npos && resetDrag.find("damageMonitor") != std::string::npos,
           "central drag reset clears pure state, restores left_ptr, and damages affected live monitors");
    expect(resetDrag.find("liveOverviewMonitorKeys()") != std::string::npos,
           "central reset always supplies current registry liveness when a monitor weak reference has expired");
    const auto destroyOne = extractFunction(source, "void destroyOverview(IOverviewSession* overview) {");
    const auto destroyAll = extractFunction(source, "void destroyAllOverviews() {");
    const auto moveOwnerPos   = destroyOne.find("auto OWNER = std::move(*IT);");
    const auto eraseSlotPos   = destroyOne.find("g_overviews.erase(IT);");
    const auto destroyOwnerPos = destroyOne.find("OWNER.reset();");
    expect(destroyOne.find("resetOverviewDrag(") < moveOwnerPos, "single-overview destruction resets drag before unregistering ownership");
    expect(moveOwnerPos != std::string::npos && eraseSlotPos != std::string::npos && destroyOwnerPos != std::string::npos && moveOwnerPos < eraseSlotPos && eraseSlotPos < destroyOwnerPos,
           "single-overview teardown unregisters the moved owner before its cursor-restoring destructor runs");
    const auto swapRegistryPos = destroyAll.find("OWNERS.swap(g_overviews);");
    const auto clearOwnersPos  = destroyAll.find("OWNERS.clear();");
    expect(destroyAll.find("resetOverviewDrag(") < swapRegistryPos && swapRegistryPos < clearOwnersPos,
           "all-overview teardown empties the registry before any owner destructor runs");

    const auto byAnimVar  = extractFunction(source, "IOverviewSession* overviewForAnimVar(");
    const auto byMonitor  = extractFunction(source, "IOverviewSession* overviewForMonitor(");
    const auto byKey      = extractFunction(source, "IOverviewSession* overviewForMonitorKey(");
    const auto byPoint    = extractFunction(source, "IOverviewSession* overviewForGlobalPoint(");
    expect(byAnimVar.find("if (!OV)") < byAnimVar.find("OV->ownsAnimVar("), "animation-owner lookup skips null registry slots before dereference");
    expect(byMonitor.find("if (!OV)") < byMonitor.find("OV->monitor()"), "monitor lookup skips null registry slots before dereference");
    expect(byKey.find("if (!OV)") < byKey.find("OV->monitor()"), "monitor-key lookup skips null registry slots before dereference");
    expect(byPoint.find("if (!OV)") < byPoint.find("OV->monitor()"), "point lookup skips null registry slots before dereference");
    expect(source.find("if (OV)\n            snapshot.emplace_back(overviewMonitorKey(OV->monitor()), OV->sessionGeneration());") != std::string::npos,
           "safe registry snapshots record only live monitor and generation identities");
    expectContains(extractFunction(source, "void forEachOverview("), "overviewForSession(monitorKey, generation)",
                   "registry iteration rejects replacement sessions created by earlier callbacks");
    expect(interactionSource.find("activeOverview() != this") == std::string::npos,
           "settle timer liveness never depends on whichever overview currently owns keyboard input");

    const auto redrawTimer = extractFunction(interactionSource, "void COverview::redrawDraggedWorkspace(int64_t workspaceID) {");
    expect(redrawTimer.find("[this]") == std::string::npos,
           "settle timer callbacks never capture a raw overview pointer");
    expect(redrawTimer.find("const auto OVERVIEWKEY = overviewMonitorKey(") != std::string::npos && redrawTimer.find("[OVERVIEWKEY, GENERATION]") != std::string::npos &&
               redrawTimer.find("overviewForSession(OVERVIEWKEY, GENERATION)") != std::string::npos,
           "settle timer callbacks re-resolve their overview by monitor and generation on every tick");
    expect(redrawTimer.find("OVERVIEW->redrawSettleTimer.get() != self.get()") != std::string::npos,
           "a timer cannot mutate a replacement overview registered on the same monitor");
    expect(redrawTimer.find("settlingRedrawWorkspaceIDs") != std::string::npos && redrawTimer.find("OVERVIEW->tileForWorkspaceID(workspaceID)") != std::string::npos,
           "every settle tick re-resolves the queued workspace identity to its current tile");

    const auto overviewDestructor = extractFunction(source, "COverview::~COverview() {");
    const auto cancelTimerPos     = overviewDestructor.find("redrawSettleTimer->cancel();");
    const auto restoreCursorPos   = overviewDestructor.find("Pointer::Cursor::overrideController->unsetOverride");
    expect(cancelTimerPos != std::string::npos && restoreCursorPos != std::string::npos && cancelTimerPos < restoreCursorPos,
           "overview teardown cancels and detaches settle timers before cursor restoration can re-enter callbacks");

    const auto headerSource = readFile("src/Overview.hpp");
    expect(headerSource.find("dragStartLocal") == std::string::npos && headerSource.find("dragSourceID") == std::string::npos && headerSource.find("dragWindow") == std::string::npos,
           "per-overview drag ownership fields are removed in favor of the registry session");
    const auto beginDrag  = extractFunction(interactionSource, "void COverview::beginWindowDrag() {");
    const auto updateDrag = extractFunction(interactionSource, "void COverview::updateWindowDrag() {");
    const auto finishDrag = extractFunction(interactionSource, "bool COverview::finishWindowDrag() {");
    expect(beginDrag.find("hitTestGlobalTile(") != std::string::npos && beginDrag.find("EOverviewDragEventType::Press") != std::string::npos,
           "drag press ownership comes from pure global hit testing and the shared coordinator");
    expect(updateDrag.find("sourceMonitorKey") != std::string::npos && updateDrag.find("EOverviewDragEventType::Move") != std::string::npos &&
               updateDrag.find("EOverviewDragEventType::Target") != std::string::npos,
           "only the shared source owner advances move and target transitions");
    expect(finishDrag.find("EOverviewDragEventType::Release") != std::string::npos && finishDrag.find("resetOverviewDrag(") != std::string::npos,
           "release consumes the pure coordinator result and terminates through centralized reset");
    expect(finishDrag.find("TARGETWS->m_monitor.lock() != TARGETMON") != std::string::npos,
           "release rejects a target workspace that does not belong to the target overview monitor");
    expect(countOccurrences(finishDrag, "moveWindowToWorkspace(") == 1,
           "validated release moves the window exactly once");
    const auto moveWindowPos    = finishDrag.find("moveWindowToWorkspace(");
    const auto settleWindowPos  = finishDrag.find("settleWorkspaceMoveAnimation(", moveWindowPos);
    const auto sourceCapturePos = finishDrag.find("SOURCEOV->redrawDraggedWorkspace(SOURCEWORKSPACEID)", settleWindowPos);
    const auto targetCapturePos = finishDrag.find("TARGETOV->redrawDraggedWorkspace(TARGETWORKSPACEID)", settleWindowPos);
    expect(moveWindowPos != std::string::npos && settleWindowPos != std::string::npos && sourceCapturePos != std::string::npos && targetCapturePos != std::string::npos &&
               moveWindowPos < settleWindowPos && settleWindowPos < sourceCapturePos && settleWindowPos < targetCapturePos,
           "source and destination workspace identities queue independent recaptures only after move animations settle");
    expect(headerSource.find("settlingRedrawWorkspaceIDs") != std::string::npos && headerSource.find("settlingRedrawIDs") == std::string::npos,
           "settled recapture state stores workspace identities instead of stale tile indices");
    const auto flushRedraws = extractFunction(interactionSource, "void COverview::flushQueuedRedraws() {");
    expect(flushRedraws.find("MON->scheduleFrame();") != std::string::npos,
           "every completed framebuffer recapture schedules a compositor frame");
    expect(finishDrag.find("\"left_ptr\"") == std::string::npos,
           "release cannot restore the cursor outside centralized reset");

    const auto overviewConstructor = extractFunction(source, "COverview::COverview(");
    expectContains(configSource, "\"plugin:hyprexpo:rows\"", "explicit fixed rows are registered as configuration");
    expectContains(source, "{\"plugin:hyprexpo:rows\", HyprexpoConfig::ROWS_DEFAULT}", "row fallback uses the shared default");
    expectContains(overviewConstructor, "Hyprexpo::computeFixedGridShape(**PCOLUMNS, **PROWS)", "fixed grids resolve both configured dimensions");
    expectContains(overviewConstructor, "images.resize(gridShape.cols * gridShape.rows)", "fixed grids allocate rectangular capacity including empty slots");
    expectContains(overviewConstructor, "**PROWS > 0 ? gridShape.rows : 0", "first-anchor growth distinguishes explicit from inherited rows");
    expectAbsent(overviewConstructor, "SIDE_LENGTH", "construction and captures no longer square a rectangle");
    const auto currentShape = extractFunction(source, "Hyprexpo::SGridShape COverview::currentGridShape(");
    expectContains(currentShape, "return gridShape;", "every geometry consumer sees the resolved grid shape");
    expectAbsent(currentShape, "SIDE_LENGTH", "fixed geometry does not replace resolved rows with columns");
    expect(!overviewConstructor.empty(), "overview constructor exists");
    const auto gapExpansionPos = overviewConstructor.find("Hyprexpo::expandDynamicWorkspaceIDs(");
    const auto dynamicResizePos = overviewConstructor.find("images.resize(visibleWorkspaceIDs.size())");
    expect(gapExpansionPos != std::string::npos, "dynamic workspace enumeration uses the bounded expansion helper");
    expect(dynamicResizePos != std::string::npos && gapExpansionPos < dynamicResizePos, "dynamic expansion is bounded before image allocation");
    expect(overviewConstructor.find("for (int64_t id = minID; id <= maxID; ++id)") == std::string::npos,
           "dynamic workspace enumeration has no unbounded min-to-max fill loop");

    const auto boundsGatePos = overviewConstructor.find("if (!skipEmpty)", overviewConstructor.find("auto [methodCenter, methodStartID]"));
    const auto boundsScanPos = overviewConstructor.find("State::workspaceState()->workspacesCopy()", boundsGatePos);
    expect(boundsGatePos != std::string::npos && boundsScanPos != std::string::npos && boundsGatePos < boundsScanPos,
           "regular workspace bounds are collected only for consecutive traversal");
    expect(boundsScanPos != std::string::npos && overviewConstructor.find("!workspace", boundsScanPos) != std::string::npos,
           "center-current bounds ignore null workspace entries");
    expect(boundsScanPos != std::string::npos && overviewConstructor.find("workspace->m_isSpecialWorkspace", boundsScanPos) != std::string::npos,
           "center-current bounds exclude special workspaces");
    expect(boundsScanPos != std::string::npos && overviewConstructor.find("workspace->m_monitor != PMONITOR", boundsScanPos) != std::string::npos,
           "center-current bounds exclude workspaces owned by other monitors");

    const auto cappedBranchStart = overviewConstructor.find("if (!skipEmpty && maxWorkspace > 0)");
    const auto centerBranchStart = overviewConstructor.find("if (methodCenter) {");
    const auto centerBranchEnd   = overviewConstructor.find("\n    } else {", centerBranchStart);
    const auto cappedBranch      = extractFunction(overviewConstructor, "if (!skipEmpty && maxWorkspace > 0) {");
    const auto centerBranch      = centerBranchStart == std::string::npos || centerBranchEnd == std::string::npos ? std::string{} :
                                                                                                                    overviewConstructor.substr(centerBranchStart, centerBranchEnd - centerBranchStart);
    expect(!cappedBranch.empty(), "capped regular-grid branch exists");
    expect(cappedBranchStart > centerBranchEnd && cappedBranchStart < overviewConstructor.find("if (dynamicGrid)"),
           "the cap filters completed monitor-aware traversal before dynamic-grid overrides");
    expect(cappedBranch.find("image.workspaceID > maxWorkspace") != std::string::npos && cappedBranch.find("image.workspaceID = WORKSPACE_INVALID") != std::string::npos,
           "the cap invalidates only emitted IDs above the maximum");
    expect(overviewConstructor.find("maxWorkspace - tileCount + 1") == std::string::npos,
           "max_workspace no longer back-clamps the configured monitor anchor");
    expect(!centerBranch.empty(), "center-current traversal branch exists");
    const auto anchorPos = overviewConstructor.find("pMonitor->m_activeWorkspace = PWORKSPACESTART;");
    const auto restoreAnchorPos = overviewConstructor.find("pMonitor->m_activeWorkspace = startedOn;", centerBranchEnd);
    expect(anchorPos != std::string::npos && anchorPos < centerBranchStart && restoreAnchorPos < cappedBranchStart,
           "explicit first and capped center selectors share a temporary anchor restored before capture");
    expect(overviewConstructor.find("!methodCenter || (!skipEmpty && maxWorkspace > 0 && methodStartID != startedOn->m_id)") != std::string::npos,
           "capped explicit centers anchor relative traversal without changing legacy skip-empty centering");

    const auto helperPos = centerBranch.find("Hyprexpo::centeredWorkspaceBacktrack(");
    expect(helperPos != std::string::npos && boundsScanPos != std::string::npos && boundsScanPos < centerBranchStart + helperPos,
           "center-current traversal uses the pure backtrack helper after collecting bounds");
    expect(centerBranch.find("for (size_t i = 1; i <= backtrackTarget; ++i)") != std::string::npos,
           "center-current lower scan includes the full helper target");
    expect(centerBranch.find("if (currentID >= firstID)") != std::string::npos && centerBranch.find("if (i > 0 && currentID <= firstID)") != std::string::npos,
           "skip-empty center traversal retains lower and forward wrap guards");

    const auto renderSource = readFile("src/OverviewRender.cpp");
    expect(!renderSource.empty(), "src/OverviewRender.cpp can be read from repo root");
    const auto closeOverview = extractFunction(renderSource, "void COverview::close(bool switchToSelection) {");
    expect(!closeOverview.empty(), "overview close function exists");
    expect(closeOverview.find("resetSubmapIfNeeded();") != std::string::npos,
           "normal overview close restores the captured submap");
    expect(overviewConstructor.find("emptyTilesSelectable = skipEmpty;") != std::string::npos,
           "empty tile creation policy is captured when the overview is built");
    expect(closeOverview.find("TILE.workspaceID != WORKSPACE_INVALID || emptyTilesSelectable") != std::string::npos,
           "all close inputs reject capped padding while preserving skip-empty creation tiles");
    expect(closeOverview.find("Desktop::focusState()->monitor() != MON") != std::string::npos,
           "selecting an already-active grid workspace also focuses its monitor");
    expect(closeOverview.find("if (CHANGE && OLDWS != MON->m_activeWorkspace)") != std::string::npos,
           "focus-only selection cannot apply an OUT animation to its unchanged workspace");
    expect(closeOverview.find("State::workspaceState()->create(NEWID, MON->m_id") != std::string::npos,
           "skip-empty creation is bound to the selected overview monitor");
    expect(closeOverview.find("nextEmptyWorkspaceIDForMonitor(MON)") != std::string::npos,
           "empty-workspace selection resolves against the selected monitor");
    const auto emptySelector = extractFunction(source, "WORKSPACEID nextEmptyWorkspaceIDForMonitor(");
    expectContains(emptySelector, "workspaceIDForMonitor(monitor, \"r+\"", "empty selection excludes foreign monitor bindings and ownership");
    expectContains(emptySelector, "(*workspace)->m_monitor == monitor", "existing empty candidates must belong to the selected monitor");
    expectContains(emptySelector, "workspaces.size() + 1", "empty selection has a finite materialized-workspace search bound");
    const auto selectorHelper = extractFunction(source, "WORKSPACEID workspaceIDForMonitor(");
    expect(selectorHelper.find("CScopeGuard") != std::string::npos && selectorHelper.find("FOCUS->m_focusMonitor = previousMonitor;") != std::string::npos,
           "monitor-relative enumeration restores the focus context on every return");
    expect(overviewConstructor.find("getWorkspaceIDNameFromString(") == std::string::npos && overviewConstructor.find("workspaceIDForMonitor(PMONITOR,") != std::string::npos,
           "simultaneous grids do not enumerate through another monitor's focus context");

    // An anchored grid (workspace_method "<output> first N") lays out
    // max_workspace slots whether or not those workspaces exist. Selecting a
    // tile for one that has never been opened must create it: changeWorkspace()
    // cannot, and the selection used to silently do nothing.
    const auto ensureTileWsPos = closeOverview.find("ensureWorkspaceForTile(SAFEID)");
    const auto changeWsPos     = closeOverview.find("Config::Actions::changeWorkspace(");
    expect(ensureTileWsPos != std::string::npos,
           "selection resolves the tile's workspace through the creating helper");
    expect(changeWsPos != std::string::npos && ensureTileWsPos < changeWsPos,
           "the tile's workspace is created before the monitor is switched to it");
    expect(closeOverview.find("if (TILE.workspaceID != WORKSPACE_INVALID)") != std::string::npos,
           "a WORKSPACE_INVALID tile still falls back to the next empty workspace");

    // overviewDestructor is already extracted above, next to the settle-timer checks.
    expect(!overviewDestructor.empty(), "overview destructor exists");
    expect(overviewDestructor.find("resetSubmapIfNeeded();") != std::string::npos,
           "overview teardown restores the captured submap");

    const auto fullRender = extractFunction(renderSource, "void COverview::fullRender(");
    expect(!fullRender.empty(), "overview fullRender function exists");
    const auto closeFunction = extractFunction(renderSource, "void COverview::close(bool switchToSelection) {");
    expect(closeFunction.find("Config::Actions::changeWorkspace(NEWIDWS)") != std::string::npos,
           "selection uses the resolved workspace owner and updates compositor focus together");
    expect(fullRender.find("Hyprexpo::shouldShowWorkspaceLabel(") != std::string::npos,
           "runtime label rendering uses modern label_enable and label_show policy in every grid mode");
    expect(fullRender.find("if (!closing && (**PLABELEN || **PSELECTEN || showWorkspaceNumbers))") != std::string::npos,
           "workspace and selection labels stop rendering as soon as overview close begins");
    expect(fullRender.find("Hyprexpo::resolveBorderSpec(") != std::string::npos,
           "runtime border rendering uses modern-first border resolution with legacy fallback");
    expect(fullRender.find("Hyprexpo::resolveLabelPosition(") != std::string::npos && fullRender.find("Hyprexpo::resolveLabelFontSize(") != std::string::npos,
           "runtime label rendering resolves explicit modern and legacy option precedence");
    expect(fullRender.find("CompatHyprlandAPI::configValueSetByUser(") != std::string::npos,
           "runtime label compatibility checks whether each option was explicitly configured");
    expect(fullRender.find("g_overviewDrag.state.sourceMonitorKey") != std::string::npos && fullRender.find("g_overviewDrag.state.targetMonitorKey") != std::string::npos,
           "source and destination overview renders query the one shared drag session");
    expect(fullRender.find(".pointerLocal") != std::string::npos && fullRender.find("MON->m_scale") != std::string::npos,
           "destination proxy geometry uses target-local pointer coordinates and target monitor scale");
    expect(source.find("Config::mgr()->getConfigValue(name).setByUser") != std::string::npos,
           "config compatibility exposes explicit-setting metadata for both config providers");

    expect(dispatchersSource.find("HyprlandAPI::getConfigValue") == std::string::npos,
           "gesture config avoids the legacy hyprlang getter, which is null under CONFIG_LUA");

    const auto gestureSync = extractFunction(dispatchersSource, "void syncExpoGestureFromConfig(");
    expect(!gestureSync.empty(), "syncExpoGestureFromConfig exists");
    expect(gestureSync.find("g_unloading || g_gestureRegistrationDisabled") != std::string::npos, "gesture sync bails out while the plugin is unloading");
    expect(gestureSync.find("registerExpoGesture(FINGERS, DIR, \"expo\"") != std::string::npos,
           "plain config synchronization remains expo-only");

    const auto gestureRegister = extractFunction(dispatchersSource, "static SDispatchResult registerExpoGesture(");
    expect(!gestureRegister.empty(), "registerExpoGesture definition exists");
    expect(gestureRegister.find("g_unloading || g_gestureRegistrationDisabled") != std::string::npos,
           "every gesture registration path, including the Lua helper, is fenced during unload");
    expect(gestureRegister.find("action == \"expo\"") != std::string::npos &&
               gestureRegister.find("makeUnique<CExpoGesture>(EExpoGestureAction::Expo)") != std::string::npos,
           "Lua expo registration constructs an explicit expo gesture");
    expect(gestureRegister.find("action == \"cancel\"") != std::string::npos &&
               gestureRegister.find("makeUnique<CExpoGesture>(EExpoGestureAction::Cancel)") != std::string::npos,
           "Lua cancel registration constructs an explicit cancel gesture");
    expect(gestureRegister.find("expected expo|cancel|unset") != std::string::npos,
           "invalid Lua gesture actions report the complete accepted set");

    expect(gestureHeader.find("enum class EExpoGestureAction") != std::string::npos,
           "gesture action modes use an explicit enum");
    expect(gestureHeader.find("CExpoGesture(EExpoGestureAction action)") != std::string::npos,
           "gesture construction requires an explicit action");
    expect(gestureHeader.find("\n    CExpoGesture()") == std::string::npos,
           "gesture construction cannot silently default an action");
    expect(gestureHeader.find("const EExpoGestureAction m_action") != std::string::npos,
           "each gesture retains an immutable action mode");
    expect(gestureHeader.find("IOverviewSession*    overview() const;") != std::string::npos &&
               gestureHeader.find("PHLMONITORREF m_monitor;") != std::string::npos,
           "each gesture retains an origin-monitor lookup instead of a singleton overview pointer");

    const auto gestureBegin = extractFunction(gestureSource, "void CExpoGesture::begin(");
    expect(!gestureBegin.empty(), "gesture begin function exists");
    const auto monitorQueryStart = gestureBegin.find("const auto monitor");
    const auto monitorCapture    = gestureBegin.find("m_monitor = monitor;", monitorQueryStart);
    const auto overviewResolve   = gestureBegin.find("auto* const OV = overviewForMonitor(monitor);", monitorCapture);
    const auto cancelBeginStart = gestureBegin.find("if (m_action == EExpoGestureAction::Cancel)");
    const auto expoBeginStart   = gestureBegin.find("\n    if (!OV) {", cancelBeginStart);
    const auto cancelBeginBlock = cancelBeginStart == std::string::npos || expoBeginStart == std::string::npos ? std::string{} :
                                                                                                               gestureBegin.substr(cancelBeginStart, expoBeginStart - cancelBeginStart);
    expect(monitorQueryStart != std::string::npos && monitorCapture != std::string::npos && overviewResolve != std::string::npos && cancelBeginStart != std::string::npos &&
               monitorQueryStart < monitorCapture && monitorCapture < overviewResolve && overviewResolve < cancelBeginStart,
           "gesture begin captures and resolves its origin monitor before either action runs");
    expect(cancelBeginBlock.find("!OV || OV->closeCommitted()") != std::string::npos,
           "cancel begin is inert without a mutable overview");
    expect(cancelBeginBlock.find("OV->beginCancelSwipe()") != std::string::npos,
           "cancel begin delegates target reset and interactive closing to the overview");
    expect(cancelBeginBlock.find("selectHoveredWorkspace") == std::string::npos && cancelBeginBlock.find("createOverview") == std::string::npos,
           "cancel begin neither selects a hovered workspace nor creates an overview");
    expect(overviewHeader.find("void beginCancelSwipe() override;") != std::string::npos,
           "overview exposes an owned cancel-begin transition");
    const auto cancelSwipeBegin = extractFunction(interactionSource, "void COverview::beginCancelSwipe(");
    expect(!cancelSwipeBegin.empty(), "overview cancel-begin transition exists");
    const auto resetCancelTarget = cancelSwipeBegin.find("closeOnID = openedID");
    const auto startCancelClose  = cancelSwipeBegin.find("closing", resetCancelTarget);
    const auto enableCancelClose = cancelSwipeBegin.find("= true", startCancelClose);
    expect(resetCancelTarget != std::string::npos && startCancelClose != std::string::npos && enableCancelClose != std::string::npos && resetCancelTarget < startCancelClose,
           "cancel begin replaces any aborted expo target with the opening workspace before closing");
    const auto expoBeginBlock = expoBeginStart == std::string::npos ? std::string{} : gestureBegin.substr(expoBeginStart);
    expect(expoBeginBlock.find("createOverview(monitor, true)") != std::string::npos &&
               expoBeginBlock.find("OV->selectHoveredWorkspace()") != std::string::npos &&
               expoBeginBlock.find("OV->setClosing(true)") != std::string::npos,
           "expo begin retains open and hovered-selection close behavior");

    const auto gestureOverview = extractFunction(gestureSource, "IOverviewSession* CExpoGesture::overview() const {");
    expect(gestureOverview.find("overviewForSession(overviewMonitorKey(m_monitor.lock()), m_sessionGeneration)") != std::string::npos,
           "gesture callbacks re-resolve only the overview owned by the captured origin monitor");

    const auto gestureUpdate = extractFunction(gestureSource, "void CExpoGesture::update(");
    expect(gestureUpdate.find("auto* const OV = overview();") != std::string::npos &&
               gestureUpdate.find("!OV || OV->closeCommitted()") != std::string::npos,
           "gesture updates re-resolve the origin overview and ignore committed closes");
    const auto firstUpdateGuard = gestureUpdate.find("if (m_firstUpdate)");
    const auto accumulateDelta  = gestureUpdate.find("m_lastDelta += distance(e);");
    expect(firstUpdateGuard != std::string::npos && accumulateDelta != std::string::npos && firstUpdateGuard < accumulateDelta,
           "gesture updates discard the first compositor delta before accumulating movement");

    const auto gestureEnd = extractFunction(gestureSource, "void CExpoGesture::end(");
    expect(!gestureEnd.empty(), "gesture end function exists");
    expect(gestureEnd.find("auto* const OV = overview();") != std::string::npos &&
               gestureEnd.find("!OV || OV->closeCommitted()") != std::string::npos,
           "gesture end re-resolves the origin overview and ignores committed closes");
    expect(gestureEnd.find("OV->setClosing(false)") != std::string::npos,
           "gesture end clears transient closing before threshold evaluation");
    expect(gestureEnd.find("OV->onSwipeEnd(m_action == EExpoGestureAction::Expo)") != std::string::npos,
           "gesture completion selects only for the expo action");
    expect(gestureEnd.find("if (auto* const STILL_ALIVE = overview())") != std::string::npos &&
               gestureEnd.find("STILL_ALIVE->resetSwipe()") != std::string::npos,
           "gesture completion re-resolves the origin overview before its post-close reset");

    const auto swipeEnd = extractFunction(interactionSource, "void COverview::onSwipeEnd(");
    expect(!swipeEnd.empty(), "overview swipe-end function exists");
    expect(swipeEnd.find("void COverview::onSwipeEnd(bool switchToSelection)") != std::string::npos,
           "overview swipe completion accepts the selection decision");
    const auto degenerateSpanStart = swipeEnd.find("if (std::abs(span.x) <= 1e-6)");
    const auto thresholdStart = swipeEnd.find("if (PERC > 0.5)", degenerateSpanStart);
    const auto incompleteStart = swipeEnd.find("*size = MON->m_size", thresholdStart);
    const auto degenerateSpanBlock = degenerateSpanStart == std::string::npos || thresholdStart == std::string::npos ? std::string{} :
                                                                                                                       swipeEnd.substr(degenerateSpanStart, thresholdStart - degenerateSpanStart);
    const auto thresholdBlock = thresholdStart == std::string::npos || incompleteStart == std::string::npos ? std::string{} :
                                                                                                               swipeEnd.substr(thresholdStart, incompleteStart - thresholdStart);
    const auto incompleteBlock = incompleteStart == std::string::npos ? std::string{} : swipeEnd.substr(incompleteStart);
    expect(degenerateSpanBlock.find("close(switchToSelection)") != std::string::npos &&
               thresholdBlock.find("close(switchToSelection)") != std::string::npos,
           "completed swipe paths forward the action-specific selection choice");
    expect(incompleteBlock.find("*pos  = {0, 0}") != std::string::npos &&
               incompleteBlock.find("swipeWasCommenced = true") != std::string::npos &&
               incompleteBlock.find("m_isSwiping       = false") != std::string::npos &&
               incompleteBlock.find("close(") == std::string::npos,
           "incomplete swipe still resets animation state and leaves the overview open");

    expect(source.find("#include <hyprland/src/desktop/view/window/Window.hpp>") != std::string::npos &&
               source.find("#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>") != std::string::npos,
           "overview capture uses the pinned development window and presentation headers");
    expect(source.find("window->mapped()") != std::string::npos &&
               source.find("WINDOW_STATE_PINNED") != std::string::npos &&
               source.find("window->presentation().alpha(") != std::string::npos &&
               source.find("window->presentation().movingFromMonitor()") != std::string::npos &&
               source.find("window->presentation().resetMonitorMovedFrom()") != std::string::npos,
           "overview capture retains mapped, pinned, alpha, and moved-monitor behavior on development APIs");

    const auto applyPinnedState = extractFunction(source, "std::vector<SPinnedWindowPreviewState> applyPinnedWindowPreviewState(");
    const auto restorePinnedState = extractFunction(source, "void restorePinnedWindowPreviewState(");
    expect(applyPinnedState.find("!(window->m_state & Desktop::View::WINDOW_STATE_PINNED)") != std::string::npos &&
               applyPinnedState.find(".pinned    = true") != std::string::npos &&
               applyPinnedState.find("window->m_state &= ~Desktop::View::WINDOW_STATE_PINNED") != std::string::npos &&
               applyPinnedState.find("window->m_workspace.reset()") != std::string::npos,
           "pinned preview suppression saves only pinned windows, clears their pinned bit and detaches their workspace");
    expect(restorePinnedState.find("state.window->m_workspace = state.workspace") != std::string::npos &&
               restorePinnedState.find("if (state.pinned)") != std::string::npos &&
               restorePinnedState.find("state.window->m_state |= Desktop::View::WINDOW_STATE_PINNED") != std::string::npos,
           "pinned preview suppression restores workspace and pinned bit without overwriting other state flags");

    expect(dispatchersSource.find("Input::ModifierMask modMask = Input::HL_MODIFIER_NONE") != std::string::npos &&
               dispatchersSource.find("Keybinds::modMaskFromString(mods)") != std::string::npos,
           "gesture registration uses the pinned development modifier parser and mask type");
    expect(interactionSource.find("#include <hyprland/src/keybinds/Manager.hpp>") != std::string::npos &&
               interactionSource.find("Keybinds::mgr()->currentSubmap()") != std::string::npos,
           "submap ownership captures the exact active submap through the development keybind manager API");

    for (const auto& [luaName, wrapper, handler] : std::vector<std::tuple<std::string, std::string, std::string>>{
             {"move_window", "luaMoveWindow", "onMovePreviewWindowDispatcher"},
             {"scrolling_debug", "luaScrollingDebug", "onScrollingDebugDispatcher"},
             {"scrolling_input_test", "luaScrollingInputTest", "onScrollingInputTestDispatcher"},
             {"scrolling_mutation_test", "luaScrollingMutationTest", "onScrollingMutationTestDispatcher"},
         }) {
        const auto body = extractFunction(dispatchersSource, "static int " + wrapper + "(");
        expectContains(body, "luaDispatchResult(L, \"hyprexpo." + luaName + "\", " + handler + "(luaStringArg(L, 1, \"hyprexpo." + luaName + "\")))",
                       luaName + " Lua path shares string validation, native handler and error propagation");
        expectContains(dispatchersSource, "HyprlandAPI::addLuaFunction(PHANDLE, \"hyprexpo\", \"" + luaName + "\", " + wrapper + ")",
                       luaName + " remains callable when upstream disables legacy dispatcher registration");
    }

    const auto exitFunction = extractFunction(mainSource, "APICALL EXPORT void PLUGIN_EXIT(");
    expect(!exitFunction.empty(), "PLUGIN_EXIT exists");
    const auto disablePos = exitFunction.find("disableExpoGestureRegistration();");
    const auto reloadPos  = exitFunction.find("Config::mgr()->reload();");
    expect(disablePos != std::string::npos, "PLUGIN_EXIT disables gesture registration");
    expect(reloadPos != std::string::npos, "PLUGIN_EXIT reloads the config to clear registered gestures");
    expect(disablePos < reloadPos, "the registration fence is set before the teardown reload re-runs the config");

    expect(mainSource.find("config.reloaded.listen") != std::string::npos, "the gesture is re-applied after every config reload");

    const auto multiMonitorDocs = readFile("docs/guides/multi-monitor.md");
    const auto keyboardDocs     = readFile("docs/configuration/keyboard.md");
    const auto dispatcherDocs   = readFile("docs/reference/dispatchers.md");
    expect(multiMonitorDocs.find("global logical geometry") != std::string::npos && multiMonitorDocs.find("target monitor") != std::string::npos &&
               multiMonitorDocs.find("not supported yet") == std::string::npos,
           "multi-monitor guide documents geometry-based navigation and target-local drag/drop");
    expect(keyboardDocs.find("local wrapping takes precedence") != std::string::npos && keyboardDocs.find("keyboard owner") != std::string::npos,
           "keyboard guide documents explicit ownership and local-wrap precedence");
    expect(dispatcherDocs.find("fills any missing monitor overviews") != std::string::npos && dispatcherDocs.find("closes every open overview") != std::string::npos,
           "dispatcher reference distinguishes idempotent enabling from toggle close and peer dismissal");
    expect(dispatcherDocs.find("monitor under the pointer") != std::string::npos,
           "dispatcher reference identifies cursor ownership for pointer select and bring");
    const auto adapterHeader = readFile("src/ScrollingLayoutAdapter.hpp");
    const auto adapterSource = readFile("src/ScrollingLayoutAdapter.cpp");
    const auto mutationHeader = readFile("src/ScrollingMutationTransaction.hpp");
    const auto mutationSource = readFile("src/ScrollingMutationTransaction.cpp");
    const auto scrollingHeader = readFile("src/ScrollingOverview.hpp");
    const auto scrollingSource = readFile("src/ScrollingOverview.cpp");
    const auto scrollingCommit = extractFunction(scrollingSource, "bool CScrollingOverview::commitSelection(");
    expect(scrollingCommit.find("Desktop::focusState()->monitor() != MON") != std::string::npos,
           "selecting an already-active scrolling workspace also focuses its monitor");
    const auto requestIdHeader = readFile("src/ScrollingRequestId.hpp");
    const auto diagnosticSource = readFile("src/ScrollingDiagnostics.cpp");
    const auto diagnosticScript = readFile("scripts/read-scrolling-diagnostic.sh");
    expect(!adapterHeader.empty() && !adapterSource.empty(), "scrolling adapter source can be read from repo root");
    expect(!mutationHeader.empty() && !mutationSource.empty(), "scrolling transaction source can be read from repo root");
    expect(!requestIdHeader.empty() && !diagnosticSource.empty() && !diagnosticScript.empty(), "shared request ID and diagnostic sources can be read from repo root");
    expectContains(requestIdHeader, "bool validRequestID", "one pure validator owns the request ID grammar");
    expectContains(requestIdHeader, "c == '.'", "shared request ID grammar accepts dots");
    expectContains(diagnosticSource, "validRequestID(request.requestID)", "topology diagnostics use the shared request ID validator");

    for (const auto& token : {"NullWorkspace", "InertWorkspace", "MissingSpace", "MissingAlgorithm", "MissingTiledAlgorithm", "WrongAlgorithmName", "CastFailure", "ExpiredTarget",
                              "ExpiredColumn", "ExpiredData", "MissingScrollingData", "ColumnCardinalityMismatch", "TargetCardinalityMismatch", "InvalidGeometry"})
        expectContains(adapterHeader, token, "adapter exposes typed fail-closed result " + std::string{token});
    for (const auto& token : {"workspace->inert()", "workspace->m_space", "algorithm()", "tiledAlgo()", "algoMatcher()->getNameForTiledAlgo", "dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>",
                              "dataFor(target)", ".lock()", "stripCount()", "getDirection()", "getOffset()", "getStrip(", "targetSizes.size()", "targetDatas.size()", "calculateStripStart(",
                              "calculateStripSize(", "layoutBox", "windowStableID", "algorithmFingerprint", "dataFingerprint"})
        expectContains(adapterSource, token, "adapter implements guarded snapshot token " + std::string{token});
    expectOrder(adapterSource, "algoMatcher()->getNameForTiledAlgo", "dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>", "adapter verifies matcher name before dynamic cast");
    expectOrder(adapterSource, "dataFor(target)", "column->scrollingData.lock()", "adapter locks native ownership hops after resolving target data");

    const auto snapshotImplementation = extractFunction(adapterSource, "SSnapshotResult snapshotWorkspaceImpl(");
    for (const auto& token : {".moveTape(", ".moveTapeNormalized(", ".snapToGrid(", ".setOffset(", ".adjustOffset(", ".centerCol(", ".fitCol(", ".focusColumn(", ".addStrip(",
                              ".insertStrip(", ".removeStrip(", ".swapStrips(", ".setColumnWidth(", ".setTargetSize(", ".recalculate(", "beginDragTarget", "moveWindowToWorkspace", "glReadPixels(",
                              "readPixels(", "std::ofstream", "m_title", "m_class"})
        expectAbsent(snapshotImplementation + diagnosticSource, token, "read-only snapshot/diagnostic forbids " + std::string{token});
    expectAbsent(adapterHeader, "CScrollingAlgorithm*", "adapter DTO/header retains no raw scrolling algorithm pointer");

    for (const auto& token : {"moveScrollingTarget", "SMutationResult", "executeMutation(", "snapshotPreState", "removeTarget", "addTarget", "restorePreState",
                              "setColumnWidth", "setTargetSize", "recalculate", "verifyPostconditions", "MutationResult"})
        expectContains(adapterHeader + adapterSource + mutationHeader + mutationSource, token, "native transaction boundary exposes " + std::string{token});
    const auto nativeMove = extractFunction(adapterSource, "SMutationResult moveScrollingTarget(");
    expect(!nativeMove.empty(), "same-workspace native transaction entry point exists");
    expectOrder(nativeMove, "snapshotPreState", "executeMutation(", "native transaction snapshots before the engine can remove a target");
    expectContains(adapterSource, "catch (const std::exception&", "native mutation catches standard host exceptions");
    expectContains(adapterSource, "catch (...)", "native mutation catches unknown host exceptions");
    expectOrder(adapterSource, "restorePreState", "setColumnWidth", "rollback rebuilds structure before restoring exact widths");
    expectOrder(adapterSource, "setColumnWidth", "setTargetSize", "rollback restores widths before row proportions");
    expectOrder(adapterSource, "setTargetSize", "recalculate", "native structure and sizes are restored before final recalculation");
    expectOrder(adapterSource, "recalculate", "verifyPostconditions", "native readback verifies postconditions after final recalculation");
    const auto nativeClassPosition = adapterSource.find("class CNativeMutationOperations");
    const auto nativeMutationImplementation = nativeClassPosition == std::string::npos ? std::string{} : adapterSource.substr(nativeClassPosition);
    for (const auto& token : {"beginDragTarget", "getMouseCoordsInternal", ".moveTape(", ".adjustOffset(", ".centerCol(", ".fitCol(", ".focusColumn("})
        expectAbsent(nativeMove + nativeMutationImplementation, token,
                     "native transaction forbids cursor/drag/camera operation " + std::string{token});
    expectContains(adapterSource, "controller->setDirection", "native transaction restores the exact pre-state direction after host structure changes");
    expectContains(adapterSource, "controller->setOffset(workspaceState.offset)", "native transaction restores the exact pre-state offset after host structure changes");

    for (const auto& token : {"Desktop::globalWindowController()->moveWindowToWorkspace", "controllerMove", "reverse", "reResolve", "nextUnusedOrdinaryWorkspaceID",
                              "State::workspaceState()->create", "m_monitor", "MixedFallback", "TerminalWorkspace"})
        expectContains(adapterSource + mutationSource, token, "cross/terminal transaction exposes " + std::string{token});
    expectOrder(adapterSource, "moveWindowToWorkspace", "reResolve", "cross move discards stale ownership before destination positioning");
    expectContains(adapterSource, "if (reverse)", "rollback controller path explicitly reverses ownership");
    expectContains(adapterSource, "m_createdDestination", "terminal rollback tracks the workspace created by this transaction");
    for (const auto& token : {"proveCreatedDestinationRollback", "stripCount()", "workspacesCopy()", "m_native.erase", "m_createdDestination.reset()"})
        expectContains(adapterSource, token, "terminal rollback proves empty controller/workspace release via " + std::string{token});

    expectContains(scrollingSource, "moveScrollingTarget(", "one validated release enters the native transaction boundary");
    expectContains(scrollingSource, "EMutationOutcome::Committed", "committed mutation refreshes the scrolling session");
    expectContains(scrollingSource, "EMutationOutcome::RolledBack", "rolled-back mutation refreshes the scrolling session");
    expectContains(scrollingSource, "EMutationOutcome::RollbackFailed", "fatal rollback failure safe-closes the scrolling session");
    expectContains(scrollingSource, "Log::logger->log(Log::ERR", "fatal mutation emits a high-severity diagnostic");
    const auto applyEffects = extractFunction(scrollingSource, "void CScrollingOverview::applyInputEffects(");
    expectOrder(applyEffects, "m_pendingDropIntent.reset()", "moveScrollingTarget(", "release consumes the retained transaction intent before native mutation");
    expectLastOrder(applyEffects, "moveScrollingTarget(", "refreshAfterMutation();", "commit/rollback refresh happens after the exact-once transaction result");
    expectContains(diagnosticSource, "mutationDiagnosticJson", "mutation outcomes serialize correlated postcondition evidence");
    expectContains(diagnosticSource, "violatedInvariantIDs", "mutation diagnostics include exact violated invariant IDs");
    const auto mutationDiagnostic = extractFunction(diagnosticSource, "std::string mutationDiagnosticJson(");
    for (const auto& token : {"requestId", "sessionGeneration", "beforeSummary", "afterSummary", "beforeHash", "afterHash", "mutationOutcome", "violatedInvariantIDs"})
        expectContains(mutationDiagnostic, token, "mutation diagnostic carries correlated structural evidence " + std::string{token});
    for (const auto& token : {"requestKind", "placement", "destinationColumnIndex", "destinationRowIndex", "beforeState", "afterState", "columns", "targets", "members", "width", "size"})
        expectContains(diagnosticSource, token, "mutation diagnostic retains exact native postcondition field " + std::string{token});
    for (const auto& token : {"m_title", "m_class", "windowTitle", "windowClass"})
        expectAbsent(mutationDiagnostic, token, "mutation diagnostic excludes title/class secret surface " + std::string{token});
    expectContains(scrollingSource, "m_mutationRequestSequence", "release diagnostics allocate a session-local correlation sequence");
    expectContains(adapterHeader + adapterSource, "runNativeMutationTest", "debug acceptance resolves and executes the loaded native transaction boundary");
    expectContains(adapterSource, "m_fault", "native mutation operations retain only request-scoped fault injection");
    expectContains(adapterSource, "checkpoint(EMutationPhase phase, EMutationStep step, EFaultWhen when)", "native operations evaluate exact transaction fault boundaries");
    expectContains(dispatchersSource, "hyprexpo:scrolling_mutation_test", "loaded native mutation acceptance dispatcher is registered");

    for (const auto& token : {"requestId", "sessionGeneration", "marker", "hyprlandVersion", "runtimeHash", "clientHash", "monitorId", "workspaceId", "algorithmFingerprint", "dataFingerprint", "direction",
                              "offsetBefore", "offsetAfter", "activeWorkspaceBefore", "activeWorkspaceAfter", "focusedWindowBefore", "focusedWindowAfter", "columns", "targets", "layoutBox", "visible",
                              "group", "floating", "fullscreen", "pinned", "topologyEqual", "directionEqual", "offsetEqual", "orderEqual",
                              "widthsEqual", "membershipEqual", "sizesEqual", "specialStateEqual", "algorithmEqual", "dataEqual", "activeWorkspaceEqual", "focusEqual", "mutationOutcome", "rollbackStatus", "status"})
        expectContains(diagnosticSource, token, "diagnostic JSON includes observability field " + std::string{token});
    expectContains(diagnosticSource, "snapshotWorkspace", "diagnostic serializes copied adapter snapshots");
    expectContains(diagnosticSource, "snapshotsEquivalent", "diagnostic performs before/after readback equality");
    expectContains(dispatchersSource, "hyprexpo:scrolling_debug", "read-only scrolling diagnostic dispatcher is registered");
    expectContains(dispatchersSource, "HYPREXPO_SCROLLING_DIAGNOSTIC {}", "dispatcher emits one structured diagnostic log record");
    expectContains(diagnosticScript, "expected exactly one diagnostic record", "reader rejects missing or duplicate request records");
    expectContains(diagnosticScript, "valid_request_id", "reader validates the request ID before filtering logs");

    const auto captureHeader = readFile("src/OverviewCapture.hpp");
    const auto captureSource = readFile("src/OverviewCapture.cpp");
    const auto configHeader  = readFile("src/HyprexpoConfig.hpp");
    const auto makefile      = readFile("Makefile");
    const auto cmake         = readFile("CMakeLists.txt");
    const auto meson         = readFile("meson.build");
    expect(!captureHeader.empty() && !captureSource.empty(), "shared overview capture source can be read from repo root");
    expectContains(captureHeader, "captureWorkspacePreview", "shared capture API exposes the grid and mixed-row workspace boundary");
    expectContains(captureHeader, "captureWindowPreview", "shared capture API exposes tight scrolling-target capture");
    expectContains(captureHeader, "completed", "tight capture result distinguishes completed textures from failed attempts");

    const auto workspaceCapture = extractFunction(captureSource, "bool captureWorkspacePreview(");
    expect(!workspaceCapture.empty(), "shared workspace capture implementation exists");
    for (const auto& token : {"beginRender(", "clearWithColor(", "applyExclusiveWorkspacePreviewState(", "applyWorkspaceWindowGoalState(", "CPinnedWindowPreviewGuard",
                              "renderWorkspace(", "restoreWorkspaceWindowGoalState(", "restoreWorkspacePreviewStates(", "restoreActiveWorkspaceAfterPreview(", "rendererState.finish()"})
        expectContains(workspaceCapture, token, "shared workspace capture preserves grid operation " + std::string{token});
    expectOrder(workspaceCapture, "beginRender(", "clearWithColor(", "workspace capture begins before clearing");
    expectOrder(workspaceCapture, "clearWithColor(", "applyExclusiveWorkspacePreviewState(", "workspace capture clears before temporary workspace state");
    expectOrder(workspaceCapture, "applyWorkspaceWindowGoalState(", "renderWorkspace(", "workspace goal state is applied before rendering");
    expectLastOrder(workspaceCapture, "renderWorkspace(", "restoreWorkspaceWindowGoalState(", "workspace goal state is restored after rendering");
    expectLastOrder(workspaceCapture, "renderWorkspace(", "restoreWorkspacePreviewStates(", "workspace preview state restores after rendering");
    expectLastOrder(workspaceCapture, "renderWorkspace(", "restoreActiveWorkspaceAfterPreview(", "active workspace restores after rendering");
    expectLastOrder(workspaceCapture, "restoreActiveWorkspaceAfterPreview(", "rendererState.finish()", "workspace capture ends only after workspace restoration");
    expectContains(overviewConstructor, "captureWorkspacePreview(", "initial grid capture uses the shared workspace helper");
    const auto redrawID = extractFunction(renderSource, "void COverview::redrawID(");
    expectContains(redrawID, "captureWorkspacePreview(", "grid redraw uses the shared workspace helper");
    expectAbsent(source, "g_pHyprRenderer->renderWorkspace(", "grid construction no longer duplicates workspace snapshot rendering");
    expectAbsent(renderSource, "g_pHyprRenderer->renderWorkspace(", "grid redraw no longer duplicates workspace snapshot rendering");

    const auto windowCapture = extractFunction(captureSource, "SWindowCaptureResult captureWindowPreview(");
    expect(!windowCapture.empty(), "tight scrolling-target capture implementation exists");
    for (const auto& token : {"createFB(", "beginFullFakeRender(", "m_bBlockSurfaceFeedback = true", "m_bRenderingSnapshot = true", "startRenderPass()", "renderWindow(",
                              "Render::RENDER_PASS_ALL, true, true", "blockScreenShader = true", "rendererState.finish()", "getTexture()", "result.completed"})
        expectContains(windowCapture, token, "tight target capture uses approved GPU operation " + std::string{token});
    expectOrder(windowCapture, "beginFullFakeRender(", "renderWindow(", "target capture begins fake rendering before the window draw");
    expectOrder(windowCapture, "renderWindow(", "rendererState.finish()", "target capture balances the renderer after drawing");
    expectOrder(windowCapture, "rendererState.finish()", "result.completed", "target capture publishes only after balanced completion");
    for (const auto& token : {"glReadPixels(", "readPixels(", "std::ofstream", ".ppm", "sha256", "SHA256"})
        expectAbsent(captureSource, token, "production capture forbids plugin-side pixel evidence path " + std::string{token});

    for (const auto& token : {"SCROLLING_THUMBNAIL_BUDGET_DEFAULT", "SCROLLING_THUMBNAIL_BUDGET_MIN", "SCROLLING_THUMBNAIL_BUDGET_MAX"})
        expectContains(configHeader, token, "thumbnail budget exposes bounded config constant " + std::string{token});
    expectContains(configSource, "plugin:hyprexpo:scrolling_thumbnail_budget", "scrolling thumbnail budget configuration is registered");
    expectContains(configSource, "HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_DEFAULT", "scrolling thumbnail budget registration uses the resolved default");
    expectContains(configSource, ".min = HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_MIN", "scrolling thumbnail budget registration enforces the minimum");
    expectContains(configSource, ".max = HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_MAX", "scrolling thumbnail budget registration enforces the maximum");
    expectContains(makefile, "src/OverviewCapture.cpp", "Make production sources include the shared capture boundary");
    expectContains(makefile, "src/OverviewCapture.hpp", "Make headers include the shared capture boundary");

    const std::string buildDefinitions = makefile + cmake + meson;
    expect(!cmake.empty() && !meson.empty(), "CMake and Meson build definitions can be read from repo root");
    for (const auto& productionSource : {"src/main.cpp", "src/Dispatchers.cpp", "src/PluginConfig.cpp", "src/IOverviewSession.cpp", "src/Overview.cpp", "src/OverviewInteraction.cpp",
                                         "src/OverviewRender.cpp", "src/OverviewCapture.cpp", "src/ScrollingOverview.cpp", "src/ScrollingInputState.cpp", "src/ExpoGesture.cpp",
                                         "src/OverviewPassElement.cpp", "src/HyprexpoLogic.cpp", "src/ScrollingOverviewLogic.cpp", "src/ScrollingMutationTransaction.cpp",
                                         "src/ScrollingLayoutAdapter.cpp", "src/ScrollingDiagnostics.cpp"}) {
        expectContains(makefile, productionSource, "Make includes production source " + std::string{productionSource});
        expectContains(cmake, productionSource, "CMake includes production source " + std::string{productionSource});
        expectContains(meson, productionSource, "Meson includes production source " + std::string{productionSource});
    }

    for (const auto& pureSource : {"src/HyprexpoLogic.cpp", "src/ScrollingOverviewLogic.cpp", "src/ScrollingInputState.cpp", "src/ScrollingMutationTransaction.cpp"}) {
        expectContains(cmake, pureSource, "CMake logic tests include pure source " + std::string{pureSource});
        expectContains(meson, pureSource, "Meson logic tests include pure source " + std::string{pureSource});
    }

    for (const auto& suite : {"HyprexpoLogicTests", "OverviewSourceTests"}) {
        expectContains(makefile, suite, "Make registers test suite " + std::string{suite});
        expectContains(cmake, "add_executable(" + std::string{suite}, "CMake creates test executable " + std::string{suite});
        expectContains(cmake, "add_test(NAME " + std::string{suite}, "CTest registers test suite " + std::string{suite});
        expectContains(meson, "executable('" + std::string{suite} + "'", "Meson creates test executable " + std::string{suite});
        expectContains(meson, "test('" + std::string{suite} + "'", "Meson registers test suite " + std::string{suite});
    }
    expectContains(buildDefinitions, "tests/OverviewSourceTests.cpp", "build definitions include the source-contract suite");

    const auto nestedFixture = readFile("scripts/run-nested.sh");
    const auto validator = readFile("scripts/validate-scrolling-overview.sh");
    const auto scrollingGuide = readFile("docs/guides/scrolling-overview.md");
    const auto runtimeGuide = readFile("docs/guides/runtime-smoke.md");
    const auto optionsGuide = readFile("docs/configuration/options.md");
    const auto keyboardGuide = readFile("docs/configuration/keyboard.md");
    const auto dispatcherGuide = readFile("docs/reference/dispatchers.md");
    const auto readme = readFile("README.md");
    const auto publicDocs = scrollingGuide + runtimeGuide + optionsGuide + keyboardGuide + dispatcherGuide + readme;
    for (const auto& token : {"HYPREXPO_DEV_LAYOUT", "layout:scrolling", "layout:dwindle", "layoutmsg", "consume", "expel", "fit"})
        expectContains(nestedFixture, token, "nested fixture exposes scrolling contract token " + std::string{token});
    for (const auto& token : {"--all", "--evidence", "0.56.1", "5c9377c15f85c50648f35ca5a213754f95b93ca0", "f3ed01d3b024e404563e7ce18efdf1583aaa8cba",
                              "HYPREXPO_SCROLLING_DIAGNOSTIC", "HYPREXPO_SCROLLING_INPUT", "HYPREXPO_SCROLLING_MUTATION", "clients -j", "configerrors", "plugin list", "grim",
                              "same-column", "existing-column", "new-column-before", "new-column-after", "cross-scrolling", "mixed-workspace", "terminal-workspace", "outside-release", "no-op-release",
                              "touch-cancel-pending", "touch-cancel-pan", "touch-cancel-drag", "default-grid"})
        expectContains(validator, token, "validator covers acceptance token " + std::string{token});
    expectContains(validator, "--issue-85-publication-check", "validator exposes an opt-in issue-85 publication fence");
    expectContains(validator, "publication_check=false", "reusable runtime validation defaults publication checks off");
    expectContains(validator, "if [[ $publication_check == true ]]", "branch/base/remote fences execute only when explicitly requested");
    expectContains(scrollingGuide, "Optional pre-publication fence", "docs separate reusable post-merge validation from issue-85 publication checks");
    for (const auto& token : {"input.mouse.move", "input.mouse.button", "input.mouse.axis", "input.touch.down", "input.touch.motion", "input.touch.up", "input.touch.cancel",
                              "scrolling_thumbnail_budget", "m * W * H", "mixed", "terminal", "next empty", "same-column", "new column", "rollback", "0.56.1",
                              "hyprexpo:scrolling_debug", "hyprexpo:scrolling_input_test", "not full Niri parity"})
        expectContains(publicDocs, token, "public docs describe scrolling contract token " + std::string{token});

    const auto sessionHeader   = readFile("src/IOverviewSession.hpp");
    const auto sessionSource   = readFile("src/IOverviewSession.cpp");
    const auto passSource      = readFile("src/OverviewPassElement.cpp");
    expect(!sessionHeader.empty() && !sessionSource.empty(), "common overview session interface and factory can be read from repo root");
    expect(!scrollingHeader.empty() && !scrollingSource.empty(), "read-only scrolling session source can be read from repo root");

    for (const auto& token : {"class IOverviewSession", "virtual ~IOverviewSession", "virtual void render()", "virtual void damage()", "virtual void onDamageReported()",
                              "virtual void onPreRender()", "virtual void fullRender()", "virtual void close(", "virtual bool closeCommitted()", "virtual void setClosing(",
                              "virtual void resetSwipe()", "virtual void onSwipeUpdate(", "virtual void onSwipeEnd(bool switchToSelection)", "virtual void onWindowMoveToWorkspace(",
                              "virtual bool selectHoveredWorkspace()", "virtual bool onKbMoveFocus(", "virtual bool onKbConfirm()", "virtual bool onKbSelectNumber(",
                              "virtual bool onKbSelectToken(", "virtual bool selectVisibleToken(", "virtual int64_t selectedWorkspaceID()", "virtual bool selectWorkspaceByID(",
                              "virtual bool selectVisibleIndex(", "virtual bool moveWindowBetweenVisibleIndices(", "virtual bool blocksOverviewRendering()",
                              "virtual bool blocksDamageReporting()", "virtual bool isSwiping()", "virtual PHLMONITOR monitor()", "virtual uint64_t sessionGeneration()"})
        expectContains(sessionHeader, token, "session interface covers caller surface " + std::string{token});
    expectContains(sessionHeader, "virtual void onConfigReload()", "session interface refreshes caches through the polymorphic boundary");
    expectContains(sessionHeader, "std::vector<std::unique_ptr<IOverviewSession>> g_overviews", "one polymorphic registry owns each monitor session");
    expectContains(sessionHeader, "createOverviewSession", "session factory is declared beside the polymorphic owner");
    expectContains(overviewHeader, "class COverview final : public IOverviewSession", "existing grid overview implements the common interface without mode branches");
    expectAbsent(overviewHeader, "inline std::unique_ptr<COverview> g_pOverview", "grid header no longer owns a concrete global session");

    for (const auto& token : {"workspaceUsesScrollingLayout(startedOn)", "snapshotWorkspace(startedOn)", "CScrollingOverview", "COverview", "std::make_unique<CScrollingOverview>", "std::make_unique<COverview>"})
        expectContains(sessionSource, token, "factory implements guarded selection token " + std::string{token});
    expectOrder(sessionSource, "snapshotWorkspace(startedOn)", "std::make_unique<CScrollingOverview>", "factory snapshots live native state before selecting scrolling");
    expectContains(sessionSource, "notifyScrollingFailure", "detected scrolling initialization failure is operator-visible");
    expectContains(sessionSource, "return nullptr", "detected scrolling initialization failure returns no session");
    expectAbsent(sessionSource, "using grid fallback", "detected scrolling never silently falls back to grid");
    expectOrder(sessionSource, "if (detectedScrolling)", "return std::make_unique<COverview>", "grid construction is reachable only after the detected-scrolling branch");
    expectContains(dispatchersSource, "createOverview(monitor)", "dispatcher creates sessions through the monitor registry");
    expectContains(source, "createOverviewSession(monitor->m_activeWorkspace, monitor, swipe)", "registry passes the explicit monitor to the sole session factory");
    expectContains(dispatchersSource, "failed to initialize native scrolling overview", "dispatcher reports fail-closed scrolling creation");
    expectContains(gestureSource, "createOverview(monitor, true)", "gesture creates sessions through the monitor registry and sole factory");
    expectAbsent(dispatchersSource + gestureSource, "std::make_unique<COverview>", "callers never instantiate the concrete grid implementation");

    for (const auto& token : {"->pMonitor", "->m_isSwiping", "->blockOverviewRendering", "->blockDamageReporting"})
        expectAbsent(mainSource + dispatchersSource + passSource + gestureSource, token, "external callers avoid concrete session field " + std::string{token});
    for (const auto& token : {"blocksOverviewRendering()", "blocksDamageReporting()", "isSwiping()", "monitor()"})
        expectContains(mainSource + dispatchersSource + passSource, token, "external callers use virtual accessor " + std::string{token});
    expectAbsent(mainSource + dispatchersSource + passSource + gestureSource, "dynamic_cast<COverview", "session callers contain no concrete overview downcast");

    expectContains(passSource, "overviewForSession(overviewMonitorKey(m_monitor.lock()), m_sessionGeneration)", "render pass validates monitor and generation identity");
    expectContains(passSource, "m_sessionGeneration", "render pass retains only an immutable generation identity");
    expectOrder(passSource, "if (auto* const OV = overview())", "OV->fullRender()", "render pass null-checks before virtual rendering");
    expectContains(scrollingHeader, "PHLANIMVAR<float> m_transitionProgress", "scrolling session owns one compositor-managed transition value");
    expectContains(scrollingSource, "Animation::mgr()->createAnimation", "scrolling overview entry and exit use the compositor animation manager");
    expectContains(scrollingSource, "transitionForSwipe(m_swipeClosing, m_swipeDelta", "scrolling swipe delta drives the visible transition progress");
    expectContains(scrollingSource, "CompatHyprlandAPI::intValue(\"plugin:hyprexpo:gesture_distance\")",
                   "scrolling swipe reads gesture distance through the Lua-compatible config boundary");
    expectAbsent(scrollingSource, "HyprlandAPI::getConfigValue", "scrolling session never uses the legacy config getter");
    expectContains(sessionHeader, "virtual void beginCancelSwipe() = 0", "cancel gesture dispatches through the session interface");
    expectContains(sessionHeader, "virtual void onSwipeEnd(bool switchToSelection) = 0", "session interface retains action-specific swipe completion");
    const auto scrollingCancelBegin = extractFunction(scrollingSource, "void CScrollingOverview::beginCancelSwipe(");
    expectContains(scrollingCancelBegin, "setClosing(true)", "scrolling cancel begins the existing input-fenced close transition");
    expectAbsent(scrollingCancelBegin, "commitSelection", "scrolling cancel never commits a hovered selection");
    const auto scrollingSwipeEnd = extractFunction(scrollingSource, "void CScrollingOverview::onSwipeEnd(");
    expectContains(scrollingSwipeEnd, "close(false)", "scrolling swipe completion retains the opening workspace for both gesture actions");
    expectAbsent(scrollingSwipeEnd, "close(true)", "scrolling swipe completion never commits selection implicitly");
    expectContains(scrollingSource, "applyOverviewTransition", "scrolling render boxes consume the transition transform");
    expectContains(scrollingSource, "transition.opacity", "scrolling render content consumes transition opacity");
    const auto scrollingClose = extractFunction(scrollingSource, "void CScrollingOverview::close(");
    expectContains(scrollingClose, "scheduleScrollingOverviewRemoval(overviewMonitorKey(m_monitor.lock()), m_sessionGeneration)", "scrolling close defers owner destruction until transition completion");
    expectAbsent(scrollingClose, "g_pOverview.reset()", "scrolling close never destroys itself synchronously");
    expectContains(scrollingSource, "g_pEventLoopManager->doLater", "animation completion leaves its callback before destroying session ownership");
    expectContains(scrollingSource, "overviewForSession(monitorKey, generation)", "deferred close cannot remove a replacement overview session");
    expectContains(scrollingHeader, "m_closeAnimationTimer", "scrolling close retains a compositor timer for a visible first exit frame");
    expectOrder(scrollingClose, "setValueAndWarp", "makeShared<CEventLoopTimer>", "scrolling close presents an initial transformed frame before advancing toward zero");
    expectOrder(scrollingClose, "makeShared<CEventLoopTimer>", "*m_transitionProgress = 0.F", "scrolling close begins the remaining animation from the timer callback");
    expectContains(mainSource, "destroyAllOverviews();", "plugin exit destroys the session owner");
    expectOrder(mainSource, "destroyAllOverviews();", "removeAllOfType", "plugin exit destroys session-owned GPU state before pass removal");

    for (const auto& token : {"class CScrollingOverview final : public IOverviewSession", "SCacheKey", "sessionGeneration", "workspaceID", "targetToken",
                              "contentDamageGeneration", "captureWidth", "captureHeight", "budgetGeneration", "PHLWINDOWREF", "WP<Layout::ITarget>", "releaseCacheEntry"})
        expectContains(scrollingHeader + scrollingSource, token, "scrolling session exposes bounded cache/lifetime token " + std::string{token});
    for (const auto& token : {"snapshotWorkspace(", "buildScene(", "initialPan(", "planCaptureBudget(", "captureWindowPreview(", "captureWorkspacePreview(",
                              "EWorkspaceKind::Scrolling", "EWorkspaceKind::Mixed", "EWorkspaceKind::Empty", "EWorkspaceKind::Terminal", "group", "floating", "fullscreen", "pinned"})
        expectContains(scrollingSource, token, "scrolling renderer covers full read-only scene token " + std::string{token});
    expectOrder(scrollingSource, "releaseCacheEntry", "captureWindowPreview(", "stale cache ownership is released before replacement capture");
    expectContains(scrollingSource, "scrollingThumbnailBudgetMultiplier", "scrolling renderer reads the bounded monitor-relative config");
    expectContains(scrollingSource, "captureWorkspacePreview(", "mixed-layout rows reuse the sole workspace capture boundary");
    expectContains(scrollingSource, "m_blockOverviewRendering = true", "mixed-layout capture forces the original grid render path");
    expectContains(scrollingSource, "m_blockDamageReporting   = true", "mixed-layout capture suppresses recursive damage feedback");
    expectContains(mainSource, "forEachOverview([](IOverviewSession& overview) { overview.onConfigReload(); })", "config reload invalidates the active session without a concrete downcast");
    expectAbsent(scrollingSource, "g_pHyprRenderer->renderWorkspace(", "scrolling renderer does not duplicate grid/mixed workspace capture");

    for (const auto& token : {"beginDragTarget", "moveWindowToWorkspace", ".moveTape(", ".setOffset(", ".addStrip(", ".removeStrip(", ".setColumnWidth(", ".setTargetSize(", ".recalculate("})
        expectAbsent(scrollingSource, token, "scrolling input emits intents without native mutation: " + std::string{token});

    const auto inputHeader = readFile("src/ScrollingInputState.hpp");
    const auto inputSource = readFile("src/ScrollingInputState.cpp");
    const auto inputScript = readFile("scripts/inject-scrolling-input.sh");
    expect(!inputHeader.empty() && !inputSource.empty(), "pure scrolling input state source can be read from repo root");
    expect(!inputScript.empty(), "deterministic scrolling input injection harness can be read from repo root");

    for (const auto& token : {"m_events.input.mouse.move.listen", "m_events.input.mouse.button.listen", "m_events.input.mouse.axis.listen", "m_events.input.touch.down.listen",
                              "m_events.input.touch.motion.listen", "m_events.input.touch.up.listen", "m_events.input.touch.cancel.listen"})
        expectContains(scrollingSource, token, "scrolling session subscribes to exact signal " + std::string{token});
    for (const auto& token : {"mouseMoveHook", "mouseButtonHook", "mouseAxisHook", "touchDownHook", "touchMotionHook", "touchUpHook", "touchCancelHook"})
        expectContains(scrollingHeader, token, "scrolling session owns listener handle " + std::string{token});

    expectContains(scrollingSource, "transitionInput(", "real and injected scrolling input share the pure transition function");
    expectContains(scrollingSource, "info.cancelled = info.cancelled || effects.consume", "callbacks preserve cancellation while applying the pure consume effect");
    expectContains(scrollingSource, "g_pInputManager->getMouseCoordsInternal()", "mouse signals normalize through current global logical coordinates");
    expectContains(scrollingSource, "touchToGlobalLogical(", "touch signals normalize through the touched monitor logical geometry");
    expectContains(scrollingSource, "relativeDirection", "mouse axis honors the Hyprland relative-direction payload");
    expectContains(scrollingSource, "applyInputEffects", "all runtime input effects use one application path");
    expectContains(scrollingSource, "resetInputState", "refresh, close, cancel, and teardown share one idempotent reset path");
    const auto scrollingProcessInput = extractFunction(scrollingSource, "SInputEffects CScrollingOverview::processInput(");
    expectContains(scrollingProcessInput, "m_closing || m_closeCommitted", "closing scrolling sessions reject fresh mouse and touch ownership");
    const auto scrollingKbMove = extractFunction(scrollingSource, "bool CScrollingOverview::onKbMoveFocus(");
    const auto scrollingKbConfirm = extractFunction(scrollingSource, "bool CScrollingOverview::onKbConfirm(");
    expectContains(scrollingKbMove, "m_closing || m_closeCommitted", "closing scrolling sessions reject fresh keyboard navigation");
    expectContains(scrollingKbConfirm, "m_closing || m_closeCommitted", "closing scrolling sessions reject fresh keyboard selection");
    const auto scrollingInstallInput = extractFunction(scrollingSource, "void CScrollingOverview::installInputListeners(");
    expectContains(scrollingInstallInput, "m_events.window.moveToWorkspace.listen", "workspace moves explicitly invalidate native input while damage refresh is deferred");
    const auto scrollingReset = extractFunction(scrollingSource, "SInputEffects CScrollingOverview::resetInputState(");
    expectContains(scrollingReset, "workspaceMoveHook.reset();", "teardown disconnects native workspace invalidation before destroying its session");
    expectContains(scrollingInstallInput, "event.device->m_boundOutput.empty()", "touch ownership requires an explicitly bound output");
    expectAbsent(scrollingInstallInput, ": m_monitor.lock()", "unbound touch devices are not guessed onto the overview monitor");
    const auto scrollingRefresh = extractFunction(scrollingSource, "bool CScrollingOverview::refreshScene(");
    const auto scrollingPreRender = extractFunction(scrollingSource, "void CScrollingOverview::onPreRender(");
    expectOrder(scrollingPreRender, "m_inputState.owner != EInputOwner::None", "m_damageDirty = false;",
                "ordinary damage stays pending while a mouse or touch interaction owns the scene");
    const auto scrollingEffects = extractFunction(scrollingSource, "void CScrollingOverview::applyInputEffects(");
    expectContains(scrollingEffects, "effects.resetOwnership && m_damageDirty", "ending a pan schedules its deferred refresh even without client damage");
    expectOrder(scrollingEffects, "!sceneTopologyCurrent()", "moveScrollingTarget(", "drops reject stale scene indices before touching native layout");
    const auto currentTopology = extractFunction(scrollingSource, "bool CScrollingOverview::sceneTopologyCurrent(");
    for (const auto& token : {"m_rows.size()", "snapshotWorkspace(row.workspace)", "oldColumn.fingerprint != newColumn.fingerprint",
                              "targetToken(oldTarget) != targetToken(newTarget)", "oldTarget.proportion != newTarget.proportion"})
        expectContains(currentTopology, token, "held native drops revalidate scene structure via " + std::string{token});
    const auto inputSelection = extractFunction(scrollingEffects, "if (effects.selection)");
    expectOrder(inputSelection, "updateSelectionFromFocus();", "setClosing(true);", "input selection is latched before freezing scene refresh");
    expectOrder(inputSelection, "setClosing(true);", "g_pEventLoopManager->doLater", "released input selection survives frames before deferred close");
    const auto scrollingDestructor = extractFunction(scrollingSource, "CScrollingOverview::~CScrollingOverview(");
    expectOrder(scrollingRefresh, "resetInputState(EResetReason::Refresh)", "releaseAllCaptureState();", "scene refresh clears input ownership before replacing cache state");
    expectOrder(scrollingDestructor, "resetInputState(EResetReason::Teardown)", "releaseAllCaptureState();", "destruction clears listeners/input before capture ownership");
    expectContains(sessionHeader, "injectScrollingInput", "injection routes through the polymorphic session without a concrete downcast");
    expectContains(overviewHeader, "injectScrollingInput", "grid session explicitly rejects scrolling-only injection");
    expectAbsent(source, "m_events.input.mouse.axis.listen", "grid mode does not install scrolling axis ownership");
    expectAbsent(source, "m_events.input.touch.cancel.listen", "grid mode does not install scrolling cancel ownership");

    expectContains(configHeader, "SCROLLING_INPUT_DEBUG_DEFAULT", "synthetic input is default-disabled");
    expectContains(configSource, "plugin:hyprexpo:scrolling_input_debug", "synthetic input debug gate is registered");
    expectContains(dispatchersSource, "hyprexpo:scrolling_input_test", "strict synthetic input dispatcher is registered");
    expectContains(dispatchersSource, "scrolling_input_debug", "synthetic input dispatcher checks the debug config gate");
    expectContains(dispatchersSource, "HYPREXPO_SCROLLING_INPUT {}", "request-correlated input diagnostics are emitted to the compositor log");
    expectContains(dispatchersSource, "OV->injectScrollingInput", "dispatcher routes synthetic input through the active session interface");

    const auto generationLookup = extractFunction(source, "IOverviewSession* overviewForSession(");
    expectContains(generationLookup, "overviewForMonitorKey(monitorKey)", "session identity first resolves its owning monitor");
    expectContains(generationLookup, "session->sessionGeneration() == generation", "session identity rejects a replacement on the same monitor");
    expectAbsent(scrollingSource + mainSource + dispatchersSource + gestureSource + passSource, "g_pOverview",
                 "all session consumers resolve the polymorphic registry without a singleton escape hatch");
    expectContains(sessionSource, "startedOn, monitor, swipe, generation", "both session constructors receive explicit monitor and generation");
    expectContains(source, "OWNER->prepareForTeardown();", "single removal releases polymorphic input and capture lifecycle");
    expectOrder(destroyOne, "g_overviews.erase(IT);", "OWNER->prepareForTeardown();", "single teardown callbacks see an already-unregistered owner");
    expectOrder(destroyAll, "OWNERS.swap(g_overviews);", "owner->prepareForTeardown();", "bulk teardown callbacks see an empty registry");
    expectContains(scrollingSource, "enterOverviewSubmap(m_submapActive)", "scrolling joins the exact shared navigation submap");
    expectContains(scrollingClose, "leaveOverviewSubmap(m_submapActive)", "scrolling close releases only its own submap reference");
    expectContains(scrollingInstallInput, "info.cancelled || pointerOverview() != this", "scrolling pointer input requires the owning monitor or retained drag owner");
    expectOrder(scrollingInstallInput, "resetInputState(EResetReason::Cancel)", "info.cancelled || pointerOverview() != this",
                "moving onto another output clears stale hover even when its session already consumed the event");
    expectContains(source, "dynamic_cast<COverview*>(pointerOverview())", "grid input leaves a scrolling drag's ownership intact across monitor edges");
    expectContains(scrollingInstallInput, "!ownsTouchInput(event.touchID)", "touch follow-up events route only to the session that acquired the touch");
    expectAbsent(scrollingKbConfirm, "close(", "scrolling keyboard confirmation stages selection before coordinated close");
    expectContains(scrollingSource, "closeOverviewsSelecting(OV)", "scrolling pointer selection commits only the selected monitor");
    expectContains(scrollingKbMove, "moveOverviewFocusAcrossMonitors(this, across)", "scrolling edge navigation composes with monitor geometry");
    expectContains(scrollingSource, "CScrollingOverview::globalTiles() const", "scrolling publishes visible native targets to cross-monitor navigation");
    const auto scrollingRender = extractFunction(scrollingSource, "void CScrollingOverview::fullRender(");
    expectOrder(scrollingRender, "target.pinned && !m_showPinnedWindows", "CBox box = target.box",
                "hidden pinned windows cannot paint fallback rectangles over selectable scrolling targets");

    for (const auto& token : {"hover-clear", "non-primary-passthrough", "mouse-click", "same-column", "new-column-before", "new-column-after", "cross-scrolling", "mixed-workspace",
                              "terminal-workspace", "axis-owned", "touch-pan", "touch-tap", "touch-same-column", "touch-cancel", "mismatched-cancel", "touch-reacquire", "mouse-reacquire", "stale-id",
                              "refresh-reset", "teardown-reset"})
        expectContains(inputScript, token, "injection harness covers deterministic case " + std::string{token});
    expectContains(inputScript, "--source-contract", "input harness offers a non-physical source-contract gate");
    expectContains(inputScript, "requestId", "input harness correlates every readback to its request");
    expectContains(inputScript, "ScrollingInputOracle", "source gate executes the same strict input parser and diagnostic serializer without physical hardware");
    expectContains(inputScript, "assert_case", "input harness applies case-specific behavioral oracles");
    for (const auto& token : {"axis-inside", "axis-outside", "axis-clamped", "mouse-canvas-pan", "touch-cancel-pending", "touch-cancel-pan", "touch-cancel-drag",
                              "touch-reacquire", "mouse-reacquire", "outside-release", "no-op-release"})
        expectContains(inputScript, token, "input oracle includes missing deterministic case " + std::string{token});
    for (const auto& token : {".state ==", ".consume ==", ".pan ==", ".panDelta ==", ".select ==", ".beginDrag ==", ".finishDrag ==", ".cancelDrag ==",
                              ".resetOwnership ==", ".drop =="})
        expectContains(inputScript, token, "input oracle asserts exact diagnostic field " + std::string{token});
    expectContains(inputSource, "parseInputSequence", "production and oracle share the strict finite sequence parser");
    expectContains(inputSource, "validRequestID(specs.front())", "input injection uses the shared request ID validator");
    expectContains(inputSource, "inputDiagnosticJson", "production and oracle share request-correlated JSON serialization");
    expectContains(scrollingSource, "parseInputSequence(sequence)", "runtime injection delegates strict parsing to the pure boundary");
    expectContains(scrollingSource, "inputDiagnosticJson(parsed.requestId", "runtime injection delegates readback serialization to the oracle-covered boundary");

    expectContains(makefile, "src/IOverviewSession.cpp", "Make production sources include the overview factory");
    expectContains(makefile, "src/ScrollingOverview.cpp", "Make production sources include the scrolling renderer");
    expectContains(makefile, "src/ScrollingInputState.cpp", "Make production and logic tests include the pure input model");
    expectContains(makefile, "src/ScrollingMutationTransaction.cpp", "Make production and logic tests include the pure transaction model");
    expectContains(makefile, "src/ScrollingMutationTransaction.hpp", "Make headers include the transaction contract");
    expectContains(makefile, "scripts/inject-scrolling-input.sh", "Make source tests track the deterministic input harness");
    expectContains(makefile, "src/IOverviewSession.hpp", "Make headers include the overview interface");
    expectContains(makefile, "src/ScrollingOverview.hpp", "Make headers include the scrolling renderer contract");

    if (failures != 0)
        return 1;

    std::cout << "OverviewSourceTests passed\n";
    return 0;
}
