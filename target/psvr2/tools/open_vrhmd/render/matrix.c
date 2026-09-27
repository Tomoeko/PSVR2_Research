#include "../open_vrhmd.h"

#ifdef GPU_RENDER

void m4_id(float *m) {
  memset(m, 0, 16 * sizeof(*m));
  m[0] = m[5] = m[10] = m[15] = 1;
}
void m4_mul(const float *a, const float *b, float *o) {
  float t[16];
  /* Temporary output preserves in-place multiplication (o == a or b). */
  for (int column = 0; column < 4; column++) {
    const float *bc = b + column * 4;
    for (int row = 0; row < 4; row++) {
      t[column * 4 + row] = a[row] * bc[0] + a[4 + row] * bc[1] +
                            a[8 + row] * bc[2] + a[12 + row] * bc[3];
    }
  }
  memcpy(o, t, sizeof(t));
}
void m4_frustum(float *m, float left, float right, float bottom,
                float top, float near, float far) {
  memset(m, 0, 16 * sizeof(*m));
  m[0] = (2.0f * near) / (right - left);
  m[5] = (2.0f * near) / (top - bottom);
  m[8] = (right + left) / (right - left);
  m[9] = (top + bottom) / (top - bottom);
  m[10] = -(far + near) / (far - near);
  m[11] = -1.0f;
  m[14] = -(2.0f * far * near) / (far - near);
}
void m4_trans(float *m, float x, float y, float z) {
  m4_id(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
}
void m4_roty(float *m, float a) {
  m4_id(m);
  float c = cosf(a), s = sinf(a);
  m[0] = c;
  m[2] = -s;
  m[8] = s;
  m[10] = c;
}
void m4_rotx(float *m, float a) {
  m4_id(m);
  float c = cosf(a), s = sinf(a);
  m[5] = c;
  m[6] = s;
  m[9] = -s;
  m[10] = c;
}

// clang-format off
/* Cube: 36 verts × {pos(3), normal(3), color(3)} = 324 floats */
const float cube_v[324] = {
  /* Front +Z (red) */
  -1,-1,1, 0,0,1, 1,.3,.2,  1,-1,1, 0,0,1, 1,.3,.2,  1,1,1, 0,0,1, 1,.3,.2,
  -1,-1,1, 0,0,1, 1,.3,.2,  1,1,1, 0,0,1, 1,.3,.2,  -1,1,1, 0,0,1, 1,.3,.2,
  /* Back -Z (green) */
  1,-1,-1, 0,0,-1, .2,1,.3,  -1,-1,-1, 0,0,-1, .2,1,.3,  -1,1,-1, 0,0,-1, .2,1,.3,
  1,-1,-1, 0,0,-1, .2,1,.3,  -1,1,-1, 0,0,-1, .2,1,.3,  1,1,-1, 0,0,-1, .2,1,.3,
  /* Right +X (blue) */
  1,-1,1, 1,0,0, .2,.3,1,  1,-1,-1, 1,0,0, .2,.3,1,  1,1,-1, 1,0,0, .2,.3,1,
  1,-1,1, 1,0,0, .2,.3,1,  1,1,-1, 1,0,0, .2,.3,1,  1,1,1, 1,0,0, .2,.3,1,
  /* Left -X (yellow) */
  -1,-1,-1, -1,0,0, 1,1,.2,  -1,-1,1, -1,0,0, 1,1,.2,  -1,1,1, -1,0,0, 1,1,.2,
  -1,-1,-1, -1,0,0, 1,1,.2,  -1,1,1, -1,0,0, 1,1,.2,  -1,1,-1, -1,0,0, 1,1,.2,
  /* Top +Y (cyan) */
  -1,1,1, 0,1,0, .2,1,1,  1,1,1, 0,1,0, .2,1,1,  1,1,-1, 0,1,0, .2,1,1,
  -1,1,1, 0,1,0, .2,1,1,  1,1,-1, 0,1,0, .2,1,1,  -1,1,-1, 0,1,0, .2,1,1,
  /* Bottom -Y (magenta) */
  -1,-1,-1, 0,-1,0, 1,.2,1,  1,-1,-1, 0,-1,0, 1,.2,1,  1,-1,1, 0,-1,0, 1,.2,1,
  -1,-1,-1, 0,-1,0, 1,.2,1,  1,-1,1, 0,-1,0, 1,.2,1,  -1,-1,1, 0,-1,0, 1,.2,1,
};
// clang-format on

#endif /* GPU_RENDER */
