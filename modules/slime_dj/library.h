#pragma once
// slime_dj: the song library. Reads the music folder (<game>\sr2mods_music, subfolders
// included) on a background thread: every .ogg / .mp3 / .wav file is opened once to read its
// format, length and title tags, then closed again. No game calls; any thread.
//
// Results are cached per file (path + size + write time), so a rescan only opens new or
// changed files. The list is published as an immutable shared vector.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sr2::modules::slime_dj {

struct SongInfo {
    uint64_t id = 0;            // FNV-1a of the lower-case full path
    std::filesystem::path path;
    std::string file;           // path relative to the folder (UTF-8)
    std::string name;           // "Artist - Title", "Title" or the file name without extension
    uint32_t channels = 0, rate = 0;
    uint64_t frames = 0;        // 0 = unknown
    double seconds = 0;
    std::string error;          // "" = playable
    uint64_t size = 0;
    int64_t mtime = 0;
};

using SongList = std::shared_ptr<const std::vector<SongInfo>>;

class Library {
public:
    Library();
    // Starts a scan of `dir` unless one is running (then the running one is marked to restart).
    void Rescan(const std::filesystem::path& dir);
    SongList Songs() const;           // nullptr before the first scan finished
    bool Scanning() const;
    unsigned Generation() const;      // +1 per finished scan

private:
    struct State;
    std::shared_ptr<State> state_;
};

// Title / artist from an ID3v2 tag at the start of an MP3 file (v2.2, v2.3, v2.4; text frames
// TIT2/TPE1 or TT2/TP1). False if the file has no readable tag.
bool ReadId3(const std::filesystem::path& path, std::string& title, std::string& artist);

std::string Utf8(const std::wstring& w);

}  // namespace sr2::modules::slime_dj
