export module vecgltf.textures;
import vecgltf.asset;
import vrento;

export namespace vecgltf {
inline float srgb_to_linear(float value) {
  return value <= 0.04045f ? value / 12.92f
                           : rstd::f32((value + 0.055f) / 1.055f)
                                 .powf(rstd::f32(2.4f))
                                 .to_primitive();
}
inline float linear_to_srgb(float value) {
  return value <= 0.0031308f ? value * 12.92f
                             : 1.055f * rstd::f32(value)
                                            .powf(rstd::f32(1 / 2.4f))
                                            .to_primitive() -
                                   0.055f;
}
auto texture_sample(const Sampler &sampler) -> vrento::TextureSample {
  auto wrap = [](Wrap value) {
    return value == Wrap::Clamp    ? vrento::TextureWrap::CLAMP_TO_EDGE
           : value == Wrap::Mirror ? vrento::TextureWrap::MIRRORED_REPEAT
                                   : vrento::TextureWrap::REPEAT;
  };
  vrento::TextureSample result;
  result.wrapS = wrap(sampler.s);
  result.wrapT = wrap(sampler.t);
  result.minFilter = sampler.min == Filter::Linear
                         ? vrento::TextureFilter::LINEAR
                         : vrento::TextureFilter::NEAREST;
  result.magFilter = sampler.mag == Filter::Linear
                         ? vrento::TextureFilter::LINEAR
                         : vrento::TextureFilter::NEAREST;
  result.mipFilter =
      sampler.mip == MipFilter::None      ? vrento::TextureMipmapFilter::None
      : sampler.mip == MipFilter::Nearest ? vrento::TextureMipmapFilter::Nearest
                                          : vrento::TextureMipmapFilter::Linear;
  return result;
}
auto prepare_texture(mut_ref<dyn<vrento::texture::TextureDevice>> device,
                     const Image &source, bool srgb)
    -> Result<vrento::texture::PreparedImage, vrento::texture::Error> {
  vrento::Image image;
  image.header.format =
      srgb ? vrento::TextureFormat::RGBA8_SRGB : vrento::TextureFormat::RGBA8;
  image.header.sample = texture_sample({});
  vrento::Image::Slot slot;
  slot.width = int(source.width);
  slot.height = int(source.height);
  auto pixels = source.rgba.clone();
  unsigned width = source.width, height = source.height;
  while (true) {
    Vec<u8> next;
    unsigned next_width = width > 1 ? width / 2 : 1,
             next_height = height > 1 ? height / 2 : 1;
    bool finished = width == 1 && height == 1;
    if (!finished) {
      next.reserve(usize(next_width) * usize(next_height) * usize(4));
      for (unsigned y = 0; y < next_height; ++y)
        for (unsigned x = 0; x < next_width; ++x)
          for (unsigned c = 0; c < 4; ++c) {
            float sum{};
            for (unsigned dy = 0; dy < 2; ++dy)
              for (unsigned dx = 0; dx < 2; ++dx) {
                unsigned sx = (2 * x + dx) < width ? 2 * x + dx : width - 1,
                         sy = (2 * y + dy) < height ? 2 * y + dy : height - 1;
                float value =
                    u8(pixels[usize((rstd::size_t(sy) * width + sx) * 4 + c)])
                        .to_primitive() /
                    255.0f;
                sum += srgb && c < 3 ? srgb_to_linear(value) : value;
              }
            float value =
                srgb && c < 3 ? linear_to_srgb(sum * 0.25f) : sum * 0.25f;
            next.push(u8(unsigned(clamp(value, 0, 1) * 255 + 0.5f)));
          }
    }
    auto pointer = reinterpret_cast<rstd::uint8_t *>(pixels.data());
    auto size = pixels.len();
    vrento::ImageData mip;
    mip.width = int(width);
    mip.height = int(height);
    mip.size = isize(size.to_primitive());
    mip.data = vrento::ImageDataPtr(
        pointer, [owner = rstd::move(pixels)](rstd::uint8_t *) {});
    slot.mipmaps.push(rstd::move(mip));
    if (finished)
      break;
    pixels = rstd::move(next);
    width = next_width;
    height = next_height;
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
