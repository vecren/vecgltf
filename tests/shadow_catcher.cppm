module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:shadow_catcher;
import vecgltf;
import vecgltf.render;
import rstd.test;
import :coverage;
import :shadows;

using namespace vecgltf;

namespace vecgltf_test::shadow_catcher
{
namespace vk     = vrento::vulkan;
namespace tex    = vrento::texture;
namespace image  = vecgltf_test::coverage;
namespace shadow = vecgltf_test::shadows;

auto asset() -> Arc<Asset> {
    auto     source = image::asset();
    Material material;
    material.unlit      = true;
    material.base_color = { 1, 0.5f, 0.25f, 1 };
    shadow::quad(*source, 0.12f, 0.5f, rstd::move(material), {});
    return source;
}
auto options(float opacity = 1, float fade = 0, bool enabled = true) -> SceneOptions {
    auto          result = shadow::settings(enabled);
    ShadowCatcher catcher;
    catcher.opacity       = opacity;
    catcher.fade          = fade;
    result.shadow_catcher = Some(catcher);
    return result;
}
auto draw(Renderer&                   renderer,
          vk::TextureContext&         queue,
          vk::DeviceSession           session,
          tex::TextureViewRef         color,
          tex::TextureViewRef         coverage,
          const PreparedAsset&        prepared,
          const SceneInstance&        scene,
          SceneOptions                settings,
          Option<tex::TextureViewRef> depth    = None(),
          Option<DepthEncoding>       encoding = None()) -> Vec<u8> {
    RenderTarget target { color };
    target.coverage       = Some(tex::TextureViewRef(coverage));
    target.depth          = rstd::move(depth);
    target.depth_encoding = rstd::move(encoding);
    auto work             = queue.Begin().unwrap();
    renderer
        .record_scene(
            work, rstd::move(target), prepared, scene, shadow::view(), rstd::move(settings))
        .unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    return image::pixels(session, queue, color);
}

TEST(ShadowCatcher, OpacityFadeAndCoverage) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue), coverage = image::texture(queue);
    const auto    on = draw(renderer, queue, session, target, coverage, prepared, scene, options());
    const auto    mask = image::pixels(session, queue, coverage);
    image::near(image::channel(on, 0, 22, 32), 0);
    image::near(image::channel(on, 3, 22, 32), 1);
    image::near(image::channel(mask, 3, 22, 32), 0);
    image::near(image::channel(on, 3, 50, 32), 0);
    image::near(image::channel(on, 3), 1);
    image::near(image::channel(mask, 3), 1);
    auto       compiles = renderer.graph_statistics().graph_compiles;
    const auto quarter =
        draw(renderer, queue, session, target, coverage, prepared, scene, options(0.25f));
    image::near(image::channel(quarter, 3, 22, 32), 0.25f);
    const auto faded =
        draw(renderer, queue, session, target, coverage, prepared, scene, options(0.25f, 2));
    float x = (22.5f / 32) - 1, y = 1 - (32.5f / 32);
    float radial = 1 - (x * x + y * y) * 2;
    image::near(image::channel(faded, 3, 22, 32), 0.25f * radial * radial);
    const auto zero = draw(renderer, queue, session, target, coverage, prepared, scene, options(0));
    image::near(image::channel(zero, 3, 22, 32), 0);
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles);
    const auto off =
        draw(renderer, queue, session, target, coverage, prepared, scene, options(1, 0, false));
    image::near(image::channel(off, 3, 22, 32), 0);
    const auto absent =
        draw(renderer, queue, session, target, coverage, prepared, scene, shadow::settings());
    image::near(image::channel(absent, 3, 22, 32), 0);
    auto gray             = options(0.25f);
    gray.background_color = Some(Vec3 { 0.4f, 0.2f, 0.1f });
    const auto background =
        draw(renderer, queue, session, target, coverage, prepared, scene, rstd::move(gray));
    image::near(image::channel(background, 0, 22, 32), 0.3f);
    image::near(image::channel(background, 1, 22, 32), 0.15f);
    image::near(image::channel(background, 3, 22, 32), 1);
    rstd_assert(session.validation_errors() == 0);
}

