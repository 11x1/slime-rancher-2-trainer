// slime_dj: the one translation unit that compiles the vendored decoders (CMake target
// sr2_codecs, C, warnings off like the other third-party code). See codecs.h.

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb/stb_vorbis.c"

#define DR_MP3_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#include "../../third_party/dr_libs/dr_wav.h"

#include "codecs.h"

struct sr2_decoder {
    int codec;
    uint32_t channels;
    stb_vorbis* ogg;
    drmp3* mp3;
    drwav* wav;
};

static int ends_with_ci(const wchar_t* s, const wchar_t* suffix) {
    size_t n = wcslen(s), m = wcslen(suffix);
    return n >= m && _wcsicmp(s + n - m, suffix) == 0;
}

int sr2_codec_from_path(const wchar_t* path) {
    if (!path) return SR2_CODEC_NONE;
    if (ends_with_ci(path, L".ogg") || ends_with_ci(path, L".oga")) return SR2_CODEC_OGG;
    if (ends_with_ci(path, L".mp3")) return SR2_CODEC_MP3;
    if (ends_with_ci(path, L".wav") || ends_with_ci(path, L".wave")) return SR2_CODEC_WAV;
    return SR2_CODEC_NONE;
}

static void copy_text(char* dst, size_t cap, const char* src, size_t len) {
    if (!src || cap == 0) return;
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

// "KEY=value" Vorbis comment, key compared case-insensitively.
static void vorbis_tag(const char* comment, const char* key, char* dst, size_t cap) {
    size_t k = strlen(key);
    if (!comment || dst[0] || _strnicmp(comment, key, k) != 0 || comment[k] != '=') return;
    copy_text(dst, cap, comment + k + 1, strlen(comment + k + 1));
}

sr2_decoder* sr2_decoder_open(const wchar_t* path, int countFrames, sr2_audio_info* info) {
    sr2_audio_info local;
    sr2_decoder* d;
    if (!info) info = &local;
    memset(info, 0, sizeof(*info));
    info->codec = sr2_codec_from_path(path);
    if (info->codec == SR2_CODEC_NONE) return NULL;
    d = (sr2_decoder*)calloc(1, sizeof(sr2_decoder));
    if (!d) return NULL;
    d->codec = info->codec;

    if (d->codec == SR2_CODEC_OGG) {
        int err = 0, i;
        stb_vorbis_info vi;
        stb_vorbis_comment vc;
        FILE* f = _wfopen(path, L"rb");
        if (!f) goto fail;
        d->ogg = stb_vorbis_open_file(f, 1, &err, NULL);  // closes f on failure too
        if (!d->ogg) goto fail;
        vi = stb_vorbis_get_info(d->ogg);
        info->channels = (uint32_t)(vi.channels > 0 ? vi.channels : 0);
        info->sampleRate = vi.sample_rate;
        info->totalFrames = stb_vorbis_stream_length_in_samples(d->ogg);
        vc = stb_vorbis_get_comment(d->ogg);
        for (i = 0; i < vc.comment_list_length; ++i) {
            vorbis_tag(vc.comment_list[i], "TITLE", info->title, sizeof(info->title));
            vorbis_tag(vc.comment_list[i], "ARTIST", info->artist, sizeof(info->artist));
        }
        // stream_length_in_samples seeks to the end; make sure decoding starts at the beginning.
        stb_vorbis_seek_start(d->ogg);
    } else if (d->codec == SR2_CODEC_MP3) {
        d->mp3 = (drmp3*)calloc(1, sizeof(drmp3));
        if (!d->mp3) goto fail;
        if (!drmp3_init_file_w(d->mp3, path, NULL)) {
            free(d->mp3);
            d->mp3 = NULL;
            goto fail;
        }
        info->channels = d->mp3->channels;
        info->sampleRate = d->mp3->sampleRate;
        // Header-only scan (minimp3 without synthesis); restores the read position.
        if (countFrames) info->totalFrames = drmp3_get_pcm_frame_count(d->mp3);
    } else {
        drwav_uint32 i;
        d->wav = (drwav*)calloc(1, sizeof(drwav));
        if (!d->wav) goto fail;
        if (!drwav_init_file_with_metadata_w(d->wav, path, 0, NULL)) {
            free(d->wav);
            d->wav = NULL;
            goto fail;
        }
        info->channels = d->wav->channels;
        info->sampleRate = d->wav->sampleRate;
        info->totalFrames = d->wav->totalPCMFrameCount;
        for (i = 0; i < d->wav->metadataCount; ++i) {
            const drwav_metadata* m = &d->wav->pMetadata[i];
            if (m->type == drwav_metadata_type_list_info_title && !info->title[0])
                copy_text(info->title, sizeof(info->title), m->data.infoText.pString, m->data.infoText.stringLength);
            else if (m->type == drwav_metadata_type_list_info_artist && !info->artist[0])
                copy_text(info->artist, sizeof(info->artist), m->data.infoText.pString, m->data.infoText.stringLength);
        }
    }
    if (info->channels == 0 || info->sampleRate == 0) goto fail;
    d->channels = info->channels;
    return d;

fail:
    sr2_decoder_close(d);
    return NULL;
}

uint64_t sr2_decoder_read(sr2_decoder* d, float* out, uint64_t frames) {
    if (!d || !out || frames == 0) return 0;
    if (d->ogg) {
        uint64_t done = 0;
        while (done < frames) {
            uint64_t want = frames - done;
            int n;
            if (want > 1u << 20) want = 1u << 20;
            n = stb_vorbis_get_samples_float_interleaved(d->ogg, (int)d->channels, out + done * d->channels,
                                                         (int)(want * d->channels));
            if (n <= 0) break;
            done += (uint64_t)n;
        }
        return done;
    }
    if (d->mp3) return drmp3_read_pcm_frames_f32(d->mp3, frames, out);
    if (d->wav) return drwav_read_pcm_frames_f32(d->wav, frames, out);
    return 0;
}

int sr2_decoder_seek(sr2_decoder* d, uint64_t frame) {
    if (!d) return 0;
    if (d->ogg) return frame == 0 ? stb_vorbis_seek_start(d->ogg) : stb_vorbis_seek(d->ogg, (unsigned int)frame);
    if (d->mp3) return drmp3_seek_to_pcm_frame(d->mp3, frame) ? 1 : 0;
    if (d->wav) return drwav_seek_to_pcm_frame(d->wav, frame) ? 1 : 0;
    return 0;
}

void sr2_decoder_close(sr2_decoder* d) {
    if (!d) return;
    if (d->ogg) stb_vorbis_close(d->ogg);
    if (d->mp3) {
        drmp3_uninit(d->mp3);
        free(d->mp3);
    }
    if (d->wav) {
        drwav_uninit(d->wav);
        free(d->wav);
    }
    free(d);
}
