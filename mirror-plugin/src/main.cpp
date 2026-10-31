// Capture/render-hook approach adapted from hyprexpo (BSD-3-Clause); see NOTICE.
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
extern "C" {
#include <lua.h>
}
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/state/LayerFadeout.hpp>
#include <hyprland/src/desktop/state/PopupFadeout.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
// This exact-version plugin needs renderWorkspace and the render-mode guard.
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#undef private

#include "gpu-source.hpp"
#include "source-cursor.hpp"
namespace {
template <typename Fn> struct ScopeExit {
    Fn fn;
    ~ScopeExit() { fn(); }
};
constexpr std::string_view COMMIT = "efb50993780079460b0cbed1363e2166a2de1d9f";
static_assert(std::string_view{GIT_COMMIT_HASH} == COMMIT, "Rebuild only against pinned Hyprland 0.56.2 headers");
HANDLE handle = nullptr;
CFunctionHook *windowHook = nullptr, *visibleHook = nullptr;
CFunctionHook *damageWindowHook = nullptr, *commitHook = nullptr, *cursorMoveHook = nullptr;
CFunctionHook *resourcesHook = nullptr, *damageBoxHook = nullptr, *damageMonitorHook = nullptr;
CFunctionHook *popupFadeoutHook = nullptr, *renderFadeoutsHook = nullptr;
CFunctionHook *layerRenderHook = nullptr, *fullscreenFadeHook = nullptr, *fullscreenOverrideHook = nullptr;
using RenderWindow = bool (*)(void*, PHLWINDOW, PHLMONITOR);
using VisibleWindow = bool (*)(void*, PHLWINDOW);
using DamageWindow = void (*)(void*, PHLWINDOW, bool);
using CommitSurface = void (*)(CWLSurfaceResource*, SSurfaceState&);
using CursorMoved = void (*)(void*);
using MonitorResources = WP<Monitor::CMonitorResources> (*)(Monitor::CMonitor*);
using DamageBox = void (*)(void*, const CBox&, bool);
using DamageMonitor = void (*)(void*, PHLMONITOR);
using CreatePopupFadeout = SP<Desktop::CPopupFadeout> (*)(SP<Desktop::View::CPopup>, SP<Render::IFramebuffer>, float);
using RenderFadeouts = void (*)(void*, PHLMONITOR, Desktop::eFadeoutPlane, PHLWORKSPACE);
using RenderLayer = void (*)(void*, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool);
using FullscreenFade = void (*)(PHLWORKSPACE, Animation::Workspace::eAnimationType);
using FullscreenOverride = void (*)(PHLWORKSPACE, float, PHLWINDOW);
SP<SHyprCtlCommand> statusCommand, frameCommand;
int frameRate = 30;
uint32_t captureWidth = 0, captureHeight = 0;
uint64_t lastPublishedPts = 0;
std::string sourceName;
SP<CEventLoopTimer> timer;
CHyprSignalListener lockListener, unlockListener, removedListener, movedListener, layoutListener;
CHyprSignalListener layerOpenedListener, layerClosedListener, layerRulesListener;
PHLMONITORREF sourceRef;
PHLWORKSPACEREF workspaceRef;
SP<Render::IFramebuffer> framebuffer;
UP<Monitor::CMonitorResources> captureResources;
PHLMONITORREF resourceSource;
Vector2D resourceSize;
DRMFormat resourceFormat = 0;
NColorManagement::PImageDescription resourceDescription;
Render::CRenderPass capturePass;
Render::SRenderData savedRenderData;
decltype(g_pHyprRenderer->m_cmSettingsCache) captureCMSettings;
OmarchyDisplay::MirrorGpuSource gpuSource;
OmarchyDisplay::SourceCursorTracker sourceCursor;
bool active = false, ready = false, capturing = false;
bool dirty = false, waiting = false;
Time::steady_tp lastCapture{};
bool externalTextures = false;

struct LayerVisualState {
    const void* identity;
    CBox box;
    float alpha;
    bool mapped;
    uint32_t level;
    bool aboveFullscreen;
    bool operator==(const LayerVisualState&) const = default;
};
std::vector<LayerVisualState> layerVisualState;
struct PopupFadeoutOwner {
    WP<Desktop::CPopupFadeout> fadeout;
    PHLWORKSPACEREF workspace;
    PHLMONITORREF layerMonitor;
};
std::vector<PopupFadeoutOwner> popupFadeoutOwners;
std::vector<SP<Desktop::IFadeout>> filteredPopupFadeouts;
struct LayerFullscreenPolicy {
    PHLLSREF layer;
    float normalAlpha = 1.F;
    bool nativeFullscreenAlpha = false;
    bool aboveSelectedFullscreen = false;
    PHLWORKSPACEREF nativeFullscreenWorkspace;
};
std::vector<LayerFullscreenPolicy> layerFullscreenPolicy;
PHLWORKSPACEREF layerPolicyWorkspace, layerPhysicalWorkspace;
PHLWINDOWREF layerSelectedFullscreen;
std::vector<std::pair<PHLWINDOW, float>> savedWindowFullscreenAlpha;

void invalidate();
void sourceWake();
uint64_t monotonicUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Time::steadyNow().time_since_epoch()).count();
}

bool locked() {
    // Start-locked and an inactive session must fail closed too.
    return !g_pCompositor->m_sessionActive || g_pCompositor->m_startLocked || g_pSessionLockManager->isSessionLocked();
}

