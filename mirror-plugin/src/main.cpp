// Capture/render-hook approach adapted from hyprexpo (BSD-3-Clause); see NOTICE.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
extern "C" {
#include <lua.h>
}
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
// This exact-version plugin needs renderWorkspace and the render-mode guard.
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#undef protected
#undef private

namespace {
template <typename Fn> struct ScopeExit {
    Fn fn;
    ~ScopeExit() { fn(); }
};
constexpr std::string_view COMMIT = "efb50993780079460b0cbed1363e2166a2de1d9f";
static_assert(std::string_view{GIT_COMMIT_HASH} == COMMIT, "Rebuild only against pinned Hyprland 0.56.2 headers");
HANDLE handle = nullptr;
CFunctionHook *workspaceHook = nullptr, *windowHook = nullptr, *visibleHook = nullptr;
using RenderWorkspace = void (*)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
using RenderWindow = bool (*)(void*, PHLWINDOW, PHLMONITOR);
using VisibleWindow = bool (*)(void*, PHLWINDOW);
SP<SHyprCtlCommand> statusCommand;
int frameRate = 30;
SP<CEventLoopTimer> timer;
CHyprSignalListener lockListener, unlockListener, removedListener;
PHLMONITORREF sinkRef, sourceRef;
PHLWORKSPACEREF workspaceRef;
SP<Render::IFramebuffer> framebuffer;
Render::CRenderPass capturePass;
Render::SRenderData savedRenderData;
bool active = false, ready = false, capturing = false;
bool externalTextures = false;

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
    const auto sink = sinkRef.lock(); const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
    return sink && source && valid(workspace) && sink != source && sink->enabled() && source->enabled() && sink->m_output && source->m_output &&
        sink->m_createdByUser && ownedName(sink->m_name) && !sink->isMirror() && !source->isMirror() && !workspace->m_isSpecialWorkspace &&
        workspace->m_id > 0 && workspace->m_monitor == source && !ownedName(source->m_name);
}

bool prepareFramebuffer(const PHLMONITOR& source) {
    if (!framebuffer) framebuffer = g_pHyprRenderer->createFB("omarchy live workspace mirror");
    const auto size = source->m_transformedSize;
    const auto format = source->useFP16() ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR8888;
    if (size.x < 1 || size.y < 1 || !framebuffer->alloc(size.x, size.y, format)) return false;
    framebuffer->setImageDescription(source->workBufferImageDescription());
    return true;
}

bool capture() {
    ready = false;
    if (!active || locked() || !validSource()) return false;
    const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
    GLState glState;
    if (!prepareFramebuffer(source)) return false;
    const auto oldTransform = source->m_transform;
    const auto oldPixels = source->m_pixelSize;
    const auto oldTransformed = source->m_transformedSize;
    const auto oldActive = source->m_activeWorkspace;
    const auto oldSpecial = source->m_activeSpecialWorkspace;
    const bool wasVisible = workspace->m_visible, wasForced = workspace->m_forceRendering;
    const bool oldFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
    const auto oldMode = g_pHyprRenderer->m_renderMode;
    bool begun = false;
    capturing = true;
    std::swap(savedRenderData, g_pHyprRenderer->m_renderData);
    std::swap(capturePass, g_pHyprRenderer->m_renderPass);
    ScopeExit restore{[&] {
        if (begun) { try { g_pHyprRenderer->endRender(); } catch (...) { g_pHyprRenderer->m_renderPass.clear(); } }
        std::swap(capturePass, g_pHyprRenderer->m_renderPass);
        std::swap(savedRenderData, g_pHyprRenderer->m_renderData);
        g_pHyprRenderer->m_renderMode = oldMode;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = oldFeedback;
        source->m_transform = oldTransform; source->m_pixelSize = oldPixels; source->m_transformedSize = oldTransformed;
        source->m_activeWorkspace = oldActive; source->m_activeSpecialWorkspace = oldSpecial;
        workspace->m_visible = wasVisible; workspace->m_forceRendering = wasForced;
        capturing = false;
    }};
    // Normalize only the offscreen projection. Logical layout, scale, window
    // geometry and original workspace ownership are never changed.
    source->m_transform = WL_OUTPUT_TRANSFORM_NORMAL;
    source->m_pixelSize = source->m_transformedSize;
    source->m_activeWorkspace = workspace; source->m_activeSpecialWorkspace.reset();
    workspace->m_visible = true; workspace->m_forceRendering = true;
    CurrentValue<float> alpha{workspace->m_alpha, 1.F};
    CurrentValue<Vector2D> offset{workspace->m_renderOffset, Vector2D{}};
    CurrentValue<float> specialDim{source->m_specialDim, 0.F}, specialBlur{source->m_specialBlur, 0.F};
    CRegion damage{0., 0., framebuffer->m_size.x, framebuffer->m_size.y};
    if (!g_pHyprRenderer->beginRender(source, damage, Render::RENDER_MODE_FULL_FAKE, nullptr, framebuffer)) return false;
    begun = true;
    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
    g_pHyprRenderer->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0.F, 0.F, 0.F, 1.F}});
    g_pHyprRenderer->startRenderPass();
    reinterpret_cast<RenderWorkspace>(workspaceHook->m_original)(g_pHyprRenderer.get(), source, workspace, Time::steadyNow(), CBox{{}, framebuffer->m_size});
    g_pHyprRenderer->endRender(); begun = false;
    // No stale unlocked frame may ever be presented after a lock event.
    ready = !locked();
    return ready;
}

