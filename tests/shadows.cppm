module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:shadows;
import vecgltf;
import vecgltf.render;
import rstd.test;
import :coverage;

using namespace vecgltf;

namespace vecgltf_test::shadows
{
namespace vk    = vrento::vulkan;
namespace tex   = vrento::texture;
namespace image = vecgltf_test::coverage;

void near(float actual, float expected, float tolerance = 0.002f) {
    rstd_assert(absolute(actual - expected) < tolerance);
}
void near(Vec3 actual, Vec3 expected) {
    near(actual.x, expected.x);
    near(actual.y, expected.y);
    near(actual.z, expected.z);
}
auto settings(bool enabled = true, LightSpace space = LightSpace::World, unsigned resolution = 128)
    -> SceneOptions {
    DirectionalLight light;
    light.direction = normalized(Vec3 { 0.6f, 0, 1 });
    light.color     = { 1, 1, 1 };
    light.intensity = 3;
    light.space     = space;
    if (enabled) {
        DirectionalShadow shadow;
        shadow.resolution = resolution;
        shadow.filter     = ShadowFilter::Nearest;
        light.shadow      = Some(rstd::move(shadow));
    }
    SceneOptions result;
    result.direct_light  = Some(rstd::move(light));
    result.ibl_intensity = 0;
    return result;
}
auto view(Vec3 origin = {}, bool rolled = false, float aspect = 1) -> View {
    CameraProjection projection;
    projection.orthographic = true;
    projection.xmag = projection.ymag = 1;
    projection.near                   = 0.1f;
    projection.far                    = 10;
    return camera_frame(CameraLookAt { origin + Vec3 { 0, 0, 3 }, origin },
                        projection,
                        rolled ? Vec3 { 0, -1, 0 } : Vec3 { 0, 1, 0 })
        .unwrap()
        .view(aspect)
        .unwrap();
}
void quad(Asset&   source,
          float    extent,
          float    z,
          Material material,
          Vec3     origin,
          bool     reverse  = false,
          bool     mirrored = false) {
    Primitive primitive;
    primitive.material = unsigned(count(source.materials));
    source.materials.push(rstd::move(material));
    Vec3     positions[] { { -extent, -extent, z },
                           { extent, -extent, z },
                           { extent, extent, z },
                           { -extent, extent, z } };
    Vec2     uvs[] { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    unsigned indices[] { 0, 1, 2, 0, 2, 3 };
    for (unsigned i = 0; i < 6; ++i) {
        auto   index = indices[reverse ? (i / 3) * 3 + 2 - i % 3 : i];
        Vertex vertex;
        vertex.position = positions[index];
        vertex.normal   = { 0, 0, 1 };
        vertex.uv0      = uvs[index];
        vertex.weights  = { 1, 0, 0, 0 };
        primitive.vertices.push(rstd::move(vertex));
        primitive.bounds.include(positions[index]);
    }
    Mesh mesh;
    mesh.primitives.push(rstd::move(primitive));
    Node node;
    node.mesh        = unsigned(count(source.meshes));
    node.translation = origin;
    if (mirrored) node.scale.x = -1;
    source.meshes.push(rstd::move(mesh));
    at(source.scenes, 0).roots.push(unsigned(count(source.nodes)));
    source.nodes.push(rstd::move(node));
}
auto scene_asset(AlphaMode mode         = AlphaMode::Opaque,
                 float     alpha        = 1,
                 bool      reverse      = false,
                 bool      double_sided = false,
                 bool      mirrored     = false,
                 unsigned  deformation  = 0,
                 Vec3      origin       = {}) -> Arc<Asset> {
    static unsigned generation {};
    auto            source = Arc<Asset>::make();
    source->generation     = u64(++generation);
    source->scenes.push(Scene {});
    Material receiver;
    receiver.base_color = { 1, 1, 1, 1 };
    receiver.metallic   = 0;
    receiver.roughness  = 1;
    quad(*source, 1, 0, rstd::move(receiver), origin);
    Material caster;
    caster.base_color   = { 1, 1, 1, alpha };
    caster.metallic     = 0;
    caster.roughness    = 1;
    caster.alpha        = mode;
    caster.double_sided = double_sided;
    quad(*source, 0.12f, 0.5f, rstd::move(caster), origin, reverse, mirrored);
    if (! deformation) return source;
    Animation clip;
    clip.end = 1;
    if (deformation & 1) {
        auto&       primitive = at(at(source->meshes, 1).primitives, 0);
        MorphTarget target;
        for (unsigned i = 0; i < count(primitive.vertices); ++i) {
            target.positions.push({ deformation & 2 ? 0.3f : 0.6f, 0, 0 });
            target.normals.push({});
            target.tangents.push({});
        }
        primitive.morphs.push(rstd::move(target));
        at(source->nodes, 1).weights.push(0.0f);
        AnimationSampler sampler;
        sampler.components = 1;
        sampler.times.push(0.0f);
        sampler.times.push(1.0f);
        sampler.values.push(0.0f);
        sampler.values.push(1.0f);
        clip.channels.push({ 1, unsigned(count(clip.samplers)), AnimationPath::Weights });
        clip.samplers.push(rstd::move(sampler));
    }
    if (deformation & 2) {
        Node joint;
        joint.translation = origin;
        source->nodes.push(rstd::move(joint));
        at(source->scenes, 0).roots.push(2u);
        at(source->nodes, 1).skin                               = 0;
        at(at(source->meshes, 1).primitives, 0).skin_attributes = true;
        Skin skin;
        skin.joints.push(2u);
        skin.inverse_bind.push(Mat4::Identity());
        source->skins.push(rstd::move(skin));
        AnimationSampler sampler;
        sampler.components = 3;
        sampler.times.push(0.0f);
        sampler.times.push(1.0f);
        float values[] { origin.x, origin.y, origin.z, origin.x + (deformation & 1 ? 0.3f : 0.6f),
                         origin.y, origin.z };
        for (float value : values) sampler.values.push(float(value));
        clip.channels.push({ 2, unsigned(count(clip.samplers)), AnimationPath::Translation });
        clip.samplers.push(rstd::move(sampler));
    }
    source->animations.push(rstd::move(clip));
    return source;
}
auto draw(Renderer&                   renderer,
          vk::TextureContext&         queue,
          vk::DeviceSession           session,
          tex::TextureViewRef         target,
          const PreparedAsset&        prepared,
          const SceneInstance&        scene,
          SceneOptions                options,
          View                        camera   = view(),
          Option<tex::TextureViewRef> depth    = None(),
          Option<DepthEncoding>       encoding = None()) -> Vec<u8> {
    RenderTarget destination { target };
    destination.depth          = rstd::move(depth);
    destination.depth_encoding = rstd::move(encoding);
    auto work                  = queue.Begin().unwrap();
    renderer
        .record_scene(work, rstd::move(destination), prepared, scene, camera, rstd::move(options))
        .unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    return image::pixels(session, queue, target);
}
void occluded(const Vec<u8>& on,
              const Vec<u8>& off,
              unsigned       x     = 22,
              unsigned       y     = 32,
              unsigned       width = 64) {
    auto shadowed = image::channel(on, 0, x, y, width);
    auto lit      = image::channel(off, 0, x, y, width);
    rstd_assert(lit > 0.1f && shadowed < lit * 0.25f);
}

TEST(Shadow, RelativePrimitiveBounds) {
    Vec3 origin { 1e8f, -1e8f, 0 };
    for (unsigned deformation = 0; deformation < 4; ++deformation) {
        auto local   = scene_asset(AlphaMode::Opaque, 1, false, false, false, deformation);
        auto distant = scene_asset(AlphaMode::Opaque, 1, false, false, false, deformation, origin);
        if (deformation & 1) {
            at(local->nodes, 1).weights.clear();
            at(local->nodes, 1).weights.push(0.5f);
            at(distant->nodes, 1).weights.clear();
            at(distant->nodes, 1).weights.push(0.5f);
        }
        SceneInstance a(local.clone()), b(distant.clone());
        for (unsigned node = 0; node < 2; ++node) {
            auto expected = a.relative_primitive_bounds(node, 0, {});
            auto actual   = b.relative_primitive_bounds(node, 0, origin);
            rstd_assert(expected.valid && actual.valid);
            near(actual.min, expected.min);
            near(actual.max, expected.max);
            near(at(b.pose(node).primitive_bounds, 0).min.z, actual.min.z);
        }
        const auto& primitive = at(at(distant->meshes, 1).primitives, 0);
        near(b.world_position(1, primitive, 0, origin),
             a.world_position(1, at(at(local->meshes, 1).primitives, 0), 0));
        rstd_assert(! b.relative_primitive_bounds(99, 0, origin).valid);
        rstd_assert(! b.relative_primitive_bounds(0, 99, origin).valid);
    }
}

TEST(Shadow, OcclusionAndDepth) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = scene_asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue);
    auto          off    = draw(renderer, queue, session, target, prepared, scene, settings(false));
    auto          on     = draw(renderer, queue, session, target, prepared, scene, settings());
    occluded(on, off);
    near(image::channel(on, 0, 50, 32), image::channel(off, 0, 50, 32));
    auto filtered                         = settings();
    filtered.direct_light->shadow->filter = ShadowFilter::Bilinear;
    occluded(draw(renderer, queue, session, target, prepared, scene, rstd::move(filtered)), off);
    tex::TextureDesc desc;
    desc.extent         = { 64, 64 };
    desc.format         = vrento::TextureFormat::D32F;
    desc.usage          = tex::Usage::DepthAttachment;
    auto          depth = queue
                              .CreateView(queue.CreateTexture(desc).unwrap(),
                                          { tex::Dimension::D2, { tex::Aspect::Depth } })
                              .unwrap();
    DepthEncoding encodings[] { DepthEncoding::Forward, DepthEncoding::Reversed };
    for (auto encoding : encodings) {
        auto result = draw(renderer,
                           queue,
                           session,
                           target,
                           prepared,
                           scene,
                           settings(),
                           view(),
                           Some(tex::TextureViewRef(depth)),
                           Some(encoding));
        occluded(result, off);
        near(image::channel(result, 0, 22, 32), image::channel(on, 0, 22, 32));
    }
    auto ambient_on = settings(), ambient_off = settings(false);
    ambient_on.direct_light->intensity = ambient_off.direct_light->intensity = 0;
    ambient_on.ibl_intensity = ambient_off.ibl_intensity = 1;
    auto a = draw(renderer, queue, session, target, prepared, scene, rstd::move(ambient_on));
    auto b = draw(renderer, queue, session, target, prepared, scene, rstd::move(ambient_off));
    near(image::channel(a, 0, 22, 32), image::channel(b, 0, 22, 32));
    rstd_assert(session.validation_errors() == 0);
}

