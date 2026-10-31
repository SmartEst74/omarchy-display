/*
 * Private neatvnc ODR3 encoder. The capture process, not the compositor, owns
 * this code. There is deliberately no CPU comparison or software GL fallback.
 */
#include "neatvnc.h"
#include "rfb-proto.h"
#include "buffer.h"
#include "frame.h"
#include "enc/encoder.h"
#include "enc/util.h"

#include <aml.h>
#include <pixman.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <turbojpeg.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define OD_TILE 64u
#define OD_IMPORTS 4
#define OD_PLANES 4
#define OD_MAX_RECTS 64
#define OD_JPEG_PIXELS (1920u * 1080u)
#define OD_MAX_PACKET (32u * 1024u * 1024u)
#define OD_RECT_HEADER 28u
#define OD_ATLAS_FREE (OD_MAX_RECTS * 2)

struct encoder_impl encoder_impl_od_regions;
struct encoder* od_regions_new(uint16_t width, uint16_t height);

struct od_plane_key {
	dev_t device;
	ino_t inode;
	uint32_t offset;
	uint32_t pitch;
};

struct od_image_key {
	uint32_t width, height, format;
	uint64_t modifier;
	int n_planes;
	struct od_plane_key planes[OD_PLANES];
};

struct od_import {
	struct od_image_key key;
	EGLImageKHR image;
	GLuint texture;
	uint64_t used;
};

struct od_tile {
	uint32_t flags, rgba;
};

struct od_rectangle {
	pixman_box32_t box;
	uint32_t rgba;
	bool solid;
};

struct od_atlas_map {
	int atlas; /* -1 keeps the independent kind-0 JPEG. */
	uint16_t x, y;
};

struct od_atlas {
	uint16_t width, height, count;
	pixman_box32_t source;
};

struct od_atlas_plan {
	struct od_atlas_map maps[OD_MAX_RECTS];
	struct od_atlas atlases[OD_MAX_RECTS];
	int count, images;
	uint64_t area;
};

struct od_atlas_free {
	uint16_t x, y, width, height, atlas;
};

struct od_gpu {
	int fd;
	dev_t device;
	struct gbm_device* gbm;
	EGLDisplay display;
	EGLContext context;
	bool initialized;
	bool modifiers;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
	GLuint program, previous, flags;
	GLint force_location;
	uint32_t width, height, cols, rows;
	uint64_t generation, clock;
	struct od_import imports[OD_IMPORTS];
	struct gbm_bo* atlas_bo;
	EGLImageKHR atlas_image;
	GLuint atlas_texture, atlas_fbo, source_fbo;
};

struct od_encoder {
	struct encoder encoder;
	/* Main-loop state. The worker only reads its own generation snapshot. */
	bool busy;
	uint64_t generation;
	/* Touched by one job at a time; destruction waits for the job's reference. */
	struct od_gpu gpu;
	tjhandle jpeg;
	/* One worker owns these bounded, reusable packing scratch arrays. */
	struct od_atlas_plan atlas_plan, atlas_trial;
	struct od_atlas_free atlas_free[OD_ATLAS_FREE];
	int atlas_order[OD_MAX_RECTS];
};

struct od_work {
	struct od_encoder* parent;
	struct nvnc_composite_fb fb;
	uint64_t generation;
	struct encoded_frame* result;
	struct od_rectangle rectangles[OD_MAX_RECTS];
	int n_rects;
	char error[512];
};

/* A worker may be reused by other EGL users. Restore their thread binding. */
struct od_binding {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw, read;
	EGLenum api;
};

static const char od_compute_source[] =
	"#version 310 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"layout(local_size_x=16, local_size_y=16) in;\n"
	"uniform highp sampler2D currentImage;\n"
	"uniform highp sampler2D previousImage;\n"
	"layout(rgba8, binding=0) writeonly uniform highp image2D baselineImage;\n"
	"layout(std430, binding=0) writeonly buffer Flags { uvec2 tiles[]; };\n"
	"uniform bool forceFull;\n"
	"shared uint changed;\n"
	"shared vec3 reference;\n"
	"void main() {\n"
	"  ivec2 origin = ivec2(gl_WorkGroupID.xy) * 64;\n"
	"  if (gl_LocalInvocationIndex == 0u) {\n"
	"    changed = 0u;\n"
	"    reference = texelFetch(currentImage, origin, 0).rgb;\n"
	"  }\n"
	"  barrier();\n"
	"  ivec2 size = textureSize(currentImage, 0);\n"
	"  bool localDirty = false;\n"
	"  bool nonSolid = false;\n"
	"  for (int y = int(gl_LocalInvocationID.y); y < 64; y += 16) {\n"
	"    for (int x = int(gl_LocalInvocationID.x); x < 64; x += 16) {\n"
	"      ivec2 p = origin + ivec2(x, y);\n"
	"      if (p.x >= size.x || p.y >= size.y) continue;\n"
	"      vec3 rgb = texelFetch(currentImage, p, 0).rgb;\n"
	"      nonSolid = nonSolid || any(notEqual(rgb, reference));\n"
	"      bool different = forceFull;\n"
	"      if (!forceFull) {\n"
	"        different = any(notEqual(rgb, texelFetch(previousImage, p, 0).rgb));\n"
	"      }\n"
	"      if (different) {\n"
	"        localDirty = true;\n"
	"        imageStore(baselineImage, p, vec4(rgb, 1.0));\n"
	"      }\n"
	"    }\n"
	"  }\n"
	"  uint properties = (localDirty ? 1u : 0u) | (nonSolid ? 2u : 0u);\n"
	"  if (properties != 0u) atomicOr(changed, properties);\n"
	"  barrier();\n"
	"  if (gl_LocalInvocationIndex == 0u)\n"
	"    tiles[gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x] =\n"
	"      uvec2(changed, packUnorm4x8(vec4(reference, 1.0)));\n"
	"}\n";

static int od_fail(struct od_work* work, const char* format, ...)
{
	va_list ap;
	va_start(ap, format);
	vsnprintf(work->error, sizeof(work->error), format, ap);
	va_end(ap);
	return -1;
}

static bool od_extension(const char* extensions, const char* name)
{
	if (!extensions)
		return false;
	size_t n = strlen(name);
	const char* p = extensions;
	while ((p = strstr(p, name))) {
		if ((p == extensions || p[-1] == ' ') &&
				(p[n] == '\0' || p[n] == ' '))
			return true;
		p += n;
	}
	return false;
}

static bool od_software_renderer(const char* renderer)
{
	static const char* const software[] = {
		"llvmpipe", "softpipe", "swrast", "software", "lavapipe",
		"swiftshader", "swr", "gdi generic",
	};
	if (!renderer || !*renderer)
		return true;
	for (size_t i = 0; i < sizeof(software) / sizeof(software[0]); ++i)
		if (strcasestr(renderer, software[i]))
			return true;
	return false;
}

static void od_save_binding(struct od_binding* binding)
{
	binding->display = eglGetCurrentDisplay();
	binding->context = eglGetCurrentContext();
	binding->draw = eglGetCurrentSurface(EGL_DRAW);
	binding->read = eglGetCurrentSurface(EGL_READ);
	binding->api = eglQueryAPI();
}

