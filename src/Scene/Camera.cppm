export module vecgltf.camera;
export import vecgltf.asset;

export namespace vecgltf {
struct View {
  Mat4 view{Mat4::Identity()}, projection{Mat4::Identity()};
  Vec3 eye{};
};

namespace camera_detail {
inline bool finite(float value) { return rstd::f32(value).is_finite(); }
inline Vec4 rotation(const Mat4 &matrix) {
  const auto &m = matrix.m;
  float trace = m[0] + m[5] + m[10];
  Vec4 q;
  if (trace > 0) {
    float s = square_root(trace + 1) * 2;
    q = {(m[6] - m[9]) / s, (m[8] - m[2]) / s, (m[1] - m[4]) / s, s * 0.25f};
  } else if (m[0] > m[5] && m[0] > m[10]) {
    float s = square_root(1 + m[0] - m[5] - m[10]) * 2;
    q = {s * 0.25f, (m[4] + m[1]) / s, (m[8] + m[2]) / s, (m[6] - m[9]) / s};
  } else if (m[5] > m[10]) {
    float s = square_root(1 + m[5] - m[0] - m[10]) * 2;
    q = {(m[4] + m[1]) / s, s * 0.25f, (m[9] + m[6]) / s, (m[8] - m[2]) / s};
  } else {
    float s = square_root(1 + m[10] - m[0] - m[5]) * 2;
    q = {(m[8] + m[2]) / s, (m[9] + m[6]) / s, s * 0.25f, (m[1] - m[4]) / s};
  }
  return normalized(q);
}
inline float aspect_scale(float aspect) {
  return minimum(maximum(aspect, 0.01f), 1);
}
} // namespace camera_detail

struct CameraFrame {
  Vec3 position{};
  Vec4 orientation{0, 0, 0, 1};
  CameraProjection projection;
  float focus_distance{1};

  bool valid() const {
    using camera_detail::finite;
    if (!finite(position.x) || !finite(position.y) || !finite(position.z) ||
        !finite(orientation.x) || !finite(orientation.y) ||
        !finite(orientation.z) || !finite(orientation.w) ||
        !finite(focus_distance) || focus_distance <= 0)
      return false;
    float length =
        orientation.x * orientation.x + orientation.y * orientation.y +
        orientation.z * orientation.z + orientation.w * orientation.w;
    if (!finite(length) || length <= 1e-20f)
      return false;
    const auto &p = projection;
    if (!finite(p.near) || !finite(p.far))
      return false;
    if (p.orthographic)
      return finite(p.xmag) && finite(p.ymag) && p.xmag != 0 && p.ymag != 0 &&
             p.near >= 0 && p.far > p.near;
    return finite(p.yfov) && p.yfov > 0 && p.yfov < 3.14159265f &&
           finite(p.aspect) && p.aspect >= 0 && p.near > 0 &&
           (p.far == 0 || p.far > p.near);
  }
  auto view(float aspect) const -> Option<View> {
    if (!valid() || !camera_detail::finite(aspect) || aspect <= 0)
      return None();
    auto world = compose(position, orientation, {1, 1, 1});
    View result;
    for (unsigned column = 0; column < 3; ++column)
      for (unsigned row = 0; row < 3; ++row)
        result.view.m[column * 4 + row] = world.m[row * 4 + column];
    result.view.m[12] = -dot(world.vector({1, 0, 0}), position);
    result.view.m[13] = -dot(world.vector({0, 1, 0}), position);
    result.view.m[14] = -dot(world.vector({0, 0, 1}), position);
    result.eye = position;
    const auto &p = projection;
    if (p.orthographic) {
      result.projection.m[0] = 1 / p.xmag;
      result.projection.m[5] = -1 / p.ymag;
      result.projection.m[10] = 1 / (p.near - p.far);
      result.projection.m[14] = p.near / (p.near - p.far);
    } else
      result.projection =
          perspective(p.yfov, p.aspect > 0 ? p.aspect : aspect, p.near, p.far);
    return Some(result);
  }
};

inline auto camera_frame(const Mat4 &world, CameraProjection projection,
                         float focus_distance = 1) -> Option<CameraFrame> {
  for (float value : world.m)
    if (!camera_detail::finite(value))
      return None();
  if (absolute(world.m[3]) > 0.00001f || absolute(world.m[7]) > 0.00001f ||
      absolute(world.m[11]) > 0.00001f || absolute(world.m[15] - 1) > 0.00001f)
    return None();
  auto rotation = Mat4::Identity();
  for (unsigned column = 0; column < 3; ++column) {
    double length_squared{};
    for (unsigned row = 0; row < 3; ++row) {
      double value = world.m[column * 4 + row];
      length_squared += value * value;
    }
    if (length_squared == 0)
      return None();
    double length = rstd::f64(length_squared).sqrt().to_primitive();
    for (unsigned row = 0; row < 3; ++row)
      rotation.m[column * 4 + row] = float(world.m[column * 4 + row] / length);
  }
  // Sheared or mirrored global transforms do not define a glTF camera view.
  if (absolute(rotation.determinant3() - 1) > 0.0001f)
    return None();
  CameraFrame result{{world.m[12], world.m[13], world.m[14]},
                     camera_detail::rotation(rotation),
                     projection,
                     focus_distance};
  return result.valid() ? Some(result) : None();
}

struct CameraSource {
  using Trait = CameraSource;
  static constexpr bool direct = false;
  template <typename Self, typename = void> struct Api {
    using Trait = CameraSource;
    auto Snapshot(float aspect) const -> Option<CameraFrame> {
      return rstd::trait_call<0>(this, aspect);
    }
  };
  template <typename T> using Funcs = TraitFuncs<&T::Snapshot>;
};

class OrbitCamera {
  Vec3 target_{};
  float radius_{1}, distance_{3}, yaw_{0.5f}, pitch_{0.25f}, roll_{};
  CameraProjection projection_;
  bool fitted_{true};

