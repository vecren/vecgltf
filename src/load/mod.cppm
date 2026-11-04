export module vecgltf.load;
export import vecgltf.asset;
export import vecgltf.trace;
export import vecgltf.load.resources;
import vecgltf.load.accessor;
import :external;

export namespace vecgltf
{
auto LoadAsset(ref<str> path, LoadOptions options = {}) -> Result<Arc<Asset>, Error>;
auto LoadAsset(LoadInput input) -> Result<Arc<Asset>, Error>;
auto BuildAsset(ResolvedDocument input) -> Result<Arc<Asset>, Error>;
struct HdrImage {
    unsigned   width {}, height {};
    Vec<float> rgba;
};
auto LoadEnvironment(ref<str> path) -> Result<HdrImage, Error>;
auto LoadEnvironment(ResourceBytes bytes, ref<str> source = {}) -> Result<HdrImage, Error>;
} // namespace vecgltf
