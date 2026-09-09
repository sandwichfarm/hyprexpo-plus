#include "IOverviewSession.hpp"

#include "Overview.hpp"
#include "ScrollingLayoutAdapter.hpp"
#include "ScrollingOverview.hpp"
#include "globals.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <atomic>
#include <exception>
#include <format>

namespace {

void notifyScrollingFailure(const std::string& message) {
    Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] {}", message);
    HyprlandAPI::addNotification(PHANDLE, "[hyprexpo] " + message, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

}

std::unique_ptr<IOverviewSession> createOverviewSession(const PHLWORKSPACE& startedOn, const PHLMONITOR& monitor, bool swipe) {
    if (!startedOn || !monitor || startedOn->m_monitor != monitor)
        return nullptr;
    static std::atomic<uint64_t> nextGeneration = 1;
    const uint64_t generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);

    const bool detectedScrolling = Hyprexpo::Scrolling::workspaceUsesScrollingLayout(startedOn);
    if (detectedScrolling) {
        const auto snapshot = Hyprexpo::Scrolling::snapshotWorkspace(startedOn);
        const bool emptyScrolling = startedOn && startedOn->getWindowCount() <= 0 && snapshot.failure == Hyprexpo::Scrolling::ESnapshotFailure::MissingScrollingData;
        if (!snapshot.success() && !emptyScrolling) {
            notifyScrollingFailure(std::format("native scrolling snapshot failed ({}): {}", snapshotFailureName(snapshot.failure), snapshot.error));
            return nullptr;
        }
        try {
            auto scrolling = std::make_unique<CScrollingOverview>(startedOn, monitor, swipe, generation, snapshot.snapshot);
            if (scrolling->valid())
                return scrolling;
            notifyScrollingFailure("native scrolling session initialization failed");
        } catch (const std::exception& error) {
            notifyScrollingFailure(std::format("native scrolling session threw during initialization: {}", error.what()));
        } catch (...) {
            notifyScrollingFailure("native scrolling session threw an unknown exception");
        }
        return nullptr;
    }

    return std::make_unique<COverview>(startedOn, monitor, swipe, generation);
}
