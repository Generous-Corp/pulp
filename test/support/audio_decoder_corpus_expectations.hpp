#pragma once

// The decoder corpus's expected outcome for every committed seed, shared by the
// cross-format replay and the per-format files.

#include <map>
#include <optional>
#include <string>

namespace pulp_test::audio_decoder_corpus {

// What each committed seed must do, one row per seed, so "both refused"
// cannot read as agreement for a seed that is supposed to decode.
//   must_decode        read_info and read succeed through the registry; false
//                      means it may be refused
//   truncated          WAV seeds that decode: whether the header declares more
//                      frames than the file holds (recovered, reported)
//   libsndfile_decodes mirror of the lab differential's table (checked there)
//   reason             why a refusal, a recovery or a split is expected
// A seed in the corpus without a row fails the replay until it is classified.
struct SeedExpectation {
    bool must_decode;
    std::optional<bool> truncated;
    std::optional<bool> libsndfile_decodes;
    const char* reason;
};
inline const std::map<std::string, SeedExpectation>& seed_expectations() {
    using E = SeedExpectation;
    static const std::map<std::string, SeedExpectation> rows = {
        // valid seeds
        {"aifc-none-pcm16-mono.aifc", E{true, {}, {}, ""}},
        {"aifc-sowt-pcm16-stereo.aifc", E{true, {}, {}, ""}},
        {"aiff-comm-after-ssnd.aiff", E{true, {}, {}, ""}},
        {"aiff-pcm16-mono.aiff", E{true, {}, {}, ""}},
        {"caf-pcm16-mono.caf", E{true, {}, {}, ""}},
        {"flac-fixed-blocksize-mono.flac", E{true, {}, {}, ""}},
        {"flac-seektable-padding.flac", E{true, {}, {}, ""}},
        {"flac-variable-blocksize-verbatim.flac", E{true, {}, {}, ""}},
        {"m4a-aac-co64.m4a", E{true, {}, {}, ""}},
        {"m4a-aac-lc-mono.m4a", E{true, {}, {}, ""}},
        {"m4a-aac-mdat-size1.m4a", E{true, {}, {}, ""}},
        {"m4a-alac-stereo.m4a", E{true, {}, {}, ""}},
        {"mp3-ape-size-below-footer.mp3", E{true, {}, {}, ""}},
        {"mp3-apev1-footer.mp3", E{true, {}, {}, ""}},
        {"mp3-apev1-header-flag.mp3", E{true, {}, {}, ""}},
        {"mp3-apev2-footer.mp3", E{true, {}, {}, ""}},
        {"mp3-apev2-size-high-bit.mp3", E{true, {}, {}, ""}},
        {"mp3-cbr-mono.mp3", E{true, {}, {}, ""}},
        {"mp3-id3v1-before-apev2.mp3", E{true, {}, {}, ""}},
        {"mp3-id3v2-xing-vbr.mp3", E{true, {}, {}, ""}},
        {"mp3-lyrics3v2-before-id3v1.mp3", E{true, {}, {}, ""}},
        {"mp3-trailing-junk.mp3", E{true, {}, {}, ""}},
        {"ogg-chained-two-streams.ogg", E{true, {}, {}, ""}},
        {"ogg-vorbis-mono.ogg", E{true, {}, {}, ""}},
        {"wav-bwf-bext-before-data.wav", E{true, false, {}, ""}},
        {"wav-bw64-ds64.wav",
         E{true, false, false, "BW64 envelope with ds64; libsndfile does not recognise BW64"}},
        {"wav-data-size-zero.wav", E{true, false, {}, ""}},
        {"wav-duplicate-fmt-identical.wav",
         E{true, false, false,
           "a fmt chunk repeated byte for byte is accepted with a warning; libsndfile refuses "
           "any second fmt"}},
        {"wav-extensible-pcm24in32-stereo.wav", E{true, false, {}, ""}},
        {"wav-float32-mono-fmt18.wav", E{true, false, {}, ""}},
        {"wav-float64-mono.wav", E{true, false, {}, ""}},
        {"wav-list-info-heavy.wav", E{true, false, {}, ""}},
        {"wav-pcm16-mono-canonical44.wav", E{true, false, {}, ""}},
        {"wav-pcm24-stereo.wav", E{true, false, {}, ""}},
        {"wav-pcm32-stereo.wav", E{true, false, {}, ""}},
        {"wav-pcm8-stereo.wav", E{true, false, {}, ""}},
        {"wav-rf64-ds64.wav", E{true, false, {}, "RF64 envelope with ds64"}},
        // header longer than the file: the whole frames present decode
        {"wav-data-past-eof.wav",
         E{true, true, {}, "data chunk runs past EOF: the whole frames present decode"}},
        {"wav-data-size-ffffffff-no-ds64.wav",
         E{true, true, {}, "0xFFFFFFFF data size with no ds64: decodes to the end of the file"}},
        {"wav-declares-100000001-frames.wav",
         E{true, true, {}, "declares 100,000,001 frames over 800: the 800 decode"}},
        {"wav-fmt-after-data.wav",
         E{true, false, false, "fmt after data is accepted with a warning; libsndfile refuses it"}},
        {"wav-odd-chunk-eof-no-pad.wav",
         E{true, true, {}, "odd data chunk at EOF with no pad byte: the partial frame is dropped"}},
        {"wav-riff-size-lying-small.wav",
         E{true, false, {}, "RIFF size smaller than the chunks it holds: a warning only"}},
        {"wav-rf64-ds64-data-size-zero.wav",
         E{true, true, true,
           "0xFFFFFFFF data size and a zero ds64 entry: decodes to the end of the file; "
           "libsndfile trusts the zero and decodes no frames"}},
        {"wav-rf64-ds64-data-past-eof.wav",
         E{true, true, {}, "ds64 data size runs past EOF: the whole frames present decode"}},
        // valid per the README, refused by Pulp for a stated reason
        {"wav-w64-pcm16-mono.wav",
         E{false, {}, true, "Sony Wave64 envelope; Pulp registers no Wave64 reader"}},
        // malformed seeds: refused or decoded, the oracle judges what comes back
        {"aiff-ssnd-past-eof.aiff",
         E{false, {}, true, "SSND runs past the end of the file; Pulp refuses truncated streams"}},
        {"flac-streaminfo-huge-total-samples.flac", E{false, {}, {}, ""}},
        {"flac-truncated-midframe.flac", E{false, {}, {}, "declared 800 frames, stream holds 384"}},
        {"mp3-truncated-frame.mp3", E{false, {}, {}, ""}},
        {"mp3-xing-declares-more-than-stream.mp3",
         E{false, {}, {}, "Xing declares 6400 frames, stream delivers 2351"}},
        {"mp3-id3v2-scan-counts-undecodable-frame.mp3",
         E{false, {}, {}, "no declared length; header scan counts a frame the decoder rejects"}},
        {"ogg-page-bad-crc.ogg", E{false, {}, {}, ""}},
        {"ogg-truncated-page.ogg", E{false, {}, {}, ""}},
        {"ogg-vendor-length-negative.ogg", E{false, {}, {}, "negative vendor-string length"}},
        {"ogg-comment-length-int-max.ogg",
         E{false, {}, {}, "comment-string length near INT_MAX; len + 1 overflowed the allocator"}},
        {"wav-bext-declares-4gib.wav",
         E{false,
           {},
           {},
           "bext chunk shorter than its fixed fields; CHOC once sized a 4 GiB string read"}},
        {"wav-block-align-smaller-than-frame.wav",
         E{false,
           {},
           true,
           "nBlockAlign smaller than channels x sample width contradicts the fmt chunk; "
           "libsndfile derives the stride instead"}},
        {"wav-duplicate-fmt.wav",
         E{false, {}, false, "two fmt chunks that disagree contradict each other"}},
        {"wav-overlapping-chunks.wav",
         E{false, {}, {}, "fmt declares more than it holds, so the walk never reaches data"}},
        {"wav-riff-header-only.wav", E{false, {}, {}, "no WAVE header"}},
    };
    return rows;
}

} // namespace pulp_test::audio_decoder_corpus
