#pragma once
// slime_dj: one song being decoded. A background thread opens the file, then decodes it in
// fixed-size chunks (kChunkSeconds of interleaved float PCM, mono or stereo; more channels are
// mixed down to stereo) into a bounded queue (kQueueChunks ahead). The main thread pops chunks
// and copies them into the AudioClip ring. Memory per song: at most kQueueChunks chunks plus
// one being decoded (about 10 s of audio), whatever the song's length.
//
// No game calls. The owner must call Stop() when done: it ends the thread at its next chunk
// (the thread holds a reference while it runs).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sr2::modules::slime_dj {

class Stream {
public:
    static constexpr double kChunkSeconds = 0.5;
    static constexpr size_t kQueueChunks = 20;

    enum class State { Opening, Ready, Failed };

    // `startFrame`: decode from that PCM frame (seek); chunks then count from there.
    static std::shared_ptr<Stream> Start(const std::filesystem::path& path, uint64_t startFrame = 0);
    ~Stream();

    State state() const { return state_.load(); }
    std::string error() const;
    // Valid once Ready.
    uint32_t channels() const { return channels_; }       // 1 or 2 (output)
    uint32_t rate() const { return rate_; }
    uint32_t chunkFrames() const { return chunkFrames_; }
    uint64_t totalFrames() const { return total_.load(); } // song frames, from frame 0: the file's own count (0 = unknown) until done, then exact

    // Next chunk (exactly chunkFrames() * channels() floats; the last one is zero-padded).
    // `frames` gets the real (unpadded) frames in it. False if none is ready yet.
    bool Pop(std::vector<float>& out, uint32_t& frames);
    bool Done() const;      // decoder finished and every chunk was popped
    size_t Queued() const;  // chunks waiting
    void Stop();

private:
    Stream() = default;
    void Run(std::filesystem::path path, uint64_t startFrame);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    struct Chunk {
        std::vector<float> pcm;
        uint32_t frames = 0;
    };
    std::deque<Chunk> queue_;
    std::string error_;
    std::atomic<State> state_{State::Opening};
    std::atomic<bool> stop_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> total_{0};
    uint32_t channels_ = 0, rate_ = 0, chunkFrames_ = 0;
};

}  // namespace sr2::modules::slime_dj
