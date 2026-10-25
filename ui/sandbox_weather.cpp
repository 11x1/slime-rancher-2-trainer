// The Sandbox page's "Weather" card (sandbox_weather.h). Built only with Dear ImGui, like menu.cpp.
#if defined(SR2_HAVE_IMGUI)

#include "sandbox_weather.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "item_grid.h"
#include "item_icons.h"
#include "sandbox_weather_bridge.h"
#include "theme.h"

namespace sr2::ui::sandbox_weather {
namespace {

namespace wb = ::sr2::ui::sandbox::weather;
using theme::Palette;

constexpr uintptr_t kClearId = 1;  // the "Clear skies" tile (pattern ids are object addresses, never 1)

float g_scale = 1.0f;
float U(float v) { return v * g_scale; }

item_grid::State g_grid;      // search text of the picker
uintptr_t g_pick = 0;         // picked pattern id (kClearId: clear skies, 0: none)
int g_statePick = 0;          // index into the picked pattern's states
bool g_all = false;           // "All zones"
bool g_lock = false;          // "Lock"

void DimText(const std::string& text) {
    ImGui::PushFont(theme::Body(), theme::kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void SmallHeading(const char* text) {
    ImGui::PushFont(theme::Display(), theme::kSmallSize + 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors().soft);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void HoverTip(const Helpers& h, const std::string& text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        h.tooltip(text);
}

std::string Hours(double h) {
    if (h < 1.0) return std::format("{:.0f} min", std::max(1.0, h * 60.0));
    return h < 10.0 ? std::format("{:.1f} h", h) : std::format("{:.0f} h", h);
}

// The map's chevrons for a state's tier (WeatherStateDefinition.MapTier, 0..3), small, at `p` (left, centre y).
float Chevrons(ImDrawList* dl, ImVec2 p, int tier, ImU32 col) {
    const float w = U(7), hgt = U(4), step = U(5);
    for (int i = 0; i < tier; ++i) {
        const float x = p.x + step * static_cast<float>(i);
        dl->AddPolyline(std::vector<ImVec2>{ImVec2(x, p.y + hgt * 0.5f), ImVec2(x + w * 0.5f, p.y - hgt * 0.5f),
                                            ImVec2(x + w, p.y + hgt * 0.5f)}
                            .data(),
                        3, col, 0, std::max(1.0f, U(1.6f)));
    }
    return tier > 0 ? step * static_cast<float>(tier - 1) + w + U(4) : 0.0f;
}

// One weather line: icon, "Name - State", the tier's chevrons, then `right` in the dim colour.
void WeatherLine(uint32_t icon, const std::string& name, const std::string& state, int tier, const std::string& right,
                 float iconSize) {
    const Palette& P = theme::Colors();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float lineH = std::max(U(iconSize), ImGui::GetTextLineHeight());
    const float cy = o.y + lineH * 0.5f;
    float x = o.x;
    if (icon && item_icons::Draw(dl, icon, ImVec2(x, cy - U(iconSize) * 0.5f), ImVec2(x + U(iconSize), cy + U(iconSize) * 0.5f)))
        x += U(iconSize) + U(8);
    else
        x += U(iconSize) + U(8);
    const std::string text = state.empty() || state == name ? name : name + " - " + state;
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    dl->AddText(ImVec2(x, cy - ts.y * 0.5f), P.text, text.c_str());
    x += ts.x + U(6);
    x += Chevrons(dl, ImVec2(x, cy), tier, theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
    if (!right.empty()) {
        ImGui::PushFont(theme::Body(), theme::kSmallSize);
        const ImVec2 rs = ImGui::CalcTextSize(right.c_str());
        dl->AddText(ImVec2(x + U(4), cy - rs.y * 0.5f), P.dim, right.c_str());
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, lineH));
}

void ZoneCombo(const Helpers& h, const wb::State& st, float w) {
    const Palette& P = theme::Colors();
    auto label = [&](int i) {
        std::string s = st.zones[static_cast<size_t>(i)].name;
        if (i == st.here) s += " (you are here)";
        return s;
    };
    const std::string cur = st.shown >= 0 ? label(st.shown) : std::string("Pick a zone");
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(6), U(6)));
    h.pushFloating();
    const bool open = ImGui::BeginCombo("##wxzone", cur.c_str(), ImGuiComboFlags_HeightLarge);
    h.popFloating();
    ImGui::PopStyleVar();
    HoverTip(h, "The zone this card shows and changes. It follows you until you pick another one.");
    if (!open) return;
    h.surface(true);
    // "Where you are" follows the player; where there is no weather (the ranch) it is grayed out (round 10).
    const bool noWeatherHere = st.here < 0;
    const std::string follow =
        !noWeatherHere         ? std::format("Where you are ({})", st.zones[static_cast<size_t>(st.here)].name)
        : st.hereName.empty() ? std::string("Where you are (no weather here)")
                              : std::format("Where you are ({}: no weather)", st.hereName);
    if (ImGui::Selectable(follow.c_str(), false, noWeatherHere ? ImGuiSelectableFlags_Disabled : 0)) {
        wb::Action a;
        a.op = wb::Op::Show;
        wb::Request(a);
    }
    if (noWeatherHere)
        HoverTip(h, std::format("{} has no weather of its own, so there is nothing to change where you are. Pick a zone "
                                "below; the card follows you again once you pick this in a zone with weather.",
                                st.hereName.empty() ? std::string("This place") : st.hereName));
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(st.zones.size()); ++i) {
        ImGui::PushID(i);
        const bool sel = i == st.shown;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
        if (ImGui::Selectable(label(i).c_str(), sel)) {
            wb::Action a;
            a.op = wb::Op::Show;
            a.zone = st.zones[static_cast<size_t>(i)].id;
            wb::Request(a);
        }
        if (sel) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

void NowAndForecast(const wb::State& st) {
    const Palette& P = theme::Colors();
    SmallHeading("Now");
    if (st.running.empty()) WeatherLine(0, "Clear skies", "", 0, "", 26);
    for (const auto& r : st.running) WeatherLine(r.icon, r.name, r.state, r.tier, "", 26);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(theme::Body(), theme::kSmallSize);
    if (st.mode == 2)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "Locked: %s (kept in this save)", st.overrideName.c_str());
    else if (st.mode == 1)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.soft), "%s, natural weather back in %s", st.overrideName.c_str(),
                           Hours(st.endsIn).c_str());
    else
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "Natural weather (the game's forecast)");
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, U(2)));
    SmallHeading("Forecast");
    if (st.forecast.empty()) {
        DimText("Nothing forecast yet.");
        return;
    }
    for (const auto& f : st.forecast) {
        std::string when;
        if (f.running) when = f.endIn >= 0 ? std::format("now, {} left", Hours(f.endIn)) : std::string("now");
        else when = std::format("in {}", Hours(std::max(0.0, f.startIn))) +
                    (f.endIn >= 0 ? std::format(" for {}", Hours(std::max(0.0, f.endIn - f.startIn))) : std::string());
        WeatherLine(f.icon, f.name, f.state, f.tier, when, 20);
    }
    if (st.mode != 0) DimText("The forecast is the game's own; it runs again once the weather is back to natural.");
}

