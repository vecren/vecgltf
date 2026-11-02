export module vecgltf.tests:output;
import rstd;
import vecgltf.render;
import vrento;

using namespace rstd::prelude;

export namespace vecgltf_test
{
struct Output {
    vrento::vulkan::TextureContext       queue;
    Option<vrento::texture::TexturePool> pool;
    VkExtent2D                           extent {};
    vrento::texture::SubmissionToken     last;
    explicit Output(vrento::vulkan::DeviceSession session)
        : queue(vrento::vulkan::TextureContext::create(rstd::move(session)).unwrap()) {}
    void resize(VkExtent2D size) {
        if (size.width == extent.width && size.height == extent.height) return;
        vrento::texture::TextureDesc desc;
        desc.extent = { size.width, size.height };
        desc.usage  = vrento::texture::Usage::Sampled | vrento::texture::Usage::ColorAttachment;
        auto device = dyn<vrento::texture::TextureDevice>::from_ref(queue);
        pool   = Some(vrento::texture::TexturePool::create(device.as_mut_ref(), desc).unwrap());
        extent = size;
    }
    auto render(vecgltf::Renderer&            renderer,
                const vecgltf::PreparedAsset& prepared,
                const vecgltf::SceneInstance& instance,
                const vecgltf::View&          camera,
                vecgltf::RenderOptions        options = {})
        -> Result<vrento::texture::TextureViewRef, vecgltf::Error> {
        auto work     = queue.Begin().unwrap();
        auto write    = pool->acquire().unwrap();
        auto recorded = renderer.record(work, write.view(), prepared, instance, camera, options);
        if (recorded.is_err()) return Err(recorded.unwrap_err());
        last = queue.Submit(rstd::move(work)).unwrap();
        return Ok(pool->publish(rstd::move(write), last).unwrap());
    }
    void drain() { queue.wait(last).unwrap(); }
};
} // namespace vecgltf_test
