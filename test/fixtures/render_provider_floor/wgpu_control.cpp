// Links the wgpu-native runtime on purpose, so the provider-floor scans prove they
// can see a libwgpu_native load command before they assert plug-in images
// load none.
extern "C" void* wgpuCreateInstance(const void* descriptor);

int main(int argc, char**) {
    return argc > 99 && wgpuCreateInstance(nullptr) != nullptr ? 1 : 0;
}
