# Applies one reviewed replacement to the header text held in <text_var>.
# Text that already carries the replacement is accepted unchanged, so a second
# configure is a no-op; text that carries neither form stops the configure,
# because a silently skipped patch leaves a header that compiles but misbehaves.
function(_pulp_choc_apply_patch text_var patch_name original patched)
    string(FIND "${${text_var}}" "${patched}" _pulp_choc_patch_done)
    if(NOT _pulp_choc_patch_done EQUAL -1)
        return()
    endif()
    string(FIND "${${text_var}}" "${original}" _pulp_choc_patch_at)
    if(_pulp_choc_patch_at EQUAL -1)
        message(FATAL_ERROR
            "Pulp: CHOC ${patch_name} no longer matches the reviewed source; update or retire Pulp's patch")
    endif()
    string(REPLACE "${original}" "${patched}" _pulp_choc_patch_text "${${text_var}}")
    set(${text_var} "${_pulp_choc_patch_text}" PARENT_SCOPE)
endfunction()

function(pulp_patch_choc_v8 choc_source_dir)
    set(_pulp_choc_v8_header "${choc_source_dir}/choc/javascript/choc_javascript_V8.h")

    # Pulp's pinned CHOC always ships this header and the patch runs for every
    # configure, so a missing header means the pin moved, not that V8 is off.
    if(NOT EXISTS "${_pulp_choc_v8_header}")
        message(FATAL_ERROR "Pulp: CHOC V8 header not found for compatibility patch: ${_pulp_choc_v8_header}")
    endif()

    file(READ "${_pulp_choc_v8_header}" _pulp_choc_v8)
    set(_pulp_choc_v8_original "${_pulp_choc_v8}")

    _pulp_choc_apply_patch(_pulp_choc_v8 "V8 version include"
"#include \"choc_javascript.h\""
"#include \"choc_javascript.h\"
#include <v8-version.h>")

    # V8 14 removed Context::GetIsolate and requires explicit lambda return
    # types for the module callbacks.
    _pulp_choc_apply_patch(_pulp_choc_v8 "V8 dynamic-import callback"
"        isolate->SetHostImportModuleDynamicallyCallback (+[] (v8::Local<v8::Context> c,
                                                              v8::Local<v8::Data> /*hostOptions*/,
                                                              v8::Local<v8::Value> /*resourceName*/,
                                                              v8::Local<v8::String> specifier,
                                                              v8::Local<v8::FixedArray> /*importAssertions*/)
        {
            return static_cast<V8Context*> (c->GetIsolate()->GetData (0))
                ->loadDynamicModule (c, specifier);
        });"
"        isolate->SetHostImportModuleDynamicallyCallback (+[] (v8::Local<v8::Context> c,
                                                              v8::Local<v8::Data> /*hostOptions*/,
                                                              v8::Local<v8::Value> /*resourceName*/,
                                                              v8::Local<v8::String> specifier,
                                                              v8::Local<v8::FixedArray> /*importAssertions*/)
            -> v8::MaybeLocal<v8::Promise>
        {
            return static_cast<V8Context*> (v8::Isolate::GetCurrent()->GetData (0))
                ->loadDynamicModule (c, specifier);
        });")

    # The replacement keeps the original as its #else branch, so the
    # already-patched check must see the whole guarded block.
    _pulp_choc_apply_patch(_pulp_choc_v8 "V8 UTF-8 string conversion"
"        auto len = s.Utf8Length (isolate);
        result.resize (static_cast<std::string::size_type> (len));
        s.WriteUtf8 (isolate, result.data(), len);"
"       #if defined(V8_MAJOR_VERSION) && V8_MAJOR_VERSION >= 14
        auto len = s.Utf8LengthV2 (isolate);
        result.resize (static_cast<std::string::size_type> (len));
        s.WriteUtf8V2 (isolate, result.data(), static_cast<size_t> (len), v8::String::WriteFlags::kNone);
       #else
        auto len = s.Utf8Length (isolate);
        result.resize (static_cast<std::string::size_type> (len));
        s.WriteUtf8 (isolate, result.data(), len);
       #endif")

    _pulp_choc_apply_patch(_pulp_choc_v8 "V8 static-module instantiate callback"
"        auto ok = mod.ToLocalChecked()->InstantiateModule (localContext, +[] (v8::Local<v8::Context> c, v8::Local<v8::String> specifier,
                                                                              v8::Local<v8::FixedArray>, v8::Local<v8::Module>)
        {
            return static_cast<V8Context*> (c->GetIsolate()->GetData (0))
                ->loadStaticModule (c, specifier);
        });"