// Hyprland owns and deletes pass elements. One fixed slot suffices: capture is
// outside monitor rendering, and only one sink can own a mirror pass at a time.
class MirrorPass final : public IPassElement {
  public:
    static void* operator new(size_t);
    static void operator delete(void*) noexcept;
    std::vector<UP<IPassElement>> draw() override {
        const auto sink = sinkRef.lock();
        if (!sink) return {};
        // This is part of the native pass: keep its GL state/cache updates for
        // frame finalization. Only the separate offscreen capture restores GL.
        auto& gl = Render::GL::g_pHyprOpenGL;
        // Intermediate composition must write both display and capture MRT
        // attachments; the final-output passthrough shader only writes one.
        gl->m_applyFinalShader = false;
        const CBox whole{{}, sink->m_transformedSize};
        auto& renderData = g_pHyprRenderer->m_renderData;
        const auto previousClip = renderData.clipBox;
        ScopeExit restoreClip{[&] { renderData.clipBox = previousClip; }};
        renderData.clipBox = {};
        // Clear all enabled color attachments, including the capture mirror.
        // A custom pass cannot inherit a window's stencil or color write mask.
        gl->setCapStatus(Render::GL::CHyprOpenGLImpl::CAP_STATUS_STENCIL_TEST, false);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        gl->setCapStatus(Render::GL::CHyprOpenGLImpl::CAP_STATUS_SCISSOR_TEST, false);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        gl->blend(false);
        glClearColor(0.F, 0.F, 0.F, 1.F);
        glClear(GL_COLOR_BUFFER_BIT);
        if (active && ready && !locked() && validSource() && framebuffer && framebuffer->getTexture()) {
            const auto size = framebuffer->m_size;
            const double scale = std::min(whole.w / size.x, whole.h / size.y);
            const CBox box{(whole.w - size.x * scale) / 2, (whole.h - size.y * scale) / 2, size.x * scale, size.y * scale};
            // GPU sampling directly from the capture FBO: no staging texture,
            // readPixels, JPEG conversion or extra pixel copy.
            Render::GL::CHyprOpenGLImpl::STextureRenderData textureData{};
            textureData.a = 1.F;
            textureData.allowDim = false;
            gl->renderTexture(framebuffer->getTexture(), box, textureData);
        }
        return {};
    }
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "COmarchyWorkspaceMirrorPass"; }
    ePassElementType type() override { return EK_CUSTOM; }
    bool disableSimplification() override { return true; }
    bool undiscardable() override { return true; }
};
alignas(MirrorPass) std::byte passStorage[sizeof(MirrorPass)];
bool passInUse = false;
void* MirrorPass::operator new(size_t) {
    if (passInUse) throw std::runtime_error("recursive mirror render");
    passInUse = true; return passStorage;
}
void MirrorPass::operator delete(void*) noexcept { passInUse = false; }

