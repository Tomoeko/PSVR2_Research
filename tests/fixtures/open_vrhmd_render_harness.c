#include "../../target/psvr2/tools/open_vrhmd/open_vrhmd.h"
#include <assert.h>
#include <sys/socket.h>

GLuint g_pbos[PBO_COUNT];
int g_pbo_write_idx, g_pbo_read_idx;
sem_t g_sem_free, g_sem_full;
pthread_t g_mirror_thread;
_Atomic bool g_mirror_running;
int g_stream_fd = -1;
uint8_t *g_vhdr_packets[PBO_COUNT];
volatile sig_atomic_t g_bench_stop;
volatile sig_atomic_t application_stop_requested;

static void check_matrix(const float *got, const float *want) {
  for (int i = 0; i < 16; i++)
    assert(fabsf(got[i] - want[i]) < 0.00002f);
}
static void reference_mul(const float *a, const float *b, float *out) {
  float tmp[16] = {0};
  for (int row = 0; row < 4; row++)
    for (int col = 0; col < 4; col++)
      for (int k = 0; k < 4; k++)
        tmp[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
  memcpy(out, tmp, sizeof(tmp));
}
struct reader_data { int fd; uint8_t *dst; size_t size; };
static void *drain(void *user) {
  struct reader_data *r = user;
  size_t offset = 0;
  while (offset < r->size) {
    ssize_t count = read(r->fd, r->dst + offset,
                         r->size - offset > 997 ? 997 : r->size - offset);
    assert(count > 0);
    offset += (size_t)count;
  }
  return NULL;
}
static void open_pair(int pair[2]) {
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  g_stream_fd = pair[0];
  int sendbuf = 4096;
  assert(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sendbuf, sizeof(sendbuf)) == 0);
  assert(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
}
static int active_plane, uploads;
static void active(GLenum unit) { active_plane = unit - 0x84C0; }
static void bind_texture(GLenum target, GLuint id) {
  assert(target == 0x0DE1 && id == (GLuint)(active_plane + 11));
}
static void upload(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                   GLsizei h, GLenum format, GLenum type, const void *pixels) {
  assert(target==0x0DE1 && level==0 && x==0 && y==0 && format==0x1909 && type==0x1401);
  assert(w==(active_plane ? 264 : 528));
  assert(h==(active_plane ? 200 : 400));
  assert(pixels);
  uploads++;
}
static void test_matrices(void) {
  for (int iteration = 0; iteration < 500; iteration++) {
    float a[16], b[16], expect[16], got[16], alias[16];
    for (int i = 0; i < 16; i++) {
      a[i] = sinf(iteration * 0.37f + i);
      b[i] = cosf(iteration * 0.25f - i);
    }
    reference_mul(a, b, expect);
    m4_mul(a, b, got);
    check_matrix(got, expect);
    memcpy(alias, a, sizeof(alias));
    m4_mul(alias, b, alias);
    check_matrix(alias, expect);
    memcpy(alias, b, sizeof(alias));
    m4_mul(a, alias, alias);
    check_matrix(alias, expect);
  }
  /* The optimized model must retain the original X * Y rotation order. */
  for (int frame = 0; frame < 20; frame++) {
    float t = frame * 0.73f;
    for (int gx = 0; gx < 5; gx++) for (int gz = 0; gz < 5; gz++) {
      float ry[16], rx[16], scale[16], tr[16], tmp[16], tmp2[16], ref[16], fast[16];
      float spin = t * (1.0f + (gx * 5 + gz) * 0.08f);
      m4_roty(ry, spin); m4_rotx(rx, spin * 0.7f); m4_id(scale);
      scale[0] = scale[5] = scale[10] = 0.32f;
      m4_trans(tr, (gx - 2.0f) * 1.2f, sinf(t * 2.0f + gx + gz) * 0.2f,
                 (gz - 2.0f) * 1.2f);
      m4_mul(ry, scale, tmp); m4_mul(rx, tmp, tmp2); m4_mul(tr, tmp2, ref);
      m4_mul(rx, ry, fast);
      for (int i = 0; i < 12; i++) fast[i] *= 0.32f;
      memcpy(fast + 12, tr + 12, 4 * sizeof(float));
      check_matrix(fast, ref);
    }
  }
}

static void test_colors(void) {
  uint8_t r0,g0,b0,r,g,b;
  for (int h = 0; h < 360; h++) {
    hsv2rgb(h,255,255,&r0,&g0,&b0);
    hsv2rgb(h+360,255,255,&r,&g,&b);
    assert(r==r0 && g==g0 && b==b0);
    hsv2rgb(h-360,255,255,&r,&g,&b);
    assert(r==r0 && g==g0 && b==b0);
  }
}

static void test_video_planes(void) {
  assert(media_path_is_mpeg("video.mpg"));
  assert(media_path_is_mpeg("video.MPEG"));
  assert(media_path_is_mpeg("video.m1v"));
  assert(media_path_is_mpeg("video.MPV"));
  assert(!media_path_is_mpeg("video.bin") && !media_path_is_mpeg(NULL));
  uint8_t pixel = 0;
  Psvr2MpegFrame padded_frame = {.width=513, .height=385,
      .y={.width=528,.height=400,.stride=528,.data=&pixel},
      .cb={.width=264,.height=200,.stride=264,.data=&pixel},
      .cr={.width=264,.height=200,.stride=264,.data=&pixel}};
  struct mpeg_cb_data cb = {.texY=11,.texU=12,.texV=13,
      .gBT=bind_texture,.gTexSubImage2D=upload,.gA=active};
  assert(on_video_frame(&padded_frame,&cb));
  assert(uploads==3 && active_plane==0);
  padded_frame.y.stride++;
  assert(!on_video_frame(&padded_frame,&cb) && uploads==3);
  padded_frame.y.stride--;
  padded_frame.cr.data = NULL;
  assert(!on_video_frame(&padded_frame,&cb) && uploads==3);
  padded_frame.cr.data = &pixel;
  padded_frame.width = 529;
  assert(!on_video_frame(&padded_frame,&cb) && uploads==3);
  padded_frame.width = 513;
  padded_frame.time = NAN;
  assert(!on_video_frame(&padded_frame,&cb) && uploads==3);
}

static void test_png_rows(void) {
  uint8_t pixels[24], expected[24];
  for (size_t i=0; i<sizeof(pixels); i++) pixels[i]=(uint8_t)i;
  memcpy(expected, pixels+16, 8);
  memcpy(expected+8, pixels+8, 8);
  memcpy(expected+16, pixels, 8);
  Psvr2PngImage image = {.width=2, .height=3, .pixels=pixels};
  assert(media_flip_png_rows(&image));
  assert(memcmp(pixels, expected, sizeof(pixels))==0);
  assert(media_flip_png_rows(&image));
  for (size_t i=0; i<sizeof(pixels); i++) assert(pixels[i]==i);
  image.width = 0;
  assert(!media_flip_png_rows(&image));
  assert(!media_flip_png_rows(NULL));
}

static void test_stream(void) {
  const size_t size = MIRROR_FRAME_BYTES + sizeof(struct vrh2_header);
  uint8_t *src = malloc(size), *dst = malloc(size);
  assert(src && dst);
  for (size_t i = 0; i < size; i++) src[i] = (uint8_t)(i * 31);
  int pair[2]; open_pair(pair);
  struct reader_data reader = {pair[1], dst, size};
  pthread_t thread;
  assert(pthread_create(&thread, NULL, drain, &reader) == 0);
  assert(mirrorscope_write_packet(src, size, NULL) == 1);
  assert(pthread_join(thread,NULL) == 0);
  assert(memcmp(src,dst,size) == 0);
  close(pair[0]); close(pair[1]);

  /* A partial packet stalls: fail instead of starting a new header. */
  open_pair(pair);
  assert(mirrorscope_write_packet(src,size,NULL) == -1 && errno == ETIMEDOUT);
  /* The now-full stream may drop a frame before its first byte. */
  assert(mirrorscope_write_packet(src,size,NULL) == 0);
  _Atomic bool stopped = false;
  assert(mirrorscope_write_packet(src,size,&stopped) == -1 && errno == ECANCELED);
  close(pair[0]); close(pair[1]);
  g_stream_fd=-1;
  assert(mirrorscope_write_packet(src,size,NULL) == -1 && errno == EBADF);
  free(src);free(dst);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  if (!strcmp(argv[1], "matrices"))
    test_matrices();
  else if (!strcmp(argv[1], "colors"))
    test_colors();
  else if (!strcmp(argv[1], "video"))
    test_video_planes();
  else if (!strcmp(argv[1], "png"))
    test_png_rows();
  else {
    assert(!strcmp(argv[1], "stream"));
    test_stream();
  }
  puts("render/stream check passed");
  return 0;
}