static bool od_restore_binding(struct od_gpu* gpu,
		const struct od_binding* binding)
{
	bool ok = true;
	if (eglGetCurrentContext() == gpu->context &&
			gpu->context != EGL_NO_CONTEXT)
		ok = eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				EGL_NO_CONTEXT) == EGL_TRUE;
	if (!eglBindAPI(binding->api))
		ok = false;
	if (binding->context != EGL_NO_CONTEXT &&
			!eglMakeCurrent(binding->display, binding->draw, binding->read,
				binding->context))
		ok = false;
	return ok;
}

static void od_drop_import(struct od_gpu* gpu, struct od_import* image,
		bool current)
{
	if (current && image->texture)
		glDeleteTextures(1, &image->texture);
	if (image->image != EGL_NO_IMAGE_KHR && gpu->destroy_image)
		gpu->destroy_image(gpu->display, image->image);
	memset(image, 0, sizeof(*image));
}

static void od_gpu_destroy(struct od_gpu* gpu)
{
	struct od_binding saved;
	od_save_binding(&saved);
	bool current = gpu->context != EGL_NO_CONTEXT &&
		eglBindAPI(EGL_OPENGL_ES_API) &&
		eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				gpu->context);
	for (int i = 0; i < OD_IMPORTS; ++i)
		od_drop_import(gpu, &gpu->imports[i], current);
	if (current) {
		glDeleteTextures(1, &gpu->previous);
		glDeleteBuffers(1, &gpu->flags);
		glDeleteFramebuffers(1, &gpu->source_fbo);
		glDeleteFramebuffers(1, &gpu->atlas_fbo);
		glDeleteTextures(1, &gpu->atlas_texture);
		if (gpu->program)
			glDeleteProgram(gpu->program);
	}
	if (gpu->atlas_image != EGL_NO_IMAGE_KHR && gpu->destroy_image)
		gpu->destroy_image(gpu->display, gpu->atlas_image);
	if (gpu->atlas_bo)
		gbm_bo_destroy(gpu->atlas_bo);
	od_restore_binding(gpu, &saved);
	if (gpu->context != EGL_NO_CONTEXT)
		eglDestroyContext(gpu->display, gpu->context);
	if (gpu->initialized)
		eglTerminate(gpu->display);
	if (gpu->gbm)
		gbm_device_destroy(gpu->gbm);
	if (gpu->fd >= 0)
		close(gpu->fd);
	memset(gpu, 0, sizeof(*gpu));
	gpu->fd = -1;
}

static int od_compile(struct od_work* work)
{
	struct od_gpu* gpu = &work->parent->gpu;
	GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
	const GLchar* source = od_compute_source;
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[384] = { 0 };
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		glDeleteShader(shader);
		return od_fail(work, "GLES 3.1 tile shader compilation failed: %s", log);
	}
	gpu->program = glCreateProgram();
	glAttachShader(gpu->program, shader);
	glLinkProgram(gpu->program);
	glDeleteShader(shader);
	glGetProgramiv(gpu->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[384] = { 0 };
		glGetProgramInfoLog(gpu->program, sizeof(log), NULL, log);
		return od_fail(work, "GLES 3.1 tile shader linking failed: %s", log);
	}
	glUseProgram(gpu->program);
	GLint current = glGetUniformLocation(gpu->program, "currentImage");
	GLint previous = glGetUniformLocation(gpu->program, "previousImage");
	gpu->force_location = glGetUniformLocation(gpu->program, "forceFull");
	if (current < 0 || previous < 0 || gpu->force_location < 0)
		return od_fail(work, "GLES tile shader uniforms are unavailable");
	glUniform1i(current, 0);
	glUniform1i(previous, 1);
	return 0;
}

static int od_gpu_init(struct od_work* work, struct gbm_bo* bo)
{
	struct od_gpu* gpu = &work->parent->gpu;
	int source_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
	struct stat st;
	if (fstat(source_fd, &st) < 0 || !S_ISCHR(st.st_mode))
		return od_fail(work, "GBM source is not a DRM character device");
	if (gpu->context != EGL_NO_CONTEXT) {
		if (gpu->device != st.st_rdev)
			return od_fail(work, "Capture changed DRM devices during a region stream");
		if (!eglBindAPI(EGL_OPENGL_ES_API) ||
				!eglMakeCurrent(gpu->display, EGL_NO_SURFACE,
					EGL_NO_SURFACE, gpu->context))
			return od_fail(work, "Cannot bind region EGL context (0x%x)", eglGetError());
		return 0;
	}

	gpu->device = st.st_rdev;
	gpu->fd = fcntl(source_fd, F_DUPFD_CLOEXEC, 3);
	if (gpu->fd < 0 || !(gpu->gbm = gbm_create_device(gpu->fd)))
		return od_fail(work, "Cannot create private GBM device: %s", strerror(errno));
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_display)
		return od_fail(work, "EGL GBM platform entry point is unavailable");
	gpu->display = get_display(EGL_PLATFORM_GBM_KHR, gpu->gbm, NULL);
	if (gpu->display == EGL_NO_DISPLAY ||
			!eglInitialize(gpu->display, NULL, NULL))
		return od_fail(work, "Cannot initialize hardware GBM EGL display (0x%x)", eglGetError());
	gpu->initialized = true;
	const char* extensions = eglQueryString(gpu->display, EGL_EXTENSIONS);
	if (!od_extension(extensions, "EGL_KHR_surfaceless_context") ||
			!od_extension(extensions, "EGL_EXT_image_dma_buf_import") ||
			!od_extension(extensions, "EGL_KHR_create_context"))
		return od_fail(work, "Hardware EGL requires surfaceless, DMA-BUF import and ES 3.1 contexts");
	gpu->modifiers = od_extension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
	gpu->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	gpu->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	gpu->image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!gpu->create_image || !gpu->destroy_image || !gpu->image_target)
		return od_fail(work, "Hardware EGL image entry points are unavailable");
	if (!eglBindAPI(EGL_OPENGL_ES_API))
		return od_fail(work, "Cannot select GLES for region encoder");
	const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE, 0, /* Surfaceless GBM needs no pbuffer-capable config. */
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE,
	};
	EGLConfig config;
	EGLint n_configs = 0;
	if (!eglChooseConfig(gpu->display, config_attrs, &config, 1, &n_configs) ||
			n_configs < 1)
		return od_fail(work, "No hardware GLES 3 EGL configuration");
	const EGLint context_attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
		EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE,
	};
	gpu->context = eglCreateContext(gpu->display, config, EGL_NO_CONTEXT, context_attrs);
	if (gpu->context == EGL_NO_CONTEXT ||
			!eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context))
		return od_fail(work, "Cannot create hardware GLES 3.1 context (0x%x)", eglGetError());
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	if (od_software_renderer(renderer))
		return od_fail(work, "Refusing software GLES renderer: %s", renderer ? renderer : "unknown");
	if (od_extension(extensions, "EGL_MESA_query_driver")) {
		PFNEGLGETDISPLAYDRIVERNAMEPROC driver_name =
			(PFNEGLGETDISPLAYDRIVERNAMEPROC)eglGetProcAddress("eglGetDisplayDriverName");
		if (driver_name && od_software_renderer(driver_name(gpu->display)))
			return od_fail(work, "Refusing software EGL driver");
	}
	GLint major = 0, minor = 0, invocations = 0, sx = 0, sy = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &invocations);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &sx);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &sy);
	if (major < 3 || (major == 3 && minor < 1) || invocations < 256 || sx < 16 || sy < 16 ||
			!od_extension((const char*)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image"))
		return od_fail(work, "Hardware GLES lacks required ES 3.1 compute/image capabilities");
	if (od_compile(work) < 0)
		return -1;
	nvnc_log(NVNC_LOG_INFO, "ODR3 hardware tile comparison and solid classification: %s", renderer);
	return 0;
}

static int od_gpu_resize(struct od_work* work, uint32_t width, uint32_t height)
{
	struct od_gpu* gpu = &work->parent->gpu;
	if (gpu->width == width && gpu->height == height)
		return 0;
	GLint max_texture = 0, max_x = 0, max_y = 0;
	GLint64 max_storage = 0;
	uint32_t cols = (width + OD_TILE - 1) / OD_TILE;
	uint32_t rows = (height + OD_TILE - 1) / OD_TILE;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &max_x);
	glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &max_y);
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_storage);
	if (width > (uint32_t)max_texture || height > (uint32_t)max_texture ||
			cols > (uint32_t)max_x || rows > (uint32_t)max_y ||
			(uint64_t)cols * rows * sizeof(struct od_tile) > (uint64_t)max_storage)
		return od_fail(work, "Native capture dimensions exceed hardware GLES limits");
	for (int i = 0; i < OD_IMPORTS; ++i)
		od_drop_import(gpu, &gpu->imports[i], true);
	glDeleteTextures(1, &gpu->previous);
	glGenTextures(1, &gpu->previous);
	glBindTexture(GL_TEXTURE_2D, gpu->previous);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (!gpu->flags)
		glGenBuffers(1, &gpu->flags);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gpu->flags);
	glBufferData(GL_SHADER_STORAGE_BUFFER, cols * rows * sizeof(struct od_tile), NULL, GL_DYNAMIC_READ);
	gpu->width = width;
	gpu->height = height;
	gpu->cols = cols;
	gpu->rows = rows;
	gpu->generation = 0; /* A newly allocated previous image is undefined. */
	return 0;
}

