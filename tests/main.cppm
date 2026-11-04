export module vecgltf.tests;
export import :output;
import :asset;
import :document;
import :accessor;
import :load_input;
import :attributes;
import :emissive;
import :materials;
import :resources;
import :scene;
import :render;
import :lighting;
import :point_lights;
import :display;
import :color_balance;
import :coverage;
import :shadows;
import :material_overrides;
import :shadow_catcher;
import :postprocess;
import :mipmaps;
import :controls;
import :camera;
import :window;
import rstd.test;

int main() {
    return rstd::test::run_registered().to_primitive();
}
