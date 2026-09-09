#include "HyprlandConfigCompat.hpp"
#define HyprlandAPI CompatHyprlandAPI
#include "OverviewInternal.hpp"
#include "HyprexpoLogic.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace std::chrono_literals;

bool COverview::selectHoveredWorkspace() {
    if (closing)
        return false;

    updateHoveredFromMouse();
    closeOnID = hoveredID >= 0 && hoveredID < (int)images.size() ? hoveredID : -1;
    return closeOnID != -1;
}

int64_t COverview::selectedWorkspaceID() const {
    const int id = closeOnID == -1 ? openedID : closeOnID;
    if (id < 0 || id >= (int)images.size())
        return WORKSPACE_INVALID;

    return images[id].workspaceID;
}

bool COverview::selectWorkspaceByID(int64_t workspaceID) {
    if (closing)
        return false;

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].workspaceID != workspaceID)
            continue;

        closeOnID = i;
        return true;
    }

    return false;
}

bool COverview::selectVisibleIndex(size_t index) {
    if (closing)
        return false;

    size_t visible = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].workspaceID == WORKSPACE_INVALID)
            continue;

        if (visible == index) {
            closeOnID = i;
            return true;
        }

        ++visible;
    }

    return false;
}

void COverview::updateHoveredFromMouse() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    const int newHoveredID = tileIndexAtPoint(lastMousePosLocal, size->value(), GAP_WIDTH, currentOuterInset(), true);
    if (newHoveredID == hoveredID)
        return;

    hoveredID = newHoveredID;
    damage();
}

void COverview::ensureKbFocusInitialized() {
    if (kbFocusID != -1)
        return;

    // try to set to current openedID
    if (openedID != -1) {
        kbFocusID = openedID;
        return;
    }

    // fallback: first valid tile
    for (size_t i = 0; i < images.size(); ++i) {
        if (isTileValid(i)) {
            kbFocusID = i;
            return;
        }
    }
}

bool COverview::isTileValid(int id) const {
    if (id < 0 || id >= (int)images.size())
        return false;
    return images[id].workspaceID != WORKSPACE_INVALID;
}

int COverview::tileForWorkspaceID(int wsid) const {
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].workspaceID == wsid)
            return (int)i;
    }
    return -1;
}

int COverview::tileForVisibleIndex(int vIdx) const {
    if (vIdx < 0)
        return -1;
    int seen = 0;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].workspaceID == WORKSPACE_INVALID)
            continue;
        if (seen == vIdx)
            return (int)i;
        ++seen;
    }
    return -1;
}

Vector2D COverview::tilePointToWorkspacePoint(int id, const Vector2D& localPoint) const {
    const auto MON = pMonitor.lock();
    if (!MON)
        return {};

    const auto tileBox = tileBoxForIndex(id, size->value(), GAP_WIDTH, currentOuterInset(), true);
    const Vector2D inTile = localPoint - Vector2D{tileBox.x, tileBox.y};

    return MON->m_position + Vector2D{
        std::clamp(inTile.x / std::max(1.0, tileBox.w), 0.0, 1.0) * MON->m_size.x,
        std::clamp(inTile.y / std::max(1.0, tileBox.h), 0.0, 1.0) * MON->m_size.y,
    };
}

PHLWINDOW COverview::windowAtTilePoint(int id, const Vector2D& localPoint) const {
    if (!isTileValid(id))
        return nullptr;

    PHLWORKSPACE WORKSPACE;
    if (images[id].pWorkspace) {
        WORKSPACE = images[id].pWorkspace;
    }
    else {
        for (const auto& w : State::workspaceState()->workspacesCopy()) {
            if (w->m_id == images[id].workspaceID) {
                WORKSPACE = w;
                break;
            }
        }
    }

    if (!WORKSPACE)
        return nullptr;

    const auto POINT = tilePointToWorkspacePoint(id, localPoint);
    const auto& windows = Desktop::windowState()->windows();
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        const auto& window = *it;
        if (!windowVisibleOnWorkspace(window, WORKSPACE))
            continue;

        if (window->getWindowMainSurfaceBox().containsPoint(POINT))
            return window;
    }

    return nullptr;
}

