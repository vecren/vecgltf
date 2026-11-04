module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:color_balance;
import vecgltf.display;
import rstd.test;
import :display;
import :postprocess;

using namespace vecgltf;

namespace vecgltf_test::color_balance
{
namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;
using vecgltf_test::display::channel;
using vecgltf_test::display::clear;
using vecgltf_test::display::near;
using vecgltf_test::display::pixels;
using vecgltf_test::display::texture;
using vecgltf_test::postprocess::Fixture;
using vecgltf_test::postprocess::linear;
using vecgltf_test::postprocess::quantize;

Vec3 balanced(Vec3 color, const ColorBalance& balance) {
    float original = square_root(dot(color, color));
    if (original < 1e-5f) return color;
    float lightness  = (minimum(minimum(color.x, color.y), color.z) +
                        maximum(maximum(color.x, color.y), color.z)) *
                       0.5f;
    lightness        = 1.5f * lightness / (lightness + 0.5f);
    float shadows    = clamp((lightness - 0.333f) / -0.25f + 0.5f, 0, 1);
    float midtones   = clamp((lightness - 0.333f) / 0.25f + 0.5f, 0, 1) *
                       clamp((lightness + 0.333f - 1) / -0.25f + 0.5f, 0, 1);
    float highlights = clamp((lightness + 0.333f - 1) / 0.25f + 0.5f, 0, 1);
    Vec3  shifted    = color + (balance.shadows * shadows + balance.midtones * midtones +
                                balance.highlights * highlights) *
                                   0.7f;
    shifted          = { maximum(shifted.x, 0), maximum(shifted.y, 0), maximum(shifted.z, 0) };
    float length     = square_root(dot(shifted, shifted));
    return length < 1e-5f ? shifted * original : shifted * original / length;
}
void sample(const Vec<u8>& image, Vec3 color, float alpha = 1) {
    near(channel(image, 0), quantize(color.x * alpha));
    near(channel(image, 1), quantize(color.y * alpha));
    near(channel(image, 2), quantize(color.z * alpha));
    near(channel(image, 3), quantize(alpha));
}

TEST(ColorBalance, TonalRegionsAndLength) {
    Fixture      fixture(64, 64);
    ColorBalance balance { { 0.1f, 0, 0 }, { 0, 0.1f, 0 }, { 0, 0, 0.1f } };
    auto         options  = linear();
    options.color_balance = Some(balance);
    float levels[] { 0.03125f, 0.25f, 0.75f };
    for (unsigned region = 0; region < 3; ++region) {
        float level = levels[region];
        Vec3  color { level, level, level };
        auto  image = fixture.solid({ level, level, level, 1 }, options);
        sample(image, balanced(color, balance));
        unsigned favored = channel(image, region);
        rstd_assert(favored > channel(image, (region + 1) % 3));
        Vec3 actual { float(channel(image, 0)) / 255,
                      float(channel(image, 1)) / 255,
                      float(channel(image, 2)) / 255 };
        rstd_assert(absolute(square_root(dot(actual, actual)) - square_root(dot(color, color))) <
                    0.008f);
    }
    auto black = fixture.solid({ 0, 0, 0, 1 }, options);
    sample(black, {});
    options.exposure = 65536;
    auto tiny        = fixture.solid({ 0.000001f, 0.000002f, 0.000003f, 1 }, options);
    sample(tiny, { 0.065536f, 0.131072f, 0.196608f });
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(ColorBalance, ZeroAndPremultipliedAlpha) {
    Fixture fixture(64, 64);
    auto    options = linear();
    for (unsigned post = 0; post < 2; ++post) {
        if (post) {
            options.bloom   = Some(Bloom { 0.36f, 0.3f, 0.15f });
            options.fxaa    = true;
            options.sharpen = 0.1f;
        }
        options.color_balance = None();
        auto absent           = fixture.pattern(options);
        options.color_balance = Some(ColorBalance {});
        auto zero             = fixture.pattern(options);
        rstd_assert(absent == zero);
    }
    options = linear();
    ColorBalance balance { {}, { 0.1f, 0, -0.05f }, {} };
    options.color_balance = Some(balance);
    Vec3 expected         = balanced({ 0.125f, 0.25f, 0.375f }, balance);
    auto half             = fixture.solid({ 0.0625f, 0.125f, 0.1875f, 0.5f }, options);
    sample(half, expected, 0.5f);
    options.fxaa = true;
    auto post    = fixture.solid({ 0.0625f, 0.125f, 0.1875f, 0.5f }, options);
    sample(post, expected, 0.5f);
    auto transparent = fixture.solid({ 4, 2, 1, 0 }, options);
    for (unsigned c = 0; c < 4; ++c) near(channel(transparent, c), 0);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(ColorBalance, BloomAndToneMappingOrder) {
    Fixture      fixture(64, 64);
    ColorBalance balance { { 0.1f, 0, 0 }, { 0, -0.15f, 0 }, { 0, 0, 0.1f } };
    auto         options  = linear();
    options.color_balance = Some(balance);
    options.exposure      = 2;
    options.grading       = { 0.75f, 0.4f, 0.2f };
    Vec3  mapped          = balanced({ 0.125f, 0.25f, 0.375f }, balance) * 1.5f;
    float luminance       = dot(mapped, { 0.2126f, 0.7152f, 0.0722f });
    mapped                = Vec3 { luminance, luminance, luminance } * 0.6f + mapped * 0.4f;
    mapped                = { maximum(mapped.x * 1.2f - 0.1f, 0),
                              maximum(mapped.y * 1.2f - 0.1f, 0),
                              maximum(mapped.z * 1.2f - 0.1f, 0) };
    auto image            = fixture.solid({ 0.125f, 0.25f, 0.375f, 1 }, options);
    sample(image, mapped);
    options.exposure = 1;
    options.grading  = {};
    options.bloom    = Some(Bloom { 1, 0, 0.15f });
    float row6 = 0.96875f * 0.96875f, row10 = 0.998046875f * 0.998046875f;
    float weights  = 0.95f + row6 * (0.81f + 0.6f + row10 * (0.39f + 0.25f));
    float combined = 0.25f + 0.0625f * weights;
    auto  bloom    = fixture.solid({ 0.25f, 0.25f, 0.25f, 1 }, options);
    sample(bloom, balanced({ combined, combined, combined }, balance));
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(ColorBalance, NumericParametersReuseGraph) {
    Fixture fixture(64, 64);
    auto    options = linear();
    fixture.solid({ 0.125f, 0.25f, 0.375f, 1 }, options);
    auto compiles = fixture.renderer.graph_statistics().graph_compiles;
    options.color_balance =
        Some(ColorBalance { {}, { 0, 0, -0.06666667f }, { -0.01111111f, 0, 0 } });
    fixture.solid({ 0.125f, 0.25f, 0.375f, 1 }, options);
    options.color_balance = Some(ColorBalance { { -1, 0, 1 }, { 1, -1, 0 }, { 0, 1, -1 } });
    fixture.solid({ 0.125f, 0.25f, 0.375f, 1 }, options);
    options.color_balance = Some(ColorBalance {});
    fixture.solid({ 0.125f, 0.25f, 0.375f, 1 }, options);
    rstd_assert(fixture.renderer.graph_statistics().graph_compiles == compiles);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(ColorBalance, ValidationAndPending) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto renderer =
        DisplayRenderer::create(session, vk::PipelinePreparationMode::Deferred).unwrap();
    auto  hdr     = texture(queue, vrento::TextureFormat::RGBA16F);
    auto  output  = texture(queue, vrento::TextureFormat::RGBA8);
    auto  work    = queue.Begin().unwrap();
    auto  initial = renderer.upload_statistics();
    float invalid[] {
        -1.01f, 1.01f, rstd::f32::INFINITY_.to_primitive(), rstd::f32::NAN_.to_primitive()
    };
    for (unsigned region = 0; region < 3; ++region)
        for (unsigned component = 0; component < 3; ++component)
            for (float value : invalid) {
                ColorBalance balance;
                Vec3*        values[] { &balance.shadows, &balance.midtones, &balance.highlights };
                auto&        vector = *values[region];
                if (component == 0)
                    vector.x = value;
                else if (component == 1)
                    vector.y = value;
                else
                    vector.z = value;
                auto options          = linear();
                options.color_balance = Some(balance);
                auto prepared         = renderer.prepare_target({ output }, options);
                rstd_assert(prepared.is_err() &&
                            prepared.unwrap_err().kind == ErrorKind::InvalidAsset);
                auto result = renderer.record(work, hdr, { output }, options);
                rstd_assert(result.is_err() && result.unwrap_err().kind == ErrorKind::InvalidAsset);
            }
    auto options          = linear();
    options.fxaa          = true;
    options.color_balance = Some(ColorBalance { {}, { 0.1f, 0, -0.05f }, {} });
    auto pending          = renderer.record(work, hdr, { output }, options);
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    rstd_assert(work.valid() && work.receipt().status() == tex::SubmissionStatus::Recording);
    rstd_assert(output.texture().content_revision() == 0 &&
                renderer.statistics().recorded_frames == 0);
    rstd_assert(renderer.graph_statistics().graph_compiles == 0);
    for (unsigned i = 0; i < 5; ++i) {
        rstd_assert(renderer.upload_statistics().queued_copies[i] == initial.queued_copies[i]);
        rstd_assert(renderer.upload_statistics().recorded_copies[i] == initial.recorded_copies[i]);
    }
    auto start = rstd::time::Instant::now();
    while (renderer.prepare_target({ output }, options).unwrap() ==
           vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    clear(session, work, hdr, { 0.125f, 0.25f, 0.375f, 1 });
    renderer.record(work, hdr, { output }, options).unwrap();
    renderer.close();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    sample(pixels(session, queue, output),
           balanced({ 0.125f, 0.25f, 0.375f }, *options.color_balance));
    rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::color_balance
