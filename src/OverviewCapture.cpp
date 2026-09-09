#include "OverviewCapture.hpp"

#include "OverviewInternal.hpp"
#include "HyprlandConfigCompat.hpp"
#include "HyprexpoConfig.hpp"

#define private   public
#define protected public
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#undef private
#undef protected

#include <algorithm>
#include <cmath>
#include <exception>
#include <atomic>
#include <mutex>
#include <optional>

namespace Hyprexpo::Capture {

namespace {

std::atomic<uint64_t> g_budgetGeneration = 1;
std::mutex            g_budgetLogMutex;
uint64_t              g_lastLoggedBudgetGeneration = 0;

class CRendererStateGuard {
  public:
    explicit CRendererStateGuard(Render::IHyprRenderer* renderer) : m_renderer(renderer), m_blockSurfaceFeedback(renderer->m_bBlockSurfaceFeedback),
                                                                    m_renderingSnapshot(renderer->m_bRenderingSnapshot),
                                                                    m_blockScreenShader(renderer->m_renderData.blockScreenShader) {}

    ~CRendererStateGuard() {
        if (m_begun) {
            try {
                m_renderer->endRender();
            } catch (...) {}
        }
        restore();
    }

    void begun() {
        m_begun = true;
    }

    void finish() {
        if (!m_begun)
            return;
        m_renderer->endRender();
        m_begun = false;
    }

    void restore() {
        m_renderer->m_bBlockSurfaceFeedback        = m_blockSurfaceFeedback;
        m_renderer->m_bRenderingSnapshot           = m_renderingSnapshot;
        m_renderer->m_renderData.blockScreenShader = m_blockScreenShader;
    }

    bool restored() const {
        return m_renderer->m_bBlockSurfaceFeedback == m_blockSurfaceFeedback && m_renderer->m_bRenderingSnapshot == m_renderingSnapshot &&
            m_renderer->m_renderData.blockScreenShader == m_blockScreenShader;
    }

  private:
    Render::IHyprRenderer* m_renderer;
    bool                   m_blockSurfaceFeedback;
    bool                   m_renderingSnapshot;
    bool                   m_blockScreenShader;
    bool                   m_begun = false;
};

class CMonitorStateGuard {
  public:
    CMonitorStateGuard(const PHLMONITOR& monitor, const PHLWORKSPACE& startedOn) : m_monitor(monitor), m_startedOn(startedOn), m_transform(monitor->m_transform),
                                                                                  m_transformedSize(monitor->m_transformedSize), m_pixelSize(monitor->m_pixelSize),
                                                                                  m_activeWorkspace(monitor->m_activeWorkspace),
                                                                                  m_activeSpecialWorkspace(monitor->m_activeSpecialWorkspace),
                                                                                  m_startedOnVisible(startedOn ? startedOn->m_visible : false) {}

    ~CMonitorStateGuard() {
        restore();
    }

    void restore() {
        if (m_restored || !m_monitor)
            return;
        m_monitor->m_transform              = m_transform;
        m_monitor->m_transformedSize        = m_transformedSize;
        m_monitor->m_pixelSize              = m_pixelSize;
        m_monitor->m_activeWorkspace        = m_activeWorkspace ? m_activeWorkspace : m_startedOn;
        m_monitor->m_activeSpecialWorkspace = m_activeSpecialWorkspace;
        if (m_startedOn)
            m_startedOn->m_visible = m_startedOnVisible;
        m_restored = true;
    }

    wl_output_transform transform() const {
        return m_transform;
    }

    PHLWORKSPACE activeWorkspace() const {
        return m_activeWorkspace;
    }

    PHLWORKSPACE activeSpecialWorkspace() const {
        return m_activeSpecialWorkspace;
    }