void Picker(const Helpers& h, const wb::State& st, const wb::Zone& z) {
    const Palette& P = theme::Colors();
    // The pick must belong to the zone shown.
    const wb::Pattern* picked = nullptr;
    for (const auto& p : z.patterns)
        if (p.id == g_pick) picked = &p;
    if (!picked && g_pick != kClearId) g_pick = 0;
    static const std::string kClear = "Clear skies";
    std::vector<item_grid::Tile> tiles;
    item_grid::Tile clear;
    clear.id = kClearId;
    clear.name = &kClear;
    clear.accent = P.dim;
    tiles.push_back(clear);
    for (const auto& p : z.patterns) {
        if (!item_grid::Matches(p.name, g_grid.search)) continue;
        item_grid::Tile t;
        t.id = p.id;
        t.name = &p.name;
        t.icon = p.icon;
        t.accent = theme::Current() == theme::Mode::Dark ? P.grab : P.accent;
        tiles.push_back(t);
    }
    item_grid::Options o;
    o.scale = g_scale;
    o.tile = 54.0f;
    o.height = 128.0f;
    o.selected = g_pick;
    o.hint = "Search weather...";
    o.empty = "No matching weather.";
    const item_grid::Result r = item_grid::Draw("##wxpick", tiles, g_grid, o);
    if (r.clicked >= 0) {
        const uintptr_t id = tiles[static_cast<size_t>(r.clicked)].id;
        if (id != g_pick) g_statePick = 0;
        g_pick = id;
    }
    if (r.hovered >= 0) {
        const uintptr_t id = tiles[static_cast<size_t>(r.hovered)].id;
        std::string tip = "Clear skies: no weather at all.";
        for (const auto& p : z.patterns)
            if (p.id == id) {
                tip = p.name;
                if (!p.states.empty()) {
                    tip += "\nStates:";
                    for (const auto& s : p.states)
                        tip += std::format("\n  {}{}", s.name, s.tier ? std::string(static_cast<size_t>(s.tier), '^') : "");
                }
            }
        h.tooltip(tip);
    }
    picked = nullptr;
    for (const auto& p : z.patterns)
        if (p.id == g_pick) picked = &p;
    if (picked && picked->states.size() > 1) {
        std::vector<std::string> labels;
        for (const auto& s : picked->states) labels.push_back(s.name);
        g_statePick = std::clamp(g_statePick, 0, static_cast<int>(labels.size()) - 1);
        h.subTabs("##wxstate", labels, g_statePick);
        const auto& s = picked->states[static_cast<size_t>(g_statePick)];
        DimText(std::format("{}: map tier {}{}", s.name, s.tier,
                            s.hours > 0 ? std::format(", lasts at least {:g} h naturally", s.hours) : std::string()));
    } else {
        g_statePick = 0;
    }
    (void)st;
}

