#include "ScrollingOverview.hpp"

#include "HyprexpoConfig.hpp"
#include "HyprlandConfigCompat.hpp"
#include "OverviewCapture.hpp"
#include "OverviewInternal.hpp"
#include "OverviewPassElement.hpp"
#include "ScrollingDiagnostics.hpp"

#define private   public
#define protected public
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#undef private
#undef protected

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

using namespace Hyprexpo::Scrolling;

namespace {

EDirection parseDirection(const std::string& direction) {
    if (direction == "left")
        return EDirection::Left;
    if (direction == "down")
        return EDirection::Down;
    if (direction == "up")
        return EDirection::Up;
    return EDirection::Right;
}

uint64_t targetToken(const STargetSnapshot& target) {
    return target.targetFingerprint ? static_cast<uint64_t>(target.targetFingerprint) : target.windowStableID;
}

CBox sceneBox(const Hyprexpo::SRect& box, double pan) {
    return {box.x, box.y - pan, box.w, box.h};
}

CBox applyTransition(CBox box, const Hyprexpo::SSize& viewport, const SOverviewTransition& transition) {
    const auto transformed = applyOverviewTransition({box.x, box.y, box.w, box.h}, viewport, transition);
    return {transformed.x, transformed.y, transformed.w, transformed.h};
}

CHyprColor applyTransitionOpacity(const CHyprColor& color, const SOverviewTransition& transition) {
    return color.modifyA(color.a * transition.opacity);
}

void scheduleScrollingOverviewRemoval(uint64_t monitorKey, uint64_t generation) {
    g_pEventLoopManager->doLater([monitorKey, generation]() {
        auto* const OV = overviewForSession(monitorKey, generation);
        if (!OV)
            return;
        const auto MON = OV->monitor();
        destroyOverview(OV);
        if (!MON)
            return;
        g_pHyprRenderer->damageMonitor(MON);
        MON->scheduleFrame();
    });
}

EOutputTransform outputTransform(wl_output_transform transform) {
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_90: return EOutputTransform::Rotate90;
        case WL_OUTPUT_TRANSFORM_180: return EOutputTransform::Rotate180;
        case WL_OUTPUT_TRANSFORM_270: return EOutputTransform::Rotate270;
        case WL_OUTPUT_TRANSFORM_FLIPPED: return EOutputTransform::Flipped;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90: return EOutputTransform::Flipped90;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180: return EOutputTransform::Flipped180;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270: return EOutputTransform::Flipped270;
        default: return EOutputTransform::Normal;
    }
}

}

