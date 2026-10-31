/* Private GPU-only RFB capture. No Wayland connection or compositor output. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <aml.h>
#include <neatvnc.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include "mirror-wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sync_file.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_WAITERS 2
#define FRAME_FDS (OD_MIRROR_MAX_PLANES + 1)

struct capture;
struct socket_path {
    const char *name;
    dev_t device;
    ino_t inode;
    bool bound;
};

enum slot_state { SLOT_FREE, SLOT_FENCE, SLOT_LIVE, SLOT_RELEASE };
struct imported_slot {
    struct capture *capture;
    enum slot_state state;
    struct od_mirror_message message;
    struct gbm_bo *bo;
    int planes[OD_MIRROR_MAX_PLANES];
    int fence;
    struct aml_handler *fence_handler;
    struct nvnc_frame *pending;
    bool released; /* Protected by capture.release_mutex, including shutdown. */
};

struct control_waiter {
    struct capture *capture;
    int fd;
    struct aml_handler *handler;
    uint8_t request[8];
    size_t used;
    uint64_t minimum_pts;
};

struct capture {
    struct aml *loop;
    struct nvnc *server;
    struct nvnc_display *display; /* Borrowed from server after registration. */
    struct gbm_device *device;
    int drm_fd, source_fd, control_fd, release_fd, signal_fd;
    struct aml_handler *source_handler, *control_handler;
    struct aml_handler *release_handler, *signal_handler;
    struct imported_slot slots[OD_MIRROR_SLOTS];
    struct control_waiter waiters[CONTROL_WAITERS];
    pthread_mutex_t release_mutex;
    struct socket_path rfb_path, control_path;
    const char *source_path;
    uint32_t width, height;
    uint64_t received_sequence, received_pts, fed_sequence, fed_pts;
    bool stopping, failed, listening;
};

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static void stop_handler(struct capture *capture, struct aml_handler **handler)
{
    if (!*handler)
        return;
    aml_stop(capture->loop, *handler);
    aml_handler_unref(*handler);
    *handler = NULL;
}

static void close_waiter(struct control_waiter *waiter)
{
    stop_handler(waiter->capture, &waiter->handler);
    close_fd(&waiter->fd);
    waiter->used = 0;
}

static void fail_capture(struct capture *capture, const char *reason)
{
    if (!capture->failed)
        fprintf(stderr, "mirror-capture: %s\n", reason);
    capture->failed = true;
    capture->stopping = true;
    for (size_t i = 0; i < CONTROL_WAITERS; ++i)
        close_waiter(&capture->waiters[i]);
    if (capture->loop)
        aml_exit(capture->loop);
}

static bool peer_is_owner(int fd)
{
    struct ucred peer;
    socklen_t size = sizeof(peer);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
        size == sizeof(peer) && peer.uid == getuid();
}

static bool private_parent(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return false;
    char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent || !slash[1])
        return false;
    *slash = '\0';
    struct stat st;
    return lstat(parent, &st) == 0 && S_ISDIR(st.st_mode) &&
        st.st_uid == getuid() && (st.st_mode & 0777) == 0700;
}

