export module vecgltf.viewer.camera;
import vecgltf;
import rstd.json;

namespace gltf = vecgltf;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace viewer {
enum class CameraMode { Orbit, Asset, Track };

struct CameraKeyframe {
  float time{};
  gltf::CameraFrame camera;
};

struct CameraTrackStatus {
  usize keyframes{};
  float time{}, duration{}, interval{2};
  bool playing{}, looping{true}, active{}, dirty{};
};

struct CameraTrackError {
  String source, location, message;
};

class CameraTrack {
  Vec<CameraKeyframe> keys_;
  float time_{}, interval_{2};
  bool playing_{}, looping_{true}, dirty_{};

  static bool finite(float value) { return rstd::f32(value).is_finite(); }
  static bool valid(const gltf::CameraFrame &frame) {
    const auto &p = frame.projection;
    return frame.valid() && finite(p.yfov) && finite(p.aspect) &&
           finite(p.near) && finite(p.far) && finite(p.xmag) && finite(p.ymag);
  }
  static float mix(float a, float b, float t) {
    return float(double(a) * (1 - double(t)) + double(b) * double(t));
  }
  static auto interpolate(const gltf::CameraFrame &a,
                          const gltf::CameraFrame &b, float t)
      -> gltf::CameraFrame {
    gltf::CameraFrame result;
    result.position = {mix(a.position.x, b.position.x, t),
                       mix(a.position.y, b.position.y, t),
                       mix(a.position.z, b.position.z, t)};
    result.orientation = gltf::slerp(a.orientation, b.orientation, t);
    result.focus_distance = mix(a.focus_distance, b.focus_distance, t);
    const auto &p = a.projection;
    const auto &q = b.projection;
    result.projection = p;
    bool compatible = p.orthographic == q.orthographic &&
                      (p.aspect == 0) == (q.aspect == 0) &&
                      (p.far == 0) == (q.far == 0);
    if (p.orthographic)
      compatible = compatible && (p.xmag > 0) == (q.xmag > 0) &&
                   (p.ymag > 0) == (q.ymag > 0);
    if (compatible) {
      auto &projection = result.projection;
      projection.near = mix(p.near, q.near, t);
      projection.far = mix(p.far, q.far, t);
      if (p.orthographic) {
        projection.xmag = mix(p.xmag, q.xmag, t);
        projection.ymag = mix(p.ymag, q.ymag, t);
      } else {
        projection.yfov = mix(p.yfov, q.yfov, t);
        projection.aspect = mix(p.aspect, q.aspect, t);
      }
      // Rounding can collapse distinct clipping planes to the same float.
      if (!result.valid())
        result.projection = p;
    }
    return result;
  }

public:
  auto keys() const -> slice<CameraKeyframe> { return keys_.as_slice(); }
  float time() const { return time_; }
  float duration() const {
    return keys_.is_empty() ? 0 : keys_.last().unwrap()->time;
  }
  float interval() const { return interval_; }
  bool playing() const { return playing_; }
  bool looping() const { return looping_; }
  bool dirty() const { return dirty_; }
  void mark_saved() { dirty_ = false; }