void renderWorkspace(void* renderer, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now, const CBox& box) {
    if (!capturing && monitor == sinkRef.lock()) {
        g_pHyprRenderer->m_renderPass.add(makeUnique<MirrorPass>());
        return;
    }
    reinterpret_cast<RenderWorkspace>(workspaceHook->m_original)(renderer, monitor, workspace, now, box);
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

void damageSink() {
    if (const auto sink = sinkRef.lock(); sink && sink->enabled()) {
        sink->m_solitaryClient.reset(); // the owned parking output must use the render hook, never direct scanout
        g_pHyprRenderer->damageMonitor(sink);
        sink->scheduleFrame();
    }
}
void clearMirror(bool synchronous) {
    active = ready = false;
    workspaceRef.reset(); sourceRef.reset();
    if (timer) timer->updateTimeout(std::nullopt);
    damageSink();
    if (synchronous) {
        if (const auto sink = sinkRef.lock(); sink && sink->enabled()) g_pHyprRenderer->renderMonitor(sink);
    }
    if (framebuffer) {
        GLState state;
        capturePass.clear();
        savedRenderData = {};
        framebuffer.reset();
    }
}
void tick(SP<CEventLoopTimer> self, void*) {
    if (!active) return;
    try {
        if (!validSource()) { clearMirror(false); return; }
        if (locked()) ready = false;
        else if (!capture()) { clearMirror(false); return; }
        damageSink();
        if (!locked()) {
            const auto source = sourceRef.lock(); const auto workspace = workspaceRef.lock();
            // Normal monitor rendering services visible clients. Inactive clients
            // receive bounded callbacks for every root/subsurface and popup.
            if (source->m_activeWorkspace != workspace) {
                const auto now = Time::steadyNow();
                const auto frame = [&now](const SP<CWLSurfaceResource>& surface) {
                    if (surface) surface->breadthfirst([&now](SP<CWLSurfaceResource> child, const Vector2D&, void*) { child->frame(now); }, nullptr);
                };
                for (const auto& window : Desktop::windowState()->windows()) {
                    if (!window->m_isMapped || window->isHidden() || window->m_workspace != workspace) continue;
                    if (window->wlSurface()) frame(window->wlSurface()->resource());
                    if (window->m_popupHead) window->m_popupHead->breadthfirst([&frame](SP<Desktop::View::CPopup> popup, void*) {
                        if (popup->m_mapped && popup->wlSurface()) frame(popup->wlSurface()->resource());
                    }, nullptr);
                }
            }
        }
        self->updateTimeout(std::chrono::nanoseconds{1'000'000'000 / frameRate});
    } catch (...) { clearMirror(false); }
}

SDispatchResult mirror(std::string_view name, WORKSPACEID id, int rate) {
    if (!ownedName(name) || id < 1 || rate < 1 || rate > 60)
        return {.success = false, .error = "mirror requires an owned output, positive workspace and frame rate 1–60"};
    PHLMONITOR sink;
    PHLWORKSPACE workspace;
    for (const auto& monitor : State::monitorState()->monitors()) if (monitor->m_name == name) sink = monitor;
    for (const auto& ref : State::workspaceState()->workspaces()) if (ref->m_id == id) workspace = ref.lock();
    const auto source = workspace ? workspace->m_monitor.lock() : PHLMONITOR{};
    if (!sink || !source || !valid(workspace) || !sink->m_createdByUser || !sink->enabled() || !source->enabled() || sink == source ||
        sink->isMirror() || source->isMirror() || ownedName(source->m_name) || workspace->m_isSpecialWorkspace)
        return {.success = false, .error = "workspace must exist on its original, non-TV monitor; sink must be an enabled user-created output"};
    if (const auto old = sinkRef.lock(); old && old != sink)
        return {.success = false, .error = "stop and remove the previous owned mirror output before changing outputs"};
    try {
        clearMirror(false);
        sinkRef = sink; sourceRef = source; workspaceRef = workspace;
        frameRate = rate;
        active = true;
        if (!locked() && !capture()) { clearMirror(true); return {.success = false, .error = "offscreen workspace rendering failed"}; }
        damageSink();
        timer->updateTimeout(std::chrono::nanoseconds{1'000'000'000 / frameRate});
    } catch (...) {
        clearMirror(true);
        return {.success = false, .error = "offscreen workspace rendering failed"};
    }
    return {};
}

int luaMirror(lua_State* state) {
    if (lua_gettop(state) != 3 || lua_type(state, 1) != LUA_TSTRING ||
        !lua_isinteger(state, 2) || !lua_isinteger(state, 3)) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "expected output string, workspace integer and frame-rate integer");
        return 2;
    }
    size_t size = 0;
    const auto* name = lua_tolstring(state, 1, &size);
    const auto rate = lua_tointeger(state, 3);
    if (rate < 1 || rate > 60) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "mirror frame rate must be 1–60");
        return 2;
    }
    const auto result = mirror(std::string_view{name, size}, lua_tointeger(state, 2), static_cast<int>(rate));
    lua_pushboolean(state, result.success);
    lua_pushlstring(state, result.error.data(), result.error.size());
    return 2;
}

int luaStop(lua_State* state) {
    if (lua_gettop(state)) {
        lua_pushboolean(state, false);
        lua_pushliteral(state, "stop takes no arguments");
        return 2;
    }
    clearMirror(true);
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
    lockListener.reset(); unlockListener.reset(); removedListener.reset();
    clearMirror(true);
    g_pHyprRenderer->m_renderPass.removeAllOfType("COmarchyWorkspaceMirrorPass");
    capturePass.clear();
    sinkRef.reset();
    for (auto** owned : {&visibleHook, &windowHook, &workspaceHook}) {
        if (*owned) { HyprlandAPI::removeFunctionHook(handle, *owned); *owned = nullptr; }
    }
    if (statusCommand) { HyprlandAPI::unregisterHyprCtlCommand(handle, statusCommand); statusCommand.reset(); }
    HyprlandAPI::removeLuaFunction(handle, "omarchy_display", "mirror");
    HyprlandAPI::removeLuaFunction(handle, "omarchy_display", "stop");
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
        workspaceHook = hook("_ZN6Render13IHyprRenderer15renderWorkspaceEN9Hyprutils6Memory14CSharedPointerIN7Monitor8CMonitorEEENS3_I10CWorkspaceEERKNSt6chrono10time_pointINS9_3_V212steady_clockENS9_8durationIlSt5ratioILl1ELl1000000000EEEEEERKNS1_4Math4CBoxE", reinterpret_cast<void*>(renderWorkspace));
        windowHook = hook("_ZN6Render13IHyprRenderer18shouldRenderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEENS3_IN7Monitor8CMonitorEEE", reinterpret_cast<void*>(shouldRenderWindow));
        visibleHook = hook("_ZN6Render13IHyprRenderer18shouldRenderWindowEN9Hyprutils6Memory14CSharedPointerIN7Desktop4View7CWindowEEE", reinterpret_cast<void*>(shouldRefreshWindow));
        timer = makeShared<CEventLoopTimer>(std::nullopt, tick, nullptr);
        g_pEventLoopManager->addTimer(timer);
        lockListener = g_pSessionLockManager->m_events.lock.listen([] { ready = false; damageSink(); });
        unlockListener = g_pSessionLockManager->m_events.unlock.listen([] { ready = false; if (active) timer->updateTimeout(std::chrono::nanoseconds{1}); damageSink(); });
        removedListener = Event::bus()->m_events.monitor.removed.listen([](PHLMONITOR monitor) {
            if (monitor == sinkRef.lock() || monitor == sourceRef.lock()) { clearMirror(false); if (monitor == sinkRef.lock()) sinkRef.reset(); }
        });
        if (!HyprlandAPI::addLuaFunction(handle, "omarchy_display", "mirror", luaMirror) ||
            !HyprlandAPI::addLuaFunction(handle, "omarchy_display", "stop", luaStop))
            throw std::runtime_error("cannot register mirror Lua functions");
        statusCommand = HyprlandAPI::registerHyprCtlCommand(handle, {"omarchy-display-mirror", true, [](eHyprCtlOutputFormat, std::string) {
            const auto workspace = workspaceRef.lock(); const auto sink = sinkRef.lock();
            const bool healthy = active && validSource();
            return std::string{"{\"active\":"} + (healthy ? "true" : "false") + ",\"workspace\":" +
                (healthy ? std::to_string(workspace->m_id) : "null") + ",\"output\":" +
                (healthy ? "\"" + sink->m_name + "\"" : "null") + ",\"locked\":" + (locked() ? "true" : "false") + "}";
        }});
        if (!statusCommand) throw std::runtime_error("cannot register mirror status");
    } catch (...) { cleanup(); throw; }
    return {"omarchy-workspace-mirror", "Exact-version live workspace clone for an owned TV output", "omarchy-display", "0.1.0"};
}
APICALL EXPORT void PLUGIN_EXIT() { cleanup(); }
