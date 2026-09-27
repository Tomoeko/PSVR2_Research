#include "../open_vrhmd.h"

#ifdef GPU_RENDER
#include <strings.h>

/* EGL resources are released through one path while the context is current. */
int gpu_app_loop(int fb_share_fd) {
  int result = -1;
  ED dpy = NULL;
  EX ctx = NULL;
  EI eimg = NULL;
  int egl_initialized = 0, context_current = 0;
  int free_sem_initialized = 0, full_sem_initialized = 0;
  int stream_thread_started = 0;
  GLuint rbo = 0, drbo = 0, fbo = 0;
  GLuint mirror_fbo = 0, mirror_tex = 0, vertex_buffer = 0;
  GLuint vsh = 0, fsh = 0, prog = 0;
  GLuint tex = 0, texY = 0, texU = 0, texV = 0;
  uint8_t *video_mmap = NULL, *video_frame_buf = NULL;
  size_t video_size = 0, video_total_frames = 0;
  size_t capture_pixel_bytes = 4;
  size_t video_last_frame = SIZE_MAX;
  Psvr2MpegDecoder *mpeg = NULL;
  Psvr2MpegFrame mpeg_pending = {0};
  int mpeg_has_pending = 0;
  unsigned mpeg_w = 0, mpeg_h = 0, mpeg_plane_w = 0, mpeg_plane_h = 0;
  double mpeg_display_time = -1;
  int old_fl = -1;
  struct sigaction old_sigint, old_sigterm;
  int sigint_installed = 0, sigterm_installed = 0;
  int frames = 0, total_frames = 0;

  void *he = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  void *hg = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
  if (!he || !hg) {
    ERR("dlopen: %s", dlerror());
    goto unload;
  }

  /* EGL */
  ED (*eGetDisp)(void *) = dlsym(he, "eglGetDisplay");
  EB (*eInit)(ED, EGLint *, EGLint *) = dlsym(he, "eglInitialize");
  EB(*eChoose)
  (ED, const EGLint *, EC *, EGLint, EGLint *) = dlsym(he, "eglChooseConfig");
  EX (*eCtx)(ED, EC, EX, const EGLint *) = dlsym(he, "eglCreateContext");
  EB (*eMake)(ED, void *, void *, EX) = dlsym(he, "eglMakeCurrent");
  EB (*eBind)(EU) = dlsym(he, "eglBindAPI");
  EGLint (*eErr)(void) = dlsym(he, "eglGetError");
  EB (*eDestCtx)(ED, EX) = dlsym(he, "eglDestroyContext");
  EB (*eTerm)(ED) = dlsym(he, "eglTerminate");
  void *(*eProc)(const char *) = dlsym(he, "eglGetProcAddress");
  if (!eGetDisp || !eInit || !eChoose || !eCtx || !eMake || !eBind ||
      !eErr || !eDestCtx || !eTerm || !eProc) {
    LOG("Missing required EGL entry points");
    goto unload;
  }
  EI(*eCrImg)
  (ED, EX, EU, void *, const EGLint *) = (void *)eProc("eglCreateImageKHR");
  EB (*eDeImg)(ED, EI) = (void *)eProc("eglDestroyImageKHR");
  void (*gImgRBS)(GLenum, EI) =
      (void *)eProc("glEGLImageTargetRenderbufferStorageOES");

  /* GLES */
  void (*gGenRB)(GLsizei, GLuint *) = dlsym(hg, "glGenRenderbuffers");
  void (*gBindRB)(GLenum, GLuint) = dlsym(hg, "glBindRenderbuffer");
  void (*gRBStor)(GLenum, GLenum, GLsizei, GLsizei) =
      dlsym(hg, "glRenderbufferStorage");
  void (*gGenFB)(GLsizei, GLuint *) = dlsym(hg, "glGenFramebuffers");
  void (*gBindFB)(GLenum, GLuint) = dlsym(hg, "glBindFramebuffer");
  void (*gFBRB)(GLenum, GLenum, GLenum, GLuint) =
      dlsym(hg, "glFramebufferRenderbuffer");
  GLenum (*gChkFB)(GLenum) = dlsym(hg, "glCheckFramebufferStatus");
  void (*gVP)(GLint, GLint, GLsizei, GLsizei) = dlsym(hg, "glViewport");
  void (*gCC)(float, float, float, float) = dlsym(hg, "glClearColor");
  void (*gClr)(unsigned int) = dlsym(hg, "glClear");
  void (*gEn)(GLenum) = dlsym(hg, "glEnable");
  void (*gDF)(GLenum) = dlsym(hg, "glDepthFunc");
  GLuint (*gCrSh)(GLenum) = dlsym(hg, "glCreateShader");
  void (*gShSrc)(GLuint, GLsizei, const GLchar *const *, const GLint *) =
      dlsym(hg, "glShaderSource");
  void (*gComp)(GLuint) = dlsym(hg, "glCompileShader");
  void (*gGShiv)(GLuint, GLenum, GLint *) = dlsym(hg, "glGetShaderiv");
  void (*gShLog)(GLuint, GLsizei, GLsizei *, GLchar *) =
      dlsym(hg, "glGetShaderInfoLog");
  GLuint (*gCrPr)(void) = dlsym(hg, "glCreateProgram");
  void (*gAttSh)(GLuint, GLuint) = dlsym(hg, "glAttachShader");
  void (*gLink)(GLuint) = dlsym(hg, "glLinkProgram");
  void (*gGPiv)(GLuint, GLenum, GLint *) = dlsym(hg, "glGetProgramiv");
  void (*gUse)(GLuint) = dlsym(hg, "glUseProgram");
  GLint (*gGAL)(GLuint, const GLchar *) = dlsym(hg, "glGetAttribLocation");
  GLint (*gGUL)(GLuint, const GLchar *) = dlsym(hg, "glGetUniformLocation");
  void (*gVAP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) =
      dlsym(hg, "glVertexAttribPointer");
  void (*gEnVA)(GLuint) = dlsym(hg, "glEnableVertexAttribArray");
  void (*gDraw)(GLenum, GLint, GLsizei) = dlsym(hg, "glDrawArrays");
  void (*gFin)(void) = dlsym(hg, "glFinish");
  void (*gU4fv)(GLint, GLsizei, GLboolean, const float *) =
      dlsym(hg, "glUniformMatrix4fv");
  void (*gU3f)(GLint, float, float, float) = dlsym(hg, "glUniform3f");
  void (*gU1i)(GLint, GLint) = dlsym(hg, "glUniform1i");
  void (*gU2f)(GLint, float, float) = dlsym(hg, "glUniform2f");
  void (*gA)(GLenum) = dlsym(hg, "glActiveTexture");
  void (*gPSi)(GLenum, GLint) = dlsym(hg, "glPixelStorei");
  void (*gGIv)(GLenum, GLint *) = dlsym(hg, "glGetIntegerv");
  GLenum (*gErr)(void) = dlsym(hg, "glGetError");

  void (*gDelSh)(GLuint) = dlsym(hg, "glDeleteShader");
  void (*gDelPr)(GLuint) = dlsym(hg, "glDeleteProgram");
  void (*gDelFB)(GLsizei, const GLuint *) = dlsym(hg, "glDeleteFramebuffers");
  void (*gDelRB)(GLsizei, const GLuint *) = dlsym(hg, "glDeleteRenderbuffers");
  void (*gGens)(GLsizei, GLuint *) = dlsym(hg, "glGenTextures");
  void (*gDelTex)(GLsizei, const GLuint *) = dlsym(hg, "glDeleteTextures");
  void (*gBT)(GLenum, GLuint) = dlsym(hg, "glBindTexture");
  void (*gTexImg2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                    GLenum, const void *) = dlsym(hg, "glTexImage2D");
  void (*gTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                         GLenum, const void *) = dlsym(hg, "glTexSubImage2D");
  void (*gTexParami)(GLenum, GLenum, GLint) = dlsym(hg, "glTexParameteri");
  void (*gFBT)(GLenum, GLenum, GLenum, GLuint, GLint) = dlsym(hg, "glFramebufferTexture2D");
  void (*gBlit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = dlsym(hg, "glBlitFramebuffer");

  /* PBO / Mapping Support (GLES 3.0+) */
  void (*gGenBufs)(GLsizei, GLuint *) = dlsym(hg, "glGenBuffers");
  void (*gBindBuf)(GLenum, GLuint) = dlsym(hg, "glBindBuffer");
  void (*gBufData)(GLenum, GLsizeiptr, const void *, GLenum) = dlsym(hg, "glBufferData");
  void *(*gMapBuf)(GLenum, GLintptr, GLsizeiptr, GLbitfield) = dlsym(hg, "glMapBufferRange");
  GLboolean (*gUnmapBuf)(GLenum) = dlsym(hg, "glUnmapBuffer");
  void (*gDelBufs)(GLsizei, const GLuint *) = dlsym(hg, "glDeleteBuffers");

  if (!eCrImg || !eDeImg || !gImgRBS || !gGenRB || !gBindRB ||
      !gRBStor || !gGenFB || !gBindFB || !gFBRB || !gChkFB || !gVP ||
      !gCC || !gClr || !gEn || !gDF || !gCrSh || !gShSrc || !gComp ||
      !gGShiv || !gShLog || !gCrPr || !gAttSh || !gLink || !gGPiv ||
      !gUse || !gGAL || !gGUL || !gVAP || !gEnVA || !gDraw || !gFin ||
      !gU4fv || !gU3f || !gU1i || !gU2f || !gA || !gPSi || !gGIv || !gErr ||
      !gDelSh || !gDelPr || !gDelFB || !gDelRB || !gGens || !gDelTex ||
      !gBT || !gTexImg2D || !gTexSubImage2D || !gTexParami ||
      !gGenBufs || !gBindBuf || !gBufData || !gDelBufs) {
    LOG("Missing required EGL/GLES entry points");
    goto unload;
  }

  /* Prefer ES 3 for the optional framebuffer blit/PBO capture path. */
  int stream_capable = g_stream && gFBT && gBlit && gGenBufs && gBindBuf &&
                       gBufData && gMapBuf && gUnmapBuf && gDelBufs;
  if (!eBind(0x30A0) || !(dpy = eGetDisp(NULL)) ||
      !eInit(dpy, NULL, NULL)) {
    LOG("EGL initialization failed: 0x%x", eErr());
    goto cleanup;
  }
  egl_initialized = 1;
  EGLint ca[] = {0x3040, stream_capable ? 0x40 : 4, 0x3033, 0, 0x3024, 8, 0x3023, 8,
                       0x3022, 8, 0x3038};
  EC ecfg = NULL;
  EGLint nc = 0;
  if (!eChoose(dpy, ca, &ecfg, 1, &nc) || nc < 1) {
    stream_capable = 0;
    ca[1] = 4; /* EGL_OPENGL_ES2_BIT */
    if (!eChoose(dpy, ca, &ecfg, 1, &nc) || nc < 1) {
      LOG("No compatible EGL configuration: 0x%x", eErr());
      goto cleanup;
    }
  }
  EGLint xa[] = {0x3098, stream_capable ? 3 : 2, 0x3038};
  ctx = eCtx(dpy, ecfg, NULL, xa);
  if (!ctx && stream_capable) {
    stream_capable = 0;
    xa[1] = 2;
    ctx = eCtx(dpy, ecfg, NULL, xa);
  }
  if (!ctx || !eMake(dpy, NULL, NULL, ctx)) {
    LOG("EGL context creation/binding failed: 0x%x", eErr());
    goto cleanup;
  }
  context_current = 1;

  /* EGL image from ION dmabuf */
  EGLint ia[] = {0x3057,     4000,   0x3056,      2040,   0x3271,
                 0x34324742, 0x3272, fb_share_fd, 0x3273, 0,
                 0x3274,     12000,  0x3038};
  eimg = eCrImg(dpy, 0, 0x3270, 0, ia);
  if (!eimg) {
    ia[5] = 0x34324752;
    eimg = eCrImg(dpy, 0, 0x3270, 0, ia);
  }
  if (!eimg) {
    ERR("eglCreateImageKHR: 0x%x", eErr());
    goto cleanup;
  }

  /* FBO: color from EGL image + depth renderbuffer */
  gGenRB(1, &rbo);
  gBindRB(0x8D41, rbo);
  gImgRBS(0x8D41, eimg);
  gGenRB(1, &drbo);
  gBindRB(0x8D41, drbo);
  gRBStor(0x8D41, 0x81A5 /* GL_DEPTH_COMPONENT16 */, 4000, 2040);
  gGenFB(1, &fbo);
  gBindFB(0x8D40, fbo);
  gFBRB(0x8D40, 0x8CE0, 0x8D41, rbo); /* color */
  gFBRB(0x8D40, 0x8D00 /* GL_DEPTH_ATTACHMENT */, 0x8D41, drbo);
  if (gChkFB(0x8D40) != 0x8CD5) {
    ERR("FBO incomplete: 0x%x", gChkFB(0x8D40));
    goto cleanup;
  }

  /* Compile 3D shader */
  const char *vs = "attribute vec3 aPos;\n"
                   "attribute vec3 aNorm;\n"
                   "attribute vec3 aCol;\n"
                   "uniform mat4 uMVP;\n"
                   "uniform mat4 uModel;\n"
                   "varying vec3 vN, vC, vW;\n"
                   "void main(){\n"
                   "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
                   "  vN = mat3(uModel) * aNorm;\n"
                   "  vW = (uModel * vec4(aPos, 1.0)).xyz;\n"
                   "  vC = aCol;\n"
                   "}\n";
  const char *fs =
      "precision mediump float;\n"
      "varying vec3 vN, vC, vW;\n"
      "uniform vec3 uEye;\n"
      "void main(){\n"
      "  vec3 N = normalize(vN);\n"
      "  vec3 L = normalize(vec3(1.0, 2.0, 1.5));\n"
      "  vec3 V = normalize(uEye - vW);\n"
      "  vec3 H = normalize(L + V);\n"
      "  float diff = max(dot(N, L), 0.0);\n"
      "  float spec = pow(max(dot(N, H), 0.0), 32.0);\n"
      "  vec3 col = vC * (0.12 + diff * 0.78) + vec3(1.0) * spec * 0.4;\n"
      "  gl_FragColor = vec4(col, 1.0);\n"
      "}\n";

  const char *vs_img = "attribute vec3 aPos;\n"
                       "attribute vec2 aUv;\n"
                       "uniform mat4 uMVP;\n"
                       "varying vec2 vUv;\n"
                       "void main(){\n"
                       "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
                       "  vUv = aUv;\n"
                       "}\n";
  static const char *fs_yuv =
      "precision highp float;\n"
      "varying vec2 vUv;\n"
      "uniform sampler2D uTexY;\n"
      "uniform sampler2D uTexU;\n"
      "uniform sampler2D uTexV;\n"
      "uniform vec2 uLensCenter;\n"
      "uniform vec2 uPlaneScale;\n"
      "uniform vec2 uChromaScale;\n"
      "vec2 distort(vec2 uv, float k1, float k2) {\n"
      "  vec2 t = (uv - uLensCenter) * 2.0;\n"
      "  float r2 = dot(t, t);\n"
      "  float d = 1.0 + k1*r2 + k2*r2*r2;\n"
      "  return (t * d * 0.5) + uLensCenter;\n"
      "}\n"
      "void main() {\n"
      "  vec2 uvR = distort(vUv, 0.032, 0.022);\n"
      "  vec2 uvG = distort(vUv, 0.030, 0.020);\n"
      "  vec2 uvB = distort(vUv, 0.028, 0.018);\n"
      "  /* Use Green channel for primary clipping and chroma */\n"
      "  if (uvG.x < 0.01 || uvG.x > 0.99 || uvG.y < 0.01 || uvG.y > 0.99) {\n"
      "    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
      "    return;\n"
      "  }\n"
      "  /* Sample Luma at three positions for CA correction */\n"
      "  float yG = (texture2D(uTexY, vec2(uvG.x, 1.0 - uvG.y) * uPlaneScale).r - (16.0/255.0)) * 1.16438;\n"
      "  float yR = (texture2D(uTexY, vec2(uvR.x, 1.0 - uvR.y) * uPlaneScale).r - (16.0/255.0)) * 1.16438;\n"
      "  float yB = (texture2D(uTexY, vec2(uvB.x, 1.0 - uvB.y) * uPlaneScale).r - (16.0/255.0)) * 1.16438;\n"
      "  /* Chroma at Green center */\n"
      "  float u_raw = texture2D(uTexU, vec2(uvG.x, 1.0 - uvG.y) * uChromaScale).r - (128.0/255.0);\n"
      "  float v_raw = texture2D(uTexV, vec2(uvG.x, 1.0 - uvG.y) * uChromaScale).r - (128.0/255.0);\n"
      "  float u = abs(u_raw) < 0.05 ? 0.0 : u_raw;\n"
      "  float v = abs(v_raw) < 0.05 ? 0.0 : v_raw;\n"
      "  float r = yR + 1.59603 * v;\n"
      "  float g = yG - 0.39176 * u - 0.81297 * v;\n"
      "  float b = yB + 2.01723 * u;\n"
      "  gl_FragColor = vec4(r, g, b, 1.0);\n"
      "}\n";

  const char *fs_img = "precision mediump float;\n"
                       "varying vec2 vUv;\n"
                       "uniform sampler2D uTex;\n"
                       "void main(){\n"
                       "  vec2 uv = (vUv - 0.5) * 2.0;\n"
                       "  float r2 = uv.x*uv.x + uv.y*uv.y;\n"
                       "  float k1 = 0.03;\n"
                       "  float k2 = 0.02;\n"
                       "  float dist = 1.0 + k1*r2 + k2*r2*r2;\n"
                       "  vec2 uv_dist = (uv * dist * 0.5) + 0.5;\n"
                       "  if (uv_dist.x < 0.0 || uv_dist.x > 1.0 || uv_dist.y "
                       "< 0.0 || uv_dist.y > 1.0) {\n"
                       "    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
                       "  } else {\n"
                       "    vec4 t = texture2D(uTex, uv_dist);\n"
                       "    gl_FragColor = vec4(t.b, t.g, t.r, t.a);\n"
                       "  }\n"
                       "}\n";

  const char *active_vs = (g_image_path || g_video_path) ? vs_img : vs;
  const int is_mpg = media_path_is_mpeg(g_video_path);
  const char *active_fs =
      g_image_path ? fs_img : (g_video_path ? (is_mpg ? fs_yuv : fs_img) : fs);

  const char *shader_sources[] = {active_vs, active_fs};
  const GLenum shader_types[] = {0x8B31, 0x8B30};
  GLuint *shader_ids[] = {&vsh, &fsh};
  GLint ok = 0;
  for (int shader = 0; shader < 2; shader++) {
    GLuint id = gCrSh(shader_types[shader]);
    *shader_ids[shader] = id;
    if (!id) {
      LOG("Cannot allocate %s shader", shader ? "fragment" : "vertex");
      goto cleanup;
    }
    gShSrc(id, 1, &shader_sources[shader], NULL);
    gComp(id);
    gGShiv(id, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (!ok) {
      char log[512] = {0};
      gShLog(id, sizeof(log), NULL, log);
      LOG("%s shader compilation failed: %s", shader ? "Fragment" : "Vertex", log);
      goto cleanup;
    }
  }
  prog = gCrPr();
  if (!prog) {
    LOG("Cannot allocate shader program");
    goto cleanup;
  }
  gAttSh(prog, vsh);
  gAttSh(prog, fsh);
  gLink(prog);
  gGPiv(prog, 0x8B82, &ok);
  if (!ok) {
    ERR("Link failed");
    goto cleanup;
  }
  gUse(prog);

  GLint aP = gGAL(prog, "aPos");
  GLint aN = (g_image_path || g_video_path) ? -1 : gGAL(prog, "aNorm");
  GLint aC = (g_image_path || g_video_path) ? gGAL(prog, "aUv")
                                            : gGAL(prog, "aCol"); // Repurpose aC for UV
  GLint uMVP = gGUL(prog, "uMVP");
  GLint uMod = (g_image_path || g_video_path) ? -1 : gGUL(prog, "uModel");
  GLint uEye = (g_image_path || g_video_path) ? -1 : gGUL(prog, "uEye");
  GLint uTex = g_video_path && is_mpg ? -1 : gGUL(prog, "uTex");
  GLint uTexY = g_video_path && is_mpg ? gGUL(prog, "uTexY") : -1;
  GLint uTexU = g_video_path && is_mpg ? gGUL(prog, "uTexU") : -1;
  GLint uTexV = g_video_path && is_mpg ? gGUL(prog, "uTexV") : -1;
  GLint uLensCenter = g_video_path && is_mpg ? gGUL(prog, "uLensCenter") : -1;
  GLint uPlaneScale = g_video_path && is_mpg ? gGUL(prog, "uPlaneScale") : -1;
  GLint uChromaScale = g_video_path && is_mpg ? gGUL(prog, "uChromaScale") : -1;
  if (aP < 0 || aC < 0 || uMVP < 0 ||
      (!(g_image_path || g_video_path) && (aN < 0 || uMod < 0 || uEye < 0))) {
    LOG("Shader is missing required attributes/uniforms");
    goto cleanup;
  }

  if (uTex != -1)
    gU1i(uTex, 0);
  if (uTexY != -1)
    gU1i(uTexY, 0);
  if (uTexU != -1)
    gU1i(uTexU, 1);
  if (uTexV != -1)
    gU1i(uTexV, 2);

  gRP = dlsym(hg, "glReadPixels");

  gPSi(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 1);
  gPSi(0x0D05 /* GL_PACK_ALIGNMENT */, 1);

  int img_w = 0, img_h = 0;
  GLint max_texture_size = 0;
  gGIv(0x0D33 /* GL_MAX_TEXTURE_SIZE */, &max_texture_size);
  if (max_texture_size <= 0) {
    LOG("Invalid GPU texture-size limit");
    goto cleanup;
  }
  float asp = 1.0f;
  if (g_image_path) {
    LOG("Loading image: %s", g_image_path);
    Psvr2PngImage image = {0};
    if (!psvr2_png_load(g_image_path, &image) &&
        !psvr2_jpeg_load(g_image_path, &image)) {
      LOG("Failed to decode PNG/JPEG image: %s", g_image_path);
      goto cleanup;
    }
    if (image.width > (unsigned)max_texture_size ||
        image.height > (unsigned)max_texture_size || !media_flip_png_rows(&image)) {
      LOG("Image exceeds the GPU texture-size limit or has invalid pixels");
      psvr2_png_free(&image);
      goto cleanup;
    }
    img_w = (int)image.width;
    img_h = (int)image.height;
    asp = (float)img_w / (float)img_h;
    gGens(1, &tex);
    gBT(0x0DE1 /* GL_TEXTURE_2D */, tex);
    gTexParami(0x0DE1, 0x2801 /* GL_TEXTURE_MIN_FILTER */,
               0x2601 /* GL_LINEAR */);
    gTexParami(0x0DE1, 0x2800 /* GL_TEXTURE_MAG_FILTER */,
               0x2601 /* GL_LINEAR */);
    gTexImg2D(0x0DE1, 0, 0x1908 /* GL_RGBA */, img_w, img_h, 0,
              0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, image.pixels);
    psvr2_png_free(&image);
  }

  if (g_video_path) {
    if (is_mpg) {
      char decoder_error[192] = {0};
      LOG("Loading MPEG-1 video: %s", g_video_path);
      mpeg = psvr2_mpeg_open(g_video_path, decoder_error, sizeof(decoder_error));
      if (!mpeg) {
        LOG("MPEG open failed: %s", decoder_error);
        goto cleanup;
      }
      int decoded = psvr2_mpeg_read_frame(mpeg, &mpeg_pending);
      if (decoded != 1 || !video_frame_valid(&mpeg_pending)) {
        LOG("MPEG has no valid initial frame: %s", psvr2_mpeg_error(mpeg));
        goto cleanup;
      }
      mpeg_has_pending = 1;
      mpeg_w = mpeg_pending.width;
      mpeg_h = mpeg_pending.height;
      mpeg_plane_w = mpeg_pending.y.width;
      mpeg_plane_h = mpeg_pending.y.height;
      if (mpeg_plane_w > (unsigned)max_texture_size ||
          mpeg_plane_h > (unsigned)max_texture_size) {
        LOG("MPEG padded frame exceeds the GPU texture-size limit");
        goto cleanup;
      }
      asp = (float)mpeg_w / mpeg_h;
      gU2f(uPlaneScale, (float)mpeg_w / mpeg_plane_w,
            (float)mpeg_h / mpeg_plane_h);
      gU2f(uChromaScale, (float)((mpeg_w + 1) / 2) / mpeg_pending.cb.width,
            (float)((mpeg_h + 1) / 2) / mpeg_pending.cb.height);
      LOG("  MPEG: %ux%u, %.3f fps (video only)", mpeg_w, mpeg_h,
          psvr2_mpeg_frame_rate(mpeg));

      GLuint planes[3] = {0};
      gGens(3, planes);
      texY = planes[0]; texU = planes[1]; texV = planes[2];
      for (int plane = 0; plane < 3; plane++) {
        int divisor = plane ? 2 : 1;
        GLint filter = plane ? 0x2600 /* GL_NEAREST */ : 0x2601 /* GL_LINEAR */;
        gBT(0x0DE1, planes[plane]);
        gTexParami(0x0DE1, 0x2801, filter);
        gTexParami(0x0DE1, 0x2800, filter);
        gTexParami(0x0DE1, 0x2802, 0x812F /* GL_CLAMP_TO_EDGE */);
        gTexParami(0x0DE1, 0x2803, 0x812F);
        gTexImg2D(0x0DE1, 0, 0x1909, mpeg_plane_w / divisor,
                   mpeg_plane_h / divisor, 0, 0x1909, 0x1401, NULL);
      }
    } else {
      int vid_fd = open(g_video_path, O_RDONLY);
      if (vid_fd < 0) {
        ERR("Failed to open video %s", g_video_path);
        goto cleanup;
      }
      struct stat st;
      if (fstat(vid_fd, &st) < 0 || st.st_size <= 0 ||
          (uintmax_t)st.st_size > SIZE_MAX) {
        LOG("Video file is empty or unavailable: %s", g_video_path);
        close(vid_fd);
        goto cleanup;
      }
      video_size = (size_t)st.st_size;
      video_mmap = mmap(NULL, video_size, PROT_READ, MAP_PRIVATE, vid_fd, 0);
      close(vid_fd);
      if (video_mmap == MAP_FAILED) {
        video_mmap = NULL;
        ERR("Failed to mmap video");
        goto cleanup;
      }
      LOG("Loading 1-bit video bitstream: %s", g_video_path);
      if (video_size % MONO_VIDEO_FRAME_BYTES != 0) {
        LOG("1-bit video must contain complete %zu-byte frames",
            MONO_VIDEO_FRAME_BYTES);
        goto cleanup;
      }
      video_total_frames = video_size / MONO_VIDEO_FRAME_BYTES;
      video_frame_buf = malloc(MONO_VIDEO_WIDTH * MONO_VIDEO_HEIGHT);
      if (!video_frame_buf) {
        ERR("Allocating 1-bit video frame");
        goto cleanup;
      }
      asp = 4.0f / 3.0f;
      gGens(1, &tex);
      gBT(0x0DE1, tex);
      gTexParami(0x0DE1, 0x2801, 0x2601);
      gTexParami(0x0DE1, 0x2800, 0x2601);
      gTexImg2D(0x0DE1, 0, 0x1909, 512, 384, 0, 0x1909, 0x1401, NULL);
    }
  }

  gEn(0x0B71 /* GL_DEPTH_TEST */);
  gDF(0x0201 /* GL_LESS */);
  gCC(0.02f, 0.02f, 0.06f, 1.0f); /* dark blue-black background */

  if (g_image_path) {
    LOG("=== 2D IMAGE VR: Rendering %dx%d image with proper aspect ratio ===",
        img_w, img_h);
  } else if (is_mpg) {
    LOG("=== MPEG-1 VIDEO VR ===");
  } else if (g_video_path) {
    LOG("=== 1-BIT VIDEO VR: Rendering 512x384 Bitstream (%zu frames) ===",
        video_total_frames);
  } else {
    LOG("=== 3D BENCH: Rendering 5×5 cube grid with proper VR calibration ===");
  }
  LOG("  GPU: PowerVR GE8430, Resolution: 4000×2040 (stereo)");
  LOG("  Press Ctrl+C or type 'stop' then Enter on stdin to end.");

  /* Load exact factory calibration from /data/optical_calib */
  struct optical_calib cal = {0};

  /* Fallback to typical PSVR2 logs if file missing */
  cal.leftX = 2.716f;
  cal.leftY = -2.439f;
  cal.leftZ = 0.165f;
  cal.rightX = 0.244f;
  cal.rightY = -0.110f;
  cal.rightZ = -0.187f;

  int cal_fd = open("/data/optical_calib", O_RDONLY);
  if (cal_fd >= 0) {
    struct optical_calib loaded;
    ssize_t bytes;
    do {
      bytes = pread(cal_fd, &loaded, sizeof(loaded), 32);
    } while (bytes < 0 && errno == EINTR);
    if (bytes == sizeof(loaded) && isfinite(loaded.leftX) &&
        isfinite(loaded.leftY) && isfinite(loaded.rightX) &&
        isfinite(loaded.rightY)) {
      cal = loaded;
      LOG("  Loaded factory optical calibration: L(%.3f, %.3f) R(%.3f, %.3f)",
          cal.leftX, cal.leftY, cal.rightX, cal.rightY);
    } else {
      LOG("  Optical calibration is incomplete or invalid; using defaults.");
    }
    close(cal_fd);
  } else {
    LOG("  /data/optical_calib not found, using defaults.");
  }

  if (g_stream && (!stream_capable || !gRP))
    LOG("Mirrorscope requires OpenGL ES 3 framebuffer/PBO support; disabled.");
  if (stream_capable && gRP) {
    auto_takeover(4);
    g_stream_fd = open("/dev/fast_stream", O_WRONLY | O_NONBLOCK);
    if (g_stream_fd < 0) {
      ERR("Opening /dev/fast_stream; Mirrorscope disabled");
    } else {
      gGens(1, &mirror_tex);
      gBT(0x0DE1, mirror_tex);
      gTexParami(0x0DE1, 0x2801, 0x2601);
      gTexParami(0x0DE1, 0x2800, 0x2601);
      gTexImg2D(0x0DE1, 0, 0x1907, MIRROR_WIDTH, MIRROR_HEIGHT, 0,
                 0x1907, 0x1401, NULL);
      gGenFB(1, &mirror_fbo);
      gBindFB(0x8D40, mirror_fbo);
      gFBT(0x8D40, 0x8CE0, 0x0DE1, mirror_tex, 0);
      if (gChkFB(0x8D40) != 0x8CD5) {
        LOG("Mirrorscope framebuffer is incomplete");
        goto cleanup;
      }
      GLint read_format, read_type;
      gGIv(0x8B9B /* GL_IMPLEMENTATION_COLOR_READ_FORMAT */, &read_format);
      gGIv(0x8B9A /* GL_IMPLEMENTATION_COLOR_READ_TYPE */, &read_type);
      if (read_format == 0x1907 && read_type == 0x1401)
        capture_pixel_bytes = 3;
      gBindFB(0x8D40, fbo);
      gGenBufs(PBO_COUNT, g_pbos);
      for (int i = 0; i < PBO_COUNT; i++) {
        gBindBuf(0x88EB, g_pbos[i]);
        gBufData(0x88EB, MIRROR_WIDTH * MIRROR_HEIGHT * capture_pixel_bytes,
                   NULL, 0x88E9);
        g_vhdr_packets[i] = malloc(sizeof(struct vrh2_header) + MIRROR_FRAME_BYTES);
        if (!g_vhdr_packets[i]) {
          ERR("Allocating Mirrorscope packet");
          goto cleanup;
        }
      }
      gBindBuf(0x88EB, 0);
      if (sem_init(&g_sem_free, 0, PBO_COUNT) < 0)
        goto cleanup;
      free_sem_initialized = 1;
      if (sem_init(&g_sem_full, 0, 0) < 0)
        goto cleanup;
      full_sem_initialized = 1;
      g_pbo_write_idx = g_pbo_read_idx = 0;
      g_mirror_running = true;
      int thread_error = pthread_create(&g_mirror_thread, NULL,
                                        mirrorscope_stream_thread, NULL);
      if (thread_error) {
        errno = thread_error;
        ERR("Starting Mirrorscope thread");
        goto cleanup;
      }
      stream_thread_started = 1;
      LOG("Mirrorscope active via /dev/fast_stream");
    }
  }

  /* Benchmark render loop */
  struct timespec t0, tnow, tlast;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  tlast = t0;
  const float ipd_half = 0.032f; /* 32mm half IPD for stereo */
  const int media_mode = g_image_path || g_video_path;
  const float cam_y = media_mode ? 0.0f : 12.0f;
  const float cam_z = media_mode ? fmaxf(asp, 1.0f) * 1.25f : 0.0f;
  const float lens_centers[2][2] = {
      {1108.0f + cal.leftX, 1019.0f + cal.leftY},
      {892.0f + cal.rightX, 1019.0f + cal.rightY}};
  float eye_vp[2][16];
  for (int eye = 0; eye < 2; eye++) {
    const float cx = lens_centers[eye][0], cy = lens_centers[eye][1];
    const float near = 0.1f, pixel_scale = near / 1020.0f;
    float proj[16], view_trans[16], view[16];
    m4_frustum(proj, -cx * pixel_scale, (2000.0f - cx) * pixel_scale,
               -(2040.0f - cy) * pixel_scale, cy * pixel_scale, near, 100.0f);
    m4_trans(view_trans, eye ? -ipd_half : ipd_half, -cam_y, -cam_z);
    if (media_mode) {
      memcpy(view, view_trans, sizeof(view));
    } else {
      float rx[16];
      m4_rotx(rx, 1.5707963267948966f);
      m4_mul(rx, view_trans, view);
    }
    m4_mul(proj, view, eye_vp[eye]);
  }
  const float quad_v[] = {
      -asp, -1.0f, 0, 0, 0, asp, -1.0f, 0, 1, 0,
      -asp, 1.0f, 0, 0, 1, asp, 1.0f, 0, 1, 1};
  /* VBOs also make the same vertex path valid in an ES 3 context. */
  gGenBufs(1, &vertex_buffer);
  if (!vertex_buffer) {
    LOG("Cannot allocate vertex buffer");
    goto cleanup;
  }
  gBindBuf(0x8892 /* GL_ARRAY_BUFFER */, vertex_buffer);
  gBufData(0x8892, media_mode ? sizeof(quad_v) : sizeof(cube_v),
            media_mode ? quad_v : cube_v, 0x88E4 /* GL_STATIC_DRAW */);
  gVAP(aP, 3, 0x1406, 0, media_mode ? 20 : 36, NULL);
  gVAP(aC, media_mode ? 2 : 3, 0x1406, 0, media_mode ? 20 : 36,
        (const void *)(uintptr_t)((media_mode ? 3 : 6) * sizeof(float)));
  gEnVA(aP);
  gEnVA(aC);
  if (!media_mode) {
    gVAP(aN, 3, 0x1406, 0, 36, (const void *)(uintptr_t)(3 * sizeof(float)));
    gEnVA(aN);
  }
  float models[25][16];
  GLenum setup_error = gErr();
  if (setup_error) {
    LOG("GPU resource setup failed: GL error 0x%x", setup_error);
    goto cleanup;
  }

  /* SIGINT handler for Ctrl+C clean shutdown */
  g_bench_stop = application_stop_requested;
  struct sigaction stop_action = {0};
  stop_action.sa_handler = bench_sigint;
  sigemptyset(&stop_action.sa_mask);
  if (sigaction(SIGINT, &stop_action, &old_sigint) < 0) {
    ERR("Installing SIGINT handler");
    goto cleanup;
  }
  sigint_installed = 1;
  if (sigaction(SIGTERM, &stop_action, &old_sigterm) < 0) {
    ERR("Installing SIGTERM handler");
    goto cleanup;
  }
  sigterm_installed = 1;

  /* Make stdin non-blocking so read() doesn't hang */
  old_fl = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (old_fl >= 0 && fcntl(STDIN_FILENO, F_SETFL, old_fl | O_NONBLOCK) < 0)
    old_fl = -1;

  /* Line buffer for stop detection */
  char stopbuf[64];
  int stoplen = 0;
  int stdin_open = old_fl >= 0;

  struct mpeg_cb_data cb_data = {
      .texY = texY,
      .texU = texU,
      .texV = texV,
      .gBT = gBT,
      .gTexSubImage2D = gTexSubImage2D,
      .gA = gA};

  while (!g_bench_stop && !application_stop_requested) {
    clock_gettime(CLOCK_MONOTONIC, &tnow);
    float t = (tnow.tv_sec - t0.tv_sec) + (tnow.tv_nsec - t0.tv_nsec) * 1e-9f;

    gClr(0x4100); /* GL_COLOR|GL_DEPTH */

    /* Compute animation once per frame; both eyes share each model. */
    if (!media_mode) {
      for (int gx = 0; gx < 5; gx++) {
        for (int gz = 0; gz < 5; gz++) {
          float spin = t * (1.0f + (gx * 5 + gz) * 0.08f);
          float ry[16], rx[16], rotation[16];
          m4_roty(ry, spin);
          m4_rotx(rx, spin * 0.7f);
          m4_mul(rx, ry, rotation);
          /* Scale rotation columns and set translation directly. */
          float *model = models[gx * 5 + gz];
          memcpy(model, rotation, sizeof(rotation));
          for (int i = 0; i < 12; i++)
            model[i] *= 0.32f;
          model[12] = (gx - 2.0f) * 1.2f;
          model[13] = sinf(t * 2.0f + gx + gz) * 0.2f;
          model[14] = (gz - 2.0f) * 1.2f;
        }
      }
    }

    for (int eye = 0; eye < 2; eye++) {
      const float eye_off = eye ? ipd_half : -ipd_half;
      const float *vp = eye_vp[eye];
      gVP(eye * 2000, 0, 2000, 2040);
      if (uLensCenter != -1)
        gU2f(uLensCenter, lens_centers[eye][0] / 2000.0f,
              lens_centers[eye][1] / 2040.0f);

      if (g_video_path && eye == 0) {
        if (is_mpg) {
          /* Keep one display-order frame pending. Decode only after its bytes
           * have been copied to GL, and limit catch-up work per render loop. */
          for (int catchup = 0; mpeg_has_pending && catchup < 8 &&
               !g_bench_stop && !application_stop_requested &&
               mpeg_pending.time <= (double)t; catchup++) {
            if (mpeg_pending.width != mpeg_w || mpeg_pending.height != mpeg_h ||
                mpeg_pending.y.width != mpeg_plane_w ||
                mpeg_pending.y.height != mpeg_plane_h ||
                mpeg_pending.time < mpeg_display_time ||
                !on_video_frame(&mpeg_pending, &cb_data)) {
              LOG("MPEG frame dimensions, pitch or timestamp changed unexpectedly");
              goto cleanup;
            }
            mpeg_display_time = mpeg_pending.time;
            int decoded = psvr2_mpeg_read_frame(mpeg, &mpeg_pending);
            if (decoded < 0 || (decoded == 1 && !video_frame_valid(&mpeg_pending))) {
              LOG("MPEG decode failed or returned an invalid frame: %s",
                  psvr2_mpeg_error(mpeg));
              goto cleanup;
            }
            mpeg_has_pending = decoded == 1;
            if (!mpeg_has_pending)
              LOG("MPEG playback complete; holding the last frame");
          }
        } else {
          /* Update 1-bit video texture... */
          size_t target_frame = (size_t)(t * 30.0) % video_total_frames;
          if (target_frame != video_last_frame) {
            video_last_frame = target_frame;
            uint8_t *src = video_mmap + (target_frame * MONO_VIDEO_FRAME_BYTES);
            for (size_t i = 0; i < MONO_VIDEO_FRAME_BYTES; i++) {
              uint8_t b = src[i];
              for (int bit = 0; bit < 8; bit++) {
                video_frame_buf[i * 8 + bit] = ((b >> (7 - bit)) & 1) ? 0 : 255;
              }
            }
            gBT(0x0DE1, tex);
            gTexSubImage2D(0x0DE1, 0, 0, 0, 512, 384, 0x1909, 0x1401,
                           video_frame_buf);
          }
        }
      }

      if (media_mode) {
        if (is_mpg) {
          gA(0x84C0);
          gBT(0x0DE1, texY);
          gA(0x84C1);
          gBT(0x0DE1, texU);
          gA(0x84C2);
          gBT(0x0DE1, texV);
          gA(0x84C0);
        } else {
          gBT(0x0DE1, tex);
        }
        gU4fv(uMVP, 1, 0, vp);
        gDraw(0x0005 /* GL_TRIANGLE_STRIP */, 0, 4);
      } else {
        gU3f(uEye, eye_off, cam_y, cam_z);
        for (int cube = 0; cube < 25; cube++) {
          float mvp[16];
          m4_mul(vp, models[cube], mvp);
          gU4fv(uMVP, 1, 0, mvp);
          gU4fv(uMod, 1, 0, models[cube]);
          gDraw(0x0004 /* GL_TRIANGLES */, 0, 36);
        }
      }
    }

    gFin();

    if (stream_thread_started && g_mirror_running &&
        sem_trywait(&g_sem_free) == 0) {
      int idx = g_pbo_write_idx;
      gBindFB(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);
      gBindFB(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, mirror_fbo);
      gBlit(0, 0, 4000, 2040, 0, 0, MIRROR_WIDTH, MIRROR_HEIGHT,
            0x00004000 /* GL_COLOR_BUFFER_BIT */, 0x2601 /* GL_LINEAR */);
      gBindFB(0x8D40, mirror_fbo);
      gBindBuf(0x88EB /* GL_PIXEL_PACK_BUFFER */, g_pbos[idx]);
      /* RGBA/UNSIGNED_BYTE is guaranteed; use RGB only when the driver reports
       * that exact implementation read format/type. Pack away alpha if needed. */
      gRP(0, 0, MIRROR_WIDTH, MIRROR_HEIGHT,
          capture_pixel_bytes == 3 ? 0x1907 /* GL_RGB */ : 0x1908 /* GL_RGBA */,
          0x1401 /* GL_UNSIGNED_BYTE */, NULL);

      /* Mapping waits for readback; only publish a successfully copied slot. */
      void *ptr = gErr() == 0
          ? gMapBuf(0x88EB, 0,
                    MIRROR_WIDTH * MIRROR_HEIGHT * capture_pixel_bytes, 0x0001)
          : NULL;
      int captured = 0;
      if (ptr) {
        struct vrh2_header *hdr = (struct vrh2_header *)g_vhdr_packets[idx];
        *hdr = (struct vrh2_header){
            .magic = 0x32485256, .frame_no = (uint32_t)total_frames,
            .width = MIRROR_WIDTH, .height = MIRROR_HEIGHT,
            .pitch = MIRROR_WIDTH * 3, .format = 0,
            .size = MIRROR_FRAME_BYTES};
        uint8_t *payload = g_vhdr_packets[idx] + sizeof(*hdr);
        if (capture_pixel_bytes == 3) {
          memcpy(payload, ptr, MIRROR_FRAME_BYTES);
        } else {
          const uint8_t *rgba = ptr;
          for (size_t pixel = 0; pixel < MIRROR_WIDTH * MIRROR_HEIGHT; pixel++) {
            payload[pixel * 3] = rgba[pixel * 4];
            payload[pixel * 3 + 1] = rgba[pixel * 4 + 1];
            payload[pixel * 3 + 2] = rgba[pixel * 4 + 2];
          }
        }
        captured = gUnmapBuf(0x88EB);
      }
      gBindBuf(0x88EB, 0);
      gBindFB(0x8D40, fbo);
      if (captured) {
        g_pbo_write_idx = (idx + 1) % PBO_COUNT;
        sem_post(&g_sem_full);
      } else {
        sem_post(&g_sem_free);
      }
    }

    frames++;
    total_frames++;

    /* FPS report every 2 seconds */
    float elapsed =
        (tnow.tv_sec - tlast.tv_sec) + (tnow.tv_nsec - tlast.tv_nsec) * 1e-9f;
    if (elapsed >= 2.0f) {
      if (g_video_path) {
        float fps = frames / elapsed;
        LOG("  [VIDEO] %.1f FPS", fps);
      } else if (!g_image_path) {
        float fps = frames / elapsed;
        float total_t =
            (tnow.tv_sec - t0.tv_sec) + (tnow.tv_nsec - t0.tv_nsec) * 1e-9f;
        LOG("  [BENCH] %d frames in %.1fs = %.1f FPS  (total: %d frames, "
            "%.1fs)",
            frames, elapsed, fps, total_frames, total_t);
        int ncubes = 50; /* 5×5×2 eyes */
        LOG("         %d cubes x 12 tris x 2 eyes = %d tris/frame, %.0f "
            "tris/sec",
            ncubes / 2, ncubes * 12, fps * ncubes * 12);
      }
      frames = 0;
      tlast = tnow;
    }

    if (stdin_open) {
      struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
      int ready = poll(&input, 1, 0);
      if (ready > 0 && (input.revents & (POLLIN | POLLHUP))) {
        char bytes[64];
        ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (!count)
          stdin_open = 0;
        for (ssize_t i = 0; i < count; i++) {
          if (bytes[i] == '\n') {
            stopbuf[stoplen] = '\0';
            if (strcmp(stopbuf, "stop") == 0)
              g_bench_stop = 1;
            stoplen = 0;
          } else if (stoplen < (int)sizeof(stopbuf) - 1) {
            stopbuf[stoplen++] = bytes[i];
          }
        }
      } else if ((ready < 0 && errno != EINTR) ||
                 (ready > 0 && (input.revents & (POLLERR | POLLNVAL)))) {
        stdin_open = 0;
      }
    }
  }

  result = 0;
  LOG("=== RENDER COMPLETE: %d total frames ===", total_frames);

cleanup:
  if (old_fl >= 0)
    fcntl(STDIN_FILENO, F_SETFL, old_fl);

  /* Stop the consumer before freeing its queue or closing the device. */
  g_mirror_running = false;
  if (stream_thread_started) {
    sem_post(&g_sem_full);
    pthread_join(g_mirror_thread, NULL);
  }
  if (g_stream_fd >= 0) {
    close(g_stream_fd);
    g_stream_fd = -1;
  }
  if (free_sem_initialized)
    sem_destroy(&g_sem_free);
  if (full_sem_initialized)
    sem_destroy(&g_sem_full);
  for (int i = 0; i < PBO_COUNT; i++) {
    free(g_vhdr_packets[i]);
    g_vhdr_packets[i] = NULL;
  }
  if (mpeg)
    psvr2_mpeg_close(mpeg);
  if (video_mmap)
    munmap(video_mmap, video_size);
  free(video_frame_buf);

  if (context_current) {
    if (gDelBufs)
      gDelBufs(PBO_COUNT, g_pbos);
    if (vertex_buffer)
      gDelBufs(1, &vertex_buffer);
    memset(g_pbos, 0, sizeof(g_pbos));
    const GLuint textures[] = {tex, texY, texU, texV, mirror_tex};
    gDelTex(5, textures);
    if (vsh) gDelSh(vsh);
    if (fsh) gDelSh(fsh);
    if (prog) gDelPr(prog);
    gBindFB(0x8D40, 0);
    if (mirror_fbo) gDelFB(1, &mirror_fbo);
    if (fbo) gDelFB(1, &fbo);
    if (rbo) gDelRB(1, &rbo);
    if (drbo) gDelRB(1, &drbo);
    if (eimg) eDeImg(dpy, eimg);
    eMake(dpy, NULL, NULL, NULL);
  }
  if (ctx)
    eDestCtx(dpy, ctx);
  if (egl_initialized)
    eTerm(dpy);
  gRP = NULL;
  if (sigint_installed)
    sigaction(SIGINT, &old_sigint, NULL);
  if (sigterm_installed)
    sigaction(SIGTERM, &old_sigterm, NULL);
unload:
  if (hg) dlclose(hg);
  if (he) dlclose(he);
  return result;
}

#endif /* GPU_RENDER */
