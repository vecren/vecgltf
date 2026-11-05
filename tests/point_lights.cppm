module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:point_lights;
import vecgltf;
import vecgltf.render;
import rstd.test;
import :coverage;

using namespace vecgltf;

namespace vecgltf_test::point_lights
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
auto rgb(const Vec<u8>& pixels) -> Vec3 {
    return { image::channel(pixels, 0), image::channel(pixels, 1), image::channel(pixels, 2) };
}
auto triangle(Vec3 origin = {}, bool reverse = false) -> Arc<Asset> {
    auto source = image::asset();
    image::triangle(*source, AlphaMode::Opaque, 1, 0, reverse);
    auto& material                   = at(source->materials, 0);
    material.unlit                   = false;
    material.base_color              = { 1, 1, 1, 1 };
    material.metallic                = 0;
    material.roughness               = 1;
    at(source->nodes, 0).translation = origin;
    return source;
}
auto view(Vec3 origin = {}, bool rolled = false, float aspect = 1) -> View {
    CameraProjection projection;
    projection.orthographic = true;
    projection.xmag = projection.ymag = 1;
    projection.near                   = 0.1f;
    projection.far                    = 10;
    return camera_frame(CameraLookAt { origin + Vec3 { 0, 0, 2 }, origin },
                        projection,
                        rolled ? Vec3 { 0, -1, 0 } : Vec3 { 0, 1, 0 })
        .unwrap()
        .view(aspect)
        .unwrap();
}
auto point(Vec3 position = { 0.015625f, -0.015625f, 1 }, Vec3 attenuation = { 1, 0, 0 })
    -> PointLight {
    PointLight result;
    result.position    = position;
    result.attenuation = attenuation;
    return result;
}
auto empty_options() -> SceneOptions {
    SceneOptions result;
    result.direct_light  = None();
    result.ibl_intensity = 0;
    return result;
}
auto options(PointLight light) -> SceneOptions {
    auto            result = empty_options();
    Vec<PointLight> values;
    values.push(rstd::move(light));
    result.point_lights = PointLights(rstd::move(values));
    return result;
}
auto draw(Renderer&            renderer,
          vk::TextureContext&  queue,
          vk::DeviceSession    session,
          tex::TextureViewRef  target,
          const PreparedAsset& prepared,
          const SceneInstance& scene,
          SceneOptions         options,
          View                 camera = view()) -> Vec<u8> {
    auto work = queue.Begin().unwrap();
    renderer.record_scene(work, target, prepared, scene, camera, rstd::move(options)).unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    return image::pixels(session, queue, target);
}

TEST(PointLight, AttenuationAndDirectional) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = triangle();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue);
    const auto baseline = draw(renderer, queue, session, target, prepared, scene, options(point()));
    float      reference = image::channel(baseline, 0);
    rstd_assert(reference > 0.1f);
    float distances[] { 1, 2, 3 };
    for (float distance : distances) {
        auto values = draw(renderer,
                           queue,
                           session,
                           target,
                           prepared,
                           scene,
                           options(point({ 0.015625f, -0.015625f, distance }, { 1, 2, 3 })));
        near(image::channel(values, 0) / reference,
             1 / (1 + 2 * distance + 3 * distance * distance),
             0.001f);
    }
    auto directional         = empty_options();
    directional.direct_light = Some(DirectionalLight { { 0, 0, 1 }, { 1, 1, 1 }, 3 });
    auto values = draw(renderer, queue, session, target, prepared, scene, rstd::move(directional));
    near(rgb(values), rgb(baseline));
    rstd_assert(session.validation_errors() == 0);
}

TEST(PointLight, SpotCone) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = triangle();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target    = image::texture(queue);
    auto          draw_spot = [&](Vec3 direction) {
        auto           settings = empty_options();
        Vec<SpotLight> lights;
        SpotLight      spot;
        spot.point     = point();
        spot.direction = direction;
        spot.inner_cos = 0.9f;
        spot.outer_cos = 0.8f;
        lights.push(rstd::move(spot));
        settings.spot_lights = SpotLights(rstd::move(lights));
        return draw(renderer, queue, session, target, prepared, scene, rstd::move(settings));
    };
    const auto inside  = draw_spot({ 0, 0, -1 });
    const auto edge    = draw_spot({ 0.5267827f, 0, -0.85f });
    const auto outside = draw_spot({ 1, 0, 0 });
    rstd_assert(image::channel(inside, 0) > 0.1f);
    near(image::channel(edge, 0) / image::channel(inside, 0), 0.425f, 0.015f);
    rstd_assert(image::channel(outside, 0) < 0.001f);
    rstd_assert(session.validation_errors() == 0);
}

