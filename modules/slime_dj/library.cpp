#include "library.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <format>
#include <fstream>
#include <map>
#include <thread>

#include "core/log.h"
#include "modules/slime_dj/codecs.h"

namespace sr2::modules::slime_dj {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxFiles = 2000;  // songs per folder (the rest is ignored, logged once)
constexpr int kMaxDepth = 4;        // subfolder levels below the music folder

uint64_t Fnv1a(const std::wstring& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (wchar_t c : s) {
        const wchar_t l = static_cast<wchar_t>(towlower(c));
        h ^= static_cast<uint64_t>(l);
        h *= 0x100000001b3ull;
    }
    return h ? h : 1;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) <= ' ')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    return s.substr(a);
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ID3 text frame payload (encoding byte + text) -> UTF-8, first value only.
std::string Id3Text(const std::vector<uint8_t>& d) {
    if (d.empty()) return {};
    const uint8_t enc = d[0];
    std::string out;
    if (enc == 0 || enc == 3) {  // Latin-1 / UTF-8
        for (size_t i = 1; i < d.size() && d[i]; ++i) {
            if (enc == 3) out.push_back(static_cast<char>(d[i]));
            else AppendUtf8(out, d[i]);
        }
        return Trim(out);
    }
    // UTF-16 with BOM (1) or big-endian without (2)
    bool be = enc == 2;
    size_t i = 1;
    if (enc == 1 && d.size() >= 3) {
        if (d[1] == 0xFE && d[2] == 0xFF) be = true, i = 3;
        else if (d[1] == 0xFF && d[2] == 0xFE) be = false, i = 3;
    }
    for (; i + 1 < d.size(); i += 2) {
        uint32_t u = be ? (d[i] << 8 | d[i + 1]) : (d[i + 1] << 8 | d[i]);
        if (u == 0) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < d.size()) {
            const uint32_t lo = be ? (d[i + 2] << 8 | d[i + 3]) : (d[i + 3] << 8 | d[i + 2]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, u);
    }
    return Trim(out);
}

std::string DisplayName(const std::string& title, const std::string& artist, const fs::path& p) {
    if (!title.empty()) return artist.empty() ? title : artist + " - " + title;
    return Utf8(p.stem().wstring());
}

SongInfo Probe(const fs::path& p, const fs::path& root, uint64_t size, int64_t mtime) {
    SongInfo s;
    s.path = p;
    s.id = Fnv1a(p.wstring());
    std::error_code ec;
    fs::path rel = fs::relative(p, root, ec);
    s.file = Utf8((ec ? p.filename() : rel).wstring());
    s.size = size;
    s.mtime = mtime;
    sr2_audio_info info{};
    sr2_decoder* d = sr2_decoder_open(p.c_str(), 1, &info);
    if (!d) {
        s.error = "can't be decoded (unsupported or damaged file)";
        s.name = Utf8(p.stem().wstring());
        return s;
    }
    sr2_decoder_close(d);
    s.channels = info.channels;
    s.rate = info.sampleRate;
    s.frames = info.totalFrames;
    s.seconds = info.sampleRate ? static_cast<double>(info.totalFrames) / info.sampleRate : 0.0;
    std::string title = info.title, artist = info.artist;
    if (info.codec == SR2_CODEC_MP3) ReadId3(p, title, artist);
    s.name = DisplayName(Trim(title), Trim(artist), p);
    if (info.sampleRate < 8000 || info.sampleRate > 192000) s.error = "unsupported sample rate";
    return s;
}

}  // namespace

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadId3(const fs::path& path, std::string& title, std::string& artist) {
    std::ifstream f(path, std::ios::binary);
    uint8_t h[10];
    if (!f.read(reinterpret_cast<char*>(h), 10) || h[0] != 'I' || h[1] != 'D' || h[2] != '3') return false;
    const int ver = h[3];
    if (ver < 2 || ver > 4) return false;
    const uint32_t tagSize = (h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F);
    uint64_t pos = 10;
    const uint64_t end = 10ull + tagSize;
    if (ver >= 3 && (h[5] & 0x40)) {  // extended header: skip it
        uint8_t e[4];
        if (!f.read(reinterpret_cast<char*>(e), 4)) return false;
        uint32_t es = ver == 4 ? ((e[0] & 0x7F) << 21 | (e[1] & 0x7F) << 14 | (e[2] & 0x7F) << 7 | (e[3] & 0x7F))
                               : (static_cast<uint32_t>(e[0]) << 24 | e[1] << 16 | e[2] << 8 | e[3]);
        pos += ver == 4 ? es : es + 4;
        f.seekg(static_cast<std::streamoff>(pos));
    }
    std::string t, a;
    const size_t hdr = ver == 2 ? 6 : 10;
    for (int frames = 0; frames < 512 && pos + hdr <= end && (t.empty() || a.empty()); ++frames) {
        uint8_t fh[10];
        if (!f.read(reinterpret_cast<char*>(fh), static_cast<std::streamsize>(hdr))) break;
        if (fh[0] == 0) break;  // padding
        std::string id(reinterpret_cast<char*>(fh), ver == 2 ? 3 : 4);
        uint32_t size;
        if (ver == 2) size = fh[3] << 16 | fh[4] << 8 | fh[5];
        else if (ver == 4) size = (fh[4] & 0x7F) << 21 | (fh[5] & 0x7F) << 14 | (fh[6] & 0x7F) << 7 | (fh[7] & 0x7F);
        else size = static_cast<uint32_t>(fh[4]) << 24 | fh[5] << 16 | fh[6] << 8 | fh[7];
        pos += hdr;
        if (size == 0 || pos + size > end) break;
        const bool wantTitle = id == "TIT2" || id == "TT2", wantArtist = id == "TPE1" || id == "TP1";
        if ((wantTitle || wantArtist) && size <= 4096) {
            std::vector<uint8_t> d(size);
            if (!f.read(reinterpret_cast<char*>(d.data()), size)) break;
            (wantTitle ? t : a) = Id3Text(d);
        } else {
            f.seekg(static_cast<std::streamoff>(pos + size));
        }
        pos += size;
    }
    if (t.empty() && a.empty()) return false;
    if (title.empty()) title = t;
    if (artist.empty()) artist = a;
    return true;
}

