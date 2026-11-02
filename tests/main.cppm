export module vecgltf.tests;
export import :output;
import :asset;
import :scene;
import :render;
import :mipmaps;
import :controls;
import :camera;
import :window;
import rstd.test;

int main() {
    return rstd::test::run_registered().to_primitive();
}
