#include <RTNeural/RTNeural.h>
#include <cstdio>

int main() {
    RTNeural::ModelT<float, 1, 1,
        RTNeural::DenseT<float, 1, 4>,
        RTNeural::TanhActivationT<float, 4>,
        RTNeural::DenseT<float, 4, 1>
    > model;
    model.reset();
    float input[] = { 0.5f };
    float output = model.forward(input);
    std::printf("rtneural: OK (forward output=%.4f)\n", output);
    return 0;
}
