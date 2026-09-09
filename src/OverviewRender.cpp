#include "HyprlandConfigCompat.hpp"
#define HyprlandAPI CompatHyprlandAPI
#include "OverviewInternal.hpp"
#include "OverviewCapture.hpp"
#include "HyprexpoLogic.hpp"
#include "OverviewPassElement.hpp"
#define private   public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#undef private
#undef protected
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

void COverview::redrawID(int id, bool forcelowres) {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    if (MON->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    blockOverviewRendering = true;

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    settleWorkspaceMoveAnimations();

    if (images.empty()) {
        blockOverviewRendering = false;
        return;
    }

    id = std::clamp(id, 0, (int)images.size() - 1);

    CBox monbox{0, 0, MON->m_pixelSize.x, MON->m_pixelSize.y};

    if (!forcelowres && (size->value() != MON->m_size || closing))
        monbox = {{0, 0}, MON->m_pixelSize};

    if (!ENABLE_LOWRES)
        monbox = {{0, 0}, MON->m_pixelSize};

    auto& image = images[id];

    PHLWORKSPACE PWORKSPACE;
    if (image.pWorkspace) {
        PWORKSPACE = image.pWorkspace;
    }
    else {
        for (const auto& w : State::workspaceState()->workspacesCopy()) {
            if (w->m_id == image.workspaceID) {
                PWORKSPACE = w;
                break;
            }
        }
    }
    image.pWorkspace = PWORKSPACE;
    Hyprexpo::Capture::captureWorkspacePreview({
        .monitor                   = MON,
        .workspace                 = PWORKSPACE,
        .startedOn                 = startedOn,
        .box                       = monbox,
        .showPinnedWindows         = showPinnedWindowsInPreview(),
        .animateStartedOnRestore   = true,
    }, image.fb);

    blockOverviewRendering = false;
}

void COverview::redrawAll(bool forcelowres) {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    for (size_t i = 0; i < images.size(); ++i) {
        redrawID(i, forcelowres);
    }
}

void COverview::damage() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(MON);
    blockDamageReporting = false;
}

void COverview::onDamageReported() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    damageDirty = true;

    Vector2D SIZE = size->value();

    const auto GAPSIZE = (closing ? (1.0 - size->getPercent()) : size->getPercent()) * GAP_WIDTH;
    const auto OUTER = currentOuterInset();
    CBox texbox = tileBoxForIndex(openedID, SIZE, GAPSIZE, OUTER, true).translate(MON->m_position);

    damage();

    blockDamageReporting = true;
    g_pHyprRenderer->damageBox(texbox);
    blockDamageReporting = false;
    MON->scheduleFrame();
}