bool ownedName(std::string_view name) {
    constexpr std::string_view prefix = "OMARCHY-TV-";
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 8)
        return false;
    return std::ranges::all_of(name.substr(prefix.size()), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// No animation API calls: changing only the current value preserves goals,
// begun values, spring velocity, timestamps, callbacks and active-list membership.
template <typename T> class CurrentValue {
  public:
    explicit CurrentValue(PHLANIMVAR<T>& variable, const T& value) : variable(variable), previous(variable->value()) { variable->value() = value; }
    ~CurrentValue() { variable->value() = previous; }
  private:
    PHLANIMVAR<T>& variable;
    T previous;
};

PHLWINDOW coveringFullscreen(const PHLWORKSPACE& workspace) {
    const auto window = Fullscreen::controller()->getFullscreenWindow(workspace, true);
    return window && Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN ? window : PHLWINDOW{};
}

// LS_ALPHA_FADE is shared by lifecycle and physical-workspace fullscreen
// fades. Track just the latter's provenance and selected-workspace policy;
// never retarget, warp or otherwise alter a native animation.
void updateLayerFullscreenPolicy() {
    if (!active || capturing) return;
    const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
    if (!source || !workspace) return;
    const auto fullscreen = coveringFullscreen(workspace);
    const bool changedWorkspace = layerPolicyWorkspace != workspace;
    const bool changedFullscreen = layerSelectedFullscreen != fullscreen;
    if (changedWorkspace) layerFullscreenPolicy.clear();
    else if (changedFullscreen && fullscreen)
        for (auto& policy : layerFullscreenPolicy) policy.aboveSelectedFullscreen = false;
    layerPolicyWorkspace = workspace;
    layerPhysicalWorkspace = source->m_activeWorkspace;
    layerSelectedFullscreen = fullscreen;
    std::erase_if(layerFullscreenPolicy, [&](const auto& policy) {
        return !policy.layer || policy.layer->m_monitor != source || policy.layer->m_layer != ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    });
    const auto physicalFullscreenWorkspace = coveringFullscreen(source->m_activeSpecialWorkspace) ? source->m_activeSpecialWorkspace :
        (coveringFullscreen(source->m_activeWorkspace) ? source->m_activeWorkspace : PHLWORKSPACE{});
    for (const auto& layer : source->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) {
        if (!layer) continue;
        const auto& alpha = layer->alpha()[Desktop::View::LS_ALPHA_FADE];
        // Pinned writers: animateIn targets 1; animateOut targets 0 only
        // after unmap (in its separate fadeout). A mapped Top layer with a
        // zero goal therefore carries fullscreen suppression, even if the
        // physical workspace has since changed and left that goal stale.
        const bool suppressed = layer->m_mapped && alpha->goal() == 0.F;
        const auto found = std::ranges::find_if(layerFullscreenPolicy, [&](const auto& policy) { return policy.layer == layer; });
        if (found != layerFullscreenPolicy.end()) {
            if (suppressed && !found->nativeFullscreenAlpha) {
                found->nativeFullscreenAlpha = true;
                found->nativeFullscreenWorkspace = physicalFullscreenWorkspace ? physicalFullscreenWorkspace : source->m_activeWorkspace;
            }
            continue;
        }
        layerFullscreenPolicy.push_back({layer, suppressed ? 1.F : alpha->goal(), suppressed,
            source->m_activeWorkspace == workspace ? layer->m_aboveFullscreen : false,
            physicalFullscreenWorkspace ? physicalFullscreenWorkspace : source->m_activeWorkspace});
    }
}

LayerFullscreenPolicy* layerPolicy(const PHLLS& layer) {
    const auto found = std::ranges::find_if(layerFullscreenPolicy, [&](const auto& policy) { return policy.layer == layer; });
    return found == layerFullscreenPolicy.end() ? nullptr : &*found;
}

float selectedLayerAlpha(const PHLLS& layer) {
    const auto& alpha = layer->alpha()[Desktop::View::LS_ALPHA_FADE];
    const auto* policy = layerPolicy(layer);
    if (!policy || !layer->m_mapped) return alpha->value();
    if (layerPhysicalWorkspace == workspaceRef &&
        (!policy->nativeFullscreenAlpha ||
         (policy->nativeFullscreenWorkspace == workspaceRef && (layerSelectedFullscreen || alpha->goal() != 0.F)))) return alpha->value();
    if (layerSelectedFullscreen && !policy->aboveSelectedFullscreen) return 0.F;
    return policy->nativeFullscreenAlpha ? policy->normalAlpha : alpha->value();
}

void markNativeFullscreenAlpha(const PHLWORKSPACE& workspace) {
    if (!active || capturing || !workspace) return;
    const auto source = sourceRef.lock();
    if (!source || (workspace != source->m_activeWorkspace && workspace != source->m_activeSpecialWorkspace)) return;
    updateLayerFullscreenPolicy();
    for (auto& policy : layerFullscreenPolicy) {
        if (!policy.layer || !policy.layer->m_mapped) continue;
        if (!policy.nativeFullscreenAlpha)
            policy.normalAlpha = policy.layer->alpha()[Desktop::View::LS_ALPHA_FADE]->goal();
        policy.nativeFullscreenAlpha = true;
        policy.nativeFullscreenWorkspace = workspace;
    }
}

void fullscreenFade(PHLWORKSPACE workspace, Animation::Workspace::eAnimationType type) {
    markNativeFullscreenAlpha(workspace);
    reinterpret_cast<FullscreenFade>(fullscreenFadeHook->m_original)(workspace, type);
}
void fullscreenOverride(PHLWORKSPACE workspace, float fade, PHLWINDOW exclude) {
    markNativeFullscreenAlpha(workspace);
    reinterpret_cast<FullscreenOverride>(fullscreenOverrideHook->m_original)(workspace, fade, exclude);
}
void renderLayer(void* renderer, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& time, bool popups, bool lockscreen) {
    const auto original = reinterpret_cast<RenderLayer>(layerRenderHook->m_original);
    if (!capturing || !layer || monitor != sourceRef.lock() || layer->m_layer != ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
        original(renderer, layer, monitor, time, popups, lockscreen);
        return;
    }
    CurrentValue<float> alpha{layer->alpha()[Desktop::View::LS_ALPHA_FADE], selectedLayerAlpha(layer)};
    original(renderer, layer, monitor, time, popups, lockscreen);
}

// Capture is only entered between monitor renders. Preserve the caller's GL
// bindings/capabilities as well as Hyprland's renderer data, without pixel readback.
class GLState {
  public:
    GLState() {
        display = eglGetCurrentDisplay(); context = eglGetCurrentContext();
        draw = eglGetCurrentSurface(EGL_DRAW); read = eglGetCurrentSurface(EGL_READ);
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
        glGetIntegerv(GL_VIEWPORT, viewport.data()); glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
        glGetIntegerv(GL_CURRENT_PROGRAM, &program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &vbo); glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &textureUnit);
        for (size_t i = 0; i < textures.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i][0]);
            glGetIntegerv(GL_TEXTURE_BINDING_3D, &textures[i][1]);
            if (externalTextures) glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &textures[i][2]);
        }
        glActiveTexture(textureUnit);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB); glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha); glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &equationRGB); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equationAlpha);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data()); glGetBooleanv(GL_COLOR_WRITEMASK, colorMask.data());
        glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &stencilClear);
        constexpr std::array<GLenum, 7> frontQueries{GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK, GL_STENCIL_WRITEMASK,
            GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS};
        constexpr std::array<GLenum, 7> backQueries{GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK, GL_STENCIL_BACK_WRITEMASK,
            GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL, GL_STENCIL_BACK_PASS_DEPTH_PASS};
        for (size_t i = 0; i < frontQueries.size(); ++i) {
            glGetIntegerv(frontQueries[i], &frontStencil[i]); glGetIntegerv(backQueries[i], &backStencil[i]);
        }
        for (size_t i = 0; i < caps.size(); ++i) enabled[i] = glIsEnabled(caps[i]);
        cachedProgram = Render::GL::g_pHyprOpenGL->m_currentProgram;
        cachedViewport = Render::GL::g_pHyprOpenGL->m_lastViewport;
        cachedCaps = Render::GL::g_pHyprOpenGL->m_capStatus;
        cachedBlend = Render::GL::g_pHyprOpenGL->m_blend;
        cachedFake = Render::GL::g_pHyprOpenGL->m_fakeFrame;
        cachedFinal = Render::GL::g_pHyprOpenGL->m_applyFinalShader;
        cachedOffloaded = Render::GL::g_pHyprOpenGL->m_offloadedFramebuffer;
    }
    ~GLState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB); glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]); glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        glUseProgram(program); Render::GL::g_pHyprOpenGL->m_currentProgram = cachedProgram;
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        for (size_t i = 0; i < textures.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, textures[i][0]);
            glBindTexture(GL_TEXTURE_3D, textures[i][1]);
            if (externalTextures) glBindTexture(GL_TEXTURE_EXTERNAL_OES, textures[i][2]);
        }
        glActiveTexture(textureUnit);
        glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha); glBlendEquationSeparate(equationRGB, equationAlpha);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glClearStencil(stencilClear);
        restoreStencil(GL_FRONT, frontStencil); restoreStencil(GL_BACK, backStencil);
        for (size_t i = 0; i < caps.size(); ++i) { if (enabled[i]) glEnable(caps[i]); else glDisable(caps[i]); }
        Render::GL::g_pHyprOpenGL->m_lastViewport = cachedViewport;
        Render::GL::g_pHyprOpenGL->m_capStatus = cachedCaps;
        Render::GL::g_pHyprOpenGL->m_blend = cachedBlend;
        Render::GL::g_pHyprOpenGL->m_fakeFrame = cachedFake;
        Render::GL::g_pHyprOpenGL->m_applyFinalShader = cachedFinal;
        Render::GL::g_pHyprOpenGL->m_offloadedFramebuffer = cachedOffloaded;
        if (context != eglGetCurrentContext() || draw != eglGetCurrentSurface(EGL_DRAW) || read != eglGetCurrentSurface(EGL_READ))
            eglMakeCurrent(display == EGL_NO_DISPLAY ? eglGetCurrentDisplay() : display, draw, read, context);
    }
  private:
    EGLDisplay display; EGLContext context; EGLSurface draw, read;
    GLint drawFB, readFB, program, vao, vbo, ebo, textureUnit, srcRGB, dstRGB, srcAlpha, dstAlpha, equationRGB, equationAlpha;
    GLuint cachedProgram;
    decltype(Render::GL::g_pHyprOpenGL->m_lastViewport) cachedViewport;
    decltype(Render::GL::g_pHyprOpenGL->m_capStatus) cachedCaps;
    bool cachedBlend, cachedFake, cachedFinal, cachedOffloaded;
    // The pinned renderer uses units 0..2 and unit 8 for the color-management LUT.
    std::array<std::array<GLint, 3>, 9> textures;
    std::array<GLint, 4> viewport, scissor;
    std::array<GLint, 7> frontStencil, backStencil;
    GLint stencilClear;
    static void restoreStencil(GLenum face, const std::array<GLint, 7>& state) {
        glStencilFuncSeparate(face, state[0], state[1], state[2]); glStencilMaskSeparate(face, state[3]);
        glStencilOpSeparate(face, state[4], state[5], state[6]);
    }
    std::array<GLfloat, 4> clearColor;
    std::array<GLboolean, 4> colorMask;
    static constexpr std::array<GLenum, 5> caps{GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE};
    std::array<GLboolean, caps.size()> enabled;
};

