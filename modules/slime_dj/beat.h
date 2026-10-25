#pragma once
// slime_dj: beat analysis of the playing song, for "Walls dance to the music" (walls.cpp).
//
// The decoder thread (stream.cpp) feeds every decoded chunk in; the analysis keeps one compact
// entry per 10 ms hop of the song: the bass level (0..1, auto-gained) and the onset strength of a
// beat that starts in that hop (0 = none). The main thread reads the entries at the play
// position. The decoder runs seconds ahead of the play head, so the entries are always there in
// time. Memory: 4 bytes per hop (24 KB per minute of song).
//
// How (no libraries): mono mix, Hann window of ~23 ms (a power of two: 1024 frames at 44.1/48 kHz),
// one radix-2 FFT per hop. Bass level = energy of the 35-160 Hz bins in dB between a falling peak
// and a rising floor (8-24 dB window), so loud, compressed music still swings 0..1 on every kick.
// Onsets = half spectral flux (rise of log magnitudes up to ~3 kHz, bass bins x3), half rise of the
// bass level, above an adaptive threshold (running mean + 1.5 deviations), local maximum, at least
// 180 ms apart; strength against the recent strongest onsets.
// Energy (0..1, round 11): how loud this part of the song is against its loud parts, so a chill
// section does not dance like a drop although the relative bass level still swings 0..1. Bass and
// broadband loudness, each a ~1 s power average in dB, against the song's loudest so far (falls
// very slowly) and an absolute reference (so a quiet intro is calm before the drop has been heard);
// the lower of the two (bass, broadband) counts.
//
// No game calls. Feed and Reset: one thread (the decoder); Query: any thread.

#include <cstdint>
#include <mutex>
#include <vector>

namespace sr2::modules::slime_dj {

class BeatTrack {
public:
    static constexpr double kHopSeconds = 0.01;

    struct Sample {
        float level = 0;  // bass level 0..1 at the end frame
        float beat = 0;   // strongest onset in the range, 0..1 (0 = no beat)
        float energy = 1; // loudness of this part of the song, 0 (chill) .. 1 (loud part)
    };

    // Decoder thread, before the first Feed: song rate and the song frame decoding starts at.
    void Reset(uint32_t rate, uint64_t startFrame);
    // Decoder thread: interleaved float PCM (1 or 2 channels), in song order.
    void Feed(const float* pcm, uint32_t frames, uint32_t channels);
    // Song frames (from frame 0) [from, to]: level at `to`, strongest onset after `from` up to `to`.
    // False while `to` is not analysed yet (or before the start frame).
    bool Query(uint64_t fromFrame, uint64_t toFrame, Sample& out) const;
    size_t Hops() const;  // analysed hops (for the log)
    size_t Beats() const; // onsets found so far

private:
    void Hop();

    // decoder-thread state
    uint32_t rate_ = 0, hopFrames_ = 0, n_ = 0;  // n_: FFT size
    std::vector<float> ring_;                     // last n_ mono samples (circular)
    size_t ringPos_ = 0;
    uint32_t sinceHop_ = 0;
    std::vector<float> window_, re_, im_, prevMag_;
    std::vector<uint32_t> bitrev_;
    std::vector<float> cosT_, sinT_;
    uint32_t bassLo_ = 1, bassHi_ = 2, fluxHi_ = 64;
    float peakDb_ = -200.0f, slowPeakDb_ = -200.0f, floorDb_ = -200.0f, prevBassDb_ = -200.0f;
    float onsetPeak_ = 0;          // the recent strongest onset (falls slowly)
    float fluxMean_ = 0, fluxVar_ = 0;
    float flux1_ = 0, flux2_ = 0;  // flux of the previous two hops (peak picking)
    float thr1_ = 0;               // threshold at the previous hop
    int sinceOnset_ = 1000;        // hops since the last onset
    uint64_t warm_ = 0;            // hops analysed (the threshold needs ~0.5 s to settle)
    double hopSumSq_ = 0;          // broadband: sum of squared mono samples in this hop
    float secBass_ = 0, secBroad_ = 0;  // ~1 s power averages (bass bins, broadband)
    float songBassDb_ = -200.0f, songBroadDb_ = -200.0f;  // their loudest so far (falling slowly)

    // shared
    mutable std::mutex mu_;
    uint64_t firstHop_ = 0;        // song hop of entries_[0]
    uint64_t hopFramesShared_ = 0;
    std::vector<uint32_t> entries_;  // byte 0 level, byte 1 onset, byte 2 energy
    size_t beats_ = 0;
};

}  // namespace sr2::modules::slime_dj