static int bind_private(struct socket_path *path)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path->name);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    struct stat st;
    if (lstat(path->name, &st) < 0) {
        close(fd);
        return -1;
    }
    path->device = st.st_dev;
    path->inode = st.st_ino;
    path->bound = true;
    if (chmod(path->name, 0600) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void unlink_owned(struct socket_path *path)
{
    struct stat st;
    if (path->bound && lstat(path->name, &st) == 0 && S_ISSOCK(st.st_mode) &&
            st.st_uid == getuid() && st.st_dev == path->device && st.st_ino == path->inode)
        unlink(path->name);
}

static bool start_handler(struct capture *capture, struct aml_handler **handler,
        int fd, void (*callback)(struct aml_handler *), void *userdata)
{
    *handler = aml_handler_new(fd, callback, userdata, NULL);
    if (!*handler)
        return false;
    aml_set_event_mask(*handler, AML_EVENT_READ);
    if (aml_start(capture->loop, *handler) < 0) {
        aml_handler_unref(*handler);
        *handler = NULL;
        return false;
    }
    return true;
}

/* Encoder frame retirement is main-loop-only. Keep the notification locked
 * independently, and keep its resources alive through the explicit worker
 * join-and-retire barrier and subsequent display/server-reference retirement. */
static void buffer_released(void *userdata)
{
    struct imported_slot *slot = userdata;
    struct capture *capture = slot->capture;
    pthread_mutex_lock(&capture->release_mutex);
    slot->released = true;
    pthread_mutex_unlock(&capture->release_mutex);
    uint64_t one = 1;
    ssize_t result;
    do {
        result = write(capture->release_fd, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
    if (result != sizeof(one) && !(result < 0 && errno == EAGAIN))
        _exit(EXIT_FAILURE); /* A broken release dispatcher must fail closed. */
}

static void destroy_import(struct imported_slot *slot)
{
    if (slot->bo)
        gbm_bo_destroy(slot->bo);
    slot->bo = NULL;
    for (size_t i = 0; i < OD_MIRROR_MAX_PLANES; ++i)
        close_fd(&slot->planes[i]);
    close_fd(&slot->fence);
}

static void collect_releases(struct capture *capture)
{
    bool released[OD_MIRROR_SLOTS];
    pthread_mutex_lock(&capture->release_mutex);
    for (size_t i = 0; i < OD_MIRROR_SLOTS; ++i) {
        released[i] = capture->slots[i].released;
        capture->slots[i].released = false;
    }
    pthread_mutex_unlock(&capture->release_mutex);
    for (size_t i = 0; i < OD_MIRROR_SLOTS; ++i) {
        if (!released[i])
            continue;
        struct imported_slot *slot = &capture->slots[i];
        destroy_import(slot);
        slot->state = SLOT_RELEASE;
    }
}

static void send_releases(struct capture *capture)
{
    bool blocked = false;
    for (size_t i = 0; i < OD_MIRROR_SLOTS && !capture->stopping; ++i) {
        struct imported_slot *slot = &capture->slots[i];
        if (slot->state != SLOT_RELEASE)
            continue;
        struct od_mirror_message message = {
            .magic = OD_MIRROR_MAGIC, .version = OD_MIRROR_VERSION,
            .type = OD_MIRROR_RELEASE, .slot = i, .sequence = slot->message.sequence,
        };
        ssize_t result;
        do {
            result = send(capture->source_fd, &message, sizeof(message),
                    MSG_DONTWAIT | MSG_NOSIGNAL);
        } while (result < 0 && errno == EINTR);
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            blocked = true;
            break;
        }
        if (result != sizeof(message)) {
            fail_capture(capture, "Cannot release GPU slot lease");
            return;
        }
        slot->state = SLOT_FREE;
    }
    if (capture->source_handler && !capture->stopping)
        aml_set_event_mask(capture->source_handler,
                AML_EVENT_READ | (blocked ? AML_EVENT_WRITE : 0));
}

static void on_release(struct aml_handler *handler)
{
    struct capture *capture = aml_get_userdata(handler);
    uint64_t count;
    ssize_t result;
    do {
        result = read(capture->release_fd, &count, sizeof(count));
    } while (result < 0 && errno == EINTR);
    if (capture->stopping)
        return;
    if (result != sizeof(count) && !(result < 0 && errno == EAGAIN)) {
        fail_capture(capture, "Cannot dispatch GPU slot cleanup");
        return;
    }
    collect_releases(capture);
    send_releases(capture);
}

static void satisfy_waiter(struct control_waiter *waiter)
{
    struct capture *capture = waiter->capture;
    if (waiter->fd < 0 || waiter->used != sizeof(waiter->request) ||
            !capture->fed_sequence || capture->fed_pts < waiter->minimum_pts)
        return;
    const uint8_t ready = 1;
    ssize_t result;
    do {
        result = send(waiter->fd, &ready, sizeof(ready), MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        aml_set_event_mask(waiter->handler, AML_EVENT_READ | AML_EVENT_WRITE);
        return;
    }
    if (result != sizeof(ready)) {
        fail_capture(capture, "Cannot acknowledge installed GPU frame");
        return;
    }
    close_waiter(waiter);
}

static void on_waiter(struct aml_handler *handler)
{
    struct control_waiter *waiter = aml_get_userdata(handler);
    struct capture *capture = waiter->capture;
    if (capture->stopping || waiter->handler != handler)
        return;
    if (aml_get_revents(handler) & AML_EVENT_READ) {
        uint8_t bytes[9];
        ssize_t result;
        do {
            result = recv(waiter->fd, bytes, sizeof(bytes) - waiter->used, MSG_DONTWAIT);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            close_waiter(waiter);
            return;
        }
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fail_capture(capture, "GPU readiness control read failed");
            return;
        }
        if (result > 0) {
            if (waiter->used + result > sizeof(waiter->request)) {
                fail_capture(capture, "Invalid GPU readiness request length");
                return;
            }
            memcpy(waiter->request + waiter->used, bytes, result);
            waiter->used += result;
            if (waiter->used == sizeof(waiter->request)) {
                waiter->minimum_pts = 0;
                for (size_t i = 0; i < sizeof(waiter->request); ++i)
                    waiter->minimum_pts = (waiter->minimum_pts << 8) | waiter->request[i];
            }
        }
    }
    satisfy_waiter(waiter);
}

static void on_control(struct aml_handler *handler)
{
    struct capture *capture = aml_get_userdata(handler);
    if (capture->stopping)
        return;
    /* Bounded accept work; only two connections may retain readiness state. */
    for (size_t attempt = 0; attempt < CONTROL_WAITERS + 1; ++attempt) {
        int fd = accept4(capture->control_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                fail_capture(capture, "GPU readiness accept failed");
            return;
        }
        struct control_waiter *waiter = NULL;
        for (size_t i = 0; i < CONTROL_WAITERS; ++i) {
            if (capture->waiters[i].fd < 0) {
                waiter = &capture->waiters[i];
                break;
            }
        }
        if (!waiter || !peer_is_owner(fd)) {
            close(fd);
            continue;
        }
        waiter->fd = fd;
        waiter->used = 0;
        if (!start_handler(capture, &waiter->handler, fd, on_waiter, waiter)) {
            fail_capture(capture, "Cannot monitor GPU readiness request");
            return;
        }
    }
}

static bool rgb8888(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888: case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBX8888: case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRX8888: case DRM_FORMAT_BGRA8888:
        return true;
    default:
        return false;
    }
}

static void feed_slot(struct imported_slot *slot)
{
    struct capture *capture = slot->capture;
    stop_handler(capture, &slot->fence_handler);
    close_fd(&slot->fence);
    struct nvnc_frame *frame = slot->pending;
    slot->pending = NULL;
    slot->state = SLOT_LIVE;
    if (slot->message.sequence > capture->fed_sequence) {
        nvnc_display_feed_frame(capture->display, frame);
        capture->fed_sequence = slot->message.sequence;
        capture->fed_pts = slot->message.pts_us;
        if (!capture->listening) {
            int fd = bind_private(&capture->rfb_path);
            if (fd < 0 || nvnc_listen(capture->server, fd, NVNC_STREAM_NORMAL) < 0) {
                if (fd >= 0)
                    close(fd);
                fail_capture(capture, "Cannot listen on private GPU RFB socket");
            } else {
                capture->listening = true;
            }
        }
        if (!capture->stopping) {
            for (size_t i = 0; i < CONTROL_WAITERS; ++i) {
                satisfy_waiter(&capture->waiters[i]);
                if (capture->stopping)
                    break;
            }
        }
    }
    /* Display and all encoder/composite refs now solely own the lease. An
     * older fence completing out of order is retired without regressing PTS. */
    nvnc_frame_unref(frame);
}

static int fence_status(int fd)
{
    struct sync_file_info info = {0};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0)
        return -1;
    return info.status;
}