bool validSource() {
    const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
    return source && valid(workspace) && source->enabled() && source->m_output &&
        !source->isMirror() && !workspace->m_isSpecialWorkspace && workspace->m_id > 0 &&
        workspace->m_monitor == source && !ownedName(source->m_name) && !gpuSource.failed();
}

bool prepareFramebuffer(const PHLMONITOR& source) {
    if (!framebuffer) framebuffer = g_pHyprRenderer->createFB("omarchy live workspace mirror");
    const auto size = source->m_transformedSize;
    const auto format = source->useFP16() ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR8888;
    if (size.x < 1 || size.y < 1 || !framebuffer->alloc(size.x, size.y, format)) return false;
    framebuffer->setImageDescription(source->workBufferImageDescription());
    return true;
}

// The pinned native popup snapshot has no workspace provenance. Record it at
// creation without mutating native state; unknown pre-plugin snapshots fail
// closed during capture. Physical rendering always uses the original list.
SP<Desktop::CPopupFadeout> createPopupFadeout(SP<Desktop::View::CPopup> popup, SP<Render::IFramebuffer> snapshot, float alpha) {
    const auto layer = popup ? popup->layerOwner() : PHLLS{};
    const auto root = popup ? popup->getT1Owner() : SP<Desktop::View::CWLSurface>{};
    const auto window = root ? Desktop::View::CWindow::fromView(root->view()) : PHLWINDOW{};
    const auto result = reinterpret_cast<CreatePopupFadeout>(popupFadeoutHook->m_original)(popup, snapshot, alpha);
    std::erase_if(popupFadeoutOwners, [](const auto& owner) { return owner.fadeout.expired(); });
    if (result) {
        PopupFadeoutOwner owner;
        owner.fadeout = result;
        if (window) owner.workspace = window->m_workspace;
        if (layer) owner.layerMonitor = layer->m_monitor;
        popupFadeoutOwners.push_back(std::move(owner));
    }
    return result;
}

bool ownsPopupFadeout(const Desktop::IFadeout* fadeout) {
    for (const auto& owner : popupFadeoutOwners) {
        if (owner.fadeout.get() != fadeout) continue;
        return (owner.layerMonitor && owner.layerMonitor == sourceRef) || (owner.workspace && owner.workspace == workspaceRef);
    }
    return false;
}

void renderFadeouts(void* renderer, PHLMONITOR monitor, Desktop::eFadeoutPlane plane, PHLWORKSPACE workspace) {
    const auto original = reinterpret_cast<RenderFadeouts>(renderFadeoutsHook->m_original);
    if (!capturing || plane != Desktop::FADEOUT_PLANE_POPUP) {
        original(renderer, monitor, plane, workspace);
        return;
    }
    // The actual object is mutable; only the public accessor is const. Swap
    // backing vectors rather than copy or permanently alter the physical list.
    auto& native = const_cast<std::vector<SP<Desktop::IFadeout>>&>(Desktop::fadingOutState()->fadeouts());
    auto all = std::move(native);
    std::swap(native, filteredPopupFadeouts);
    ScopeExit restore{[&] {
        native.clear();
        std::swap(native, filteredPopupFadeouts);
        std::swap(native, all);
    }};
    for (const auto& fadeout : all)
        if (fadeout && fadeout->plane() == plane && ownsPopupFadeout(fadeout.get())) native.push_back(fadeout);
    original(renderer, monitor, plane, workspace);
}

// Never let FULL_FAKE borrow the physical output's workbuffers, stencil or
// preblur cache. The exact-version resource hook is active only in capture().
WP<Monitor::CMonitorResources> monitorResources(Monitor::CMonitor* monitor) {
    if (capturing && monitor == sourceRef.get()) return captureResources;
    return reinterpret_cast<MonitorResources>(resourcesHook->m_original)(monitor);
}