TEST(Shadow, SpotOcclusion) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = scene_asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target    = image::texture(queue);
    auto          spotlight = [](bool shadow, bool colored = false, bool view_space = false) {
        SpotLight spot;
        spot.point.position    = { 0.9f, 0, 1.5f };
        spot.point.attenuation = { 1, 0, 0 };
        if (colored) spot.point.color = { 0, 1, 0 };
        if (view_space) {
            spot.point.position.z -= 3;
            spot.point.space = LightSpace::View;
        }
        spot.direction = normalized(Vec3 { -0.9f, 0, -1.5f });
        spot.outer_cos = 0.6f;
        spot.inner_cos = 0.9f;
        if (shadow) {
            ShadowOptions options;
            options.resolution = 256;
            options.filter     = ShadowFilter::Nearest;
            spot.shadow        = Some(options);
        }
        Vec<SpotLight> values;
        values.push(rstd::move(spot));
        SceneOptions result;
        result.direct_light  = None();
        result.ibl_intensity = 0;
        result.spot_lights   = SpotLights(rstd::move(values));
        return result;
    };
    const auto off = draw(renderer, queue, session, target, prepared, scene, spotlight(false));
    const auto on  = draw(renderer, queue, session, target, prepared, scene, spotlight(true));
    unsigned   dark {}, lit {};
    for (unsigned y = 5; y < 59; ++y)
        for (unsigned x = 5; x < 59; ++x) {
            float before = image::channel(off, 0, x, y);
            float after  = image::channel(on, 0, x, y);
            if (before > 0.1f && after < before * 0.3f) ++dark;
            if (before > 0.1f && absolute(after - before) < 0.01f) ++lit;
        }
    rstd_assert(dark > 10 && lit > 100);
    const auto view_light =
        draw(renderer, queue, session, target, prepared, scene, spotlight(true, false, true));
    for (unsigned y = 12; y < 54; y += 7)
        for (unsigned x = 12; x < 54; x += 7)
            near(image::channel(on, 0, x, y), image::channel(view_light, 0, x, y), 0.01f);
    auto direction_only                = settings();
    direction_only.direct_light->color = { 1, 0, 0 };
    auto both                          = spotlight(true, true);
    both.direct_light                  = direction_only.direct_light;
    const auto mixed = draw(renderer, queue, session, target, prepared, scene, rstd::move(both));
    const auto directional =
        draw(renderer, queue, session, target, prepared, scene, rstd::move(direction_only));
    const auto spot =
        draw(renderer, queue, session, target, prepared, scene, spotlight(true, true));
    for (unsigned y = 12; y < 54; y += 7)
        for (unsigned x = 12; x < 54; x += 7) {
            near(image::channel(mixed, 0, x, y), image::channel(directional, 0, x, y), 0.01f);
            near(image::channel(mixed, 1, x, y), image::channel(spot, 1, x, y), 0.01f);
        }
    rstd_assert(session.validation_errors() == 0);
}

