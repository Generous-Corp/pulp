# Image codec and pixel-buffer tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# PNG / GIF / TIFF codec round-trip suite: GIF89a encoder/decoder +
# Baseline TIFF 6.0 encoder/decoder. Skia-free, runs on every config.
pulp_add_test_suite(pulp-test-image-codecs-extended LIBRARIES pulp::canvas)