void COverview::beginWindowDrag() {
    if (g_overviewDrag.state.active)
        return;

    std::vector<Hyprexpo::SGlobalTile> tiles;
    for (const auto& session : g_overviews) {
        auto* const OV = dynamic_cast<COverview*>(session.get());
        if (!OV)
            continue;
        auto overviewTiles = OV->globalTiles();
        tiles.insert(tiles.end(), overviewTiles.begin(), overviewTiles.end());
    }

    const Vector2D GLOBAL = g_pInputManager->getMouseCoordsInternal();
    const auto     HIT    = Hyprexpo::hitTestGlobalTile({GLOBAL.x, GLOBAL.y}, tiles);
    const auto     MON    = pMonitor.lock();
    if (!HIT || !MON || HIT->overviewKey != overviewMonitorKey(MON))
        return;

    const Vector2D LOCAL{HIT->pointLocal.x, HIT->pointLocal.y};
    const auto     WINDOW = windowAtTilePoint(HIT->tileIndex, LOCAL);
    if (!WINDOW)
        return;

    auto transition = Hyprexpo::transitionOverviewDrag(
        g_overviewDrag.state,
        {.type = Hyprexpo::EOverviewDragEventType::Press, .monitorKey = HIT->overviewKey, .tileIndex = HIT->tileIndex, .windowKey = reinterpret_cast<uint64_t>(WINDOW.get())},
        liveOverviewMonitorKeys());
    if (!transition.accepted)
        return;

    transition = Hyprexpo::transitionOverviewDrag(
        transition.next, {.type = Hyprexpo::EOverviewDragEventType::Target, .monitorKey = HIT->overviewKey, .tileIndex = HIT->tileIndex}, liveOverviewMonitorKeys());

    const auto POINT              = tilePointToWorkspacePoint(HIT->tileIndex, LOCAL);
    const auto BOX                = WINDOW->getWindowMainSurfaceBox();
    g_overviewDrag.state          = transition.next;
    g_overviewDrag.window         = WINDOW;
    g_overviewDrag.pressGlobal    = GLOBAL;
    g_overviewDrag.pointerGlobal  = GLOBAL;
    g_overviewDrag.grabOffset     = POINT - Vector2D{BOX.x, BOX.y};
    Pointer::Cursor::overrideController->setOverride("grabbing", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    damage();
}

void COverview::updateWindowDrag() {
    const auto MON = pMonitor.lock();
    if (!MON || !g_overviewDrag.state.active || g_overviewDrag.state.sourceMonitorKey != overviewMonitorKey(MON))
        return;

    const Vector2D GLOBAL = g_pInputManager->getMouseCoordsInternal();
    g_overviewDrag.pointerGlobal = GLOBAL;
    const auto dx = GLOBAL.x - g_overviewDrag.pressGlobal.x;
    const auto dy = GLOBAL.y - g_overviewDrag.pressGlobal.y;
    if (!g_overviewDrag.state.moved && std::hypot(dx, dy) < 12.0)
        return;

    if (!g_overviewDrag.state.moved) {
        const auto move = Hyprexpo::transitionOverviewDrag(g_overviewDrag.state, {.type = Hyprexpo::EOverviewDragEventType::Move}, liveOverviewMonitorKeys());
        if (!move.accepted)
            return;
        g_overviewDrag.state = move.next;
    }

    std::vector<Hyprexpo::SGlobalTile> tiles;
    for (const auto& session : g_overviews) {
        auto* const OV = dynamic_cast<COverview*>(session.get());
        if (!OV)
            continue;
        auto overviewTiles = OV->globalTiles();
        tiles.insert(tiles.end(), overviewTiles.begin(), overviewTiles.end());
    }

    const auto HIT = Hyprexpo::hitTestGlobalTile({GLOBAL.x, GLOBAL.y}, tiles);
    const auto target = Hyprexpo::transitionOverviewDrag(
        g_overviewDrag.state,
        HIT ? Hyprexpo::SOverviewDragEvent{.type = Hyprexpo::EOverviewDragEventType::Target, .monitorKey = HIT->overviewKey, .tileIndex = HIT->tileIndex}
            : Hyprexpo::SOverviewDragEvent{.type = Hyprexpo::EOverviewDragEventType::Target},
        liveOverviewMonitorKeys());
    if (target.accepted)
        g_overviewDrag.state = target.next;

    for (const auto key : g_overviewDrag.state.affectedMonitorKeys) {
        if (auto* const OV = overviewForMonitorKey(key))
            OV->damage();
    }
}

PHLWORKSPACE COverview::ensureWorkspaceForTile(int id) {
    if (!isTileValid(id))
        return nullptr;

    const auto MON = pMonitor.lock();
    if (!MON)
        return nullptr;

    auto& image = images[id];
    if (image.pWorkspace)
        return image.pWorkspace;

    PHLWORKSPACE workspace;
    for (const auto& w : State::workspaceState()->workspacesCopy()) {
        if (w->m_id == image.workspaceID) {
            workspace = w;
            break;
        }
    }

    if (!workspace)
        workspace = State::workspaceState()->create(image.workspaceID, MON->m_id, std::to_string(image.workspaceID), false);

    image.pWorkspace = workspace;
    return workspace;
}

bool COverview::finishWindowDrag() {
    const auto MON = pMonitor.lock();
    if (!MON || !g_overviewDrag.state.active || g_overviewDrag.state.sourceMonitorKey != overviewMonitorKey(MON))
        return false;

    const auto STATE      = g_overviewDrag.state;
    const auto TRANSITION = Hyprexpo::transitionOverviewDrag(STATE, {.type = Hyprexpo::EOverviewDragEventType::Release}, liveOverviewMonitorKeys());
    const bool CONSUMED   = STATE.moved;

    if (TRANSITION.drop && g_overviewDrag.window && reinterpret_cast<uint64_t>(g_overviewDrag.window.get()) == TRANSITION.drop->windowKey) {
        auto* const SOURCEOV  = gridOverviewForMonitorKey(TRANSITION.drop->sourceMonitorKey);
        auto* const TARGETOV  = gridOverviewForMonitorKey(TRANSITION.drop->targetMonitorKey);
        const auto  TARGETMON = TARGETOV ? TARGETOV->pMonitor.lock() : PHLMONITOR{};
        const int   SOURCE    = TRANSITION.drop->sourceTileIndex;
        const int   TARGET    = TRANSITION.drop->targetTileIndex;

        if (SOURCEOV && TARGETOV && TARGETMON && SOURCEOV->isTileValid(SOURCE) && TARGETOV->isTileValid(TARGET)) {
            PHLWORKSPACE SOURCEWS = SOURCEOV->images[SOURCE].pWorkspace;
            if (!SOURCEWS) {
                for (const auto& workspace : State::workspaceState()->workspacesCopy()) {
                    if (workspace->m_id == SOURCEOV->images[SOURCE].workspaceID) {
                        SOURCEWS = workspace;
                        break;
                    }
                }
            }

            const auto TARGETWS = TARGETOV->ensureWorkspaceForTile(TARGET);
            if (TARGETWS && TARGETWS->m_monitor.lock() != TARGETMON)
                Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] rejected drag target workspace on the wrong monitor");
            else if (windowVisibleOnWorkspace(g_overviewDrag.window, SOURCEWS) && TARGETWS && TARGETWS != SOURCEWS) {
                const int64_t SOURCEWORKSPACEID = SOURCEOV->images[SOURCE].workspaceID;
                const int64_t TARGETWORKSPACEID = TARGETOV->images[TARGET].workspaceID;
                SOURCEOV->images[SOURCE].pWorkspace = SOURCEWS;
                Desktop::globalWindowController()->moveWindowToWorkspace(g_overviewDrag.window, TARGETWS);
                settleWorkspaceMoveAnimation(g_overviewDrag.window);
                SOURCEOV->redrawDraggedWorkspace(SOURCEWORKSPACEID);
                TARGETOV->redrawDraggedWorkspace(TARGETWORKSPACEID);
            }
        }
    }

    resetOverviewDrag(Hyprexpo::EOverviewDragEventType::Release);
    return CONSUMED;
}

