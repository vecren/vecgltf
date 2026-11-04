export module vecgltf.viewer.model;
import rstd;
import vecgltf;
import vecgltf.render;
export namespace viewer
{
using namespace rstd::prelude;
auto reload(vecgltf::Renderer&               renderer,
            ref<str>                         path,
            rstd::sync::Arc<vecgltf::Asset>& asset,
            vecgltf::PreparedAsset&          prepared,
            vecgltf::SceneInstance&          instance,
            vecgltf::PrepareOptions          options = {}) -> Result<empty, vecgltf::Error> {
    auto loaded = vecgltf::LoadAsset(path);
    if (loaded.is_err()) return Err(loaded.unwrap_err());
    auto next = loaded.unwrap();
    auto gpu  = renderer.prepare(next.clone(), options);
    if (gpu.is_err()) return Err(gpu.unwrap_err());
    auto scene = vecgltf::SceneInstance(next.clone());
    prepared   = gpu.unwrap();
    asset      = rstd::move(next);
    instance   = rstd::move(scene);
    return Ok(empty {});
}
} // namespace viewer
