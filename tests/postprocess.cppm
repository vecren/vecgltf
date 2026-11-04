module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:postprocess;
import vecgltf.display;
import rstd.test;
import :display;

using namespace vecgltf;

namespace vecgltf_test::postprocess
{
namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;
using vecgltf_test::display::channel;
using vecgltf_test::display::clear;
using vecgltf_test::display::near;
using vecgltf_test::display::pixels;
using vecgltf_test::display::texture;

struct Fixture {
    vk::DeviceSession   session { vk::DeviceSession::create({ .validation = true }).unwrap() };
    vk::TextureContext  queue { vk::TextureContext::create(session).unwrap() };
    DisplayRenderer     renderer { DisplayRenderer::create(session).unwrap() };
    unsigned            width, height;
    tex::TextureViewRef hdr, coverage, output;
    Fixture(unsigned w = 65, unsigned h = 35)
        : width(w),
          height(h),
          hdr(texture(queue, vrento::TextureFormat::RGBA16F, w, h)),
          coverage(texture(queue, vrento::TextureFormat::RGBA16F, w, h)),
          output(texture(queue, vrento::TextureFormat::RGBA8, w, h)) {}
    auto finish(tex::Submission work, DisplayOptions options, bool separate_coverage) -> Vec<u8> {
        renderer
            .record(work,
                    DisplayInput {
                        hdr, separate_coverage ? Some(tex::TextureViewRef(coverage)) : None() },
                    { output },
                    options)
            .unwrap();
        queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
        return pixels(session, queue, output);
    }
    auto solid(Vec4 color, DisplayOptions options, Option<float> amount = None()) -> Vec<u8> {
        auto work = queue.Begin().unwrap();
        clear(session, work, hdr, color);
        if (amount) clear(session, work, coverage, { 0, 0, 0, *amount });
        return finish(rstd::move(work), options, bool(amount));
    }
    auto pattern(DisplayOptions options, bool impulse = false, Option<float> amount = None())
        -> Vec<u8> {
        auto               work = queue.Begin().unwrap();
        tex::TextureUpload upload;
        upload.target = hdr;
        upload.width  = width;
        upload.height = height;
        Vec<u8> bytes;
        auto    half = [&](unsigned value) {
            bytes.push(u8(value & 255));
            bytes.push(u8(value >> 8));
        };
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                unsigned value = impulse ? (x == width / 2 && y == height / 2 ? 0x3c00 : 0)
                                         : (x > y + 3 ? 0x3800 : 0x3000);
                for (unsigned c = 0; c < 3; ++c) half(value);
                half(0x3c00);
            }
        upload.bytes = rstd::move(bytes);
        work.commands()->Upload(rstd::move(upload)).unwrap();
        if (amount) clear(session, work, coverage, { 0, 0, 0, *amount });
        return finish(rstd::move(work), options, bool(amount));
    }
    unsigned sample(const Vec<u8>& image, unsigned x, unsigned y, unsigned c = 0) const {
        return channel(image, c, x, y, width);
    }
};
auto linear() -> DisplayOptions {
    DisplayOptions options;
    options.tone_mapping = ToneMapping::Linear;
    return options;
}
float encode(float value) {
    return value <= 0.0031308f
               ? value * 12.92f
               : 1.055f * rstd::f32(value).powf(rstd::f32(1.0f / 2.4f)).to_primitive() - 0.055f;
}
float decode(float value) {
    return value <= 0.04045f
               ? value / 12.92f
               : rstd::f32((value + 0.055f) / 1.055f).powf(rstd::f32(2.4f)).to_primitive();
}
unsigned quantize(float value) {
    return unsigned(clamp(value, 0, 1) * 255 + 0.5f);
}

