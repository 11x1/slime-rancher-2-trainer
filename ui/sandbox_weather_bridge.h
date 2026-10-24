#pragma once
// Data exchange between the sandbox module's weather changer
// (modules/sandbox/weather.cpp) and the "Weather" card on the Sandbox page
// (ui/sandbox_weather.cpp). Kept apart from sandbox_bridge.h like the player
// extras and the upgrades manager.
//
// Threading is the same as sandbox_bridge.h: the module publishes a State on
// the Unity main thread (~2 Hz while the page is open, and right after each
// change); the render thread only reads it. Requests are posted to the main
// thread and handled there. Results go to the shared status line
// (sandbox::SetStatus) and the log.
//
// Ids are opaque (the game objects' addresses); the module checks every id
// against its own catalog before using it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sr2::ui::sandbox::weather {

// One state of a weather pattern (WeatherStateDefinition: light rain, heavy rain, ...).
struct WeatherState {
    uintptr_t id = 0;
    std::string name;   // WeatherStateDefinition.StateName, else its asset name
    int tier = 0;       // MapTier: the chevrons the map shows (0..3)
    float hours = 0;    // MinDurationHours
};

// One weather pattern a zone can have (WeatherPatternDefinition: Rain, Thunderstorm, Slime Rain, ...).
struct Pattern {
    uintptr_t id = 0;
    std::string name;   // its WeatherTypeMetadata's Slimepedia title, else the asset name
    uint32_t icon = 0;  // ui/item_icons.h id (WeatherTypeMetadata.Icon, the map's icon), 0 = none
    std::vector<WeatherState> states;
};

struct Zone {
    uintptr_t id = 0;
    std::string name;   // ZoneDefinition._localizedName, else the asset name
    std::vector<Pattern> patterns;
};

// What runs in a zone right now (WeatherRegistry: the zone's running patterns and their states).
struct Running {
    std::string name, state;
    uint32_t icon = 0;
    int tier = 0;
};

// One entry of a zone's forecast (WeatherModel.ZoneData.Forecast, the save's own data).
struct Forecast {
    std::string name, state;
    uint32_t icon = 0;
    int tier = 0;
    double startIn = 0;   // game hours from now (<= 0: started)
    double endIn = -1;    // game hours from now; < 0: end not known
    bool running = false; // started and not over
};

struct State {
    unsigned long long tick = 0;  // GetTickCount64 when it was taken
    bool inGame = false;
    std::string why;              // !inGame / unavailable: what is missing
    std::vector<Zone> zones;      // every zone with weather, in the registry's order
    int here = -1;                // index of the zone the player is in (-1: none with weather)
    int shown = -1;               // index of the zone the lists below are about (the UI's choice, else `here`)
    std::vector<Running> running; // in `shown`
    std::vector<Forecast> forecast;  // in `shown`, by start time
    // The override in `shown`: 0 = natural weather, 1 = started for a while (endsIn), 2 = locked.
    int mode = 0;
    std::string overrideName;     // what it holds ("Thunderstorm - Heavy", "Clear skies")
    double endsIn = -1;           // mode 1: game hours left
    int lockedZones = 0;          // zones with a lock (any)
    int overriddenZones = 0;      // zones with any override (lock or for a while)
};

enum class Op {
    Show,     // zone = the zone the card shows (0 = where the player is)
    Start,    // zone, pattern (0 = clear skies), state, all, lock
    Natural,  // zone, all: back to the natural weather (clears the lock / override)
};

struct Action {
    Op op = Op::Show;
    uintptr_t zone = 0;
    uintptr_t pattern = 0;
    uintptr_t state = 0;
    bool all = false;   // every weather zone (the same weather where the zone has it)
    bool lock = false;  // keep it: re-applied when the game changes it, kept per save
};

// ---- module side (Unity main thread) ----
void Publish(State s);
void SetHandler(void (*fn)(const Action&));  // nullptr detaches (module off). Any thread.

// ---- render thread ----
std::shared_ptr<const State> Get();  // nullptr until the first publish
bool Request(Action a);

}  // namespace sr2::ui::sandbox::weather
