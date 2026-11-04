module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:lighting;
import vecgltf.render;
import rstd.test;
import :render;

using namespace vecgltf;

namespace vecgltf_test::lighting
{
namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;
using render::near;
using render::read_pixels;
using render::triangle;

auto target(vk::TextureContext& queue) -> tex::TextureViewRef {
    tex::TextureDesc description;
    description.extent = { 64, 64 };
    description.format = vrento::TextureFormat::RGBA8;
    description.usage =
        tex::Usage::ColorAttachment | tex::Usage::Sampled | tex::Usage::TransferSource;
    return queue.CreateView(queue.CreateTexture(description).unwrap(), {}).unwrap();
}
struct Fixture {
    vk::DeviceSession   session { vk::DeviceSession::create({ .validation = true }).unwrap() };
    vk::TextureContext  queue { vk::TextureContext::create(session).unwrap() };
    Renderer            renderer { Renderer::create(session, { 64, 64 }).unwrap() };
    tex::TextureViewRef output { target(queue) };
    View view { look_at({ 0, 0, 2 }, {}), perspective(0.8f, 1, 0.1f, 10), { 0, 0, 2 } };
    auto draw(Arc<Asset> asset, RenderOptions options) -> Vec<u8> {
        auto          prepared = renderer.prepare(asset.clone()).unwrap();
        SceneInstance instance(asset.clone());
        auto          work = queue.Begin().unwrap();
        renderer.record(work, output, prepared, instance, view, options).unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        return read_pixels(session, queue, output);
    }
};
auto sample(const Vec<u8>& pixels, unsigned channel, unsigned x = 32, unsigned y = 32) -> unsigned {
    return pixels[usize((y * 64 + x) * 4 + channel)].to_primitive();
}
auto surface(float metallic = 0) -> Arc<Asset> {
    auto  asset        = triangle({ 0.5f, 0.5f, 0.5f, 1 });
    auto& material     = at(asset->materials, 0);
    material.unlit     = false;
    material.metallic  = metallic;
    material.roughness = 1;
    return asset;
}
auto lights(Vec<DirectionalLight> values) -> DirectionalLights {
    return DirectionalLights(rstd::move(values));
}
auto solid(unsigned width, unsigned height, Vec3 color) -> HdrImage {
    HdrImage image { width, height };
    for (unsigned i = 0; i < width * height; ++i) {
        image.rgba.push(float(color.x));
        image.rgba.push(float(color.y));
        image.rgba.push(float(color.z));
        image.rgba.push(1.0f);
    }
    return image;
}
auto split(unsigned width, unsigned height) -> HdrImage {
    HdrImage image { width, height };
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            image.rgba.push(x < width / 2 ? 0.4f : 0.0f);
            image.rgba.push(0.0f);
            image.rgba.push(x < width / 2 ? 0.0f : 0.4f);
            image.rgba.push(1.0f);
        }
    return image;
}
auto environment(bool mip_colors = false) -> EnvironmentImages {
    EnvironmentImages image;
    image.background = split(16, 8);
    image.diffuse    = split(16, 8);
    image.brdf       = solid(8, 8, { 1, 0, 0 });
    image.specular.push(mip_colors ? solid(16, 8, { 0.4f, 0, 0 }) : split(16, 8));
    image.specular.push(mip_colors ? solid(8, 4, { 0, 0, 0.4f }) : split(8, 4));
    return image;
}
auto rotate_environment() -> Mat4 {
    auto matrix = Mat4::Identity();
    matrix.m[0] = matrix.m[10] = -1;
    return matrix;
}
TEST(Lighting, DirectionalLights) {
    Fixture       fixture;
    auto          asset = surface();
    RenderOptions options;
    options.tone_mapping  = ToneMapping::Linear;
    options.ibl_intensity = 0;
    auto defaults         = fixture.draw(asset.clone(), {});
    auto legacy           = fixture.draw(
        asset.clone(),
        { 1, DebugView::None, false, Some(DirectionalLight {}), 1, ToneMapping::ACES });
    for (unsigned c = 0; c < 3; ++c) near(sample(defaults, c), sample(legacy, c));
    Vec<DirectionalLight> red;
    red.push({ { 0, 0, 1 }, { 1, 0, 0 }, 0.5f });
    options.directional_lights  = lights(rstd::move(red));
    auto                  first = fixture.draw(asset.clone(), options);
    Vec<DirectionalLight> blue;
    blue.push({ { 0, 0, 1 }, { 0, 0, 1 }, 0.5f });
    options.directional_lights   = lights(rstd::move(blue));
    auto                  second = fixture.draw(asset.clone(), options);
    Vec<DirectionalLight> both;
    both.push({ { 0, 0, 1 }, { 1, 0, 0 }, 0.5f });
    both.push({ { 0, 0, 1 }, { 0, 0, 1 }, 0.5f });
    both.push({ { 0, 0, -1 }, { 0, 1, 0 }, 2 });
    options.directional_lights = lights(rstd::move(both));
    auto combined              = fixture.draw(asset.clone(), options);
    rstd_assert(sample(combined, 0) > 5 && sample(combined, 2) > 5);
    for (unsigned c = 0; c < 3; ++c)
        near(sample(combined, c), sample(first, c) + sample(second, c));
    auto copied                = options;
    options.directional_lights = lights(Vec<DirectionalLight> {});
    auto empty                 = fixture.draw(asset.clone(), options);
    auto retained              = fixture.draw(asset.clone(), copied);
    for (unsigned c = 0; c < 3; ++c) {
        near(sample(empty, c), 0);
        near(sample(retained, c), sample(combined, c));
    }
    rstd_assert(fixture.renderer.graph_statistics().graph_compiles == 1);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Lighting, ViewSpace) {
    Fixture fixture;
    auto    asset     = surface();
    auto&   primitive = at(at(asset->meshes, 0).primitives, 0);
    primitive.bounds  = {};
    for (auto& vertex : primitive.vertices) {
        auto original   = vertex.position;
        vertex.position = { original.z, original.y, -original.x };
        vertex.normal   = { 1, 0, 0 };
        vertex.tangent  = { 0, 0, -1, 1 };
        primitive.bounds.include(vertex.position);
    }
    fixture.view = { look_at({ 2, 0, 0 }, {}), perspective(0.8f, 1, 0.1f, 10), { 2, 0, 0 } };
    RenderOptions options;
    options.ibl_intensity = 0;
    options.tone_mapping  = ToneMapping::Linear;
    Vec<DirectionalLight> world;
    world.push({ { 1, 0, 0 }, { 1, 0.5f, 0.25f }, 1 });
    options.directional_lights     = lights(rstd::move(world));
    auto                  expected = fixture.draw(asset.clone(), options);
    Vec<DirectionalLight> view;
    view.push({ { 0, 0, 1 }, { 1, 0.5f, 0.25f }, 1, LightSpace::View });
    options.directional_lights = lights(rstd::move(view));
    auto relative              = fixture.draw(asset.clone(), options);
    rstd_assert(sample(relative, 0) > 5);
    for (unsigned c = 0; c < 3; ++c) near(sample(relative, c), sample(expected, c));
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Lighting, EnvironmentSampling) {
    Fixture fixture;
    fixture.renderer.set_environment(environment()).unwrap();
    RenderOptions options;
    options.direct_light         = None();
    options.tone_mapping         = ToneMapping::Linear;
    options.background           = true;
    auto matte                   = surface();
    auto identity                = fixture.draw(matte.clone(), options);
    options.world_to_environment = rotate_environment();
    auto rotated                 = fixture.draw(matte.clone(), options);
    rstd_assert(sample(identity, 2) > sample(identity, 0) + 20);
    rstd_assert(sample(rotated, 0) > sample(rotated, 2) + 20);
    rstd_assert(sample(identity, 0, 0, 0) > sample(identity, 2, 0, 0) + 20);
    rstd_assert(sample(rotated, 2, 0, 0) > sample(rotated, 0, 0, 0) + 20);
    auto glossy                  = surface(1);
    auto original_specular       = fixture.draw(glossy.clone(), options);
    options.world_to_environment = Mat4::Identity();
    auto turned_specular         = fixture.draw(glossy.clone(), options);
    rstd_assert(sample(original_specular, 0) > sample(original_specular, 2) + 20);
    rstd_assert(sample(turned_specular, 2) > sample(turned_specular, 0) + 20);
    options.ibl_intensity = 0;
    auto no_ibl           = fixture.draw(matte.clone(), options);
    near(sample(no_ibl, 0), 0);
    near(sample(no_ibl, 2), 0);
    near(sample(no_ibl, 0, 0, 0), sample(identity, 0, 0, 0));
    options.background_intensity = 0.5f;
    auto half_background         = fixture.draw(matte.clone(), options);
    near(sample(half_background, 0, 0, 0) * 2, sample(no_ibl, 0, 0, 0));
    near(sample(half_background, 0), 0);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Lighting, EnvironmentLod) {
    Fixture       fixture;
    RenderOptions options;
    options.direct_light = None();
    options.tone_mapping = ToneMapping::Linear;
    auto asset           = surface(1);
    fixture.renderer.set_environment(environment(true)).unwrap();
    auto last_mip             = fixture.draw(asset.clone(), options);
    auto source               = environment(true);
    source.specular_lod_scale = 0;
    fixture.renderer.set_environment(rstd::move(source)).unwrap();
    auto first_mip = fixture.draw(asset.clone(), options);
    rstd_assert(sample(last_mip, 2) > sample(last_mip, 0) + 20);
    rstd_assert(sample(first_mip, 0) > sample(first_mip, 2) + 20);
    at(asset->materials, 0).roughness = 0.5f;
    auto ordinary                     = environment(true);
    ordinary.specular_lod_scale       = 1;
    fixture.renderer.set_environment(rstd::move(ordinary)).unwrap();
    auto linear                 = fixture.draw(asset.clone(), options);
    auto power                  = environment(true);
    power.specular_lod_exponent = 2;
    power.brdf_filter           = vrento::TextureFilter::NEAREST;
    fixture.renderer.set_environment(rstd::move(power)).unwrap();
    auto squared = fixture.draw(asset.clone(), options);
    rstd_assert(sample(linear, 2) > sample(squared, 2) + 5);
    rstd_assert(sample(squared, 0) > sample(linear, 0) + 5);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Lighting, InvalidOptionsAreUnrecorded) {
    Fixture       fixture;
    auto          asset    = surface();
    auto          prepared = fixture.renderer.prepare(asset.clone()).unwrap();
    SceneInstance instance(asset.clone());
    auto          baseline = fixture.renderer.upload_statistics();
    auto          frames   = fixture.renderer.statistics().recorded_frames;
    auto          revision = fixture.output.texture().content_revision();
    auto rejected = [&](RenderOptions options, ErrorKind expected = ErrorKind::InvalidAsset) {
        auto work   = fixture.queue.Begin().unwrap();
        auto result = fixture.renderer.record(
            work, fixture.output, prepared, instance, fixture.view, options);
        rstd_assert(result.is_err() && result.unwrap_err().kind == expected);
        rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
        rstd_assert(fixture.renderer.statistics().recorded_frames == frames);
        rstd_assert(fixture.output.texture().content_revision() == revision);
        auto after = fixture.renderer.upload_statistics();
        for (unsigned i = 0; i < 5; ++i) {
            rstd_assert(after.queued_copies[i] == baseline.queued_copies[i]);
            rstd_assert(after.recorded_copies[i] == baseline.recorded_copies[i]);
        }
    };
    RenderOptions options;
    options.background_intensity = -1;
    rejected(options);
    options                            = {};
    options.world_to_environment.m[12] = 1;
    rejected(options);
    options                           = {};
    options.world_to_environment.m[0] = 2;
    rejected(options);
    options                           = {};
    options.world_to_environment.m[0] = __builtin_nanf("");
    rejected(options);
    options              = {};
    options.direct_light = Some(DirectionalLight { { 1, 0, 0 }, { 1, 1, 1 }, 1, LightSpace(99) });
    rejected(options);
    options = {};
    Vec<DirectionalLight> invalid;
    for (unsigned i = 0; i <= max_directional_lights; ++i) invalid.push(DirectionalLight {});
    options.directional_lights = lights(rstd::move(invalid));
    rejected(options, ErrorKind::Limit);
    auto preparations           = fixture.renderer.statistics().environment_preparations;
    auto image                  = environment();
    image.specular_lod_exponent = 0;
    rstd_assert(fixture.renderer.set_environment(rstd::move(image)).is_err());
    rstd_assert(fixture.renderer.statistics().environment_preparations == preparations);
    image                          = environment();
    image.specular[usize(1)].width = 1;
    rstd_assert(fixture.renderer.set_environment(rstd::move(image)).is_err());
    rstd_assert(fixture.renderer.statistics().environment_preparations == preparations);
    rstd_assert(fixture.session.validation_errors() == 0);
}
} // namespace vecgltf_test::lighting
