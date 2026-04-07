#include <cstdio>
#include <fstream>

int main() {
    // Verify the font file exists and is non-empty
    std::ifstream f(FONTAUDIO_TTF, std::ios::binary | std::ios::ate);
    if (f.good() && f.tellg() > 0) {
        std::printf("fontaudio: OK (ttf size=%lld bytes)\n",
                    static_cast<long long>(f.tellg()));
        return 0;
    }
    std::printf("fontaudio: FAIL (ttf not found or empty)\n");
    return 1;
}
