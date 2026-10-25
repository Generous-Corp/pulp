namespace pulp::view {
class Meter {
  public:
    virtual ~Meter();
    void set_level(float, float);
    float level = 0;
};
Meter::~Meter() {}
void Meter::set_level(float a, float b) {
    level = a + b;
}
} // namespace pulp::view