TEST(PointLight, AdditionAndEmpty) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = triangle();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue);
    auto          a = point(), b = point({ 0.015625f, -0.015625f, 2 });
    a.color                = { 0.4f, 0.1f, 0.2f };
    b.color                = { 0.1f, 0.3f, 0.2f };
    const auto      first  = draw(renderer, queue, session, target, prepared, scene, options(a));
    const auto      second = draw(renderer, queue, session, target, prepared, scene, options(b));
    auto            combined_options = empty_options();
    Vec<PointLight> values;
    values.push(PointLight(a));
    values.push(PointLight(b));
    combined_options.point_lights = PointLights(rstd::move(values));
    auto copied                   = combined_options.point_lights;
    auto combined =
        draw(renderer, queue, session, target, prepared, scene, rstd::move(combined_options));
    near(rgb(combined), rgb(first) + rgb(second));
    auto compiles               = renderer.graph_statistics().graph_compiles;
    auto replay_options         = empty_options();
    replay_options.point_lights = copied;
    auto replay =
        draw(renderer, queue, session, target, prepared, scene, rstd::move(replay_options));
    near(rgb(replay), rgb(combined));
    auto absent         = draw(renderer, queue, session, target, prepared, scene, empty_options());
    auto explicit_empty = empty_options();
    explicit_empty.point_lights = PointLights(Vec<PointLight> {});
    auto empty =
        draw(renderer, queue, session, target, prepared, scene, rstd::move(explicit_empty));
    near(rgb(absent), {});
    near(rgb(empty), {});
    auto            maximum = empty_options();
    Vec<PointLight> many;
    for (unsigned i = 0; i < max_point_lights; ++i) {
        auto light      = point();
        light.intensity = 0.1f;
        many.push(rstd::move(light));
    }
    maximum.point_lights = PointLights(rstd::move(many));
    auto accepted = draw(renderer, queue, session, target, prepared, scene, rstd::move(maximum));
    rstd_assert(image::channel(accepted, 0) > 0.1f);
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles);
    rstd_assert(session.validation_errors() == 0);
}

TEST(PointLight, SpacesAndTranslated) {
    auto         session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto         queue    = vk::TextureContext::create(session).unwrap();
    auto         renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto         target   = image::texture(queue);
    Vec3         origins[] { {}, { 1e8f, -1e8f, 0 } };
    Option<Vec3> world_reference, attached_reference;
    for (auto origin : origins) {
        auto          source   = triangle(origin);
        auto          prepared = renderer.prepare(source.clone()).unwrap();
        SceneInstance scene(source.clone());
        auto          world_light = point(origin + Vec3 { 0, 0, 1 });
        auto          world       = draw(
            renderer, queue, session, target, prepared, scene, options(world_light), view(origin));
        auto attached_light  = point({ -0.5f, -0.3f, -1 });
        attached_light.space = LightSpace::View;
        auto attached        = draw(renderer,
                                    queue,
                                    session,
                                    target,
                                    prepared,
                                    scene,
                                    options(attached_light),
                                    view(origin, true));
        rstd_assert(image::channel(world, 0) > 0.1f && image::channel(attached, 0) > 0.1f);
        if (world_reference) {
            near(rgb(world), *world_reference);
            near(rgb(attached), *attached_reference);
        } else {
            world_reference       = Some(rgb(world));
            attached_reference    = Some(rgb(attached));
            auto equivalent_world = draw(renderer,
                                         queue,
                                         session,
                                         target,
                                         prepared,
                                         scene,
                                         options(point({ 0.5f, 0.3f, 1 })),
                                         view({}, true));
            near(rgb(equivalent_world), rgb(attached));
        }
    }
    rstd_assert(session.validation_errors() == 0);
}

TEST(PointLight, ZeroDistanceAndClamp) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = triangle();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue);
    auto          zero   = draw(renderer,
                                queue,
                                session,
                                target,
                                prepared,
                                scene,
                                options(point({ 0.015625f, -0.015625f, 0 }, { 0, 0, 1 })));
    near(rgb(zero), {});
    auto far   = draw(renderer,
                      queue,
                      session,
                      target,
                      prepared,
                      scene,
                      options(point({ 0.015625f, -0.015625f, 1 }, { 0, 0, 1 })));
    auto close = draw(renderer,
                      queue,
                      session,
                      target,
                      prepared,
                      scene,
                      options(point({ 0.015625f, -0.015625f, 0.00001f }, { 0, 0, 1 })));
    near(image::channel(close, 0) / image::channel(far, 0), 10000, 30);
    for (unsigned channel = 0; channel < 3; ++channel)
        rstd_assert(rstd::f32(image::channel(close, channel)).is_finite());
    rstd_assert(session.validation_errors() == 0);
}