TEST(Shadow, AlphaAndCulling) {
    auto session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vk::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto target   = image::texture(queue);
    struct Case {
        AlphaMode mode;
        float     alpha;
        bool      reverse, double_sided, mirrored, casts;
    };
    Case cases[] { { AlphaMode::Opaque, 0, false, false, false, true },
                   { AlphaMode::Mask, 0.25f, false, false, false, false },
                   { AlphaMode::Mask, 0.75f, false, false, false, true },
                   { AlphaMode::Blend, 0.5f, false, false, false, false },
                   { AlphaMode::Blend, 1, false, false, false, true },
                   { AlphaMode::Opaque, 1, true, false, false, false },
                   { AlphaMode::Opaque, 1, true, true, false, true },
                   { AlphaMode::Opaque, 1, false, false, true, true } };
    for (const auto& test : cases) {
        auto source =
            scene_asset(test.mode, test.alpha, test.reverse, test.double_sided, test.mirrored);
        auto          prepared = renderer.prepare(source.clone()).unwrap();
        SceneInstance scene(source.clone());
        auto off = draw(renderer, queue, session, target, prepared, scene, settings(false));
        auto on  = draw(renderer, queue, session, target, prepared, scene, settings());
        if (test.casts) {
            occluded(on, off);
        } else {
            near(image::channel(on, 0, 22, 32), image::channel(off, 0, 22, 32));
        }
    }
    AlphaMode modes[] { AlphaMode::Mask, AlphaMode::Blend };
    for (auto mode : modes) {
        auto  source = scene_asset(mode);
        Image pixels;
        pixels.width  = 2;
        pixels.height = 1;
        unsigned values[] {
            255, 255, 255, mode == AlphaMode::Mask ? 0u : 128u, 255, 255, 255, 255
        };
        for (unsigned value : values) pixels.rgba.push(u8(value));
        source->images.push(rstd::move(pixels));
        Texture sampler;
        sampler.image       = 0;
        sampler.sampler.min = sampler.sampler.mag = Filter::Nearest;
        sampler.sampler.mip                       = MipFilter::None;
        source->textures.push(rstd::move(sampler));
        at(source->materials, 1).base.texture = 0;
        auto          prepared                = renderer.prepare(source.clone()).unwrap();
        SceneInstance scene(source.clone());
        auto off = draw(renderer, queue, session, target, prepared, scene, settings(false));
        auto on  = draw(renderer, queue, session, target, prepared, scene, settings());
        near(image::channel(on, 0, 20, 32), image::channel(off, 0, 20, 32));
        occluded(on, off, 24);
    }
    rstd_assert(session.validation_errors() == 0);
}