// "label [switch]" on the current line.
void SwitchItem(const Helpers& h, const char* id, const char* label, bool& v, const std::string& tip) {
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    HoverTip(h, tip);
    ImGui::SameLine();
    if (h.toggle("##sw", v)) v = !v;
    HoverTip(h, tip);
    ImGui::PopID();
}

void Controls(const Helpers& h, const wb::State& st, const wb::Zone& z) {
    const wb::Pattern* picked = nullptr;
    for (const auto& p : z.patterns)
        if (p.id == g_pick) picked = &p;
    SwitchItem(h, "all", "All zones", g_all,
               "Every zone with weather gets it: the same weather type where the zone has one (rain is a different "
               "pattern per zone), else that zone is left alone. Clear skies applies everywhere.");
    ImGui::SameLine(0, U(28));
    SwitchItem(h, "lock", "Lock", g_lock,
               "Keep it: whenever the game's forecast changes the weather, it is put back (checked every second), and "
               "it comes back after a reload (kept in this save's sr2mods sidecar, never in the save). Off: it runs "
               "for a while (the state's natural length, 1-24 game hours; 4 h for clear skies), then the natural "
               "weather comes back.");
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float bw = std::floor((ImGui::GetContentRegionAvail().x - 2 * sp) / 3);
    const ImVec2 bs(bw, U(32));
    const bool canStart = g_pick != 0;
    auto start = [&](uintptr_t pattern, uintptr_t state) {
        wb::Action a;
        a.op = wb::Op::Start;
        a.zone = z.id;
        a.pattern = pattern;
        a.state = state;
        a.all = g_all;
        a.lock = g_lock;
        wb::Request(a);
    };
    ImGui::BeginDisabled(!canStart);
    if (h.accentButton("Start now", bs)) {
        if (g_pick == kClearId) start(0, 0);
        else if (picked)
            start(picked->id, picked->states.empty() ? 0 : picked->states[static_cast<size_t>(std::clamp(
                                                               g_statePick, 0, static_cast<int>(picked->states.size()) - 1))]
                                                                    .id);
    }
    ImGui::EndDisabled();
    HoverTip(h, canStart ? std::string("Starts the picked weather now, with the game's own transition (rain, wind and "
                                       "storm effects fade in; weather that spawns things spawns them).")
                         : std::string("Pick a weather above first."));
    ImGui::SameLine();
    if (ImGui::Button("End weather", bs)) start(0, 0);
    HoverTip(h, "Ends the weather now (clear skies), for a while or locked as the Lock switch says.");
    ImGui::SameLine();
    const bool canNatural = st.mode != 0 || (g_all && st.overriddenZones > 0);
    ImGui::BeginDisabled(!canNatural);
    if (ImGui::Button("Back to natural", bs)) {
        wb::Action a;
        a.op = wb::Op::Natural;
        a.zone = z.id;
        a.all = g_all;
        wb::Request(a);
    }
    ImGui::EndDisabled();
    HoverTip(h, canNatural ? std::string("Back to natural weather: ends ours and starts what the game's forecast has "
                                         "running now; a lock is removed from the save.") +
                                 (g_all ? " All zones: every changed zone." : "")
                           : std::string("This zone already has its natural weather."));
    if (st.lockedZones > 0 || st.overriddenZones > 0)
        DimText(std::format("Changed weather in {} zone(s), {} locked.", st.overriddenZones, st.lockedZones));
}