void prepareResources(const PHLMONITOR& source) {
    const auto format = source->useFP16() ? DRM_FORMAT_ABGR16161616F : source->m_output->state->state().drmFormat;
    const auto description = source->workBufferImageDescription();
    if (captureResources && resourceSource == source && resourceSize == source->m_pixelSize &&
        resourceFormat == format && resourceDescription == description) return;
    captureResources = makeUnique<Monitor::CMonitorResources>(source, format, source->m_pixelSize, description);
    resourceSource = source;
    resourceSize = source->m_pixelSize;
    resourceFormat = format;
    resourceDescription = description;
    source->m_blurFBDirty = true;
    source->m_blurFBShouldRender = false;
}

// Export is a single private-target draw, not another monitor render. Explicit
// RPT_EXPORT uses fbSize rather than either cached physical-monitor matrix.
// Select the native CM shader directly: renderTexture's normal dispatch can
// bypass CM for a physical output using hardware CM or render:cm_enabled=0.
void composeExport(const SP<Render::IFramebuffer>& target) {
    auto& gl = Render::GL::g_pHyprOpenGL;
    auto& data = g_pHyprRenderer->m_renderData;
    static const auto srgb = [] {
        NColorManagement::SImageDescription description{};
        description.transferFunction = NColorManagement::CM_TRANSFER_FUNCTION_SRGB;
        description.primariesNameSet = true;
        description.primariesNamed = NColorManagement::CM_PRIMARIES_SRGB;
        description.primaries = NColorManagement::NColorPrimaries::BT709;
        return NColorManagement::CImageDescription::from(description);
    }();
    target->setImageDescription(srgb);
    data.pMonitor = sourceRef;
    g_pHyprRenderer->bindFB(target);
    data.fbSize = target->m_size;
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    gl->setViewport(0, 0, captureWidth, captureHeight);
    gl->m_applyFinalShader = false;
    gl->setCapStatus(Render::GL::CHyprOpenGLImpl::CAP_STATUS_STENCIL_TEST, false);
    gl->setCapStatus(Render::GL::CHyprOpenGLImpl::CAP_STATUS_SCISSOR_TEST, false);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->blend(false);
    glClearColor(0.F, 0.F, 0.F, 1.F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (locked()) return;

    const auto texture = framebuffer->getTexture();
    const auto description = framebuffer->imageDescription();
    const auto settings = g_pHyprRenderer->getCMSettings(description, srgb, nullptr, false);
    Render::ShaderFeatureFlags features = Render::SH_FEAT_CM;
    if (settings.needsTonemap) features |= Render::SH_FEAT_TONEMAP;
    if (settings.needsTonemap && settings.tonemapMode == 3) features |= Render::SH_FEAT_ALT_TONEMAP;
    auto shader = gl->useShader(gl->getShaderVariant(Render::SH_FRAG_SURFACE, features));
    gl->passCMUniforms(shader, description, srgb, false, -1.F, -1, settings);
    const auto size = framebuffer->m_size;
    const double scale = std::min(target->m_size.x / size.x, target->m_size.y / size.y);
    const CBox box{(target->m_size.x - size.x * scale) / 2, (target->m_size.y - size.y * scale) / 2, size.x * scale, size.y * scale};
    const auto projection = g_pHyprRenderer->projectBoxToTarget(box, texture->m_transform);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, projection.getMatrix());
    shader->setUniformFloat(SHADER_ALPHA, 1.F);
    shader->setUniformInt(SHADER_TEX, 0);
    glActiveTexture(GL_TEXTURE0);
    texture->bind();
    texture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    texture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    texture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    // A shared variant may last have drawn a custom-UV surface.
    if (shader->usesCustomUV()) {
        glBindBuffer(GL_ARRAY_BUFFER, shader->getUniformLocation(SHADER_SHADER_VBO));
        glBufferData(GL_ARRAY_BUFFER, sizeof(Render::GL::fullVerts), Render::GL::fullVerts.data(), GL_DYNAMIC_DRAW);
        shader->setUsesCustomUV(false);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    texture->unbind();
}

bool capture() {
    if (!active || !validSource() || !gpuSource.connected()) return false;
    const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
    updateLayerFullscreenPolicy();
    const auto cursor = sourceCursor.snapshot(); // real workspace, before temporary render state
    // GLState also restores the context that was current on entry.
    GLState glState;
    const int slot = gpuSource.acquire();
    if (slot < 0) return false;
    bool published = false;
    ScopeExit releaseSlot{[&] { if (!published) gpuSource.cancel(slot); }};
    const auto oldTransform = source->m_transform;
    const auto oldPixels = source->m_pixelSize;
    const auto oldTransformed = source->m_transformedSize;
    const auto oldActive = source->m_activeWorkspace;
    const auto oldSpecial = source->m_activeSpecialWorkspace;
    const bool wasVisible = workspace->m_visible, wasForced = workspace->m_forceRendering;
    const bool oldFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const auto oldMode = g_pHyprRenderer->m_renderMode;
    auto* const oldCurrentPass = g_pHyprRenderer->m_currentPass;
    const auto oldTransformDepth = g_pHyprRenderer->m_monitorTransformStack.size();
    const bool oldTransformEnabled = g_pHyprRenderer->m_monitorTransformEnabled;
    const bool oldBlurDirty = source->m_blurFBDirty, oldBlurShouldRender = source->m_blurFBShouldRender;
    const uint64_t pts = monotonicUs();
    capturing = true;
    std::swap(savedRenderData, g_pHyprRenderer->m_renderData);
    std::swap(capturePass, g_pHyprRenderer->m_renderPass);
    std::swap(captureCMSettings, g_pHyprRenderer->m_cmSettingsCache);
    g_pHyprRenderer->m_currentPass = nullptr;
    ScopeExit restore{[&] {
        // endRender may already have popped its stacks before throwing.
        // Unwind directly, never replay or finalize a partly executed pass.
        g_pHyprRenderer->m_renderPass.clear();
        while (g_pHyprRenderer->m_monitorTransformStack.size() > oldTransformDepth)
            g_pHyprRenderer->m_monitorTransformStack.pop();
        g_pHyprRenderer->m_monitorTransformEnabled = oldTransformEnabled;
        source->m_blurFBDirty = oldBlurDirty; source->m_blurFBShouldRender = oldBlurShouldRender;
        std::swap(capturePass, g_pHyprRenderer->m_renderPass);
        std::swap(savedRenderData, g_pHyprRenderer->m_renderData);
        std::swap(captureCMSettings, g_pHyprRenderer->m_cmSettingsCache);
        g_pHyprRenderer->m_currentPass = oldCurrentPass;
        g_pHyprRenderer->m_renderMode = oldMode;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = oldFeedback;
        source->m_transform = oldTransform; source->m_pixelSize = oldPixels; source->m_transformedSize = oldTransformed;
        source->m_activeWorkspace = oldActive; source->m_activeSpecialWorkspace = oldSpecial;
        workspace->m_visible = wasVisible; workspace->m_forceRendering = wasForced;
        capturing = false;
    }};
    source->m_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    source->m_pixelSize = oldTransformed;
    // Dirty captures need fresh private preblur pixels; physical-output
    // damage must never decide the validity of this separate cache.
    source->m_blurFBDirty = true; source->m_blurFBShouldRender = false;
    if (!locked()) {
        if (!prepareFramebuffer(source)) throw std::runtime_error("cannot allocate source framebuffer");
        prepareResources(source);
        source->m_activeWorkspace = workspace; source->m_activeSpecialWorkspace.reset();
        workspace->m_visible = true; workspace->m_forceRendering = true;
        CurrentValue<float> alpha{workspace->m_alpha, 1.F};
        CurrentValue<Vector2D> offset{workspace->m_renderOffset, Vector2D{}};
        CurrentValue<float> specialDim{source->m_specialDim, 0.F}, specialBlur{source->m_specialBlur, 0.F};
        // The dedicated FULLSCREEN channel is policy, not lifecycle/opacity.
        // Native fullscreen fade/override writers can leave it suppressed on
        // an inactive workspace until WindowState::raise (physical focus)
        // recomputes it from isBlockedByFullscreen(). Do that policy decision
        // only for this render, before enumeration/effectiveAlpha culling.
        // Leave FADE, ACTIVE, LAYOUT and move channels entirely untouched.
        ScopeExit restoreWindowFullscreenAlpha{[] {
            for (const auto& [window, previous] : savedWindowFullscreenAlpha)
                window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->value() = previous;
            savedWindowFullscreenAlpha.clear();
        }};
        for (const auto& window : Desktop::windowState()->windows()) {
            if (!window->m_isMapped || window->isHidden() || window->m_workspace != workspace ||
                window->isBlockedByFullscreen()) continue;
            auto& fullscreenAlpha = window->alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
            // A legitimate fade back toward visible must retain its current
            // value; only the stale zero-target suppression needs repair.
            if (fullscreenAlpha->value() == 1.F || fullscreenAlpha->goal() != 0.F) continue;
            savedWindowFullscreenAlpha.emplace_back(window, fullscreenAlpha->value());
            fullscreenAlpha->value() = 1.F;
        }
        CRegion damage{0., 0., framebuffer->m_size.x, framebuffer->m_size.y};
        if (!g_pHyprRenderer->beginRender(source, damage, Render::RENDER_MODE_FULL_FAKE, nullptr, framebuffer))
            throw std::runtime_error("cannot begin workspace capture");
        g_pHyprRenderer->m_renderData.fbSize = framebuffer->m_size;
        g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
        g_pHyprRenderer->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}});
        g_pHyprRenderer->startRenderPass();
        const auto now = Time::steadyNow();
        g_pHyprRenderer->renderWorkspace(source, workspace, now, CBox{{}, framebuffer->m_size});
        sourceCursor.compose(source, cursor, now, damage);
        g_pHyprRenderer->endRender();
    }
    // No second monitor begin/end: export touches only the acquired DMA-BUF.
    composeExport(gpuSource.target(slot));
    gpuSource.publish(slot, pts);
    published = true;
    lastPublishedPts = pts;
    ready = !locked();
    return true;
}

