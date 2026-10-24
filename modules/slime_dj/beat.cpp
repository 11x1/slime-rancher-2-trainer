#include "beat.h"

#include <algorithm>
#include <cmath>

namespace sr2::modules::slime_dj {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kWindowSeconds = 0.023f;  // FFT window, rounded up to a power of two
constexpr float kBassLoHz = 35.0f, kBassHiHz = 160.0f, kFluxHiHz = 3000.0f;
constexpr float kBassWeight = 3.0f;       // bass bins count this much more in the flux
constexpr float kBassRiseShare = 0.5f;    // detection function: this share from the bass level rise, the rest flux
constexpr float kBassRiseDb = 12.0f;      // a bass rise of this many dB in one hop counts 1
constexpr float kSlowFallDbPerSec = 2.0f; // slow peak of the bass level (silence gate)
constexpr float kSilenceDb = -60.0f;      // slow peak below this: level 0 (silence, fade-in)
constexpr float kGateDb = 30.0f;          // more than this under the slow peak: level 0 (breaks)
constexpr float kPeakFallDbPerSec = 6.0f; // fast peak of the bass level
constexpr float kFloorRiseDbPerSec = 10.0f;  // floor of the bass level (follows the dips between kicks)
constexpr float kMinSpanDb = 8.0f, kMaxSpanDb = 24.0f;  // level 0..1 spans peak - floor, within these
constexpr float kStatAlpha = 1.0f / 50;   // running flux mean / variance (~0.5 s)
constexpr float kThrDeviations = 1.5f;
constexpr float kMinFlux = 0.01f;         // absolute floor of the threshold
constexpr int kMinGapHops = 18;           // onsets at least 180 ms apart (333 BPM)
constexpr float kOnsetPeakFall = 0.33f;   // the recent strongest onset falls this fraction per second
constexpr uint64_t kWarmHops = 30;        // no onsets in the first 0.3 s (statistics settle)
constexpr size_t kMaxHops = 6u * 3600u * 100u;  // 6 hours (2 bytes each)
}  // namespace

void BeatTrack::Reset(uint32_t rate, uint64_t startFrame) {
    rate_ = std::max<uint32_t>(rate, 1000);
    hopFrames_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(rate_ * kHopSeconds)));
    n_ = 256;
    while (n_ < 4096 && static_cast<float>(n_) < rate_ * kWindowSeconds) n_ <<= 1;
    ring_.assign(n_, 0.0f);
    ringPos_ = 0;
    sinceHop_ = 0;
    window_.resize(n_);
    for (uint32_t i = 0; i < n_; ++i) window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(n_));
    re_.assign(n_, 0.0f);
    im_.assign(n_, 0.0f);
    prevMag_.assign(n_ / 2, 0.0f);
    bitrev_.resize(n_);
    uint32_t bits = 0;
    while ((1u << bits) < n_) ++bits;
    for (uint32_t i = 0; i < n_; ++i) {
        uint32_t r = 0;
        for (uint32_t b = 0; b < bits; ++b)
            if (i & (1u << b)) r |= 1u << (bits - 1 - b);
        bitrev_[i] = r;
    }
    cosT_.resize(n_ / 2);
    sinT_.resize(n_ / 2);
    for (uint32_t i = 0; i < n_ / 2; ++i) {
        cosT_[i] = std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(n_));
        sinT_[i] = -std::sin(2.0f * kPi * static_cast<float>(i) / static_cast<float>(n_));
    }
    const float binHz = static_cast<float>(rate_) / static_cast<float>(n_);
    bassLo_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kBassLoHz / binHz)));
    bassHi_ = std::max(bassLo_, static_cast<uint32_t>(kBassHiHz / binHz));
    fluxHi_ = std::clamp<uint32_t>(static_cast<uint32_t>(kFluxHiHz / binHz), bassHi_, n_ / 2 - 1);
    peakDb_ = slowPeakDb_ = floorDb_ = prevBassDb_ = -200.0f;
    onsetPeak_ = 0;
    fluxMean_ = fluxVar_ = 0;
    flux1_ = flux2_ = thr1_ = 0;
    sinceOnset_ = 1000;
    warm_ = 0;
    std::lock_guard lock(mu_);
    hopFramesShared_ = hopFrames_;
    firstHop_ = startFrame / hopFrames_;
    entries_.clear();
    entries_.reserve(static_cast<size_t>(60.0 / kHopSeconds) * 4);  // 4 minutes, grows if needed
    beats_ = 0;
}

void BeatTrack::Feed(const float* pcm, uint32_t frames, uint32_t channels) {
    if (!n_ || !pcm || channels == 0) return;
    for (uint32_t i = 0; i < frames; ++i) {
        const float* f = pcm + static_cast<size_t>(i) * channels;
        ring_[ringPos_] = channels >= 2 ? 0.5f * (f[0] + f[1]) : f[0];
        if (++ringPos_ == n_) ringPos_ = 0;
        if (++sinceHop_ == hopFrames_) {
            sinceHop_ = 0;
            Hop();
        }
    }
}

