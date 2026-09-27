#include "../open_vrhmd.h"

#ifdef GPU_RENDER

void (*gRP)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

void bench_sigint(int s) {
  (void)s;
  g_bench_stop = 1;
  application_stop_requested = 1;
}

/* ES2 has no row-length unpack setting. Storage width must therefore equal
 * the row pitch; validate all planes before touching any texture. */
bool video_frame_valid(const Psvr2MpegFrame *frame) {
  if (!frame || !frame->width || !frame->height ||
      frame->width > INT32_MAX || frame->height > INT32_MAX ||
      !isfinite(frame->time) || frame->time < 0)
    return false;
  const Psvr2MpegPlane *planes[] = {&frame->y, &frame->cb, &frame->cr};
  for (int plane = 0; plane < 3; plane++) {
    unsigned visible_w = plane ? (frame->width + 1) / 2 : frame->width;
    unsigned visible_h = plane ? (frame->height + 1) / 2 : frame->height;
    if (!planes[plane]->data || !planes[plane]->width || !planes[plane]->height ||
        planes[plane]->width > INT32_MAX || planes[plane]->height > INT32_MAX ||
        planes[plane]->stride != planes[plane]->width ||
        visible_w > planes[plane]->width || visible_h > planes[plane]->height)
      return false;
  }
  return frame->y.width == frame->cb.width * 2 &&
         frame->y.height == frame->cb.height * 2 &&
         frame->cb.width == frame->cr.width &&
         frame->cb.height == frame->cr.height;
}

bool on_video_frame(const Psvr2MpegFrame *frame, struct mpeg_cb_data *cb) {
  if (!video_frame_valid(frame) || !cb || !cb->gA || !cb->gBT ||
      !cb->gTexSubImage2D)
    return false;
  const Psvr2MpegPlane *planes[] = {&frame->y, &frame->cb, &frame->cr};
  const GLuint textures[] = {cb->texY, cb->texU, cb->texV};
  for (int plane = 0; plane < 3; plane++) {
    cb->gA(0x84C0 + plane); /* GL_TEXTURE0 + plane */
    cb->gBT(0x0DE1, textures[plane]);
    cb->gTexSubImage2D(0x0DE1, 0, 0, 0, planes[plane]->width,
                       planes[plane]->height, 0x1909, 0x1401,
                       planes[plane]->data);
  }
  cb->gA(0x84C0);
  return true;
}

#endif /* GPU_RENDER */
