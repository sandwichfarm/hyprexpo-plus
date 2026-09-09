#include "ScrollingLayoutAdapter.hpp"

#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowMetadata.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <ranges>
#include <stdexcept>
#include <unordered_map>

namespace Hyprexpo::Scrolling {

namespace {

uintptr_t fingerprintPointer(const void* pointer) {
    if (!pointer)
        return 0;
    static const int saltAnchor = 0;
    uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer) ^ reinterpret_cast<uintptr_t>(&saltAnchor));
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<uintptr_t>(value);
}

template <typename T>
uintptr_t fingerprint(const SP<T>& value) {
    return fingerprintPointer(value.get());
}

bool validPositive(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool validBox(const CBox& box) {
    return std::isfinite(box.x) && std::isfinite(box.y) && validPositive(box.w) && validPositive(box.h);
}

std::string directionName(Layout::Tiled::eScrollDirection direction) {
    switch (direction) {
        case Layout::Tiled::SCROLL_DIR_RIGHT: return "right";
        case Layout::Tiled::SCROLL_DIR_LEFT: return "left";
        case Layout::Tiled::SCROLL_DIR_DOWN: return "down";
        case Layout::Tiled::SCROLL_DIR_UP: return "up";
    }
    return "unknown";
}

std::optional<Layout::Tiled::eScrollDirection> parseDirection(const std::string& direction) {
    if (direction == "right")
        return Layout::Tiled::SCROLL_DIR_RIGHT;
    if (direction == "left")
        return Layout::Tiled::SCROLL_DIR_LEFT;
    if (direction == "down")
        return Layout::Tiled::SCROLL_DIR_DOWN;
    if (direction == "up")
        return Layout::Tiled::SCROLL_DIR_UP;
    return std::nullopt;
}

SSnapshotResult failure(ESnapshotFailure code, std::string error) {
    return {.failure = code, .error = std::move(error), .snapshot = std::nullopt};
}

STargetSnapshot copyTarget(const SP<Layout::ITarget>& target, const CBox& layoutBox, size_t rowIndex, float proportion, bool visible) {
    const auto window = target->window();
    return {
        .rowIndex = rowIndex,
        .proportion = proportion,
        .targetFingerprint = fingerprint(target),
        .windowFingerprint = fingerprint(window),
        .windowStableID = window ? window->metadata().stableID() : 0,
        .layoutBox = layoutBox,
        .group = target->type() == Layout::TARGET_TYPE_GROUP,
        .floating = target->floating(),
        .fullscreen = window && Fullscreen::controller() && Fullscreen::controller()->isFullscreen(window),
        .pinned = window && (window->m_state & Desktop::View::WINDOW_STATE_PINNED),
        .visible = visible,
        .targetRef = target,
        .windowRef = window,
    };
}

SSnapshotResult snapshotWorkspaceImpl(const PHLWORKSPACE& workspace) {
    if (!workspace)
        return failure(ESnapshotFailure::NullWorkspace, "workspace is null");
    if (workspace->inert())
        return failure(ESnapshotFailure::InertWorkspace, "workspace is inert");

    const auto monitor = workspace->m_monitor.lock();
    if (!monitor)
        return failure(ESnapshotFailure::MissingMonitor, "workspace monitor expired");
    if (!workspace->m_space)
        return failure(ESnapshotFailure::MissingSpace, "workspace space is unavailable");

    const auto algorithmOwner = workspace->m_space->algorithm();
    if (!algorithmOwner)
        return failure(ESnapshotFailure::MissingAlgorithm, "workspace algorithm is unavailable");
    const auto& tiledOwner = algorithmOwner->tiledAlgo();
    if (!tiledOwner)
        return failure(ESnapshotFailure::MissingTiledAlgorithm, "workspace tiled algorithm is unavailable");

    auto* const tiled = tiledOwner.get();
    if (Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(tiled) != "scrolling")
        return failure(ESnapshotFailure::WrongAlgorithmName, "workspace tiled algorithm is not scrolling");

    auto* const scrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(tiled);
    if (!scrolling)
        return failure(ESnapshotFailure::CastFailure, "scrolling matcher succeeded but dynamic cast failed");

    std::vector<SP<Layout::ITarget>> layoutTargets;
    layoutTargets.reserve(workspace->m_space->targets().size());
    for (const auto& targetRef : workspace->m_space->targets()) {
        const auto target = targetRef.lock();
        if (!target)
            return failure(ESnapshotFailure::ExpiredTarget, "workspace target expired while resolving scrolling data");
        layoutTargets.push_back(target);
    }

    SP<Layout::Tiled::SScrollingData> data;
    for (const auto& target : layoutTargets) {
        if (target->floating())
            continue;
        const auto targetData = scrolling->dataFor(target);
        if (!targetData)
            continue;
        const auto column = targetData->column.lock();
        if (!column)
            return failure(ESnapshotFailure::ExpiredColumn, "scrolling target column expired");
        data = column->scrollingData.lock();
        if (!data)
            return failure(ESnapshotFailure::ExpiredData, "scrolling column data expired");
        break;
    }

    if (!data || !data->controller)
        return failure(ESnapshotFailure::MissingScrollingData, "no live scrolling data/controller is available");
    const auto& controller = *data->controller;
    if (data->columns.size() != controller.stripCount())
        return failure(ESnapshotFailure::ColumnCardinalityMismatch, "column/controller strip cardinality mismatch");

    SWorkspaceSnapshot snapshot{
        .workspaceID = workspace->m_id,
        .monitorID = monitor->m_id,
        .algorithmFingerprint = fingerprintPointer(scrolling),
        .dataFingerprint = fingerprint(data),
        .direction = directionName(controller.getDirection()),
        .offset = controller.getOffset(),
        .activeWorkspaceID = monitor->m_activeWorkspace ? monitor->m_activeWorkspace->m_id : 0,
        .focusedWindowFingerprint = fingerprint(Desktop::focusState() ? Desktop::focusState()->window() : PHLWINDOW{}),
        .columns = {},
        .layoutTargets = {},
    };
    if (!std::isfinite(snapshot.offset))
        return failure(ESnapshotFailure::InvalidGeometry, "scrolling offset is not finite");

    const auto usable = scrolling->usableArea();
    if (!validBox(usable))
        return failure(ESnapshotFailure::InvalidGeometry, "scrolling usable area is invalid");
    const bool horizontal = controller.getDirection() == Layout::Tiled::SCROLL_DIR_RIGHT || controller.getDirection() == Layout::Tiled::SCROLL_DIR_LEFT;
    const double viewportStart = horizontal ? usable.x : usable.y;
    const double viewportEnd = viewportStart + (horizontal ? usable.w : usable.h);

    snapshot.columns.reserve(data->columns.size());
    for (size_t columnIndex = 0; columnIndex < data->columns.size(); ++columnIndex) {
        const auto& column = data->columns[columnIndex];
        if (!column)
            return failure(ESnapshotFailure::ExpiredColumn, "scrolling column is null");
        const auto columnData = column->scrollingData.lock();
        if (!columnData || columnData != data)
            return failure(ESnapshotFailure::ExpiredData, "scrolling column owner expired or changed");

        const auto& strip = controller.getStrip(columnIndex);
        const auto stripColumn = strip.userData.lock();
        if (!stripColumn || stripColumn != column)
            return failure(ESnapshotFailure::ExpiredColumn, "controller strip column expired or changed");
        if (strip.targetSizes.size() != column->targetDatas.size())
            return failure(ESnapshotFailure::TargetCardinalityMismatch, "column/controller target cardinality mismatch");
        if (!validPositive(strip.size))
            return failure(ESnapshotFailure::InvalidGeometry, "scrolling column width is invalid");

        const double primaryStart = controller.calculateStripStart(columnIndex, usable);
        const double primarySize = controller.calculateStripSize(columnIndex, usable);
        if (!std::isfinite(primaryStart) || !validPositive(primarySize))
            return failure(ESnapshotFailure::InvalidGeometry, "scrolling column placement is invalid");
        const bool visible = primaryStart < viewportEnd && primaryStart + primarySize > viewportStart;

        SColumnSnapshot columnSnapshot{
            .index = columnIndex,
            .fingerprint = fingerprint(column),
            .width = strip.size,
            .primaryStart = primaryStart,
            .primarySize = primarySize,
            .visible = visible,
            .targets = {},
        };
        columnSnapshot.targets.reserve(column->targetDatas.size());
        for (size_t rowIndex = 0; rowIndex < column->targetDatas.size(); ++rowIndex) {
            const auto& targetData = column->targetDatas[rowIndex];
            if (!targetData)
                return failure(ESnapshotFailure::ExpiredTarget, "scrolling target data is null");
            const auto targetColumn = targetData->column.lock();
            if (!targetColumn || targetColumn != column)
                return failure(ESnapshotFailure::ExpiredColumn, "scrolling target column expired or changed");
            const auto target = targetData->target.lock();
            if (!target)
                return failure(ESnapshotFailure::ExpiredTarget, "scrolling target expired while copying topology");
            if (!validPositive(strip.targetSizes[rowIndex]) || !validBox(targetData->layoutBox))
                return failure(ESnapshotFailure::InvalidGeometry, "scrolling target size/layout box is invalid");
            columnSnapshot.targets.push_back(copyTarget(target, targetData->layoutBox, rowIndex, strip.targetSizes[rowIndex], visible));
        }
        snapshot.columns.push_back(std::move(columnSnapshot));
    }

    snapshot.layoutTargets.reserve(layoutTargets.size());
    for (const auto& target : layoutTargets) {
        const auto box = target->position();
        if (!validBox(box))
            return failure(ESnapshotFailure::InvalidGeometry, "layout target box is invalid");
        snapshot.layoutTargets.push_back(copyTarget(target, box, 0, 0.F, workspace->m_visible));
    }

    return {.failure = ESnapshotFailure::None, .error = {}, .snapshot = std::move(snapshot)};
}

uint64_t mutationTargetIdentity(const STargetSnapshot& target) {
    return target.targetFingerprint ? static_cast<uint64_t>(target.targetFingerprint) : target.windowStableID;
}

struct SResolvedNativeWorkspace {
    PHLWORKSPACE                           workspace;
    Layout::Tiled::CScrollingAlgorithm*    algorithm = nullptr;
    SP<Layout::Tiled::SScrollingData>      data;
};

SResolvedNativeWorkspace resolveNativeWorkspace(const PHLWORKSPACE& workspace, const SP<Layout::ITarget>& seed = {}) {
    if (!workspace || workspace->inert() || !workspace->m_space)
        throw std::runtime_error("native workspace is unavailable");
    const auto owner = workspace->m_space->algorithm();
    if (!owner || !owner->tiledAlgo())
        throw std::runtime_error("native tiled algorithm is unavailable");
    auto* const tiled = owner->tiledAlgo().get();
    if (Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(tiled) != "scrolling")
        throw std::runtime_error("native destination is not scrolling");
    auto* const scrolling = dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(tiled);
    if (!scrolling)
        throw std::runtime_error("native scrolling dynamic cast failed");

    SP<Layout::ITarget> candidate = seed;
    if (!candidate) {
        for (const auto& weak : workspace->m_space->targets()) {
            candidate = weak.lock();
            if (candidate && !candidate->floating() && scrolling->dataFor(candidate))
                break;
            candidate.reset();
        }
    }
    if (!candidate)
        throw std::runtime_error("native scrolling seed target expired");
    const auto targetData = scrolling->dataFor(candidate);
    const auto column = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
    const auto data = column ? column->scrollingData.lock() : SP<Layout::Tiled::SScrollingData>{};
    if (!targetData || !column || !data || !data->controller || data->columns.size() != data->controller->stripCount())
        throw std::runtime_error("native scrolling ownership chain changed");
    return {.workspace = workspace, .algorithm = scrolling, .data = data};
}

SMutationWorkspace mutationWorkspace(const SWorkspaceSnapshot& snapshot) {
    SMutationWorkspace result{
        .workspaceID = snapshot.workspaceID,
        .modelIdentity = snapshot.dataFingerprint,
        .kind = EMutationWorkspaceKind::Scrolling,
        .direction = snapshot.direction,
        .offset = snapshot.offset,
        .focusedTargetIdentity = 0,
        .focusedWindowIdentity = snapshot.focusedWindowFingerprint,
        .columns = {},
        .members = {},
    };
    result.columns.reserve(snapshot.columns.size());
    for (const auto& column : snapshot.columns) {
        SMutationColumn copied{.identity = column.fingerprint, .width = column.width, .targets = {}};
        copied.targets.reserve(column.targets.size());
        for (const auto& target : column.targets) {
            const auto identity = mutationTargetIdentity(target);
            copied.targets.push_back({.identity = identity, .windowIdentity = target.windowFingerprint, .size = target.proportion,
                                      .group = target.group, .fullscreen = target.fullscreen});
            if (target.windowFingerprint == snapshot.focusedWindowFingerprint)
                result.focusedTargetIdentity = identity;
        }
        result.columns.push_back(std::move(copied));
    }
    result.members.reserve(snapshot.layoutTargets.size());
    for (const auto& target : snapshot.layoutTargets)
        result.members.push_back(mutationTargetIdentity(target));
    return result;
}

class CNativeMutationOperations final : public IMutationOperations {
  public:
    CNativeMutationOperations(PHLWORKSPACE sourceWorkspace, PHLWORKSPACE destinationWorkspace, PHLMONITOR initiatingMonitor, std::optional<SFaultInjection> fault) :
        m_sourceWorkspace(std::move(sourceWorkspace)), m_destinationWorkspace(std::move(destinationWorkspace)), m_monitor(std::move(initiatingMonitor)), m_fault(fault) {}

    void checkpoint(EMutationPhase phase, EMutationStep step, EFaultWhen when) override {
        if (!m_fault || m_fault->phase != phase || m_fault->step != step || m_fault->when != when)
            return;
        m_fault.reset();
        throw std::runtime_error("injected request-scoped native mutation fault");
    }

    SMutationState snapshotPreState(const SMutationRequest& request) override {
        if (m_preState)
            return *m_preState;
        if (!m_sourceWorkspace || request.sourceWorkspaceID != m_sourceWorkspace->m_id)
            throw std::runtime_error("source workspace changed before snapshot");
        if (!request.createDestination && (!m_destinationWorkspace || request.destinationWorkspaceID != m_destinationWorkspace->m_id))
            throw std::runtime_error("destination workspace changed before snapshot");

        SMutationState state;
        snapshotAndRetain(m_sourceWorkspace, state);
        if (m_destinationWorkspace && m_destinationWorkspace != m_sourceWorkspace) {
            if (workspaceUsesScrollingLayout(m_destinationWorkspace))
                snapshotScrollingOrEmptyAndRetain(m_destinationWorkspace, state);
            else
                snapshotMixedAndRetain(m_destinationWorkspace, state, true);
        }
        m_preState = state;
        return state;
    }

    void removeTarget(const SMutationPlan& plan) override {
        auto& source = nativeFor(plan.request.sourceWorkspaceID);
        const auto target = targetFor(plan.request.targetIdentity);
        const auto resolved = resolveNativeWorkspace(source.workspace, target);
        const auto targetData = resolved.algorithm->dataFor(target);
        const auto column = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
        if (!targetData || !column || column->scrollingData.lock() != resolved.data || !column->has(target))
            throw std::runtime_error("source target failed strict weak re-resolution");
        column->remove(target);
        source.data = resolved.data;
    }

    void controllerMove(const SMutationPlan& plan, bool reverse) override {
        if (!plan.crossWorkspace)
            throw std::runtime_error("controller move requested for a same-workspace transaction");
        if (!reverse && !m_destinationWorkspace) {
            if (!plan.createDestination || !m_monitor || plan.request.destinationWorkspaceID <= 0)
                throw std::runtime_error("terminal destination cannot be created");
            m_destinationWorkspace = State::workspaceState()->create(plan.request.destinationWorkspaceID, m_monitor->m_id,
                                                                      std::to_string(plan.request.destinationWorkspaceID), true);
            if (!m_destinationWorkspace)
                throw std::runtime_error("terminal destination creation failed");
            m_createdDestination = m_destinationWorkspace;
            m_native[plan.request.destinationWorkspaceID] = {.workspace = m_destinationWorkspace, .kind = EMutationWorkspaceKind::Scrolling, .data = {}};
        }

        const auto target = targetFor(plan.request.targetIdentity);
        const auto window = target ? target->window() : PHLWINDOW{};
        PHLWORKSPACE from = m_sourceWorkspace;
        PHLWORKSPACE to = m_destinationWorkspace;
        if (reverse) {
            from = m_destinationWorkspace;
            to = m_sourceWorkspace;
        }
        if (!window || !from || !to || window->m_workspace != from)
            throw std::runtime_error("controller move source ownership changed");
        Desktop::globalWindowController()->moveWindowToWorkspace(window, to);
        if (window->m_workspace != to)
            throw std::runtime_error("controller move did not establish destination ownership");
    }

    void reResolve(const SMutationPlan& plan, bool rollback) override {
        const auto target = targetFor(plan.request.targetIdentity);
        auto& current = nativeFor(rollback ? plan.request.sourceWorkspaceID : plan.request.destinationWorkspaceID);
        if (current.kind == EMutationWorkspaceKind::Mixed) {
            const auto window = target->window();
            if (!window || window->m_workspace != current.workspace)
                throw std::runtime_error("mixed destination ownership failed strict re-resolution");
            return;
        }
        const auto resolved = resolveNativeWorkspace(current.workspace, target);
        const auto targetData = resolved.algorithm->dataFor(target);
        if (!targetData || !targetData->column.lock() || targetData->column->scrollingData.lock() != resolved.data)
            throw std::runtime_error("moved target failed strict destination re-resolution");
        current.data = resolved.data;
    }

    void addTarget(const SMutationPlan& plan) override {
        auto& destination = nativeFor(plan.request.destinationWorkspaceID);
        const auto target = targetFor(plan.request.targetIdentity);
        if (destination.kind != EMutationWorkspaceKind::Scrolling)
            throw std::runtime_error("positional add requested for a mixed destination");
        if (plan.crossWorkspace && plan.request.placement == EColumnPlacement::None)
            return;
        if (plan.crossWorkspace) {
            const auto resolved = resolveNativeWorkspace(destination.workspace, target);
            const auto targetData = resolved.algorithm->dataFor(target);
            const auto currentColumn = targetData ? targetData->column.lock() : SP<Layout::Tiled::SColumnData>{};
            if (!targetData || !currentColumn || !currentColumn->has(target))
                throw std::runtime_error("automatic destination placement expired before positional add");
            currentColumn->remove(target);
            destination.data = resolved.data;
        }
        auto data = destination.data;
        if (!data || !data->controller)
            throw std::runtime_error("retained native scrolling model expired");

        SP<Layout::Tiled::SColumnData> column;
        if (plan.request.placement == EColumnPlacement::Existing) {
            if (plan.request.destinationColumnIndex >= data->columns.size())
                throw std::runtime_error("destination column index changed before add");
            column = data->columns[plan.request.destinationColumnIndex];
        } else {
            if (plan.request.destinationColumnIndex > data->columns.size())
                throw std::runtime_error("new column index changed before add");
            column = data->add(static_cast<int>(plan.request.destinationColumnIndex) - 1, static_cast<float>(plan.sourceColumnWidth));
        }
        if (!column || column->scrollingData.lock() != data)
            throw std::runtime_error("destination column failed strict re-resolution");
        const int after = static_cast<int>(plan.request.destinationRowIndex) - 1;
        if (plan.request.destinationRowIndex > column->targetDatas.size())
            throw std::runtime_error("destination row index changed before add");
        column->add(target, after);
    }

    void restorePreState(const SMutationState& before, const SMutationPlan&) override {
        for (const auto& workspaceState : before.workspaces) {
            auto& native = nativeFor(workspaceState.workspaceID);
            if (workspaceState.kind == EMutationWorkspaceKind::Mixed)
                continue;
            auto data = native.data;
            if (!data && workspaceState.columns.empty())
                continue;
            if (!data || !data->controller)
                throw std::runtime_error("rollback model ownership expired");

            const auto currentColumns = data->columns;
            for (const auto& column : currentColumns) {
                if (!column)
                    throw std::runtime_error("rollback encountered an expired column");
                std::vector<SP<Layout::ITarget>> targets;
                targets.reserve(column->targetDatas.size());
                for (const auto& targetData : column->targetDatas) {
                    const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
                    if (!target)
                        throw std::runtime_error("rollback encountered an expired target");
                    targets.push_back(target);
                }
                for (const auto& target : targets)
                    column->remove(target);
            }

            for (size_t columnIndex = 0; columnIndex < workspaceState.columns.size(); ++columnIndex) {
                const auto& expectedColumn = workspaceState.columns[columnIndex];
                auto column = data->add(static_cast<int>(columnIndex) - 1, static_cast<float>(expectedColumn.width));
                m_logicalColumnIdentities[column.get()] = expectedColumn.identity;
                for (const auto& expectedTarget : expectedColumn.targets)
                    column->add(targetFor(expectedTarget.identity));
            }
        }
    }

    void restoreWidths(const SMutationState& before, const SMutationPlan& plan, bool) override {
        for (const auto& workspaceState : before.workspaces) {
            auto& native = nativeFor(workspaceState.workspaceID);
            if (workspaceState.kind == EMutationWorkspaceKind::Mixed || !native.data)
                continue;
            const auto direction = parseDirection(workspaceState.direction);
            if (!direction || !native.data->controller)
                throw std::runtime_error("native controller pre-state is incomplete");
            native.data->controller->setDirection(*direction);
            native.data->controller->setOffset(workspaceState.offset);
            for (const auto& expectedColumn : workspaceState.columns) {
                const auto column = columnForIdentity(native, expectedColumn.identity);
                if (column)
                    column->setColumnWidth(static_cast<float>(expectedColumn.width));
            }
        }
        auto& destination = nativeFor(plan.request.destinationWorkspaceID);
        if (destination.kind == EMutationWorkspaceKind::Mixed || !destination.data)
            return;
        for (const auto& column : destination.data->columns) {
            if (column && std::ranges::any_of(column->targetDatas, [&](const auto& targetData) {
                    const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
                    return target && mutationIdentity(target) == plan.request.targetIdentity;
                }) && logicalColumnIdentity(column) == 0)
                column->setColumnWidth(static_cast<float>(plan.sourceColumnWidth));
        }
    }

    void restoreSizes(const SMutationState& before, const SMutationPlan& plan, bool rollback) override {
        std::unordered_map<uint64_t, double> originalSizes;
        for (const auto& workspace : before.workspaces)
            for (const auto& column : workspace.columns)
                for (const auto& target : column.targets)
                    originalSizes[target.identity] = target.size;
        for (auto& [workspaceID, native] : m_native) {
            if (!native.data)
                continue;
            for (const auto& column : native.data->columns) {
                std::vector<uint64_t> identities;
                identities.reserve(column->targetDatas.size());
                for (const auto& targetData : column->targetDatas) {
                    const auto target = targetData ? targetData->target.lock() : SP<Layout::ITarget>{};
                    if (!target)
                        throw std::runtime_error("target expired while planning row proportions");
                    identities.push_back(mutationIdentity(target));
                }
                std::vector<double> sizes;
                if (rollback) {
                    sizes.reserve(identities.size());
                    for (const auto identity : identities) {
                        const auto original = originalSizes.find(identity);
                        if (original == originalSizes.end())
                            throw std::runtime_error("rollback row proportion has no pre-state identity");
                        sizes.push_back(original->second);
                    }
                } else {
                    sizes = expectedCommittedTargetSizes(before, plan, workspaceID, logicalColumnIdentity(column), identities);
                }
                if (sizes.size() != column->targetDatas.size())
                    throw std::runtime_error("native normalized row-size plan is incomplete");
                for (size_t rowIndex = 0; rowIndex < column->targetDatas.size(); ++rowIndex) {
                    const auto target = column->targetDatas[rowIndex]->target.lock();
                    if (!target)
                        throw std::runtime_error("target expired while restoring row proportions");
                    column->setTargetSize(rowIndex, static_cast<float>(sizes[rowIndex]));
                }
            }
        }
    }

    void recalculate(const SMutationPlan&, bool) override {
        for (auto& [workspaceID, native] : m_native) {
            (void)workspaceID;
            if (native.data)
                native.data->recalculate();
        }
        if (!m_preState)
            throw std::runtime_error("controller restoration has no pre-state");
        for (const auto& workspaceState : m_preState->workspaces) {
            auto& native = nativeFor(workspaceState.workspaceID);
            if (workspaceState.kind == EMutationWorkspaceKind::Mixed || !native.data)
                continue;
            const auto direction = parseDirection(workspaceState.direction);
            if (!direction || !native.data->controller)
                throw std::runtime_error("native controller pre-state is incomplete after recalculation");
            native.data->controller->setDirection(*direction);
            native.data->controller->setOffset(workspaceState.offset);
        }
    }

    SMutationState snapshotPostState(const SMutationPlan& plan, bool rollback) override {
        SMutationState state;
        if (rollback && m_createdDestination)
            proveCreatedDestinationRollback(plan);
        for (auto& [workspaceID, native] : m_native) {
            if (native.kind == EMutationWorkspaceKind::Mixed) {
                snapshotMixedAndRetain(native.workspace, state, false);
                continue;
            }
            const auto snapshot = snapshotWorkspace(native.workspace);
            SMutationWorkspace converted;
            if (snapshot.success())
                converted = mutationWorkspace(*snapshot.snapshot);
            else if (native.data && native.data->controller && native.data->columns.empty())
                converted = emptyNativeState(native, rollback);
            else
                throw std::runtime_error("native readback failed: " + snapshot.error);
            for (size_t index = 0; index < native.data->columns.size() && index < converted.columns.size(); ++index) {
                if (const auto logical = m_logicalColumnIdentities.find(native.data->columns[index].get()); logical != m_logicalColumnIdentities.end())
                    converted.columns[index].identity = logical->second;
            }
            state.workspaces.push_back(std::move(converted));
        }
        std::ranges::sort(state.workspaces, {}, &SMutationWorkspace::workspaceID);
        return state;
    }

    void primePreState(SMutationState state) {
        m_preState = std::move(state);
    }

  private:
    struct SNativeWorkspace {
        PHLWORKSPACE                      workspace;
        EMutationWorkspaceKind            kind = EMutationWorkspaceKind::Scrolling;
        SP<Layout::Tiled::SScrollingData> data;
    };

    uint64_t mutationIdentity(const SP<Layout::ITarget>& target) const {
        for (const auto& [identity, retained] : m_targets)
            if (retained == target)
                return identity;
        return 0;
    }

    SP<Layout::ITarget> targetFor(uint64_t identity) const {
        const auto target = m_targets.find(identity);
        if (target == m_targets.end() || !target->second)
            throw std::runtime_error("retained mutation target is unavailable");
        return target->second;
    }

    SNativeWorkspace& nativeFor(int64_t workspaceID) {
        const auto native = m_native.find(workspaceID);
        if (native == m_native.end())
            throw std::runtime_error("native workspace was not retained");
        return native->second;
    }

    SP<Layout::Tiled::SColumnData> columnForIdentity(const SNativeWorkspace& native, uint64_t identity) const {
        if (!native.data)
            return {};
        for (const auto& column : native.data->columns) {
            if (const auto logical = m_logicalColumnIdentities.find(column.get()); logical != m_logicalColumnIdentities.end() && logical->second == identity)
                return column;
        }
        return {};
    }

    uint64_t logicalColumnIdentity(const SP<Layout::Tiled::SColumnData>& column) const {
        if (!column)
            return 0;
        const auto logical = m_logicalColumnIdentities.find(column.get());
        return logical == m_logicalColumnIdentities.end() ? 0 : logical->second;
    }

    void proveCreatedDestinationRollback(const SMutationPlan& plan) {
        if (!m_createdDestination || m_createdDestination->m_id != plan.request.destinationWorkspaceID || !m_createdDestination->m_wasCreatedEmpty ||
            m_createdDestination->getWindowCount() != 0 || !m_createdDestination->m_space)
            throw std::runtime_error("terminal rollback did not restore an empty created workspace");
        for (const auto& weak : m_createdDestination->m_space->targets()) {
            const auto target = weak.lock();
            if (target && mutationIdentity(target) == plan.request.targetIdentity)
                throw std::runtime_error("terminal rollback retained the moved target identity");
            if (target)
                throw std::runtime_error("terminal rollback created workspace retained foreign membership");
        }
        const auto native = m_native.find(plan.request.destinationWorkspaceID);
        if (native == m_native.end() || (native->second.data && (!native->second.data->columns.empty() || !native->second.data->controller ||
                                                                 native->second.data->controller->stripCount() != 0)))
            throw std::runtime_error("terminal rollback retained native columns or controller strips");

        const auto createdID = m_createdDestination->m_id;
        m_native.erase(plan.request.destinationWorkspaceID);
        m_destinationWorkspace.reset();
        m_createdDestination.reset();
        for (const auto& workspace : State::workspaceState()->workspacesCopy())
            if (workspace && workspace->m_id == createdID)
                throw std::runtime_error("terminal rollback created workspace was not released");
    }

    void snapshotAndRetain(const PHLWORKSPACE& workspace, SMutationState& state) {
        const auto snapshot = snapshotWorkspace(workspace);
        if (!snapshot.success())
            throw std::runtime_error("pre-state snapshot failed: " + snapshot.error);
        auto resolved = resolveNativeWorkspace(workspace);
        for (size_t columnIndex = 0; columnIndex < snapshot.snapshot->columns.size(); ++columnIndex) {
            const auto& snapshotColumn = snapshot.snapshot->columns[columnIndex];
            if (columnIndex >= resolved.data->columns.size())
                throw std::runtime_error("pre-state column cardinality changed");
            m_logicalColumnIdentities[resolved.data->columns[columnIndex].get()] = snapshotColumn.fingerprint;
            for (const auto& target : snapshotColumn.targets) {
                const auto strong = target.targetRef.lock();
                if (!strong)
                    throw std::runtime_error("pre-state target expired while retaining transaction handles");
                m_targets[mutationTargetIdentity(target)] = strong;
            }
        }
        m_native[workspace->m_id] = {.workspace = workspace, .kind = EMutationWorkspaceKind::Scrolling, .data = resolved.data};
        state.workspaces.push_back(mutationWorkspace(*snapshot.snapshot));
        std::ranges::sort(state.workspaces, {}, &SMutationWorkspace::workspaceID);
    }

    void snapshotScrollingOrEmptyAndRetain(const PHLWORKSPACE& workspace, SMutationState& state) {
        const auto snapshot = snapshotWorkspace(workspace);
        if (snapshot.success()) {
            snapshotAndRetain(workspace, state);
            return;
        }
        if (workspace->getWindowCount(true) != 0)
            throw std::runtime_error("non-empty scrolling destination has no complete pre-state");
        m_native[workspace->m_id] = {.workspace = workspace, .kind = EMutationWorkspaceKind::Scrolling, .data = {}};
        state.workspaces.push_back({.workspaceID = workspace->m_id, .modelIdentity = 0, .kind = EMutationWorkspaceKind::Scrolling, .direction = {}, .offset = 0.0,
                                    .focusedTargetIdentity = 0, .focusedWindowIdentity = 0, .columns = {}, .members = {}});
        std::ranges::sort(state.workspaces, {}, &SMutationWorkspace::workspaceID);
    }

    void snapshotMixedAndRetain(const PHLWORKSPACE& workspace, SMutationState& state, bool retain) {
        SMutationWorkspace mixed{.workspaceID = workspace ? workspace->m_id : 0, .modelIdentity = 0, .kind = EMutationWorkspaceKind::Mixed,
                                 .direction = {}, .offset = 0.0, .focusedTargetIdentity = 0, .focusedWindowIdentity = 0, .columns = {}, .members = {}};
        if (!workspace || workspace->inert() || !workspace->m_space)
            throw std::runtime_error("mixed workspace expired during transaction readback");
        for (const auto& weak : workspace->m_space->targets()) {
            const auto target = weak.lock();
            if (!target)
                throw std::runtime_error("mixed workspace target expired during transaction readback");
            const auto identity = static_cast<uint64_t>(fingerprint(target));
            mixed.members.push_back(identity);
            if (retain)
                m_targets[identity] = target;
        }
        if (retain)
            m_native[workspace->m_id] = {.workspace = workspace, .kind = EMutationWorkspaceKind::Mixed, .data = {}};
        state.workspaces.push_back(std::move(mixed));
    }

    SMutationWorkspace emptyNativeState(const SNativeWorkspace& native, bool rollback) const {
        const SMutationWorkspace* expected = nullptr;
        if (m_preState) {
            const auto found = std::ranges::find(m_preState->workspaces, native.workspace->m_id, &SMutationWorkspace::workspaceID);
            if (found != m_preState->workspaces.end())
                expected = &*found;
        }
        SMutationWorkspace result{.workspaceID = native.workspace->m_id,
                                  .modelIdentity = rollback && expected ? expected->modelIdentity : fingerprint(native.data),
                                  .kind = EMutationWorkspaceKind::Scrolling,
                                  .direction = rollback && expected ? expected->direction : directionName(native.data->controller->getDirection()),
                                  .offset = rollback && expected ? expected->offset : native.data->controller->getOffset(),
                                  .focusedTargetIdentity = rollback && expected ? expected->focusedTargetIdentity : 0,
                                  .focusedWindowIdentity = rollback && expected ? expected->focusedWindowIdentity : 0,
                                  .columns = {}, .members = {}};
        for (const auto& weak : native.workspace->m_space->targets()) {
            const auto target = weak.lock();
            if (!target)
                throw std::runtime_error("empty native workspace retained an expired target");
            result.members.push_back(mutationIdentity(target));
        }
        return result;
    }

    PHLWORKSPACE m_sourceWorkspace;
    PHLWORKSPACE m_destinationWorkspace;
    PHLWORKSPACE m_createdDestination;
    PHLMONITOR   m_monitor;
    std::optional<SMutationState> m_preState;
    std::unordered_map<int64_t, SNativeWorkspace> m_native;
    std::unordered_map<uint64_t, SP<Layout::ITarget>> m_targets;
    std::unordered_map<const Layout::Tiled::SColumnData*, uint64_t> m_logicalColumnIdentities;
    std::optional<SFaultInjection> m_fault;
};

} // namespace