static void on_fence(struct aml_handler *handler)
{
    struct imported_slot *slot = aml_get_userdata(handler);
    if (slot->capture->stopping || slot->fence_handler != handler)
        return;
    if (fence_status(slot->fence) != 1) {
        fail_capture(slot->capture, "Native GPU fence failed");
        return;
    }
    feed_slot(slot);
}

static bool accept_hello(struct capture *capture, const struct od_mirror_message *message,
        int *fds, size_t count)
{
    struct od_mirror_message expected = {
        .magic = OD_MIRROR_MAGIC, .version = OD_MIRROR_VERSION,
        .type = OD_MIRROR_HELLO, .width = capture->width, .height = capture->height,
    };
    if (capture->device || count != 1 || memcmp(message, &expected, sizeof(expected)) ||
            drmGetNodeTypeFromFd(fds[0]) != DRM_NODE_RENDER)
        return false;
    capture->device = gbm_create_device(fds[0]);
    if (!capture->device)
        return false;
    capture->drm_fd = fds[0];
    fds[0] = -1;
    return true;
}

static bool accept_frame(struct capture *capture, const struct od_mirror_message *message,
        int *fds, size_t count)
{
    if (!capture->device || message->slot >= OD_MIRROR_SLOTS ||
            capture->slots[message->slot].state != SLOT_FREE ||
            message->width != capture->width || message->height != capture->height ||
            !rgb8888(message->format) || message->reserved ||
            !message->sequence || message->sequence <= capture->received_sequence ||
            !message->pts_us || message->pts_us <= capture->received_pts ||
            !message->plane_count || message->plane_count > OD_MIRROR_MAX_PLANES ||
            count != message->plane_count + 1 || message->modifier == DRM_FORMAT_MOD_INVALID)
        return false;
    struct gbm_import_fd_modifier_data import = {
        .width = message->width, .height = message->height,
        .format = message->format, .num_fds = message->plane_count,
        .modifier = message->modifier,
    };
    for (size_t i = 0; i < OD_MIRROR_MAX_PLANES; ++i) {
        if (i >= message->plane_count) {
            if (message->strides[i] || message->offsets[i])
                return false;
            continue;
        }
        if (!message->strides[i] || message->strides[i] > INT_MAX ||
                message->offsets[i] > INT_MAX ||
                (!i && message->strides[i] < capture->width * 4))
            return false;
        import.fds[i] = fds[i];
        import.strides[i] = message->strides[i];
        import.offsets[i] = message->offsets[i];
    }
    int status = fence_status(fds[message->plane_count]);
    if (status < 0)
        return false;
    struct imported_slot *slot = &capture->slots[message->slot];
    slot->bo = gbm_bo_import(capture->device, GBM_BO_IMPORT_FD_MODIFIER, &import, 0);
    if (!slot->bo)
        return false;
    /* Never accept a driver conversion, resize or implicit modifier fallback. */
    if (gbm_bo_get_width(slot->bo) != message->width ||
            gbm_bo_get_height(slot->bo) != message->height ||
            gbm_bo_get_format(slot->bo) != message->format ||
            gbm_bo_get_modifier(slot->bo) != message->modifier ||
            gbm_bo_get_plane_count(slot->bo) != (int)message->plane_count) {
        destroy_import(slot);
        return false;
    }
    for (size_t i = 0; i < message->plane_count; ++i) {
        if (gbm_bo_get_stride_for_plane(slot->bo, i) != message->strides[i] ||
                gbm_bo_get_offset(slot->bo, i) != message->offsets[i]) {
            destroy_import(slot);
            return false;
        }
    }
    slot->pending = nvnc_frame_from_gbm_bo(slot->bo);
    if (!slot->pending) {
        destroy_import(slot);
        return false;
    }
    slot->message = *message;
    slot->state = SLOT_FENCE;
    for (size_t i = 0; i < message->plane_count; ++i) {
        slot->planes[i] = fds[i];
        fds[i] = -1;
    }
    slot->fence = fds[message->plane_count];
    fds[message->plane_count] = -1;
    nvnc_buffer_set_userdata(nvnc_frame_get_buffer(slot->pending), slot, buffer_released);
    nvnc_frame_set_pts(slot->pending, message->pts_us);
    nvnc_frame_set_transform(slot->pending, NVNC_TRANSFORM_NORMAL);
    capture->received_sequence = message->sequence;
    capture->received_pts = message->pts_us;
    if (status == 1)
        feed_slot(slot);
    else if (!start_handler(capture, &slot->fence_handler, slot->fence, on_fence, slot))
        return false;
    return !capture->stopping;
}

