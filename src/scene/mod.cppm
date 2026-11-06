export module vecgltf.scene;
export import vecgltf.camera;

export namespace vecgltf
{
enum class SceneBoundsMode
{
    Exact,
    Conservative
};
struct NodePose {
    Vec3        translation, scale;
    Vec4        rotation;
    Mat4        world { Mat4::Identity() };
    Vec<float>  weights;
    Vec<Mat4>   skin_palette;
    Vec<Bounds> primitive_bounds;
};
class SceneInstance {
    Arc<Asset>      asset_;
    Vec<NodePose>   poses_;
    Vec<unsigned>   order_;
    Vec<bool>       visible_;
    unsigned        scene_ {}, animation_ { invalid_index };
    float           time_ {};
    bool            playing_ { true }, looping_ { true };
    SceneBoundsMode bounds_mode_ { SceneBoundsMode::Exact };
    u64             revision_ { 1 };
    Bounds          bounds_;

    struct Position {
        double x, y, z;
    };
    static auto relative_point(const Mat4& matrix, Vec3 point, Vec3 origin) -> Position {
        const auto& m = matrix.m;
        double      w =
            double(m[3]) * point.x + double(m[7]) * point.y + double(m[11]) * point.z + m[15];
        return { ((double(m[12]) - double(origin.x) * w) + double(m[0]) * point.x +
                  double(m[4]) * point.y + double(m[8]) * point.z) /
                     w,
                 ((double(m[13]) - double(origin.y) * w) + double(m[1]) * point.x +
                  double(m[5]) * point.y + double(m[9]) * point.z) /
                     w,
                 ((double(m[14]) - double(origin.z) * w) + double(m[2]) * point.x +
                  double(m[6]) * point.y + double(m[10]) * point.z) /
                     w };
    }