void COverview::close(bool switchToSelection) {
    if (closing)
        return;

    // The teardown animation is now committed; lock out further swipe input so a
    // re-grabbed gesture can't rewind it (issue #57 follow-up: close replay).
    m_closeCommitted = true;

    const auto MON = pMonitor.lock();
    if (!MON) {
        closing = true;
        destroyOverview(this);
        return;
    }

    resetSubmapIfNeeded();

    if (images.empty()) {
        destroyOverview(this);
        return;
    }

    const int   ID = closeOnID == -1 ? openedID : closeOnID;

    const int   SAFEID = std::clamp(ID, 0, (int)images.size() - 1);
    const auto& TILE   = images[SAFEID];

    const auto targetSize = zoomSizeForCurrentGrid(MON->m_size);
    *size = targetSize;
    *pos  = -(tilePosForID(SAFEID, targetSize, 0.0) * MON->m_scale);

    closing = true;

    redrawAll();

    if (switchToSelection && (TILE.workspaceID != WORKSPACE_INVALID || emptyTilesSelectable) &&
        (TILE.workspaceID != MON->activeWorkspaceID() || Desktop::focusState()->monitor() != MON)) {
        MON->setSpecialWorkspace(0);

        // If this tile's workspace was WORKSPACE_INVALID, move to the next
        // empty workspace. This should only happen if skip_empty is on, in
        // which case some tiles will be left with this ID intentionally.
        const WORKSPACEID NEWID = TILE.workspaceID == WORKSPACE_INVALID ? nextEmptyWorkspaceIDForMonitor(MON) : TILE.workspaceID;

        // A tile can name a workspace that does not exist yet -- an anchored
        // grid (workspace_method "<output> first N") lays out max_workspace
        // slots whether or not those workspaces have ever been opened. Reuse
        // the same helper the drag paths use: it returns the existing
        // workspace or creates it. Without this, selecting such a tile fell
        // through to changeWorkspace(id), which cannot create one, and the
        // selection silently did nothing.
        PHLWORKSPACE NEWIDWS;
        if (TILE.workspaceID != WORKSPACE_INVALID)
            NEWIDWS = ensureWorkspaceForTile(SAFEID);

        if (!NEWIDWS) {
            for (const auto& w : State::workspaceState()->workspacesCopy()) {
                if (w->m_id == NEWID) {
                    NEWIDWS = w;
                    break;
                }
            }
        }

        const auto OLDWS = MON->m_activeWorkspace;

        if (!NEWIDWS && NEWID != WORKSPACE_INVALID)
            NEWIDWS = State::workspaceState()->create(NEWID, MON->m_id, std::to_string(NEWID), false);

        const auto CHANGE = Config::Actions::changeWorkspace(NEWIDWS);
        if (!CHANGE)
            Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] failed to change workspace: {}", CHANGE.error().message);

        if (CHANGE && OLDWS != MON->m_activeWorkspace) {
            Animation::Workspace::startAnimation(MON->m_activeWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true, true);
            Animation::Workspace::startAnimation(OLDWS, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);
        }

        startedOn = MON->m_activeWorkspace;
    }

    size->setCallbackOnEnd(removeOverview);
}

void COverview::onPreRender() {
    if (damageDirty) {
        damageDirty = false;
        redrawID(closing ? (closeOnID == -1 ? openedID : closeOnID) : openedID);
    }
}

void COverview::onWorkspaceChange() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    if (valid(startedOn))
        Animation::Workspace::startAnimation(startedOn, Animation::Workspace::ANIMATION_TYPE_OUT, false, true);
    else
        startedOn = MON->m_activeWorkspace;

    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].workspaceID != MON->activeWorkspaceID())
            continue;

        openedID = i;
        break;
    }

    closeOnID = openedID;
    close();
}

void COverview::render() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>(pMonitor.lock(), m_sessionGeneration));
}

bool COverview::shouldRenderOverviewForMonitor(const PHLMONITOR& monitor) const {
    if (pMonitor != monitor)
        return false;

    const auto MON = pMonitor.lock();
    if (!MON)
        return false;

    if (closing && (externalWorkspaceMoveDuringClose || MON->m_activeWorkspace != startedOn))
        return false;

    return true;
}