CScrollingOverview::CScrollingOverview(const PHLWORKSPACE& startedOn, const PHLMONITOR& monitor, bool swipe, uint64_t sessionGeneration, const std::optional<SWorkspaceSnapshot>& initialSnapshot) :
    m_startedOn(startedOn), m_monitor(monitor), m_sessionGeneration(sessionGeneration), m_isSwiping(swipe),
    m_selectedWorkspaceID(startedOn ? startedOn->m_id : 0) {
    Animation::mgr()->createAnimation(0.F, m_transitionProgress, Config::animationTree()->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    m_transitionProgress->setUpdateCallback([monitorKey = overviewMonitorKey(monitor), generation = m_sessionGeneration](auto) {
        if (auto* const OV = overviewForSession(monitorKey, generation))
            OV->damage();
    });
    m_valid = refreshScene(initialSnapshot);
    if (m_valid) {
        installInputListeners();
        enterOverviewSubmap(m_submapActive);
        if (!swipe)
            *m_transitionProgress = 1.F;
    }
}

CScrollingOverview::~CScrollingOverview() {
    leaveOverviewSubmap(m_submapActive);
    if (m_closeAnimationTimer)
        m_closeAnimationTimer->cancel();
    m_closeAnimationTimer.reset();
    resetInputState(EResetReason::Teardown);
    releaseAllCaptureState();
}

bool CScrollingOverview::valid() const {
    return m_valid;
}

SInputContext CScrollingOverview::inputContext() const {
    const auto MON = m_monitor.lock();
    if (!MON)
        return {};
    return {
        .scene = &m_scene,
        .monitor = {
            .position = {MON->m_position.x, MON->m_position.y},
            .logicalSize = {MON->m_size.x, MON->m_size.y},
            .pixelSize = {MON->m_pixelSize.x, MON->m_pixelSize.y},
            .scale = MON->m_scale,
            .transform = outputTransform(MON->m_transform),
        },
        .pan = m_pan,
        .viewportHeight = MON->m_size.y,
        .dragThreshold = 12.0,
    };
}

std::optional<Hyprexpo::SPoint> CScrollingOverview::normalizeTouchPoint(Hyprexpo::SPoint normalizedPoint, const PHLMONITOR& touchedMonitor) const {
    if (!touchedMonitor)
        return std::nullopt;
    return touchToGlobalLogical(normalizedPoint, {
        .position = {touchedMonitor->m_position.x, touchedMonitor->m_position.y},
        .logicalSize = {touchedMonitor->m_size.x, touchedMonitor->m_size.y},
        .pixelSize = {touchedMonitor->m_pixelSize.x, touchedMonitor->m_pixelSize.y},
        .scale = touchedMonitor->m_scale,
        .transform = outputTransform(touchedMonitor->m_transform),
    });
}

SInputEffects CScrollingOverview::resetInputState(EResetReason reason) {
    if (reason == EResetReason::Teardown) {
        mouseMoveHook.reset();
        mouseButtonHook.reset();
        mouseAxisHook.reset();
        touchDownHook.reset();
        touchMotionHook.reset();
        touchUpHook.reset();
        touchCancelHook.reset();
        workspaceMoveHook.reset();
    }
    const auto reset = resetInput(m_inputState, reason);
    m_inputState = reset.state;
    m_touchMonitor.reset();
    m_pendingDropSource = {};
    m_pendingDropIntent.reset();
    return reset.effects;
}

void CScrollingOverview::prepareForTeardown() {
    leaveOverviewSubmap(m_submapActive);
    if (m_closeAnimationTimer)
        m_closeAnimationTimer->cancel();
    m_closeAnimationTimer.reset();
    resetInputState(EResetReason::Teardown);
}

void CScrollingOverview::applyInputEffects(const SInputEffects& effects, const SInputState& previousState) {
    bool needsDamage = effects.hoverChanged || effects.clearHover || effects.beginDrag || effects.updateDrag || effects.cancelDrag || effects.finishDrag ||
        (effects.resetOwnership && m_damageDirty);
    if (effects.panDelta != 0.0) {
        m_pan += effects.panDelta;
        needsDamage = true;
    }
    if (effects.dropIntent)
        m_pendingDropIntent = *effects.dropIntent;
    if (effects.beginDrag || effects.updateDrag || effects.finishDrag)
        m_pendingDropSource = previousState.mode == EInputMode::WindowDrag ? previousState.dragSource : m_inputState.dragSource;
    if (effects.cancelDrag)
        m_pendingDropIntent.reset();
    if (effects.resetOwnership)
        m_touchMonitor.reset();
    if (needsDamage)
        damage();

    if (effects.finishDrag) {
        const auto source = previousState.dragSource;
        const auto targetIdentity = previousState.pressed.targetToken;
        const auto intent = effects.dropIntent ? effects.dropIntent : m_pendingDropIntent;
        m_pendingDropIntent.reset();
        m_pendingDropSource = {};

        const auto refreshAfterMutation = [this]() {
            if (refreshScene()) {
                damage();
                return;
            }
            const auto generation = m_sessionGeneration;
            const auto monitorKey = overviewMonitorKey(m_monitor.lock());
            setClosing(true);
            g_pEventLoopManager->doLater([monitorKey, generation]() {
                if (auto* const OV = overviewForSession(monitorKey, generation))
                    OV->close(false);
            });
        };

        if (!intent || targetIdentity == 0 || intent->kind == EDropKind::Invalid || intent->kind == EDropKind::NoOp || !sceneTopologyCurrent()) {
            refreshAfterMutation();
        } else {
            const auto sourceRow = workspaceRow(source.workspaceID);
            const auto destinationRow = intent->kind == EDropKind::TerminalWorkspace ? nullptr : workspaceRow(intent->workspaceID);
            if (!sourceRow || !sourceRow->workspace || (intent->kind != EDropKind::TerminalWorkspace && (!destinationRow || !destinationRow->workspace))) {
                refreshAfterMutation();
            } else {
                const SMutationRequest request{
                    .requestID = "scroll-drop-" + std::to_string(m_sessionGeneration) + "-" + std::to_string(++m_mutationRequestSequence),
                    .sessionGeneration = m_sessionGeneration,
                    .targetIdentity = targetIdentity,
                    .sourceWorkspaceID = source.workspaceID,
                    .destinationWorkspaceID = intent->workspaceID,
                    .kind = intent->kind,
                    .placement = intent->placement,
                    .destinationColumnIndex = intent->adjustedColumnIndex,
                    .destinationRowIndex = intent->rowIndex,
                    .createDestination = intent->kind == EDropKind::TerminalWorkspace,
                };
                const auto MON = m_monitor.lock();
                const auto result = moveScrollingTarget(sourceRow->workspace, destinationRow ? destinationRow->workspace : PHLWORKSPACE{}, MON, request);
                const auto diagnostic = mutationDiagnosticJson(result);
                if (result.outcome == EMutationOutcome::RollbackFailed) {
                    Log::logger->log(Log::ERR, Log::logFnName(), "HYPREXPO_SCROLLING_MUTATION {}", diagnostic);
                    const auto generation = m_sessionGeneration;
                    const auto monitorKey = overviewMonitorKey(m_monitor.lock());
                    setClosing(true);
                    g_pEventLoopManager->doLater([monitorKey, generation]() {
                        if (auto* const OV = overviewForSession(monitorKey, generation))
                            OV->close(false);
                    });
                } else {
                    Log::logger->log(Log::INFO, Log::logFnName(), "HYPREXPO_SCROLLING_MUTATION {}", diagnostic);
                    switch (result.outcome) {
                        case EMutationOutcome::Committed:
                        case EMutationOutcome::RolledBack:
                        case EMutationOutcome::Rejected: refreshAfterMutation(); break;
                        case EMutationOutcome::RollbackFailed: break;
                    }
                }
            }
        }
    }

    if (effects.selection) {
        m_focus = {.kind = effects.selection->kind, .workspaceID = effects.selection->workspaceID, .targetToken = effects.selection->targetToken};
        updateSelectionFromFocus();
        // The release relinquishes input before the deferred close runs.
        setClosing(true);
        const auto generation = m_sessionGeneration;
        const auto monitorKey = overviewMonitorKey(m_monitor.lock());
        g_pEventLoopManager->doLater([monitorKey, generation]() {
            if (auto* const OV = overviewForSession(monitorKey, generation))
                closeOverviewsSelecting(OV);
        });
    }
}

SInputEffects CScrollingOverview::processInput(const SInputEvent& event) {
    if (m_closing || m_closeCommitted || !m_transitionProgress || m_transitionProgress->value() < 0.999F)
        return {};
    const auto previousState = m_inputState;
    const auto transition = transitionInput(previousState, event, inputContext());
    m_inputState = transition.state;
    const auto effects = transition.effects;
    applyInputEffects(effects, previousState);
    return effects;
}

void CScrollingOverview::installInputListeners() {
    workspaceMoveHook = Event::bus()->m_events.window.moveToWorkspace.listen([this](PHLWINDOW window, PHLWORKSPACE workspace) { onWindowMoveToWorkspace(window, workspace); });
    mouseMoveHook = Event::bus()->m_events.input.mouse.move.listen([this](const Vector2D&, Event::SCallbackInfo& info) {
        // Leaving this output must clear its hover even when another session owns the event.
        if (pointerOverview() != this && m_inputState.owner == EInputOwner::None && m_inputState.hover.kind != EHitKind::Outside) {
            resetInputState(EResetReason::Cancel);
            damage();
        }
        if (info.cancelled || pointerOverview() != this)
            return;
        const auto point = g_pInputManager->getMouseCoordsInternal();
        const auto effects = processInput({.kind = EInputKind::MouseMove, .globalLogicalPoint = {point.x, point.y}});
        info.cancelled = info.cancelled || effects.consume;
    });
    mouseButtonHook = Event::bus()->m_events.input.mouse.button.listen([this](const IPointer::SButtonEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled || pointerOverview() != this)
            return;
        const auto point = g_pInputManager->getMouseCoordsInternal();
        const auto effects = processInput({.kind = EInputKind::MouseButton,
                                           .globalLogicalPoint = {point.x, point.y},
                                           .button = event.button,
                                           .pressed = event.state == WL_POINTER_BUTTON_STATE_PRESSED,
                                           .time = event.timeMs});
        info.cancelled = info.cancelled || effects.consume;
    });
    mouseAxisHook = Event::bus()->m_events.input.mouse.axis.listen([this](const IPointer::SAxisEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled || pointerOverview() != this)
            return;
        const auto point = g_pInputManager->getMouseCoordsInternal();
        const double direction = event.relativeDirection == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED ? -1.0 : 1.0;
        const auto effects = processInput({.kind = EInputKind::MouseAxis,
                                           .globalLogicalPoint = {point.x, point.y},
                                           .axisDelta = event.delta * direction,
                                           .time = event.timeMs});
        info.cancelled = info.cancelled || effects.consume;
    });
    touchDownHook = Event::bus()->m_events.input.touch.down.listen([this](const ITouch::SDownEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled)
            return;
        if (!event.device || event.device->m_boundOutput.empty())
            return;
        const auto touchedMonitor = State::monitorState()->query().name(event.device->m_boundOutput).run();
        const auto point = normalizeTouchPoint({event.pos.x, event.pos.y}, touchedMonitor);
        if (!point)
            return;
        const auto effects = processInput({.kind = EInputKind::TouchDown, .globalLogicalPoint = *point, .touchId = event.touchID, .time = event.timeMs});
        if (effects.consume && m_inputState.owner == EInputOwner::Touch && m_inputState.owningTouchId == event.touchID)
            m_touchMonitor = touchedMonitor;
        info.cancelled = info.cancelled || effects.consume;
    });
    touchMotionHook = Event::bus()->m_events.input.touch.motion.listen([this](const ITouch::SMotionEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled || !ownsTouchInput(event.touchID))
            return;
        const auto point = normalizeTouchPoint({event.pos.x, event.pos.y}, m_touchMonitor.lock());
        if (!point)
            return;
        const auto effects = processInput({.kind = EInputKind::TouchMotion, .globalLogicalPoint = *point, .touchId = event.touchID, .time = event.timeMs});
        info.cancelled = info.cancelled || effects.consume;
    });
    touchUpHook = Event::bus()->m_events.input.touch.up.listen([this](const ITouch::SUpEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled || !ownsTouchInput(event.touchID))
            return;
        const auto effects = processInput({.kind = EInputKind::TouchUp, .globalLogicalPoint = {}, .touchId = event.touchID, .time = event.timeMs});
        info.cancelled = info.cancelled || effects.consume;
    });
    touchCancelHook = Event::bus()->m_events.input.touch.cancel.listen([this](const ITouch::SCancelEvent& event, Event::SCallbackInfo& info) {
        if (info.cancelled || !ownsTouchInput(event.touchID))
            return;
        const auto effects = processInput({.kind = EInputKind::TouchCancel, .globalLogicalPoint = {}, .touchId = event.touchID, .time = event.timeMs});
        info.cancelled = info.cancelled || effects.consume;
    });
}

