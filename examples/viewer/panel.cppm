export module vecgltf.viewer.panel;
import vecgltf;
import vecgltf.render;
import vecgltf.viewer.camera;
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
  Debug,
  CameraRecord,
  CameraRemove,
  CameraPlay,
  CameraLoop,
  CameraSeek,
  CameraIntervalDown,
  CameraIntervalUp,
  CameraClear,
  CameraSave
};
struct Command {
  Action action;
  float value{};
};
class Panel {
  struct Timeline {
    UiId track, fill;
    float fraction{};
  };
  UiSession &session_;
  UiId content_, scroll_, name_, details_, scene_, camera_, animation_, play_,
      loop_, time_, camera_time_, camera_play_, camera_loop_, camera_interval_,
      camera_save_, light_, background_, debug_, status_;
  Timeline animation_timeline_, camera_timeline_;
  Vec<Command> commands_;
  Vec<Binding> click_bindings_;
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
  void timeline(Timeline &value, double y, Action action) {
    auto &ui = session_.document();
    auto style = box(16, y, 216, 20);
    style.background = {0.06, 0.09, 0.12, 1};
    style.radius = 3;
    style.focusable = true;
    value.track = ui.append<Stack>(content_, style).unwrap();
    auto fill_style = box(0, 0, 0, 20);
    fill_style.background = {0.13, 0.36, 0.5, 1};
    fill_style.hit_test_visible = false;
    fill_style.radius = 3;
    value.fill = ui.append<Rectangle>(value.track, fill_style).unwrap();
    auto *target = &value;
    auto seek = [this, target, action](auto &context, const auto &event) {
      if (event.input.type == EventType::PointerDown) {
        if (event.input.button != PointerButton::Primary)
          return;
        context.capture_pointer().unwrap();
        context.focus().unwrap();
      } else if (session_.input().captured(event.input.pointer) !=
                 Some(target->track))
        return;
      target->fraction = gltf::clamp(float(event.local.x / 216), 0, 1);
      commands_.push({action, target->fraction});
      context.stop_propagation();
      context.prevent_default();
    };
    session_.input().on(value.track, EventType::PointerDown, seek).unwrap();
    session_.input().on(value.track, EventType::PointerMove, seek).unwrap();
    session_.input()
        .on(value.track, EventType::PointerUp,
            [this, target](auto &context, const auto &event) {
              if (session_.input().captured(event.input.pointer) ==
                  Some(target->track)) {
                context.release_pointer().unwrap();
                context.stop_propagation();
                context.prevent_default();
              }
            })
        .unwrap();
    session_.input()
        .on(value.track, EventType::KeyDown,
            [this, target, action](auto &context, const auto &event) {
              switch (event.input.key) {
              case Key::Home:
                target->fraction = 0;
                break;
              case Key::End:
                target->fraction = 1;
                break;
              case Key::Left:
                target->fraction = gltf::clamp(target->fraction - 0.01f, 0, 1);
                break;
              case Key::Right:
                target->fraction = gltf::clamp(target->fraction + 0.01f, 0, 1);
                break;
              default:
                return;
              }
              commands_.push({action, target->fraction});
              context.stop_propagation();
              context.prevent_default();
            })
        .unwrap();
    session_.input()
        .on(value.track, EventType::FocusGained,
            [target](auto &context, const auto &) {
              context.document()
                  .set_background(target->track, {0.12, 0.3, 0.4, 1}, 3)
                  .unwrap();
            })
        .unwrap();
    session_.input()
        .on(value.track, EventType::FocusLost,
            [target](auto &context, const auto &) {
              context.document()
                  .set_background(target->track, {0.06, 0.09, 0.12, 1}, 3)
                  .unwrap();
            })
        .unwrap();
  }
  void progress(Timeline &value, float fraction) {
    value.fraction = gltf::clamp(fraction, 0, 1);
    session_.document()
        .set_layout(value.fill, {.width = Some(double(value.fraction) * 216),
                                 .height = Some(20.0)})
        .unwrap();
  }

public:
  ~Panel() {
    for (auto &binding : click_bindings_)
      binding.disconnect();
  }
  explicit Panel(UiSession &session, UiId root) : session_(session) {
    auto &ui = session_.document();
    content_ = ui.create<Stack>(box(0, 0, 248, 1106)).unwrap();
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
    label(content_, "相机轨迹"_str, 16, 303, 12, muted);
    button("记录视角"_str, 16, 328, 104, Action::CameraRecord);
    button("撤回末帧"_str, 128, 328, 104, Action::CameraRemove);
    camera_time_ = label(content_, ""_str, 16, 372, 12, muted);
    camera_play_ = button(""_str, 16, 420, 104, Action::CameraPlay);
    camera_loop_ = button(""_str, 128, 420, 104, Action::CameraLoop);
    timeline(camera_timeline_, 466, Action::CameraSeek);
    button("−"_str, 16, 504, 40, Action::CameraIntervalDown);
    button("+"_str, 192, 504, 40, Action::CameraIntervalUp);
    camera_interval_ = label(content_, ""_str, 66, 512, 12, text, false, 120);
    button("清空"_str, 16, 546, 104, Action::CameraClear);
    camera_save_ = button(""_str, 128, 546, 104, Action::CameraSave);
    label(content_, "模型动画"_str, 16, 591, 12, muted);
    animation_ = button(""_str, 16, 616, 216, Action::Animation);
    play_ = button(""_str, 16, 658, 104, Action::Play);
    loop_ = button(""_str, 128, 658, 104, Action::Loop);
    time_ = label(content_, ""_str, 16, 702, 12, muted);
    timeline(animation_timeline_, 734, Action::Seek);
    label(content_, "光照与材质"_str, 16, 776, 12, muted);
    button("−"_str, 16, 802, 40, Action::ExposureDown);
    button("+"_str, 192, 802, 40, Action::ExposureUp);
    light_ = label(content_, ""_str, 66, 810, 14, text, false, 120);
    background_ = button(""_str, 16, 844, 216, Action::Background);
    debug_ = button(""_str, 16, 886, 216, Action::Debug);
    button("重新加载模型"_str, 16, 936, 216, Action::Reload);
    status_ = label(content_, ""_str, 16, 980, 12, {0.88, 0.5, 0.28, 1});
    ui.set_layout(status_, {.width = Some(216.0), .height = Some(56.0)})
        .unwrap();
    label(content_, "左键旋转，右键平移\n滚轮缩放，Home 取景"_str, 16, 1044, 12,
          muted);
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
              bool looping, const gltf::RenderOptions &options, ref<str> status,
              const CameraTrackStatus &camera_track = {}) {
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
    caption(camera_, camera_track.active ? "相机轨迹"_Str
                     : camera == gltf::invalid_index
                         ? "轨道相机"_Str
                         : rstd::format("相机节点 {}", camera));
    caption(camera_time_,
            rstd::format("{} 帧  {:.2f} / {:.2f} 秒{}", camera_track.keyframes,
                         f32(camera_track.time), f32(camera_track.duration),
                         camera_track.keyframes < usize(2)
                             ? "\n先记录两个视角"_str
                             : ""_str));
    caption(camera_play_,
            camera_track.playing ? "暂停轨迹"_Str : "播放轨迹"_Str);
    caption(camera_loop_,
            camera_track.looping ? "循环：开"_Str : "循环：关"_Str);
    caption(camera_interval_,
            rstd::format("追加间隔 {:.2f}秒", f32(camera_track.interval)));
    caption(camera_save_,
            camera_track.dirty ? "保存轨迹 *"_Str : "保存轨迹"_Str);
    progress(camera_timeline_, camera_track.duration > 0
                                   ? camera_track.time / camera_track.duration
                                   : 0);
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
    caption(play_, instance.playing() ? "暂停"_Str : "播放"_Str);
    caption(loop_, looping ? "循环：开"_Str : "循环：关"_Str);
    caption(time_, rstd::format("{:.2f} / {:.2f} 秒", f32(instance.time()),
                                f32(duration)));
    progress(animation_timeline_, fraction);
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