bool COverview::moveWindowBetweenVisibleIndices(size_t sourceIndex, size_t targetIndex, const PHLWINDOW& requestedWindow) {
    if (closing)
        return false;

    const int SOURCE = tileForVisibleIndex(sourceIndex);
    const int TARGET = tileForVisibleIndex(targetIndex);
    if (!isTileValid(SOURCE) || !isTileValid(TARGET) || SOURCE == TARGET)
        return false;

    PHLWORKSPACE SOURCEWS;
    if (images[SOURCE].pWorkspace) {
        SOURCEWS = images[SOURCE].pWorkspace;
    }
    else {
        for (const auto& w : State::workspaceState()->workspacesCopy()) {
            if (w->m_id == images[SOURCE].workspaceID) {
                SOURCEWS = w;
                break;
            }
        }
    }

    const auto TARGETWS = ensureWorkspaceForTile(TARGET);

    if (!SOURCEWS || !TARGETWS || SOURCEWS == TARGETWS)
        return false;

    PHLWINDOW window = requestedWindow;
    if (window) {
        if (!windowVisibleOnWorkspace(window, SOURCEWS))
            return false;
    } else {
        const auto& windows = Desktop::windowState()->windows();
        for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
            const auto& candidate = *it;
            if (!windowVisibleOnWorkspace(candidate, SOURCEWS))
                continue;

            window = candidate;
            break;
        }
    }

    if (!window)
        return false;

    images[SOURCE].pWorkspace = SOURCEWS;
    const int64_t SOURCEWORKSPACEID = images[SOURCE].workspaceID;
    const int64_t TARGETWORKSPACEID = images[TARGET].workspaceID;
    Desktop::globalWindowController()->moveWindowToWorkspace(window, TARGETWS);
    settleWorkspaceMoveAnimation(window);
    redrawDraggedWorkspace(SOURCEWORKSPACEID);
    redrawDraggedWorkspace(TARGETWORKSPACEID);
    return true;
}

