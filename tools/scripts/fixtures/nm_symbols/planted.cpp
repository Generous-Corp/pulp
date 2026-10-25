namespace pulp::view {
// A view-core translation unit that sees Meter's class definition inline.
class Meter {
  public:
    virtual ~Meter() {}
    virtual void set_level(float, float) {}
    float level = 0;
};
void* make_meter() {
    return new Meter;
}
} // namespace pulp::view