std::expected<std::string, std::string> CScrollingOverview::injectScrollingInput(const std::string& sequence) {
    if (m_closing || m_closeCommitted)
        return std::unexpected("scrolling overview is closing");
    const auto parsed = parseInputSequence(sequence);
    if (!parsed.valid)
        return std::unexpected(parsed.error);
    std::vector<SInputDiagnosticRecord> records;
    records.reserve(parsed.steps.size());
    for (const auto& step : parsed.steps) {
        SInputEffects effects;
        if (step.reset)
            effects = resetInputState(*step.reset);
        else
            effects = processInput(*step.event);
        records.push_back({.state = m_inputState, .effects = effects, .pan = m_pan});
    }
    return inputDiagnosticJson(parsed.requestId, records, m_inputState, m_pendingDropIntent.has_value());
}

void CScrollingOverview::releaseCacheEntry(SCacheEntry& entry) {
    entry.texture.reset();
    if (entry.framebuffer)
        entry.framebuffer->release();
    entry.framebuffer.reset();
    entry.targetRef.reset();
    entry.windowRef.reset();
}

void CScrollingOverview::releaseAllCaptureState() {
    if (Render::GL::g_pHyprOpenGL)
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    for (auto& entry : m_cache)
        releaseCacheEntry(entry);
    m_cache.clear();
    for (auto& row : m_rows) {
        if (row.workspacePreview)
            row.workspacePreview->release();
        row.workspacePreview.reset();
    }
}

CScrollingOverview::SCacheEntry* CScrollingOverview::cacheEntry(int64_t workspaceID, uint64_t targetTokenValue) {
    const auto found = std::ranges::find_if(m_cache, [&](const auto& entry) { return entry.key.workspaceID == workspaceID && entry.key.targetToken == targetTokenValue; });
    return found == m_cache.end() ? nullptr : &*found;
}

const CScrollingOverview::SWorkspaceRow* CScrollingOverview::workspaceRow(int64_t workspaceID) const {
    const auto found = std::ranges::find_if(m_rows, [&](const auto& row) { return row.workspace && row.workspace->m_id == workspaceID; });
    return found == m_rows.end() ? nullptr : &*found;
}