  bool append(gltf::CameraFrame camera) {
    if (!valid(camera))
      return false;
    float next = keys_.is_empty() ? 0 : duration() + interval_;
    if (!finite(next) || (!keys_.is_empty() && next <= duration()))
      return false;
    keys_.push({next, camera});
    time_ = next;
    playing_ = false;
    dirty_ = true;
    return true;
  }
  bool replace(Vec<CameraKeyframe> keys) {
    float previous = -1;
    for (const auto &key : keys) {
      if (!finite(key.time) || key.time <= previous ||
          (previous < 0 && key.time != 0) || !valid(key.camera))
        return false;
      previous = key.time;
    }
    keys_ = rstd::move(keys);
    time_ = 0;
    playing_ = false;
    dirty_ = true;
    return true;
  }
  bool remove_last() {
    if (keys_.is_empty())
      return false;
    (void)keys_.pop();
    time_ = gltf::minimum(time_, duration());
    playing_ = false;
    dirty_ = true;
    return true;
  }
  void clear() {
    if (!keys_.is_empty())
      dirty_ = true;
    keys_.clear();
    time_ = 0;
    playing_ = false;
  }
  bool play() {
    if (keys_.len() < usize(2))
      return false;
    if (time_ >= duration())
      time_ = 0;
    playing_ = true;
    return true;
  }
  void pause() { playing_ = false; }
  bool seek(float seconds) {
    if (keys_.is_empty() || !finite(seconds))
      return false;
    time_ = gltf::clamp(seconds, 0, duration());
    playing_ = false;
    return true;
  }
  void update(float delta) {
    if (!playing_ || !finite(delta) || delta <= 0)
      return;
    double next = double(time_) + double(delta);
    double end = duration();
    if (looping_)
      time_ = float((rstd::f64(next) % rstd::f64(end)).to_primitive());
    else if (next >= end) {
      time_ = duration();
      playing_ = false;
    } else
      time_ = float(next);
  }
  void set_looping(bool value) {
    if (looping_ != value)
      dirty_ = true;
    looping_ = value;
  }
  bool set_interval(float value) {
    if (!finite(value) || value <= 0)
      return false;
    if (interval_ != value)
      dirty_ = true;
    interval_ = value;
    return true;
  }
  auto sample(float seconds) const -> Option<gltf::CameraFrame> {
    if (keys_.is_empty() || !finite(seconds))
      return None();
    auto values = keys();
    if (seconds <= 0)
      return Some(gltf::CameraFrame(values[usize(0)].camera));
    if (seconds >= duration())
      return Some(gltf::CameraFrame(values[values.len() - usize(1)].camera));
    usize right{1};
    while (values[right].time < seconds)
      ++right;
    const auto &a = values[right - usize(1)];
    const auto &b = values[right];
    if (seconds == b.time)
      return Some(gltf::CameraFrame(b.camera));
    float t = (seconds - a.time) / (b.time - a.time);
    return Some(interpolate(a.camera, b.camera, t));
  }
  auto Snapshot(float aspect) const -> Option<gltf::CameraFrame> {
    auto result = sample(time_);
    if (result.is_none() || result->view(aspect).is_none())
      return None();
    return result;
  }
};
} // namespace viewer

export namespace rstd {
template <>
struct Impl<vecgltf::CameraSource, viewer::CameraTrack>
    : ImplBase<viewer::CameraTrack> {
  auto Snapshot(float aspect) const -> Option<vecgltf::CameraFrame> {
    return this->self().Snapshot(aspect);
  }
};
} // namespace rstd

export namespace viewer {
class CameraController {
  gltf::OrbitCamera orbit_;
  CameraTrack track_;
  CameraMode mode_{CameraMode::Orbit};
  gltf::AssetId asset_camera_;
  Option<gltf::CameraFrame> last_;

public:
  auto orbit() -> gltf::OrbitCamera & { return orbit_; }
  auto track() -> CameraTrack & { return track_; }
  auto track() const -> const CameraTrack & { return track_; }
  CameraMode mode() const { return mode_; }
  unsigned asset_camera() const {
    return mode_ == CameraMode::Asset ? asset_camera_.index : gltf::invalid_index;
  }
  void fit(const gltf::SceneInstance &instance) {
    track_.pause();
    orbit_.fit(instance.bounds());
    mode_ = CameraMode::Orbit;
    asset_camera_ = {};
    last_ = None();
  }
  bool select_asset(const gltf::SceneInstance &instance, unsigned node,
                    float aspect) {
    gltf::SceneCameraSource source{&instance, instance.asset().node_id(node)};
    auto snapshot = rstd::as<gltf::CameraSource>(source).Snapshot(aspect);
    if (snapshot.is_none())
      return false;
    asset_camera_ = source.node;
    track_.pause();
    mode_ = CameraMode::Asset;
    last_ = snapshot;
    return true;
  }
  auto frame(const gltf::SceneInstance &instance, float aspect)
      -> Option<gltf::CameraFrame> {
    Option<gltf::CameraFrame> result;
    switch (mode_) {
    case CameraMode::Orbit:
      result = rstd::as<gltf::CameraSource>(orbit_).Snapshot(aspect);
      break;
    case CameraMode::Asset: {
      gltf::SceneCameraSource source{&instance, asset_camera_};
      result = rstd::as<gltf::CameraSource>(source).Snapshot(aspect);
      break;
    }
    case CameraMode::Track:
      result = rstd::as<gltf::CameraSource>(track_).Snapshot(aspect);
      break;
    }
    if (result.is_some())
      last_ = result;
    return result;
  }
  bool take_control(const gltf::SceneInstance &instance, float aspect) {
    auto current = frame(instance, aspect);
    if (mode_ == CameraMode::Orbit && current.is_some()) {
      track_.pause();
      return true;
    }
    if (current.is_none())
      current = last_;
    if (current.is_none() || !orbit_.restore(*current, aspect))
      return false;
    track_.pause();
    mode_ = CameraMode::Orbit;
    asset_camera_ = {};
    return true;
  }
  bool rotate(const gltf::SceneInstance &instance, float aspect, float x,
              float y) {
    if (!rstd::f32(x).is_finite() || !rstd::f32(y).is_finite() ||
        !take_control(instance, aspect))
      return false;
    orbit_.rotate(x, y);
    return true;
  }
  bool pan(const gltf::SceneInstance &instance, float aspect, float x, float y) {
    if (!rstd::f32(x).is_finite() || !rstd::f32(y).is_finite() ||
        !take_control(instance, aspect))
      return false;
    orbit_.pan(x, y);
    return true;
  }
  bool zoom(const gltf::SceneInstance &instance, float aspect, float amount) {
    if (!rstd::f32(amount).is_finite() || !take_control(instance, aspect))
      return false;
    orbit_.zoom(amount);
    return true;
  }
  bool record(const gltf::SceneInstance &instance, float aspect) {
    auto current = frame(instance, aspect);
    return current.is_some() && track_.append(*current);
  }
  bool toggle_track() {
    if (track_.playing()) {
      track_.pause();
      return true;
    }
    if (!track_.play())
      return false;
    mode_ = CameraMode::Track;
    asset_camera_ = {};
    return true;
  }
  bool seek_track(float seconds) {
    if (!track_.seek(seconds))
      return false;
    mode_ = CameraMode::Track;
    asset_camera_ = {};
    return true;
  }
  bool remove_last(const gltf::SceneInstance &instance, float aspect) {
    if (track_.keys().is_empty() || !take_control(instance, aspect))
      return false;
    return track_.remove_last();
  }
  void clear_track(const gltf::SceneInstance &instance, float aspect) {
    if (take_control(instance, aspect))
      track_.clear();
  }
  void update(float delta) {
    if (mode_ == CameraMode::Track)
      track_.update(delta);
  }
  bool animating() const {
    return mode_ == CameraMode::Track && track_.playing();
  }
  CameraTrackStatus status() const {
    return {track_.keys().len(), track_.time(), track_.duration(),
            track_.interval(), track_.playing(), track_.looping(),
            mode_ == CameraMode::Track, track_.dirty()};
  }
  void replace_track(CameraTrack track) {
    track.pause();
    if (!track.keys().is_empty()) {
      mode_ = CameraMode::Track;
      asset_camera_ = {};
    } else if (mode_ == CameraMode::Track) {
      if (last_.is_some())
        (void)orbit_.restore(*last_);
      mode_ = CameraMode::Orbit;
    }
    track_ = rstd::move(track);
  }
};
} // namespace viewer