// ---- Time of day (round 10: "let me set time in sandbox, missed it by a second") ----
constexpr double kDay = 86400.0;
int g_timePick = -1;        // picked time of day in minutes (-1: not set yet, starts at the game's time)
bool g_rewindAsk = false;   // open the rewind confirmation this frame
int g_rewindMin = 0;        // the time it asks about

std::string HhMm(int mins) { return std::format("{:02}:{:02}", mins / 60 % 24, mins % 60); }
int MinuteOfDay(double t) { return static_cast<int>(std::floor((t - std::floor(t / kDay) * kDay) / 60.0)); }
std::string Clock(double t) {
    return std::format("Day {}, {}", static_cast<long long>(std::floor(t / kDay)) + 1, HhMm(MinuteOfDay(t)));
}
// The next / last time the clock shows `mins` (as the module computes it).
double NextAt(double now, int mins) {
    double t = std::floor(now / kDay) * kDay + mins * 60.0;
    return t <= now + 30.0 ? t + kDay : t;
}
double LastAt(double now, int mins) {
    double t = std::floor(now / kDay) * kDay + mins * 60.0;
    return t >= now ? t - kDay : t;
}
std::string Span(double secs) {
    const int m = static_cast<int>(std::lround(secs / 60.0));
    return m < 60 ? std::format("{} min", m) : std::format("{} h {:02} min", m / 60, m % 60);
}
const char* DayPart(int mins) {
    const int h = mins / 60;
    return h < 6 ? "night" : h < 12 ? "morning" : h < 18 ? "afternoon" : "evening";
}

void TimeRequest(wb::Op op, int mins, bool on = false) {
    wb::Action a;
    a.op = op;
    a.hour = mins / 60.0;
    a.lock = on;
    wb::Request(a);
}