void cursorMoved(void* manager) {
    reinterpret_cast<CursorMoved>(cursorMoveHook->m_original)(manager);
    sourceCursor.observeMovement();
}
bool shouldRenderWindow(void* renderer, PHLWINDOW window, PHLMONITOR monitor) {
    if (capturing)
        return window && window->m_isMapped && !window->isHidden() && window->m_workspace == workspaceRef.lock() && monitor == sourceRef.lock();
    return reinterpret_cast<RenderWindow>(windowHook->m_original)(renderer, window, monitor);
}
bool shouldRefreshWindow(void* renderer, PHLWINDOW window) {
    // Keep off-workspace clients eligible for frame-driven rendering instead of
    // allowing the unfocused/suspension machinery to freeze the clone.
    if (active && !locked() && validSource() && window && window->m_isMapped && !window->isHidden() && window->m_workspace == workspaceRef.lock()) return true;
    return reinterpret_cast<VisibleWindow>(visibleHook->m_original)(renderer, window);
}

// A timer may service inactive clients' frame callbacks without rendering.
// Real surface commits and decoration/geometry damage invalidate the clone.
void invalidate() {
    if (!active || capturing || locked()) return;
    dirty = true;
    if (!waiting) return;
    const auto interval = std::chrono::nanoseconds{1'000'000'000 / frameRate};
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Time::steadyNow() - lastCapture);
    timer->updateTimeout(std::max(std::chrono::nanoseconds{1}, interval - elapsed));
}

bool containsSurface(CWLSurfaceResource* surface) {
    const auto workspace = workspaceRef.lock();
    const auto contains = [surface](SP<CWLSurfaceResource> root) {
        return root && root->findFirstPreorder([surface](SP<CWLSurfaceResource> child) { return child.get() == surface; });
    };
    for (const auto& window : Desktop::windowState()->windows()) {
        if (window->m_workspace != workspace) continue;
        if (window->wlSurface() && contains(window->wlSurface()->resource())) return true;
        bool found = false;
        if (window->m_popupHead) window->m_popupHead->breadthfirst([&](SP<Desktop::View::CPopup> popup, void*) {
            if (!found && popup->wlSurface()) found = contains(popup->wlSurface()->resource());
        }, nullptr);
        if (found) return true;
    }
    const auto source = sourceRef.lock();
    if (!source) return false;
    for (const auto& level : source->m_layerSurfaceLayers) {
        for (const auto& layer : level) {
            if (!layer) continue;
            if (layer->wlSurface() && contains(layer->wlSurface()->resource())) return true;
            bool found = false;
            if (layer->m_popupHead) layer->m_popupHead->breadthfirst([&](SP<Desktop::View::CPopup> popup, void*) {
                if (!found && popup->wlSurface()) found = contains(popup->wlSurface()->resource());
            }, nullptr);
            if (found) return true;
        }
    }
    return false;
}

void commitSurface(CWLSurfaceResource* surface, SSurfaceState& state) {
    // Include empty parent commits: synchronized subsurface geometry can change
    // without replacing the parent's buffer. Test membership before unmapping.
    if (active && !capturing && !locked() && containsSurface(surface)) invalidate();
    reinterpret_cast<CommitSurface>(commitHook->m_original)(surface, state);
}

void damageWindow(void* renderer, PHLWINDOW window, bool full) {
    if (active && !capturing && window && window->m_workspace == workspaceRef.lock()) invalidate();
    reinterpret_cast<DamageWindow>(damageWindowHook->m_original)(renderer, window, full);
}

