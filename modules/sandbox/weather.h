#pragma once
// sandbox: the weather changer on the Sandbox page's "Weather" card (see NOTES.md, "Weather changer").
// SR2 1.3.0.
//
// The game keeps a forecast per zone (WeatherModel.ZoneData.Forecast, saved) and runs what it says
// through WeatherRegistry (Update starts / ends forecast entries; each running WeatherPattern has one
// WeatherStateDefinition whose activities - rain, wind, lightning, slime rain, spawns - the zone's
// WeatherDirector starts and stops with their fades). The changer uses the game's own debug entry point,
// WeatherRegistry.DEBUG_SetZoneWeather(zone, pattern, state): it stops the pattern's state and runs the
// new one through the same RunPatternState / WeatherDirector.RunState path (state = null ends the
// pattern). The forecast is never touched, so the save only ever holds the game's own forecast; a lock
// lives in the save's sidecar (cheated_save.h, SaveData "weather") and is re-applied after a load.
//
// No hooks: a 1 Hz main-thread check re-applies a lock when the game's forecast changes the zone's weather
// and ends a timed start. Nothing is required: a missing symbol turns the card off (logged, the card says why).

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::sandbox {

// Worker thread, from Sandbox::Setup. Never fails the module.
void SetupWeather(sr2::core::ModuleContext& ctx);
// Module on / off (any thread): attaches the request handler.
void WeatherEnabled(bool on);
// Main thread, every frame while the module is live. `snapshot`: the page is open.
void WeatherTick(bool snapshot);

}  // namespace sr2::modules::sandbox