bool CScrollingOverview::sceneTopologyCurrent() const {
    const auto MON = m_monitor.lock();
    if (!MON)
        return false;

    const auto workspaces = State::workspaceState()->workspacesCopy();
    const auto count = std::ranges::count_if(workspaces, [&](const auto& ws) { return ws && !ws->m_isSpecialWorkspace && ws->m_monitor == MON; });
    if (static_cast<size_t>(count) != m_rows.size())
        return false;

    for (const auto& row : m_rows) {
        if (!row.workspace || row.workspace->inert() || row.workspace->m_monitor != MON)
            return false;
        if (row.kind != EWorkspaceKind::Scrolling) {
            const bool empty = row.workspace->getWindowCount() <= 0;
            if ((row.kind == EWorkspaceKind::Empty) != empty || (!empty && workspaceUsesScrollingLayout(row.workspace)))
                return false;
            continue;
        }

        const auto current = snapshotWorkspace(row.workspace);
        if (!current.success())
            return false;
        const auto& before = row.snapshot;
        const auto& after = *current.snapshot;
        if (before.algorithmFingerprint != after.algorithmFingerprint || before.dataFingerprint != after.dataFingerprint ||
            before.direction != after.direction || before.columns.size() != after.columns.size())
            return false;
        for (size_t i = 0; i < before.columns.size(); ++i) {
            const auto& oldColumn = before.columns[i];
            const auto& newColumn = after.columns[i];
            if (oldColumn.fingerprint != newColumn.fingerprint || oldColumn.width != newColumn.width ||
                oldColumn.primarySize != newColumn.primarySize || oldColumn.targets.size() != newColumn.targets.size())
                return false;
            for (size_t j = 0; j < oldColumn.targets.size(); ++j) {
                const auto& oldTarget = oldColumn.targets[j];
                const auto& newTarget = newColumn.targets[j];
                if (targetToken(oldTarget) != targetToken(newTarget) || oldTarget.windowStableID != newTarget.windowStableID ||
                    oldTarget.proportion != newTarget.proportion || oldTarget.group != newTarget.group || oldTarget.fullscreen != newTarget.fullscreen)
                    return false;
            }
        }
    }
    return true;
}

bool CScrollingOverview::refreshScene(const std::optional<SWorkspaceSnapshot>& initialSnapshot) {
    const auto MON = m_monitor.lock();
    if (!MON || !m_startedOn || m_startedOn->inert())
        return false;

    resetInputState(EResetReason::Refresh);
    releaseAllCaptureState();
    m_rows.clear();
    m_renderTargets.clear();

    std::vector<PHLWORKSPACE> workspaces;
    for (const auto& workspace : State::workspaceState()->workspacesCopy()) {
        if (!workspace || workspace->m_isSpecialWorkspace || workspace->m_monitor != MON)
            continue;
        workspaces.push_back(workspace);
    }
    if (std::ranges::find(workspaces, m_startedOn) == workspaces.end())
        workspaces.push_back(m_startedOn);
    std::ranges::sort(workspaces, {}, [](const auto& workspace) { return workspace->m_id; });

    std::vector<SWorkspaceSpec> specs;
    specs.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
        auto result = workspace == m_startedOn && initialSnapshot ?
            SSnapshotResult{.failure = ESnapshotFailure::None, .error = {}, .snapshot = *initialSnapshot} : snapshotWorkspace(workspace);
        if (result.success()) {
            SWorkspaceSpec spec{.workspaceID = workspace->m_id, .kind = EWorkspaceKind::Scrolling, .tape = {}};
            spec.tape.direction = parseDirection(result.snapshot->direction);
            for (const auto& column : result.snapshot->columns) {
                SColumnSpec columnSpec{.token = static_cast<uint64_t>(column.fingerprint), .extent = column.primarySize, .targets = {}};
                for (const auto& target : column.targets)
                    columnSpec.targets.push_back({.token = targetToken(target), .proportion = target.proportion});
                spec.tape.columns.push_back(std::move(columnSpec));
            }
            specs.push_back(std::move(spec));
            m_rows.push_back({.workspace = workspace, .kind = EWorkspaceKind::Scrolling, .snapshot = std::move(*result.snapshot), .workspacePreview = {}});
            continue;
        }

        const bool empty = workspace->getWindowCount() <= 0;
        if (workspaceUsesScrollingLayout(workspace) && !empty)
            return false;
        if (workspace == m_startedOn && !empty)
            return false;
        const auto kind = empty ? EWorkspaceKind::Empty : EWorkspaceKind::Mixed;
        specs.push_back({.workspaceID = workspace->m_id, .kind = kind, .tape = {}});
        m_rows.push_back({.workspace = workspace, .kind = kind, .snapshot = {}, .workspacePreview = {}});
    }

    if (specs.empty())
        return false;
    const int64_t terminalWorkspaceID = std::max<int64_t>(1, workspaces.back()->m_id + 1);
    const SSceneConfig sceneConfig{
        .viewportWidth = MON->m_size.x,
        .viewportHeight = MON->m_size.y,
        .rowHeight = std::max(64.0, MON->m_size.y * 0.68),
        .rowGap = std::max(12.0, MON->m_size.y * 0.025),
        .columnGap = std::max(8.0, MON->m_size.x * 0.008),
        .terminalWorkspaceID = terminalWorkspaceID,
    };
    m_scene = buildScene(specs, m_startedOn->m_id, sceneConfig);
    if (!m_scene.valid)
        return false;
    m_pan = initialPan(m_scene, m_startedOn->m_id, MON->m_size.y);

    std::unordered_set<uint64_t> seenTargets;
    for (const auto& row : m_rows) {
        if (row.kind != EWorkspaceKind::Scrolling)
            continue;
        for (const auto& column : row.snapshot.columns) {
            for (const auto& target : column.targets) {
                const auto token = targetToken(target);
                const auto placed = std::ranges::find_if(m_scene.targets, [&](const auto& sceneTarget) { return sceneTarget.workspaceID == row.workspace->m_id && sceneTarget.token == token; });
                if (placed == m_scene.targets.end() || !seenTargets.insert(token).second)
                    continue;
                m_renderTargets.push_back(SRenderTarget{.workspaceID = row.workspace->m_id,
                                           .targetToken = token,
                                           .windowStableID = target.windowStableID,
                                           .box = sceneBox(placed->box, 0.0),
                                           .targetRef = target.targetRef,
                                           .windowRef = target.windowRef,
                                           .group = target.group,
                                           .floating = target.floating,
                                           .fullscreen = target.fullscreen,
                                           .pinned = target.pinned});
            }
        }

        const auto placedRow = std::ranges::find_if(m_scene.workspaces, [&](const auto& sceneRow) { return sceneRow.workspaceID == row.workspace->m_id; });
        if (placedRow == m_scene.workspaces.end())
            continue;
        for (const auto& target : row.snapshot.layoutTargets) {
            const auto token = targetToken(target);
            if (!target.floating || !seenTargets.insert(token).second)
                continue;
            const CBox& source = target.layoutBox;
            const double localX = source.x - MON->m_position.x;
            const double localY = source.y - MON->m_position.y;
            const CBox overlay{placedRow->box.x + localX / std::max(1.0, MON->m_size.x) * placedRow->box.w,
                               placedRow->box.y + localY / std::max(1.0, MON->m_size.y) * placedRow->box.h,
                               source.w / std::max(1.0, MON->m_size.x) * placedRow->box.w,
                               source.h / std::max(1.0, MON->m_size.y) * placedRow->box.h};
            m_renderTargets.push_back(SRenderTarget{.workspaceID = row.workspace->m_id,
                                       .targetToken = token,
                                       .windowStableID = target.windowStableID,
                                       .box = overlay,
                                       .targetRef = target.targetRef,
                                       .windowRef = target.windowRef,
                                       .group = target.group,
                                       .floating = target.floating,
                                       .fullscreen = target.fullscreen,
                                       .pinned = target.pinned});
        }
    }

    m_blockOverviewRendering = true;
    m_blockDamageReporting   = true;
    Hyprutils::Utils::CScopeGuard captureGuard{[this]() {
        m_blockOverviewRendering = false;
        m_blockDamageReporting   = false;
    }};
    for (auto& row : m_rows) {
        if (row.kind != EWorkspaceKind::Mixed)
            continue;
        Hyprexpo::Capture::captureWorkspacePreview({
            .monitor = MON,
            .workspace = row.workspace,
            .startedOn = m_startedOn,
            .box = CBox{0, 0, MON->m_pixelSize.x, MON->m_pixelSize.y},
            .showPinnedWindows = showPinnedWindowsInPreview(),
            .blockSurfaceFeedback = true,
        }, row.workspacePreview);
    }

    refreshCache();
    m_focus = {.kind = EHitKind::EmptyWorkspace, .workspaceID = m_startedOn->m_id};
    for (const auto& target : m_scene.targets) {
        if (target.workspaceID == m_startedOn->m_id) {
            m_focus = {.kind = EHitKind::Target, .workspaceID = target.workspaceID, .targetToken = target.token};
            break;
        }
    }
    updateSelectionFromFocus();
    return true;
}

