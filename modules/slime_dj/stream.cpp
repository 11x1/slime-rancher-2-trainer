#include "stream.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include "core/log.h"
#include "modules/slime_dj/codecs.h"
#include "modules/slime_dj/library.h"

namespace sr2::modules::slime_dj {

std::shared_ptr<Stream> Stream::Start(const std::filesystem::path& path, uint64_t startFrame) {
    std::shared_ptr<Stream> s(new Stream());
    // The thread keeps the stream alive while it runs; the owner ends it with Stop().
    std::thread([w = std::weak_ptr<Stream>(s), path, startFrame] {
        if (auto self = w.lock()) self->Run(path, startFrame);
    }).detach();
    return s;
}

Stream::~Stream() { Stop(); }

std::string Stream::error() const {
    std::lock_guard lock(mu_);
    return error_;
}

void Stream::Stop() {
    stop_.store(true);
    cv_.notify_all();
}

bool Stream::Pop(std::vector<float>& out, uint32_t& frames) {
    std::lock_guard lock(mu_);
    if (queue_.empty()) return false;
    out.swap(queue_.front().pcm);
    frames = queue_.front().frames;
    queue_.pop_front();
    cv_.notify_all();
    return true;
}

bool Stream::Done() const {
    std::lock_guard lock(mu_);
    return finished_.load() && queue_.empty();
}

size_t Stream::Queued() const {
    std::lock_guard lock(mu_);
    return queue_.size();
}

void Stream::Run(std::filesystem::path path, uint64_t startFrame) {
    // Runs with a strong reference (taken by the thread lambda) for its whole life, so the owner
    // must call Stop() before letting go; the wait below wakes up on stop_.
    sr2_audio_info info{};
    sr2_decoder* d = sr2_decoder_open(path.c_str(), 0, &info);
    if (!d) {
        std::lock_guard lock(mu_);
        error_ = "can't be decoded (unsupported or damaged file)";
        state_.store(State::Failed);
        return;
    }
    const uint32_t inCh = info.channels;
    channels_ = inCh >= 2 ? 2 : 1;
    rate_ = info.sampleRate;
    chunkFrames_ = std::max<uint32_t>(1024, static_cast<uint32_t>(std::lround(rate_ * kChunkSeconds)));
    total_.store(info.totalFrames);
    if (startFrame && !sr2_decoder_seek(d, startFrame)) {
        sr2_decoder_close(d);
        std::lock_guard lock(mu_);
        error_ = "can't seek in this file";
        state_.store(State::Failed);
        return;
    }
    state_.store(State::Ready);

    std::vector<float> raw(static_cast<size_t>(chunkFrames_) * inCh);
    uint64_t decoded = 0;
    for (;;) {
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return stop_.load() || queue_.size() < kQueueChunks; });
            if (stop_.load()) break;
        }
        const uint64_t got = sr2_decoder_read(d, raw.data(), chunkFrames_);
        if (got == 0) break;
        Chunk c;
        c.frames = static_cast<uint32_t>(got);
        c.pcm.assign(static_cast<size_t>(chunkFrames_) * channels_, 0.0f);
        if (inCh == channels_) {
            std::copy_n(raw.data(), static_cast<size_t>(got) * inCh, c.pcm.data());
        } else {
            // More than two channels (WAV/Vorbis order: L, R, C, ...): front pair plus the centre.
            for (uint64_t i = 0; i < got; ++i) {
                const float* f = &raw[static_cast<size_t>(i) * inCh];
                const float centre = inCh >= 3 ? f[2] * 0.7071f : 0.0f;
                c.pcm[static_cast<size_t>(i) * 2] = std::clamp(f[0] + centre, -1.0f, 1.0f);
                c.pcm[static_cast<size_t>(i) * 2 + 1] = std::clamp(f[1] + centre, -1.0f, 1.0f);
            }
        }
        decoded += got;
        {
            std::lock_guard lock(mu_);
            queue_.push_back(std::move(c));
        }
        if (got < chunkFrames_) break;  // short read: end of stream (or a decode error)
    }
    sr2_decoder_close(d);
    if (!stop_.load()) {
        decoded += startFrame;
        if (info.totalFrames && decoded + rate_ < info.totalFrames)
            SR2_WARN("slime_dj: '{}' ended early: {} of {} frames decoded (damaged file?)", Utf8(path.filename().wstring()),
                     decoded, info.totalFrames);
        total_.store(decoded);
        finished_.store(true);
    }
}

}  // namespace sr2::modules::slime_dj
