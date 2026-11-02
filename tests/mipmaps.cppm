module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

module vecgltf.tests:mipmaps;
import vecgltf;
import vecgltf.textures;
import vrento;
import rstd.test;

using namespace vecgltf;
namespace tex = vrento::texture;
namespace vk = vrento::vulkan;

namespace vecgltf_test::mipmaps {
struct ReadbackDevice {
  vk::TextureContext &context;
  Option<tex::ErrorKind> reject;
  mutable unsigned rejected{};
  unsigned cpu_allocations{};

  auto check(const tex::TextureDesc &desc) const -> Result<empty, tex::Error> {
    if (reject && tex::has(desc.usage, tex::Usage::TransferSource)) {
      ++rejected;
      return Err(tex::Error{*reject});
    }
    auto readable = desc;
    readable.usage = readable.usage | tex::Usage::TransferSource;
    return context.QuerySupport(readable);
  }
};
} // namespace vecgltf_test::mipmaps

namespace rstd {
template <>
struct Impl<tex::TextureDevice, vecgltf_test::mipmaps::ReadbackDevice>
    : ImplBase<vecgltf_test::mipmaps::ReadbackDevice> {
  auto QuerySupport(const tex::TextureDesc &desc) const
      -> Result<empty, tex::Error> {
    return self().check(desc);
  }
  auto CreateTexture(const tex::TextureDesc &desc)
      -> Result<tex::TextureRef, tex::Error> {
    auto supported = self().check(desc);
    if (supported.is_err())
      return Err(supported.unwrap_err());
    if (!tex::has(desc.usage, tex::Usage::TransferSource))
      ++self().cpu_allocations;
    auto readable = desc;
    readable.usage = readable.usage | tex::Usage::TransferSource;
    return self().context.CreateTexture(readable);
  }
  auto CreateView(const tex::TextureRef &texture, tex::ViewDesc desc)
      -> Result<tex::TextureViewRef, tex::Error> {
    return self().context.CreateView(texture, desc);
  }
  auto CreateSampler(const tex::SamplerDesc &desc)
      -> Result<tex::SamplerRef, tex::Error> {
    return self().context.CreateSampler(desc);
  }
};
} // namespace rstd