void CScrollingOverview::refreshCache() {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    const bool showPinned = showPinnedWindowsInPreview();
    m_showPinnedWindows = showPinned;
    const int budgetMultiplier = Hyprexpo::Capture::scrollingThumbnailBudgetMultiplier();
    m_budgetGeneration = Hyprexpo::Capture::scrollingThumbnailBudgetGeneration();

    std::vector<SCaptureRequest> requests;
    for (const auto& target : m_renderTargets) {
        if (target.pinned && !showPinned)
            continue;
        const auto window = target.windowRef.lock();
        if (!window)
            continue;
        requests.push_back({.token = target.targetToken,
                            .width = static_cast<uint32_t>(std::max(1.0, std::ceil(target.box.w * MON->m_scale))),
                            .height = static_cast<uint32_t>(std::max(1.0, std::ceil(target.box.h * MON->m_scale)))});
    }
    const auto plan = planCaptureBudget(static_cast<uint32_t>(MON->m_pixelSize.x), static_cast<uint32_t>(MON->m_pixelSize.y), budgetMultiplier, requests);
    if (!plan.valid)
        return;

    for (const auto& allocation : plan.allocations) {
        const auto target = std::ranges::find_if(m_renderTargets, [&](const auto& item) { return item.targetToken == allocation.token; });
        if (target == m_renderTargets.end())
            continue;
        const SCacheKey key{.sessionGeneration = m_sessionGeneration,
                            .workspaceID = target->workspaceID,
                            .targetToken = target->targetToken,
                            .contentDamageGeneration = m_contentDamageGeneration,
                            .captureWidth = allocation.width,
                            .captureHeight = allocation.height,
                            .budgetGeneration = m_budgetGeneration};
        if (auto* existing = cacheEntry(target->workspaceID, target->targetToken)) {
            if (existing->key == key)
                continue;
            releaseCacheEntry(*existing);
            std::erase_if(m_cache, [&](const auto& entry) { return entry.key.workspaceID == target->workspaceID && entry.key.targetToken == target->targetToken; });
        }
        if (!allocation.capture)
            continue;

        const auto captured = Hyprexpo::Capture::captureWindowPreview(target->targetRef, target->windowRef, MON,
                                                                       Vector2D{static_cast<double>(allocation.width), static_cast<double>(allocation.height)});
        if (!captured.completed)
            continue;
        m_cache.push_back(SCacheEntry{.key = key,
                           .targetRef = target->targetRef,
                           .windowRef = target->windowRef,
                           .framebuffer = captured.framebuffer,
                           .texture = captured.texture});
    }
}

void CScrollingOverview::render() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>(m_monitor.lock(), m_sessionGeneration));
}

void CScrollingOverview::damage() {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    m_blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(MON);
    m_blockDamageReporting = false;
}

void CScrollingOverview::onDamageReported() {
    ++m_contentDamageGeneration;
    m_damageDirty = true;
    damage();
}

void CScrollingOverview::onPreRender() {
    // Content damage must not replace the scene between press and release.
    // Config and workspace moves still invalidate input explicitly.
    if (!m_damageDirty || m_closing || m_inputState.owner != EInputOwner::None)
        return;
    m_damageDirty = false;
    if (!refreshScene()) {
        close(false);
        return;
    }
    damage();
}

