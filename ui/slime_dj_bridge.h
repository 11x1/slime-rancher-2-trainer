#pragma once
// Data exchange between the slime_dj module (modules/slime_dj) and its overlay tab (menu.cpp,
// Ranch > Slime DJ). Same pattern as sandbox_bridge.h:
//   * The module copies what the tab shows into a Snapshot on the Unity main thread (about
//     5 Hz, only while the tab was drawn in the last ~0.6 s) and publishes it here. The render
//     thread only reads that copy: no game calls.
//   * The tab asks for changes with Request(). That posts a job to the main thread, which hands
//     the Action to the module's handler.
//   * Song ids are a hash of the file path (stable across rescans); queue entries have their
//     own ids, so the same song can be queued twice.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr2::ui::slime_dj {

inline constexpr const char* kModule = "slime_dj";

struct Song {
    uint64_t id = 0;
    std::string name;    // what the list shows: "Artist - Title" from tags, else the file name
    std::string file;    // file name relative to the music folder
    double seconds = 0;  // 0 = unknown
    std::string error;   // not playable: why ("" = fine)
};

struct QueueEntry {
    uint64_t uid = 0;     // queue entry id
    uint64_t song = 0;    // Song::id
    std::string name;
    double seconds = 0;
};

struct Snapshot {
    unsigned long long tick = 0;                      // GetTickCount64 when it was taken
    std::shared_ptr<const std::vector<Song>> songs;   // sorted by name; nullptr before the first scan
    bool scanning = false;                            // the folder is being read
    std::string folder;                               // the music folder (full path, UTF-8)
    std::vector<QueueEntry> queue;
    // now playing
    bool hasSong = false;
    uint64_t song = 0;
    std::string name;
    double position = 0, duration = 0;  // seconds (duration 0 = unknown)
    bool paused = false;                // paused by the player
    std::string waiting;                // loaded but not audible yet: why ("" = playing or paused)
    int speakers = 0;                   // speakers the song plays from (music boxes + emitters)
    int emitterSpeakers = 0;            // of those, on registered emitters (slime_dj/api.h)
    int boxes = -1;                     // owned music boxes found nearby (-1 = not looked yet)
    int emitters = 0;                   // registered emitters alive at the last scan
    bool shuffle = true;                // module setting "shuffle"
};

enum class Op {
    Play,         // song: play it now (replaces the current song)
    Enqueue,      // song: append to the queue
    Remove,       // uid: remove from the queue
    MoveUp,       // uid
    MoveDown,     // uid
    ClearQueue,
    TogglePause,  // pause / resume; starts the next song when nothing is loaded
    Skip,         // next song (queue, else a random one with shuffle on)
    Stop,         // unload the current song
    Rescan,       // read the music folder again
};

struct Action {
    Op op = Op::Rescan;
    uint64_t song = 0;
    uint64_t uid = 0;
};

struct Status {
    std::string text;
    bool error = false;
    unsigned long long tick = 0;  // GetTickCount64; 0 = none yet
};

// ---- module side ----
void Publish(Snapshot s);
void SetStatus(std::string text, bool error);
void SetHandler(void (*fn)(const Action&));  // nullptr detaches (module off). Any thread.
bool WantsSnapshot();                         // the tab was drawn within the last ~0.6 s

// ---- render thread ----
std::shared_ptr<const Snapshot> Get();  // nullptr until the first publish
Status GetStatus();
void MarkViewed();  // call every frame the tab is drawn
// Posts the action to the main thread. False (and a status line) if the dispatcher is not running.
bool Request(const Action& a);
// Opens the music folder in Explorer (short-lived thread; never blocks the caller).
void OpenFolder(const std::string& utf8Path);

}  // namespace sr2::ui::slime_dj