static void on_source(struct aml_handler *handler)
{
    struct capture *capture = aml_get_userdata(handler);
    if (capture->stopping)
        return;
    if (aml_get_revents(handler) & AML_EVENT_WRITE)
        send_releases(capture);
    /* The producer owns only three leases. Bound work even for malformed peers. */
    for (size_t attempt = 0; attempt < OD_MIRROR_SLOTS + 1 && !capture->stopping; ++attempt) {
        struct od_mirror_message message;
        struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
        union {
            struct cmsghdr align;
            char bytes[CMSG_SPACE(FRAME_FDS * sizeof(int))];
        } ancillary;
        struct msghdr packet = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes),
        };
        ssize_t result;
        do {
            result = recvmsg(capture->source_fd, &packet, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        } while (result < 0 && errno == EINTR);
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        int fds[FRAME_FDS];
        size_t count = 0;
        bool valid = result == sizeof(message) &&
            !(packet.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
        for (struct cmsghdr *cmsg = result >= 0 ? CMSG_FIRSTHDR(&packet) : NULL;
                cmsg; cmsg = CMSG_NXTHDR(&packet, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
                    cmsg->cmsg_len < CMSG_LEN(0)) {
                valid = false;
                continue;
            }
            size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
            if (bytes % sizeof(int))
                valid = false;
            const int *received = (const int *)CMSG_DATA(cmsg);
            for (size_t i = 0; i < bytes / sizeof(int); ++i) {
                if (count < FRAME_FDS)
                    fds[count++] = received[i];
                else {
                    close(received[i]);
                    valid = false;
                }
            }
        }
        if (valid) {
            valid = message.magic == OD_MIRROR_MAGIC && message.version == OD_MIRROR_VERSION;
            if (valid && message.type == OD_MIRROR_HELLO)
                valid = accept_hello(capture, &message, fds, count);
            else if (valid && message.type == OD_MIRROR_FRAME)
                valid = accept_frame(capture, &message, fds, count);
            else
                valid = false;
        }
        for (size_t i = 0; i < count; ++i)
            close_fd(&fds[i]);
        if (!valid) {
            fail_capture(capture, result == 0 ? "GPU source disconnected" : "Invalid GPU source packet or import");
            return;
        }
    }
}