  private:
    PHLMONITOR          m_monitor;
    PHLWORKSPACE        m_startedOn;
    wl_output_transform m_transform;
    Vector2D            m_transformedSize;
    Vector2D            m_pixelSize;
    PHLWORKSPACE        m_activeWorkspace;
    PHLWORKSPACE        m_activeSpecialWorkspace;
    bool                m_startedOnVisible = false;
    bool                m_restored         = false;
};

bool ensureFramebuffer(SP<Render::IFramebuffer>& framebuffer, const CBox& box, uint32_t drmFormat) {
    if (!framebuffer)
        framebuffer = g_pHyprRenderer->createFB("hyprexpo workspace preview");
    if (!framebuffer)
        return false;
    if (framebuffer->m_size == box.size())
        return true;
    framebuffer->release();
    return framebuffer->alloc(box.w, box.h, drmFormat);
}

}

int scrollingThumbnailBudgetMultiplier() {
    const int raw = static_cast<int>(CompatHyprlandAPI::intValue("plugin:hyprexpo:scrolling_thumbnail_budget"));
    const int clamped = std::clamp(raw, HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_MIN, HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_MAX);
    if (raw != clamped) {
        std::scoped_lock lock{g_budgetLogMutex};
        const auto generation = g_budgetGeneration.load(std::memory_order_relaxed);
        if (g_lastLoggedBudgetGeneration != generation) {
            Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] scrolling_thumbnail_budget {} is outside {}..{}; clamping to {}", raw, HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_MIN,
                             HyprexpoConfig::SCROLLING_THUMBNAIL_BUDGET_MAX, clamped);
            g_lastLoggedBudgetGeneration = generation;
        }
    }
    return clamped;
}

uint64_t scrollingThumbnailBudgetGeneration() {
    return g_budgetGeneration.load(std::memory_order_relaxed);
}

void notifyOverviewCaptureConfigReload() {
    g_budgetGeneration.fetch_add(1, std::memory_order_relaxed);
}

