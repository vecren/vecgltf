export module vecgltf.trace;
import rstd;

export namespace vecgltf
{
using namespace rstd::prelude;
using rstd::sync::Arc;

enum class LoadEvent
{
    Asset,
    Document,
    Buffers,
    Validation,
    Materials,
    Geometry,
    Scene,
    Animations,
    Images,
    ImageDecode,
    ImageCopy,
    RendererCreate,
    RendererInitialize,
    Environment,
    EnvironmentProcess,
    EnvironmentImage,
    PrepareAsset,
    PrepareMaterials,
    PrepareGeometry,
    Texture,
    TextureCopy,
    Mipmaps,
    ImagePrepare,
    Uploads,
    GpuMipmaps,
    Count
};

// Callbacks run synchronously; each observer receives nested, balanced scopes.
struct LoadObserver {
    using Trait                  = LoadObserver;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = LoadObserver;
        void Begin(LoadEvent event) const { rstd::trait_call<0>(this, event); }
        void End() const { rstd::trait_call<1>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Begin, &T::End>;
};

class LoadTrace {
    Option<Arc<dyn<LoadObserver>>> observer_;

public:
    LoadTrace() = default;
    explicit LoadTrace(Arc<dyn<LoadObserver>> observer): observer_(Some(rstd::move(observer))) {}
    LoadTrace(const LoadTrace& other)
        : observer_(other.observer_ ? Some(other.observer_->clone()) : None()) {}
    auto operator=(const LoadTrace& other) -> LoadTrace& {
        if (this != &other) observer_ = other.observer_ ? Some(other.observer_->clone()) : None();
        return *this;
    }
    LoadTrace(LoadTrace&&)                    = default;
    auto operator=(LoadTrace&&) -> LoadTrace& = default;
    bool begin(LoadEvent event) const {
        if (! observer_) return false;
        (*observer_)->Begin(event);
        return true;
    }
    void end() const {
        if (observer_) (*observer_)->End();
    }
};

class LoadScope {
    LoadTrace trace_;
    bool      active_ {};

public:
    LoadScope(const LoadTrace& trace, LoadEvent event)
        : trace_(trace), active_(trace_.begin(event)) {}
    LoadScope(const LoadScope&)                    = delete;
    auto operator=(const LoadScope&) -> LoadScope& = delete;
    LoadScope(LoadScope&& other)
        : trace_(rstd::move(other.trace_)), active_(rstd::exchange(other.active_, false)) {}
    auto operator=(LoadScope&& other) -> LoadScope& {
        if (this != &other) {
            finish();
            trace_  = rstd::move(other.trace_);
            active_ = rstd::exchange(other.active_, false);
        }
        return *this;
    }
    ~LoadScope() { finish(); }
    void finish() {
        if (rstd::exchange(active_, false)) trace_.end();
    }
};
} // namespace vecgltf
