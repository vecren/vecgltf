export module vecgltf.viewer.panel;
import vecgltf;
import vecgltf.render;
import vecgltf.viewer.camera;
import vui.compose;

export namespace viewer
{
using namespace rstd::prelude;
using namespace rstd::literals;
using namespace vui;
namespace gltf = vecgltf;
enum class Action
{
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
    float  value {};
};
namespace cx = vui::compose;

enum class Caption
{
    Name,
    Details,
    Scene,
    Camera,
    Animation,
    Play,
    Loop,
    Time,
    CameraTime,
    CameraPlay,
    CameraLoop,
    CameraInterval,
    CameraSave,
    Light,
    Background,
    Debug,
    Status,
    Count
};
struct CommandQueue {
    Vec<Command> values;
};

auto panel_box(double x, double y, double width, double height) -> NodeStyle {
    return NodeStyleBuilder {}.width(width).height(height).transform({ 1, 0, 0, 1, x, y }).build();
}

class Timeline {
    EventRouter&               input_;
    rstd::rc::Rc<CommandQueue> commands_;
    Action                     action_;
    state::Cell<float>         fraction_;
    Option<PointerKey>         pointer_;
    void                       seek(EventContext& context, float value) {
        value = gltf::clamp(value, 0, 1);
        fraction_.set(value).unwrap();
        commands_.get()->values.push(Command { action_, value });
        context.stop_propagation();
        context.prevent_default();
    }
    void seek_pointer(EventContext& context, UiId target) {
        auto point    = context.content_position();
        auto snapshot = input_.snapshot();
        if (point.is_none() || snapshot.is_none()) return;
        auto visual = snapshot->get(target);
        if (! visual || visual->content_geometry.size.width <= 0) return;
        seek(context, float(point->x / visual->content_geometry.size.width));
    }

public:
    Timeline(EventRouter&               input,
             rstd::rc::Rc<CommandQueue> commands,
             Action                     action,
             state::Cell<float>         fraction)
        : input_(input),
          commands_(rstd::move(commands)),
          action_(action),
          fraction_(rstd::move(fraction)) {}
    auto fraction() const -> state::Value<float> { return fraction_.value(); }
    void progress(state::GraphUpdate& changes, float value) {
        changes.set(fraction_, gltf::clamp(value, 0, 1)).unwrap();
    }
    void handle(EventContext& context, const RoutedEvent& event) {
        auto pointer = PointerKey { event.input.device, event.input.pointer };
        switch (event.input.type) {
        case EventType::PointerDown:
            if (event.input.button != PointerButton::Primary || pointer_.is_some()) return;
            context.capture_pointer().unwrap();
            context.focus().unwrap();
            pointer_ = Some(pointer);
            seek_pointer(context, event.current);
            break;
        case EventType::PointerMove:
            if (pointer_ == Some(pointer) &&
                input_.captured(pointer.pointer, pointer.device) == Some(UiId(event.current)))
                seek_pointer(context, event.current);
            break;
        case EventType::PointerUp:
            if (pointer_ != Some(pointer) || event.input.button != PointerButton::Primary) return;
            pointer_ = None();
            if (input_.captured(pointer.pointer, pointer.device) == Some(UiId(event.current))) {
                context.release_pointer().unwrap();
                context.stop_propagation();
                context.prevent_default();
            }
            break;
        case EventType::PointerCancel:
        case EventType::CaptureLost:
            if (pointer_ == Some(pointer)) pointer_ = None();
            break;
        case EventType::KeyDown:
            switch (event.input.key) {
            case Key::Home: seek(context, 0); break;
            case Key::End: seek(context, 1); break;
            case Key::Left:
                seek(context, state::read(fraction_.value()).unwrap().value() - .01f);
                break;
            case Key::Right:
                seek(context, state::read(fraction_.value()).unwrap().value() + .01f);
                break;
            default: break;
            }
            break;
        case EventType::FocusGained:
            context.document().set_background(event.current, { .12, .3, .4, 1 }, 3).unwrap();
            break;
        case EventType::FocusLost:
            context.document().set_background(event.current, { .06, .09, .12, 1 }, 3).unwrap();
            break;
        default: break;
        }
    }
};
struct TimelineBehavior {
    rstd::rc::Rc<Timeline> controller;
    void                   handle(EventContext& context, const RoutedEvent& event) {
        controller.get()->handle(context, event);
    }
    auto draw(ModifierDrawContext& context) -> Result<empty, UiError> {
        auto content = context.draw_content();
        if (content.is_err()) return content;
        auto progress = state::read(controller->fraction());
        if (progress.is_err()) return Err(UiError::PaintFailed);
        auto& paint = context.paint();
        auto  size  = paint.size();
        paint.rect(
            { 0, 0, size.width * progress.unwrap().value(), size.height }, { .13, .36, .5, 1 }, 3);
        return Ok(empty {});
    }
};
struct TimelineDescriptor {
    using State = TimelineBehavior;
    rstd::rc::Rc<Timeline> controller;
    auto                   create() const -> State { return { controller }; }
    void update(State& state, cx::BehaviorContext&) const { state.controller = controller; }
};
struct CaptionTag {};
struct PanelModel {
    state::Graph               graph;
    Vec<state::Cell<String>>   captions;
    rstd::rc::Rc<CommandQueue> commands { rstd::rc::make_rc<CommandQueue>() };
    rstd::rc::Rc<Timeline>     animation_timeline, camera_timeline;
    static constexpr Color     text { .82, .86, .9, 1 }, muted { .42, .5, .57, 1 };