std::string snapshotFailureName(ESnapshotFailure failureCode) {
    switch (failureCode) {
        case ESnapshotFailure::None: return "none";
        case ESnapshotFailure::NullWorkspace: return "null-workspace";
        case ESnapshotFailure::InertWorkspace: return "inert-workspace";
        case ESnapshotFailure::MissingMonitor: return "missing-monitor";
        case ESnapshotFailure::MissingSpace: return "missing-space";
        case ESnapshotFailure::MissingAlgorithm: return "missing-algorithm";
        case ESnapshotFailure::MissingTiledAlgorithm: return "missing-tiled-algorithm";
        case ESnapshotFailure::WrongAlgorithmName: return "wrong-algorithm-name";
        case ESnapshotFailure::CastFailure: return "cast-failure";
        case ESnapshotFailure::ExpiredTarget: return "expired-target";
        case ESnapshotFailure::ExpiredColumn: return "expired-column";
        case ESnapshotFailure::ExpiredData: return "expired-data";
        case ESnapshotFailure::MissingScrollingData: return "missing-scrolling-data";
        case ESnapshotFailure::ColumnCardinalityMismatch: return "column-cardinality-mismatch";
        case ESnapshotFailure::TargetCardinalityMismatch: return "target-cardinality-mismatch";
        case ESnapshotFailure::InvalidGeometry: return "invalid-geometry";
        case ESnapshotFailure::HostException: return "host-exception";
    }
    return "unknown";
}