void BeatTrack::Hop() {
    // Windowed copy of the last n_ samples (oldest first), in bit-reversed order for the FFT.
    for (uint32_t i = 0; i < n_; ++i) {
        const float s = ring_[(ringPos_ + i) % n_] * window_[i];
        re_[bitrev_[i]] = s;
        im_[bitrev_[i]] = 0.0f;
    }
    // Iterative radix-2 FFT.
    for (uint32_t len = 2; len <= n_; len <<= 1) {
        const uint32_t half = len >> 1, step = n_ / len;
        for (uint32_t s = 0; s < n_; s += len) {
            for (uint32_t k = 0; k < half; ++k) {
                const float wr = cosT_[k * step], wi = sinT_[k * step];
                const uint32_t a = s + k, b = a + half;
                const float tr = re_[b] * wr - im_[b] * wi;
                const float ti = re_[b] * wi + im_[b] * wr;
                re_[b] = re_[a] - tr;
                im_[b] = im_[a] - ti;
                re_[a] += tr;
                im_[a] += ti;
            }
        }
    }
    // Magnitudes as sine amplitudes (Hann coherent gain 0.5).
    const float norm = 4.0f / static_cast<float>(n_);
    float bass = 0, flux = 0, weights = 0;
    for (uint32_t k = 1; k <= fluxHi_; ++k) {
        const float m = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]) * norm;
        const bool isBass = k >= bassLo_ && k <= bassHi_;
        if (isBass) bass += m * m;
        const float l = std::log1p(100.0f * m);
        const float w = isBass ? kBassWeight : 1.0f;
        flux += w * std::max(0.0f, l - prevMag_[k]);
        weights += w;
        prevMag_[k] = l;
    }
    flux /= std::max(weights, 1.0f);

    // Bass level between a falling peak and a rising floor.
    const float hop = static_cast<float>(kHopSeconds);
    const float db = 10.0f * std::log10(bass + 1e-10f);
    slowPeakDb_ = std::max(db, slowPeakDb_ - kSlowFallDbPerSec * hop);
    peakDb_ = std::max(db, peakDb_ - kPeakFallDbPerSec * hop);
    floorDb_ = std::max(std::min(db, floorDb_ + kFloorRiseDbPerSec * hop), peakDb_ - kMaxSpanDb);
    float level = 0;
    if (slowPeakDb_ > kSilenceDb && db > slowPeakDb_ - kGateDb) {
        const float span = std::clamp(peakDb_ - floorDb_, kMinSpanDb, kMaxSpanDb);
        level = std::clamp((db - (peakDb_ - span)) / span, 0.0f, 1.0f);
    }
    // Detection function: spectral flux, mixed with the rise of the bass level (kicks).
    const float rise = std::max(0.0f, db - prevBassDb_) / kBassRiseDb;
    prevBassDb_ = std::max(db, slowPeakDb_ - kGateDb);
    flux = (1.0f - kBassRiseShare) * flux + kBassRiseShare * rise;

    // Onset at the previous hop: a local flux maximum above the threshold of its time.
    const float thr = std::max(kMinFlux, fluxMean_ + kThrDeviations * std::sqrt(std::max(fluxVar_, 0.0f)));
    const float d = flux - fluxMean_;
    fluxMean_ += kStatAlpha * d;
    fluxVar_ = (1.0f - kStatAlpha) * (fluxVar_ + kStatAlpha * d * d);
    ++warm_;
    ++sinceOnset_;
    uint8_t onsetPrev = 0;
    onsetPeak_ *= 1.0f - kOnsetPeakFall * hop;
    if (warm_ > kWarmHops && flux1_ > thr1_ && flux1_ > flux2_ && flux1_ >= flux && sinceOnset_ > kMinGapHops) {
        onsetPeak_ = std::max(onsetPeak_, flux1_);
        // Strength against the recent strongest onsets: on loud, even kicks (hardtekk) every kick is
        // near 1 (against the threshold they were all ~0.4: "chill").
        const float strength = std::clamp((flux1_ - thr1_) / std::max(onsetPeak_ - thr1_, 1e-6f), 0.0f, 1.0f);
        onsetPrev = static_cast<uint8_t>(std::lround(255.0f * (0.3f + 0.7f * strength)));
        sinceOnset_ = 1;  // counted from the previous hop
    }
    flux2_ = flux1_;
    flux1_ = flux;
    thr1_ = thr;

    std::lock_guard lock(mu_);
    if (onsetPrev && !entries_.empty()) {
        entries_.back() = static_cast<uint16_t>((entries_.back() & 0xFF) | (onsetPrev << 8));
        ++beats_;
    }
    if (entries_.size() < kMaxHops) entries_.push_back(static_cast<uint16_t>(std::lround(level * 255.0f)));
}

bool BeatTrack::Query(uint64_t fromFrame, uint64_t toFrame, Sample& out) const {
    std::lock_guard lock(mu_);
    if (!hopFramesShared_ || entries_.empty()) return false;
    const uint64_t toHop = toFrame / hopFramesShared_;
    if (toHop < firstHop_ || toHop - firstHop_ >= entries_.size()) return false;
    const size_t to = static_cast<size_t>(toHop - firstHop_);
    out.level = static_cast<float>(entries_[to] & 0xFF) / 255.0f;
    out.beat = 0;
    // (fromFrame, toFrame]; at most 0.5 s back (a seek or a long frame)
    // (no movement or a step back: no beat, so a paused song does not repeat its last one)
    if (fromFrame >= toFrame) return true;
    uint64_t fromHop = fromFrame / hopFramesShared_ + 1;
    fromHop = std::max({fromHop, firstHop_, toHop >= 50 ? toHop - 50 : uint64_t{0}});
    for (uint64_t h = fromHop; h <= toHop; ++h)
        out.beat = std::max(out.beat, static_cast<float>(entries_[static_cast<size_t>(h - firstHop_)] >> 8) / 255.0f);
    return true;
}

size_t BeatTrack::Hops() const {
    std::lock_guard lock(mu_);
    return entries_.size();
}

size_t BeatTrack::Beats() const {
    std::lock_guard lock(mu_);
    return beats_;
}

}  // namespace sr2::modules::slime_dj