// Native layer geometry/alpha/fadeout updates have only generic damage entry
// points. Compare the shared layers themselves, not the damaged monitor/box:
// another workspace's windows or an ineligible physical cursor must not wake
// this capture. Retain storage so ordinary damage checks allocate nothing.
void observeLayers() {
    if (!active || capturing || locked()) return;
    const auto source = sourceRef.lock();
    if (!source) return;
    updateLayerFullscreenPolicy();
    size_t count = 0;
    bool changed = false;
    const auto sample = [&](const void* identity, const CBox& box, float alpha, bool mapped, uint32_t level, bool aboveFullscreen) {
        const LayerVisualState value{identity, box, alpha, mapped, level, aboveFullscreen};
        if (count == layerVisualState.size()) {
            layerVisualState.push_back(value);
            changed = true;
        } else if (!(layerVisualState[count] == value)) {
            layerVisualState[count] = value;
            changed = true;
        }
        ++count;
    };
    const auto surfaces = [&](SP<CWLSurfaceResource> root, uint32_t level) {
        if (!root) return;
        root->breadthfirst([&](SP<CWLSurfaceResource> child, const Vector2D& offset, void*) {
            sample(child.get(), CBox{offset, child->m_current.size}, 1.F, child->m_mapped, level, true);
        }, nullptr);
    };
    for (const auto& level : source->m_layerSurfaceLayers) {
        for (const auto& layerRef : level) {
            const auto layer = layerRef.lock();
            if (!layer) continue;
            const auto* policy = layerPolicy(layer);
            const bool aboveFullscreen = policy ? policy->aboveSelectedFullscreen : layer->m_aboveFullscreen;
            sample(layer.get(), layer->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                   selectedLayerAlpha(layer), layer->m_mapped, layer->m_layer,
                   aboveFullscreen);
            if (layer->wlSurface()) surfaces(layer->wlSurface()->resource(), layer->m_layer);
            if (layer->m_popupHead) layer->m_popupHead->breadthfirst([&](SP<Desktop::View::CPopup> popup, void*) {
                sample(popup.get(), popup->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                       popup->alpha().getTotal(), popup->m_mapped, layer->m_layer,
                       aboveFullscreen);
                if (popup->wlSurface()) surfaces(popup->wlSurface()->resource(), layer->m_layer);
            }, nullptr);
        }
    }
    // Keep shared layer and eligible popup snapshot animations live, without
    // attributing workspace-less physical-window snapshots to shared layers.
    for (const auto& fadeout : Desktop::fadingOutState()->fadeouts()) {
        if (!fadeout || fadeout->monitor() != source ||
            (!dynamic_cast<Desktop::CLayerFadeout*>(fadeout.get()) && !ownsPopupFadeout(fadeout.get()))) continue;
        sample(fadeout.get(), fadeout->renderBox(), fadeout->alpha(), !fadeout->done(), fadeout->plane(), true);
    }
    if (count != layerVisualState.size()) {
        layerVisualState.resize(count);
        changed = true;
    }
    if (changed) invalidate();
}

void damageBox(void* renderer, const CBox& box, bool skipFrameSchedule) {
    reinterpret_cast<DamageBox>(damageBoxHook->m_original)(renderer, box, skipFrameSchedule);
    const auto source = sourceRef.lock();
    if (source && box.overlaps(CBox{source->m_position, source->m_size})) observeLayers();
}
void damageMonitor(void* renderer, PHLMONITOR monitor) {
    reinterpret_cast<DamageMonitor>(damageMonitorHook->m_original)(renderer, monitor);
    if (monitor == sourceRef.lock()) observeLayers();
}

void stopMirror() {
    active = ready = dirty = waiting = false;
    sourceCursor.reset();
    workspaceRef.reset(); sourceRef.reset();
    if (timer) timer->updateTimeout(std::nullopt);
    GLState state;
    capturePass.clear();
    savedRenderData = {};
    captureCMSettings.clear();
    framebuffer.reset();
    captureResources.reset(); resourceSource.reset(); resourceDescription.reset();
    resourceSize = {}; resourceFormat = 0;
    layerVisualState.clear();
    layerFullscreenPolicy.clear(); layerPolicyWorkspace.reset(); layerPhysicalWorkspace.reset(); layerSelectedFullscreen.reset();
    gpuSource.stop();
    sourceName.clear();
    captureWidth = captureHeight = 0;
    lastPublishedPts = 0;
}

void sourceWake() {
    if (!active || !timer) return;
    if (!lastPublishedPts && gpuSource.connected()) { dirty = true; waiting = true; }
    if (gpuSource.failed()) timer->updateTimeout(std::chrono::nanoseconds{1});
    else if (waiting && dirty) {
        const auto interval = std::chrono::nanoseconds{1'000'000'000 / frameRate};
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Time::steadyNow() - lastCapture);
        timer->updateTimeout(std::max(std::chrono::nanoseconds{1}, interval - elapsed));
    }
}
bool updateClients(bool callbacks) {
    const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
    const bool inactive = callbacks && source->m_activeWorkspace != workspace;
    const auto now = Time::steadyNow();
    bool released = false;
    const auto frame = [&now, &released](const SP<CWLSurfaceResource>& surface) {
        if (surface) surface->breadthfirst([&now, &released](SP<CWLSurfaceResource> child, const Vector2D&, void*) {
            if (!child->m_current.callbacks.empty()) { released = true; child->frame(now); }
        }, nullptr);
    };
    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->m_isMapped || window->isHidden() || window->m_workspace != workspace) continue;
        if (inactive && window->wlSurface()) frame(window->wlSurface()->resource());
        if (window->m_popupHead) window->m_popupHead->breadthfirst([&](SP<Desktop::View::CPopup> popup, void*) {
            const auto& alpha = popup->alpha().get(Desktop::View::POPUP_ALPHA_FADE);
            if (alpha && alpha->isBeingAnimated()) dirty = true;
            if (inactive && popup->m_mapped && popup->wlSurface()) frame(popup->wlSurface()->resource());
        }, nullptr);
    }
    if (callbacks) {
        // These are shared source-monitor surfaces, but physical fullscreen
        // suppression can prevent their native passes from delivering frames.
        // Release only pending callbacks for layers rendered by this capture,
        // on viewer demand. Only resulting commits invalidate GPU pixels.
        for (const auto& level : source->m_layerSurfaceLayers) {
            for (const auto& ref : level) {
                const auto layer = ref.lock();
                if (!layer || !layer->visible() || selectedLayerAlpha(layer) <= 0.F) continue;
                if (layerSelectedFullscreen && layer->m_layer < ZWLR_LAYER_SHELL_V1_LAYER_TOP) continue;
                if (layer->wlSurface()) frame(layer->wlSurface()->resource());
                if (layer->m_popupHead) layer->m_popupHead->breadthfirst([&](SP<Desktop::View::CPopup> popup, void*) {
                    if (popup->m_mapped && popup->wlSurface()) frame(popup->wlSurface()->resource());
                }, nullptr);
            }
        }
    }
    return released;
}

