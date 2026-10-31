#include "gpu-source.hpp"
#include "../../desktop/mirror-wire.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>
#include <wayland-server-core.h>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

namespace OmarchyDisplay {
namespace {
static_assert(OD_MIRROR_SLOTS == 3 && OD_MIRROR_MAX_PLANES == 4);

void closeFD(int& fd) noexcept {
    if (fd >= 0) close(fd);
    fd = -1;
}

struct OwnedFD {
    int fd;
    ~OwnedFD() { closeFD(fd); }
};

[[noreturn]] void systemFailure(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

bool extension(std::string_view list, std::string_view name) {
    while (!list.empty()) {
        const auto end = list.find(' ');
        if (list.substr(0, end) == name) return true;
        if (end == std::string_view::npos) break;
        list.remove_prefix(end + 1);
    }
    return false;
}

template <typename T> T eglProc(const char* name) {
    const auto proc = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!proc) throw std::runtime_error(std::string("Missing EGL procedure: ") + name);
    return proc;
}

od_mirror_message message(uint16_t type) {
    od_mirror_message msg{};
    msg.magic = OD_MIRROR_MAGIC;
    msg.version = OD_MIRROR_VERSION;
    msg.type = type;
    return msg;
}

void sendPacket(int socket, const od_mirror_message& packet, const int* fds, size_t count) {
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * (OD_MIRROR_MAX_PLANES + 1))> control{};
    iovec iov{const_cast<od_mirror_message*>(&packet), sizeof(packet)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (count) {
        msg.msg_control = control.data();
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * count);
        auto* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * count);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * count);
    }
    ssize_t sent;
    do { sent = sendmsg(socket, &msg, MSG_DONTWAIT | MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
    if (sent < 0) systemFailure("Sending GPU mirror packet");
    if (sent != static_cast<ssize_t>(sizeof(packet))) throw std::runtime_error("Short GPU mirror packet send");
}

bool rgb8888(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XBGR8888: case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBX8888: case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRX8888: case DRM_FORMAT_BGRA8888: return true;
        default: return false;
    }
}
}

MirrorGpuSource::~MirrorGpuSource() { stop(); }

void MirrorGpuSource::start(std::string_view path, uint32_t width, uint32_t height, void (*wake)()) {
    if (m_started) throw std::logic_error("Stop GPU mirror source before restarting");
    if (!wake || width == 0 || height == 0 || width > 3840 || height > 2160 ||
        path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos ||
        path.size() >= sizeof(sockaddr_un{}.sun_path))
        throw std::invalid_argument("Invalid GPU mirror source configuration");
    const auto slash = path.rfind('/');
    const std::string directory{path.substr(0, slash ? slash : 1)};
    const auto name = path.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") throw std::invalid_argument("Invalid GPU mirror socket name");
    m_started = true;
    m_failed = false;
    m_width = width;
    m_height = height;
    m_wake = wake;
    try {
        const auto& gl = Render::GL::g_pHyprOpenGL;
        if (!gl || !g_pCompositor || !g_pCompositor->m_wlEventLoop) throw std::runtime_error("GPU mirror renderer is unavailable");
        m_display = gl->m_eglDisplay;
        m_context = gl->m_eglContext;
        if (m_display == EGL_NO_DISPLAY || m_context == EGL_NO_CONTEXT) throw std::runtime_error("GPU mirror requires the compositor EGL context");
        const char* extensions = eglQueryString(m_display, EGL_EXTENSIONS);
        if (!extensions || !extension(extensions, "EGL_MESA_image_dma_buf_export") ||
            !extension(extensions, "EGL_KHR_gl_texture_2D_image") ||
            !extension(extensions, "EGL_ANDROID_native_fence_sync"))
            throw std::runtime_error("GPU mirror requires DMA-BUF texture export and native GPU fences");
        m_createImage = eglProc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
        m_destroyImage = eglProc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR");
        m_queryImage = eglProc<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>("eglExportDMABUFImageQueryMESA");
        m_exportImage = eglProc<PFNEGLEXPORTDMABUFIMAGEMESAPROC>("eglExportDMABUFImageMESA");
        m_createSync = eglProc<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
        m_destroySync = eglProc<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR");
        m_dupFence = eglProc<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>("eglDupNativeFenceFDANDROID");

        // Resolve the render node from this EGL display. Device-platform EGL
        // does not populate Hyprland's optional GBM allocation descriptor.
        const auto queryDisplay = eglProc<PFNEGLQUERYDISPLAYATTRIBEXTPROC>("eglQueryDisplayAttribEXT");
        const auto queryDevice = eglProc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
        EGLAttrib device = 0;
        if (!queryDisplay(m_display, EGL_DEVICE_EXT, &device) || !device)
            throw std::runtime_error("Cannot identify the GPU mirror EGL device");
        const char* renderName = queryDevice(reinterpret_cast<EGLDeviceEXT>(device), EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!renderName || renderName[0] != '/')
            throw std::runtime_error("GPU mirror EGL device has no DRM render node");
        m_renderFD = open(renderName, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        const int openError = errno;
        if (m_renderFD < 0) { errno = openError; systemFailure("Opening GPU mirror DRM render node"); }
        if (drmGetNodeTypeFromFd(m_renderFD) != DRM_NODE_RENDER) throw std::runtime_error("GPU mirror refuses non-render DRM descriptors");

        if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) systemFailure("Creating private GPU mirror directory");
        m_directoryFD = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (m_directoryFD < 0) systemFailure("Opening private GPU mirror directory");
        struct stat st{};
        if (fstat(m_directoryFD, &st) < 0) systemFailure("Inspecting private GPU mirror directory");
        if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 07777) != 0700)
            throw std::runtime_error("GPU mirror socket parent must be owned by this UID and mode 0700");
        m_socketName = name;
        m_listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (m_listener < 0) systemFailure("Creating GPU mirror socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.data(), path.size());
        if (bind(m_listener, reinterpret_cast<sockaddr*>(&address), offsetof(sockaddr_un, sun_path) + path.size() + 1) < 0)
            systemFailure("Binding GPU mirror socket");
        m_bound = true;
        if (fchmodat(m_directoryFD, m_socketName.c_str(), 0600, 0) < 0) systemFailure("Protecting GPU mirror socket");
        if (listen(m_listener, 1) < 0) systemFailure("Listening on GPU mirror socket");
        m_listenerEvent = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_listener, WL_EVENT_READABLE, acceptReady, this);
        if (!m_listenerEvent) throw std::runtime_error("Cannot watch GPU mirror listener");
    } catch (...) {
        stop();
        throw;
    }
}

