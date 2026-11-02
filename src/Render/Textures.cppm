export module vecgltf.textures;
import vecgltf.asset;
import vecgltf.trace;
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
struct PreparedTextureImage {
  vrento::texture::PreparedImage image;
  Option<vrento::vulkan::TextureMipmapProgram> mipmaps;
};

auto prepare_cpu_texture(mut_ref<dyn<vrento::texture::TextureDevice>> device,
                         const Image &source, bool srgb,
                         const LoadTrace &trace)
    -> Result<vrento::texture::PreparedImage, vrento::texture::Error> {
  auto image = Arc<vrento::Image>::make();
  image->header.format =
      srgb ? vrento::TextureFormat::RGBA8_SRGB : vrento::TextureFormat::RGBA8;
  image->header.sample = texture_sample({});
  vrento::Image::Slot slot;
  slot.width = int(source.width);
  slot.height = int(source.height);
  Vec<u8> pixels;
  {
    LoadScope copy(trace, LoadEvent::TextureCopy);
    pixels = source.rgba.clone();
  }
  unsigned width = source.width, height = source.height;
  LoadScope mipmaps(trace, LoadEvent::Mipmaps);
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
  mipmaps.finish();
  image->slots.push(rstd::move(slot));
  LoadScope prepare(trace, LoadEvent::ImagePrepare);
  vrento::texture::ImagePrepareContext context(device);
  auto prepared =
      rstd::as<vrento::texture::TextureContentPreparer>(context).PrepareImage(
          rstd::move(image));
  if (prepared.is_err())
    return Err(prepared.unwrap_err());
  auto result = prepared.unwrap();
  return Ok(rstd::move(result[usize()]));
}

auto prepare_gpu_texture(const vrento::vulkan::DeviceSession &session,
                         mut_ref<dyn<vrento::texture::TextureDevice>> device,
                         const Image &source, bool srgb,
                         const LoadTrace &trace)
    -> Result<PreparedTextureImage, vrento::texture::Error> {
  namespace tex = vrento::texture;
  tex::TextureDesc description;
  description.extent = {source.width, source.height};
  description.format =
      srgb ? vrento::TextureFormat::RGBA8_SRGB : vrento::TextureFormat::RGBA8;
  description.usage = tex::Usage::Sampled | tex::Usage::TransferSource |
                      tex::Usage::TransferDestination;
  for (unsigned size = source.width > source.height ? source.width : source.height;
       size > 1; size >>= 1)
    ++description.mip_levels;
  LoadScope prepare(trace, LoadEvent::ImagePrepare);
  auto texture = device->CreateTexture(description);
  if (texture.is_err())
    return Err(texture.unwrap_err());
  auto created_view = device->CreateView(texture.unwrap(), {});
  if (created_view.is_err())
    return Err(created_view.unwrap_err());
  auto view = created_view.unwrap();
  auto program = vrento::vulkan::TextureMipmapProgram::prepare(session, view);
  if (program.is_err())
    return Err(program.unwrap_err());
  auto sampler = device->CreateSampler({texture_sample({})});
  if (sampler.is_err())
    return Err(sampler.unwrap_err());
  prepare.finish();

  auto image = Arc<vrento::Image>::make();
  image->header.format = description.format;
  image->header.sample = texture_sample({});
  vrento::Image::Slot slot;
  slot.width = int(source.width);
  slot.height = int(source.height);
  Vec<u8> pixels;
  {
    LoadScope copy(trace, LoadEvent::TextureCopy);
    pixels = source.rgba.clone();
  }
  vrento::ImageData base;
  base.width = slot.width;
  base.height = slot.height;
  base.size = isize(pixels.len().to_primitive());
  auto pointer = reinterpret_cast<rstd::uint8_t *>(pixels.data());
  base.data = vrento::ImageDataPtr(
      pointer, [owner = rstd::move(pixels)](rstd::uint8_t *) {});
  slot.mipmaps.push(rstd::move(base));
  image->slots.push(rstd::move(slot));
  tex::TextureUpload upload;
  upload.target = view;
  upload.width = source.width;
  upload.height = source.height;
  upload.bytes = tex::TextureUploadPayload::FromImage(rstd::move(image), usize(), usize()).unwrap();
  tex::PreparedImage prepared{view, sampler.unwrap()};
  prepared.uploads.push(rstd::move(upload));
  return Ok(PreparedTextureImage{rstd::move(prepared), Some(program.unwrap())});
}

auto prepare_texture(const vrento::vulkan::DeviceSession &session,
                     mut_ref<dyn<vrento::texture::TextureDevice>> device,
                     const Image &source, bool srgb,
                     const LoadTrace &trace = {})
    -> Result<PreparedTextureImage, vrento::texture::Error> {
  LoadScope scope(trace, LoadEvent::Texture);
  if (!source.width || !source.height ||
      rstd::size_t(source.width) > ~rstd::size_t(0) / source.height / 4 ||
      source.rgba.len() != usize(rstd::size_t(source.width) * source.height * 4))
    return Err(vrento::texture::Error{vrento::texture::ErrorKind::Invalid});
  auto power_of_two = [](unsigned value) { return (value & (value - 1)) == 0; };
  if ((source.width > 1 || source.height > 1) && power_of_two(source.width) &&
      power_of_two(source.height)) {
    auto prepared = prepare_gpu_texture(session, device, source, srgb, trace);
    if (prepared.is_ok())
      return prepared;
    auto error = prepared.unwrap_err();
    if (error.kind != vrento::texture::ErrorKind::Unsupported)
      return Err(rstd::move(error));
  }
  auto prepared = prepare_cpu_texture(device, source, srgb, trace);
  if (prepared.is_err())
    return Err(prepared.unwrap_err());
  return Ok(PreparedTextureImage{prepared.unwrap(), None()});
}

auto record_texture(vrento::texture::Submission &work,
                    const vrento::texture::PreparedImage &source,
                    const Option<vrento::vulkan::TextureMipmapProgram> &mipmaps = {},
                    const LoadTrace &trace = {})
    -> Result<empty, vrento::texture::Error> {
  if (!source.view.texture().ready().valid()) {
    for (const auto &upload : source.uploads) {
      auto queued = work.commands()->Upload(upload.clone());
      if (queued.is_err())
        return Err(queued.unwrap_err());
    }
    if (mipmaps) {
      LoadScope scope(trace, LoadEvent::GpuMipmaps);
      auto recorded = mipmaps->record(work);
      if (recorded.is_err())
        return recorded;
    }
  }
  return work.commands()->UseSampledView(
      source.view, source.sampler, vrento::texture::Stages::Fragment);
}
} // namespace vecgltf