bool captureWorkspacePreview(const SWorkspaceCaptureRequest& request, SP<Render::IFramebuffer>& framebuffer) {
    if (!request.monitor || !request.monitor->m_output || !request.startedOn || request.box.w <= 0 || request.box.h <= 0)
        return false;

    try {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        CMonitorStateGuard monitorState{request.monitor, request.startedOn};
        CRendererStateGuard rendererState{g_pHyprRenderer.get()};
        CBox captureBox = request.box;

        if (isTransformRotated(monitorState.transform())) {
            captureBox = {{0, 0}, {captureBox.h, captureBox.w}};
            request.monitor->m_transform       = WL_OUTPUT_TRANSFORM_NORMAL;
            request.monitor->m_pixelSize       = captureBox.size();
            request.monitor->m_transformedSize = captureBox.size();
        }

        if (!ensureFramebuffer(framebuffer, captureBox, framebufferFormatWithAlpha(request.monitor->m_output->state->state().drmFormat)))
            return false;

        if (monitorState.activeSpecialWorkspace())
            request.monitor->m_activeSpecialWorkspace.reset();
        request.startedOn->m_visible = false;

        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        if (!g_pHyprRenderer->beginRender(request.monitor, fakeDamage, Render::RENDER_MODE_FULL_FAKE, nullptr, framebuffer))
            return false;
        rendererState.begun();
        g_pHyprRenderer->m_bBlockSurfaceFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback || request.blockSurfaceFeedback;
        clearWithColor(CHyprColor{0, 0, 0, 1.0});

        if (request.workspace) {
            PHLWORKSPACE previousWS;
            std::vector<std::pair<PHLWORKSPACE, SWorkspacePreviewState>> previewStates;
            std::vector<SWindowPreviewState> windowState;
            bool activeChanged   = false;
            bool previewApplied  = false;
            bool windowApplied   = false;
            bool temporaryRestored = false;
            Hyprutils::Utils::CScopeGuard temporaryStateGuard{[&]() {
                if (temporaryRestored)
                    return;
                try {
                    if (windowApplied)
                        restoreWorkspaceWindowGoalState(windowState);
                    if (previewApplied)
                        restoreWorkspacePreviewStates(previewStates);
                    if (activeChanged)
                        restoreActiveWorkspaceAfterPreview(request.monitor, previousWS);
                } catch (...) {}
            }};

            previousWS   = activateWorkspaceForPreview(request.monitor, request.workspace);
            activeChanged = true;
            previewStates = applyExclusiveWorkspacePreviewState(request.workspace);
            previewApplied = true;
            if (request.workspace != request.startedOn) {
                windowState   = applyWorkspaceWindowGoalState(request.workspace);
                windowApplied = true;
            }

            if (request.workspace == request.startedOn)
                request.monitor->m_activeSpecialWorkspace = monitorState.activeSpecialWorkspace();

            {
                CPinnedWindowPreviewGuard pinnedWindowPreviewGuard{request.showPinnedWindows};
                g_pHyprRenderer->renderWorkspace(request.monitor, request.workspace, Time::steadyNow(), captureBox);
            }

            restoreWorkspaceWindowGoalState(windowState);
            restoreWorkspacePreviewStates(previewStates);
            restoreActiveWorkspaceAfterPreview(request.monitor, previousWS);
            temporaryRestored = true;

            if (request.workspace == request.startedOn)
                request.monitor->m_activeSpecialWorkspace.reset();
        } else {
            CPinnedWindowPreviewGuard pinnedWindowPreviewGuard{request.showPinnedWindows};
            g_pHyprRenderer->renderWorkspace(request.monitor, request.workspace, Time::steadyNow(), captureBox);
        }

        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        rendererState.finish();
        rendererState.restore();
        if (!rendererState.restored())
            return false;

        if (const auto texture = framebuffer->getTexture(); texture)
            texture->m_transform = isTransformRotated(monitorState.transform()) ? HYPRUTILS_TRANSFORM_180 : HYPRUTILS_TRANSFORM_NORMAL;
        else
            return false;

        monitorState.restore();
        if (request.animateStartedOnRestore) {
            request.startedOn->m_visible = false;
            const auto activeWorkspace = monitorState.activeWorkspace() ? monitorState.activeWorkspace() : request.startedOn;
            activeWorkspace->m_visible = true;
            if (activeWorkspace == request.startedOn)
                Animation::Workspace::startAnimation(activeWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
        }
        return true;
    } catch (...) {
        return false;
    }
}

SWindowCaptureResult captureWindowPreview(const WP<Layout::ITarget>& targetRef, const PHLWINDOWREF& windowRef, const PHLMONITORREF& monitorRef, const Vector2D& pixelSize) {
    SWindowCaptureResult result;
    try {
        auto target  = targetRef.lock();
        auto window  = windowRef.lock();
        auto monitor = monitorRef.lock();
        if (!target || !window || !monitor || !monitor->m_output) {
            result.error = "target, window, or monitor expired before capture";
            return result;
        }

        const int width  = static_cast<int>(std::floor(pixelSize.x));
        const int height = static_cast<int>(std::floor(pixelSize.y));
        if (width < 16 || height < 16 || width > static_cast<int>(monitor->m_pixelSize.x) || height > static_cast<int>(monitor->m_pixelSize.y)) {
            result.error = "target capture dimensions are outside the bounded monitor range";
            return result;
        }

        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        const auto framebuffer = g_pHyprRenderer->createFB("hyprexpo scrolling target preview");
        if (!framebuffer || !framebuffer->alloc(width, height, DRM_FORMAT_ABGR8888)) {
            result.error = "target framebuffer allocation failed";
            return result;
        }
        framebuffer->setImageDescription(monitor->workBufferImageDescription());

        CRegion fakeDamage{0, 0, width, height};
        CRendererStateGuard rendererState{g_pHyprRenderer.get()};
        if (!g_pHyprRenderer->beginFullFakeRender(monitor, fakeDamage, framebuffer)) {
            result.error = "beginFullFakeRender failed";
            return result;
        }
        rendererState.begun();
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
        g_pHyprRenderer->m_bRenderingSnapshot = true;
        glClearColor(0.F, 0.F, 0.F, 0.F);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprRenderer->startRenderPass();

        target = targetRef.lock();
        window = windowRef.lock();
        if (!target || !window || target->window() != window) {
            result.error = "target or window expired during capture";
            return result;
        }

        g_pHyprRenderer->renderWindow(window, monitor, Time::steadyNow(), false, Render::RENDER_PASS_ALL, true, true);
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        rendererState.finish();
        rendererState.restore();
        if (!rendererState.restored()) {
            result.error = "renderer state was not restored";
            return result;
        }

        const auto texture = framebuffer->getTexture();
        if (!texture) {
            result.error = "completed framebuffer has no texture";
            return result;
        }

        result.framebuffer = framebuffer;
        result.texture     = texture;
        result.completed   = true;
        return result;
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    } catch (...) {
        result.error = "unknown target capture failure";
        return result;
    }
}

}
