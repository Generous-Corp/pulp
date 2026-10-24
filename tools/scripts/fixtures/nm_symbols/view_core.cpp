namespace std {
struct format_error {
    virtual ~format_error() {}
};
namespace __function {
template <class F> struct __func {
    virtual ~__func() {}
    virtual void call() {
        f();
    }
    F f;
};
} // namespace __function
} // namespace std
namespace pulp::view {
class ChannelStripView {
  public:
    virtual ~ChannelStripView() {}
    virtual int width() {
        return 1;
    }
};
struct SliderCore {
    void* notify_value() {
        auto lambda = [] {};
        return new std::__function::__func<decltype(lambda)>{};
    }
};
void* make_slider_func(SliderCore& s) {
    return s.notify_value();
}
void* make_strip() {
    return new ChannelStripView;
}
void* make_error() {
    return new std::format_error;
}
} // namespace pulp::view