// The rewind confirmation (Cancel / Rewind, Esc cancels), centred on the page.
void RewindModal(const Helpers& h, const wb::Time& t, ImVec2 centre) {
    const Palette& P = theme::Colors();
    constexpr const char* kId = "##wxrewind";
    if (g_rewindAsk) {
        ImGui::OpenPopup(kId);
        g_rewindAsk = false;
    }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(U(430), 0), ImVec2(U(430), FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(U(22), U(18)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(U(10), U(8)));
    ImGui::PushStyleColor(ImGuiCol_Border, P.cardBorder);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::BeginPopupModal(kId, nullptr, f)) {
        const double target = LastAt(t.now, g_rewindMin);
        ImGui::PushFont(theme::Display(), theme::kDisplaySize + 4);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Current() == theme::Mode::Dark ? P.grab : P.accent);
        ImGui::TextUnformatted("Set the clock back?");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(std::format("From {} back to {} ({} earlier).", Clock(t.now), Clock(target),
                                           Span(t.now - target))
                                   .c_str());
        ImGui::PopTextWrapPos();
        DimText("Nothing is undone: plorts, items, Newbucks and the ranch stay as they are. Whatever the game has "
                "scheduled after now (garden regrowth, slime hunger and feeding, the weather forecast, refinery and "
                "drone timers) just comes that much later. Kept when the game saves.");
        ImGui::Dummy(ImVec2(0, U(4)));
        const ImVec2 bs(U(112), U(34));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2 * bs.x -
                             ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Cancel", bs) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (h.accentButton("Rewind", bs)) {
            TimeRequest(wb::Op::TimeRewind, g_rewindMin);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void TimeOfDay(const Helpers& h, const wb::State& st) {
    const Palette& P = theme::Colors();
    const wb::Time& t = st.time;
    if (!t.ok) {
        if (t.why.rfind("Time of day unavailable", 0) == 0) DimText(t.why);
        return;
    }
    const ImVec2 centre(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f,
                        ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.5f);
    const int nowMin = MinuteOfDay(t.now);
    if (g_timePick < 0) g_timePick = (nowMin + 4) / 5 * 5 % 1440;
    SmallHeading("Time of day");
    // The clock, then what it is doing.
    ImGui::PushFont(theme::Display(), theme::kDisplaySize + 6);
    ImGui::TextUnformatted(Clock(t.now).c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, U(12));
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(theme::Body(), theme::kSmallSize);
    if (t.skipping) {
        const double left = std::max(0.0, t.skipUntil - t.now);
        const double real = t.ffSecsPerDay > 0 ? left / kDay * t.ffSecsPerDay : 0;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.soft), "skipping ahead to %s, %s of game time left (about %.0f s)",
                           HhMm(MinuteOfDay(t.skipUntil)).c_str(), Span(left).c_str(), real);
    } else if (t.paused) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.warn), "paused");
    } else {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P.dim), "%s", DayPart(nowMin));
    }
    ImGui::PopFont();

    // Quick skips: the next 06:00 / 12:00 / 18:00 / 00:00.
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    static const int kQuick[] = {6 * 60, 12 * 60, 18 * 60, 0};
    const float qw = std::floor((avail - 3 * sp) / 4);
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        const int m = kQuick[i];
        if (ImGui::Button(std::format("{}##wxq{}", HhMm(m), i).c_str(), ImVec2(qw, U(30)))) TimeRequest(wb::Op::TimeSkip, m);
        const double in = NextAt(t.now, m) - t.now;
        HoverTip(h, std::format("Skip ahead to the next {} (in {} of game time, about {:.0f} s): time runs fast until "
                                "then, as when you sleep, so the ranch and the world catch up.",
                                HhMm(m), Span(in), t.ffSecsPerDay > 0 ? in / kDay * t.ffSecsPerDay : 0.0));
    }

    // Any time: a slider in 5 minute steps, then Skip ahead / Rewind.
    const float bw = U(104);
    ImGui::SetNextItemWidth(std::max(U(120), avail - 2 * bw - 2 * sp));
    const std::string fmt = HhMm(g_timePick);
    if (ImGui::SliderInt("##wxtime", &g_timePick, 0, 1435, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp))
        g_timePick = std::clamp(g_timePick / 5 * 5, 0, 1435);
    HoverTip(h, "The time of day to skip ahead or rewind to (5 minute steps; Ctrl+click to type it).");
    ImGui::SameLine();
    const double ahead = NextAt(t.now, g_timePick) - t.now;
    if (h.accentButton("Skip ahead", ImVec2(bw, 0))) TimeRequest(wb::Op::TimeSkip, g_timePick);
    HoverTip(h, std::format("Skip ahead to {} (in {} of game time, about {:.0f} s), as sleeping does: time runs fast, "
                            "the ranch and the world catch up.",
                            HhMm(g_timePick), Span(ahead), t.ffSecsPerDay > 0 ? ahead / kDay * t.ffSecsPerDay : 0.0));
    ImGui::SameLine();
    const double back = LastAt(t.now, g_timePick);
    const bool canBack = back >= t.start;
    ImGui::BeginDisabled(!canBack);
    if (ImGui::Button("Rewind", ImVec2(bw, 0))) {
        g_rewindMin = g_timePick;
        g_rewindAsk = true;
    }
    ImGui::EndDisabled();
    HoverTip(h, canBack ? std::format("Set the clock back to {} ({} earlier), at once. Asks first.", Clock(back),
                                      Span(t.now - back))
                        : std::format("{} is before this game started ({}).", Clock(back), Clock(t.start)));

    // Pause, and Stop while skipping.
    bool paused = t.paused;
    SwitchItem(h, "pause", "Pause time", paused,
               "Stops the clock: no time passes for the ranch, the weather or anything timed until you switch it off "
               "(the game keeps running). Not saved: a reload or turning Sandbox off lets it run again.");
    if (paused != t.paused) TimeRequest(wb::Op::TimePause, 0, paused);
    if (t.skipping) {
        ImGui::SameLine(0, U(28));
        if (ImGui::Button("Stop skipping", ImVec2(0, 0))) TimeRequest(wb::Op::TimeStop, 0);
        HoverTip(h, "Ends the skip where the clock is now.");
    }
    RewindModal(h, t, centre);
}

}  // namespace