    explicit PanelModel(EventRouter& input)
        : animation_timeline(rstd::rc::make_rc<Timeline>(input,
                                                         commands.clone(),
                                                         Action::Seek,
                                                         graph.cell(0.0f).unwrap())),
          camera_timeline(rstd::rc::make_rc<Timeline>(input,
                                                      commands.clone(),
                                                      Action::CameraSeek,
                                                      graph.cell(0.0f).unwrap())) {
        for (unsigned i {}; i < unsigned(Caption::Count); ++i)
            captions.push(graph.cell(String::make()).unwrap());
    }
    static auto label(ref<str> value,
                      double   x,
                      double   y,
                      double   size   = 14,
                      Color    color  = text,
                      bool     bold   = false,
                      double   width  = 216,
                      double   height = 40) -> cx::Element {
        return cx::text(value)
            .format(TextFormatBuilder {}
                        .family("Noto Sans CJK SC"_str)
                        .language("zh"_str)
                        .size(size)
                        .bold(bold)
                        .build())
            .color(color)
            .style(NodeStyleBuilder { panel_box(x, y, width, height) }
                       .hit_test_visible(false)
                       .build());
    }
    auto label(Caption caption,
               double  x,
               double  y,
               double  size    = 14,
               Color   color   = text,
               bool    bold    = false,
               double  width   = 216,
               double  height  = 40,
               bool    clipped = false) const -> cx::Element {
        auto value = captions[usize(unsigned(caption))].value();
        return cx::component<CaptionTag>(
            cx::Key { unsigned(caption) + 1 },
            [value, x, y, size, color, bold, width, height, clipped](
                cx::CompositionContext& scope) -> cx::ComposeResult<cx::Element> {
                auto read = scope.read(value);
                if (read.is_err()) return Err(read.unwrap_err());
                auto result =
                    label(read.unwrap().value().as_str(), x, y, size, color, bold, width, height);
                if (clipped)
                    result = result.style(NodeStyleBuilder { panel_box(x, y, width, height) }
                                              .hit_test_visible(false)
                                              .clip({ true, { 0, 0, width, 22 }, 0 })
                                              .build());
                return Ok(rstd::move(result));
            });
    }
    auto button(cx::Element caption, double x, double y, double width, Action action) const
        -> cx::Element {
        return cx::button(
                   rstd::move(caption),
                   [commands = commands.clone(), action]() mutable {
                       commands.get()->values.push(Command { action });
                   },
                   ButtonStyleBuilder {}
                       .normal({ .055, .085, .11, 1 })
                       .pressed({ .025, .12, .16, 1 })
                       .focused({ .12, .3, .4, 1 })
                       .radius(4)
                       .build())
            .style(panel_box(x, y, width, 34));
    }
    auto button(Caption caption, double x, double y, double width, Action action) const
        -> cx::Element {
        return button(
            label(caption, 10, 6, 14, text, false, width - 20, 40, true), x, y, width, action);
    }
    auto button(ref<str> caption, double x, double y, double width, Action action) const
        -> cx::Element {
        auto content = label(caption, 10, 6, 14, text, false, width - 20)
                           .style(NodeStyleBuilder { panel_box(10, 6, width - 20, 40) }
                                      .hit_test_visible(false)
                                      .clip({ true, { 0, 0, width - 20, 22 }, 0 })
                                      .build());
        return button(rstd::move(content), x, y, width, action);
    }
    static auto timeline(rstd::rc::Rc<Timeline> controller, double y, ref<str> key) -> cx::Element {
        return cx::stack(Vec<cx::Element> {})
            .style(NodeStyleBuilder { panel_box(16, y, 216, 20) }
                       .background({ .06, .09, .12, 1 })
                       .radius(3)
                       .focusable(true)
                       .build())
            .key(cx::Key { key })
            .behavior(cx::Key { 1 }, TimelineDescriptor { rstd::move(controller) });
    }
    auto content(double height) const -> cx::Element {
        return cx::scroll_view(
                   cx::stack(
                       {
                           label("vecgltf"_str, 16, 14, 22, text, true),
                           label(Caption::Name, 16, 53, 14, text, true),
                           label(Caption::Details, 16, 98, 12, muted),
                           label("场景与视角"_str, 16, 141, 12, muted),
                           button(Caption::Scene, 16, 166, 216, Action::Scene),
                           button(Caption::Camera, 16, 208, 216, Action::Camera),
                           button("重新取景"_str, 16, 250, 216, Action::Fit),
                           label("相机轨迹"_str, 16, 303, 12, muted),
                           button("记录视角"_str, 16, 328, 104, Action::CameraRecord),
                           button("撤回末帧"_str, 128, 328, 104, Action::CameraRemove),
                           label(Caption::CameraTime, 16, 372, 12, muted),
                           button(Caption::CameraPlay, 16, 420, 104, Action::CameraPlay),
                           button(Caption::CameraLoop, 128, 420, 104, Action::CameraLoop),
                           timeline(camera_timeline.clone(), 466, "camera-timeline"_str),
                           button("−"_str, 16, 504, 40, Action::CameraIntervalDown),
                           button("+"_str, 192, 504, 40, Action::CameraIntervalUp),
                           label(Caption::CameraInterval, 66, 512, 12, text, false, 120),
                           button("清空"_str, 16, 546, 104, Action::CameraClear),
                           button(Caption::CameraSave, 128, 546, 104, Action::CameraSave),
                           label("模型动画"_str, 16, 591, 12, muted),
                           button(Caption::Animation, 16, 616, 216, Action::Animation),
                           button(Caption::Play, 16, 658, 104, Action::Play),
                           button(Caption::Loop, 128, 658, 104, Action::Loop),
                           label(Caption::Time, 16, 702, 12, muted),
                           timeline(animation_timeline.clone(), 734, "animation-timeline"_str),
                           label("光照与材质"_str, 16, 776, 12, muted),
                           button("−"_str, 16, 802, 40, Action::ExposureDown),
                           button("+"_str, 192, 802, 40, Action::ExposureUp),
                           label(Caption::Light, 66, 810, 14, text, false, 120),
                           button(Caption::Background, 16, 844, 216, Action::Background),
                           button(Caption::Debug, 16, 886, 216, Action::Debug),
                           button("重新加载模型"_str, 16, 936, 216, Action::Reload),
                           label(Caption::Status, 16, 980, 12, { .88, .5, .28, 1 }, false, 216, 56),
                           label(
                               "左键旋转，右键平移\n滚轮缩放，Home 取景"_str, 16, 1044, 12, muted),
                       })
                       .style(panel_box(0, 0, 248, 1106)))
            .style(NodeStyleBuilder { panel_box(0, 0, 248, height) }
                       .background({ .018, .033, .05, 1 })
                       .build());
    }
};

class Panel {
    UiSession&               session_;
    UiId                     mount_;
    rstd::rc::Rc<PanelModel> model_;
    Option<cx::Composition>  composition_;
    void                     caption(state::GraphUpdate& changes, Caption target, String value) {
        changes.set(model_->captions[usize(unsigned(target))], rstd::move(value)).unwrap();
    }

public:
    explicit Panel(UiSession& session, UiId root)
        : session_(session), model_(rstd::rc::make_rc<PanelModel>(session.input())) {
        mount_       = session_.document().append<Stack>(root, panel_box(0, 0, 248, 720)).unwrap();
        composition_ = Some(cx::Composition::create(session_, mount_).unwrap());
        composition_
            ->set_content(cx::with_constraints(
                cx::Key { 1 },
                [model = model_.clone()](cx::CompositionContext&, LayoutConstraints constraints)
                    -> cx::ComposeResult<cx::Element> {
                    return Ok(
                        model->content(constraints.max_height.unwrap_or(constraints.min_height)));
                }))
            .unwrap();
    }
    void set_visible(bool value) { session_.document().set_visible(mount_, value).unwrap(); }
    void resize(double height) {
        session_.document()
            .set_layout(mount_, LayoutStyleBuilder {}.width(248).height(height).build())
            .unwrap();
    }
    auto take_commands() -> Vec<Command> {
        return rstd::exchange(model_.get()->commands.get()->values, Vec<Command> {});
    }
    auto check() -> bool {
        auto diagnostics = composition_->take_diagnostics();
        auto ok          = true;
        auto report      = [&](const cx::ComposeError& error) {
            auto file = rstd::ffi::CStr::from_ptr(error.source.file_name()).to_str().unwrap();
            rstd::io::eprintln("viewer composition stage={} operation={} path={} at={}:{}",
                               unsigned(error.stage),
                               error.operation.as_str(),
                               error.path.as_str(),
                               file,
                               error.source.line());
            ok = false;
        };
        for (const auto& error : diagnostics.entries) report(error);
        if (diagnostics.omitted)
            rstd::io::eprintln("viewer composition omitted {} diagnostics", diagnostics.omitted);
        if (diagnostics.entries.is_empty()) {
            auto result = composition_->last_result();
            if (result.is_some() && result->is_err()) report(result->unwrap_err());
        }
        return ok;
    }
    void update(const gltf::SceneInstance& instance,
                unsigned                   camera,
                bool                       looping,
                const gltf::RenderOptions& options,
                ref<str>                   status,
                const CameraTrackStatus&   camera_track = {}) {
        auto        changes = state::begin_update(model_->graph.view()).unwrap();
        const auto& asset   = instance.asset();
        auto        path    = rstd::path::PathBuf::from(asset.source.as_str());
        auto        name    = path.as_path().file_name();
        caption(changes,
                Caption::Name,
                name.is_some() && name->to_str().is_some() ? String::make(*name->to_str())
                                                           : asset.source.clone());
        caption(changes,
                Caption::Details,
                rstd::format("{} 个节点 / {} 个 mesh\n{} 个材质 / {} 段动画",
                             gltf::count(asset.nodes),
                             gltf::count(asset.meshes),
                             gltf::count(asset.materials),
                             gltf::count(asset.animations)));
        const auto& scene = gltf::at(asset.scenes, instance.scene());
        caption(changes,
                Caption::Scene,
                scene.name.is_empty() ? rstd::format("场景 {}", instance.scene())
                                      : rstd::format("场景 {}：{}", instance.scene(), scene.name));
        caption(changes,
                Caption::Camera,
                camera_track.active             ? "相机轨迹"_Str
                : camera == gltf::invalid_index ? "轨道相机"_Str
                                                : rstd::format("相机节点 {}", camera));
        caption(changes,
                Caption::CameraTime,
                rstd::format("{} 帧  {:.2f} / {:.2f} 秒{}",
                             camera_track.keyframes,
                             f32(camera_track.time),
                             f32(camera_track.duration),
                             camera_track.keyframes < usize(2) ? "\n先记录两个视角"_str : ""_str));
        caption(
            changes, Caption::CameraPlay, camera_track.playing ? "暂停轨迹"_Str : "播放轨迹"_Str);
        caption(
            changes, Caption::CameraLoop, camera_track.looping ? "循环：开"_Str : "循环：关"_Str);
        caption(changes,
                Caption::CameraInterval,
                rstd::format("追加间隔 {:.2f}秒", f32(camera_track.interval)));
        caption(
            changes, Caption::CameraSave, camera_track.dirty ? "保存轨迹 *"_Str : "保存轨迹"_Str);
        model_.get()->camera_timeline.get()->progress(
            changes, camera_track.duration > 0 ? camera_track.time / camera_track.duration : 0);
        auto  animation = instance.animation();
        float fraction {}, duration {};
        if (animation == gltf::invalid_index)
            caption(changes, Caption::Animation, "不播放动画"_Str);
        else {
            const auto& clip = gltf::at(asset.animations, animation);
            duration         = clip.end - clip.start;
            fraction         = duration > 0 ? (instance.time() - clip.start) / duration : 0;
            caption(changes,
                    Caption::Animation,
                    clip.name.is_empty() ? rstd::format("动画 {}", animation)
                                         : rstd::format("{}：{}", animation, clip.name));
        }
        caption(changes, Caption::Play, instance.playing() ? "暂停"_Str : "播放"_Str);
        caption(changes, Caption::Loop, looping ? "循环：开"_Str : "循环：关"_Str);
        caption(changes,
                Caption::Time,
                rstd::format("{:.2f} / {:.2f} 秒", f32(instance.time()), f32(duration)));
        model_.get()->animation_timeline.get()->progress(changes, fraction);
        caption(changes, Caption::Light, rstd::format("曝光 {:.2f}", f32(options.exposure)));
        caption(changes,
                Caption::Background,
                options.background ? "环境背景：开"_Str : "环境背景：关"_Str);
        ref<str> modes[] { "PBR"_str,    "Base color"_str, "法线"_str,
                           "金属度"_str, "粗糙度"_str,     "Emissive"_str };
        caption(changes, Caption::Debug, rstd::format("显示：{}", modes[unsigned(options.debug)]));
        caption(changes, Caption::Status, String::make(status));
        changes.commit().unwrap();
    }
};
} // namespace viewer
