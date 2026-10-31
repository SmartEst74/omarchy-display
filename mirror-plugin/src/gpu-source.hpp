#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <hyprland/src/render/Framebuffer.hpp>

struct wl_event_source;

namespace OmarchyDisplay {

// All methods and wake callbacks run on the compositor event-loop thread.
// acquire/publish/stop (and destruction after use) require the parent's GL-state
// guard and the Hyprland EGL context. Stop before unloading the plugin.
class MirrorGpuSource {
  public:
    MirrorGpuSource() = default;
    ~MirrorGpuSource();
    MirrorGpuSource(const MirrorGpuSource&) = delete;
    MirrorGpuSource& operator=(const MirrorGpuSource&) = delete;

    void start(std::string_view path, uint32_t width, uint32_t height, void (*wake)());
    void stop();
    bool connected() const;
    bool failed() const;
    int acquire();
    const SP<Render::IFramebuffer>& target(int slot) const;
    void publish(int slot, uint64_t ptsUs);
    void cancel(int slot);

  private:
    enum class SlotState { FREE, ACQUIRED, LEASED };
    struct Slot {
        SP<Render::IFramebuffer> framebuffer;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        std::array<int, 4> fds{-1, -1, -1, -1};
        std::array<uint32_t, 4> strides{}, offsets{};
        uint32_t format = 0, planes = 0;
        uint64_t modifier = 0, sequence = 0;
        SlotState state = SlotState::FREE;
    };

    static int acceptReady(int fd, uint32_t mask, void* data) noexcept;
    static int peerReady(int fd, uint32_t mask, void* data) noexcept;
    void acceptPeer(uint32_t mask);
    void receiveReleases(uint32_t mask);
    void allocate(Slot& slot);
    void retire(Slot& slot);
    void closeTransport() noexcept;
    void failStream() noexcept;
    void notify() noexcept;
    Slot& checkedSlot(int slot);

    std::array<Slot, 3> m_slots;
    int m_listener = -1, m_peer = -1, m_renderFD = -1, m_directoryFD = -1;
    wl_event_source* m_listenerEvent = nullptr;
    wl_event_source* m_peerEvent = nullptr;
    std::string m_socketName;
    bool m_bound = false, m_started = false, m_failed = false;
    uint32_t m_width = 0, m_height = 0;
    uint64_t m_sequence = 0;
    void (*m_wake)() = nullptr;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC m_createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_destroyImage = nullptr;
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC m_queryImage = nullptr;
    PFNEGLEXPORTDMABUFIMAGEMESAPROC m_exportImage = nullptr;
    PFNEGLCREATESYNCKHRPROC m_createSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC m_destroySync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC m_dupFence = nullptr;
};

}