static bool od_same_image(const struct od_image_key* a, const struct od_image_key* b)
{
	if (a->width != b->width || a->height != b->height || a->format != b->format ||
			a->modifier != b->modifier || a->n_planes != b->n_planes)
		return false;
	for (int i = 0; i < a->n_planes; ++i)
		if (a->planes[i].device != b->planes[i].device ||
				a->planes[i].inode != b->planes[i].inode ||
				a->planes[i].offset != b->planes[i].offset ||
				a->planes[i].pitch != b->planes[i].pitch)
			return false;
	return true;
}

static int od_import_image(struct od_work* work, struct gbm_bo* bo, GLuint* texture)
{
	struct od_gpu* gpu = &work->parent->gpu;
	struct od_image_key key = {
		.width = gbm_bo_get_width(bo), .height = gbm_bo_get_height(bo),
		.format = gbm_bo_get_format(bo), .modifier = gbm_bo_get_modifier(bo),
		.n_planes = gbm_bo_get_plane_count(bo),
	};
	int fds[OD_PLANES] = { -1, -1, -1, -1 };
	int rc = -1;
	if (key.n_planes < 1 || key.n_planes > OD_PLANES)
		return od_fail(work, "Unsupported DMA-BUF plane count: %d", key.n_planes);
	if (!gpu->modifiers && (key.n_planes > 3 ||
			(key.modifier != DRM_FORMAT_MOD_INVALID && key.modifier != DRM_FORMAT_MOD_LINEAR)))
		return od_fail(work, "Hardware EGL cannot import this explicit DMA-BUF modifier");
	for (int i = 0; i < key.n_planes; ++i) {
		fds[i] = gbm_bo_get_fd_for_plane(bo, i);
		struct stat st;
		if (fds[i] < 0 || fstat(fds[i], &st) < 0) {
			od_fail(work, "Cannot export capture DMA-BUF plane: %s", strerror(errno));
			goto done;
		}
		key.planes[i].device = st.st_dev;
		key.planes[i].inode = st.st_ino;
		key.planes[i].offset = gbm_bo_get_offset(bo, i);
		key.planes[i].pitch = gbm_bo_get_stride_for_plane(bo, i);
		if (!key.planes[i].pitch || key.planes[i].pitch > INT_MAX ||
				key.planes[i].offset > INT_MAX) {
			od_fail(work, "DMA-BUF plane pitch/offset is outside EGL integer range");
			goto done;
		}
	}

	struct od_import* slot = NULL;
	for (int i = 0; i < OD_IMPORTS; ++i) {
		struct od_import* entry = &gpu->imports[i];
		if (entry->image != EGL_NO_IMAGE_KHR && od_same_image(&entry->key, &key)) {
			slot = entry;
			break;
		}
	}
	if (!slot) {
		slot = &gpu->imports[0];
		for (int i = 1; i < OD_IMPORTS; ++i)
			if (gpu->imports[i].used < slot->used)
				slot = &gpu->imports[i];
		od_drop_import(gpu, slot, true);
		static const EGLint fd_attr[] = {
			EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
			EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT,
		};
		static const EGLint offset_attr[] = {
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
			EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
		};
		static const EGLint pitch_attr[] = {
			EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
			EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
		};
		static const EGLint mod_lo[] = {
			EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
		};
		static const EGLint mod_hi[] = {
			EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
			EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
		};
		EGLint attrs[7 + OD_PLANES * 10];
		int n = 0;
		attrs[n++] = EGL_WIDTH; attrs[n++] = key.width;
		attrs[n++] = EGL_HEIGHT; attrs[n++] = key.height;
		attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[n++] = key.format;
		for (int i = 0; i < key.n_planes; ++i) {
			attrs[n++] = fd_attr[i]; attrs[n++] = fds[i];
			attrs[n++] = offset_attr[i]; attrs[n++] = key.planes[i].offset;
			attrs[n++] = pitch_attr[i]; attrs[n++] = key.planes[i].pitch;
			if (gpu->modifiers && key.modifier != DRM_FORMAT_MOD_INVALID) {
				attrs[n++] = mod_lo[i]; attrs[n++] = (EGLint)(uint32_t)key.modifier;
				attrs[n++] = mod_hi[i]; attrs[n++] = (EGLint)(uint32_t)(key.modifier >> 32);
			}
		}
		attrs[n++] = EGL_NONE;
		slot->image = gpu->create_image(gpu->display, EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
		if (slot->image == EGL_NO_IMAGE_KHR) {
			od_fail(work, "Hardware EGL DMA-BUF import failed (0x%x)", eglGetError());
			goto done;
		}
		glGenTextures(1, &slot->texture);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, slot->texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		slot->key = key;
	}
	/* The EGLImage owns its DMA-BUF references, not a capture-pool buffer ref.
	 * Rebind even a cached image after an external producer has written it. */
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, slot->texture);
	gpu->image_target(GL_TEXTURE_2D, (GLeglImageOES)slot->image);
	slot->used = ++gpu->clock;
	*texture = slot->texture;
	rc = 0;
done:
	for (int i = 0; i < OD_PLANES; ++i)
		if (fds[i] >= 0)
			close(fds[i]);
	return rc;
}

