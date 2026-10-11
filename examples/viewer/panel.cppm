export module vecgltf.viewer.panel;
import vecgltf;
import vecgltf.render;
import vui.controls;

export namespace viewer {
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vui;
namespace gltf = vecgltf;
enum class Action {
  Scene,
  Camera,
  Animation,
  Play,
  Loop,
  Seek,
  Fit,
  Reload,
  Background,
  ExposureDown,
  ExposureUp,
  Debug
};
struct Command {
  Action action;
  float value{};
};
class Panel {
  UiSession &session_;
  UiId content_, scroll_, name_, details_, scene_, camera_, animation_, play_,
      loop_, time_, track_, fill_, light_, background_, debug_, status_;
  Vec<Command> commands_;
  Vec<Binding> click_bindings_;
  float fraction_{};
  static constexpr Color text{0.82, 0.86, 0.9, 1}, muted{0.42, 0.5, 0.57, 1};
  NodeStyle box(double x, double y, double width, double height) {
    NodeStyle style;
    style.layout.width = Some(width);
    style.layout.height = Some(height);
    style.transform = {1, 0, 0, 1, x, y};
    return style;
  }
  UiId label(UiId parent, ref<str> value, double x, double y, double size = 14,
             Color color = text, bool bold = false, double width = 216) {
    auto &ui = session_.document();
    auto style = box(x, y, width, 40);
    style.hit_test_visible = false;
    auto id = ui.append<Text>(parent, style).unwrap();
    ui.set_text_format(id, {.family = "Noto Sans CJK SC"_str,
                            .language = "zh"_str,
                            .size = size,
                            .bold = bold})
        .unwrap();
    ui.set_text(id, value).unwrap();
    ui.set_text_color(id, color).unwrap();
    return id;
  }
  UiId button(ref<str> value, double x, double y, double width, Action action) {
    auto &ui = session_.document();
    auto id = Button::create(session_, box(x, y, width, 34),
                             {{0.055, 0.085, 0.11, 1},
                              {0.025, 0.12, 0.16, 1},
                              {0.12, 0.3, 0.4, 1},
                              4})
                  .unwrap();
    ui.attach(content_, id).unwrap();
    auto caption = label(id, value, 10, 6, 14, text, false, width - 20);
    ui.set_clip(caption, {true, {0, 0, width - 20, 22}, 0}).unwrap();
    click_bindings_.push(
        Button::on_clicked(ui, id, [this, action](auto &, const auto &) {
          commands_.push({action});
        }).unwrap());
    return caption;
  }
  void caption(UiId id, String value) {
    session_.document().set_text(id, value.as_str()).unwrap();
  }

public:
  ~Panel() {
    for (auto &binding : click_bindings_)
      binding.disconnect();
  }
  explicit Panel(UiSession &session, UiId root) : session_(session) {
    auto &ui = session_.document();
    content_ = ui.create<Stack>(box(0, 0, 248, 788)).unwrap();
    auto style = box(0, 0, 248, 720);
    style.background = {0.018, 0.033, 0.05, 1};
    scroll_ = ScrollView::create(session_, content_, style).unwrap();
    ui.attach(root, scroll_).unwrap();
    label(content_, "vecgltf"_str, 16, 14, 22, text, true);
    name_ = label(content_, ""_str, 16, 53, 14, text, true);
    details_ = label(content_, ""_str, 16, 98, 12, muted);
    label(content_, "场景与视角"_str, 16, 141, 12, muted);
    scene_ = button(""_str, 16, 166, 216, Action::Scene);
    camera_ = button(""_str, 16, 208, 216, Action::Camera);
    button("重新取景"_str, 16, 250, 216, Action::Fit);
    label(content_, "动画"_str, 16, 303, 12, muted);
    animation_ = button(""_str, 16, 328, 216, Action::Animation);
    play_ = button(""_str, 16, 370, 104, Action::Play);
    loop_ = button(""_str, 128, 370, 104, Action::Loop);
    time_ = label(content_, ""_str, 16, 414, 12, muted);
    auto track_style = box(16, 446, 216, 20);
    track_style.background = {0.06, 0.09, 0.12, 1};
    track_style.radius = 3;
    track_style.focusable = true;
    track_ = ui.append<Stack>(content_, track_style).unwrap();
    auto fill_style = box(0, 0, 0, 20);
    fill_style.background = {0.13, 0.36, 0.5, 1};
    fill_style.hit_test_visible = false;
    fill_style.radius = 3;
    fill_ = ui.append<Rectangle>(track_, fill_style).unwrap();
    auto seek = [this](auto &context, const auto &event) {
      if (event.input.type == EventType::PointerDown) {
        context.capture_pointer().unwrap();
        context.focus().unwrap();
        context.stop_propagation();
        context.prevent_default();
      }
      if (event.input.type == EventType::PointerDown ||
          session_.input().captured(event.input.pointer) == Some(track_))
        commands_.push(
            {Action::Seek, gltf::clamp(float(event.local.x / 216), 0, 1)});
    };
    session_.input().on(track_, EventType::PointerDown, seek).unwrap();
    session_.input().on(track_, EventType::PointerMove, seek).unwrap();
    session_.input()
        .on(track_, EventType::PointerUp,
            [](auto &context, const auto &) {
              context.release_pointer().unwrap();
            })
        .unwrap();
    session_.input()
        .on(track_, EventType::KeyDown,
            [this](auto &, const auto &event) {
              if (event.input.key == Key::Home)
                commands_.push({Action::Seek, 0});
              if (event.input.key == Key::Left)
                commands_.push(
                    {Action::Seek, gltf::clamp(fraction_ - 0.01f, 0, 1)});
              if (event.input.key == Key::Right)
                commands_.push(
                    {Action::Seek, gltf::clamp(fraction_ + 0.01f, 0, 1)});
              if (event.input.key == Key::End)
                commands_.push({Action::Seek, 1});
            })
        .unwrap();
    label(content_, "光照与材质"_str, 16, 488, 12, muted);
    button("−"_str, 16, 514, 40, Action::ExposureDown);
    button("+"_str, 192, 514, 40, Action::ExposureUp);
    light_ = label(content_, ""_str, 66, 522, 14, text, false, 120);
    background_ = button(""_str, 16, 556, 216, Action::Background);
    debug_ = button(""_str, 16, 598, 216, Action::Debug);
    button("重新加载模型"_str, 16, 648, 216, Action::Reload);
    status_ = label(content_, ""_str, 16, 692, 12, {0.88, 0.5, 0.28, 1});
    ui.set_layout(status_, {.width = Some(216.0), .height = Some(80.0)})
        .unwrap();
    label(content_, "左键旋转，右键平移\n滚轮缩放，Home 取景"_str, 16, 756, 12,
          muted);
    ui.set_layout(content_, {.width = Some(248.0), .height = Some(818.0)})
        .unwrap();
  }
  void set_visible(bool value) {
    session_.document().set_visible(scroll_, value).unwrap();
  }
  void resize(double height) {
    session_.document()
        .set_layout(scroll_, {.width = Some(248.0), .height = Some(height)})
        .unwrap();
  }
  auto take_commands() -> Vec<Command> {
    return rstd::exchange(commands_, Vec<Command>{});
  }
  void update(const gltf::SceneInstance &instance, unsigned camera,
              bool looping, const gltf::RenderOptions &options,
              ref<str> status) {
    const auto &asset = instance.asset();
    auto path = rstd::path::PathBuf::from(asset.source.as_str());
    auto name = path.as_path().file_name();
    caption(name_, name.is_some() && name->to_str().is_some()
                       ? String::make(*name->to_str())
                       : asset.source.clone());
    caption(details_,
            rstd::format("{} 个节点 / {} 个 mesh\n{} 个材质 / {} 段动画",
                         gltf::count(asset.nodes), gltf::count(asset.meshes),
                         gltf::count(asset.materials),
                         gltf::count(asset.animations)));
    const auto &scene = gltf::at(asset.scenes, instance.scene());
    caption(scene_,
            scene.name.is_empty()
                ? rstd::format("场景 {}", instance.scene())
                : rstd::format("场景 {}：{}", instance.scene(), scene.name));
    caption(camera_, camera == gltf::invalid_index
                         ? "轨道相机"_Str
                         : rstd::format("相机节点 {}", camera));
    auto animation = instance.animation();
    float fraction{}, duration{};
    if (animation == gltf::invalid_index)
      caption(animation_, "不播放动画"_Str);
    else {
      const auto &clip = gltf::at(asset.animations, animation);
      duration = clip.end - clip.start;
      fraction = duration > 0 ? (instance.time() - clip.start) / duration : 0;
      caption(animation_, clip.name.is_empty()
                              ? rstd::format("动画 {}", animation)
                              : rstd::format("{}：{}", animation, clip.name));
    }
    fraction_ = fraction;
    caption(play_, instance.playing() ? "暂停"_Str : "播放"_Str);
    caption(loop_, looping ? "循环：开"_Str : "循环：关"_Str);
    caption(time_, rstd::format("{:.2f} / {:.2f} 秒", f32(instance.time()),
                                f32(duration)));
    session_.document()
        .set_layout(fill_, {.width = Some(double(fraction) * 216),
                            .height = Some(20.0)})
        .unwrap();
    caption(light_, rstd::format("曝光 {:.2f}", f32(options.exposure)));
    caption(background_,
            options.background ? "环境背景：开"_Str : "环境背景：关"_Str);
    ref<str> modes[]{"PBR"_str,    "Base color"_str, "法线"_str,
                     "金属度"_str, "粗糙度"_str,     "Emissive"_str};
    caption(debug_, rstd::format("显示：{}", modes[unsigned(options.debug)]));
    session_.document().set_text(status_, status).unwrap();
  }
};
} // namespace viewer
