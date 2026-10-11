export module vecgltf.load;
export import vecgltf.asset;

export namespace vecgltf {
struct LoadOptions {
  usize max_vertices{10000000}, max_image_pixels{67108864};
};
auto LoadAsset(ref<str> path, LoadOptions options = {})
    -> Result<Arc<Asset>, Error>;
struct HdrImage {
  unsigned width{}, height{};
  Vec<float> rgba;
};
auto LoadEnvironment(ref<str> path) -> Result<HdrImage, Error>;
} // namespace vecgltf