SSnapshotResult snapshotWorkspace(const PHLWORKSPACE& workspace) {
    try {
        return snapshotWorkspaceImpl(workspace);
    } catch (const std::exception& error) {
        return failure(ESnapshotFailure::HostException, error.what());
    } catch (...) {
        return failure(ESnapshotFailure::HostException, "unknown host exception");
    }
}

bool workspaceUsesScrollingLayout(const PHLWORKSPACE& workspace) {
    try {
        if (!workspace || workspace->inert() || !workspace->m_space)
            return false;
        const auto algorithmOwner = workspace->m_space->algorithm();
        if (!algorithmOwner)
            return false;
        const auto& tiledOwner = algorithmOwner->tiledAlgo();
        if (!tiledOwner)
            return false;
        auto* const tiled = tiledOwner.get();
        if (Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(tiled) != "scrolling")
            return false;
        return dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(tiled) != nullptr;
    } catch (...) {
        return false;
    }
}

SMutationResult moveScrollingTarget(const PHLWORKSPACE& sourceWorkspace, const PHLWORKSPACE& destinationWorkspace, const PHLMONITOR& initiatingMonitor,
                                    const SMutationRequest& originalRequest, std::optional<SFaultInjection> fault) {
    auto request = originalRequest;
    const auto failureResult = [&request](EMutationOutcome outcome, std::string error, std::vector<std::string> violations = {}) {
        SMutationResult result;
        result.outcome = outcome;
        result.plan.request = request;
        result.violatedInvariantIDs = std::move(violations);
        result.error = std::move(error);
        return result;
    };
    try {
        if (!sourceWorkspace || !initiatingMonitor || sourceWorkspace->m_monitor.lock() != initiatingMonitor)
            return failureResult(EMutationOutcome::Rejected, "source workspace or initiating monitor expired");
        PHLWORKSPACE resolvedDestination = destinationWorkspace;
        if (request.kind == EDropKind::TerminalWorkspace) {
            std::vector<int64_t> workspaceIDs;
            for (const auto& workspace : State::workspaceState()->workspacesCopy())
                if (workspace && !workspace->m_isSpecialWorkspace)
                    workspaceIDs.push_back(workspace->m_id);
            request.destinationWorkspaceID = nextUnusedOrdinaryWorkspaceID(workspaceIDs);
            request.createDestination = true;
            request.placement = EColumnPlacement::None;
            resolvedDestination.reset();
            if (request.destinationWorkspaceID <= 0)
                return failureResult(EMutationOutcome::Rejected, "no unused positive workspace ID is available");
        } else if (!resolvedDestination || request.destinationWorkspaceID != resolvedDestination->m_id) {
            return failureResult(EMutationOutcome::Rejected, "destination workspace changed before mutation");
        }

        CNativeMutationOperations operations{sourceWorkspace, resolvedDestination, initiatingMonitor, fault};
        SMutationState snapshotPreState;
        try {
            snapshotPreState = operations.snapshotPreState(request);
        } catch (const std::exception& error) {
            return failureResult(EMutationOutcome::Rejected, error.what());
        } catch (...) {
            return failureResult(EMutationOutcome::Rejected, "unknown pre-state snapshot exception");
        }
        operations.primePreState(snapshotPreState);
        auto result = executeMutation(request, operations);
        if (result.outcome == EMutationOutcome::Committed || result.outcome == EMutationOutcome::RolledBack) {
            const auto verified = verifyPostconditions(result.before, result.after, result.plan, result.outcome == EMutationOutcome::RolledBack);
            if (result.violatedInvariantIDs.empty() && !verified.empty())
                result.violatedInvariantIDs = verified;
        }
        if (const auto monitor = sourceWorkspace->m_monitor.lock())
            g_pHyprRenderer->damageMonitor(monitor);
        return result;
    } catch (const std::exception& error) {
        return failureResult(EMutationOutcome::RollbackFailed, error.what(), {"native.exception-boundary"});
    } catch (...) {
        return failureResult(EMutationOutcome::RollbackFailed, "unknown native mutation exception", {"native.exception-boundary"});
    }
}