namespace viewer {
auto camera_track_error(ref<str> source, ref<str> location, ref<str> message)
    -> CameraTrackError {
  return {String::make(source), String::make(location), String::make(message)};
}

bool camera_number(const rstd::json::Value &value, float &output) {
  auto number = value.as_f64();
  if (number.is_none())
    return false;
  float result = float(number.unwrap().to_primitive());
  if (!rstd::f32(result).is_finite())
    return false;
  output = result;
  return true;
}

template <unsigned N>
bool camera_numbers(const rstd::json::Value &value, float (&output)[N]) {
  auto array = value.as_array();
  if (array.is_none() || array.unwrap()->len() != usize(N))
    return false;
  for (unsigned i = 0; i < N; ++i)
    if (!camera_number(value[usize(i)], output[i]))
      return false;
  return true;
}

bool camera_keyframe(const rstd::json::Value &value, CameraKeyframe &key) {
  if (!value.is_object())
    return false;
  float position[3], orientation[4];
  const auto &projection = value["projection"_str];
  auto orthographic = projection["orthographic"_str].as_bool();
  auto &p = key.camera.projection;
  if (!projection.is_object() || orthographic.is_none() ||
      !camera_number(value["time"_str], key.time) ||
      !camera_numbers(value["position"_str], position) ||
      !camera_numbers(value["orientation"_str], orientation) ||
      !camera_number(value["focus_distance"_str], key.camera.focus_distance) ||
      !camera_number(projection["yfov"_str], p.yfov) ||
      !camera_number(projection["aspect"_str], p.aspect) ||
      !camera_number(projection["near"_str], p.near) ||
      !camera_number(projection["far"_str], p.far) ||
      !camera_number(projection["xmag"_str], p.xmag) ||
      !camera_number(projection["ymag"_str], p.ymag))
    return false;
  key.camera.position = {position[0], position[1], position[2]};
  key.camera.orientation = {orientation[0], orientation[1], orientation[2],
                            orientation[3]};
  p.orthographic = orthographic.unwrap();
  return true;
}

auto camera_json_number(float value) -> rstd::json::Value {
  return rstd::json::Value::Number(
      rstd::json::Number::from_f64(rstd::f64(value)).unwrap());
}

template <unsigned N>
auto camera_json_numbers(const float (&values)[N]) -> rstd::json::Value {
  rstd::json::Array array;
  for (float value : values)
    array.push(camera_json_number(value));
  return rstd::json::Value::Array(rstd::move(array));
}

auto camera_json_keyframe(const CameraKeyframe &key) -> rstd::json::Value {
  rstd::json::Value value, projection;
  const auto &camera = key.camera;
  const auto &p = camera.projection;
  float position[]{camera.position.x, camera.position.y, camera.position.z};
  float orientation[]{camera.orientation.x, camera.orientation.y,
                      camera.orientation.z, camera.orientation.w};
  value["time"_str] = camera_json_number(key.time);
  value["position"_str] = camera_json_numbers(position);
  value["orientation"_str] = camera_json_numbers(orientation);
  value["focus_distance"_str] = camera_json_number(camera.focus_distance);
  projection["orthographic"_str] = rstd::json::Value::Bool(p.orthographic);
  projection["yfov"_str] = camera_json_number(p.yfov);
  projection["aspect"_str] = camera_json_number(p.aspect);
  projection["near"_str] = camera_json_number(p.near);
  projection["far"_str] = camera_json_number(p.far);
  projection["xmag"_str] = camera_json_number(p.xmag);
  projection["ymag"_str] = camera_json_number(p.ymag);
  value["projection"_str] = rstd::move(projection);
  return value;
}
} // namespace viewer