namespace vecgltf_test::mipmaps {
auto read(vk::TextureContext &context, tex::TextureViewRef image, unsigned mip)
    -> Vec<u8> {
  auto session = context.session();
  auto extent = tex::mip_extent(image.texture().description(), mip);
  auto bytes = VkDeviceSize(extent.width) * extent.height * 4;
  auto buffer = vk::GpuBuffer::create(session, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      vvk::MemoryRequest::Readback())
                    .unwrap();
  auto work = context.Begin().unwrap();
  work.commands()
      ->UseTexture({image, tex::Access::TransferSource, tex::Stages::Transfer,
                    false, Some(tex::Range{tex::Aspect::Color, mip, 1, 0, 1}),
                    true})
      .unwrap();
  buffer.use(work, {0, bytes, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, true, false})
      .unwrap();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
  region.imageExtent = {extent.width, extent.height, 1};
  session.device().handle().Dispatch().vkCmdCopyImageToBuffer(
      vk::native_recording(work, session).unwrap(),
      vk::native_texture(image.texture()).unwrap(),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.native(), 1, &region);
  buffer.use(work, {0, bytes, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT})
      .unwrap();
  context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
  return buffer.readback().unwrap();
}

auto checker() -> Image {
  Image image;
  image.width = image.height = 4;
  unsigned alpha[]{0, 64, 128, 255};
  for (unsigned y = 0; y < 4; ++y)
    for (unsigned x = 0; x < 4; ++x) {
      for (unsigned c = 0; c < 3; ++c)
        image.rgba.push(u8((x + y) % 2 ? 255 : 0));
      image.rgba.push(u8(alpha[(y % 2) * 2 + x % 2]));
    }
  return image;
}

void near(unsigned value, unsigned expected) {
  rstd_assert(value + 1 >= expected && value <= expected + 1);
}

void check_chain(vk::TextureContext &context, const PreparedTextureImage &prepared,
                 const Image &source, bool srgb) {
  rstd_assert(read(context, prepared.image.view, 0) == source.rgba);
  for (unsigned mip = 1; mip < 3; ++mip) {
    auto pixels = read(context, prepared.image.view, mip);
    for (usize i{}; i < pixels.len(); ++i) {
      auto expected = i.to_primitive() % 4 == 3 ? 112u : srgb ? 188u : 128u;
      near(u8(pixels[i]).to_primitive(), expected);
    }
  }
}

TEST(Render, TextureMipmaps) {
  auto session = vk::DeviceSession::create({.validation = true}).unwrap();
  auto context = vk::TextureContext::create(session).unwrap();
  ReadbackDevice device{context};
  auto factory = dyn<tex::TextureDevice>::from_ref(device);
  auto source = checker();
  for (bool srgb : rstd::array<bool, 2>{false, true}) {
    auto prepared = prepare_texture(session, factory.as_mut_ref(), source, srgb).unwrap();
    rstd_assert(prepared.mipmaps && prepared.image.uploads.len() == usize(1));
    auto texture = prepared.image.view.texture();
    {
      auto cancelled = context.Begin().unwrap();
      record_texture(cancelled, prepared.image, prepared.mipmaps).unwrap();
      rstd_assert(!texture.ready().valid());
      cancelled.discard();
    }
    rstd_assert(!texture.ready().valid() && texture.content_revision() == 0);
    auto work = context.Begin().unwrap();
    record_texture(work, prepared.image, prepared.mipmaps).unwrap();
    context.wait(context.Submit(rstd::move(work)).unwrap()).unwrap();
    rstd_assert(texture.ready().valid() && texture.content_revision() == 1);
    check_chain(context, prepared, source, srgb);
    auto sampled = context.Begin().unwrap();
    record_texture(sampled, prepared.image, prepared.mipmaps).unwrap();
    context.wait(context.Submit(rstd::move(sampled)).unwrap()).unwrap();
    rstd_assert(texture.content_revision() == 1);
  }

  Image odd;
  odd.width = odd.height = 3;
  for (unsigned y = 0; y < 3; ++y)
    for (unsigned x = 0; x < 3; ++x) {
      for (unsigned c = 0; c < 3; ++c)
        odd.rgba.push(u8(x == 2 || y == 2 || (x == 0 && y == 0) ? 255 : 0));
      odd.rgba.push(u8(x == 2 || y == 2 ? 255 : 64));
    }
  auto odd_prepared = prepare_texture(session, factory.as_mut_ref(), odd, false).unwrap();
  rstd_assert(!odd_prepared.mipmaps && odd_prepared.image.uploads.len() == usize(2));
  auto odd_work = context.Begin().unwrap();
  record_texture(odd_work, odd_prepared.image, odd_prepared.mipmaps).unwrap();
  context.wait(context.Submit(rstd::move(odd_work)).unwrap()).unwrap();
  auto odd_mip = read(context, odd_prepared.image.view, 1);
  for (auto channel : odd_mip)
    near(u8(channel).to_primitive(), 64);

  device.reject = Some(tex::ErrorKind::Unsupported);
  auto allocated = device.cpu_allocations;
  auto fallback = prepare_texture(session, factory.as_mut_ref(), source, true).unwrap();
  rstd_assert(device.rejected && device.cpu_allocations == allocated + 1 &&
              !fallback.mipmaps && fallback.image.uploads.len() == usize(3));
  auto fallback_work = context.Begin().unwrap();
  record_texture(fallback_work, fallback.image, fallback.mipmaps).unwrap();
  context.wait(context.Submit(rstd::move(fallback_work)).unwrap()).unwrap();
  check_chain(context, fallback, source, true);

  device.reject = Some(tex::ErrorKind::Backend);
  allocated = device.cpu_allocations;
  auto failed = prepare_texture(session, factory.as_mut_ref(), source, false);
  rstd_assert(failed.is_err() && failed.unwrap_err().kind == tex::ErrorKind::Backend &&
              device.cpu_allocations == allocated);
  rstd_assert(session.validation_errors() == 0);
}
} // namespace vecgltf_test::mipmaps