void CScrollingOverview::onConfigReload() {
    resetInputState(EResetReason::Refresh);
    ++m_contentDamageGeneration;
    m_damageDirty = true;
    damage();
}

void CScrollingOverview::fullRender() {
    const auto MON = m_monitor.lock();
    if (!MON || !m_scene.valid)
        return;
    if (!m_closing && MON->m_activeWorkspace != m_startedOn) {
        close(false);
        return;
    }
    const auto transition = overviewTransition(m_transitionProgress ? m_transitionProgress->value() : 1.0,
                                               {MON->m_size.x, MON->m_size.y});
    clearWithColor(applyTransitionOpacity(CHyprColor{0.06, 0.06, 0.06, 1.0}, transition));
    CRegion damageRegion{0, 0, INT16_MAX, INT16_MAX};

    for (const auto& workspace : m_scene.workspaces) {
        CBox box = applyTransition(sceneBox(workspace.box, m_pan), {MON->m_size.x, MON->m_size.y}, transition);
        box.scale(MON->m_scale).round();
        if (box.y + box.h <= 0 || box.y >= MON->m_pixelSize.y)
            continue;
        const auto row = workspaceRow(workspace.workspaceID);
        switch (workspace.kind) {
            case EWorkspaceKind::Mixed:
                if (row && row->workspacePreview && row->workspacePreview->getTexture())
                    Render::GL::g_pHyprOpenGL->renderTextureInternal(row->workspacePreview->getTexture(), box, {.damage = &damageRegion, .a = static_cast<float>(transition.opacity), .round = 12});
                else
                    Render::GL::g_pHyprOpenGL->renderRect(box, applyTransitionOpacity(CHyprColor{0.14, 0.14, 0.14, 1.0}, transition), {.round = 12});
                break;
            case EWorkspaceKind::Empty: Render::GL::g_pHyprOpenGL->renderRect(box, applyTransitionOpacity(CHyprColor{0.10, 0.10, 0.10, 1.0}, transition), {.round = 12}); break;
            case EWorkspaceKind::Terminal: Render::GL::g_pHyprOpenGL->renderRect(box, applyTransitionOpacity(CHyprColor{0.08, 0.12, 0.10, 1.0}, transition), {.round = 12}); break;
            case EWorkspaceKind::Scrolling: break;
        }
    }

    for (const auto& target : m_renderTargets) {
        if (target.pinned && !m_showPinnedWindows)
            continue;
        CBox box = target.box;
        box.y -= m_pan;
        box = applyTransition(box, {MON->m_size.x, MON->m_size.y}, transition);
        box.scale(MON->m_scale).round();
        if (box.y + box.h <= 0 || box.y >= MON->m_pixelSize.y)
            continue;
        const auto entry = cacheEntry(target.workspaceID, target.targetToken);
        if (entry && entry->texture)
            Render::GL::g_pHyprOpenGL->renderTextureInternal(entry->texture, box, {.damage = &damageRegion, .a = static_cast<float>(transition.opacity), .round = target.fullscreen ? 0 : 8});
        else
            Render::GL::g_pHyprOpenGL->renderRect(box, applyTransitionOpacity(target.pinned ? CHyprColor{0.18, 0.12, 0.12, 1.0} : CHyprColor{0.12, 0.12, 0.12, 1.0}, transition), {.round = 8});
        const bool hovered = m_inputState.hover.kind == EHitKind::Target && m_inputState.hover.workspaceID == target.workspaceID && m_inputState.hover.targetToken == target.targetToken;
        const bool dragging = m_inputState.mode == EInputMode::WindowDrag && m_inputState.pressed.workspaceID == target.workspaceID && m_inputState.pressed.targetToken == target.targetToken;
        if (hovered || dragging)
            Render::GL::g_pHyprOpenGL->renderRect(box, applyTransitionOpacity(dragging ? CHyprColor{0.20, 0.48, 0.85, 0.20} : CHyprColor{0.85, 0.90, 1.0, 0.12}, transition), {.round = target.fullscreen ? 0 : 8});
    }
}

void CScrollingOverview::setClosing(bool closing) {
    m_closing = closing;
    if (closing) {
        m_swipeClosing = true;
        resetInputState(EResetReason::Close);
    }
}

void CScrollingOverview::beginCancelSwipe() {
    // Scrolling swipes never commit the hovered selection, including explicit cancel.
    setClosing(true);
}

bool CScrollingOverview::closeCommitted() const {
    return m_closeCommitted;
}

bool CScrollingOverview::shouldRenderOverviewForMonitor(const PHLMONITOR& requestedMonitor) const {
    return requestedMonitor && requestedMonitor == m_monitor.lock();
}

void CScrollingOverview::onWindowMoveToWorkspace(const PHLWINDOW&, const PHLWORKSPACE&) {
    resetInputState(EResetReason::Refresh);
    ++m_contentDamageGeneration;
    m_damageDirty = true;
}

void CScrollingOverview::resetSwipe() {
    m_isSwiping = false;
    m_swipeDelta = 0.0;
    m_swipeClosing = false;
}

void CScrollingOverview::onSwipeUpdate(double delta) {
    if (m_closeCommitted)
        return;
    m_isSwiping = true;
    m_swipeDelta = delta;
    const double distance = std::max<Hyprlang::INT>(1, CompatHyprlandAPI::intValue("plugin:hyprexpo:gesture_distance"));
    const float progress = static_cast<float>(transitionForSwipe(m_swipeClosing, m_swipeDelta, distance));
    m_transitionProgress->setCallbackOnEnd(nullptr);
    m_transitionProgress->setValueAndWarp(progress);
    damage();
}

void CScrollingOverview::onSwipeEnd(bool /*switchToSelection*/) {
    if (m_closeCommitted)
        return;
    m_isSwiping = false;
    m_closing = false;
    const float progress = m_transitionProgress ? m_transitionProgress->value() : 1.F;
    if (progress < 0.5F) {
        close(false);
        return;
    }
    m_swipeClosing = false;
    m_swipeDelta = 0.0;
    *m_transitionProgress = 1.F;
    damage();
}