TEST(Shadow, AnimationAndCamera) {
    auto session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vk::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto target   = image::texture(queue);
    for (unsigned deformation = 1; deformation < 4; ++deformation) {
        auto source = scene_asset(AlphaMode::Opaque, 1, false, false, false, deformation);
        if (deformation == 2)
            rstd_assert(at(at(source->meshes, 1).primitives, 0).prepare_joint_bounds());
        auto          prepared = renderer.prepare(source.clone()).unwrap();
        SceneInstance scene(source.clone(), SceneBoundsMode::Conservative);
        rstd_assert(scene.set_animation(0));
        auto off      = draw(renderer, queue, session, target, prepared, scene, settings(false));
        auto initial  = draw(renderer, queue, session, target, prepared, scene, settings());
        auto compiles = renderer.graph_statistics().graph_compiles;
        occluded(initial, off);
        scene.set_time(1);
        near(scene.relative_primitive_bounds(1, 0, {}).center().x, 0.6f);
        near(at(at(source->meshes, 1).primitives, 0).bounds.center().x, 0);
        auto moved = draw(renderer, queue, session, target, prepared, scene, settings());
        near(image::channel(moved, 0, 22, 32), image::channel(off, 0, 22, 32));
        occluded(moved, off, 42);
        rstd_assert(renderer.graph_statistics().graph_compiles == compiles);
    }
    auto          source   = scene_asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          off =
        draw(renderer, queue, session, target, prepared, scene, settings(false), view({}, true));
    auto attached     = draw(renderer,
                             queue,
                             session,
                             target,
                             prepared,
                             scene,
                             settings(true, LightSpace::View),
                             view({}, true));
    auto attached_off = draw(renderer,
                             queue,
                             session,
                             target,
                             prepared,
                             scene,
                             settings(false, LightSpace::View),
                             view({}, true));
    auto world =
        draw(renderer, queue, session, target, prepared, scene, settings(), view({}, true));
    occluded(attached, attached_off);
    occluded(world, off, 42);
    near(image::channel(attached, 0, 42, 32), image::channel(attached_off, 0, 42, 32));
    near(image::channel(world, 0, 22, 32), image::channel(off, 0, 22, 32));
    rstd_assert(session.validation_errors() == 0);
}