void COverview::redrawDraggedWorkspace(int64_t workspaceID) {
    if (workspaceID == WORKSPACE_INVALID)
        return;

    if (std::find(settlingRedrawWorkspaceIDs.begin(), settlingRedrawWorkspaceIDs.end(), workspaceID) == settlingRedrawWorkspaceIDs.end())
        settlingRedrawWorkspaceIDs.push_back(workspaceID);
    redrawSettleTicks = 8;

    queueRedrawID(tileForWorkspaceID(workspaceID));
    flushQueuedRedraws();

    if (redrawSettleTimer)
        return;

    const auto OVERVIEWKEY = overviewMonitorKey(pMonitor.lock());
    const auto GENERATION = m_sessionGeneration;
    if (OVERVIEWKEY == 0)
        return;

    redrawSettleTimer = makeShared<CEventLoopTimer>(
        75ms,
        [OVERVIEWKEY, GENERATION](SP<CEventLoopTimer> self, void*) {
            auto* const OVERVIEW = dynamic_cast<COverview*>(overviewForSession(OVERVIEWKEY, GENERATION));
            if (!OVERVIEW || OVERVIEW->redrawSettleTimer.get() != self.get()) {
                self->cancel();
                return;
            }
            if (OVERVIEW->closing) {
                self->cancel();
                OVERVIEW->redrawSettleTimer.reset();
                return;
            }

            for (const auto workspaceID : OVERVIEW->settlingRedrawWorkspaceIDs)
                OVERVIEW->queueRedrawID(OVERVIEW->tileForWorkspaceID(workspaceID));

            OVERVIEW->flushQueuedRedraws();

            if (--OVERVIEW->redrawSettleTicks <= 0) {
                OVERVIEW->settlingRedrawWorkspaceIDs.clear();
                OVERVIEW->redrawSettleTimer.reset();
                self->cancel();
                return;
            }

            self->updateTimeout(100ms);
        },
        nullptr);
    g_pEventLoopManager->addTimer(redrawSettleTimer);
}

void COverview::queueRedrawID(int id) {
    if (!isTileValid(id))
        return;

    if (std::find(queuedRedrawIDs.begin(), queuedRedrawIDs.end(), id) == queuedRedrawIDs.end())
        queuedRedrawIDs.push_back(id);
}

void COverview::flushQueuedRedraws() {
    if (queuedRedrawIDs.empty())
        return;

    const auto ids = queuedRedrawIDs;
    queuedRedrawIDs.clear();

    for (const auto id : ids)
        redrawID(id);

    damage();
    if (const auto MON = pMonitor.lock())
        MON->scheduleFrame();
}

bool COverview::selectVisibleToken(const std::string& token) {
    if (closing)
        return false;

    static auto* const* PSELECTLABEL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_enable")->getDataStaticPtr();
    static auto const*  PSELECTMAP   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_token_map")->getDataStaticPtr();

    const std::string normalized = lowerString(trimString(token));
    if (normalized.empty())
        return false;

    if (**PSELECTLABEL) {
        const auto tokens = splitCommaList(std::string{*PSELECTMAP});
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i].empty() || lowerString(tokens[i]) != normalized)
                continue;

            return selectVisibleIndex(i);
        }

        return false;
    }

    const int visibleIndex = fallbackTokenToVisibleIndex(normalized);
    if (visibleIndex < 0)
        return false;

    return selectVisibleIndex(visibleIndex);
}