static void on_signal(struct aml_handler *handler)
{
    struct capture *capture = aml_get_userdata(handler);
    struct signalfd_siginfo signal_info;
    if (read(capture->signal_fd, &signal_info, sizeof(signal_info)) == sizeof(signal_info)) {
        capture->stopping = true;
        for (size_t i = 0; i < CONTROL_WAITERS; ++i)
            close_waiter(&capture->waiters[i]);
        aml_exit(capture->loop);
    }
}

static bool dimension(const char *value, uint32_t maximum, uint32_t *result)
{
    char *end;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || !*value || *end || number < 1 || number > maximum)
        return false;
    *result = number;
    return true;
}

static bool arguments(struct capture *capture, int argc, char **argv)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc)
            return false;
        if (!strcmp(argv[i], "--source") && !capture->source_path)
            capture->source_path = argv[i + 1];
        else if (!strcmp(argv[i], "--rfb") && !capture->rfb_path.name)
            capture->rfb_path.name = argv[i + 1];
        else if (!strcmp(argv[i], "--control") && !capture->control_path.name)
            capture->control_path.name = argv[i + 1];
        else if (!strcmp(argv[i], "--width") && !capture->width) {
            if (!dimension(argv[i + 1], 3840, &capture->width))
                return false;
        } else if (!strcmp(argv[i], "--height") && !capture->height) {
            if (!dimension(argv[i + 1], 2160, &capture->height))
                return false;
        } else
            return false;
    }
    return capture->width && capture->height && private_parent(capture->source_path) &&
        private_parent(capture->rfb_path.name) && private_parent(capture->control_path.name) &&
        strcmp(capture->source_path, capture->rfb_path.name) &&
        strcmp(capture->source_path, capture->control_path.name) &&
        strcmp(capture->rfb_path.name, capture->control_path.name);
}

static bool initialize(struct capture *capture)
{
    struct stat st;
    if (lstat(capture->source_path, &st) < 0 || !S_ISSOCK(st.st_mode) ||
            st.st_uid != getuid() || (st.st_mode & 0777) != 0600)
        return false;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL) != 0)
        return false;
    signal(SIGPIPE, SIG_IGN);
    capture->signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    capture->release_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    capture->loop = aml_new();
    if (capture->signal_fd < 0 || capture->release_fd < 0 || !capture->loop)
        return false;
    aml_set_default(capture->loop);
    /* The patched private server rejects non-ODR3 update requests in this
     * process before creating an encoder. Extend's wayvnc does not set this. */
    if (setenv("OMARCHY_TV_MIRROR_DMABUF", "1", 1) < 0)
        return false;
    capture->server = nvnc_new();
    if (!capture->server)
        return false;
    nvnc_set_name(capture->server, "Omarchy direct GPU Mirror");
    capture->display = nvnc_display_new(0, 0);
    if (!capture->display)
        return false;
    nvnc_display_set_logical_size(capture->display, capture->width, capture->height);
    nvnc_add_display(capture->server, capture->display);
    nvnc_display_unref(capture->display);
    capture->source_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un source = {.sun_family = AF_UNIX};
    strcpy(source.sun_path, capture->source_path);
    if (capture->source_fd < 0 || connect(capture->source_fd,
                (struct sockaddr *)&source, sizeof(source)) < 0 || !peer_is_owner(capture->source_fd))
        return false;
    capture->control_fd = bind_private(&capture->control_path);
    if (capture->control_fd < 0 || listen(capture->control_fd, CONTROL_WAITERS) < 0)
        return false;
    return start_handler(capture, &capture->signal_handler, capture->signal_fd, on_signal, capture) &&
        start_handler(capture, &capture->release_handler, capture->release_fd, on_release, capture) &&
        start_handler(capture, &capture->source_handler, capture->source_fd, on_source, capture) &&
        start_handler(capture, &capture->control_handler, capture->control_fd, on_control, capture);
}