    void visit(unsigned index) {
        if (at(visible_, index)) return;
        at(visible_, index) = true;
        for (auto child : at(asset_->nodes, index).children) visit(child);
    }
    void order(unsigned index, Vec<bool>& added) {
        if (at(added, index)) return;
        auto parent = at(asset_->nodes, index).parent;
        if (parent != invalid_index) order(parent, added);
        at(added, index) = true;
        order_.push(unsigned(index));
    }
    void sample(const AnimationSampler& sampler, AnimationPath path, NodePose& pose) {
        unsigned left = 0, right = 0;
        while (right + 1 < count(sampler.times) && at(sampler.times, right + 1) <= time_) ++right;
        left = right;
        if (right + 1 < count(sampler.times)) ++right;
        float duration = at(sampler.times, right) - at(sampler.times, left);
        float t = duration > 0 ? clamp((time_ - at(sampler.times, left)) / duration, 0, 1) : 0;
        auto  components = sampler.components;
        auto  value      = [&](unsigned key, unsigned component, unsigned part) {
            return at(
                sampler.values,
                (rstd::size_t(key) * (sampler.interpolation == Interpolation::CubicSpline ? 3 : 1) +
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
                pose.rotation = slerp(
                    { value(left, 0, 0), value(left, 1, 0), value(left, 2, 0), value(left, 3, 0) },
                    { value(right, 0, 0),
                      value(right, 1, 0),
                      value(right, 2, 0),
                      value(right, 3, 0) },
                    t);
            else
                pose.rotation =
                    normalized(Vec4 { at(result, 0), at(result, 1), at(result, 2), at(result, 3) });
        } else {
            auto v = Vec3 { at(result, 0), at(result, 1), at(result, 2) };
            if (path == AnimationPath::Translation)
                pose.translation = v;
            else
                pose.scale = v;
        }
    }
    void evaluate() {
        for (unsigned i = 0; i < count(poses_); ++i) {
            const auto& node = at(asset_->nodes, i);
            auto&       pose = at(poses_, i);
            pose.translation = node.translation;
            pose.rotation    = node.rotation;
            pose.scale       = node.scale;
            pose.weights     = node.weights.clone();
        }
        if (animation_ != invalid_index) {
            const auto& animation = at(asset_->animations, animation_);
            for (const auto& channel : animation.channels)
                sample(at(animation.samplers, channel.sampler),
                       channel.path,
                       at(poses_, channel.node));
        }
        for (auto index : order_) {
            const auto& node = at(asset_->nodes, index);
            auto&       pose = at(poses_, index);
            auto local = node.has_matrix ? node.matrix
                                         : compose(pose.translation, pose.rotation, pose.scale);
            pose.world =
                node.parent == invalid_index ? local : at(poses_, node.parent).world * local;
        }
        for (unsigned i = 0; i < count(poses_); ++i) {
            auto& pose = at(poses_, i);
            pose.skin_palette.clear();
            auto skin_index = at(asset_->nodes, i).skin;
            if (skin_index == invalid_index) continue;
            const auto& skin = at(asset_->skins, skin_index);
            for (unsigned joint = 0; joint < count(skin.joints); ++joint)
                pose.skin_palette.push(at(poses_, at(skin.joints, joint)).world *
                                       at(skin.inverse_bind, joint));
        }
        bounds_ = {};
        for (unsigned i = 0; i < count(poses_); ++i) {
            auto& pose = at(poses_, i);
            pose.primitive_bounds.clear();
            const auto& node = at(asset_->nodes, i);
            if (! at(visible_, i) || node.mesh == invalid_index) continue;
            const auto& mesh = at(asset_->meshes, node.mesh);
            for (unsigned p = 0; p < count(mesh.primitives); ++p) {
                auto bounds = selected_primitive_bounds(i, p, {});
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
    explicit SceneInstance(Arc<Asset> asset, SceneBoundsMode bounds_mode = SceneBoundsMode::Exact)
        : asset_(rstd::move(asset)), bounds_mode_(bounds_mode) {
        for (const auto& node : asset_->nodes)
            poses_.push({ node.translation,
                          node.scale,
                          node.rotation,
                          Mat4::Identity(),
                          node.weights.clone() });
        Vec<bool> added;
        added.resize(asset_->nodes.len(), false);
        for (unsigned i = 0; i < count(asset_->nodes); ++i) order(i, added);
        set_scene(asset_->default_scene);
    }
    const Asset&    asset() const { return *asset_; }
    auto            asset_owner() const -> Arc<Asset> { return asset_.clone(); }
    const NodePose& pose(unsigned node) const { return at(poses_, node); }
    bool     visible(unsigned node) const { return node < count(visible_) && at(visible_, node); }
    unsigned scene() const { return scene_; }
    auto     scene_root() const -> Option<AssetId> {
        if (scene_ >= count(asset_->scenes)) return None();
        const auto& roots = at(asset_->scenes, scene_).roots;
        if (roots.len() != usize(1) || ! visible(at(roots, 0))) return None();
        return Some(asset_->node_id(at(roots, 0)));
    }
    unsigned        animation() const { return animation_; }
    float           time() const { return time_; }
    bool            playing() const { return playing_; }
    void            set_playing(bool value) { playing_ = value; }
    void            set_looping(bool value) { looping_ = value; }
    u64             revision() const { return revision_; }
    const Bounds&   bounds() const { return bounds_; }
    SceneBoundsMode bounds_mode() const { return bounds_mode_; }
    void            set_bounds_mode(SceneBoundsMode value) {
        if (value == bounds_mode_) return;
        bounds_mode_ = value;
        evaluate();
    }
    auto exact_bounds(Vec3 origin = {}) const -> Bounds {
        Bounds result;
        for (unsigned i = 0; i < count(asset_->nodes); ++i) {
            const auto& node = at(asset_->nodes, i);
            if (! visible(i) || node.mesh == invalid_index || node.mesh >= count(asset_->meshes))
                continue;
            const auto& mesh = at(asset_->meshes, node.mesh);
            for (unsigned p = 0; p < count(mesh.primitives); ++p) {
                auto bounds = relative_primitive_bounds(i, p, origin);
                if (bounds.valid) {
                    result.include(bounds.min);
                    result.include(bounds.max);
                }
            }
        }
        return result;
    }
    auto camera_frame(AssetId id, float focus_distance = 1) const -> Option<CameraFrame> {
        auto node = asset_->node(id);
        if (node.is_none() || (**node).camera == invalid_index ||
            (**node).camera >= count(asset_->cameras))
            return None();
        const auto& camera = at(asset_->cameras, (**node).camera);
        return vecgltf::camera_frame(pose(id.index).world, camera, focus_distance);
    }
    // None uses world space; otherwise positions and projection lengths use the node's local units.
    auto camera_frame(CameraLookAt     value,
                      CameraProjection projection = {},
                      Vec3             up         = { 0, 1, 0 },
                      Option<AssetId>  reference  = None()) const -> Option<CameraFrame> {
        auto local = vecgltf::camera_frame(value, projection, up);
        if (local.is_none() || reference.is_none()) return local;
        auto id = reference.unwrap();
        if (asset_->node(id).is_none() || ! visible(id.index)) return None();
        const auto& world = pose(id.index).world;
        if (vecgltf::camera_frame(world, projection).is_none()) return None();
        double squared[3] {};
        for (unsigned column = 0; column < 3; ++column)
            for (unsigned row = 0; row < 3; ++row) {
                double component = world.m[column * 4 + row];
                squared[column] += component * component;
            }
        for (unsigned column = 1; column < 3; ++column)
            if (squared[column] < squared[0] * 0.9999 || squared[column] > squared[0] * 1.0001)
                return None();
        double scale   = rstd::f64(squared[0]).sqrt().to_primitive();
        auto   convert = [&](float& length) {
            double converted = double(length) * scale;
            double limit     = rstd::f32::MAX.to_primitive();
            if (converted < -limit || converted > limit) return false;
            length = float(converted);
            return true;
        };
        float focus_distance = local->focus_distance;
        if (! convert(projection.near) || ! convert(projection.far) || ! convert(focus_distance) ||
            (projection.orthographic && (! convert(projection.xmag) || ! convert(projection.ymag))))
            return None();
        auto camera_world = world * compose(local->position, local->orientation, { 1, 1, 1 });
        return vecgltf::camera_frame(camera_world, projection, focus_distance);
    }
    auto camera_view(AssetId id, float aspect) const -> Option<View> {
        auto frame = camera_frame(id);
        return frame.is_some() ? frame->view(aspect) : None();
    }
    bool set_scene(unsigned index) {
        if (index >= count(asset_->scenes)) return false;
        scene_ = index;
        visible_.clear();
        visible_.resize(asset_->nodes.len(), false);
        for (auto root : at(asset_->scenes, index).roots) visit(root);
        evaluate();
        return true;
    }
    bool set_animation(unsigned index) {
        if (index != invalid_index && index >= count(asset_->animations)) return false;
        animation_ = index;
        time_      = index == invalid_index ? 0 : at(asset_->animations, index).start;
        evaluate();
        return true;
    }
    void set_time(float value) {
        if (! rstd::f32(value).is_finite()) return;
        if (animation_ != invalid_index) {
            const auto& animation = at(asset_->animations, animation_);
            value                 = clamp(value, animation.start, animation.end);
        }
        time_ = value;
        evaluate();
    }
    void update(float delta) {
        if (! playing_ || animation_ == invalid_index || ! rstd::f32(delta).is_finite() ||
            delta == 0)
            return;
        const auto& animation = at(asset_->animations, animation_);
        float       value = time_ + delta, duration = animation.end - animation.start;
        if (looping_ && duration > 0) {
            float cycles = rstd::f32((value - animation.start) / duration).floor().to_primitive();
            value -= cycles * duration;
        }
        set_time(value);
    }
    Vec3 world_position(unsigned         node_index,
                        const Primitive& primitive,
                        unsigned         vertex_index,
                        Vec3             origin = {}) const {
        const auto& node   = at(asset_->nodes, node_index);
        const auto& pose   = at(poses_, node_index);
        const auto& vertex = at(primitive.vertices, vertex_index);
        auto        p      = vertex.position;
        for (unsigned i = 0; i < count(primitive.morphs) && i < count(pose.weights); ++i)
            p = p + at(at(primitive.morphs, i).positions, vertex_index) * at(pose.weights, i);
        if (node.skin == invalid_index) {
            auto result = relative_point(pose.world, p, origin);
            return { float(result.x), float(result.y), float(result.z) };
        }
        Position result {};
        float weights[] { vertex.weights.x, vertex.weights.y, vertex.weights.z, vertex.weights.w };
        for (unsigned j = 0; j < 4; ++j)
            if (weights[j] > 0 && vertex.joints[j] < count(pose.skin_palette)) {
                auto point = relative_point(at(pose.skin_palette, vertex.joints[j]), p, origin);
                result.x += point.x * weights[j];
                result.y += point.y * weights[j];
                result.z += point.z * weights[j];
            }
        return { float(result.x), float(result.y), float(result.z) };
    }
    auto relative_primitive_bounds(unsigned node_index, unsigned primitive_index, Vec3 origin) const
        -> Bounds {
        Bounds result;
        if (! visible(node_index)) return result;
        const auto& node = at(asset_->nodes, node_index);
        if (node.mesh == invalid_index || node.mesh >= count(asset_->meshes)) return result;
        const auto& mesh = at(asset_->meshes, node.mesh);
        if (primitive_index >= count(mesh.primitives)) return result;
        const auto& primitive = at(mesh.primitives, primitive_index);
        if (node.skin != invalid_index || ! primitive.morphs.is_empty()) {
            for (unsigned v = 0; v < count(primitive.vertices); ++v)
                result.include(world_position(node_index, primitive, v, origin));
        } else if (primitive.bounds.valid) {
            const auto& bounds = primitive.bounds;
            const auto& world  = at(poses_, node_index).world;
            for (unsigned corner = 0; corner < 8; ++corner) {
                auto point = relative_point(world,
                                            { corner & 1 ? bounds.max.x : bounds.min.x,
                                              corner & 2 ? bounds.max.y : bounds.min.y,
                                              corner & 4 ? bounds.max.z : bounds.min.z },
                                            origin);
                result.include({ float(point.x), float(point.y), float(point.z) });
            }
        }
        return result;
    }
    auto selected_primitive_bounds(unsigned node_index, unsigned primitive_index, Vec3 origin) const
        -> Bounds {
        if (bounds_mode_ != SceneBoundsMode::Conservative || ! visible(node_index))
            return relative_primitive_bounds(node_index, primitive_index, origin);
        const auto& node = at(asset_->nodes, node_index);
        if (node.skin == invalid_index || node.mesh == invalid_index ||
            node.mesh >= count(asset_->meshes))
            return relative_primitive_bounds(node_index, primitive_index, origin);
        const auto& mesh = at(asset_->meshes, node.mesh);
        if (primitive_index >= count(mesh.primitives))
            return relative_primitive_bounds(node_index, primitive_index, origin);
        const auto& primitive = at(mesh.primitives, primitive_index);
        if (primitive.joint_bounds.is_empty() || ! primitive.morphs.is_empty())
            return relative_primitive_bounds(node_index, primitive_index, origin);
        const auto& palette = at(poses_, node_index).skin_palette;
        Bounds      result;
        double      scale[] { 0, 0, 0 };
        auto        abs_double = [](double value) {
            return value < 0 ? -value : value;
        };
        for (const auto& joint : primitive.joint_bounds) {
            if (! joint.bounds.valid || joint.joint >= count(palette))
                return relative_primitive_bounds(node_index, primitive_index, origin);
            const auto& matrix = at(palette, joint.joint);
            for (float value : matrix.m)
                if (! rstd::f32(value).is_finite())
                    return relative_primitive_bounds(node_index, primitive_index, origin);
            if (matrix.m[3] != 0 || matrix.m[7] != 0 || matrix.m[11] != 0 || matrix.m[15] != 1)
                return relative_primitive_bounds(node_index, primitive_index, origin);
            for (unsigned corner = 0; corner < 8; ++corner) {
                Vec3  point { corner & 1 ? joint.bounds.max.x : joint.bounds.min.x,
                              corner & 2 ? joint.bounds.max.y : joint.bounds.min.y,
                              corner & 4 ? joint.bounds.max.z : joint.bounds.min.z };
                auto  p = relative_point(matrix, point, origin);
                float origins[] { origin.x, origin.y, origin.z };
                for (unsigned axis = 0; axis < 3; ++axis) {
                    double terms = abs_double(double(matrix.m[axis]) * point.x) +
                                   abs_double(double(matrix.m[4 + axis]) * point.y) +
                                   abs_double(double(matrix.m[8 + axis]) * point.z) +
                                   abs_double(double(matrix.m[12 + axis]) - origins[axis]);
                    if (terms > scale[axis]) scale[axis] = terms;
                }
                Vec3 value { float(p.x), float(p.y), float(p.z) };
                if (! rstd::f32(value.x).is_finite() || ! rstd::f32(value.y).is_finite() ||
                    ! rstd::f32(value.z).is_finite())
                    return relative_primitive_bounds(node_index, primitive_index, origin);
                result.include(value);
            }
        }
        if (! result.valid) return relative_primitive_bounds(node_index, primitive_index, origin);
        auto pad = [&](float& lower, float& upper, double term_scale) {
            float magnitude = maximum(absolute(lower), absolute(upper));
            float margin    = float(double(magnitude) * primitive.joint_weight_error +
                                    term_scale * (16 * double(rstd::f32::EPSILON.to_primitive())));
            lower           = rstd::f32(lower - margin).next_down().to_primitive();
            upper           = rstd::f32(upper + margin).next_up().to_primitive();
            return rstd::f32(lower).is_finite() && rstd::f32(upper).is_finite();
        };
        if (! pad(result.min.x, result.max.x, scale[0]) ||
            ! pad(result.min.y, result.max.y, scale[1]) ||
            ! pad(result.min.z, result.max.z, scale[2]))
            return relative_primitive_bounds(node_index, primitive_index, origin);
        return result;
    }
};

struct SceneCameraSource {
    const SceneInstance* instance {};
    AssetId              node;
    float                focus_distance { 1 };
};
} // namespace vecgltf

export namespace rstd
{
template<>
struct Impl<vecgltf::CameraSource, vecgltf::SceneCameraSource>
    : ImplBase<vecgltf::SceneCameraSource> {
    auto Snapshot(float aspect) const -> Option<vecgltf::CameraFrame> {
        const auto& source = this->self();
        if (! source.instance || ! vecgltf::camera_detail::finite(aspect) || aspect <= 0)
            return None();
        return source.instance->camera_frame(source.node, source.focus_distance);
    }
};
} // namespace rstd