static int od_wait_gpu(struct od_work* work)
{
	GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (!fence)
		return od_fail(work, "Cannot fence GPU tile comparison");
	GLenum status = GL_TIMEOUT_EXPIRED;
	for (int i = 0; i < 5 && status == GL_TIMEOUT_EXPIRED; ++i)
		status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	glDeleteSync(fence);
	if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
		return od_fail(work, "GPU tile comparison failed or exceeded five seconds (0x%x)", status);
	return 0;
}

static bool od_add_solid(struct od_work* work, pixman_box32_t box, uint32_t rgba)
{
	for (int i = 0; i < work->n_rects; ++i) {
		struct od_rectangle* rectangle = &work->rectangles[i];
		if (rectangle->rgba == rgba && rectangle->box.x1 == box.x1 &&
				rectangle->box.x2 == box.x2 && rectangle->box.y2 == box.y1) {
			rectangle->box.y2 = box.y2;
			return true;
		}
	}
	if (work->n_rects == OD_MAX_RECTS)
		return false;
	work->rectangles[work->n_rects++] = (struct od_rectangle){
		.box = box, .rgba = rgba, .solid = true,
	};
	return true;
}

static int od_dirty_region(struct od_work* work, struct gbm_bo* bo,
		pixman_region32_t* region, pixman_region32_t* jpeg)
{
	struct od_gpu* gpu = &work->parent->gpu;
	struct od_binding saved;
	od_save_binding(&saved);
	int rc = -1;
	if (od_gpu_init(work, bo) < 0 ||
			od_gpu_resize(work, gbm_bo_get_width(bo), gbm_bo_get_height(bo)) < 0)
		goto done;
	GLuint current;
	if (od_import_image(work, bo, &current) < 0)
		goto done;
	bool full = gpu->generation != work->generation;
	glUseProgram(gpu->program);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, gpu->previous);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, current);
	glBindImageTexture(0, gpu->previous, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gpu->flags);
	glUniform1i(gpu->force_location, full);
	glDispatchCompute(gpu->cols, gpu->rows, 1);
	/* Each invocation owns its pixels. Reads use the GPU texture cache;
	 * make image stores visible to those cached reads in the next dispatch. */
	glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
			GL_BUFFER_UPDATE_BARRIER_BIT);
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		od_fail(work, "Hardware GPU tile dispatch failed (0x%x)", error);
		goto done;
	}
	if (od_wait_gpu(work) < 0)
		goto done;
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gpu->flags);
	const struct od_tile* tiles = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
			gpu->cols * gpu->rows * sizeof(*tiles), GL_MAP_READ_BIT);
	if (!tiles) {
		od_fail(work, "Cannot read compact GPU tile classification");
		goto done;
	}
	bool ok = true;
	for (uint32_t y = 0; y < gpu->rows && ok; ++y) {
		for (uint32_t x = 0; x < gpu->cols && ok;) {
			const struct od_tile* tile = &tiles[y * gpu->cols + x];
			if (!(tile->flags & 1u)) {
				++x;
				continue;
			}
			bool solid = !(tile->flags & 2u);
			uint32_t rgba = tile->rgba, first = x++;
			while (x < gpu->cols) {
				const struct od_tile* next = &tiles[y * gpu->cols + x];
				if (!(next->flags & 1u) || (work->n_rects >= 0 &&
						(solid != !(next->flags & 2u) || (solid && next->rgba != rgba))))
					break;
				++x;
			}
			uint32_t right = x * OD_TILE, bottom = (y + 1) * OD_TILE;
			if (right > gpu->width) right = gpu->width;
			if (bottom > gpu->height) bottom = gpu->height;
			pixman_box32_t box = { first * OD_TILE, y * OD_TILE, right, bottom };
			ok = pixman_region32_union_rect(region, region, box.x1, box.y1,
					box.x2 - box.x1, box.y2 - box.y1);
			if (work->n_rects < 0)
				continue;
			if (solid) {
				if (!od_add_solid(work, box, rgba))
					work->n_rects = -1; /* Fragmented content uses bounded JPEG regions. */
			} else {
				ok = ok && pixman_region32_union_rect(jpeg, jpeg, box.x1, box.y1,
						box.x2 - box.x1, box.y2 - box.y1);
			}
		}
	}
	if (!glUnmapBuffer(GL_SHADER_STORAGE_BUFFER) || !ok) {
		od_fail(work, "GPU tile classification or region allocation failed");
		goto done;
	}
	error = glGetError();
	if (error != GL_NO_ERROR) {
		od_fail(work, "Hardware GPU dirty-flag readback failed (0x%x)", error);
		goto done;
	}
	gpu->generation = work->generation;
	rc = 0;
done:
	if (!od_restore_binding(gpu, &saved))
		rc = od_fail(work, "Cannot release/restore worker EGL binding");
	return rc;
}