void tick(SP<CEventLoopTimer> self, void*) {
    if (!active) return;
    try {
        if (!validSource()) { stopMirror(); return; }
        if (!waiting) return;
        if (!locked()) updateClients(false);
        observeLayers();
        if (dirty) {
            if (!capture()) { self->updateTimeout(std::nullopt); return; }
            dirty = waiting = false;
            lastCapture = Time::steadyNow();
        }
        // Keep demand pending; real commits, animation damage or slot releases
        // wake it. An unchanged desktop needs no recurring capture timer.
        self->updateTimeout(std::nullopt);
    } catch (...) { stopMirror(); }
}

void requestCapture() {
    if (!active || locked()) return;
    try {
        if (!validSource()) { stopMirror(); return; }
        // Catch lifecycle/tree changes applied after native pre-damage, even
        // when no animation or subsequent physical frame is scheduled.
        observeLayers();
        waiting = true;
        const auto interval = std::chrono::nanoseconds{1'000'000'000 / frameRate};
        if (updateClients(true)) {
            // Let a throttled client commit its newest pixels, rather than
            // sending a buffer one slow-viewer interval behind. A commit wakes
            // capture immediately; one frame interval bounds a silent client.
            timer->updateTimeout(interval);
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Time::steadyNow() - lastCapture);
        if (dirty && elapsed >= interval) tick(timer, nullptr);
        else if (dirty) timer->updateTimeout(std::max(std::chrono::nanoseconds{1}, interval - elapsed));
        else timer->updateTimeout(std::nullopt);
    } catch (...) { stopMirror(); }
}

std::string mirrorStatus() {
    const auto workspace = workspaceRef.lock();
    const bool healthy = active && validSource();
    std::string status;
    status.reserve(256);
    const auto number = [&status](auto value) {
        char digits[24];
        const auto formatted = std::to_chars(digits, digits + sizeof(digits), value);
        status.append(digits, formatted.ptr);
    };
    status += healthy ? "{\"active\":true,\"workspace\":" : "{\"active\":false,\"workspace\":";
    if (healthy) number(workspace->m_id); else status += "null";
    status += ",\"source\":";
    if (healthy) { status += '"'; status += sourceName; status += '"'; } else status += "null";
    status += ",\"width\":"; number(captureWidth);
    status += ",\"height\":"; number(captureHeight);
    status += ",\"minimumPtsUs\":"; number(dirty || !lastPublishedPts ? monotonicUs() : lastPublishedPts);
    status += locked() ? ",\"locked\":true}" : ",\"locked\":false}";
    return status;
}

SDispatchResult mirror(WORKSPACEID id) {
    if (sourceName.empty() || id < 1)
        return {.success = false, .error = "start a GPU source before selecting a positive workspace"};
    PHLWORKSPACE workspace;
    for (const auto& ref : State::workspaceState()->workspaces()) if (ref->m_id == id) workspace = ref.lock();
    const auto source = workspace ? workspace->m_monitor.lock() : PHLMONITOR{};
    if (!source || !valid(workspace) || !source->enabled() || source->isMirror() ||
        ownedName(source->m_name) || workspace->m_isSpecialWorkspace)
        return {.success = false, .error = "workspace must exist on its original non-TV monitor"};
    ready = false;
    sourceRef = source; workspaceRef = workspace;
    active = true;
    sourceCursor.setSource(source, workspace);
    dirty = waiting = true;
    observeLayers();
    sourceWake();
    return {};
}

int luaMirror(lua_State* state) {
    if (lua_gettop(state) != 1 || !lua_isinteger(state, 1)) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "expected a workspace integer");
        return 2;
    }
    const auto result = mirror(lua_tointeger(state, 1));
    lua_pushboolean(state, result.success);
    lua_pushlstring(state, result.error.data(), result.error.size());
    return 2;
}

int luaStart(lua_State* state) {
    if (lua_gettop(state) != 6 || lua_type(state, 1) != LUA_TSTRING ||
        lua_type(state, 2) != LUA_TSTRING || !lua_isinteger(state, 3) ||
        !lua_isinteger(state, 4) || !lua_isinteger(state, 5) || !lua_isinteger(state, 6)) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "expected source name, socket path, workspace, width, height and frame rate");
        return 2;
    }
    const std::string_view name{lua_tostring(state, 1)};
    constexpr std::string_view prefix = "OMARCHY-MIRROR-";
    const auto width = lua_tointeger(state, 4), height = lua_tointeger(state, 5), rate = lua_tointeger(state, 6);
    if (!sourceName.empty() || !name.starts_with(prefix) || name.size() != prefix.size() + 32 ||
        !std::ranges::all_of(name.substr(prefix.size()), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        width < 320 || height < 200 || width > 4096 || height > 4096 ||
        width * height > 3840 * 2160 || rate < 1 || rate > 60) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "invalid or already active GPU source");
        return 2;
    }
    try {
        sourceName = name;
        captureWidth = width; captureHeight = height; frameRate = rate;
        gpuSource.start(lua_tostring(state, 2), captureWidth, captureHeight, sourceWake);
        const auto result = mirror(lua_tointeger(state, 3));
        if (!result.success) throw std::runtime_error(result.error);
        lua_pushboolean(state, true);
        return 1;
    } catch (const std::exception& error) {
        stopMirror();
        lua_pushboolean(state, false);
        lua_pushstring(state, error.what());
        return 2;
    }
}

int luaStop(lua_State* state) {
    if (lua_gettop(state)) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "stop takes no arguments");
        return 2;
    }
    stopMirror();
    lua_pushboolean(state, true);
    return 1;
}