TEST(ShadowCatcher, SpotLight) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue), coverage = image::texture(queue);
    auto          settings = []() {
        SceneOptions result;
        result.direct_light   = None();
        result.ibl_intensity  = 0;
        result.shadow_catcher = Some(ShadowCatcher {});
        SpotLight light;
        light.point.position    = { 0.9f, 0, 1.5f };
        light.point.attenuation = { 1, 0, 0 };
        light.direction         = normalized(Vec3 { -0.9f, 0, -1.5f });
        light.outer_cos         = 0.6f;
        light.inner_cos         = 0.9f;
        ShadowOptions options;
        options.resolution = 256;
        light.shadow       = Some(options);
        Vec<SpotLight> lights;
        lights.push(rstd::move(light));
        result.spot_lights = SpotLights(rstd::move(lights));
        return result;
    };
    const auto pixels =
        draw(renderer, queue, session, target, coverage, prepared, scene, settings());
    unsigned shadowed {};
    for (unsigned y = 20; y < 44; ++y)
        for (unsigned x = 5; x < 26; ++x)
            if (image::channel(pixels, 3, x, y) > 0.2f) ++shadowed;
    rstd_assert(shadowed > 10);
    rstd_assert(session.validation_errors() == 0);
}

TEST(ShadowCatcher, TransformAndDepth) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target = image::texture(queue), coverage = image::texture(queue);
    auto          narrow             = options();
    narrow.shadow_catcher->transform = compose({ -0.3f, 0, 0 }, { 0, 0, 0, 1 }, { 0.03f, 0.2f, 1 });
    const auto limited =
        draw(renderer, queue, session, target, coverage, prepared, scene, rstd::move(narrow));
    image::near(image::channel(limited, 3, 22, 32), 1);
    image::near(image::channel(limited, 3, 20, 32), 0);
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
        const auto value = draw(renderer,
                                queue,
                                session,
                                target,
                                coverage,
                                prepared,
                                scene,
                                options(),
                                Some(tex::TextureViewRef(depth)),
                                Some(encoding));
        image::near(image::channel(value, 3, 22, 32), 1);
    }
    auto     occluded_source = asset();
    Material foreground;
    foreground.unlit      = true;
    foreground.base_color = { 0.25f, 0.5f, 0.75f, 1 };
    shadow::quad(*occluded_source, 1, 0.2f, rstd::move(foreground), {});
    auto          occluded_prepared = renderer.prepare(occluded_source.clone()).unwrap();
    SceneInstance occluded_scene(occluded_source.clone());
    const auto    hidden = draw(
        renderer, queue, session, target, coverage, occluded_prepared, occluded_scene, options());
    image::near(image::channel(hidden, 0, 22, 32), 0.25f);
    image::near(image::channel(hidden, 1, 22, 32), 0.5f);
    image::near(image::channel(hidden, 2, 22, 32), 0.75f);
    rstd_assert(session.validation_errors() == 0);
}

TEST(ShadowCatcher, Validation) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = asset();
    auto          prepared = renderer.prepare(source.clone()).unwrap();
    SceneInstance scene(source.clone());
    auto          target   = image::texture(queue);
    auto          work     = queue.Begin().unwrap();
    auto          before   = renderer.upload_statistics();
    auto          rejected = [&](SceneOptions settings) {
        auto result = renderer.record_scene(
            work, target, prepared, scene, shadow::view(), rstd::move(settings));
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
        rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
        rstd_assert(renderer.statistics().recorded_frames == 0);
        rstd_assert(target.texture().content_revision() == 0);
        auto current = renderer.upload_statistics();
        for (unsigned i = 0; i < 5; ++i) {
            rstd_assert(current.queued_bytes[i] == before.queued_bytes[i]);
            rstd_assert(current.recorded_copies[i] == before.recorded_copies[i]);
        }
    };
    rejected(options(-0.1f));
    rejected(options(1.1f));
    rejected(options(rstd::f32::NAN_.to_primitive()));
    rejected(options(1, -1));
    rejected(options(1, rstd::f32::INFINITY_.to_primitive()));
    auto invalid                           = options();
    invalid.shadow_catcher->transform.m[0] = rstd::f32::NAN_.to_primitive();
    rejected(rstd::move(invalid));
    invalid                                = options();
    invalid.shadow_catcher->transform.m[0] = 0;
    rejected(rstd::move(invalid));
    invalid                                = options();
    invalid.shadow_catcher->transform.m[3] = 0.5f;
    rejected(rstd::move(invalid));
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::shadow_catcher