std::expected<SMutationResult, std::string> runNativeMutationTest(const std::string& argument, uint64_t sessionGeneration) {
    const auto parsed = parseMutationDebugRequest(argument);
    if (!parsed.valid)
        return std::unexpected(parsed.error);

    PHLWORKSPACE sourceWorkspace;
    PHLWORKSPACE destinationWorkspace;
    for (const auto& workspace : State::workspaceState()->workspacesCopy()) {
        if (!workspace || workspace->m_isSpecialWorkspace)
            continue;
        if (workspace->m_id == parsed.sourceWorkspaceID)
            sourceWorkspace = workspace;
        if (workspace->m_id == parsed.destinationWorkspaceID)
            destinationWorkspace = workspace;
    }
    if (!sourceWorkspace)
        return std::unexpected("source workspace is unavailable");
    if (parsed.kind != EDropKind::TerminalWorkspace && !destinationWorkspace)
        return std::unexpected("destination workspace is unavailable");

    const auto sourceSnapshot = snapshotWorkspace(sourceWorkspace);
    if (!sourceSnapshot.success())
        return std::unexpected("source snapshot failed: " + sourceSnapshot.error);
    uint64_t targetIdentity = 0;
    size_t stableMatches = 0;
    for (const auto& column : sourceSnapshot.snapshot->columns) {
        for (const auto& target : column.targets) {
            if (target.windowStableID != parsed.targetStableID)
                continue;
            ++stableMatches;
            targetIdentity = target.targetFingerprint;
        }
    }
    if (stableMatches != 1 || targetIdentity == 0)
        return std::unexpected("target stable ID does not resolve exactly once in native scrolling columns");

    const SMutationRequest request{
        .requestID = parsed.requestID,
        .sessionGeneration = sessionGeneration,
        .targetIdentity = targetIdentity,
        .sourceWorkspaceID = parsed.sourceWorkspaceID,
        .destinationWorkspaceID = parsed.destinationWorkspaceID,
        .kind = parsed.kind,
        .placement = parsed.placement,
        .destinationColumnIndex = parsed.destinationColumnIndex,
        .destinationRowIndex = parsed.destinationRowIndex,
        .createDestination = parsed.createDestination,
    };
    const auto monitor = sourceWorkspace->m_monitor.lock();
    if (!monitor)
        return std::unexpected("source monitor expired");
    return moveScrollingTarget(sourceWorkspace, destinationWorkspace, monitor, request, parsed.fault);
}

}