void DrawCard(const Helpers& h, float w) {
    g_scale = h.scale;
    const Palette& P = theme::Colors();
    h.beginCard("sbx_weather", w);
    h.heading("Time & weather", P.head);
    const auto st = wb::Get();
    if (!st || GetTickCount64() - st->tick > 10000) {
        DimText("Reading the weather...");
        h.endCard();
        return;
    }
    TimeOfDay(h, *st);  // the clock first: it works where there is no weather too
    if (st->time.ok) {
        ImGui::Dummy(ImVec2(0, U(4)));
        SmallHeading("Weather");
    }
    if (!st->inGame || st->zones.empty()) {
        DimText(st->why.empty() ? std::string("No weather here.") : st->why);
        h.endCard();
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x, gap = U(18);
    const bool split = avail >= U(620);
    const float leftW = split ? std::floor((avail - gap) * 0.42f) : avail;
    ZoneCombo(h, *st, split ? leftW : avail);
    if (st->shown < 0) {
        DimText(st->hereName.empty() ? std::string("There is no weather where you are. Pick a zone to change its weather.")
                                     : std::format("{} has no weather. Pick a zone to change its weather.", st->hereName));
        h.endCard();
        return;
    }
    const wb::Zone& z = st->zones[static_cast<size_t>(st->shown)];
    if (split) {
        const ImVec2 top = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        ImGui::PushItemWidth(leftW);
        ImGui::BeginChild("##wxleft", ImVec2(leftW, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        NowAndForecast(*st);
        ImGui::EndChild();
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        const float leftBottom = ImGui::GetItemRectMax().y;
        ImGui::SetCursorScreenPos(ImVec2(top.x + leftW + gap, top.y - ImGui::GetFrameHeightWithSpacing()));
        ImGui::BeginChild("##wxright", ImVec2(avail - leftW - gap, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoBackground);
        Picker(h, *st, z);
        Controls(h, *st, z);
        ImGui::EndChild();
        const float rightBottom = ImGui::GetItemRectMax().y;
        ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(leftBottom, rightBottom)));
        ImGui::Dummy(ImVec2(0, 0));
    } else {
        NowAndForecast(*st);
        ImGui::Dummy(ImVec2(0, U(4)));
        Picker(h, *st, z);
        Controls(h, *st, z);
    }
    DimText("Weather from the game's own weather system: its transitions, effects and sounds, and what it spawns "
            "(lightning, radiant slimes, slime rain). The save keeps the game's forecast only.");
    h.endCard();
}

}  // namespace sr2::ui::sandbox_weather

#endif  // SR2_HAVE_IMGUI