void MirrorGpuSource::closeTransport() noexcept {
    if (m_listenerEvent) wl_event_source_remove(m_listenerEvent);
    if (m_peerEvent) wl_event_source_remove(m_peerEvent);
    m_listenerEvent = m_peerEvent = nullptr;
    closeFD(m_listener);
    closeFD(m_peer);
}

void MirrorGpuSource::stop() {
    m_wake = nullptr;
    closeTransport();
    for (auto& slot : m_slots) retire(slot);
    closeFD(m_renderFD);
    if (m_bound && m_directoryFD >= 0) unlinkat(m_directoryFD, m_socketName.c_str(), 0);
    m_bound = false;
    closeFD(m_directoryFD);
    m_socketName.clear();
    m_started = m_failed = false;
    m_width = m_height = 0;
    m_sequence = 0;
    m_display = EGL_NO_DISPLAY;
    m_context = EGL_NO_CONTEXT;
}

bool MirrorGpuSource::connected() const { return m_peer >= 0 && !m_failed; }
bool MirrorGpuSource::failed() const { return m_failed; }

void MirrorGpuSource::notify() noexcept {
    if (m_wake) {
        try { m_wake(); } catch (...) {
            // Never unwind through Wayland or leave a source whose capture
            // scheduler failed apparently connected.
            m_failed = true;
            closeTransport();
        }
    }
}

void MirrorGpuSource::failStream() noexcept {
    if (m_failed) return;
    m_failed = true;
    // Do not release or reuse any slots here: callbacks do not own a GL guard,
    // and native consumers may still hold imports of their immutable storage.
    closeTransport();
    notify();
}

int MirrorGpuSource::acceptReady(int, uint32_t mask, void* data) noexcept {
    auto* self = static_cast<MirrorGpuSource*>(data);
    try { self->acceptPeer(mask); } catch (...) { self->failStream(); }
    return 0;
}

void MirrorGpuSource::acceptPeer(uint32_t mask) {
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) throw std::runtime_error("GPU mirror listener failed");
    int peer;
    do { peer = accept4(m_listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC); } while (peer < 0 && errno == EINTR);
    if (peer < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        systemFailure("Accepting GPU mirror consumer");
    }
    m_peer = peer;
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0) systemFailure("Identifying GPU mirror consumer");
    if (length != sizeof(credentials) || credentials.uid != getuid()) throw std::runtime_error("GPU mirror consumer UID mismatch");
    // Exactly one connection per start. A failed or disconnected peer cannot
    // silently reconnect while an earlier process still owns buffer leases.
    wl_event_source_remove(m_listenerEvent);
    m_listenerEvent = nullptr;
    closeFD(m_listener);
    m_peerEvent = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, peer, WL_EVENT_READABLE, peerReady, this);
    if (!m_peerEvent) throw std::runtime_error("Cannot watch GPU mirror consumer");
    auto hello = message(OD_MIRROR_HELLO);
    hello.width = m_width;
    hello.height = m_height;
    sendPacket(peer, hello, &m_renderFD, 1);
    notify();
}