export namespace viewer {
auto LoadCameraTrack(ref<str> path) -> Result<CameraTrack, CameraTrackError> {
  auto source = rstd::path::PathBuf::from(path);
  auto read = rstd::fs::read_to_string(source.as_path());
  if (read.is_err()) {
    auto message = rstd::format("{}", read.unwrap_err());
    return Err(camera_track_error(path, "read"_str, message.as_str()));
  }
  auto text = read.unwrap();
  rstd::json::ParseOptions options;
  options.reject_duplicate_keys = true;
  options.reject_integer_overflow = true;
  auto parsed = rstd::json::from_str(text.as_str(), options);
  if (parsed.is_err()) {
    auto error = parsed.unwrap_err();
    auto location = rstd::format("{}:{}", error.line(), error.column());
    auto message = rstd::format("{}", error);
    return Err(camera_track_error(path, location.as_str(), message.as_str()));
  }
  const auto value = parsed.unwrap();
  auto version = value["version"_str].as_u64();
  if (!value.is_object() || version.is_none() || version.unwrap() != u64(1))
    return Err(camera_track_error(path, "version"_str,
                                  "expected camera track version 1"_str));
  CameraTrack track;
  float interval;
  auto looping = value["looping"_str].as_bool();
  if (!camera_number(value["interval"_str], interval) ||
      !track.set_interval(interval))
    return Err(camera_track_error(path, "interval"_str,
                                  "expected a finite positive interval"_str));
  if (looping.is_none())
    return Err(camera_track_error(path, "looping"_str, "expected a boolean"_str));
  track.set_looping(looping.unwrap());
  auto array = value["keyframes"_str].as_array();
  if (array.is_none())
    return Err(camera_track_error(path, "keyframes"_str, "expected an array"_str));
  Vec<CameraKeyframe> keys;
  usize index{};
  for (const auto &item : **array) {
    CameraKeyframe key;
    if (!camera_keyframe(item, key)) {
      auto location = rstd::format("keyframes[{}]", index);
      return Err(camera_track_error(path, location.as_str(),
                                    "expected finite camera frame fields"_str));
    }
    keys.push(rstd::move(key));
    ++index;
  }
  if (!track.replace(rstd::move(keys)))
    return Err(camera_track_error(
        path, "keyframes"_str,
        "expected valid cameras and increasing times starting at zero"_str));
  track.mark_saved();
  return Ok(rstd::move(track));
}

auto SaveCameraTrack(ref<str> path, const CameraTrack &track)
    -> Result<empty, CameraTrackError> {
  rstd::json::Value value;
  value["version"_str] =
      rstd::json::Value::Number(rstd::json::Number::from_u64(u64(1)));
  value["interval"_str] = camera_json_number(track.interval());
  value["looping"_str] = rstd::json::Value::Bool(track.looping());
  rstd::json::Array keys;
  for (const auto &key : track.keys())
    keys.push(camera_json_keyframe(key));
  value["keyframes"_str] = rstd::json::Value::Array(rstd::move(keys));
  auto text = rstd::json::to_string(value, {.pretty = true});
  auto destination = rstd::path::PathBuf::from(path);
  auto written = rstd::fs::write_atomic(destination.as_path(),
                                       text.as_str().as_bytes());
  if (written.is_err()) {
    auto message = rstd::format("{}", written.unwrap_err());
    return Err(camera_track_error(path, "write"_str, message.as_str()));
  }
  return Ok(empty{});
}
} // namespace viewer