struct Library::State {
    mutable std::mutex mu;
    SongList songs;
    bool scanning = false;
    bool restart = false;
    fs::path dir;
    unsigned generation = 0;
    std::map<std::wstring, SongInfo> cache;  // worker thread only (one scan at a time)
};

Library::Library() : state_(std::make_shared<State>()) {}

void Library::Rescan(const fs::path& dir) {
    {
        std::lock_guard lock(state_->mu);
        state_->dir = dir;
        if (state_->scanning) {
            state_->restart = true;
            return;
        }
        state_->scanning = true;
    }
    std::thread([st = state_] {
        for (;;) {
            fs::path root;
            {
                std::lock_guard lock(st->mu);
                root = st->dir;
                st->restart = false;
            }
            const auto t0 = std::chrono::steady_clock::now();
            auto out = std::make_shared<std::vector<SongInfo>>();
            std::map<std::wstring, SongInfo> cache;
            size_t probed = 0, skipped = 0;
            std::error_code ec;
            if (fs::is_directory(root, ec)) {
                fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), endIt;
                for (; !ec && it != endIt; it.increment(ec)) {
                    if (it.depth() > kMaxDepth) {
                        it.disable_recursion_pending();
                        continue;
                    }
                    std::error_code e2;
                    if (!it->is_regular_file(e2)) continue;
                    const fs::path& p = it->path();
                    if (sr2_codec_from_path(p.c_str()) == SR2_CODEC_NONE) continue;
                    if (out->size() >= kMaxFiles) {
                        ++skipped;
                        continue;
                    }
                    const uint64_t size = it->file_size(e2);
                    const int64_t mtime = static_cast<int64_t>(it->last_write_time(e2).time_since_epoch().count());
                    auto hit = st->cache.find(p.wstring());
                    SongInfo s;
                    if (hit != st->cache.end() && hit->second.size == size && hit->second.mtime == mtime) {
                        s = hit->second;
                    } else {
                        s = Probe(p, root, size, mtime);
                        ++probed;
                    }
                    cache.emplace(p.wstring(), s);
                    out->push_back(std::move(s));
                }
            }
            std::sort(out->begin(), out->end(), [](const SongInfo& a, const SongInfo& b) {
                const std::string x = Lower(a.name), y = Lower(b.name);
                return x != y ? x < y : a.file < b.file;
            });
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            const size_t bad = static_cast<size_t>(
                std::count_if(out->begin(), out->end(), [](const SongInfo& s) { return !s.error.empty(); }));
            SR2_INFO("slime_dj: music folder '{}': {} song(s) ({} newly read, {} not playable) in {} ms{}",
                     Utf8(root.wstring()), out->size(), probed, bad, ms,
                     skipped ? std::format(", {} more ignored (limit {})", skipped, kMaxFiles) : std::string());
            std::lock_guard lock(st->mu);
            st->cache = std::move(cache);
            st->songs = std::move(out);
            ++st->generation;
            if (!st->restart) {
                st->scanning = false;
                return;
            }
        }
    }).detach();
}

SongList Library::Songs() const {
    std::lock_guard lock(state_->mu);
    return state_->songs;
}

bool Library::Scanning() const {
    std::lock_guard lock(state_->mu);
    return state_->scanning;
}

unsigned Library::Generation() const {
    std::lock_guard lock(state_->mu);
    return state_->generation;
}

}  // namespace sr2::modules::slime_dj
