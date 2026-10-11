export module vecgltf.math;
import rstd;

export namespace vecgltf {
inline float minimum(float a, float b) { return a < b ? a : b; }
inline float maximum(float a, float b) { return a > b ? a : b; }
inline float clamp(float v, float a, float b) {
  return minimum(maximum(v, a), b);
}
inline float absolute(float a) { return a < 0 ? -a : a; }
inline float square_root(float a) { return rstd::f32(a).sqrt().to_primitive(); }
inline float sine(float a) { return rstd::f32(a).sin().to_primitive(); }
inline float cosine(float a) { return rstd::f32(a).cos().to_primitive(); }

struct Vec2 {
  float x{}, y{};
};
struct Vec3 {
  float x{}, y{}, z{};
  friend Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
  }
  friend Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
  }
  friend Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
  friend Vec3 operator/(Vec3 a, float s) { return a * (1 / s); }
  friend bool operator==(const Vec3 &, const Vec3 &) = default;
};
struct Vec4 {
  float x{}, y{}, z{}, w{};
  friend Vec4 operator+(Vec4 a, Vec4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
  }
  friend Vec4 operator*(Vec4 a, float s) {
    return {a.x * s, a.y * s, a.z * s, a.w * s};
  }
};
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 normalized(Vec3 v) {
  float length = square_root(dot(v, v));
  return length > 1e-20f ? v / length : Vec3{0, 0, 1};
}
inline Vec4 normalized(Vec4 v) {
  float length = square_root(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
  return length > 1e-20f ? v * (1 / length) : Vec4{0, 0, 0, 1};
}
inline Vec4 slerp(Vec4 a, Vec4 b, float t) {
  a = normalized(a);
  b = normalized(b);
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0) {
    b = b * -1;
    d = -d;
  }
  if (d > 0.9995f)
    return normalized(a * (1 - t) + b * t);
  float angle = rstd::f32(clamp(d, -1, 1)).acos().to_primitive();
  return normalized(a * (sine((1 - t) * angle) / sine(angle)) +
                    b * (sine(t * angle) / sine(angle)));
}

// Column-major matrices transform column vectors; projection uses Vulkan's [0,
// 1] depth.
struct Mat4 {
  float m[16]{};
  static Mat4 Identity() {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
  }
  friend bool operator==(const Mat4 &, const Mat4 &) = default;
  friend Mat4 operator*(const Mat4 &a, const Mat4 &b) {
    Mat4 r;
    for (unsigned c = 0; c < 4; ++c)
      for (unsigned row = 0; row < 4; ++row)
        for (unsigned k = 0; k < 4; ++k)
          r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
    return r;
  }
  Vec3 point(Vec3 v) const {
    float w = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
    return Vec3{m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12],
                m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
                m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]} /
           w;
  }
  Vec3 vector(Vec3 v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z,
            m[1] * v.x + m[5] * v.y + m[9] * v.z,
            m[2] * v.x + m[6] * v.y + m[10] * v.z};
  }
  float determinant3() const {
    return dot(Vec3{m[0], m[1], m[2]},
               cross({m[4], m[5], m[6]}, {m[8], m[9], m[10]}));
  }
  auto inverse() const -> rstd::Option<Mat4> {
    float a[4][8]{};
    for (unsigned row = 0; row < 4; ++row) {
      for (unsigned c = 0; c < 4; ++c)
        a[row][c] = m[c * 4 + row];
      a[row][row + 4] = 1;
    }
    for (unsigned c = 0; c < 4; ++c) {
      unsigned pivot = c;
      for (unsigned row = c + 1; row < 4; ++row)
        if (absolute(a[row][c]) > absolute(a[pivot][c]))
          pivot = row;
      if (absolute(a[pivot][c]) < 1e-12f)
        return rstd::None();
      for (unsigned k = 0; k < 8; ++k) {
        float t = a[c][k];
        a[c][k] = a[pivot][k];
        a[pivot][k] = t;
      }
      float div = a[c][c];
      for (unsigned k = 0; k < 8; ++k)
        a[c][k] /= div;
      for (unsigned row = 0; row < 4; ++row)
        if (row != c) {
          float scale = a[row][c];
          for (unsigned k = 0; k < 8; ++k)
            a[row][k] -= a[c][k] * scale;
        }
    }
    Mat4 r;
    for (unsigned row = 0; row < 4; ++row)
      for (unsigned c = 0; c < 4; ++c)
        r.m[c * 4 + row] = a[row][c + 4];
    return rstd::Some(r);
  }
};
inline Mat4 compose(Vec3 t, Vec4 rotation, Vec3 s) {
  auto q = normalized(rotation);
  float x = q.x, y = q.y, z = q.z, w = q.w;
  Mat4 r = Mat4::Identity();
  r.m[0] = (1 - 2 * (y * y + z * z)) * s.x;
  r.m[1] = 2 * (x * y + w * z) * s.x;
  r.m[2] = 2 * (x * z - w * y) * s.x;
  r.m[4] = 2 * (x * y - w * z) * s.y;
  r.m[5] = (1 - 2 * (x * x + z * z)) * s.y;
  r.m[6] = 2 * (y * z + w * x) * s.y;
  r.m[8] = 2 * (x * z + w * y) * s.z;
  r.m[9] = 2 * (y * z - w * x) * s.z;
  r.m[10] = (1 - 2 * (x * x + y * y)) * s.z;
  r.m[12] = t.x;
  r.m[13] = t.y;
  r.m[14] = t.z;
  return r;
}
inline Mat4 perspective(float yfov, float aspect, float near, float far = 0) {
  float f = 1 / rstd::f32(yfov * 0.5f).tan().to_primitive();
  Mat4 r;
  r.m[0] = f / aspect;
  r.m[5] = -f;
  r.m[11] = -1;
  r.m[10] = far > near ? far / (near - far) : -1;
  r.m[14] = far > near ? far * near / (near - far) : -near;
  return r;
}
inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up = {0, 1, 0}) {
  auto z = normalized(eye - target), x = normalized(cross(up, z)),
       y = cross(z, x);
  Mat4 r = Mat4::Identity();
  r.m[0] = x.x;
  r.m[4] = x.y;
  r.m[8] = x.z;
  r.m[12] = -dot(x, eye);
  r.m[1] = y.x;
  r.m[5] = y.y;
  r.m[9] = y.z;
  r.m[13] = -dot(y, eye);
  r.m[2] = z.x;
  r.m[6] = z.y;
  r.m[10] = z.z;
  r.m[14] = -dot(z, eye);
  return r;
}
struct Bounds {
  Vec3 min{}, max{};
  bool valid{};
  void include(Vec3 v) {
    if (!valid) {
      min = max = v;
      valid = true;
      return;
    }
    min = {minimum(min.x, v.x), minimum(min.y, v.y), minimum(min.z, v.z)};
    max = {maximum(max.x, v.x), maximum(max.y, v.y), maximum(max.z, v.z)};
  }
  Vec3 center() const { return (min + max) * 0.5f; }
  float radius() const {
    auto size = max - min;
    return square_root(dot(size, size)) * 0.5f;
  }
};
} // namespace vecgltf