int MirrorGpuSource::peerReady(int, uint32_t mask, void* data) noexcept {
    auto* self = static_cast<MirrorGpuSource*>(data);
    try { self->receiveReleases(mask); } catch (...) { self->failStream(); }
    return 0;
}

void MirrorGpuSource::receiveReleases(uint32_t mask) {
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) throw std::runtime_error("GPU mirror consumer disconnected");
    // There can be at most three valid outstanding releases. Bound even hostile
    // traffic so the compositor's event loop cannot be starved by this socket.
    for (unsigned attempt = 0; attempt < OD_MIRROR_SLOTS + 1; ++attempt) {
        od_mirror_message packet{};
        iovec iov{&packet, sizeof(packet)};
        // Linux SCM_MAX_FD is 253. Receive and close all unexpected descriptors,
        // including the installed subset of a MSG_CTRUNC packet.
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 253)> control{};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();
        ssize_t count;
        do { count = recvmsg(m_peer, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC); } while (count < 0 && errno == EINTR);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            systemFailure("Receiving GPU mirror release");
        }
        bool ancillary = false;
        for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            ancillary = true;
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && cmsg->cmsg_len >= CMSG_LEN(0)) {
                const auto bytes = cmsg->cmsg_len - CMSG_LEN(0);
                for (size_t i = 0; i + sizeof(int) <= bytes; i += sizeof(int)) {
                    int fd;
                    std::memcpy(&fd, CMSG_DATA(cmsg) + i, sizeof(fd));
                    closeFD(fd);
                }
            }
        }
        if (count != sizeof(packet) || ancillary || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
            throw std::runtime_error("Malformed GPU mirror release packet");
        auto expected = message(OD_MIRROR_RELEASE);
        expected.slot = packet.slot;
        expected.sequence = packet.sequence;
        if (std::memcmp(&packet, &expected, sizeof(packet)) != 0 || packet.slot >= m_slots.size())
            throw std::runtime_error("Invalid GPU mirror release fields");
        auto& slot = m_slots[packet.slot];
        if (slot.state != SlotState::LEASED || slot.sequence != packet.sequence)
            throw std::runtime_error("Stale or duplicate GPU mirror release");
        slot.state = SlotState::FREE;
        notify();
        // A wake callback may stop the source synchronously.
        if (!connected()) return;
    }
}

void MirrorGpuSource::allocate(Slot& slot) {
    if (slot.framebuffer) return;
    slot.framebuffer = g_pHyprRenderer->createFB("omarchy private mirror DMA-BUF");
    if (!slot.framebuffer || !slot.framebuffer->alloc(m_width, m_height, DRM_FORMAT_ABGR8888))
        throw std::runtime_error("Cannot allocate GPU mirror RGBA8 framebuffer");
    const auto texture = slot.framebuffer->getTexture();
    if (!texture || !texture->m_texID) throw std::runtime_error("GPU mirror framebuffer has no texture");
    const EGLint attrs[]{EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    slot.image = m_createImage(m_display, m_context, EGL_GL_TEXTURE_2D_KHR,
        reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(texture->m_texID)), attrs);
    if (slot.image == EGL_NO_IMAGE_KHR) throw std::runtime_error("Cannot export GPU mirror texture as EGLImage");
    int format = 0, planes = 0;
    std::array<EGLuint64KHR, OD_MIRROR_MAX_PLANES> modifiers{};
    if (!m_queryImage(m_display, slot.image, &format, &planes, modifiers.data()) || planes < 1 ||
        static_cast<unsigned>(planes) > OD_MIRROR_MAX_PLANES)
        throw std::runtime_error("Cannot query GPU mirror DMA-BUF planes");
    if (!rgb8888(static_cast<uint32_t>(format))) throw std::runtime_error("GPU mirror exported an unsupported pixel format");
    slot.planes = planes;
    slot.format = format;
    slot.modifier = modifiers[0];
    for (int i = 1; i < planes; ++i)
        if (modifiers[i] != modifiers[0]) throw std::runtime_error("GPU mirror DMA-BUF plane modifiers disagree");
    std::array<EGLint, OD_MIRROR_MAX_PLANES> strides{}, offsets{};
    if (!m_exportImage(m_display, slot.image, slot.fds.data(), strides.data(), offsets.data()))
        throw std::runtime_error("Cannot export GPU mirror DMA-BUF descriptors");
    for (int i = 0; i < planes; ++i) {
        // Mesa may report -1 for a plane sharing the first plane's storage.
        if (slot.fds[i] < 0 && i > 0 && slot.fds[0] >= 0) slot.fds[i] = fcntl(slot.fds[0], F_DUPFD_CLOEXEC, 0);
        if (slot.fds[i] < 0 || strides[i] <= 0 || offsets[i] < 0)
            throw std::runtime_error("Invalid exported GPU mirror DMA-BUF plane");
        const int flags = fcntl(slot.fds[i], F_GETFD);
        if (flags < 0 || fcntl(slot.fds[i], F_SETFD, flags | FD_CLOEXEC) < 0) systemFailure("Protecting GPU mirror DMA-BUF descriptor");
        slot.strides[i] = strides[i];
        slot.offsets[i] = offsets[i];
    }
}