"        auto ok = mod.ToLocalChecked()->InstantiateModule (localContext, +[] (v8::Local<v8::Context> c, v8::Local<v8::String> specifier,
                                                                              v8::Local<v8::FixedArray>, v8::Local<v8::Module>)
            -> v8::MaybeLocal<v8::Module>
        {
            return static_cast<V8Context*> (v8::Isolate::GetCurrent()->GetData (0))
                ->loadStaticModule (c, specifier);
        });")

    # v8ToChoc collects property values inside an Array::Iterate callback and
    # converts them after it returns. Since V8 15.4 Iterate runs the callback in
    # its own HandleScope, so those Locals dangle and the next conversion reads
    # freed handle slots (SIGBUS in KeyAccumulator). Iterate also forbids the JS
    # execution a getter can trigger through Get(). Walk the names by index.
    _pulp_choc_apply_patch(_pulp_choc_v8 "V8 v8ToChoc property walk"
"                using IteratorType = decltype (iterator);

                propNames.ToLocalChecked()->Iterate (c, +[] (uint32_t, v8::Local<v8::Value> key, void* i)
                {
                    (*static_cast<IteratorType*> (i)) (key);
                    return v8::Array::CallbackResult::kContinue;
                },
                std::addressof (iterator));"
"                auto names = propNames.ToLocalChecked();

                for (uint32_t i = 0; i < names->Length(); ++i)
                {
                    auto key = names->Get (c, i);

                    if (! key.IsEmpty())
                        iterator (key.ToLocalChecked());
                }")

    if(NOT _pulp_choc_v8 STREQUAL _pulp_choc_v8_original)
        file(WRITE "${_pulp_choc_v8_header}" "${_pulp_choc_v8}")
        message(STATUS "Pulp: applied CHOC V8 compatibility patch")
    endif()
endfunction()

function(pulp_patch_choc_json_number_exponent choc_source_dir)
    set(_pulp_choc_json_header
        "${choc_source_dir}/choc/text/choc_JSON.h")
    if(NOT EXISTS "${_pulp_choc_json_header}")
        message(FATAL_ERROR
            "Pulp: CHOC JSON header not found for standards patch: ${_pulp_choc_json_header}")
    endif()

    file(READ "${_pulp_choc_json_header}" _pulp_choc_json)
    set(_pulp_choc_json_original
"                    hadExponent = true;
                    popIf ('-');
                    continue;")
    set(_pulp_choc_json_patched
"                    hadExponent = true;
                    if (! popIf ('-'))
                        popIf ('+');
                    continue;")

    string(FIND "${_pulp_choc_json}" "${_pulp_choc_json_patched}"
        _pulp_choc_json_patched_position)
    if(NOT _pulp_choc_json_patched_position EQUAL -1)
        return()
    endif()

    string(FIND "${_pulp_choc_json}" "${_pulp_choc_json_original}"
        _pulp_choc_json_original_position)
    if(_pulp_choc_json_original_position EQUAL -1)
        message(FATAL_ERROR
            "Pulp: CHOC JSON exponent parser no longer matches the reviewed source; update or retire Pulp's standards patch")
    endif()

    string(REPLACE "${_pulp_choc_json_original}" "${_pulp_choc_json_patched}"
        _pulp_choc_json "${_pulp_choc_json}")
    file(WRITE "${_pulp_choc_json_header}" "${_pulp_choc_json}")
    message(STATUS "Pulp: applied CHOC JSON positive-exponent patch")
endfunction()

# choc::file::Watcher on macOS stops its FSEventStream in ~Pimpl, but
# FSEventStreamStop and FSEventStreamInvalidate only prevent new callbacks; one
# already running on the stream's dispatch queue keeps going and calls
# watcher.callback on a Watcher that is being freed (an ASan heap-use-after-free
# in Pimpl::sendEvent under HotReloader). The patch drains the serial queue
# after invalidating, which waits for that callback, and tags the queue so a
# Watcher destroyed from inside its own callback skips the drain instead of
# deadlocking.
function(pulp_patch_choc_file_watcher choc_source_dir)
    set(_pulp_choc_watcher_header "${choc_source_dir}/choc/platform/choc_FileWatcher.h")
    if(NOT EXISTS "${_pulp_choc_watcher_header}")
        message(FATAL_ERROR
            "Pulp: CHOC file watcher header not found for lifetime patch: ${_pulp_choc_watcher_header}")
    endif()

    file(READ "${_pulp_choc_watcher_header}" _pulp_choc_watcher)
    set(_pulp_choc_watcher_original "${_pulp_choc_watcher}")

    _pulp_choc_apply_patch(_pulp_choc_watcher "file watcher queue tag"
[==[        FSEventStreamSetDispatchQueue (eventStream, dispatchQueue);
        FSEventStreamStart (eventStream);]==]
[==[        // Tags the queue so ~Pimpl can tell when it runs on it.
        dispatch_queue_set_specific (dispatchQueue, this, this, nullptr);
        FSEventStreamSetDispatchQueue (eventStream, dispatchQueue);
        FSEventStreamStart (eventStream);]==])

    _pulp_choc_apply_patch(_pulp_choc_watcher "file watcher drain on destruction"
[==[            FSEventStreamStop (eventStream);
            FSEventStreamInvalidate (eventStream);
            FSEventStreamRelease (eventStream);]==]
[==[            FSEventStreamStop (eventStream);
            FSEventStreamInvalidate (eventStream);
            // Stop and Invalidate schedule no new callbacks but do not wait for
            // one already running on the queue, which reads the Watcher being
            // destroyed. Draining the serial queue waits for it, except when
            // this destructor runs on that queue itself.
            if (dispatchQueue && dispatch_get_specific (this) != this)
                dispatch_sync_f (dispatchQueue, nullptr, [] (void*) {});
            FSEventStreamRelease (eventStream);]==])

    if(NOT _pulp_choc_watcher STREQUAL _pulp_choc_watcher_original)
        file(WRITE "${_pulp_choc_watcher_header}" "${_pulp_choc_watcher}")
        message(STATUS "Pulp: applied CHOC file watcher lifetime patch")
    endif()
endfunction()

# WAVE_FORMAT_EXTENSIBLE carries two bit counts: wBitsPerSample is the
# container width, which is the sample width the data chunk is laid out in,
# and wValidBitsPerSample only says how many of those bits carry audio (the
# rest are zero). CHOC derives the integer depth from the valid-bits field
# while keeping the container stride, so a 24-valid-bits-in-32-bit-container
# file is read as packed 24-bit at a 4-byte stride: three of every four bytes,
# off by one byte, 256x too loud and sign-garbled. dr_wav and libsndfile scale
# the container value. The patch reads the field and keeps the container width.
# Consequence: read_audio_file_info().bits_per_sample reports the container
# width (32 for a 24-valid-bits-in-32 file, as inspect_wav already does),
# not the valid-bits count.
function(pulp_patch_choc_wav_extensible_container_bits choc_source_dir)
    set(_pulp_choc_wav_header
        "${choc_source_dir}/choc/audio/choc_AudioFileFormat_WAV.h")
    if(NOT EXISTS "${_pulp_choc_wav_header}")
        message(FATAL_ERROR
            "Pulp: CHOC WAV header not found for the extensible container-width patch: ${_pulp_choc_wav_header}")
    endif()

    file(READ "${_pulp_choc_wav_header}" _pulp_choc_wav)
    _pulp_choc_apply_patch(_pulp_choc_wav "WAV extensible container width"
"                    bitsPerSample = readInt<uint16_t>();
                    properties.speakers = getSpeakers (readInt<uint32_t>());"
"                    (void) readInt<uint16_t>(); // wValidBitsPerSample: the container width stays the sample width
                    properties.speakers = getSpeakers (readInt<uint32_t>());")
    file(WRITE "${_pulp_choc_wav_header}" "${_pulp_choc_wav}")
    message(STATUS "Pulp: applied CHOC WAV extensible container-width patch")
endfunction()

# A WAV fmt chunk carries both nBlockAlign (the frame stride) and the channel
# count and sample width that imply it. CHOC trusts a non-zero nBlockAlign
# and only derives it when it is zero, so a header declaring two 8-bit
# channels with a block of one byte makes the reader step through the data
# one byte per frame while copying two samples per frame: the last frame of
# each 2048-byte raw block reads past the stack buffer (an AddressSanitizer
# stack-buffer-overflow found by the audio decoder fuzz target). A block
# smaller than its channels' samples cannot be laid out, so the patch refuses
# it; a larger block (padding) stays legal.
function(pulp_patch_choc_wav_block_align_bounds choc_source_dir)
    set(_pulp_choc_wav_header
        "${choc_source_dir}/choc/audio/choc_AudioFileFormat_WAV.h")
    if(NOT EXISTS "${_pulp_choc_wav_header}")
        message(FATAL_ERROR
            "Pulp: CHOC WAV header not found for the block-align patch: ${_pulp_choc_wav_header}")
    endif()

    file(READ "${_pulp_choc_wav_header}" _pulp_choc_wav)
    _pulp_choc_apply_patch(_pulp_choc_wav "WAV block-align bounds"
"            if (frameStride == 0)
                frameStride = properties.numChannels * (bitsPerSample / 8u);

            sampleStride = bitsPerSample / 8u;"
"            sampleStride = bitsPerSample / 8u;

            if (frameStride == 0)
                frameStride = properties.numChannels * sampleStride;
            else if (frameStride < properties.numChannels * sampleStride)
                throwFormatException(); // a block smaller than its channels' samples cannot be laid out")
    file(WRITE "${_pulp_choc_wav_header}" "${_pulp_choc_wav}")
    message(STATUS "Pulp: applied CHOC WAV block-align bounds patch")
endfunction()

# CHOC's WAV metadata readers size their string reads from the chunk header:
# readStringToEndOfChunk computes "chunk end minus position" as unsigned, so a
# chunk whose fixed fields have already run past its declared end (a bext
# chunk shorter than BWAV's 602 fixed bytes) wraps to about 4 GiB and
# std::string::resize asks for it before a byte is read (a libFuzzer
# out-of-memory finding). The patch bounds every metadata read to the bytes
# the stream still holds and to the chunk it belongs to, and keeps a bext
# chunk shorter than its fixed fields as opaque bytes instead of parsing it.
function(pulp_patch_choc_wav_bounded_metadata_reads choc_source_dir)
    set(_pulp_choc_wav_header
        "${choc_source_dir}/choc/audio/choc_AudioFileFormat_WAV.h")
    if(NOT EXISTS "${_pulp_choc_wav_header}")
        message(FATAL_ERROR
            "Pulp: CHOC WAV header not found for the bounded-metadata patch: ${_pulp_choc_wav_header}")
    endif()

    file(READ "${_pulp_choc_wav_header}" _pulp_choc_wav)
    _pulp_choc_apply_patch(_pulp_choc_wav "WAV bounded readString"
"        std::string readString (uint32_t size)
        {
            std::string s;
            s.resize (size);"
"        std::string readString (uint32_t size)
        {
            auto pos = getPosition(); // never size a read past the bytes the stream still holds
            if (pos >= streamEndPosition)
                return {};
            size = static_cast<uint32_t> (std::min<uint64_t> (size, streamEndPosition - pos));
            std::string s;
            s.resize (size);")
    _pulp_choc_apply_patch(_pulp_choc_wav "WAV bounded readStringToEndOfChunk"
"        std::string readStringToEndOfChunk (ChunkRange range)
        {
            return readString (static_cast<uint32_t> (range.getEnd() - getPosition()));
        }"
"        std::string readStringToEndOfChunk (ChunkRange range)
        {
            auto pos = getPosition(); // a chunk whose fixed fields overran it has nothing left to read
            if (pos >= range.getEnd())
                return {};
            return readString (static_cast<uint32_t> (std::min<uint64_t> (range.getEnd() - pos, 0xffffffffu)));
        }")
    _pulp_choc_apply_patch(_pulp_choc_wav "WAV bounded readIntoBase64"
"        template <typename Size>
        std::string readIntoBase64 (Size size)
        {
            std::vector<char> data;
            data.resize (static_cast<size_t>  (size));"
"        template <typename Size>
        std::string readIntoBase64 (Size size)
        {
            auto pos = getPosition(); // never size a read past the bytes the stream still holds
            auto remaining = pos < streamEndPosition ? streamEndPosition - pos : 0u;
            size = static_cast<Size> (std::min<uint64_t> (static_cast<uint64_t> (size), remaining));
            std::vector<char> data;
            data.resize (static_cast<size_t>  (size));")
    _pulp_choc_apply_patch(_pulp_choc_wav "WAV bext minimum size"
"        void readMetadata_BWAV (std::string type, ChunkRange chunkRange)
        {
            auto readUMID = [this]"
"        void readMetadata_BWAV (std::string type, ChunkRange chunkRange)
        {
            if (chunkRange.length < 602) // shorter than BWAV's fixed fields: keep the bytes, do not parse them
            {
                readMetadata_Unknown (type, chunkRange);
                return;
            }

            auto readUMID = [this]")
    file(WRITE "${_pulp_choc_wav_header}" "${_pulp_choc_wav}")
    message(STATUS "Pulp: applied CHOC WAV bounded metadata reads patch")
endfunction()