static int od_pixel_format(uint32_t format)
{
	/* DRM fourcc words are little-endian; TurboJPEG consumes byte layouts. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	switch (format) {
	case DRM_FORMAT_XRGB8888: return TJPF_BGRX;
	case DRM_FORMAT_ARGB8888: return TJPF_BGRA;
	case DRM_FORMAT_XBGR8888: return TJPF_RGBX;
	case DRM_FORMAT_ABGR8888: return TJPF_RGBA;
	case DRM_FORMAT_RGBX8888: return TJPF_XBGR;
	case DRM_FORMAT_RGBA8888: return TJPF_ABGR;
	case DRM_FORMAT_BGRX8888: return TJPF_XRGB;
	case DRM_FORMAT_BGRA8888: return TJPF_ARGB;
	default: return TJPF_UNKNOWN;
	}
#else
	(void)format;
	return TJPF_UNKNOWN;
#endif
}

static void od_be16(uint8_t* p, uint16_t value)
{
	p[0] = value >> 8;
	p[1] = value;
}

static void od_be32(uint8_t* p, uint32_t value)
{
	p[0] = value >> 24;
	p[1] = value >> 16;
	p[2] = value >> 8;
	p[3] = value;
}

static void od_be64(uint8_t* p, uint64_t value)
{
	od_be32(p, value >> 32);
	od_be32(p + 4, value);
}

static bool od_add_jpeg(struct od_work* work, pixman_box32_t box)
{
	int width = box.x2 - box.x1, height = box.y2 - box.y1;
	if ((uint64_t)width * height > OD_JPEG_PIXELS) {
		pixman_box32_t second = box;
		if (width >= height) {
			box.x2 = box.x1 + width / 2;
			second.x1 = box.x2;
		} else {
			box.y2 = box.y1 + height / 2;
			second.y1 = box.y2;
		}
		return od_add_jpeg(work, box) && od_add_jpeg(work, second);
	}
	if (work->n_rects == OD_MAX_RECTS)
		return false;
	work->rectangles[work->n_rects++] = (struct od_rectangle){ .box = box };
	return true;
}

/* Disjoint guillotine free boxes grow by at most one per placement. All
 * placements and padded extents are 8-aligned; JPEG DCT origins never rotate. */
static bool od_plan_atlases(struct od_work* work)
{
	struct od_encoder* encoder = work->parent;
	struct od_atlas_plan* best = &encoder->atlas_plan;
	uint64_t source_area = 0;
	int count = 0;
	pixman_box32_t source = { 0 };
	uint32_t frame_width = (encoder->gpu.width + 7u) & ~7u;
	uint32_t frame_height = (encoder->gpu.height + 7u) & ~7u;
	for (int i = 0; i < work->n_rects; ++i) {
		const struct od_rectangle* rectangle = &work->rectangles[i];
		if (rectangle->solid)
			continue;
		encoder->atlas_order[count++] = i;
		if (count == 1) {
			source = rectangle->box;
		} else {
			if (rectangle->box.x1 < source.x1) source.x1 = rectangle->box.x1;
			if (rectangle->box.y1 < source.y1) source.y1 = rectangle->box.y1;
			if (rectangle->box.x2 > source.x2) source.x2 = rectangle->box.x2;
			if (rectangle->box.y2 > source.y2) source.y2 = rectangle->box.y2;
		}
		source_area += (uint64_t)(rectangle->box.x2 - rectangle->box.x1) *
			(rectangle->box.y2 - rectangle->box.y1);
	}
	memset(best, 0, sizeof(*best));
	if (count < 8 || !frame_width || !frame_height ||
			frame_width > 4096 || frame_height > 4096)
		return false;
	/* Dense content can keep source-relative placement, including wide/tall
	 * rectangles that formerly needed independent images. Holes still count
	 * against the same decoded-area budget; no source rectangles are split. */
	int origin_x = source.x1 & ~7, origin_y = source.y1 & ~7;
	unsigned dense_width = (source.x2 - origin_x + 7u) & ~7u;
	unsigned dense_height = (source.y2 - origin_y + 7u) & ~7u;
	bool aligned = true;
	for (int i = 0; i < count; ++i) {
		const pixman_box32_t* box = &work->rectangles[encoder->atlas_order[i]].box;
		if (((box->x1 - origin_x) & 7) || ((box->y1 - origin_y) & 7))
			aligned = false;
	}
	uint64_t dense_area = (uint64_t)dense_width * dense_height;
	if (aligned && dense_width <= frame_width && dense_height <= frame_height &&
			dense_area * 4 <= source_area * 5) {
		best->count = best->images = 1;
		best->area = dense_area;
		best->atlases[0] = (struct od_atlas){
			.width = dense_width, .height = dense_height,
			.count = count, .source = source,
		};
		for (int i = 0; i < work->n_rects; ++i) {
			const struct od_rectangle* rectangle = &work->rectangles[i];
			best->maps[i] = (struct od_atlas_map){
				.atlas = rectangle->solid ? -1 : 0,
				.x = rectangle->box.x1 - origin_x,
				.y = rectangle->box.y1 - origin_y,
			};
		}
	}
	const uint16_t widths[] = { frame_width, 1920, 1536, 1280, 1024, 768, 512, 256 };
	for (int order = 0; order < 3; ++order) {
		for (int i = 1; i < count; ++i) {
			int index = encoder->atlas_order[i], j = i;
			const pixman_box32_t* box = &work->rectangles[index].box;
			int width = box->x2 - box->x1, height = box->y2 - box->y1;
			int score = order == 0 ? width * height : order == 1 ? height : width;
			while (j > 0) {
				const pixman_box32_t* other =
					&work->rectangles[encoder->atlas_order[j - 1]].box;
				int ow = other->x2 - other->x1, oh = other->y2 - other->y1;
				int previous = order == 0 ? ow * oh : order == 1 ? oh : ow;
				if (previous >= score)
					break;
				encoder->atlas_order[j] = encoder->atlas_order[j - 1];
				--j;
			}
			encoder->atlas_order[j] = index;
		}
		for (unsigned candidate = 0; candidate < sizeof(widths) / sizeof(widths[0]); ++candidate) {
			unsigned bin_width = widths[candidate] < frame_width ? widths[candidate] : frame_width;
			struct od_atlas_plan* plan = &encoder->atlas_trial;
			memset(plan, 0, sizeof(*plan));
			for (int i = 0; i < OD_MAX_RECTS; ++i)
				plan->maps[i].atlas = -1;
			int n_free = 0;
			for (int next = 0; next < count; ++next) {
				int index = encoder->atlas_order[next];
				pixman_box32_t box = work->rectangles[index].box;
				unsigned width = (box.x2 - box.x1 + 7u) & ~7u;
				unsigned height = (box.y2 - box.y1 + 7u) & ~7u;
				if (width > bin_width || height > frame_height) {
					++plan->images;
					plan->area += (uint64_t)(box.x2 - box.x1) * (box.y2 - box.y1);
					continue;
				}
				int chosen = -1;
				unsigned waste = UINT_MAX;
				for (int i = 0; i < n_free; ++i) {
					const struct od_atlas_free* space = &encoder->atlas_free[i];
					if (width <= space->width && height <= space->height &&
							(unsigned)space->width * space->height - width * height < waste) {
						chosen = i;
						waste = (unsigned)space->width * space->height - width * height;
					}
				}
				if (chosen < 0) {
					chosen = n_free++;
					encoder->atlas_free[chosen] = (struct od_atlas_free){
						.width = bin_width, .height = frame_height,
						.atlas = plan->count++,
					};
					++plan->images;
				}
				struct od_atlas_free space = encoder->atlas_free[chosen];
				encoder->atlas_free[chosen] = encoder->atlas_free[--n_free];
				unsigned dw = space.width - width, dh = space.height - height;
				if (dw)
					encoder->atlas_free[n_free++] = (struct od_atlas_free){
						.x = space.x + width, .y = space.y, .width = dw,
						.height = dw > dh ? space.height : height, .atlas = space.atlas,
					};
				if (dh)
					encoder->atlas_free[n_free++] = (struct od_atlas_free){
						.x = space.x, .y = space.y + height,
						.width = dw > dh ? width : space.width,
						.height = dh, .atlas = space.atlas,
					};
				plan->maps[index] = (struct od_atlas_map){
					.atlas = space.atlas, .x = space.x, .y = space.y,
				};
				struct od_atlas* atlas = &plan->atlases[space.atlas];
				if (!atlas->count) {
					atlas->source = box;
				} else {
					if (box.x1 < atlas->source.x1) atlas->source.x1 = box.x1;
					if (box.y1 < atlas->source.y1) atlas->source.y1 = box.y1;
					if (box.x2 > atlas->source.x2) atlas->source.x2 = box.x2;
					if (box.y2 > atlas->source.y2) atlas->source.y2 = box.y2;
				}
				++atlas->count;
				if (space.x + width > atlas->width) atlas->width = space.x + width;
				if (space.y + height > atlas->height) atlas->height = space.y + height;
			}
			for (int i = 0; i < plan->count; ++i)
				plan->area += (uint64_t)plan->atlases[i].width * plan->atlases[i].height;
			if (plan->images * 2 > count || plan->area * 4 > source_area * 5)
				continue;
			if (!best->count || plan->images < best->images ||
					(plan->images == best->images && plan->area < best->area))
				*best = *plan;
		}
	}
	return best->count > 0;
}

static int od_atlas_init(struct od_work* work)
{
	struct od_gpu* gpu = &work->parent->gpu;
	uint32_t width = (gpu->width + 7u) & ~7u;
	uint32_t height = (gpu->height + 7u) & ~7u;
	if (gpu->atlas_bo && gbm_bo_get_width(gpu->atlas_bo) == width &&
			gbm_bo_get_height(gpu->atlas_bo) == height)
		return 0;
	GLint max_texture = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
	if (!width || !height || width > 4096 || height > 4096 ||
			width > (uint32_t)max_texture || height > (uint32_t)max_texture)
		return od_fail(work, "Padded capture exceeds GPU JPEG atlas limits");
	if (gpu->atlas_bo) {
		glDeleteFramebuffers(1, &gpu->atlas_fbo);
		glDeleteTextures(1, &gpu->atlas_texture);
		gpu->destroy_image(gpu->display, gpu->atlas_image);
		gbm_bo_destroy(gpu->atlas_bo);
		gpu->atlas_fbo = gpu->atlas_texture = 0;
		gpu->atlas_image = EGL_NO_IMAGE_KHR;
		gpu->atlas_bo = NULL;
	}
	gpu->atlas_bo = gbm_bo_create(gpu->gbm, width, height,
			GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!gpu->atlas_bo || gbm_bo_get_modifier(gpu->atlas_bo) != DRM_FORMAT_MOD_LINEAR)
		return od_fail(work, "Cannot allocate renderable linear RGBA8 JPEG atlas");
	int fd = gbm_bo_get_fd(gpu->atlas_bo);
	if (fd < 0)
		return od_fail(work, "Cannot export linear JPEG atlas: %s", strerror(errno));
	const EGLint attrs[] = {
		EGL_WIDTH, width, EGL_HEIGHT, height,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, gbm_bo_get_offset(gpu->atlas_bo, 0),
		EGL_DMA_BUF_PLANE0_PITCH_EXT, gbm_bo_get_stride(gpu->atlas_bo),
		EGL_NONE,
	};
	gpu->atlas_image = gpu->create_image(gpu->display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
	close(fd);
	if (gpu->atlas_image == EGL_NO_IMAGE_KHR)
		return od_fail(work, "Cannot import linear JPEG atlas (0x%x)", eglGetError());
	glGenTextures(1, &gpu->atlas_texture);
	glBindTexture(GL_TEXTURE_2D, gpu->atlas_texture);
	gpu->image_target(GL_TEXTURE_2D, (GLeglImageOES)gpu->atlas_image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &gpu->atlas_fbo);
	if (!gpu->source_fbo)
		glGenFramebuffers(1, &gpu->source_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gpu->atlas_fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, gpu->atlas_texture, 0);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
			glGetError() != GL_NO_ERROR)
		return od_fail(work, "Linear RGBA8 JPEG atlas is not GPU renderable");
	return 0;
}

/* DMA-BUF texture y=0 is memory row zero, as in od_dirty_region. Blit semantic
 * RGB (not raw DRM channels) into ABGR8888's little-endian TJPF_RGBA bytes. */
static int od_encode_atlas(struct od_work* work, struct nvnc_frame* frame,
		int index, unsigned char* payload, unsigned long* size)
{
	struct od_gpu* gpu = &work->parent->gpu;
	const struct od_atlas_plan* plan = &work->parent->atlas_plan;
	const struct od_atlas* atlas = &plan->atlases[index];
	struct od_binding saved;
	od_save_binding(&saved);
	int rc = -1;
	GLuint source;
	if (od_gpu_init(work, frame->buffer->bo) < 0 || od_atlas_init(work) < 0 ||
			od_import_image(work, frame->buffer->bo, &source) < 0)
		goto done;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu->source_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, source, 0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		od_fail(work, "Capture DMA-BUF is not GPU blit readable");
		goto done;
	}
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gpu->atlas_fbo);
	glDisable(GL_DITHER);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, atlas->width, atlas->height);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT); /* Holes must not expose previous pixels. */
	glDisable(GL_SCISSOR_TEST);
	for (int i = 0; i < work->n_rects; ++i) {
		const struct od_atlas_map* map = &plan->maps[i];
		if (map->atlas != index)
			continue;
		pixman_box32_t box = work->rectangles[i].box;
		int width = box.x2 - box.x1, height = box.y2 - box.y1;
		int right = map->x + width, bottom = map->y + height;
		int padded_right = map->x + ((width + 7) & ~7);
		int padded_bottom = map->y + ((height + 7) & ~7);
		glBlitFramebuffer(box.x1, box.y1, box.x2, box.y2,
				map->x, map->y, right, bottom, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		/* Match libjpeg-turbo's final-sample extension of partial DCT blocks.
		 * Always read source edges, never sample the atlas being written. */
		if (padded_right > right)
			glBlitFramebuffer(box.x2 - 1, box.y1, box.x2, box.y2,
					right, map->y, padded_right, bottom, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if (padded_bottom > bottom)
			glBlitFramebuffer(box.x1, box.y2 - 1, box.x2, box.y2,
					map->x, bottom, right, padded_bottom, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if (padded_right > right && padded_bottom > bottom)
			glBlitFramebuffer(box.x2 - 1, box.y2 - 1, box.x2, box.y2,
					right, bottom, padded_right, padded_bottom,
					GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	if (glGetError() != GL_NO_ERROR) {
		od_fail(work, "GPU JPEG atlas blit failed");
		goto done;
	}
	if (od_wait_gpu(work) < 0)
		goto done;
	uint32_t stride = 0;
	void* map_data = NULL;
	const uint8_t* pixels = gbm_bo_map(gpu->atlas_bo, 0, 0, atlas->width, atlas->height,
			GBM_BO_TRANSFER_READ, &stride, &map_data);
	if (!pixels || pixels == (void*)-1) {
		od_fail(work, "Cannot map used GPU JPEG atlas extent: %s", strerror(errno));
		goto done;
	}
	if (stride < (uint32_t)atlas->width * 4 || stride > INT_MAX) {
		od_fail(work, "GPU JPEG atlas mapping has unsupported pitch");
	} else {
		unsigned char* output = payload;
		if (tjCompress2(work->parent->jpeg, pixels, atlas->width, stride, atlas->height,
				TJPF_RGBA, &output, size, TJSAMP_444, 90, TJFLAG_NOREALLOC) < 0 ||
				output != payload)
			od_fail(work, "GPU atlas JPEG encoding failed: %s",
					tjGetErrorStr2(work->parent->jpeg));
		else
			rc = 0;
	}
	gbm_bo_unmap(gpu->atlas_bo, map_data);
done:
	if (gpu->source_fbo && gpu->context != EGL_NO_CONTEXT &&
			eglGetCurrentContext() == gpu->context) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu->source_fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	if (!od_restore_binding(gpu, &saved))
		rc = od_fail(work, "Cannot release/restore atlas worker EGL binding");
	return rc;
}

static int od_encode_regions(struct od_work* work, pixman_region32_t* region,
		pixman_region32_t* jpeg_region, struct nvnc_frame* frame, int format)
{
	int n_jpeg = 0;
	pixman_box32_t* boxes = pixman_region32_rectangles(jpeg_region, &n_jpeg);
	bool planned = work->n_rects >= 0;
	for (int i = 0; planned && i < n_jpeg; ++i)
		planned = od_add_jpeg(work, boxes[i]);
	if (!planned) {
		/* Fall back before emitting anything; baseline and wire remain atomic. */
		int count = 0;
		boxes = pixman_region32_rectangles(region, &count);
		uint64_t area = 0;
		for (int i = 0; i < count; ++i)
			area += (uint64_t)(boxes[i].x2 - boxes[i].x1) * (boxes[i].y2 - boxes[i].y1);
		work->n_rects = 0;
		planned = count <= OD_MAX_RECTS && area * 3 <= (uint64_t)frame->width * frame->height;
		for (int i = 0; planned && i < count; ++i)
			planned = od_add_jpeg(work, boxes[i]);
		if (!planned) {
			work->n_rects = 0;
			if (!od_add_jpeg(work, (pixman_box32_t){ 0, 0, frame->width, frame->height }))
				return od_fail(work, "Capture exceeds bounded JPEG region capacity");
		}
	}
	bool packed = od_plan_atlases(work);
	const struct od_atlas_plan* plan = &work->parent->atlas_plan;
	int atlas_count = packed ? plan->count : 0;
	size_t capacity = 0;
	bool has_jpeg = false;
	for (int i = 0; i < work->n_rects; ++i) {
		const struct od_rectangle* rectangle = &work->rectangles[i];
		if (packed && !rectangle->solid && plan->maps[i].atlas >= 0)
			continue;
		unsigned long bound = 4;
		if (!rectangle->solid) {
			has_jpeg = true;
			bound = tjBufSize(rectangle->box.x2 - rectangle->box.x1,
					rectangle->box.y2 - rectangle->box.y1, TJSAMP_444);
		}
		if (bound == (unsigned long)-1 || bound > SIZE_MAX - capacity - OD_RECT_HEADER)
			return od_fail(work, "Region allocation size overflow");
		capacity += OD_RECT_HEADER + bound;
	}
	for (int i = 0; i < atlas_count; ++i) {
		const struct od_atlas* atlas = &plan->atlases[i];
		unsigned long bound = tjBufSize(atlas->width, atlas->height, TJSAMP_444);
		size_t metadata = 8u + 12u * atlas->count;
		if (bound == (unsigned long)-1 ||
				bound > SIZE_MAX - capacity - OD_RECT_HEADER - metadata)
			return od_fail(work, "Atlas allocation size overflow");
		capacity += OD_RECT_HEADER + metadata + bound;
		has_jpeg = true;
	}
	uint8_t* packet = capacity ? malloc(capacity) : NULL;
	if (capacity && !packet)
		return od_fail(work, "Cannot allocate region packet");
	if (has_jpeg && !work->parent->jpeg)
		work->parent->jpeg = tjInitCompress();
	if (has_jpeg && !work->parent->jpeg) {
		free(packet);
		return od_fail(work, "Cannot initialize TurboJPEG compressor");
	}
	size_t length = 0;
	int records = 0;
	for (int i = 0; i < work->n_rects + atlas_count; ++i) {
		int atlas_index = i - work->n_rects;
		const struct od_atlas* atlas = atlas_index >= 0 ? &plan->atlases[atlas_index] : NULL;
		struct od_rectangle outer = { .box = atlas ? atlas->source : (pixman_box32_t){ 0 } };
		const struct od_rectangle* rectangle = atlas ? &outer : &work->rectangles[i];
		if (!atlas && packed && !rectangle->solid && plan->maps[i].atlas >= 0)
			continue;
		int x = rectangle->box.x1, y = rectangle->box.y1;
		int width = rectangle->box.x2 - x, height = rectangle->box.y2 - y;
		uint8_t* header = packet + length;
		unsigned char* payload = header + OD_RECT_HEADER;
		unsigned long payload_size = 4;
		if (rectangle->solid) {
			/* packUnorm4x8 stores R in the low byte; the wire is explicit RGBA. */
			for (int channel = 0; channel < 4; ++channel)
				payload[channel] = (rectangle->rgba >> (channel * 8)) & 255u;
		} else if (atlas) {
			od_be16(payload, atlas->width);
			od_be16(payload + 2, atlas->height);
			od_be16(payload + 4, atlas->count);
			od_be16(payload + 6, 0);
			size_t offset = 8;
			for (int source = 0; source < work->n_rects; ++source) {
				const struct od_atlas_map* map = &plan->maps[source];
				if (map->atlas != atlas_index)
					continue;
				pixman_box32_t box = work->rectangles[source].box;
				od_be16(payload + offset, box.x1);
				od_be16(payload + offset + 2, box.y1);
				od_be16(payload + offset + 4, box.x2 - box.x1);
				od_be16(payload + offset + 6, box.y2 - box.y1);
				od_be16(payload + offset + 8, map->x);
				od_be16(payload + offset + 10, map->y);
				offset += 12;
			}
			payload_size = capacity - length - OD_RECT_HEADER - offset;
			if (od_encode_atlas(work, frame, atlas_index, payload + offset, &payload_size) < 0) {
				free(packet);
				return -1;
			}
			payload_size += offset;
		} else {
			uint32_t stride = 0;
			void* map_data = NULL;
			const uint8_t* pixels = gbm_bo_map(frame->buffer->bo, x, y, width, height,
					GBM_BO_TRANSFER_READ, &stride, &map_data);
			if (!pixels || pixels == (void*)-1) {
				free(packet);
				return od_fail(work, "Cannot map changed GBM rectangle: %s", strerror(errno));
			}
			if (stride < (uint32_t)width * 4 || stride > INT_MAX) {
				gbm_bo_unmap(frame->buffer->bo, map_data);
				free(packet);
				return od_fail(work, "Cropped GBM mapping has unsupported pitch");
			}
			/* NOREALLOC requires the reserved tjBufSize worst case, not wire size. */
			payload_size = capacity - length - OD_RECT_HEADER;
			int rc = tjCompress2(work->parent->jpeg, pixels, width, stride, height,
					format, &payload, &payload_size, TJSAMP_444, 90, TJFLAG_NOREALLOC);
			gbm_bo_unmap(frame->buffer->bo, map_data);
			if (rc < 0) {
				od_fail(work, "Changed-region JPEG encoding failed: %s",
						tjGetErrorStr2(work->parent->jpeg));
				free(packet);
				return -1;
			}
		}
		if (payload != header + OD_RECT_HEADER || !payload_size ||
				payload_size > OD_MAX_PACKET - OD_RECT_HEADER ||
				length > OD_MAX_PACKET - OD_RECT_HEADER - payload_size) {
			free(packet);
			return od_fail(work, "Region frame exceeds the 32 MiB wire limit");
		}
		od_be16(header, x);
		od_be16(header + 2, y);
		od_be16(header + 4, width);
		od_be16(header + 6, height);
		od_be32(header + 8, RFB_ENCODING_OMARCHY_REGIONS);
		od_be64(header + 12, frame->pts);
		od_be32(header + 20, atlas ? 2u : rectangle->solid ? 1u : 0u);
		od_be32(header + 24, payload_size);
		length += OD_RECT_HEADER + payload_size;
		++records;
	}
	work->result = nvnc__encoded_frame_new(packet, length, records,
			frame->width, frame->height, frame->pts);
	if (!work->result) {
		free(packet);
		return od_fail(work, "Cannot allocate encoded region frame");
	}
	return 0;
}

static void od_do_work(struct aml_work* aml_work)
{
	struct od_work* work = aml_get_userdata(aml_work);
	struct nvnc_frame* frame = work->fb.fbs[0];
	int format = od_pixel_format(gbm_bo_get_format(frame->buffer->bo));
	pixman_region32_t region, jpeg;
	pixman_region32_init(&region);
	pixman_region32_init(&jpeg);
	if (od_dirty_region(work, frame->buffer->bo, &region, &jpeg) == 0)
		od_encode_regions(work, &region, &jpeg, frame, format);
	pixman_region32_fini(&jpeg);
	pixman_region32_fini(&region);
	/* Failure cannot leave a pending RFB request indefinitely unanswered. */
	if (!work->result) {
		od_gpu_destroy(&work->parent->gpu);
		if (work->parent->jpeg) {
			tjDestroy(work->parent->jpeg);
			work->parent->jpeg = NULL;
		}
	}
}

/* Only the main loop retires neatvnc's non-atomic references. A successful
 * completion does this before AML's worker can become the final job owner.
 * Undelivered/cancelled jobs stay loop-owned until the worker barrier joins
 * workers and retires work refs; allocation/start failures retire on the loop. */
static void od_work_retire(struct od_work* work)
{
	if (!work->parent)
		return;
	struct od_encoder* parent = work->parent;
	if (work->result)
		encoded_frame_unref(work->result);
	work->result = NULL;
	nvnc_composite_fb_unref(&work->fb);
	work->parent = NULL;
	encoder_unref(&parent->encoder);
}

static void od_on_done(struct aml_work* aml_work)
{
	struct od_work* work = aml_get_userdata(aml_work);
	struct od_encoder* self = work->parent;
	self->busy = false;
	if (!work->result) {
		nvnc_log(NVNC_LOG_ERROR, "ODR3 capture failed closed: %s",
				work->error[0] ? work->error : "native region encoder failure");
		fflush(stderr);
		od_work_retire(work);
		_exit(EXIT_FAILURE);
	}
	/* Same borrowed-metadata convention as neatvnc's raw/tight encoders:
	 * composite_fb holds it throughout the completion callback. Socket writes
	 * retain the independently reference-counted encoded_frame payload. */
	work->result->metadata = work->fb.metadata;
	encoder_finish_frame(&self->encoder, work->result);
	od_work_retire(work);
}

static void od_work_destroy(void* data)
{
	struct od_work* work = data;
	/* After normal completion only this allocation can die on a worker.
	 * Shutdown retires undelivered jobs on the owner after joining workers. */
	od_work_retire(work);
	free(work);
}

static int od_encode(struct encoder* encoder, struct nvnc_composite_fb* fb,
		struct pixman_region16* damage)
{
	(void)damage; /* Damage is deliberately not a CPU comparison input. */
	struct od_encoder* self = (struct od_encoder*)encoder;
	if (self->busy || fb->n_fbs != 1)
		goto invalid;
	struct nvnc_frame* frame = fb->fbs[0];
	if (!frame || !frame->buffer || frame->buffer->type != NVNC_BUFFER_GBM_BO ||
			!frame->buffer->bo || frame->x_off || frame->y_off ||
			frame->transform != NVNC_TRANSFORM_NORMAL || !frame->width || !frame->height ||
			(frame->logical_width && frame->logical_width != frame->width) ||
			(frame->logical_height && frame->logical_height != frame->height) ||
			gbm_bo_get_width(frame->buffer->bo) != frame->width ||
			gbm_bo_get_height(frame->buffer->bo) != frame->height ||
			frame->pts == NVNC_NO_PTS ||
			od_pixel_format(gbm_bo_get_format(frame->buffer->bo)) == TJPF_UNKNOWN)
		goto invalid;
	struct od_work* work = calloc(1, sizeof(*work));
	if (!work)
		return -1;
	work->parent = self;
	work->generation = self->generation;
	nvnc_composite_fb_copy(&work->fb, fb);
	encoder_ref(encoder);
	struct aml_work* job = aml_work_new(od_do_work, od_on_done, work, od_work_destroy);
	if (!job) {
		od_work_destroy(work);
		return -1;
	}
	self->busy = true;
	int rc = aml_start(aml_get_default(), job);
	if (rc < 0)
		self->busy = false;
	/* AML's started/event references keep an undelivered job loop-owned.
	 * Both capture executables join and retire those refs before teardown. */
	aml_unref(job);
	return rc;
invalid:
	nvnc_log(NVNC_LOG_ERROR, "ODR3 requires one unscaled, untransformed, timestamped RGB8888 GBM capture at origin; concurrent jobs are forbidden");
	return -1;
}

static void od_refresh(struct encoder* encoder)
{
	struct od_encoder* self = (struct od_encoder*)encoder;
	/* An in-flight job has a snapshot, so it cannot consume a later reset. */
	++self->generation;
	if (!self->generation)
		++self->generation;
}

static void od_destroy(struct encoder* encoder)
{
	struct od_encoder* self = (struct od_encoder*)encoder;
	/* Completion retires the job reference on the loop; undelivered jobs
	 * retire on the owner inside the explicit join-and-retire barrier. */
	od_gpu_destroy(&self->gpu);
	if (self->jpeg)
		tjDestroy(self->jpeg);
	free(self);
}

struct encoder* od_regions_new(uint16_t width, uint16_t height)
{
	if (!width || !height)
		return NULL;
	struct od_encoder* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;
	encoder_init(&self->encoder, &encoder_impl_od_regions);
	self->generation = 1;
	self->gpu.fd = -1;
	return &self->encoder;
}

struct encoder_impl encoder_impl_od_regions = {
	.flags = ENCODER_IMPL_FLAG_IGNORES_DAMAGE,
	.destroy = od_destroy,
	.encode = od_encode,
	.request_key_frame = od_refresh,
	.reset = od_refresh,
};