TEST(Shadow, TranslatedCoordinates) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          target   = image::texture(queue);
    Vec3          origins[] { {}, { 1e8f, -1e8f, 0 } };
    Option<float> reference_shadow, reference_lit;
    for (auto origin : origins) {
        auto          source   = scene_asset(AlphaMode::Opaque, 1, false, false, false, 0, origin);
        auto          prepared = renderer.prepare(source.clone()).unwrap();
        SceneInstance scene(source.clone());
        auto          off =
            draw(renderer, queue, session, target, prepared, scene, settings(false), view(origin));
        auto on = draw(renderer, queue, session, target, prepared, scene, settings(), view(origin));
        occluded(on, off);
        if (reference_shadow) {
            near(image::channel(on, 0, 22, 32), *reference_shadow);
            near(image::channel(on, 0, 50, 32), *reference_lit);
        } else {
            reference_shadow = Some(image::channel(on, 0, 22, 32));
            reference_lit    = Some(image::channel(on, 0, 50, 32));
        }
    }
    rstd_assert(session.validation_errors() == 0);
}

TEST(Shadow, AtlasLights) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = scene_asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue);
    auto          lights = [](unsigned selected, bool shadows = true) -> SceneOptions {
        Vec3                  directions[] { { 0.6f, 0, 1 }, { -0.6f, 0, 1 }, { 0, 0.6f, 1 } };
        Vec3                  colors[] { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        unsigned              resolutions[] { 128, 256, 512 };
        Vec<DirectionalLight> values;
        for (unsigned i = 0; i < 3; ++i) {
            if (! (selected & (1u << i))) continue;
            DirectionalLight light;
            light.direction = normalized(directions[i]);
            light.color     = colors[i];
            light.intensity = 3;
            if (shadows) {
                DirectionalShadow shadow;
                shadow.resolution = resolutions[i];
                shadow.filter     = i == 2 ? ShadowFilter::Bilinear : ShadowFilter::Nearest;
                light.shadow      = Some(rstd::move(shadow));
            }
            values.push(rstd::move(light));
        }
        SceneOptions result;
        result.direct_light       = None();
        result.directional_lights = DirectionalLights(rstd::move(values));
        result.ibl_intensity      = 0;
        return result;
    };
    const auto combined = draw(renderer, queue, session, target, prepared, scene, lights(7));
    const auto red      = draw(renderer, queue, session, target, prepared, scene, lights(1));
    const auto green    = draw(renderer, queue, session, target, prepared, scene, lights(2));
    const auto blue     = draw(renderer, queue, session, target, prepared, scene, lights(4));
    const auto unshadowed =
        draw(renderer, queue, session, target, prepared, scene, lights(7, false));
    struct Sample {
        unsigned x, y;
    };
    Sample points[] { { 22, 32 }, { 42, 32 }, { 32, 42 }, { 50, 50 }, { 12, 12 }, { 32, 32 } };
    for (const auto& point : points)
        for (unsigned channel = 0; channel < 3; ++channel) {
            float expected = image::channel(red, channel, point.x, point.y) +
                             image::channel(green, channel, point.x, point.y) +
                             image::channel(blue, channel, point.x, point.y);
            near(image::channel(combined, channel, point.x, point.y), expected);
        }
    for (unsigned light = 0; light < 3; ++light) {
        const auto& point    = points[light];
        float       lit      = image::channel(unshadowed, light, point.x, point.y);
        float       shadowed = image::channel(combined, light, point.x, point.y);
        rstd_assert(lit > 0.1f && shadowed < lit * 0.25f);
        for (unsigned other = 0; other < 3; ++other) {
            if (other == light) continue;
            float control = image::channel(combined, other, point.x, point.y);
            rstd_assert(control > 0.1f);
            near(control, image::channel(unshadowed, other, point.x, point.y));
        }
    }
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
            for (unsigned channel = 0; channel < 3; ++channel) {
                float value = image::channel(combined, channel, x, y);
                rstd_assert(rstd::f32(value).is_finite() && value >= 0);
            }
    rstd_assert(session.validation_errors() == 0);
}

