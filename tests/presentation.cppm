module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:presentation;
import vecgltf.presentation;
import rstd.test;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
namespace gltf = vecgltf;

namespace vecgltf_test::presentation
{
auto parse(ref<str> text) -> Result<gltf::PresentationSpec, gltf::Error> {
    return gltf::ParsePresentation(text.as_bytes(), "test-profile"_str);
}
auto model(gltf::Vec3 scale = { 1, 1, 1 }) -> Arc<gltf::Asset> {
    auto asset        = Arc<gltf::Asset>::make();
    asset->generation = u64(17);
    gltf::Node root;
    root.scale       = scale;
    root.translation = { 1, 2, 3 };
    asset->nodes.push(rstd::move(root));
    gltf::Scene scene;
    scene.roots.push(0);
    asset->scenes.push(rstd::move(scene));
    return asset;
}
void near(float a, float b) {
    rstd_assert(gltf::absolute(a - b) < 0.00001f);
}

TEST(Presentation, SchemaAndScalarValidation) {
    rstd_assert(parse(R"({"version":2})"_str).is_ok());
    ref<str> invalid[] {
        R"({})"_str,
        R"({"version":1})"_str,
        R"({"version":null})"_str,
        R"({"version":2,"camera":null})"_str,
        R"({"version":2,"display":{"exposure":false}})"_str,
        R"({"version":2,"display":{"exposure":1e100}})"_str,
        R"({"version":2,"display":{"exposure":1e30,"brightness":1e30}})"_str,
        R"({"version":2,"directional_lights":null})"_str,
        R"({"version":2,"point_lights":[{"position":[0,0,0],"attenuation_units":"unknown"}]})"_str,
        R"({"version":2,"camera":{"position":[0,0,3],"target":[0,0,0],"clip_units":"unknown"}})"_str
    };
    for (auto text : invalid) rstd_assert(parse(text).is_err());
    auto zero =
        parse(
            R"({"version":2,"directional_lights":[],"point_lights":[],"display":{"exposure":0,"contrast":-1,"fxaa":false},"environment":{"ibl_intensity":0,"background":false}})"_str)
            .unwrap();
    rstd_assert(zero.scene.directional_lights && zero.scene.directional_lights.values().is_empty());
    rstd_assert(zero.scene.point_lights && zero.scene.point_lights.values().is_empty());
    near(zero.display.exposure, 0);
    Vec<u8> large;
    large.resize(usize(65537), u8(32));
    auto failed = gltf::ParsePresentation(large.as_slice());
    rstd_assert(failed.is_err() && failed.unwrap_err().kind == gltf::ErrorKind::Limit);
}

TEST(Presentation, AssetGenerationAndMaterialBinding) {
    auto asset = model();
    auto bound =
        gltf::BindPresentation(parse(R"({"version":2})"_str).unwrap(), asset.clone()).unwrap();
    rstd_assert(&*bound.asset() == &*asset);
    gltf::SceneInstance same(asset.clone()), other(model());
    rstd_assert(gltf::ResolvePresentation(bound, same).is_ok());
    rstd_assert(gltf::ResolvePresentation(bound, other).is_err());
    asset->generation = u64(18);
    rstd_assert(gltf::ResolvePresentation(bound, same).is_err());
    auto invalid =
        parse(R"({"version":2,"material_overrides":[{"material":0,"alpha_mode":"dither"}]})"_str)
            .unwrap();
    rstd_assert(gltf::BindPresentation(rstd::move(invalid), asset.clone()).is_err());
    asset = model();
    gltf::Material material;
    material.alpha = gltf::AlphaMode::Blend;
    asset->materials.push(rstd::move(material));
    auto valid =
        gltf::BindPresentation(
            parse(
                R"({"version":2,"material_overrides":[{"material":0,"alpha_mode":"dither"}]})"_str)
                .unwrap(),
            asset.clone())
            .unwrap();
    rstd_assert(valid.prepare_options().material_overrides.values().len() == usize(1));
}

TEST(Presentation, FileEntryUsesTheSameSchemaAndRestrictedResources) {
    auto file = gltf::LoadPresentation("samples/presentation.json"_str).unwrap();
    rstd_assert(file.camera && file.display.tone_mapping == gltf::ToneMapping::Linear);
    auto bound = gltf::BindPresentation(rstd::move(file), model()).unwrap();
    rstd_assert(bound.spec().scene.background_color && bound.spec().display.color_balance);
    auto directory = rstd::fs::TempDir::make("vecgltf-presentation"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(directory.path())
                         .join(rstd::path::PathBuf::from("root"_str).as_path());
    rstd::fs::create_dir_all(root.as_path()).unwrap();
    auto outside = rstd::path::PathBuf::from(directory.path())
                       .join(rstd::path::PathBuf::from("outside.hdr"_str).as_path());
    rstd::fs::write(outside.as_path(), "outside"_str.as_bytes()).unwrap();
    auto spec =
        parse(
            R"({"version":2,"environment":{"preprocessed":{"background":"../outside.hdr","diffuse":"../outside.hdr","brdf":"../outside.hdr","specular":["../outside.hdr"]}}})"_str)
            .unwrap();
    gltf::PresentationResources resources;
    resources.base_directory = rstd::move(root);
    auto escaped = gltf::BindPresentation(rstd::move(spec), model(), rstd::move(resources));
    rstd_assert(escaped.is_err() && escaped.unwrap_err().location == "resource root"_str);
}

TEST(Presentation, SceneRootCameraKeepsWorldClipUnits) {
    float scale = 0.0034276118967682123f;
    auto  asset = model({ scale, scale, scale });
    auto  bound =
        gltf::BindPresentation(
            parse(
                R"({"version":2,"camera":{"position":[0,0,1000],"target":[0,0,0],"up":[0,1,0],"yfov":0.8,"reference":"scene-root"}})"_str)
                .unwrap(),
            asset.clone())
            .unwrap();
    gltf::SceneInstance instance(asset.clone());
    gltf::OrbitCamera   fit;
    fit.fit(instance.bounds());
    auto resolved = gltf::ResolvePresentation(bound, instance).unwrap();
    near(resolved.camera->position.z, 3 + 1000 * scale);
    near(resolved.camera->projection.near, fit.frame().projection.near);
    near(resolved.camera->projection.far, 0);
    auto explicit_clip =
        gltf::BindPresentation(
            parse(
                R"({"version":2,"camera":{"position":[0,0,1000],"target":[0,0,0],"reference":"scene-root","near":2,"far":2000,"clip_units":"reference"}})"_str)
                .unwrap(),
            asset.clone())
            .unwrap();
    auto reference = gltf::ResolvePresentation(explicit_clip, instance).unwrap();
    near(reference.camera->projection.near, 2 * scale);
    near(reference.camera->projection.far, 2000 * scale);
    auto far_only =
        gltf::BindPresentation(
            parse(
                R"({"version":2,"camera":{"position":[0,0,1],"target":[0,0,0],"far":0.0001}})"_str)
                .unwrap(),
            asset.clone())
            .unwrap();
    auto clip = gltf::ResolvePresentation(far_only, instance).unwrap();
    near(clip.camera->projection.near, fit.frame().projection.near);
    near(clip.camera->projection.far, 0.0001f);
    gltf::PresentationOverrides overrides;
    overrides.camera = Some(fit.frame());
    auto interactive = gltf::ResolvePresentation(bound, instance, overrides).unwrap();
    near(interactive.camera->position.z, fit.frame().position.z);
}

TEST(Presentation, InvalidCameraReferencesAreRejected) {
    auto text =
        R"({"version":2,"camera":{"position":[0,0,10],"target":[0,0,0],"reference":"scene-root"}})"_str;
    rstd_assert(gltf::BindPresentation(parse(text).unwrap(), model({ 1, 2, 1 })).is_err());
    rstd_assert(gltf::BindPresentation(parse(text).unwrap(), model({ -1, 1, 1 })).is_err());
    auto asset = model();
    asset->nodes.push(gltf::Node {});
    gltf::at(asset->scenes, 0).roots.push(1);
    rstd_assert(gltf::BindPresentation(parse(text).unwrap(), asset.clone()).is_err());
    auto invalid =
        parse(R"({"version":2,"camera":{"position":[0,0,10],"target":[0,0,0],"reference":99}})"_str)
            .unwrap();
    rstd_assert(gltf::BindPresentation(rstd::move(invalid), asset.clone()).is_err());
}

TEST(Presentation, PointAttenuationUsesDeclaredDistanceUnits) {
    auto asset = model({ 2, 2, 2 });
    auto bound =
        gltf::BindPresentation(
            parse(
                R"({"version":2,"point_lights":[{"position":[1,2,3],"reference":"scene-root","attenuation":[1,4,8],"attenuation_units":"reference"}]})"_str)
                .unwrap(),
            asset.clone())
            .unwrap();
    gltf::SceneInstance instance(asset.clone());
    auto                light = gltf::at(
        gltf::ResolvePresentation(bound, instance).unwrap().scene.point_lights.values(), 0);
    near(light.position.x, 3);
    near(light.position.y, 6);
    near(light.position.z, 9);
    near(light.attenuation.x, 1);
    near(light.attenuation.y, 2);
    near(light.attenuation.z, 2);
    auto nonuniform = model({ 1, 2, 1 });
    auto reference =
        parse(
            R"({"version":2,"point_lights":[{"position":[1,2,3],"reference":"scene-root","attenuation_units":"reference"}]})"_str)
            .unwrap();
    rstd_assert(gltf::BindPresentation(rstd::move(reference), nonuniform.clone()).is_err());
    auto world =
        parse(
            R"({"version":2,"point_lights":[{"position":[1,2,3],"reference":"scene-root","attenuation_units":"world"}]})"_str)
            .unwrap();
    rstd_assert(gltf::BindPresentation(rstd::move(world), nonuniform.clone()).is_ok());
}

TEST(Presentation, SpotReference) {
    auto asset = model({ 2, 2, 2 });
    auto spec =
        parse(
            R"({"version":2,"spot_lights":[{"position":[1,2,3],"direction":[0,0,-1],"inner_cos":0.9,"outer_cos":0.8,"reference":"scene-root","attenuation":[1,4,8],"attenuation_units":"reference"}]})"_str)
            .unwrap();
    auto                bound = gltf::BindPresentation(rstd::move(spec), asset.clone()).unwrap();
    gltf::SceneInstance instance(asset.clone());
    auto                resolved = gltf::ResolvePresentation(bound, instance).unwrap();
    auto                light    = gltf::at(resolved.scene.spot_lights.values(), 0);
    near(light.point.position.x, 3);
    near(light.point.attenuation.y, 2);
    near(light.point.attenuation.z, 2);
    near(light.direction.z, -1);
    near(light.inner_cos, 0.9f);
    rstd_assert(
        parse(R"({"version":2,"spot_lights":[{"position":[0,0,0],"direction":[0,0,0]}]})"_str)
            .is_err());
    rstd_assert(
        parse(
            R"({"version":2,"spot_lights":[{"position":[0,0,0],"direction":[0,0,-1],"inner_cos":0.1,"outer_cos":0.9}]})"_str)
            .is_err());
}

TEST(Presentation, ExposureAndViewEnvironmentResolveWithoutReparsing) {
    auto asset = model();
    auto bound =
        gltf::BindPresentation(
            parse(
                R"({"version":2,"environment":{"reference":"view"},"display":{"exposure":2}})"_str)
                .unwrap(),
            asset.clone())
            .unwrap();
    gltf::SceneInstance    instance(asset.clone());
    gltf::CameraProjection projection;
    auto camera = gltf::camera_frame(gltf::CameraLookAt { { 10, 0, 0 }, {} }, projection).unwrap();
    auto resolved = gltf::ResolvePresentation(
                        bound, instance, { .exposure_multiplier = 3, .camera = Some(camera) })
                        .unwrap();
    near(resolved.display.exposure, 6);
    auto matrix = gltf::compose({}, camera.orientation, { 1, 1, 1 });
    for (unsigned x = 0; x < 3; ++x)
        for (unsigned y = 0; y < 3; ++y)
            near(resolved.scene.world_to_environment.m[x * 4 + y], matrix.m[y * 4 + x]);
    near(bound.spec().display.exposure, 2);
}

TEST(Presentation, NodeReferencesFollowCurrentPose) {
    auto       asset = model();
    gltf::Node child;
    child.parent = 0;
    asset->nodes.push(rstd::move(child));
    gltf::at(asset->nodes, 0).children.push(1);
    gltf::Animation animation;
    animation.end = 1;
    gltf::AnimationSampler sampler;
    sampler.components = 3;
    sampler.times.push(0.0f);
    sampler.times.push(1.0f);
    float positions[] { 0, 0, 0, 4, 0, 0 };
    for (auto v : positions) sampler.values.push(float(v));
    animation.samplers.push(rstd::move(sampler));
    animation.channels.push({ .node = 1, .sampler = 0, .path = gltf::AnimationPath::Translation });
    asset->animations.push(rstd::move(animation));
    auto spec =
        parse(
            R"({"version":2,"directional_lights":[{"direction":[0,0,1],"reference":1}],"point_lights":[{"position":[1,0,0],"reference":1},{"position":[0,1,0],"reference":"view"}],"shadow_catcher":{"position":[0,0,0],"reference":1}})"_str)
            .unwrap();
    auto                bound = gltf::BindPresentation(rstd::move(spec), asset.clone()).unwrap();
    gltf::SceneInstance instance(asset.clone());
    auto                first = gltf::ResolvePresentation(bound, instance).unwrap();
    near(gltf::at(first.scene.point_lights.values(), 0).position.x, 2);
    rstd_assert(gltf::at(first.scene.point_lights.values(), 1).space == gltf::LightSpace::View);
    rstd_assert(instance.set_animation(0));
    instance.set_time(0.5f);
    auto next = gltf::ResolvePresentation(bound, instance).unwrap();
    near(gltf::at(next.scene.point_lights.values(), 0).position.x, 4);
    near(next.scene.shadow_catcher->transform.m[12], 3);
    near(gltf::at(next.scene.directional_lights.values(), 0).direction.z, 1);
}

struct Reads {
    unsigned count {};
    Vec<u8>  bytes;
};
struct Resolver {
    Arc<Reads> reads;
    auto       Read(const gltf::ResourceRequest& request) const
        -> Result<gltf::ResourceBytes, gltf::Error> {
        rstd_assert(request.kind == gltf::ResourceKind::Image);
        ++reads->count;
        return Ok(gltf::ResourceBytes(reads->bytes.clone()));
    }
};
} // namespace vecgltf_test::presentation

namespace rstd
{
template<>
struct Impl<gltf::ResourceResolver, vecgltf_test::presentation::Resolver>
    : ImplBase<vecgltf_test::presentation::Resolver> {
    auto Read(const gltf::ResourceRequest& request) const
        -> Result<gltf::ResourceBytes, gltf::Error> {
        return this->self().Read(request);
    }
};
} // namespace rstd

namespace vecgltf_test::presentation
{
TEST(Presentation, EnvironmentReadsAndDecodesEachResourceOnce) {
    auto reads = Arc<Reads>::make();
    reads->bytes =
        Vec<u8>::from("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n"_str.as_bytes());
    int rgbe[] { 128, 64, 32, 129 };
    for (auto value : rgbe) reads->bytes.push(u8(value));
    auto source =
        R"({"version":2,"environment":{"preprocessed":{"background":"same.hdr","diffuse":"same.hdr","brdf":"same.hdr","specular":["same.hdr"]}}})"_str;
    auto resources = [&]() {
        gltf::PresentationResources value;
        value.base_directory = rstd::path::PathBuf::from("virtual"_str);
        value.resolver       = gltf::ResourceAccess(
            Arc<dyn<gltf::ResourceResolver>>::make(Resolver { reads.clone() }));
        return value;
    };
    auto bound = gltf::BindPresentation(parse(source).unwrap(), model(), resources()).unwrap();
    rstd_assert(reads->count == 1);
    auto images = bound.take_environment().unwrap();
    rstd_assert(images.background.width == 1 && images.specular.len() == usize(1));
    near(gltf::at(images.background.rgba, 0), 1);
    near(gltf::at(images.diffuse.rgba, 1), 0.5f);
    rstd_assert(! bound.take_environment() && reads->count == 1);
    auto limited      = resources();
    limited.max_bytes = usize(1);
    rstd_assert(
        gltf::BindPresentation(parse(source).unwrap(), model(), rstd::move(limited)).is_err());
}
} // namespace vecgltf_test::presentation
