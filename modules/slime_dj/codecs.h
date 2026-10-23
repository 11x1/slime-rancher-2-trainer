#pragma once
// slime_dj: the compiled-in song decoders behind one small C interface (codecs.c: stb_vorbis,
// dr_mp3, dr_wav). Streaming: a decoder holds the open file and hands out interleaved float
// PCM in pieces, so a song never has to be decoded into memory as a whole.
//
// Thread-safe as long as one decoder is used by one thread at a time. No game calls.

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sr2_codec { SR2_CODEC_NONE = 0, SR2_CODEC_OGG = 1, SR2_CODEC_MP3 = 2, SR2_CODEC_WAV = 3 };

typedef struct sr2_audio_info {
    int codec;             // sr2_codec
    uint32_t channels;     // as stored in the file (1..8 typically)
    uint32_t sampleRate;   // Hz
    uint64_t totalFrames;  // PCM frames (samples per channel); 0 = unknown
    char title[256];       // UTF-8 from Vorbis comments / WAV LIST INFO ("" = none; MP3: never, see id3)
    char artist[256];
} sr2_audio_info;

typedef struct sr2_decoder sr2_decoder;

// Codec from the file extension (.ogg/.oga, .mp3, .wav/.wave), else SR2_CODEC_NONE.
int sr2_codec_from_path(const wchar_t* path);

// Opens `path` for streaming. `countFrames`: also determine totalFrames when the container
// does not store it (MP3: a header-only scan of the whole file, then back to the start).
// nullptr on failure (unsupported, unreadable or corrupt file).
sr2_decoder* sr2_decoder_open(const wchar_t* path, int countFrames, sr2_audio_info* info);
// Reads up to `frames` frames of interleaved float PCM (info.channels per frame) into `out`.
// Returns the frames read; fewer than asked (0 at the end) = end of stream or a decode error.
uint64_t sr2_decoder_read(sr2_decoder* d, float* out, uint64_t frames);
void sr2_decoder_close(sr2_decoder* d);

#ifdef __cplusplus
}
#endif