void MirrorGpuSource::retire(Slot& slot) {
    for (auto& fd : slot.fds) closeFD(fd);
    if (slot.image != EGL_NO_IMAGE_KHR && m_destroyImage) m_destroyImage(m_display, slot.image);
    slot.image = EGL_NO_IMAGE_KHR;
    slot.framebuffer.reset();
    slot = Slot{};
}

MirrorGpuSource::Slot& MirrorGpuSource::checkedSlot(int index) {
    if (index < 0 || static_cast<size_t>(index) >= m_slots.size()) throw std::out_of_range("Invalid GPU mirror slot");
    return m_slots[index];
}

int MirrorGpuSource::acquire() {
    if (!connected()) return -1;
    if (eglGetCurrentContext() != m_context || eglGetCurrentDisplay() != m_display)
        throw std::logic_error("GPU mirror acquire requires the compositor EGL context");
    for (size_t i = 0; i < m_slots.size(); ++i) {
        auto& slot = m_slots[i];
        if (slot.state != SlotState::FREE) continue;
        try { allocate(slot); } catch (...) { failStream(); throw; }
        slot.state = SlotState::ACQUIRED;
        return i;
    }
    return -1;
}

const SP<Render::IFramebuffer>& MirrorGpuSource::target(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= m_slots.size() || m_slots[index].state != SlotState::ACQUIRED)
        throw std::logic_error("GPU mirror target must be an acquired slot");
    return m_slots[index].framebuffer;
}

void MirrorGpuSource::publish(int index, uint64_t ptsUs) {
    auto& slot = checkedSlot(index);
    if (slot.state != SlotState::ACQUIRED) throw std::logic_error("GPU mirror publish requires an acquired slot");
    try {
        if (!connected()) throw std::runtime_error("GPU mirror consumer is unavailable");
        if (eglGetCurrentContext() != m_context || eglGetCurrentDisplay() != m_display)
            throw std::logic_error("GPU mirror publish requires the compositor EGL context");
        if (m_sequence == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("GPU mirror sequence exhausted");
        const EGLint attrs[]{EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID, EGL_NONE};
        const EGLSyncKHR sync = m_createSync(m_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
        if (sync == EGL_NO_SYNC_KHR) throw std::runtime_error("Cannot create GPU mirror native fence");
        glFlush();
        OwnedFD fence{m_dupFence(m_display, sync)};
        m_destroySync(m_display, sync);
        if (fence.fd < 0) throw std::runtime_error("Cannot export GPU mirror native fence");
        auto frame = message(OD_MIRROR_FRAME);
        frame.sequence = ++m_sequence;
        frame.pts_us = ptsUs;
        frame.slot = index;
        frame.width = m_width;
        frame.height = m_height;
        frame.format = slot.format;
        frame.plane_count = slot.planes;
        frame.modifier = slot.modifier;
        std::array<int, OD_MIRROR_MAX_PLANES + 1> descriptors{};
        for (uint32_t i = 0; i < slot.planes; ++i) {
            frame.strides[i] = slot.strides[i];
            frame.offsets[i] = slot.offsets[i];
            descriptors[i] = slot.fds[i];
        }
        descriptors[slot.planes] = fence.fd;
        sendPacket(m_peer, frame, descriptors.data(), slot.planes + 1);
        slot.sequence = frame.sequence;
        slot.state = SlotState::LEASED;
    } catch (...) { failStream(); throw; }
}

void MirrorGpuSource::cancel(int index) {
    auto& slot = checkedSlot(index);
    if (slot.state != SlotState::ACQUIRED) throw std::logic_error("GPU mirror cancel requires an acquired slot");
    slot.state = SlotState::FREE;
}

}
