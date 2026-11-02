module;
#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
#if ! defined(_WIN32)
#include <unistd.h>
#endif

module vecgltf.tests:resources;
import vecgltf;
import rstd;
import rstd.test;

using namespace vecgltf;
using namespace rstd::literals;

namespace vecgltf_test::resources
{
auto c_string(ref<str> value) -> rstd::ffi::CString {
    return rstd::ffi::CString::make(Vec<u8>::from(value.as_bytes())).unwrap();
}
auto join(ref<rstd::path::Path> base, ref<str> suffix) -> rstd::path::PathBuf {
    auto path  = rstd::path::PathBuf::from(base);
    auto child = rstd::path::PathBuf::from(suffix);
    return path.join(child.as_path());
}
void write(ref<rstd::path::Path> path, ref<str> text) {
    rstd::fs::write(path, text.as_bytes()).unwrap();
}
auto document(ref<str> buffer, ref<str> image) -> String {
    auto result = String::make(
        "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":4,\"uri\":\""_str);
    result.push_str(buffer);
    result.push_str("\"}],\"images\":[{\"uri\":\""_str);
    result.push_str(image);
    result.push_str("\"}],\"textures\":[{\"source\":0}]}"_str);
    return result;
}
struct Files {
    rstd::fs::TempDir   directory { rstd::fs::TempDir::make("vecgltf-resources"_str).unwrap() };
    rstd::path::PathBuf root, model, shared, outside;
    Files()
        : root(join(directory.path(), "root"_str)),
          model(join(root.as_path(), "models/model.gltf"_str)),
          shared(join(root.as_path(), "shared"_str)),
          outside(join(directory.path(), "outside.bin"_str)) {
        rstd::fs::create_dir_all(model.as_path().parent().unwrap()).unwrap();
        rstd::fs::create_dir_all(shared.as_path()).unwrap();
        write(join(shared.as_path(), "buffer.bin"_str).as_path(), "data"_str);
        auto png   = rstd::path::PathBuf::from("data/white pixel.png"_str);
        auto bytes = rstd::fs::read(png.as_path()).unwrap();
        rstd::fs::write(join(shared.as_path(), "image.png"_str).as_path(), bytes.as_slice())
            .unwrap();
        write(outside.as_path(), "data"_str);
    }
    auto load(ref<str> buffer = "../shared/buffer.bin"_str,
              ref<str> image  = "../shared/image.png"_str) -> Result<Arc<Asset>, Error> {
        auto json = document(buffer, image);
        write(model.as_path(), json.as_str());
        auto path = model.as_path().to_string_lossy();
        return LoadAsset(path.as_str(), { .resource_root = Some(root.as_path()) });
    }
};

TEST(Asset, ResourceRootRelativeAndEmbedded) {
    Files files;
    auto  loaded = files.load();
    rstd_assert(loaded.is_ok());
    rstd_assert(loaded.unwrap()->images.len() == usize(1));
    rstd_assert(files.load("../shared/buffer%2ebin"_str, "../shared/image%2epng"_str).is_ok());
    auto inside = join(files.shared.as_path(), "same.gltf"_str);
    auto json   = document("buffer.bin"_str, "image.png"_str);
    write(inside.as_path(), json.as_str());
    auto path = inside.as_path().to_string_lossy();
    rstd_assert(LoadAsset(path.as_str(), { .resource_root = Some(files.root.as_path()) }).is_ok());
    auto data = rstd::path::PathBuf::from("data"_str);
    auto root = rstd::fs::canonicalize(data.as_path()).unwrap();
    auto gltf = LoadAsset("data/triangle.gltf"_str, { .resource_root = Some(root.as_path()) });
    auto glb  = LoadAsset("data/triangle.glb"_str, { .resource_root = Some(root.as_path()) });
    rstd_assert(gltf.is_ok() && glb.is_ok());
}

TEST(Asset, ResourceRootRejectsUriEscapes) {
    Files    files;
    ref<str> paths[] { "../../outside.bin"_str,
                       "%2e%2e/%2e%2e/outside.bin"_str,
                       "/tmp/outside.bin"_str,
                       "%2ftmp/outside.bin"_str,
                       "file:///tmp/outside.bin"_str,
                       "https://example.com/model.bin"_str,
                       "https%3a%2f%2fexample.com/model.bin"_str,
                       "../shared/buffer.bin%00ignored"_str,
                       "../shared/buffer.bin\\u0000ignored"_str,
                       "C:/outside.bin"_str,
                       "../shared/bad%"_str };
    for (auto uri : paths) {
        rstd_assert(files.load(uri).is_err());
        rstd_assert(files.load("../shared/buffer.bin"_str, uri).is_err());
    }
    auto root     = files.root.as_path().to_string_lossy();
    auto absolute = rstd::format("{}/shared/buffer.bin", root);
    rstd_assert(files.load(absolute.as_str()).is_err());
    auto image = rstd::format("{}/shared/image.png", root);
    rstd_assert(files.load("../shared/buffer.bin"_str, image.as_str()).is_err());
    auto path = files.model.as_path().to_string_lossy();
    auto json = document("../shared/buffer.bin"_str, "../shared/image.png"_str);
    write(files.model.as_path(), json.as_str());
    rstd_assert(
        LoadAsset(path.as_str(), { .resource_root = Some(files.shared.as_path()) }).is_err());
}

TEST(Asset, ResourceRootRejectsSymlinkEscapes) {
#if ! defined(_WIN32)
    Files files;
    auto  link   = join(files.shared.as_path(), "link.bin"_str);
    auto  source = files.outside.as_path().to_string_lossy();
    auto  target = link.as_path().to_string_lossy();
    auto  a = c_string(source.as_str()), b = c_string(target.as_str());
    rstd_assert(::symlink(a.as_ptr(), b.as_ptr()) == 0);
    rstd_assert(files.load("../shared/link.bin"_str).is_err());
    auto outside_image = join(files.directory.path(), "outside.png"_str);
    auto image         = join(files.shared.as_path(), "image.png"_str);
    auto pixels        = rstd::fs::read(image.as_path()).unwrap();
    rstd::fs::write(outside_image.as_path(), pixels.as_slice()).unwrap();
    auto image_link = join(files.shared.as_path(), "link.png"_str);
    source          = outside_image.as_path().to_string_lossy();
    target          = image_link.as_path().to_string_lossy();
    a               = c_string(source.as_str());
    b               = c_string(target.as_str());
    rstd_assert(::symlink(a.as_ptr(), b.as_ptr()) == 0);
    rstd_assert(files.load("../shared/buffer.bin"_str, "../shared/link.png"_str).is_err());
    auto outside_model = join(files.directory.path(), "outside.gltf"_str);
    auto json          = document("root/shared/buffer.bin"_str, "root/shared/image.png"_str);
    write(outside_model.as_path(), json.as_str());
    auto model_link = join(files.root.as_path(), "linked.gltf"_str);
    source          = outside_model.as_path().to_string_lossy();
    rstd_assert(LoadAsset(source.as_str()).is_ok());
    target = model_link.as_path().to_string_lossy();
    a      = c_string(source.as_str());
    b      = c_string(target.as_str());
    rstd_assert(::symlink(a.as_ptr(), b.as_ptr()) == 0);
    rstd_assert(
        LoadAsset(target.as_str(), { .resource_root = Some(files.root.as_path()) }).is_err());
#endif
}

TEST(Asset, ResourceRootDisabledKeepsLocalUris) {
    Files files;
    auto  json = document("../../outside.bin"_str, "../shared/image.png"_str);
    write(files.model.as_path(), json.as_str());
    auto path = files.model.as_path().to_string_lossy();
    rstd_assert(LoadAsset(path.as_str()).is_ok());
    rstd_assert(LoadAsset(path.as_str(), { .resource_root = Some(files.root.as_path()) }).is_err());
}
} // namespace vecgltf_test::resources