TEST(Postprocess, BloomWeightsAndCoverage) {
    Fixture fixture;
    auto    options   = linear();
    options.bloom     = Some(Bloom { 1, 0, 0.15f });
    auto     image    = fixture.solid({ 0.25f, 0.25f, 0.25f, 1 }, options);
    float    row6     = 0.96875f * 0.96875f;
    float    row10    = 0.998046875f * 0.998046875f;
    float    weights  = 0.95f + row6 * (0.81f + 0.6f + row10 * (0.39f + 0.25f));
    unsigned expected = quantize(0.25f + 0.0625f * weights);
    unsigned rows[] { 0, 17, 34 }, columns[] { 0, 32, 64 };
    for (unsigned y : rows)
        for (unsigned x : columns) {
            near(fixture.sample(image, x, y), expected);
            near(fixture.sample(image, x, y, 3), 255);
        }
    auto no_geometry = fixture.solid({ 0.25f, 0.25f, 0.25f, 1 }, options, Some(0.0f));
    near(fixture.sample(no_geometry, 32, 17), 64);
    auto half = fixture.solid({ 0.125f, 0.125f, 0.125f, 0.5f }, options);
    near(fixture.sample(half, 32, 17), quantize((0.25f + 0.015625f * weights) * 0.5f));
    near(fixture.sample(half, 32, 17, 3), 128);
    options.bloom->threshold = 0.25f;
    auto threshold           = fixture.solid({ 0.25f, 0.25f, 0.25f, 1 }, options);
    near(fixture.sample(threshold, 32, 17), 64);
    Fixture minimum(1, 1);
    options.bloom->threshold = 0;
    auto single              = minimum.solid({ 0.25f, 0.25f, 0.25f, 1 }, options);
    near(minimum.sample(single, 0, 0), expected);
    rstd_assert(fixture.session.validation_errors() == 0 &&
                minimum.session.validation_errors() == 0);
}
TEST(Postprocess, BloomImpulse) {
    Fixture fixture(64, 64);
    auto    options = linear();
    options.bloom   = Some(Bloom { 16, 0, 0.15f });
    auto bloom      = fixture.pattern(options, true);
    rstd_assert(fixture.sample(bloom, 31, 32) > 0);
    rstd_assert(fixture.sample(bloom, 31, 32) > fixture.sample(bloom, 0, 0));
    options.bloom->threshold = 1;
    auto threshold           = fixture.pattern(options, true);
    near(fixture.sample(threshold, 31, 32), 0);
    near(fixture.sample(threshold, 32, 32), 255);
    options.bloom->threshold = 0;
    auto background          = fixture.pattern(options, true, Some(0.0f));
    near(fixture.sample(background, 31, 32), 0);
    near(fixture.sample(background, 32, 32), 255);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Postprocess, FxaaSharpenSources) {
    Fixture fixture(64, 64);
    auto    options        = linear();
    auto    original       = fixture.pattern(options);
    options.fxaa           = true;
    auto filtered          = fixture.pattern(options);
    options.sampling_scale = 1.75f;
    auto rescaled          = fixture.pattern(options);
    rstd_assert(filtered == rescaled);
    options.sampling_scale = 1;
    options.sharpen        = 0.2f;
    auto sharpened         = fixture.pattern(options);
    auto background        = fixture.pattern(options, false, Some(0.0f));
    bool antialiased {}, distinguished {};
    for (unsigned y = 2; y < 62; ++y)
        for (unsigned x = 2; x < 62; ++x) {
            unsigned center = fixture.sample(filtered, x, y);
            antialiased |= center != fixture.sample(original, x, y);
            near(fixture.sample(background, x, y), center);
            float filtered_center = encode(float(center) / 255);
            float neighbors {}, filtered_neighbors {};
            int   offsets[] { -1, 1 };
            for (int dy : offsets)
                for (int dx : offsets) {
                    unsigned px = unsigned(int(x) + dx), py = unsigned(int(y) + dy);
                    neighbors += encode(px > py + 3 ? 0.5f : 0.125f);
                    filtered_neighbors += encode(float(fixture.sample(filtered, px, py)) / 255);
                }
            unsigned expected =
                quantize(decode(filtered_center + 0.2f * (4 * filtered_center - neighbors)));
            unsigned alternative = quantize(
                decode(filtered_center + 0.2f * (4 * filtered_center - filtered_neighbors)));
            unsigned actual = fixture.sample(sharpened, x, y);
            rstd_assert(actual + 3 >= expected && actual <= expected + 3);
            if (expected + 5 < alternative || alternative + 5 < expected) distinguished = true;
            near(fixture.sample(sharpened, x, y, 3), 255);
        }
    rstd_assert(antialiased && distinguished);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Postprocess, GrainAndAlpha) {
    Fixture fixture;
    auto    options = linear();
    options.grain   = Some(Grain { 1, 0 });
    auto first      = fixture.solid({ 0.25f, 0.25f, 0.25f, 0.5f }, options);
    auto repeated   = fixture.solid({ 0.25f, 0.25f, 0.25f, 0.5f }, options);
    rstd_assert(first == repeated);
    unsigned low = 255, high {};
    for (unsigned y = 0; y < fixture.height; ++y)
        for (unsigned x = 0; x < fixture.width; ++x) {
            unsigned value = fixture.sample(first, x, y);
            if (value < low) low = value;
            if (value > high) high = value;
            near(fixture.sample(first, x, y, 1), value);
            near(fixture.sample(first, x, y, 2), value);
            near(fixture.sample(first, x, y, 3), 128);
        }
    rstd_assert(high > low + 4);
    auto     black       = fixture.solid({ 0, 0, 0, 1 }, options);
    auto     white       = fixture.solid({ 1, 1, 1, 1 }, options);
    auto     transparent = fixture.solid({ 4, 2, 1, 0 }, options);
    unsigned rows[] { 0, 17, 34 }, columns[] { 0, 32, 64 };
    for (unsigned y : rows)
        for (unsigned x : columns) {
            for (unsigned c = 0; c < 3; ++c) {
                near(fixture.sample(black, x, y, c), 0);
                near(fixture.sample(white, x, y, c), 255);
            }
            for (unsigned c = 0; c < 4; ++c) near(fixture.sample(transparent, x, y, c), 0);
        }
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Postprocess, NumericParametersReuseGraph) {
    Fixture fixture;
    auto    options = linear();
    options.bloom   = Some(Bloom { 0.36f, 0.3f, 0.15f });
    options.fxaa    = true;
    options.sharpen = 0.3645f;
    options.grain   = Some(Grain { 0.025f, 0 });
    fixture.solid({ 0.25f, 0.25f, 0.25f, 1 }, options);
    auto compiles          = fixture.renderer.graph_statistics().graph_compiles;
    options.bloom          = Some(Bloom { 0.5f, 0.1f, 0.8f });
    options.fxaa           = false;
    options.sharpen        = 0.1f;
    options.grain          = Some(Grain { 0.2f, 0.25f });
    options.sampling_scale = 1.25f;
    fixture.solid({ 0.5f, 0.25f, 0.125f, 1 }, options);
    rstd_assert(fixture.renderer.graph_statistics().graph_compiles == compiles);
    auto first = fixture.queue.Begin().unwrap();
    clear(fixture.session, first, fixture.hdr, { 0.25f, 0.25f, 0.25f, 1 });
    fixture.renderer.record(first, fixture.hdr, { fixture.output }, options).unwrap();
    auto submitted   = fixture.queue.Submit(rstd::move(first)).unwrap();
    auto smaller_hdr = texture(fixture.queue, vrento::TextureFormat::RGBA16F, 17, 9);
    auto smaller     = texture(fixture.queue, vrento::TextureFormat::RGBA8, 17, 9);
    auto second      = fixture.queue.Begin().unwrap();
    clear(fixture.session, second, smaller_hdr, { 0.25f, 0.25f, 0.25f, 1 });
    fixture.renderer.record(second, smaller_hdr, { smaller }, options).unwrap();
    rstd_assert(fixture.renderer.graph_statistics().graph_compiles == compiles + 1);
    fixture.renderer.close();
    fixture.queue.wait(fixture.queue.Submit(rstd::move(second)).unwrap()).unwrap();
    fixture.queue.wait(submitted).unwrap();
    auto resized = pixels(fixture.session, fixture.queue, smaller);
    near(channel(resized, 3, 8, 4, 17), 255);
    rstd_assert(channel(resized, 0, 8, 4, 17) > 0);
    rstd_assert(fixture.session.validation_errors() == 0);
}
TEST(Postprocess, DeferredPreparation) {
    auto session = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto display = DisplayRenderer::create(session, vk::PipelinePreparationMode::Deferred).unwrap();
    auto source  = texture(queue, vrento::TextureFormat::RGBA16F);
    auto output  = texture(queue, vrento::TextureFormat::RGBA8);
    auto wrong_coverage = texture(queue, vrento::TextureFormat::RGBA16F, 32, 32);
    auto work           = queue.Begin().unwrap();
    auto options        = linear();
    options.bloom       = Some(Bloom { 0.36f, 0.3f, 0.15f });
    options.fxaa        = true;
    options.sharpen     = 0.3645f;
    options.grain       = Some(Grain { 0.025f, 0 });
    auto initial        = display.upload_statistics();
    auto other_session  = vk::DeviceSession::create({ .validation = true }).unwrap();
    auto other_queue    = vk::TextureContext::create(other_session).unwrap();
    auto other_work     = other_queue.Begin().unwrap();
    auto mismatch       = display.record(other_work, source, { output }, options);
    rstd_assert(mismatch.is_err() && mismatch.unwrap_err().kind == ErrorKind::Device);
    rstd_assert(other_work.valid() &&
                other_work.receipt().status() == tex::SubmissionStatus::Recording);
    auto invalid =
        display.record(work,
                       DisplayInput { source, Some(tex::TextureViewRef(wrong_coverage)) },
                       { output },
                       options);
    rstd_assert(invalid.is_err() && invalid.unwrap_err().kind == ErrorKind::InvalidAsset);
    auto pending = display.record(work, source, { output }, options);
    rstd_assert(pending.is_err() && pending.unwrap_err().kind == ErrorKind::Pending);
    rstd_assert(work.receipt().status() == tex::SubmissionStatus::Recording);
    rstd_assert(output.texture().content_revision() == 0 &&
                display.statistics().recorded_frames == 0);
    rstd_assert(display.graph_statistics().graph_compiles == 0);
    for (unsigned i = 0; i < 5; ++i) {
        rstd_assert(display.upload_statistics().queued_copies[i] == initial.queued_copies[i]);
        rstd_assert(display.upload_statistics().recorded_copies[i] == initial.recorded_copies[i]);
    }
    auto start = rstd::time::Instant::now();
    while (display.prepare_target({ output }, options).unwrap() ==
           vrento::resource_registry::PipelineReadiness::Pending) {
        rstd_assert(start.elapsed().as_millis() < u64(5000));
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
    }
    clear(session, work, source, { 0.25f, 0.25f, 0.25f, 1 });
    display.record(work, source, { output }, options).unwrap();
    display.close();
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto image = pixels(session, queue, output);
    near(channel(image, 3), 255);
    rstd_assert(channel(image, 0) > 0);
    rstd_assert(session.validation_errors() == 0 && other_session.validation_errors() == 0);
}
} // namespace vecgltf_test::postprocess
