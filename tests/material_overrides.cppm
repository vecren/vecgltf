module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:material_overrides;
import vecgltf;
import vecgltf.render;
import rstd.test;
import :coverage;
import :shadows;

using namespace vecgltf;

namespace vecgltf_test::material_overrides
{
namespace vk     = vrento::vulkan;
namespace tex    = vrento::texture;
namespace image  = vecgltf_test::coverage;
namespace shadow = vecgltf_test::shadows;

auto options(unsigned material = 0, MaterialAlphaMode mode = MaterialAlphaMode::Dither)
    -> PrepareOptions {
    Vec<MaterialOverride> values;
    values.push({ material, mode });
    PrepareOptions result;
    result.material_overrides = MaterialOverrides(rstd::move(values));
    return result;
}
auto quad(float alpha) -> Arc<Asset> {
    auto     source = image::asset();
    Material material;
    material.alpha      = AlphaMode::Blend;
    material.base_color = { 1, 1, 1, alpha };
    material.unlit      = true;
    shadow::quad(*source, 1, 0, rstd::move(material), {});
    return source;
}
auto draw(Renderer&            renderer,
          vk::TextureContext&  queue,
          vk::DeviceSession    session,
          tex::TextureViewRef  color,
          tex::TextureViewRef  coverage,
          const PreparedAsset& prepared,
          const SceneInstance& scene,
          unsigned             seed = 0) -> Vec<u8> {
    auto values              = image::options();
    values.alpha_dither_seed = seed;
    RenderTarget target { color };
    target.coverage = Some(tex::TextureViewRef(coverage));
    auto work       = queue.Begin().unwrap();
    renderer.record_scene(work, rstd::move(target), prepared, scene, shadow::view(), values)
        .unwrap();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    return image::pixels(session, queue, color);
}

TEST(MaterialOverride, DitherCoverageAndSeed) {
    auto          session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto          queue    = vk::TextureContext::create(session).unwrap();
    auto          renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto          source   = quad(0.5f);
    SceneInstance scene(source.clone());
    auto          configuration      = options();
    auto          copied             = configuration;
    configuration.material_overrides = MaterialOverrides {};
    auto dither                      = renderer.prepare(source.clone(), copied).unwrap();
    copied.material_overrides        = MaterialOverrides {};
    auto       standard              = renderer.prepare(source.clone()).unwrap();
    auto       target = image::texture(queue), coverage = image::texture(queue);
    const auto first    = draw(renderer, queue, session, target, coverage, dither, scene);
    const auto mask     = image::pixels(session, queue, coverage);
    const auto repeated = draw(renderer, queue, session, target, coverage, dither, scene);
    auto       compiles = renderer.graph_statistics().graph_compiles;
    const auto changed  = draw(renderer, queue, session, target, coverage, dither, scene, 17);
    // Adjacent NaN payloads catch numeric conversion and canonicalization of seed bits.
    const auto high_first =
        draw(renderer, queue, session, target, coverage, dither, scene, 0x7fc00000u);
    const auto high_second =
        draw(renderer, queue, session, target, coverage, dither, scene, 0x7fc00001u);
    const auto high_first_repeat =
        draw(renderer, queue, session, target, coverage, dither, scene, 0x7fc00000u);
    const auto high_second_repeat =
        draw(renderer, queue, session, target, coverage, dither, scene, 0x7fc00001u);
    unsigned covered {}, changed_pixels {}, high_first_covered {}, high_second_covered {},
        high_changed_pixels {};
    for (unsigned y = 1; y < 63; ++y)
        for (unsigned x = 1; x < 63; ++x) {
            float alpha = image::channel(first, 3, x, y);
            rstd_assert(alpha == 0 || alpha == 1);
            image::near(image::channel(first, 0, x, y), alpha);
            image::near(image::channel(mask, 3, x, y), alpha);
            image::near(image::channel(repeated, 3, x, y), alpha);
            if (alpha == 1) ++covered;
            if (image::channel(changed, 3, x, y) != alpha) ++changed_pixels;
            float high_a = image::channel(high_first, 3, x, y);
            float high_b = image::channel(high_second, 3, x, y);
            rstd_assert((high_a == 0 || high_a == 1) && (high_b == 0 || high_b == 1));
            image::near(image::channel(high_first_repeat, 3, x, y), high_a);
            image::near(image::channel(high_second_repeat, 3, x, y), high_b);
            if (high_a == 1) ++high_first_covered;
            if (high_b == 1) ++high_second_covered;
            if (high_a != high_b) ++high_changed_pixels;
        }
    rstd_assert(covered > 1600 && covered < 2250);
    rstd_assert(changed_pixels > 1000);
    rstd_assert(high_first_covered > 1600 && high_first_covered < 2250);
    rstd_assert(high_second_covered > 1600 && high_second_covered < 2250);
    rstd_assert(high_changed_pixels > 1000);
    rstd_assert(renderer.graph_statistics().graph_compiles == compiles);
    const auto blended = draw(renderer, queue, session, target, coverage, standard, scene);
    image::near(image::channel(blended, 0), 0.5f);
    image::near(image::channel(blended, 3), 0.5f);
    rstd_assert(at(source->materials, 0).alpha == AlphaMode::Blend);
    image::near(at(source->materials, 0).base_color.w, 0.5f);
    rstd_assert(session.validation_errors() == 0);
}

TEST(MaterialOverride, DitherEndpointsAndDepth) {
    auto  session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto  queue    = vk::TextureContext::create(session).unwrap();
    auto  renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto  target = image::texture(queue), coverage = image::texture(queue);
    float endpoints[] { 0, 1 };
    for (float alpha : endpoints) {
        auto          source   = quad(alpha);
        auto          prepared = renderer.prepare(source.clone(), options()).unwrap();
        SceneInstance scene(source.clone());
        const auto    result = draw(renderer, queue, session, target, coverage, prepared, scene);
        image::near(image::channel(result, 0), alpha);
        image::near(image::channel(result, 3), alpha);
        image::near(image::channel(result, 3, 5, 5), alpha);
    }
    auto     source = quad(0.5f);
    Material back;
    back.unlit      = true;
    back.base_color = { 0, 0, 1, 1 };
    shadow::quad(*source, 1, -0.5f, rstd::move(back), {});
    auto          prepared = renderer.prepare(source.clone(), options()).unwrap();
    SceneInstance scene(source.clone());
    const auto    result = draw(renderer, queue, session, target, coverage, prepared, scene);
    unsigned      white {}, blue {};
    for (unsigned y = 1; y < 63; ++y)
        for (unsigned x = 1; x < 63; ++x) {
            float red = image::channel(result, 0, x, y);
            image::near(image::channel(result, 2, x, y), 1);
            image::near(image::channel(result, 3, x, y), 1);
            rstd_assert(red == 0 || red == 1);
            if (red == 1)
                ++white;
            else
                ++blue;
        }
    rstd_assert(white > 1600 && blue > 1600);
    rstd_assert(session.validation_errors() == 0);
}

TEST(MaterialOverride, DitherShadow) {
    auto session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue    = vk::TextureContext::create(session).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto source   = shadow::scene_asset(AlphaMode::Blend, 0.5f);
    // A broad caster gives a statistically useful interior independent of PCF edges.
    auto& caster  = at(at(source->meshes, 1).primitives, 0);
    caster.bounds = {};
    for (auto& vertex : caster.vertices) {
        vertex.position.x *= 2.5f;
        vertex.position.y *= 2.5f;
        caster.bounds.include(vertex.position);
    }
    SceneInstance scene(source.clone());
    auto          dither   = renderer.prepare(source.clone(), options(1)).unwrap();
    auto          standard = renderer.prepare(source.clone()).unwrap();
    auto          target   = image::texture(queue);
    const auto    on       = shadow::draw(renderer,
                                          queue,
                                          session,
                                          target,
                                          dither,
                                          scene,
                                          shadow::settings(true, LightSpace::World, 128));
    const auto    off =
        shadow::draw(renderer, queue, session, target, dither, scene, shadow::settings(false));
    const auto blended =
        shadow::draw(renderer, queue, session, target, standard, scene, shadow::settings());
    float shadowed {}, unshadowed {}, standard_sum {};
    for (unsigned y = 25; y < 39; ++y)
        for (unsigned x = 15; x < 22; ++x) {
            shadowed += image::channel(on, 0, x, y);
            unshadowed += image::channel(off, 0, x, y);
            standard_sum += image::channel(blended, 0, x, y);
        }
    rstd_assert(unshadowed > 1);
    rstd_assert(shadowed > unshadowed * 0.2f && shadowed < unshadowed * 0.8f);
    rstd_assert(absolute(standard_sum - unshadowed) < unshadowed * 0.01f);
    rstd_assert(session.validation_errors() == 0);
}

TEST(MaterialOverride, Validation) {
    auto session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto renderer = Renderer::create(session, { 64, 64 }).unwrap();
    auto source   = quad(0.5f);
    auto before   = renderer.upload_statistics();
    auto rejected = [&](PrepareOptions value, Arc<Asset> input) {
        auto result = renderer.prepare(rstd::move(input), rstd::move(value));
        rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
        auto current = renderer.upload_statistics();
        for (unsigned i = 0; i < 5; ++i) {
            rstd_assert(current.queued_bytes[i] == before.queued_bytes[i]);
            rstd_assert(current.queued_copies[i] == before.queued_copies[i]);
        }
        rstd_assert(renderer.statistics().prepared_assets == 0);
    };
    rejected(options(1), source.clone());
    rejected(options(0, MaterialAlphaMode(99)), source.clone());
    Vec<MaterialOverride> duplicate;
    duplicate.push({ 0, MaterialAlphaMode::Asset });
    duplicate.push({ 0, MaterialAlphaMode::Dither });
    PrepareOptions repeats;
    repeats.material_overrides = MaterialOverrides(rstd::move(duplicate));
    rejected(rstd::move(repeats), source.clone());
    auto opaque = image::asset();
    image::triangle(*opaque, AlphaMode::Opaque, 1);
    rejected(options(), opaque.clone());
    auto mask = image::asset();
    image::triangle(*mask, AlphaMode::Mask, 0.5f);
    rejected(options(), mask.clone());
    renderer.prepare(opaque.clone(), options(0, MaterialAlphaMode::Asset)).unwrap();
    PrepareOptions empty;
    empty.material_overrides = MaterialOverrides(Vec<MaterialOverride> {});
    renderer.prepare(source.clone(), rstd::move(empty)).unwrap();
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::material_overrides