bool CScrollingOverview::commitSelection() {
    const auto row = workspaceRow(m_selectedWorkspaceID);
    if (!row || !row->workspace)
        return false;

    PHLWINDOW selectedWindow;
    if (m_selectedStableID != 0) {
        const auto snapshot = snapshotWorkspace(row->workspace);
        if (!snapshot.success())
            return false;
        for (const auto& column : snapshot.snapshot->columns) {
            for (const auto& target : column.targets) {
                if (target.windowStableID != m_selectedStableID)
                    continue;
                selectedWindow = target.windowRef.lock();
                break;
            }
            if (selectedWindow)
                break;
        }
        if (!selectedWindow || !m_selectedTarget.lock() || m_selectedWindow.lock() != selectedWindow)
            return false;
    }

    const auto MON = m_monitor.lock();
    if (!MON)
        return false;
    if (MON->m_activeWorkspace != row->workspace || Desktop::focusState()->monitor() != MON) {
        const auto changed = Config::Actions::changeWorkspace(row->workspace);
        if (!changed)
            return false;
    }
    if (selectedWindow)
        Desktop::focusState()->fullWindowFocus(selectedWindow, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD);
    return true;
}

void CScrollingOverview::close(bool switchToSelection) {
    if (m_closeCommitted)
        return;
    resetInputState(EResetReason::Close);
    leaveOverviewSubmap(m_submapActive);
    m_closeCommitted = true;
    m_closing = true;
    if (switchToSelection)
        commitSelection();
    if (!m_transitionProgress || m_transitionProgress->value() <= 0.001F) {
        scheduleScrollingOverviewRemoval(overviewMonitorKey(m_monitor.lock()), m_sessionGeneration);
        return;
    }
    const auto generation = m_sessionGeneration;
    const auto monitorKey = overviewMonitorKey(m_monitor.lock());
    m_transitionProgress->setCallbackOnEnd(nullptr);
    m_transitionProgress->setValueAndWarp(std::min(m_transitionProgress->value(), 0.82F));
    damage();
    if (const auto MON = m_monitor.lock())
        MON->scheduleFrame();
    m_closeAnimationTimer = makeShared<CEventLoopTimer>(
        std::chrono::milliseconds{50},
        [this, monitorKey, generation](SP<CEventLoopTimer> self, void*) {
            if (overviewForSession(monitorKey, generation) != this) {
                self->cancel();
                return;
            }
            self->cancel();
            m_closeAnimationTimer.reset();
            m_transitionProgress->setCallbackOnEnd([monitorKey, generation](auto) { scheduleScrollingOverviewRemoval(monitorKey, generation); });
            *m_transitionProgress = 0.F;
            damage();
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_closeAnimationTimer);
}

void CScrollingOverview::updateSelectionFromFocus() {
    m_selectedWorkspaceID = m_focus.workspaceID;
    m_selectedStableID = 0;
    m_selectedTarget.reset();
    m_selectedWindow.reset();
    if (m_focus.kind != EHitKind::Target)
        return;
    const auto target = std::ranges::find_if(m_renderTargets, [&](const auto& item) { return item.workspaceID == m_focus.workspaceID && item.targetToken == m_focus.targetToken; });
    if (target == m_renderTargets.end())
        return;
    m_selectedStableID = target->windowStableID;
    m_selectedTarget   = target->targetRef;
    m_selectedWindow   = target->windowRef;
}

void CScrollingOverview::ensureFocusVisible() {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    std::optional<Hyprexpo::SRect> focusBox;
    if (m_focus.kind == EHitKind::Target) {
        const auto target = std::ranges::find_if(m_scene.targets, [&](const auto& item) {
            return item.workspaceID == m_focus.workspaceID && item.token == m_focus.targetToken;
        });
        if (target != m_scene.targets.end())
            focusBox = target->box;
    } else {
        const auto workspace = std::ranges::find_if(m_scene.workspaces, [&](const auto& item) { return item.workspaceID == m_focus.workspaceID; });
        if (workspace != m_scene.workspaces.end())
            focusBox = workspace->box;
    }
    if (!focusBox)
        return;
    double requested = m_pan;
    if (focusBox->y < requested)
        requested = focusBox->y;
    else if (focusBox->y + focusBox->h > requested + MON->m_size.y)
        requested = focusBox->y + focusBox->h - MON->m_size.y;
    m_pan = clampPan(m_scene, requested, MON->m_size.y);
}

bool CScrollingOverview::selectHoveredWorkspace() {
    if (m_closing || m_closeCommitted)
        return false;
    const auto point = g_pInputManager->getMouseCoordsInternal();
    const auto context = inputContext();
    const auto local = monitorLocalPoint({point.x, point.y}, context.monitor);
    if (!local)
        return false;
    const auto hit = hitTest(m_scene, *local, m_pan);
    if (hit.kind == EHitKind::Outside || hit.kind == EHitKind::TerminalWorkspace)
        return false;
    m_focus = {.kind = hit.kind, .workspaceID = hit.workspaceID, .targetToken = hit.targetToken};
    updateSelectionFromFocus();
    return true;
}

bool CScrollingOverview::onKbMoveFocus(const std::string& direction) {
    if (m_closing || m_closeCommitted)
        return false;
    EFocusDirection focusDirection;
    if (direction == "left")
        focusDirection = EFocusDirection::Left;
    else if (direction == "right")
        focusDirection = EFocusDirection::Right;
    else if (direction == "up")
        focusDirection = EFocusDirection::Up;
    else if (direction == "down")
        focusDirection = EFocusDirection::Down;
    else
        return false;
    const auto next = moveFocus(m_scene, m_focus, focusDirection);
    if (next.kind == m_focus.kind && next.workspaceID == m_focus.workspaceID && next.targetToken == m_focus.targetToken) {
        const auto across = direction == "left" ? Hyprexpo::EDirection::Left : direction == "right" ? Hyprexpo::EDirection::Right :
            direction == "up" ? Hyprexpo::EDirection::Up : Hyprexpo::EDirection::Down;
        return moveOverviewFocusAcrossMonitors(this, across);
    }
    m_focus = next;
    ensureFocusVisible();
    updateSelectionFromFocus();
    damage();
    return true;
}

bool CScrollingOverview::onKbConfirm() {
    if (m_closing || m_closeCommitted || !workspaceRow(m_focus.workspaceID))
        return false;
    updateSelectionFromFocus();
    return true;
}

bool CScrollingOverview::onKbSelectNumber(int number) {
    return selectWorkspaceByID(number);
}

bool CScrollingOverview::onKbSelectToken(int visibleIndex) {
    return visibleIndex >= 0 && selectVisibleIndex(static_cast<size_t>(visibleIndex));
}

bool CScrollingOverview::selectVisibleToken(const std::string& token) {
    if (m_closing || m_closeCommitted)
        return false;
    const int visibleIndex = fallbackTokenToVisibleIndex(token);
    return visibleIndex >= 0 && selectVisibleIndex(static_cast<size_t>(visibleIndex));
}

int64_t CScrollingOverview::selectedWorkspaceID() const {
    return m_selectedWorkspaceID;
}

bool CScrollingOverview::selectWorkspaceByID(int64_t workspaceID) {
    if (m_closing || m_closeCommitted)
        return false;
    if (!workspaceRow(workspaceID))
        return false;
    m_focus = {.kind = EHitKind::EmptyWorkspace, .workspaceID = workspaceID};
    updateSelectionFromFocus();
    return true;
}

bool CScrollingOverview::selectVisibleIndex(size_t index) {
    if (m_closing || m_closeCommitted)
        return false;
    if (index >= m_rows.size() || !m_rows[index].workspace)
        return false;
    return selectWorkspaceByID(m_rows[index].workspace->m_id);
}

bool CScrollingOverview::moveWindowBetweenVisibleIndices(size_t, size_t, const PHLWINDOW&) {
    return false;
}

bool CScrollingOverview::blocksOverviewRendering() const {
    return m_blockOverviewRendering;
}

bool CScrollingOverview::blocksDamageReporting() const {
    return m_blockDamageReporting;
}

bool CScrollingOverview::isSwiping() const {
    return m_isSwiping;
}

PHLMONITOR CScrollingOverview::monitor() const {
    return m_monitor.lock();
}

uint64_t CScrollingOverview::sessionGeneration() const {
    return m_sessionGeneration;
}

bool CScrollingOverview::ownsAnimVar(const WP<Hyprutils::Animation::CBaseAnimatedVariable>& var) const {
    return var.get() && var.get() == m_transitionProgress.get();
}

bool CScrollingOverview::ownsPointerInput() const {
    return !m_closeCommitted && m_inputState.owner == EInputOwner::Mouse;
}

bool CScrollingOverview::ownsTouchInput(int32_t touchId) const {
    return !m_closeCommitted && m_inputState.owner == EInputOwner::Touch && m_inputState.owningTouchId == touchId;
}

std::vector<std::pair<SFocusRef, Hyprexpo::SRect>> CScrollingOverview::navigationTargets() const {
    std::vector<std::pair<SFocusRef, Hyprexpo::SRect>> targets;
    for (const auto& workspace : m_scene.workspaces) {
        if (workspace.kind == EWorkspaceKind::Terminal)
            continue;
        bool hasTarget = false;
        for (const auto& target : m_scene.targets) {
            if (target.workspaceID != workspace.workspaceID)
                continue;
            targets.push_back({{EHitKind::Target, target.workspaceID, target.token}, target.box});
            hasTarget = true;
        }
        if (!hasTarget)
            targets.push_back({{workspace.kind == EWorkspaceKind::Mixed ? EHitKind::MixedWorkspace : EHitKind::EmptyWorkspace, workspace.workspaceID, 0}, workspace.box});
    }
    return targets;
}

std::vector<Hyprexpo::SGlobalTile> CScrollingOverview::globalTiles() const {
    std::vector<Hyprexpo::SGlobalTile> tiles;
    const auto MON = m_monitor.lock();
    if (!MON || m_closing || m_closeCommitted)
        return tiles;
    const auto targets = navigationTargets();
    for (size_t i = 0; i < targets.size(); ++i) {
        auto box = targets[i].second;
        box.y -= m_pan;
        const auto right = std::min(box.x + box.w, MON->m_size.x);
        const auto bottom = std::min(box.y + box.h, MON->m_size.y);
        box.x = std::max(0.0, box.x);
        box.y = std::max(0.0, box.y);
        box.w = right - box.x;
        box.h = bottom - box.y;
        if (box.w <= 0.0 || box.h <= 0.0)
            continue;
        tiles.push_back({.overviewKey = overviewMonitorKey(MON), .tileIndex = static_cast<int>(i),
                        .overviewGlobal = {MON->m_position.x, MON->m_position.y, MON->m_size.x, MON->m_size.y},
                        .tileGlobal = {MON->m_position.x + box.x, MON->m_position.y + box.y, box.w, box.h}});
    }
    return tiles;
}

std::optional<Hyprexpo::SGlobalTile> CScrollingOverview::focusedGlobalTile() const {
    const auto targets = navigationTargets();
    const auto tiles = globalTiles();
    for (const auto& tile : tiles) {
        const auto& ref = targets[tile.tileIndex].first;
        if (ref.kind == m_focus.kind && ref.workspaceID == m_focus.workspaceID && ref.targetToken == m_focus.targetToken)
            return tile;
    }
    return std::nullopt;
}

bool CScrollingOverview::setKeyboardFocus(int tileIndex) {
    if (m_closing || m_closeCommitted || tileIndex < 0)
        return false;
    const auto targets = navigationTargets();
    if (static_cast<size_t>(tileIndex) >= targets.size())
        return false;
    m_focus = targets[tileIndex].first;
    ensureFocusVisible();
    updateSelectionFromFocus();
    damage();
    return true;
}