TEST(PointLight, BackgroundCoverage) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = triangle({}, true);
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue), coverage = image::texture(queue);
    auto          configured    = empty_options();
    configured.background       = true;
    configured.background_color = Some(Vec3 { 2, 0.5f, 0.25f });
    RenderTarget destination { target };
    destination.coverage = Some(tex::TextureViewRef(coverage));
    auto work            = queue.Begin().unwrap();
    renderer
        .record_scene(
            work, rstd::move(destination), prepared, scene, view(), rstd::move(configured))
        .unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto color = image::pixels(session, queue, target);
    auto mask  = image::pixels(session, queue, coverage);
    near(rgb(color), { 2, 0.5f, 0.25f });
    near(image::channel(color, 3), 1);
    near(image::channel(color, 0, 0, 0), 2);
    near(image::channel(color, 3, 0, 0), 1);
    near(image::channel(mask, 3), 0);
    near(image::channel(mask, 3, 0, 0), 0);
    auto old = draw(renderer, queue, session, target, prepared, scene, empty_options());
    near(rgb(old), {});
    near(image::channel(old, 3), 0);
    auto blend_source = image::asset();
    image::triangle(*blend_source, AlphaMode::Blend, 0.5f);
    at(blend_source->materials, 0).base_color = { 1, 1, 1, 0.5f };
    auto          blend_prepared              = renderer.prepare(blend_source.clone()).unwrap();
    SceneInstance blend_scene(blend_source.clone());
    auto          blend_options    = empty_options();
    blend_options.background_color = Some(Vec3 { 0.2f, 0.4f, 0.6f });
    RenderTarget blend_target { target };
    blend_target.coverage = Some(tex::TextureViewRef(coverage));
    auto blend_work       = queue.Begin().unwrap();
    renderer
        .record_scene(blend_work,
                      rstd::move(blend_target),
                      blend_prepared,
                      blend_scene,
                      view(),
                      rstd::move(blend_options))
        .unwrap();
    queue.wait(queue.Submit(rstd::move(blend_work)).unwrap()).unwrap();
    auto blend = image::pixels(session, queue, target);
    near(rgb(blend), { 0.6f, 0.7f, 0.8f });
    near(image::channel(blend, 3), 1);
    near(image::channel(image::pixels(session, queue, coverage), 3), 0.5f);
    rstd_assert(session.validation_errors() == 0);
}

TEST(PointLight, PreparationAndLifetime) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto renderer =
        Renderer::create(session, { 64, 64 }, vk::PipelinePreparationMode::Deferred).unwrap();
    auto          source   = triangle();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue), second_target = image::texture(queue);
    auto          work      = queue.Begin().unwrap();
    auto          uploads   = renderer.upload_statistics();
    auto          unchanged = [&]() {
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
    auto rejected = [&](SceneOptions values, ErrorKind kind = ErrorKind::InvalidAsset) {
        auto result =
            renderer.record_scene(work, target, prepared, scene, view(), rstd::move(values));
        rstd_assert(result.is_err() && result.unwrap_err().kind == kind);
        unchanged();
    };
    auto invalid       = point();
    invalid.position.x = rstd::f32::NAN_.to_primitive();
    rejected(options(invalid));
    invalid         = point();
    invalid.color.y = -1;
    rejected(options(invalid));
    invalid           = point();
    invalid.intensity = -1;
    rejected(options(invalid));
    invalid           = point();
    invalid.color.x   = rstd::f32::MAX.to_primitive();
    invalid.intensity = 2;
    rejected(options(invalid));
    invalid             = point();
    invalid.attenuation = {};
    rejected(options(invalid));
    invalid               = point();
    invalid.attenuation.y = -1;
    rejected(options(invalid));
    invalid               = point();
    invalid.attenuation.z = rstd::f32::INFINITY_.to_primitive();
    rejected(options(invalid));
    invalid       = point();
    invalid.space = LightSpace(99);
    rejected(options(invalid));
    auto            too_many = empty_options();
    Vec<PointLight> excess;
    for (unsigned i = 0; i <= max_point_lights; ++i) excess.push(point());
    too_many.point_lights = PointLights(rstd::move(excess));
    rejected(rstd::move(too_many), ErrorKind::Limit);
    auto background             = empty_options();
    background.background_color = Some(Vec3 { -1, 0, 0 });
    rejected(rstd::move(background));
    background                  = empty_options();
    background.background_color = Some(Vec3 { 0, rstd::f32::NAN_.to_primitive(), 0 });
    rejected(rstd::move(background));
    auto pending = renderer.record_scene(work, target, prepared, scene, view(), options(point()));
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    unchanged();
    auto start = rstd::time::Instant::now();
    while (renderer.prepare_scene_target(RenderTarget { target }, options(point())).unwrap() ==
           vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    renderer.record_scene(work, target, prepared, scene, view(), options(point())).unwrap();
    auto first       = queue.Submit(rstd::move(work)).unwrap();
    auto compiles    = renderer.graph_statistics().graph_compiles;
    auto second_work = queue.Begin().unwrap();
    auto dimmer      = point();
    dimmer.intensity = 1.5f;
    renderer.record_scene(second_work, second_target, prepared, scene, view(), options(dimmer))
        .unwrap();
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles);
    auto second     = queue.Submit(rstd::move(second_work)).unwrap();
    auto smaller    = image::texture(queue, 32, 24);
    auto third_work = queue.Begin().unwrap();
    renderer
        .record_scene(third_work,
                      smaller,
                      prepared,
                      scene,
                      view({}, false, 32.0f / 24),
                      options(point({ 1.0f / 32, -1.0f / 24, 1 })))
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
    rstd_assert(image::channel(a, 0) > 0.1f);
    near(image::channel(a, 0), image::channel(b, 0) * 2);
    near(image::channel(c, 0, 16, 12, 32), image::channel(a, 0));
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::point_lights