bool COverview::moveFocus(int dx, int dy) {
    ensureKbFocusInitialized();
    if (kbFocusID == -1)
        return false;

    const auto shape = currentGridShape();
    int x = kbFocusID % shape.cols;
    int y = kbFocusID / shape.cols;

    static auto* const* PWRAPH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:keynav_wrap_h")->getDataStaticPtr();
    static auto* const* PWRAPV = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:keynav_wrap_v")->getDataStaticPtr();

    if (dx != 0) {
        static auto* const* PREADING = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:keynav_reading_order")->getDataStaticPtr();
        int                 step     = dx > 0 ? 1 : -1;
        if (**PREADING) {
            // reading-order scan: proceed linearly across the grid (row-major)
            const int total = (int)images.size();
            int       idx   = kbFocusID;
            for (int tries = 0; tries < total; ++tries) {
                idx += step;
                if (idx < 0 || idx >= total) {
                    // wrap only if both wraps are enabled (edge of grid)
                    if (**PWRAPH && **PWRAPV)
                        idx = (idx + total) % total;
                    else
                        break;
                }
                if (isTileValid(idx)) {
                    kbFocusID = idx;
                    return true;
                }
            }
        } else {
            // in-row scan with optional horizontal wrap
            int nx = x;
            for (int tries = 0; tries < shape.cols; ++tries) {
                nx += step;
                if (nx < 0 || nx >= shape.cols) {
                    if (**PWRAPH)
                        nx = (nx + shape.cols) % shape.cols;
                    else
                        break;
                }
                const int nid = nx + y * shape.cols;
                if (isTileValid(nid)) {
                    kbFocusID = nid;
                    return true;
                }
            }
        }
    }

    if (dy != 0) {
        int step = dy > 0 ? 1 : -1;
        int ny   = y;
        for (int tries = 0; tries < shape.rows; ++tries) {
            ny += step;
            if (ny < 0 || ny >= shape.rows) {
                if (**PWRAPV)
                    ny = (ny + shape.rows) % shape.rows;
                else
                    break;
            }
            const int nid = x + ny * shape.cols;
            if (isTileValid(nid)) {
                kbFocusID = nid;
                return true;
            }
        }
    }
    return false;
}

bool COverview::onKbMoveFocus(const std::string& dir) {
    if (closing)
        return false;

    int                   dx = 0;
    int                   dy = 0;
    Hyprexpo::EDirection direction;
    if (dir == "left") {
        dx        = -1;
        direction = Hyprexpo::EDirection::Left;
    } else if (dir == "right") {
        dx        = 1;
        direction = Hyprexpo::EDirection::Right;
    } else if (dir == "up") {
        dy        = -1;
        direction = Hyprexpo::EDirection::Up;
    } else if (dir == "down") {
        dy        = 1;
        direction = Hyprexpo::EDirection::Down;
    } else
        return false;

    if (moveFocus(dx, dy)) {
        damage();
        return true;
    }

    return moveOverviewFocusAcrossMonitors(this, direction);
}

bool COverview::onKbConfirm() {
    if (closing)
        return false;
    ensureKbFocusInitialized();
    if (!isTileValid(kbFocusID))
        return false;
    closeOnID = kbFocusID;
    return true;
}

bool COverview::onKbSelectNumber(int num) {
    if (closing)
        return false;

    if (num == 0)
        num = 10;

    return selectWorkspaceByID(num);
}

bool COverview::onKbSelectToken(int visibleIdx) {
    if (closing)
        return false;
    if (visibleIdx < 0)
        return false;
    return selectVisibleIndex(visibleIdx);
}

static float lerpFloat(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D lerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{lerpFloat(from.x, to.x, perc), lerpFloat(from.y, to.y, perc)};
}

void COverview::setClosing(bool closing_) {
    closing = closing_;
}

void COverview::beginCancelSwipe() {
    closeOnID = openedID;
    closing   = true;
}

void COverview::onWindowMoveToWorkspace(const PHLWINDOW& window, const PHLWORKSPACE& workspace) {
    if (!closing || externalWorkspaceMoveDuringClose || !window)
        return;

    const auto monitor = pMonitor.lock();
    if (!monitor)
        return;

    const bool movedOnOverviewMonitor = window->m_monitor == monitor || (window->m_workspace && window->m_workspace->m_monitor == monitor) || (workspace && workspace->m_monitor == monitor);
    if (!Hyprexpo::shouldAbortOverviewCloseForWorkspaceMove((window->m_state & Desktop::View::WINDOW_STATE_PINNED) != Desktop::View::WINDOW_STATE_NONE, movedOnOverviewMonitor))
        return;

    externalWorkspaceMoveDuringClose = true;
    damage();
    monitor->scheduleFrame();
}

