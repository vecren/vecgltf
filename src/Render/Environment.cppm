export module vecgltf.environment;
import vecgltf.load;
import vrento;

export namespace vecgltf {
namespace environment_detail {
constexpr float pi = 3.14159265359f;
float radical_inverse(unsigned value) {
  float result{}, fraction = 0.5f;
  while (value) {
    result += float(value & 1) * fraction;
    value >>= 1;
    fraction *= 0.5f;
  }
  return result;
}
Vec3 direction(float u, float v) {
  float theta = v * pi, phi = (u - 0.5f) * 2 * pi;
  return {sine(theta) * cosine(phi), cosine(theta), sine(theta) * sine(phi)};
}
Vec3 sample(const HdrImage &image, Vec3 d) {
  float u =
      rstd::f32(d.z).atan2(rstd::f32(d.x)).to_primitive() / (2 * pi) + 0.5f;
  float v = rstd::f32(clamp(d.y, -1, 1)).acos().to_primitive() / pi;
  float x = u * image.width - 0.5f, y = v * image.height - 0.5f;
  int ix = int(rstd::f32(x).floor().to_primitive()),
      iy = int(rstd::f32(y).floor().to_primitive());
  float tx = x - ix, ty = y - iy;
  Vec3 result{};
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      int sx =
          ((ix + dx) % int(image.width) + int(image.width)) % int(image.width);
      int sy = int(clamp(float(iy + dy), 0, float(image.height - 1)));
      auto i = (rstd::size_t(sy) * image.width + sx) * 4;
      Vec3 color{at(image.rgba, i), at(image.rgba, i + 1),
                 at(image.rgba, i + 2)};
      result = result + color * ((dx ? tx : 1 - tx) * (dy ? ty : 1 - ty));
    }
  return result;
}
Vec3 basis(Vec3 n, Vec3 h) {
  Vec3 up = absolute(n.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
  auto tangent = normalized(cross(up, n));
  return normalized(tangent * h.x + cross(n, tangent) * h.y + n * h.z);
}
Vec3 ggx(unsigned sample, unsigned count, float roughness) {
  float phi = 2 * pi * float(sample) / count, y = radical_inverse(sample),
        a = roughness * roughness;
  float z = square_root((1 - y) / (1 + (a * a - 1) * y)),
        r = square_root(maximum(0, 1 - z * z));
  return {r * cosine(phi), r * sine(phi), z};
}
void append(HdrImage &image, Vec3 color) {
  image.rgba.push(float(color.x));
  image.rgba.push(float(color.y));
  image.rgba.push(float(color.z));
  image.rgba.push(1.0f);
}
} // namespace environment_detail
struct EnvironmentImages {
  HdrImage background, diffuse, brdf;
  Vec<HdrImage> specular;
};
auto default_environment() -> HdrImage {
  HdrImage image{256, 128};
  for (unsigned y = 0; y < image.height; ++y)
    for (unsigned x = 0; x < image.width; ++x) {
      auto d = environment_detail::direction((x + 0.5f) / image.width,
                                             (y + 0.5f) / image.height);
      float sky = clamp(d.y * 0.5f + 0.5f, 0, 1);
      Vec3 color =
          Vec3{0.12f, 0.10f, 0.08f} * (1 - sky) + Vec3{0.7f, 0.85f, 1.1f} * sky;
      float key = maximum(dot(d, normalized(Vec3{-0.4f, 0.7f, 0.5f})), 0);
      key = rstd::f32(key).powi(i32(80)).to_primitive();
      color = color + Vec3{7, 6, 5} * key;
      environment_detail::append(image, color);
    }
  return image;
}
auto preprocess_environment(HdrImage source) -> EnvironmentImages {
  using namespace environment_detail;
  EnvironmentImages result;
  result.background = rstd::move(source);
  constexpr unsigned samples = 128;
  result.diffuse.width = 32;
  result.diffuse.height = 16;
  for (unsigned y = 0; y < 16; ++y)
    for (unsigned x = 0; x < 32; ++x) {
      auto n = direction((x + 0.5f) / 32, (y + 0.5f) / 16);
      Vec3 color{};
      for (unsigned i = 0; i < samples; ++i) {
        float phi = 2 * pi * float(i) / samples,
              r = square_root(radical_inverse(i));
        auto l =
            basis(n, {r * cosine(phi), r * sine(phi), square_root(1 - r * r)});
        color = color + sample(result.background, l);
      }
      append(result.diffuse, color / float(samples));
    }
  for (unsigned level = 0; level < 8; ++level) {
    unsigned width = 128 >> level, height = 64 >> level;
    if (!height)
      height = 1;
    float roughness = float(level) / 7;
    HdrImage mip{width, height};
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        auto n = direction((x + 0.5f) / width, (y + 0.5f) / height);
        Vec3 color{};
        float weight{};
        if (level == 0)
          color = sample(result.background, n);
        else
          for (unsigned i = 0; i < samples; ++i) {
            auto h = basis(n, ggx(i, samples, roughness));
            auto l = h * (2 * dot(n, h)) - n;
            float nl = maximum(dot(n, l), 0);
            if (nl > 0) {
              color = color + sample(result.background, l) * nl;
              weight += nl;
            }
          }
        append(mip, level == 0 ? color : color / maximum(weight, 0.001f));
      }
    result.specular.push(rstd::move(mip));
  }
  result.brdf.width = result.brdf.height = 64;
  for (unsigned y = 0; y < 64; ++y)
    for (unsigned x = 0; x < 64; ++x) {
      float nv = (x + 0.5f) / 64, roughness = (y + 0.5f) / 64,
            a = roughness * roughness, a2 = a * a;
      Vec3 v{square_root(1 - nv * nv), 0, nv};
      float scale{}, bias{};
      for (unsigned i = 0; i < samples; ++i) {
        auto h = ggx(i, samples, roughness);
        float vh = maximum(dot(v, h), 0);
        auto l = h * (2 * vh) - v;
        float nl = maximum(l.z, 0);
        if (nl > 0) {
          float vis =
              0.5f / maximum(nl * square_root(nv * nv * (1 - a2) + a2) +
                                 nv * square_root(nl * nl * (1 - a2) + a2),
                             0.0001f);
          float g = 4 * nl * vis * vh / maximum(h.z, 0.0001f);
          float fc = rstd::f32(1 - vh).powi(i32(5)).to_primitive();
          scale += (1 - fc) * g;
          bias += fc * g;
        }
      }
      append(result.brdf, {scale / samples, bias / samples, 0});
    }
  return result;
}
auto upload_environment_image(
    mut_ref<dyn<vrento::texture::TextureDevice>> device, Vec<HdrImage> mips,
    bool wrap = true)
    -> Result<vrento::texture::PreparedImage, vrento::texture::Error> {
  if (mips.is_empty())
    return Err(vrento::texture::Error{vrento::texture::ErrorKind::Invalid});
  vrento::Image image;
  image.header.format = vrento::TextureFormat::RGBA32F;
  image.header.sample.minFilter = image.header.sample.magFilter =
      vrento::TextureFilter::LINEAR;
  image.header.sample.wrapS =
      wrap ? vrento::TextureWrap::REPEAT : vrento::TextureWrap::CLAMP_TO_EDGE;
  image.header.sample.wrapT = vrento::TextureWrap::CLAMP_TO_EDGE;
  vrento::Image::Slot slot;
  slot.width = int(at(mips, 0).width);
  slot.height = int(at(mips, 0).height);
  for (auto &mip : mips) {
    vrento::ImageData data;
    data.width = int(mip.width);
    data.height = int(mip.height);
    data.size = isize(count(mip.rgba) * sizeof(float));
    auto pointer = reinterpret_cast<rstd::uint8_t *>(mip.rgba.data());
    data.data = vrento::ImageDataPtr(
        pointer, [owner = rstd::move(mip.rgba)](rstd::uint8_t *) {});
    slot.mipmaps.push(rstd::move(data));
  }
  image.slots.push(rstd::move(slot));
  vrento::texture::ImagePrepareContext context(device);
  auto prepared =
      rstd::as<vrento::texture::TextureContentPreparer>(context).PrepareImage(
          image);
  if (prepared.is_err())
    return Err(prepared.unwrap_err());
  auto result = prepared.unwrap();
  return Ok(rstd::move(result[usize()]));
}
} // namespace vecgltf
