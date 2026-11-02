export module vecgltf.scene;
export import vecgltf.camera;

export namespace vecgltf {
struct NodePose {
  Vec3 translation, scale;
  Vec4 rotation;
  Mat4 world{Mat4::Identity()};
  Vec<float> weights;
  Vec<Mat4> skin_palette;
  Vec<Bounds> primitive_bounds;
};
class SceneInstance {
  Arc<Asset> asset_;
  Vec<NodePose> poses_;
  Vec<unsigned> order_;
  Vec<bool> visible_;
  unsigned scene_{}, animation_{invalid_index};
  float time_{};
  bool playing_{true}, looping_{true};
  u64 revision_{1};
  Bounds bounds_;

  void visit(unsigned index) {
    if (at(visible_, index))
      return;
    at(visible_, index) = true;
    for (auto child : at(asset_->nodes, index).children)
      visit(child);
  }
  void order(unsigned index, Vec<bool> &added) {
    if (at(added, index))
      return;
    auto parent = at(asset_->nodes, index).parent;
    if (parent != invalid_index)
      order(parent, added);
    at(added, index) = true;
    order_.push(unsigned(index));
  }
  void sample(const AnimationSampler &sampler, AnimationPath path,
              NodePose &pose) {
    unsigned left = 0, right = 0;
    while (right + 1 < count(sampler.times) &&
           at(sampler.times, right + 1) <= time_)
      ++right;
    left = right;
    if (right + 1 < count(sampler.times))
      ++right;
    float duration = at(sampler.times, right) - at(sampler.times, left);
    float t = duration > 0
                  ? clamp((time_ - at(sampler.times, left)) / duration, 0, 1)
                  : 0;
    auto components = sampler.components;
    auto value = [&](unsigned key, unsigned component, unsigned part) {
      return at(
          sampler.values,
          (rstd::size_t(key) *
               (sampler.interpolation == Interpolation::CubicSpline ? 3 : 1) +
           part) * components +
              component);
    };
    Vec<float> result;
    result.resize(usize(components), 0.0f);
    for (unsigned c = 0; c < components; ++c) {
      if (sampler.interpolation == Interpolation::CubicSpline) {
        float t2 = t * t, t3 = t2 * t;
        at(result, c) = (2 * t3 - 3 * t2 + 1) * value(left, c, 1) +
                        (t3 - 2 * t2 + t) * duration * value(left, c, 2) +
                        (-2 * t3 + 3 * t2) * value(right, c, 1) +
                        (t3 - t2) * duration * value(right, c, 0);
      } else if (sampler.interpolation == Interpolation::Step)
        at(result, c) = value(left, c, 0);
      else
        at(result, c) = value(left, c, 0) * (1 - t) + value(right, c, 0) * t;
    }
    if (path == AnimationPath::Weights) {
      pose.weights = rstd::move(result);
      return;
    }
    if (path == AnimationPath::Rotation) {
      if (sampler.interpolation == Interpolation::Linear)
        pose.rotation = slerp({value(left, 0, 0), value(left, 1, 0),
                               value(left, 2, 0), value(left, 3, 0)},
                              {value(right, 0, 0), value(right, 1, 0),
                               value(right, 2, 0), value(right, 3, 0)},
                              t);
      else
        pose.rotation = normalized(
            Vec4{at(result, 0), at(result, 1), at(result, 2), at(result, 3)});
    } else {
      auto v = Vec3{at(result, 0), at(result, 1), at(result, 2)};
      if (path == AnimationPath::Translation)
        pose.translation = v;
      else
        pose.scale = v;
    }
  }
  void evaluate() {
    for (unsigned i = 0; i < count(poses_); ++i) {
      const auto &node = at(asset_->nodes, i);
      auto &pose = at(poses_, i);
      pose.translation = node.translation;
      pose.rotation = node.rotation;
      pose.scale = node.scale;
      pose.weights = node.weights.clone();
    }
    if (animation_ != invalid_index) {
      const auto &animation = at(asset_->animations, animation_);
      for (const auto &channel : animation.channels)
        sample(at(animation.samplers, channel.sampler), channel.path,
               at(poses_, channel.node));
    }
    for (auto index : order_) {
      const auto &node = at(asset_->nodes, index);
      auto &pose = at(poses_, index);
      auto local = node.has_matrix
                       ? node.matrix
                       : compose(pose.translation, pose.rotation, pose.scale);
      pose.world = node.parent == invalid_index
                       ? local
                       : at(poses_, node.parent).world * local;
    }
    for (unsigned i = 0; i < count(poses_); ++i) {
      auto &pose = at(poses_, i);
      pose.skin_palette.clear();
      auto skin_index = at(asset_->nodes, i).skin;
      if (skin_index == invalid_index)
        continue;
      const auto &skin = at(asset_->skins, skin_index);
      for (unsigned joint = 0; joint < count(skin.joints); ++joint)
        pose.skin_palette.push(at(poses_, at(skin.joints, joint)).world *
                               at(skin.inverse_bind, joint));
    }
    bounds_ = {};
    for (unsigned i = 0; i < count(poses_); ++i) {
      auto &pose = at(poses_, i);
      pose.primitive_bounds.clear();
      const auto &node = at(asset_->nodes, i);
      if (!at(visible_, i) || node.mesh == invalid_index)
        continue;
      const auto &mesh = at(asset_->meshes, node.mesh);
      for (const auto &primitive : mesh.primitives) {
        Bounds bounds;
        if (node.skin != invalid_index || !primitive.morphs.is_empty()) {
          for (unsigned v = 0; v < count(primitive.vertices); ++v)
            bounds.include(world_position(i, primitive, v));
        } else if (primitive.bounds.valid) {
          for (unsigned corner = 0; corner < 8; ++corner) {
            const auto &b = primitive.bounds;
            bounds.include(pose.world.point({corner & 1 ? b.max.x : b.min.x,
                                             corner & 2 ? b.max.y : b.min.y,
                                             corner & 4 ? b.max.z : b.min.z}));
          }
        }
        if (bounds.valid) {
          bounds_.include(bounds.min);
          bounds_.include(bounds.max);
        }
        pose.primitive_bounds.push(rstd::move(bounds));
      }
    }
    ++revision_;
  }

public:
  explicit SceneInstance(Arc<Asset> asset) : asset_(rstd::move(asset)) {
    for (const auto &node : asset_->nodes)
      poses_.push({node.translation, node.scale, node.rotation,
                   Mat4::Identity(), node.weights.clone()});
    Vec<bool> added;
    added.resize(asset_->nodes.len(), false);
    for (unsigned i = 0; i < count(asset_->nodes); ++i)
      order(i, added);
    set_scene(asset_->default_scene);
  }
  const Asset &asset() const { return *asset_; }
  auto asset_owner() const -> Arc<Asset> { return asset_.clone(); }
  const NodePose &pose(unsigned node) const { return at(poses_, node); }
  bool visible(unsigned node) const {
    return node < count(visible_) && at(visible_, node);
  }
  unsigned scene() const { return scene_; }
  unsigned animation() const { return animation_; }
  float time() const { return time_; }
  bool playing() const { return playing_; }
  void set_playing(bool value) { playing_ = value; }
  void set_looping(bool value) { looping_ = value; }
  u64 revision() const { return revision_; }
  const Bounds &bounds() const { return bounds_; }
  auto camera_frame(AssetId id, float focus_distance = 1) const
      -> Option<CameraFrame> {
    auto node = asset_->node(id);
    if (node.is_none() || (**node).camera == invalid_index ||
        (**node).camera >= count(asset_->cameras))
      return None();
    const auto &camera = at(asset_->cameras, (**node).camera);
    return vecgltf::camera_frame(pose(id.index).world, camera, focus_distance);
  }
  auto camera_view(AssetId id, float aspect) const -> Option<View> {
    auto frame = camera_frame(id);
    return frame.is_some() ? frame->view(aspect) : None();
  }
  bool set_scene(unsigned index) {
    if (index >= count(asset_->scenes))
      return false;
    scene_ = index;
    visible_.clear();
    visible_.resize(asset_->nodes.len(), false);
    for (auto root : at(asset_->scenes, index).roots)
      visit(root);
    evaluate();
    return true;
  }
  bool set_animation(unsigned index) {
    if (index != invalid_index && index >= count(asset_->animations))
      return false;
    animation_ = index;
    time_ = index == invalid_index ? 0 : at(asset_->animations, index).start;
    evaluate();
    return true;
  }
  void set_time(float value) {
    if (!rstd::f32(value).is_finite())
      return;
    if (animation_ != invalid_index) {
      const auto &animation = at(asset_->animations, animation_);
      value = clamp(value, animation.start, animation.end);
    }
    time_ = value;
    evaluate();
  }
  void update(float delta) {
    if (!playing_ || animation_ == invalid_index ||
        !rstd::f32(delta).is_finite() || delta == 0)
      return;
    const auto &animation = at(asset_->animations, animation_);
    float value = time_ + delta, duration = animation.end - animation.start;
    if (looping_ && duration > 0) {
      float cycles = rstd::f32((value - animation.start) / duration)
                         .floor()
                         .to_primitive();
      value -= cycles * duration;
    }
    set_time(value);
  }
  Vec3 world_position(unsigned node_index, const Primitive &primitive,
                      unsigned vertex_index) const {
    const auto &node = at(asset_->nodes, node_index);
    const auto &pose = at(poses_, node_index);
    const auto &vertex = at(primitive.vertices, vertex_index);
    auto p = vertex.position;
    for (unsigned i = 0; i < count(primitive.morphs) && i < count(pose.weights);
         ++i)
      p = p + at(at(primitive.morphs, i).positions, vertex_index) *
                  at(pose.weights, i);
    if (node.skin == invalid_index)
      return pose.world.point(p);
    Vec3 result{};
    float weights[]{vertex.weights.x, vertex.weights.y, vertex.weights.z,
                    vertex.weights.w};
    for (unsigned j = 0; j < 4; ++j)
      if (weights[j] > 0 && vertex.joints[j] < count(pose.skin_palette)) {
        result = result +
                 at(pose.skin_palette, vertex.joints[j]).point(p) * weights[j];
      }
    return result;
  }
};

struct SceneCameraSource {
  const SceneInstance *instance{};
  AssetId node;
  float focus_distance{1};
};
} // namespace vecgltf

export namespace rstd {
template <>
struct Impl<vecgltf::CameraSource, vecgltf::SceneCameraSource>
    : ImplBase<vecgltf::SceneCameraSource> {
  auto Snapshot(float aspect) const -> Option<vecgltf::CameraFrame> {
    const auto &source = this->self();
    if (!source.instance || !vecgltf::camera_detail::finite(aspect) ||
        aspect <= 0)
      return None();
    return source.instance->camera_frame(source.node, source.focus_distance);
  }
};
} // namespace rstd
