#ifndef OMARCHY_DISPLAY_MIRROR_WIRE_H
#define OMARCHY_DISPLAY_MIRROR_WIRE_H
#include <stddef.h>
#include <stdint.h>

/* Private, same-host native-endian SOCK_SEQPACKET messages; pixels use SCM_RIGHTS. */
#define OD_MIRROR_MAGIC 0x4f444746u
#define OD_MIRROR_VERSION 1u
#define OD_MIRROR_SLOTS 3u
#define OD_MIRROR_MAX_PLANES 4u

enum od_mirror_message_type {
    OD_MIRROR_HELLO = 1,
    OD_MIRROR_FRAME = 2,
    OD_MIRROR_RELEASE = 3,
};

struct od_mirror_message {
    uint32_t magic;
    uint16_t version, type;
    uint64_t sequence, pts_us, modifier;
    uint32_t slot, width, height, format, plane_count;
    uint32_t strides[OD_MIRROR_MAX_PLANES];
    uint32_t offsets[OD_MIRROR_MAX_PLANES];
    uint32_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(od_mirror_message) == 88);
static_assert(offsetof(od_mirror_message, strides) == 52);
static_assert(offsetof(od_mirror_message, reserved) == 84);
#else
_Static_assert(sizeof(struct od_mirror_message) == 88, "Mirror message ABI");
_Static_assert(offsetof(struct od_mirror_message, strides) == 52, "Mirror plane ABI");
_Static_assert(offsetof(struct od_mirror_message, reserved) == 84, "Mirror reserved ABI");
#endif
#endif