TEST(Shadow, PreparationAndLifetime) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto renderer =
        Renderer::create(session, { 64, 64 }, vk::PipelinePreparationMode::Deferred).unwrap();
    auto          source   = scene_asset(AlphaMode::Opaque, 1, false, false, false, 3);
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    rstd_assert(scene.set_animation(0));
    auto target = image::texture(queue), second_target = image::texture(queue);
    auto work      = queue.Begin().unwrap();
    auto uploads   = renderer.upload_statistics();
    auto unchanged = [&]() {
        rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
        rstd_assert(target.texture().content_revision() == 0);
        rstd_assert(renderer.statistics().recorded_frames == 0);
        auto current = renderer.upload_statistics();
        for (unsigned i = 0; i < 5; ++i) {
            rstd_assert(current.queued_copies[i] == uploads.queued_copies[i]);
            rstd_assert(current.queued_bytes[i] == uploads.queued_bytes[i]);
            rstd_assert(current.recorded_copies[i] == uploads.recorded_copies[i]);
        }
    };
    auto rejected = [&](SceneOptions options) {
        auto result =
            renderer.record_scene(work, target, prepared, scene, view(), rstd::move(options));
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
        unchanged();
    };
    auto invalid                             = settings();
    invalid.direct_light->shadow->resolution = 0;
    rejected(rstd::move(invalid));
    invalid                              = settings();
    invalid.direct_light->shadow->filter = ShadowFilter(99);
    rejected(rstd::move(invalid));
    invalid                            = settings();
    invalid.direct_light->shadow->bias = -0.01f;
    rejected(rstd::move(invalid));
    invalid                            = settings();
    invalid.direct_light->shadow->bias = 0.02f;
    rejected(rstd::move(invalid));
    invalid                                  = settings();
    invalid.direct_light->shadow->slope_bias = rstd::f32::NAN_.to_primitive();
    rejected(rstd::move(invalid));
    invalid                                   = settings();
    invalid.direct_light->shadow->normal_bias = -1;
    rejected(rstd::move(invalid));
    SpotLight bad_spot;
    bad_spot.point.position = { 0, 0, 2 };
    bad_spot.outer_cos      = 0;
    bad_spot.shadow         = Some(ShadowOptions {});
    Vec<SpotLight> bad_lights;
    bad_lights.push(rstd::move(bad_spot));
    SceneOptions bad_options;
    bad_options.spot_lights = SpotLights(rstd::move(bad_lights));
    rejected(rstd::move(bad_options));
    auto pending = renderer.record_scene(work, target, prepared, scene, view(), settings());
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    unchanged();
    auto start = rstd::time::Instant::now();
    while (renderer.prepare_scene_target(RenderTarget { target }, settings()).unwrap() ==
           vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    renderer.record_scene(work, target, prepared, scene, view(), settings()).unwrap();
    auto first    = queue.Submit(rstd::move(work)).unwrap();
    auto compiles = renderer.graph_statistics().graph_compiles;
    scene.set_time(1);
    auto next_options                       = settings();
    next_options.direct_light->shadow->bias = 0.0001f;
    auto second_work                        = queue.Begin().unwrap();
    renderer
        .record_scene(second_work, second_target, prepared, scene, view(), rstd::move(next_options))
        .unwrap();
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles);
    auto second          = queue.Submit(rstd::move(second_work)).unwrap();
    auto smaller         = image::texture(queue, 32, 24);
    auto smaller_options = settings(true, LightSpace::World, 256);
    while (
        renderer
            .prepare_scene_target(RenderTarget { smaller }, settings(true, LightSpace::World, 256))
            .unwrap() == vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    auto third_work = queue.Begin().unwrap();
    renderer
        .record_scene(third_work,
                      smaller,
                      prepared,
                      scene,
                      view({}, false, 32.0f / 24),
                      rstd::move(smaller_options))
        .unwrap();
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles + 1);
    renderer.close();
    auto third = queue.Submit(rstd::move(third_work)).unwrap();
    queue.wait(first).unwrap();
    queue.wait(second).unwrap();
    queue.wait(third).unwrap();
    auto a = image::pixels(session, queue, target);
    auto b = image::pixels(session, queue, second_target);
    auto c = image::pixels(session, queue, smaller);
    rstd_assert(image::channel(a, 0, 22, 32) < image::channel(b, 0, 22, 32) * 0.25f);
    rstd_assert(image::channel(b, 0, 42, 32) < image::channel(a, 0, 42, 32) * 0.25f);
    rstd_assert(image::channel(c, 0, 21, 12, 32) < image::channel(c, 0, 5, 12, 32) * 0.25f);
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::shadows
