export module vecgltf.render.shadow;
export import vecgltf.render.options;

using namespace rstd::literals;

namespace vecgltf::shadow::detail
{
auto error(ref<str> message, ErrorKind kind = ErrorKind::InvalidAsset) -> Error {
    return { kind, {}, "directional shadow"_Str, String::make(message) };
}
bool finite(Vec3 value) {
    return rstd::f32(value.x).is_finite() && rstd::f32(value.y).is_finite() &&
           rstd::f32(value.z).is_finite();
}
bool finite(const Mat4& value) {
    for (float component : value.m)
        if (! rstd::f32(component).is_finite()) return false;
    return true;
}
} // namespace vecgltf::shadow::detail

export namespace vecgltf::shadow
{
struct ShadowLight {
    unsigned          index {};
    Vec3              direction;
    DirectionalShadow options;
};
struct ShadowView {
    unsigned          light_index {}, x {}, y {}, resolution {};
    Mat4              view, projection, matrix;
    DirectionalShadow options;
};
struct ShadowPlan {
    unsigned        width {}, height {};
    Vec<ShadowView> views;
};
auto ValidateShadow(const DirectionalShadow& options, unsigned max_extent) -> Result<empty, Error> {
    auto nonnegative = [](float value) {
        return rstd::f32(value).is_finite() && value >= 0;
    };
    if (! options.resolution || options.resolution > max_extent ||
        (options.filter != ShadowFilter::Nearest && options.filter != ShadowFilter::Bilinear) ||
        ! nonnegative(options.bias) || ! nonnegative(options.slope_bias) ||
        ! nonnegative(options.max_bias) || ! nonnegative(options.normal_bias) ||
        options.max_bias < options.bias)
        return Err(detail::error("invalid shadow options"_str));
    return Ok(empty {});
}
// Directions are normalized world-space vectors toward the lights; bounds use the render origin.
auto BuildShadowPlan(slice<ShadowLight> lights, const Bounds& bounds, unsigned max_extent)
    -> Result<ShadowPlan, Error> {
    unsigned cell {};
    for (const auto& light : lights) {
        auto valid = ValidateShadow(light.options, max_extent);
        if (valid.is_err()) return Err(valid.unwrap_err());
        if (light.options.resolution > cell) cell = light.options.resolution;
    }
    ShadowPlan plan;
    if (lights.is_empty() || ! bounds.valid) return Ok(rstd::move(plan));
    if (! detail::finite(bounds.min) || ! detail::finite(bounds.max) ||
        bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z)
        return Err(detail::error("invalid caster bounds"_str));
    auto count = lights.len().to_primitive();
    if (count > rstd::uint64_t(max_extent) * max_extent)
        return Err(detail::error("shadow atlas exceeds texture extent"_str, ErrorKind::Limit));
    unsigned low = 1, high = max_extent;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (rstd::uint64_t(middle) * middle >= count)
            high = middle;
        else
            low = middle + 1;
    }
    unsigned columns = low, rows = unsigned((count + columns - 1) / columns);
    if (columns > max_extent / cell || rows > max_extent / cell)
        return Err(detail::error("shadow atlas exceeds texture extent"_str, ErrorKind::Limit));
    plan.width  = columns * cell;
    plan.height = rows * cell;
    Vec3  center { bounds.min.x * 0.5f + bounds.max.x * 0.5f,
                   bounds.min.y * 0.5f + bounds.max.y * 0.5f,
                   bounds.min.z * 0.5f + bounds.max.z * 0.5f };
    Vec3  half { bounds.max.x * 0.5f - bounds.min.x * 0.5f,
                 bounds.max.y * 0.5f - bounds.min.y * 0.5f,
                 bounds.max.z * 0.5f - bounds.min.z * 0.5f };
    float radius =
        float(rstd::f64(double(half.x) * half.x + double(half.y) * half.y + double(half.z) * half.z)
                  .sqrt()
                  .to_primitive());
    if (! rstd::f32(radius).is_finite())
        return Err(detail::error("caster bounds exceed shadow precision"_str));
    radius        = maximum(radius, 0.0001f);
    float padding = maximum(radius * 0.01f, 0.0001f);
    for (usize i {}; i < lights.len(); ++i) {
        const auto& light = lights[i];
        if (! detail::finite(light.direction) || dot(light.direction, light.direction) <= 1e-20f)
            return Err(detail::error("invalid shadow direction"_str));
        Vec3 eye = center + light.direction * (2 * radius);
        if (! detail::finite(eye))
            return Err(detail::error("caster bounds exceed shadow precision"_str));
        Vec3       up = absolute(light.direction.y) > 0.99f ? Vec3 { 0, 0, 1 } : Vec3 { 0, 1, 0 };
        ShadowView view;
        view.light_index = light.index;
        view.x           = unsigned(i.to_primitive() % columns) * cell;
        view.y           = unsigned(i.to_primitive() / columns) * cell;
        view.resolution  = light.options.resolution;
        view.options     = light.options;
        view.view        = look_at(eye, center, up);
        float near = rstd::f32::INFINITY_.to_primitive(), far = -near;
        for (unsigned corner = 0; corner < 8; ++corner) {
            Vec3  point { corner & 1 ? bounds.max.x : bounds.min.x,
                          corner & 2 ? bounds.max.y : bounds.min.y,
                          corner & 4 ? bounds.max.z : bounds.min.z };
            float distance = -view.view.point(point).z;
            near           = minimum(near, distance);
            far            = maximum(far, distance);
        }
        near = maximum(0, near - padding);
        far  = maximum(near + padding, far + padding);
        if (! rstd::f32(near).is_finite() || ! rstd::f32(far).is_finite() || far <= near)
            return Err(detail::error("caster bounds exceed shadow precision"_str));
        view.projection       = Mat4::Identity();
        view.projection.m[0]  = 1 / radius;
        view.projection.m[5]  = -1 / radius;
        view.projection.m[10] = 1 / (near - far);
        view.projection.m[14] = near / (near - far);
        view.matrix           = view.projection * view.view;
        if (! detail::finite(view.view) || ! detail::finite(view.projection) ||
            ! detail::finite(view.matrix))
            return Err(detail::error("caster bounds exceed shadow precision"_str));
        plan.views.push(rstd::move(view));
    }
    return Ok(rstd::move(plan));
}
} // namespace vecgltf::shadow