  Vec3 backward() const {
    return {sine(yaw_) * cosine(pitch_), sine(pitch_),
            cosine(yaw_) * cosine(pitch_)};
  }
  Vec3 up() const {
    auto right = Vec3{cosine(yaw_), 0, -sine(yaw_)};
    auto vertical = Vec3{-sine(yaw_) * sine(pitch_), cosine(pitch_),
                         -cosine(yaw_) * sine(pitch_)};
    return vertical * cosine(roll_) + right * sine(roll_);
  }

public:
  void fit(const Bounds &bounds) {
    target_ = bounds.center();
    radius_ = maximum(bounds.radius(), 0.01f);
    distance_ = radius_ * 3;
    yaw_ = 0.5f;
    pitch_ = 0.25f;
    roll_ = 0;
    projection_ = {};
    fitted_ = true;
  }
  void rotate(float x, float y) {
    yaw_ += x;
    float limit = maximum(1.5f, absolute(pitch_));
    pitch_ = clamp(pitch_ + y, -limit, limit);
  }
  void zoom(float amount) {
    float factor = rstd::f32(-amount * 0.1f).exp().to_primitive();
    if (!fitted_ && projection_.orthographic) {
      float current = absolute(projection_.xmag);
      float width = clamp(current * factor, minimum(radius_ * 0.0001f, current),
                          maximum(radius_ * 1000, current));
      float ratio = width / current;
      projection_.xmag *= ratio;
      projection_.ymag *= ratio;
    } else
      distance_ = clamp(distance_ * factor, minimum(radius_ * 0.01f, distance_),
                        maximum(radius_ * 1000, distance_));
  }
  void roll(float amount) { roll_ += amount; }
  void pan(float x, float y) {
    auto vertical = up();
    auto right = normalized(cross(backward() * -1, vertical));
    if (!fitted_ && projection_.orthographic)
      target_ = target_ + right * (-x * 2 * projection_.xmag) +
                vertical * (y * 2 * projection_.ymag);
    else
      target_ = target_ + (right * (-x) + vertical * y) * distance_;
  }
  auto frame(float aspect = 1) const -> CameraFrame {
    float distance =
        fitted_ ? distance_ / camera_detail::aspect_scale(aspect) : distance_;
    auto z = backward(), y = up(), x = normalized(cross(y, z));
    auto rotation = Mat4::Identity();
    rotation.m[0] = x.x;
    rotation.m[1] = x.y;
    rotation.m[2] = x.z;
    rotation.m[4] = y.x;
    rotation.m[5] = y.y;
    rotation.m[6] = y.z;
    rotation.m[8] = z.x;
    rotation.m[9] = z.y;
    rotation.m[10] = z.z;
    auto projection = projection_;
    if (fitted_) {
      projection.near = maximum(radius_ * 0.001f, 0.00001f);
      projection.far = distance + radius_ * 100;
    }
    return {target_ + z * distance, camera_detail::rotation(rotation),
            projection, distance};
  }
  bool restore(const CameraFrame &frame, float aspect = 1) {
    if (!frame.valid() || !camera_detail::finite(aspect) || aspect <= 0)
      return false;
    auto rotation = compose({}, frame.orientation, {1, 1, 1});
    auto z = rotation.vector({0, 0, 1}), y = rotation.vector({0, 1, 0});
    yaw_ = rstd::f32(z.x).atan2(rstd::f32(z.z)).to_primitive();
    pitch_ = rstd::f32(z.y)
                 .atan2(rstd::f32(square_root(z.x * z.x + z.z * z.z)))
                 .to_primitive();
    auto right = Vec3{cosine(yaw_), 0, -sine(yaw_)};
    auto vertical = Vec3{-sine(yaw_) * sine(pitch_), cosine(pitch_),
                         -cosine(yaw_) * sine(pitch_)};
    roll_ = rstd::f32(dot(y, right))
                .atan2(rstd::f32(dot(y, vertical)))
                .to_primitive();
    distance_ = frame.focus_distance;
    target_ = frame.position - z * distance_;
    projection_ = frame.projection;
    fitted_ = false;
    return true;
  }
  View view(float aspect) const { return frame(aspect).view(aspect).unwrap(); }
};
} // namespace vecgltf

export namespace rstd {
template <>
struct Impl<vecgltf::CameraSource, vecgltf::OrbitCamera>
    : ImplBase<vecgltf::OrbitCamera> {
  auto Snapshot(float aspect) const -> Option<vecgltf::CameraFrame> {
    if (!vecgltf::camera_detail::finite(aspect) || aspect <= 0)
      return None();
    auto result = this->self().frame(aspect);
    return result.valid() ? Some(result) : None();
  }
};
} // namespace rstd
