export module vecgltf.bench.load;
import vecgltf;
import vecgltf.render;
import rstd.bench;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
namespace gltf = vecgltf;
namespace vk = vrento::vulkan;
namespace tex = vrento::texture;
namespace probe = rstd::bench::probe;

namespace vecgltf_bench {
auto argument(const char *value) -> ref<str> {
  return rstd::ffi::CStr::from_ptr(value).to_str().unwrap_or(""_str);
}

constexpr const char *event_labels[]{
    "load.asset",          "load.document",         "load.buffers",
    "load.validation",     "load.materials",        "load.geometry",
    "load.scene",          "load.animations",       "load.images",
    "load.image.decode",   "load.image.copy",       "render.create",
    "render.initialize",   "render.environment",    "render.environment.process",
    "render.environment.image", "render.prepare.asset", "render.prepare.materials",
    "render.prepare.geometry",  "render.texture",       "render.texture.copy",
    "render.texture.mipmaps",   "render.texture.image_prepare", "render.uploads",
    "render.texture.gpu_mipmaps"};
static_assert(sizeof(event_labels) / sizeof(event_labels[0]) ==
              unsigned(gltf::LoadEvent::Count));

struct Recorder {
  mutable probe::ProbeRecorder recorder;
  Vec<probe::ProbeId> events;
  mutable Vec<probe::SpanGuard> active;
  mutable bool failed{};

  Recorder(probe::ProbeRecorder value, Vec<probe::ProbeId> ids)
      : recorder(rstd::move(value)), events(rstd::move(ids)),
        active(Vec<probe::SpanGuard>::with_capacity(usize(64))) {}