void COverview::fullRender() {
    const auto MON = pMonitor.lock();
    if (!MON)
        return;

    if (MON->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    Vector2D SIZE = size->value();

    const auto GAPSIZE = (closing ? (1.0 - size->getPercent()) : size->getPercent()) * GAP_WIDTH;
    const auto OUTER   = currentOuterInset();
    const auto SHAPE   = currentGridShape();

    clearWithColor(BG_COLOR.stripA());
    if (wallpaperBg && MON->m_background) {
        CRegion backgroundDamage{0, 0, INT16_MAX, INT16_MAX};
        CBox    backgroundBox{{0, 0}, MON->m_transformedSize};
        Render::GL::g_pHyprOpenGL->renderTextureInternal(MON->m_background, backgroundBox, {.damage = &backgroundDamage, .a = 1.0f});
        Render::GL::g_pHyprOpenGL->renderRect(backgroundBox, CHyprColor{0x00000066}, {});
    }

    static auto* const* PTILEROUND  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding")->getDataStaticPtr();
    static auto* const* PTOUNDPWR   = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_power")->getDataStaticPtr();
    static auto* const* PTILEROUNDF = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_focus")->getDataStaticPtr();
    static auto* const* PTILEROUNDC = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_current")->getDataStaticPtr();
    static auto* const* PTILEROUNDH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:tile_rounding_hover")->getDataStaticPtr();

    const int   BASE_ROUND_SCALED    = std::max(0, (int)std::lround((double)**PTILEROUND * MON->m_scale));
    const int   FOCUS_ROUND_SCALED   = **PTILEROUNDF >= 0 ? std::max(0, (int)std::lround((double)**PTILEROUNDF * MON->m_scale)) : BASE_ROUND_SCALED;
    const int   CURRENT_ROUND_SCALED = **PTILEROUNDC >= 0 ? std::max(0, (int)std::lround((double)**PTILEROUNDC * MON->m_scale)) : BASE_ROUND_SCALED;
    const int   HOVER_ROUND_SCALED   = **PTILEROUNDH >= 0 ? std::max(0, (int)std::lround((double)**PTILEROUNDH * MON->m_scale)) : BASE_ROUND_SCALED;
    const float ROUND_PWR            = **PTOUNDPWR;

    std::vector<CBox> tileBoxes(images.size());
    const bool        entryAnimationActive = animateEntry && !closing;
    bool              entryAnimationPending = false;
    const double      entryElapsed = entryAnimationActive ? std::chrono::duration<double>(std::chrono::steady_clock::now() - createdAt).count() : 0.0;

    for (size_t y = 0; y < (size_t)SHAPE.rows; ++y) {
        for (size_t x = 0; x < (size_t)SHAPE.cols; ++x) {
            const int id = x + y * SHAPE.cols;
            if (id < 0 || id >= (int)images.size())
                continue;
            CBox texbox = tileBoxForIndex(id, SIZE, GAPSIZE, OUTER, true);
            texbox.scale(MON->m_scale).translate(pos->value());
            texbox.round();
            tileBoxes[id] = texbox;

            int tileRound = BASE_ROUND_SCALED;
            if ((int)id == kbFocusID)
                tileRound = FOCUS_ROUND_SCALED;
            else if ((int)id == openedID)
                tileRound = CURRENT_ROUND_SCALED;
            else if ((int)id == hoveredID)
                tileRound = HOVER_ROUND_SCALED;

            const int maxCornerPx = std::max(0, (int)std::floor(std::min(texbox.w, texbox.h) / 2.0));
            tileRound = std::min(tileRound, maxCornerPx);

            float alpha = 1.0f;
            if (entryAnimationActive) {
                const double delay = (double)id * 0.05;
                const double raw   = std::clamp((entryElapsed - delay) / 0.2, 0.0, 1.0);
                alpha              = (float)(raw * raw * (3.0 - 2.0 * raw));
                if (raw < 1.0)
                    entryAnimationPending = true;
            }

            CRegion damage{0, 0, INT16_MAX, INT16_MAX};
            Render::GL::g_pHyprOpenGL->renderTextureInternal(images[id].fb->getTexture(), texbox, {.damage = &damage, .a = alpha, .round = tileRound, .roundingPower = ROUND_PWR});
        }
    }

    // overlays: labels and borders
    static auto* const* PLABELEN    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_enable")->getDataStaticPtr();
    static auto* const* PLABELSIZE  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_font_size")->getDataStaticPtr();
    static auto const*  PLABELPOS   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_position")->getDataStaticPtr();
    static auto const*  PLABELPOSL  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_pos")->getDataStaticPtr();
    static auto* const* PLABELSIZEL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_size")->getDataStaticPtr();
    static auto* const* PACTCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:active_highlight_col")->getDataStaticPtr();
    static auto* const* PHOVCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:hover_highlight_col")->getDataStaticPtr();
    static auto const*  PLABELMODE  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_text_mode")->getDataStaticPtr();
    static auto const*  PTOKENMAP   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_token_map")->getDataStaticPtr();
    static auto* const* PLABELOX    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_offset_x")->getDataStaticPtr();
    static auto* const* PLABELOY    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_offset_y")->getDataStaticPtr();
    static auto const*  PLABELSHOW  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_show")->getDataStaticPtr();
    static auto* const* PLCOLDEF    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_default")->getDataStaticPtr();
    static auto* const* PLCOLHOV    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_hover")->getDataStaticPtr();
    static auto* const* PLCOLFOC    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_focus")->getDataStaticPtr();
    static auto* const* PLCOLCUR    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_color_current")->getDataStaticPtr();
    static auto* const* PWSNUMCOL   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:workspace_number_color")->getDataStaticPtr();
    static auto* const* PLSCALEH    = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_scale_hover")->getDataStaticPtr();
    static auto* const* PLSCALEF    = (Hyprlang::FLOAT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_scale_focus")->getDataStaticPtr();
    static auto* const* PLBGEN      = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_enable")->getDataStaticPtr();
    static auto* const* PLBGCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_color")->getDataStaticPtr();
    static auto* const* PLBGROUND   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_rounding")->getDataStaticPtr();
    static auto const*  PLBGSHAPE   = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_bg_shape")->getDataStaticPtr();
    static auto* const* PLBGPAD     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_padding")->getDataStaticPtr();

    static auto* const* PBWIDTH     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_width")->getDataStaticPtr();
    static auto const*  PBCOLCUR    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_color_current")->getDataStaticPtr();
    static auto const*  PBCOLFOC    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_color_focus")->getDataStaticPtr();
    static auto const*  PBCOLHOV    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_color_hover")->getDataStaticPtr();
    static auto const*  PBGRCUR     = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_grad_current")->getDataStaticPtr();
    static auto const*  PBGREFOC    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_grad_focus")->getDataStaticPtr();
    static auto const*  PBGREHOV    = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:border_grad_hover")->getDataStaticPtr();

    static auto* const* PDRAGPROXYCOL    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_color")->getDataStaticPtr();
    static auto* const* PDRAGPROXYACTCOL = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_active_color")->getDataStaticPtr();
    static auto const*  PDRAGPROXYBORDER = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_border_color")->getDataStaticPtr();
    static auto* const* PDRAGPROXYBWIDTH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_border_width")->getDataStaticPtr();
    static auto* const* PDRAGPROXYROUND  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_proxy_rounding")->getDataStaticPtr();
    static auto const*  PDRAGSOURCEBORDER = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_source_border_color")->getDataStaticPtr();
    static auto* const* PDRAGSOURCEBWIDTH = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:drag_drop_source_border_width")->getDataStaticPtr();

    static auto* const* PSELECTEN   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_enable")->getDataStaticPtr();
    static auto const*  PSELECTMAP  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_token_map")->getDataStaticPtr();
    static auto const*  PSELECTPOS  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_position")->getDataStaticPtr();
    static auto* const* PSELECTOX   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_offset_x")->getDataStaticPtr();
    static auto* const* PSELECTOY   = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_offset_y")->getDataStaticPtr();
    static auto* const* PSELECTCOL  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:selection_label_color")->getDataStaticPtr();

    auto normalizeAnchor = [](std::string anchor) {
        std::replace(anchor.begin(), anchor.end(), '_', '-');
        return anchor;
    };

    auto resolveWorkspaceName = [&](size_t id) -> std::string {
        if (id >= images.size())
            return {};

        const auto& image = images[id];
        if (image.pWorkspace && !image.pWorkspace->m_name.empty())
            return image.pWorkspace->m_name;

        for (const auto& workspace : State::workspaceState()->workspacesCopy()) {
            if (!workspace || workspace->m_id != image.workspaceID)
                continue;

            if (!workspace->m_name.empty())
                return workspace->m_name;
            break;
        }

        return std::to_string(image.workspaceID);
    };

    auto renderLabel = [&](SP<Render::ITexture>& tex, Vector2D& sz, const std::string& label, const CHyprColor& col, float scaleMul, const CBox& tile, const std::string& anchor,
                           int offsetX, int offsetY, int fontSize) {
        if (label.empty())
            return;

        const int baseF = std::max(8, fontSize);
        if (!tex || tex->m_texID == 0) {
            const int fsz = std::max(8, (int)std::round(baseF * scaleMul));
            Vector2D  buf{std::max(32, fsz * std::max(2, (int)label.size())), std::max(24, fsz + 8)};
            sz  = buf;
            tex = renderNumberTexture(label, col, buf, MON->m_scale, fsz);
        }

        if (!tex || tex->m_texID == 0)
            return;

        static auto* const* PLPIXELSNAP = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:label_pixel_snap")->getDataStaticPtr();

        auto placeBox = [&](const CBox& tileBox, const Vector2D& size, const std::string& anchorName, int offsetX2, int offsetY2) -> CBox {
            double x = tileBox.x, y = tileBox.y;
            if (anchorName == "top-left") {
                x += offsetX2;
                y += offsetY2;
            } else if (anchorName == "top-right") {
                x += tileBox.w - size.x - offsetX2;
                y += offsetY2;
            } else if (anchorName == "bottom-left") {
                x += offsetX2;
                y += tileBox.h - size.y - offsetY2;
            } else if (anchorName == "bottom-right") {
                x += tileBox.w - size.x - offsetX2;
                y += tileBox.h - size.y - offsetY2;
            } else {
                x += (tileBox.w - size.x) / 2.0;
                y += (tileBox.h - size.y) / 2.0;
            }
            return CBox{x, y, (double)size.x, (double)size.y};
        };

        auto drawWithBG = [&]() {
            const int pad = **PLBGPAD;
            Vector2D  bgSize = {sz.x + pad * 2, sz.y + pad * 2};
            const std::string shape{*PLBGSHAPE};
            int roundPx = **PLBGROUND;
            if (shape == "circle" || shape == "square") {
                const double side = std::max(bgSize.x, bgSize.y);
                bgSize            = {side, side};
                roundPx           = (shape == "circle") ? std::lround(side / 2.0) : 0;
            }
            CBox bg = placeBox(tile, bgSize, anchor, offsetX, offsetY);
            CBox lb{bg.x + (bg.w - sz.x) / 2.0, bg.y + (bg.h - sz.y) / 2.0, (double)sz.x, (double)sz.y};
            if (**PLPIXELSNAP) {
                bg.round();
                lb.round();
            }
            Render::GL::g_pHyprOpenGL->renderRect(bg, CHyprColor{(uint64_t)**PLBGCOL}, {.round = roundPx});
            Render::GL::g_pHyprOpenGL->renderTexture(tex, lb, {.a = 1.0});
        };

        auto drawNoBG = [&]() {
            CBox lb = placeBox(tile, sz, anchor, offsetX, offsetY);
            if (**PLPIXELSNAP)
                lb.round();
            Render::GL::g_pHyprOpenGL->renderTexture(tex, lb, {.a = 1.0});
        };

        if (**PLBGEN)
            drawWithBG();
        else
            drawNoBG();
    };

    auto drawBorderForID = [&](int id, const std::string& borderSpec, const std::string& deprecatedGradSpec, int roundScaled, int borderWidthOverride = -1) {
        if (!isTileValid(id) || id < 0 || id >= (int)tileBoxes.size())
            return;
        if (borderWidthOverride == 0)
            return;

        const CBox& box = tileBoxes[id];
        if (box.w <= 0.0 || box.h <= 0.0)
            return;

        const int BWIDTH = borderWidthOverride > 0 ? borderWidthOverride : (int)**PBWIDTH;
        if (BWIDTH <= 0)
            return;

        const std::string effectiveSpec = Hyprexpo::resolveBorderSpec(borderSpec, deprecatedGradSpec);

        if (isGradientBorderSpec(effectiveSpec)) {
            const auto spec = parseGradientSpec(effectiveSpec);
            if (spec.valid) {
                Config::CGradientValueData grad;
                grad.m_colors.clear();
                grad.m_colors.push_back(spec.c1);
                grad.m_colors.push_back(spec.c2);
                grad.m_angle = spec.angleDeg * (float)M_PI / 180.f;
                grad.updateColorsOk();
                Render::GL::g_pHyprOpenGL->renderBorder(box, grad, {.round = roundScaled, .roundingPower = ROUND_PWR, .borderSize = BWIDTH});
            }
        } else if (!effectiveSpec.empty()) {
            Hyprexpo::SColorRGBA parsedColor;
            if (Hyprexpo::parseSolidColorSpec(effectiveSpec, parsedColor)) {
                CHyprColor color{parsedColor.r, parsedColor.g, parsedColor.b, parsedColor.a};
                Render::GL::g_pHyprOpenGL->renderBorder(box, color, {.round = roundScaled, .roundingPower = ROUND_PWR, .borderSize = BWIDTH});
            } else {
                Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] invalid border color config: {}", effectiveSpec);
            }
        }
    };

    auto drawProxyBorder = [&](const CBox& proxy, int round, int borderWidth, const std::string& borderSpec, const std::string& fallbackSpec) {
        if (borderWidth <= 0)
            return;

        const std::string effectiveSpec = borderSpec.empty() ? fallbackSpec : borderSpec;
        if (effectiveSpec.empty())
            return;

        if (isGradientBorderSpec(effectiveSpec)) {
            const auto spec = parseGradientSpec(effectiveSpec);
            if (!spec.valid)
                return;

            Config::CGradientValueData grad;
            grad.m_colors = {spec.c1, spec.c2};
            grad.m_angle  = spec.angleDeg * (float)M_PI / 180.f;
            grad.updateColorsOk();
            Render::GL::g_pHyprOpenGL->renderBorder(proxy, grad, {.round = round, .roundingPower = ROUND_PWR, .borderSize = borderWidth});
            return;
        }

        Hyprexpo::SColorRGBA parsedColor;
        if (!Hyprexpo::parseSolidColorSpec(effectiveSpec, parsedColor)) {
            Log::logger->log(Log::ERR, Log::logFnName(), "[hyprexpo] invalid drag_drop_proxy_border_color config: {}", effectiveSpec);
            return;
        }

        Config::CGradientValueData grad{CHyprColor{parsedColor.r, parsedColor.g, parsedColor.b, parsedColor.a}};
        grad.updateColorsOk();
        Render::GL::g_pHyprOpenGL->renderBorder(proxy, grad, {.round = round, .roundingPower = ROUND_PWR, .borderSize = borderWidth});
    };

    std::vector<std::string> selectionTokens;
    if (!std::string{*PSELECTMAP}.empty())
        selectionTokens = splitCommaList(std::string{*PSELECTMAP});

    if (!closing && (**PLABELEN || **PSELECTEN || showWorkspaceNumbers)) {
        const int labelHoveredID = hoveredID;
        const bool modernPositionSet = CompatHyprlandAPI::configValueSetByUser("plugin:hyprexpo:label_position");
        const bool legacyPositionSet = CompatHyprlandAPI::configValueSetByUser("plugin:hyprexpo:label_pos");
        const bool modernSizeSet     = CompatHyprlandAPI::configValueSetByUser("plugin:hyprexpo:label_font_size");
        const bool legacySizeSet     = CompatHyprlandAPI::configValueSetByUser("plugin:hyprexpo:label_size");
        const std::string labelAnchor = normalizeAnchor(
            Hyprexpo::resolveLabelPosition(std::string{*PLABELPOS}, modernPositionSet, std::string{*PLABELPOSL}, legacyPositionSet));
        const int labelFontSize = Hyprexpo::resolveLabelFontSize(**PLABELSIZE, modernSizeSet, **PLABELSIZEL, legacySizeSet);

        auto resolveState = [&](int id) -> int {
            if (id == kbFocusID)
                return 2;
            if (id == openedID)
                return 3;
            if (id == labelHoveredID)
                return 1;
            return 0;
        };

        std::vector<std::string> labelTokens;
        if (!std::string{*PTOKENMAP}.empty())
            labelTokens = splitCommaList(std::string{*PTOKENMAP});

        int tokenCounter = 0;
        for (size_t id = 0; id < images.size(); ++id) {
            const auto& image = images[id];
            const auto& tile  = tileBoxes[id];

            if (image.workspaceID == WORKSPACE_INVALID || tile.w <= 0.0 || tile.h <= 0.0)
                continue;

            const bool labelEnabled = **PLABELEN || showWorkspaceNumbers;
            const std::string labelShow = showWorkspaceNumbers ? "always" : std::string{*PLABELSHOW};
            if (Hyprexpo::shouldShowWorkspaceLabel(labelEnabled, labelShow, (int)id == labelHoveredID, (int)id == kbFocusID, (int)id == openedID)) {
                std::string label;
                const std::string mode = showWorkspaceNumbers ? std::string{"id"} : std::string{*PLABELMODE};
                if (dynamicGrid && showWorkspaceNames) {
                    label = resolveWorkspaceName(id);
                } else if (mode == "token") {
                    if (tokenCounter < (int)labelTokens.size() && !labelTokens[tokenCounter].empty())
                        label = labelTokens[tokenCounter];
                    else
                        label = fallbackTokenForVisibleIndex(tokenCounter);
                } else if (mode == "index") {
                    label = std::to_string(tokenCounter + 1);
                } else {
                    label = std::to_string(images[id].workspaceID);
                }

                const int st = resolveState((int)id);
                if (!label.empty()) {
                    if (showWorkspaceNumbers)
                        renderLabel(images[id].labelTexDefault, images[id].labelSizeDefault, label, CHyprColor{(uint64_t)**PWSNUMCOL}, 1.0f, tile, labelAnchor, **PLABELOX, **PLABELOY,
                                    labelFontSize);
                    else if (st == 1)
                        renderLabel(images[id].labelTexHover, images[id].labelSizeHover, label, CHyprColor{(uint64_t)**PLCOLHOV}, **PLSCALEH, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                    else if (st == 2)
                        renderLabel(images[id].labelTexFocus, images[id].labelSizeFocus, label, CHyprColor{(uint64_t)**PLCOLFOC}, **PLSCALEF, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                    else if (st == 3)
                        renderLabel(images[id].labelTexCurrent, images[id].labelSizeCurrent, label, CHyprColor{(uint64_t)**PLCOLCUR}, 1.0f, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                    else
                        renderLabel(images[id].labelTexDefault, images[id].labelSizeDefault, label, CHyprColor{(uint64_t)**PLCOLDEF}, 1.0f, tile, labelAnchor, **PLABELOX,
                                    **PLABELOY, labelFontSize);
                }
            }

            if (**PSELECTEN && tokenCounter < (int)selectionTokens.size() && !selectionTokens[tokenCounter].empty())
                renderLabel(images[id].selectionLabelTex, images[id].selectionLabelSize, selectionTokens[tokenCounter], CHyprColor{(uint64_t)**PSELECTCOL}, 1.0f, tile,
                            std::string{*PSELECTPOS}, **PSELECTOX, **PSELECTOY, **PLABELSIZE);

            ++tokenCounter;
        }
    }

    const int RND_CUR = CURRENT_ROUND_SCALED;
    const int RND_FOC = FOCUS_ROUND_SCALED;
    const int RND_HOV = HOVER_ROUND_SCALED;

    auto legacyColorSpec = [](uint64_t color) {
        constexpr char HEX[] = "0123456789abcdef";
        std::string    spec  = "0x00000000";
        for (int index = 0; index < 8; ++index)
            spec[9 - index] = HEX[(color >> (index * 4)) & 0xF];
        return spec;
    };

    const std::string currentLegacySpec = dynamicGrid ? legacyColorSpec((uint64_t)**PACTCOL) : std::string{};
    const std::string hoverLegacySpec   = dynamicGrid ? legacyColorSpec((uint64_t)**PHOVCOL) : std::string{};
    const std::string currentFallback   = Hyprexpo::resolveBorderSpec(std::string{*PBGRCUR}, currentLegacySpec);
    const std::string hoverFallback     = Hyprexpo::resolveBorderSpec(std::string{*PBGREHOV}, hoverLegacySpec);

    if (hoveredID != -1 && hoveredID != openedID && hoveredID != kbFocusID)
        drawBorderForID(hoveredID, std::string{*PBCOLHOV}, hoverFallback, RND_HOV);
    drawBorderForID(openedID, std::string{*PBCOLCUR}, currentFallback, RND_CUR);
    if (kbFocusID != -1)
        drawBorderForID(kbFocusID, std::string{*PBCOLFOC}, std::string{*PBGREFOC}, RND_FOC);

    const auto OVERVIEWKEY = overviewMonitorKey(MON);
    if (g_overviewDrag.state.moved && g_overviewDrag.state.sourceMonitorKey == OVERVIEWKEY && isTileValid(g_overviewDrag.state.sourceTileIndex)) {
        const std::string sourceBorder = std::string{*PDRAGSOURCEBORDER}.empty() ? std::string{*PBCOLFOC} : std::string{*PDRAGSOURCEBORDER};
        const int         sourceWidth  = **PDRAGSOURCEBWIDTH >= 0 ? **PDRAGSOURCEBWIDTH : (int)**PBWIDTH;
        drawBorderForID(g_overviewDrag.state.sourceTileIndex, sourceBorder, std::string{*PBGREFOC}, RND_FOC, sourceWidth);
    }

    if (g_overviewDrag.window && g_overviewDrag.state.targetMonitorKey == OVERVIEWKEY && isTileValid(g_overviewDrag.state.targetTileIndex)) {
        const auto windowBox = g_overviewDrag.window->getWindowMainSurfaceBox();
        if (windowBox.w > 0 && windowBox.h > 0) {
            const int  TARGET             = g_overviewDrag.state.targetTileIndex;
            const auto targetTileBox      = tileBoxForIndex(TARGET, SIZE, GAPSIZE, OUTER, true);
            const Vector2D pointerLocal   = g_overviewDrag.pointerGlobal - MON->m_position;
            const auto dropIntent = Hyprexpo::computeDropIntentGeometry({
                .targetValid     = true,
                .pointerLocal    = {pointerLocal.x, pointerLocal.y},
                .targetTileLocal = {targetTileBox.x, targetTileBox.y, targetTileBox.w, targetTileBox.h},
                .workspaceSize   = {MON->m_size.x, MON->m_size.y},
                .windowSize      = {windowBox.w, windowBox.h},
                .grabOffset      = {g_overviewDrag.grabOffset.x, g_overviewDrag.grabOffset.y},
            });
            if (dropIntent.valid) {
                CBox proxy{
                    dropIntent.targetProxyLocal.x,
                    dropIntent.targetProxyLocal.y,
                    dropIntent.targetProxyLocal.w,
                    dropIntent.targetProxyLocal.h,
                };
                proxy.scale(MON->m_scale).translate(pos->value());
                proxy.round();

                const int maxProxyRound = std::max(0, (int)std::floor(std::min(proxy.w, proxy.h) / 2.0));
                const int autoRound     = std::min(RND_FOC, maxProxyRound);
                const int round        = **PDRAGPROXYROUND >= 0 ? std::min(std::max(0, (int)std::lround((double)**PDRAGPROXYROUND * MON->m_scale)), maxProxyRound) : autoRound;

                Render::GL::g_pHyprOpenGL->renderRect(proxy, CHyprColor{(uint64_t)(g_overviewDrag.state.moved ? **PDRAGPROXYACTCOL : **PDRAGPROXYCOL)}, {.round = round, .roundingPower = ROUND_PWR});

                const int borderWidth = **PDRAGPROXYBWIDTH >= 0 ? **PDRAGPROXYBWIDTH : std::max(2, (int)**PBWIDTH + 1);
                std::string effectiveSpec = std::string{*PDRAGPROXYBORDER}.empty() ? std::string{*PBCOLFOC} : std::string{*PDRAGPROXYBORDER};
                if (effectiveSpec.empty())
                    effectiveSpec = std::string{*PBGREFOC};
                drawProxyBorder(proxy, round, borderWidth, effectiveSpec, std::string{*PBGREFOC});
            }
        }
    }

    if (entryAnimationPending)
        damage();
}
