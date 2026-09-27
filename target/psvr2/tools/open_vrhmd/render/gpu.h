#ifndef OPEN_VRHMD_GPU_H
#define OPEN_VRHMD_GPU_H

#ifdef GPU_RENDER
#include <dlfcn.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "../codec/mpeg.h"
#include "../codec/png.h"
#include "../codec/jpeg.h"

/* --- Local GLES/EGL types for dlopen usage --- */
typedef void *ED;
typedef void *EC;
typedef void *EX;
typedef void *EI;
typedef unsigned int EB;
typedef int EGLint;
typedef unsigned int EU;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef unsigned int GLbitfield;
typedef long GLintptr;
typedef long GLsizeiptr;

/* Mirrorscope PBO Threading State */
#define PBO_COUNT 3
extern GLuint g_pbos[PBO_COUNT];
extern int g_pbo_write_idx;
extern int g_pbo_read_idx;
extern sem_t g_sem_free;
extern sem_t g_sem_full;
extern pthread_t g_mirror_thread;
extern _Atomic bool g_mirror_running;

extern const char *g_image_path;
extern const char *g_video_path;
extern int g_stream;
extern int g_stream_fd;
extern uint8_t *g_vhdr_packets[PBO_COUNT];


#define MIRROR_WIDTH 1000
#define MIRROR_HEIGHT 510
#define MIRROR_FRAME_BYTES ((size_t)MIRROR_WIDTH * MIRROR_HEIGHT * 3)
#define MONO_VIDEO_WIDTH 512
#define MONO_VIDEO_HEIGHT 384
#define MONO_VIDEO_FRAME_BYTES ((size_t)MONO_VIDEO_WIDTH * MONO_VIDEO_HEIGHT / 8)

/* render/matrix.c */
extern const float cube_v[324];
void m4_id(float *m);
void m4_mul(const float *a, const float *b, float *o);
void m4_frustum(float *m, float left, float right, float bottom, float top,
                float near, float far);
void m4_trans(float *m, float x, float y, float z);
void m4_roty(float *m, float a);
void m4_rotx(float *m, float a);

/* stream/video.c */
struct vrh2_header {
  uint32_t magic; /* 'VRH2' bytes, little-endian 0x32485256 */
  uint32_t frame_no;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t format; /* 0 = RGB888, 1 = AR30, 2 = Grayscale */
  uint32_t size;
  uint32_t reserved;
};

#pragma pack(push, 1)
struct optical_calib {
  float leftX, leftY, rightX, rightY;
  float leftZ, rightZ;
  float leftDispX, leftDispY, leftDispZ;
  float rightDispX, rightDispY, rightDispZ;
  float leftSetX, leftSetY, leftSetZ;
  float rightSetX, rightSetY, rightSetZ;
};
#pragma pack(pop)

extern volatile sig_atomic_t g_bench_stop;

struct mpeg_cb_data {
  GLuint texY, texU, texV;
  void (*gBT)(GLenum, GLuint);
  void (*gTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                         GLenum, const void *);
  void (*gA)(GLenum);
};

void bench_sigint(int s);
extern void (*gRP)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
bool media_path_is_mpeg(const char *path);
bool video_frame_valid(const Psvr2MpegFrame *frame);
bool on_video_frame(const Psvr2MpegFrame *frame, struct mpeg_cb_data *cb);
/* RGBA byte order is retained; only top/bottom row order changes for GL. */
bool media_flip_png_rows(Psvr2PngImage *image);
/* 1: complete; 0: skipped before first byte; -1: stop using the stream. */
int mirrorscope_write_packet(const void *data, size_t size,
                             const _Atomic bool *running);
void *mirrorscope_stream_thread(void *arg);

/* render/gpu_app.c */
int gpu_app_loop(int fb_share_fd);
int gpu_render_gradient(int framebuffer_fd);


_Static_assert(sizeof(struct vrh2_header) == 32, "VRH2 header size");
#endif /* GPU_RENDER */
#endif