CFunctionHook* hook(const char* name, void* replacement) {
    auto matches = HyprlandAPI::findFunctionsByName(handle, name);
    std::erase_if(matches, [name](const auto& match) { return match.signature != name; });
    if (matches.size() != 1) throw std::runtime_error(std::string{"missing or ambiguous pinned render symbol: "} + name);
    auto* result = HyprlandAPI::createFunctionHook(handle, matches[0].address, replacement);
    if (!result || !result->hook()) throw std::runtime_error(std::string{"cannot hook "} + name);
    return result;
}
void cleanup() {
    if (timer) { timer->cancel(); g_pEventLoopManager->removeTimer(timer); timer.reset(); }
    lockListener.reset(); unlockListener.reset(); removedListener.reset(); movedListener.reset(); layoutListener.reset();
    layerOpenedListener.reset(); layerClosedListener.reset(); layerRulesListener.reset();
    stopMirror();
    sourceCursor.shutdown();
    capturePass.clear();
    popupFadeoutOwners.clear(); filteredPopupFadeouts.clear();
    for (auto** owned : {&cursorMoveHook, &commitHook, &layerRenderHook, &fullscreenOverrideHook, &fullscreenFadeHook, &renderFadeoutsHook, &popupFadeoutHook, &damageMonitorHook, &damageBoxHook, &damageWindowHook, &resourcesHook, &visibleHook, &windowHook}) {
        if (*owned) { HyprlandAPI::removeFunctionHook(handle, *owned); *owned = nullptr; }
    }
    for (auto* command : {&frameCommand, &statusCommand}) {
        if (*command) { HyprlandAPI::unregisterHyprCtlCommand(handle, *command); command->reset(); }
    }
    HyprlandAPI::removeLuaFunction(handle, "omarchy_display", "mirror");
    HyprlandAPI::removeLuaFunction(handle, "omarchy_display", "stop");
    HyprlandAPI::removeLuaFunction(handle, "omarchy_display", "start");
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE pluginHandle) {
    handle = pluginHandle;
    if (std::string_view{__hyprland_api_get_hash()} != std::string_view{__hyprland_api_get_client_hash()} || HyprlandAPI::getHyprlandVersion(handle).hash != COMMIT)
        throw std::runtime_error("omarchy workspace mirror requires exact Hyprland 0.56.2 commit and matching API headers");
    if (g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL)
        throw std::runtime_error("omarchy workspace mirror requires Hyprland's OpenGL renderer");
    externalTextures = Render::GL::g_pHyprOpenGL->m_extensions.contains("GL_OES_EGL_image_external");
    try {
        windowHook = hook("_ZN6Render13IHyprRenderer18shouldRenderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS3_IN7Monitor8CMonitorEEE", reinterpret_cast<void*>(shouldRenderWindow));
        visibleHook = hook("_ZN6Render13IHyprRenderer18shouldRenderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEE", reinterpret_cast<void*>(shouldRefreshWindow));
        damageWindowHook = hook("_ZN6Render13IHyprRenderer12damageWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEEb", reinterpret_cast<void*>(damageWindow));
        commitHook = hook("_ZN18CWLSurfaceResource11commitStateER13SSurfaceState", reinterpret_cast<void*>(commitSurface));
        resourcesHook = hook("_ZN7Monitor8CMonitor9resourcesEv", reinterpret_cast<void*>(monitorResources));
        damageBoxHook = hook("_ZN6Render13IHyprRenderer9damageBoxERKN9Hyprutils4Math4CBoxEb", reinterpret_cast<void*>(damageBox));
        damageMonitorHook = hook("_ZN6Render13IHyprRenderer13damageMonitorEN9Hyprutils6Memory14CSharedPointerIN7Monitor8CMonitorEEE", reinterpret_cast<void*>(damageMonitor));
        popupFadeoutHook = hook("_ZN7Desktop13CPopupFadeout6createEN9Hyprutils6Memory14CSharedPointerINS_4View6CPopupEEENS3_IN6Render12IFramebufferEEEf", reinterpret_cast<void*>(createPopupFadeout));
        renderFadeoutsHook = hook("_ZN6Render13IHyprRenderer14renderFadeoutsEN9Hyprutils6Memory14CSharedPointerIN7Monitor8CMonitorEEEN7Desktop13eFadeoutPlaneENS3_I10CWorkspaceEE", reinterpret_cast<void*>(renderFadeouts));
        layerRenderHook = hook("_ZN6Render13IHyprRenderer11renderLayerEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View13CLayerSurfaceEEENS3_IN7Monitor8CMonitorEEERKNSt6chrono10time_pointINSB_3_V212steady_clockENSB_8durationIlSt5ratioILl1ELl1000000000EEEEEEbb", reinterpret_cast<void*>(renderLayer));
        fullscreenFadeHook = hook("_ZN9Animation9Workspace26setFullscreenFadeAnimationEN9Hyprutils6Memory14CSharedPointerI10CWorkspaceEENS0_14eAnimationTypeE", reinterpret_cast<void*>(fullscreenFade));
        fullscreenOverrideHook = hook("_ZN9Animation9Workspace28overrideFullscreenFadeAmountEN9Hyprutils6Memory14CSharedPointerI10CWorkspaceEEfNS3_IN7Desktop4View7CWindowEEE", reinterpret_cast<void*>(fullscreenOverride));
        cursorMoveHook = hook("_ZN7Pointer15CPointerManager13onCursorMovedEv", reinterpret_cast<void*>(cursorMoved));
        timer = makeShared<CEventLoopTimer>(std::nullopt, tick, nullptr);
        g_pEventLoopManager->addTimer(timer);
        sourceCursor.initialize(invalidate, [] { return capturing || !active || locked(); });
        lockListener = g_pSessionLockManager->m_events.lock.listen([] {
            if (active) { ready = false; dirty = waiting = true; timer->updateTimeout(std::chrono::nanoseconds{1}); }
        });
        unlockListener = g_pSessionLockManager->m_events.unlock.listen([] {
            if (active) { ready = false; dirty = waiting = true; timer->updateTimeout(std::chrono::nanoseconds{1}); }
        });
        removedListener = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR monitor) {
            if (monitor == sourceRef.lock()) stopMirror();
        });
        movedListener = Event::bus()->m_events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE) { invalidate(); });
        layoutListener = Event::bus()->m_events.monitor.layoutChanged.listen([] { invalidate(); });
        const auto layerChanged = [](PHLLS layer) {
            if (layer && layer->m_monitor == sourceRef) invalidate();
        };
        layerOpenedListener = Event::bus()->m_events.layer.opened.listen([layerChanged](PHLLS layer) {
            if (layer && layer->m_monitor == sourceRef) {
                updateLayerFullscreenPolicy();
                if (auto* policy = layerPolicy(layer)) {
                    policy->nativeFullscreenAlpha = false;
                    policy->normalAlpha = layer->alpha()[Desktop::View::LS_ALPHA_FADE]->goal();
                    policy->aboveSelectedFullscreen = true; // native onMap policy
                }
            }
            layerChanged(layer);
        });
        layerClosedListener = Event::bus()->m_events.layer.closed.listen(layerChanged);
        layerRulesListener = Event::bus()->m_events.layer.updateRules.listen(layerChanged);
        if (!HyprlandAPI::addLuaFunction(handle, "omarchy_display", "start", luaStart) ||
            !HyprlandAPI::addLuaFunction(handle, "omarchy_display", "mirror", luaMirror) ||
            !HyprlandAPI::addLuaFunction(handle, "omarchy_display", "stop", luaStop))
            throw std::runtime_error("cannot register mirror Lua functions");
        statusCommand = HyprlandAPI::registerHyprCtlCommand(handle, {"omarchy-display-mirror", true, [](eHyprCtlOutputFormat, std::string) {
            return mirrorStatus();
        }});
        frameCommand = HyprlandAPI::registerHyprCtlCommand(handle, {"omarchy-display-frame", true, [](eHyprCtlOutputFormat, std::string) {
            requestCapture();
            return mirrorStatus();
        }});
        if (!statusCommand || !frameCommand) throw std::runtime_error("cannot register mirror status/demand");
    } catch (...) { cleanup(); throw; }
    return {"omarchy-workspace-mirror", "Exact-version offscreen GPU workspace mirror without a desktop output", "omarchy-display", "1.3.0"};
}
APICALL EXPORT void PLUGIN_EXIT() { cleanup(); }