static void shutdown_capture(struct capture *capture)
{
    capture->stopping = true;
    for (size_t i = 0; i < CONTROL_WAITERS; ++i)
        close_waiter(&capture->waiters[i]);
    stop_handler(capture, &capture->control_handler);
    stop_handler(capture, &capture->source_handler);
    stop_handler(capture, &capture->signal_handler);
    stop_handler(capture, &capture->release_handler);
    close_fd(&capture->control_fd);
    /* Disconnect retires the whole producer generation; no slot is reused
     * while a cancelled/final encoder still owns its imported DMA-BUF. */
    close_fd(&capture->source_fd);
    close_fd(&capture->signal_fd);
    /* Hold our strong loop reference throughout the join. A worker may itself
     * briefly hold a strong loop reference while queueing completion, so an
     * ordinary unref is not a quiescence barrier. The barrier retires queued
     * work refs here without callbacks, before display/server teardown. */
    if (capture->loop && aml_stop_workers(capture->loop) < 0) {
        fprintf(stderr, "mirror-capture: Cannot quiesce private encoder workers: %s\n",
                strerror(errno));
        _exit(EXIT_FAILURE); /* Never destroy imports under a live worker. */
    }
    for (size_t i = 0; i < OD_MIRROR_SLOTS; ++i) {
        struct imported_slot *slot = &capture->slots[i];
        stop_handler(capture, &slot->fence_handler);
        close_fd(&slot->fence);
        if (slot->pending)
            nvnc_frame_unref(slot->pending);
        slot->pending = NULL;
    }
    if (capture->server)
        nvnc_del(capture->server);
    capture->server = NULL;
    capture->display = NULL;
    if (capture->loop)
        aml_loop_unref(capture->loop); /* No worker or work callbacks remain. */
    capture->loop = NULL;
    aml_set_default(NULL);
    collect_releases(capture);
    for (size_t i = 0; i < OD_MIRROR_SLOTS; ++i)
        destroy_import(&capture->slots[i]);
    if (capture->device)
        gbm_device_destroy(capture->device);
    close_fd(&capture->drm_fd);
    close_fd(&capture->release_fd);
    pthread_mutex_destroy(&capture->release_mutex);
    unlink_owned(&capture->rfb_path);
    unlink_owned(&capture->control_path);
}

int main(int argc, char **argv)
{
    struct capture capture = {
        .drm_fd = -1, .source_fd = -1, .control_fd = -1,
        .release_fd = -1, .signal_fd = -1,
        .release_mutex = PTHREAD_MUTEX_INITIALIZER,
    };
    for (size_t i = 0; i < OD_MIRROR_SLOTS; ++i) {
        capture.slots[i].capture = &capture;
        capture.slots[i].fence = -1;
        for (size_t plane = 0; plane < OD_MIRROR_MAX_PLANES; ++plane)
            capture.slots[i].planes[plane] = -1;
    }
    for (size_t i = 0; i < CONTROL_WAITERS; ++i) {
        capture.waiters[i].capture = &capture;
        capture.waiters[i].fd = -1;
    }
    umask(0077);
    if (!arguments(&capture, argc, argv)) {
        fprintf(stderr, "Usage: mirror-capture --source PATH --rfb PATH --control PATH --width N --height N\n"
                "Sockets require absolute paths in user-owned 0700 directories.\n");
        shutdown_capture(&capture);
        return EXIT_FAILURE;
    }
    if (!initialize(&capture))
        fail_capture(&capture, "Cannot initialize private direct GPU capture");
    else
        aml_run(capture.loop);
    shutdown_capture(&capture);
    return capture.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