void COverview::resetSwipe() {
    swipeWasCommenced = false;
}

void COverview::onSwipeUpdate(double delta) {
    // Once the close animation is committed, ignore further swipe input so a
    // re-grabbed gesture can't warp size/pos back and replay the close.
    if (m_closeCommitted)
        return;

    m_isSwiping = true;

    const auto MON = pMonitor.lock();
    if (!MON) {
        m_isSwiping = false;
        return;
    }

    if (swipeWasCommenced)
        return;

    static auto* const* PDISTANCE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:gesture_distance")->getDataStaticPtr();
    const double        distance  = std::max<Hyprlang::INT>(1, **PDISTANCE);

    const float         PERC               = closing ? std::clamp(delta / distance, 0.0, 1.0) : 1.0 - std::clamp(delta / distance, 0.0, 1.0);
    const auto          WORKSPACE_FOCUS_ID = closing && closeOnID != -1 ? closeOnID : openedID;

    const auto          SIZEMAX = zoomSizeForCurrentGrid(MON->m_size);
    const auto          POSMAX  = -(tilePosForID(WORKSPACE_FOCUS_ID, SIZEMAX, 0.0) * MON->m_scale);

    const auto SIZEMIN = MON->m_size;
    const auto POSMIN  = Vector2D{0, 0};

    size->setCallbackOnEnd(nullptr);
    pos->setCallbackOnEnd(nullptr);

    size->setValueAndWarp(lerp(SIZEMIN, SIZEMAX, PERC));
    pos->setValueAndWarp(lerp(POSMIN, POSMAX, PERC));
}

void COverview::onSwipeEnd(bool switchToSelection) {
    if (m_closeCommitted)
        return;

    const auto MON = pMonitor.lock();
    if (!MON) {
        m_isSwiping       = false;
        swipeWasCommenced = false;
        closing           = true;
        destroyOverview(this);
        return;
    }

    const auto SIZEMIN = MON->m_size;
    const auto SIZEMAX = zoomSizeForCurrentGrid(MON->m_size);
    const auto span    = SIZEMAX - SIZEMIN;
    if (std::abs(span.x) <= 1e-6) {
        close(switchToSelection);
        return;
    }
    const auto PERC    = (size->value() - SIZEMIN).x / span.x;
    if (PERC > 0.5) {
        close(switchToSelection);
        return;
    }
    *size = MON->m_size;
    *pos  = {0, 0};

    size->setCallbackOnEnd([this](WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) { redrawAll(true); });

    swipeWasCommenced = true;
    m_isSwiping       = false;
}

// The submap is compositor-global, but every overview enters and leaves it
// independently. Refcount it so opening on several monitors installs it once
// and only the last overview to close tears it down: without this the first
// monitor to close would drop keyboard navigation for the ones still open.
static int         g_submapRefs     = 0;
static std::string g_previousSubmap = "";

void enterOverviewSubmap(bool& submapActive) {
    static auto* const* PKEYNAV = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:keynav_enable")->getDataStaticPtr();
    if (!**PKEYNAV || submapActive)
        return;

    if (g_submapRefs++ == 0) {
        // remember whatever submap was active so we can restore it exactly on close, instead
        // of always dropping back to Hyprland's bare default submap. Configs that nest their
        // entire keybind set inside a named submap (a common pattern for e.g. a "disable all
        // keybinds" toggle) would otherwise get silently kicked out of it every time the
        // overview closes.
        //
        // The capture is global, not per-overview: only the first overview to open sees
        // the user's real submap. The others would capture "hyprexpo" and restore that.
        g_previousSubmap = Keybinds::mgr()->currentSubmap();
        // switch to a dedicated submap for hyprexpo navigation
        (void)Config::Actions::setSubmap("hyprexpo");
    }
    submapActive = true;
}

void leaveOverviewSubmap(bool& submapActive) {
    if (!submapActive)
        return;

    if (--g_submapRefs <= 0) {
        g_submapRefs = 0;
        // Restore what was captured on the way in, not this instance's copy: the overview
        // that closes last is not necessarily the one that opened first.
        (void)Config::Actions::setSubmap(g_previousSubmap);
    }
    submapActive = false;
}

void COverview::enterSubmapIfEnabled() {
    enterOverviewSubmap(submapActive);
}

void COverview::resetSubmapIfNeeded() {
    leaveOverviewSubmap(submapActive);
}