  auto span(probe::ProbeId id) const -> probe::SpanGuard {
    return recorder.span(id);
  }
  void Begin(gltf::LoadEvent event) const {
    auto index = usize(unsigned(event));
    if (index >= events.len()) {
      failed = true;
      active.push(probe::SpanGuard{});
      return;
    }
    active.push(recorder.span(events[index]));
    if (!active.last().unwrap().get().is_active())
      failed = true;
  }
  void End() const {
    auto guard = active.pop();
    if (!guard || guard->finish().is_err())
      failed = true;
  }
};

struct Observer {
  Arc<Recorder> recorder;
  void Begin(gltf::LoadEvent event) const { recorder->Begin(event); }
  void End() const { recorder->End(); }
};

struct StartupProbes {
  probe::ProbeId total, device, scene, output, frame, record, submit, wait;
};
struct Counts {
  usize images{}, vertices{}, image_pixels{}, rgba_bytes{};
};
struct Output {
  vk::TextureContext queue;
  tex::TextureViewRef view;
};

void report(const gltf::Error &error) {
  rstd::io::eprintln("{} [{}] {}: {}", error.source, unsigned(error.kind),
                    error.location, error.message);
}
template <class Error>
bool failure(ref<str> operation, const Error &error) {
  rstd::io::eprintln("{}: {}", operation, error);
  return false;
}

auto make_output(vk::DeviceSession session, VkExtent2D extent)
    -> Result<Output, tex::Error> {
  auto created = vk::TextureContext::create(rstd::move(session));
  if (created.is_err())
    return Err(created.unwrap_err());
  auto queue = created.unwrap();
  auto device = dyn<tex::TextureDevice>::from_ref(queue);
  tex::TextureDesc description;
  description.extent = {extent.width, extent.height};
  description.usage = tex::Usage::Sampled | tex::Usage::ColorAttachment;
  auto texture = device->CreateTexture(description);
  if (texture.is_err())
    return Err(texture.unwrap_err());
  auto view = device->CreateView(texture.unwrap(), {});
  if (view.is_err())
    return Err(view.unwrap_err());
  return Ok(Output{rstd::move(queue), view.unwrap()});
}

bool sample(ref<str> path, gltf::LoadTrace trace, const Recorder &recorder,
            const StartupProbes &probes, Counts &counts) {
  auto total = recorder.span(probes.total);
  gltf::LoadOptions options;
  options.trace = trace;
  auto loaded = gltf::LoadAsset(path, options);
  if (loaded.is_err()) {
    report(loaded.unwrap_err());
    return false;
  }
  auto asset = loaded.unwrap();
  counts.images = asset->images.len();
  for (const auto &mesh : asset->meshes)
    for (const auto &primitive : mesh.primitives)
      counts.vertices += primitive.vertices.len();
  for (const auto &image : asset->images) {
    counts.image_pixels += usize(image.width) * usize(image.height);
    counts.rgba_bytes += image.rgba.len();
  }

  auto device = [&] {
    auto span = recorder.span(probes.device);
    return vk::DeviceSession::create();
  }();
  if (device.is_err())
    return failure("device"_str, device.unwrap_err());
  auto session = device.unwrap();
  constexpr VkExtent2D extent{320, 240};
  auto created = gltf::Renderer::create(
      session, extent, vk::PipelinePreparationMode::Synchronous, trace);
  if (created.is_err()) {
    report(created.unwrap_err());
    return false;
  }
  auto renderer = created.unwrap();
  auto prepared = renderer.prepare(asset.clone());
  if (prepared.is_err()) {
    report(prepared.unwrap_err());
    return false;
  }
  auto instance = [&] {
    auto span = recorder.span(probes.scene);
    return gltf::SceneInstance(asset.clone());
  }();
  gltf::OrbitCamera camera;
  camera.fit(instance.bounds());
  auto output = [&] {
    auto span = recorder.span(probes.output);
    return make_output(session, extent);
  }();
  if (output.is_err())
    return failure("output"_str, output.unwrap_err());
  auto target = output.unwrap();
  {
    auto first_frame = recorder.span(probes.frame);
    auto began = target.queue.Begin();
    if (began.is_err())
      return failure("record begin"_str, began.unwrap_err());
    auto work = began.unwrap();
    auto recorded = [&] {
      auto span = recorder.span(probes.record);
      return renderer.record(work, target.view, prepared.unwrap(), instance,
                             camera.view(float(extent.width) / extent.height));
    }();
    if (recorded.is_err()) {
      report(recorded.unwrap_err());
      return false;
    }
    auto submitted = [&] {
      auto span = recorder.span(probes.submit);
      return target.queue.Submit(rstd::move(work));
    }();
    if (submitted.is_err())
      return failure("submit"_str, submitted.unwrap_err());
    auto completed = [&] {
      auto span = recorder.span(probes.wait);
      return target.queue.wait(submitted.unwrap(), 60'000'000'000ull);
    }();
    if (completed.is_err())
      return failure("wait"_str, completed.unwrap_err());
  }
  if (total.finish().is_err())
    return failure("probe"_str, "cannot finish startup span"_str);
  renderer.close();
  return true;
}
} // namespace vecgltf_bench

namespace rstd {
template <>
struct Impl<gltf::LoadObserver, vecgltf_bench::Observer>
    : ImplBase<vecgltf_bench::Observer> {
  void Begin(gltf::LoadEvent event) const { self().Begin(event); }
  void End() const { self().End(); }
};
} // namespace rstd

int main(int argc, char **argv) {
  using namespace vecgltf_bench;
  String path;
  unsigned samples = 1;
  bool valid = true;
  for (int i = 1; i < argc && valid; ++i) {
    auto arg = argument(argv[i]);
    if (arg == "--samples"_str) {
      if (++i == argc) {
        valid = false;
        break;
      }
      auto parsed = rstd::from_str<u32>(argument(argv[i]));
      if (parsed.is_err() || parsed.unwrap() < u32(1) ||
          parsed.unwrap() > u32(32))
        valid = false;
      else
        samples = parsed.unwrap().to_primitive();
    } else if (arg.starts_with("-"_str) || !path.is_empty())
      valid = false;
    else
      path = String::make(arg);
  }
  if (!valid || path.is_empty()) {
    rstd::io::eprintln("usage: load model.gltf|model.glb [--samples 1..32]");
    return 2;
  }

  auto registry = probe::ProbeRegistry::new_();
  Vec<probe::ProbeId> events;
  for (auto label : event_labels)
    events.push(registry.register_probe(argument(label)).unwrap());
  auto id = [&](ref<str> label) {
    return registry.register_probe(label).unwrap();
  };
  StartupProbes probes{id("startup.total"_str), id("startup.device"_str),
                       id("startup.scene_instance"_str), id("startup.output"_str),
                       id("startup.first_frame"_str),
                       id("startup.first_frame.record"_str),
                       id("startup.first_frame.submit"_str),
                       id("startup.first_frame.wait"_str)};
  auto schema = rstd::move(registry).freeze();
  auto session = probe::ProbeSession::new_(schema.clone());
  probe::RecorderConfig config;
  config.sample_capacity = usize(16384);
  auto recorder =
      Arc<Recorder>::make(session.recorder(rstd::move(config)), rstd::move(events));
  gltf::LoadTrace trace(Arc<dyn<gltf::LoadObserver>>::make(Observer{recorder.clone()}));
  auto collector = probe::ProbeCollector::new_(schema.clone());
  rstd::io::println("input={} samples={} timing=host-wall-time validation=off", path,
                   samples);
  bool passed = true;
  for (unsigned i = 0; i < samples; ++i) {
    Counts counts;
    bool completed = sample(path.as_str(), trace, *recorder, probes, counts);
    rstd::io::println("sample={} images={} vertices={} image_pixels={} rgba_bytes={} "
                     "completed={}",
                     i + 1, counts.images, counts.vertices, counts.image_pixels,
                     counts.rgba_bytes, completed);
    auto batch = recorder->recorder.drain();
    if (batch.is_err()) {
      rstd::io::eprintln("probe: cannot drain recorded spans");
      passed = false;
      break;
    }
    auto ingested = collector.ingest(*batch);
    if (ingested.is_err()) {
      rstd::io::eprintln("probe: cannot collect recorded spans");
      passed = false;
      break;
    }
    if (!completed || recorder->failed) {
      passed = false;
      break;
    }
  }
  auto report = rstd::move(collector).finish();
  auto output = rstd::io::stdout();
  auto written = probe::write_text(output, report);
  if (written.is_err()) {
    rstd::io::eprintln("probe: cannot write timing report: {}", written.unwrap_err());
    passed = false;
  }
  if (report.dropped_samples() != usize() || !report.diagnostics().is_empty() ||
      recorder->failed) {
    rstd::io::eprintln("probe: timing report contains dropped samples or diagnostics");
    passed = false;
  }
  return passed ? 0 : 1;
}
