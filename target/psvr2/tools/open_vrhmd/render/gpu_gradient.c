#include "../open_vrhmd.h"

#ifdef GPU_RENDER
/* Render the full scanout buffer through the target GPU libraries. */
int gpu_render_gradient(int framebuffer_fd) {
  /* GPU-accelerated fill via EGL/GLES2 using dlopen */
  LOG("  GPU render: loading EGL/GLES2 via dlopen...");
  int gpu_ok = 0;
  void *h_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  void *h_gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
  if (!h_egl || !h_gles) {
    ERR("dlopen EGL/GLES: %s", dlerror());
  } else {
    /* EGL function pointers */
    ED (*f_GetDisp)(void *) = dlsym(h_egl, "eglGetDisplay");
    EB (*f_Init)(ED, EGLint *, EGLint *) = dlsym(h_egl, "eglInitialize");
    EB (*f_ChooseCfg)(ED, const EGLint *, EC *, EGLint, EGLint *) =
        dlsym(h_egl, "eglChooseConfig");
    EX (*f_CreateCtx)(ED, EC, EX, const EGLint *) = dlsym(h_egl, "eglCreateContext");
    EB (*f_MakeCurr)(ED, void *, void *, EX) = dlsym(h_egl, "eglMakeCurrent");
    EB (*f_BindAPI)(EU) = dlsym(h_egl, "eglBindAPI");
    EGLint (*f_GetErr)(void) = dlsym(h_egl, "eglGetError");
    const char *(*f_QStr)(ED, EGLint) = dlsym(h_egl, "eglQueryString");
    EB (*f_DestroyCtx)(ED, EX) = dlsym(h_egl, "eglDestroyContext");
    EB (*f_Term)(ED) = dlsym(h_egl, "eglTerminate");
    void *(*f_GetProc)(const char *) = dlsym(h_egl, "eglGetProcAddress");

    /* Extension EGL functions via eglGetProcAddress */
    EI (*f_CreateImg)(ED, EX, EU, void *, const EGLint *) = NULL;
    EB (*f_DestroyImg)(ED, EI) = NULL;
    void (*f_ImgTargetRBS)(GLenum, EI) = NULL;
    if (f_GetProc) {
      f_CreateImg = (void *)f_GetProc("eglCreateImageKHR");
      f_DestroyImg = (void *)f_GetProc("eglDestroyImageKHR");
      f_ImgTargetRBS =
          (void *)f_GetProc("glEGLImageTargetRenderbufferStorageOES");
    }

    /* GLES function pointers */
    void (*f_GenRB)(GLsizei, GLuint *) =
        dlsym(h_gles, "glGenRenderbuffers");
    void (*f_BindRB)(GLenum, GLuint) = dlsym(h_gles, "glBindRenderbuffer");
    void (*f_GenFB)(GLsizei, GLuint *) = dlsym(h_gles, "glGenFramebuffers");
    void (*f_BindFB)(GLenum, GLuint) = dlsym(h_gles, "glBindFramebuffer");
    void (*f_FBRB)(GLenum, GLenum, GLenum, GLuint) =
        dlsym(h_gles, "glFramebufferRenderbuffer");
    GLenum (*f_ChkFB)(GLenum) = dlsym(h_gles, "glCheckFramebufferStatus");
    void (*f_VP)(GLint, GLint, GLsizei, GLsizei) =
        dlsym(h_gles, "glViewport");
    GLuint (*f_CrSh)(GLenum) = dlsym(h_gles, "glCreateShader");
    void (*f_ShSrc)(GLuint, GLsizei, const GLchar *const *, const GLint *) =
        dlsym(h_gles, "glShaderSource");
    void (*f_CompSh)(GLuint) = dlsym(h_gles, "glCompileShader");
    void (*f_GetShiv)(GLuint, GLenum, GLint *) =
        dlsym(h_gles, "glGetShaderiv");
    void (*f_ShLog)(GLuint, GLsizei, GLsizei *, GLchar *) =
        dlsym(h_gles, "glGetShaderInfoLog");
    GLuint (*f_CrProg)(void) = dlsym(h_gles, "glCreateProgram");
    void (*f_AttSh)(GLuint, GLuint) = dlsym(h_gles, "glAttachShader");
    void (*f_Link)(GLuint) = dlsym(h_gles, "glLinkProgram");
    void (*f_GetPiv)(GLuint, GLenum, GLint *) =
        dlsym(h_gles, "glGetProgramiv");
    void (*f_UseProg)(GLuint) = dlsym(h_gles, "glUseProgram");
    GLint (*f_GetALoc)(GLuint, const GLchar *) =
        dlsym(h_gles, "glGetAttribLocation");
    void (*f_VAP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) =
        dlsym(h_gles, "glVertexAttribPointer");
    void (*f_EnVAA)(GLuint) = dlsym(h_gles, "glEnableVertexAttribArray");
    void (*f_Draw)(GLenum, GLint, GLsizei) = dlsym(h_gles, "glDrawArrays");
    void (*f_Finish)(void) = dlsym(h_gles, "glFinish");
    void (*f_DelSh)(GLuint) = dlsym(h_gles, "glDeleteShader");
    void (*f_DelProg)(GLuint) = dlsym(h_gles, "glDeleteProgram");
    void (*f_DelFB)(GLsizei, const GLuint *) =
        dlsym(h_gles, "glDeleteFramebuffers");
    void (*f_DelRB)(GLsizei, const GLuint *) =
        dlsym(h_gles, "glDeleteRenderbuffers");

    if (!f_GetDisp ||
        !f_Init ||
        !f_ChooseCfg ||
        !f_CreateCtx ||
        !f_MakeCurr ||
        !f_BindAPI ||
        !f_GetErr ||
        !f_QStr ||
        !f_DestroyCtx ||
        !f_Term ||
        !f_GetProc ||
        !f_CreateImg ||
        !f_DestroyImg ||
        !f_ImgTargetRBS ||
        !f_GenRB ||
        !f_BindRB ||
        !f_GenFB ||
        !f_BindFB ||
        !f_FBRB ||
        !f_ChkFB ||
        !f_VP ||
        !f_CrSh ||
        !f_ShSrc ||
        !f_CompSh ||
        !f_GetShiv ||
        !f_ShLog ||
        !f_CrProg ||
        !f_AttSh ||
        !f_Link ||
        !f_GetPiv ||
        !f_UseProg ||
        !f_GetALoc ||
        !f_VAP ||
        !f_EnVAA ||
        !f_Draw ||
        !f_Finish ||
        !f_DelSh ||
        !f_DelProg ||
        !f_DelFB ||
        !f_DelRB) {
      ERR("Missing EGL/GLES symbols");
    } else {
      if (!f_BindAPI(0x30A0))
        goto close_libraries;
      ED dpy = f_GetDisp(0);
      EX ectx = NULL;
      EGLint emaj = 0, emin = 0;
      if (!dpy || !f_Init(dpy, &emaj, &emin)) {
        ERR("eglInitialize failed");
        goto close_libraries;
      }
      LOG("  EGL %d.%d, vendor: %s", emaj, emin, f_QStr(dpy, 0x3053));

      EGLint cfg_a[] = {0x3040, 0x0004, 0x3033, 0, 0x3024, 8,
                        0x3023, 8,      0x3022, 8, 0x3038};
      EC ecfg = NULL;
      EGLint ncfg = 0;
      if (!f_ChooseCfg(dpy, cfg_a, &ecfg, 1, &ncfg) || ncfg < 1) {
        ERR("No EGL gradient configuration");
        goto cleanup_display;
      }
      EGLint ctx_a[] = {0x3098, 2, 0x3038};
      ectx = f_CreateCtx(dpy, ecfg, 0, ctx_a);
      if (!ectx || !f_MakeCurr(dpy, 0, 0, ectx)) {
        ERR("EGL gradient context failed");
        goto cleanup_display;
      }
      LOG("  EGL context + surfaceless OK");

      EGLint img_a[] = {0x3057, 4000,        0x3056, 2040,
                        0x3271, 0x34324742, /* DRM_FORMAT_BGR888 */
                        0x3272, framebuffer_fd, 0x3273, 0,
                        0x3274, 12000,       0x3038};
      EI eimg = f_CreateImg(dpy, 0, 0x3270, 0, img_a);
      if (!eimg) {
        LOG("  BGR888 failed, trying RGB888...");
        img_a[5] = 0x34324752;
        eimg = f_CreateImg(dpy, 0, 0x3270, 0, img_a);
      }
      if (!eimg) {
        ERR("eglCreateImageKHR failed: 0x%x", f_GetErr());
      } else {
        LOG("  EGL image: %p", eimg);
        GLuint rbo = 0, fbo = 0;
        f_GenRB(1, &rbo);
        f_BindRB(0x8D41, rbo);
        f_ImgTargetRBS(0x8D41, eimg);
        f_GenFB(1, &fbo);
        f_BindFB(0x8D40, fbo);
        f_FBRB(0x8D40, 0x8CE0, 0x8D41, rbo);
        if (!rbo || !fbo || f_ChkFB(0x8D40) != 0x8CD5) {
          ERR("FBO incomplete: 0x%x", f_ChkFB(0x8D40));
        } else {
          LOG("  FBO complete, rendering gradient...");
          f_VP(0, 0, 4000, 2040);
          const char *vs =
              "attribute vec2 aPos;\nvarying vec2 vUV;\nvoid "
              "main(){vUV=aPos*0.5+0.5;gl_Position=vec4(aPos,0,1);}\n";
          const char *fs =
              "precision mediump float;\nvarying vec2 vUV;\n"
              "vec3 hsv(float h){return "
              "clamp(abs(mod(h*6.0+vec3(0,4,2),6.0)-3.0)-1.0,0.0,1.0);}\n"
              "void main(){gl_FragColor=vec4(hsv(vUV.x),1);}\n";
          GLuint vsh = f_CrSh(0x8B31);
          f_ShSrc(vsh, 1, &vs, 0);
          f_CompSh(vsh);
          GLint ck = 0;
          int shaders_ok = 1;
          f_GetShiv(vsh, 0x8B81, &ck);
          if (!ck) {
            char l[256] = {0};
            f_ShLog(vsh, 256, 0, l);
            ERR("VS: %s", l);
            shaders_ok = 0;
          }
          GLuint fsh = f_CrSh(0x8B30);
          f_ShSrc(fsh, 1, &fs, 0);
          f_CompSh(fsh);
          f_GetShiv(fsh, 0x8B81, &ck);
          if (!ck) {
            char l[256] = {0};
            f_ShLog(fsh, 256, 0, l);
            ERR("FS: %s", l);
            shaders_ok = 0;
          }
          GLuint prog = f_CrProg();
          f_AttSh(prog, vsh);
          f_AttSh(prog, fsh);
          f_Link(prog);
          f_GetPiv(prog, 0x8B82, &ck);
          if (ck && shaders_ok) {
            f_UseProg(prog);
            GLint aP = f_GetALoc(prog, "aPos");
            float q[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1};
            if (aP < 0) {
              ERR("Gradient shader has no position attribute");
              goto cleanup_program;
            }
            f_VAP(aP, 2, 0x1406, 0, 0, q);
            f_EnVAA(aP);
            f_Draw(0x0004, 0, 6);
            f_Finish();
            gpu_ok = 1;
            LOG("  GPU render complete!");
          } else {
            ERR("Shader link failed");
          }
        cleanup_program:
          f_DelSh(vsh);
          f_DelSh(fsh);
          f_DelProg(prog);
        }
        f_BindFB(0x8D40, 0);
        f_DelFB(1, &fbo);
        f_DelRB(1, &rbo);
        f_DestroyImg(dpy, eimg);
      }
    cleanup_display:
      if (ectx) {
        f_MakeCurr(dpy, 0, 0, 0);
        f_DestroyCtx(dpy, ectx);
      }
      f_Term(dpy);
    }
  }
close_libraries:
  if (h_gles)
    dlclose(h_gles);
  if (h_egl)
    dlclose(h_egl);
  return gpu_ok ? 0 : -1;
}
#endif
