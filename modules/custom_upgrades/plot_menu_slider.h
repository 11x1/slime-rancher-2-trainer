#pragma once
// custom_upgrades: a reusable integer slider for the game's plot purchase menu (LandPlotUIRoot)
// details panel, built at runtime from uGUI parts (no managed code of ours).
//
// Layout: one root GameObject "sr2mods_Slider_<name>" placed into the bound
// PurchaseItemDetailsContentView, in the slot of its cost row (CraftCostView; the game hides that row
// for owned items): with a LayoutGroup on the row's parent it is inserted right after the row with a
// LayoutElement, else it copies the row's RectTransform. Several sliders side by side (spec.columns):
// the first builds a row "sr2mods_SliderRow_<name>" in that slot and takes its column, the others
// (Create with rowOf) put their roots into the same row. Inside: a TextMeshPro label (font, material,
// size and colour copied from the panel's description text) above a UnityEngine.UI.Slider
// ("Background" / "Fill Area/Fill" / "Handle Slide Area/Handle", the standard uGUI hierarchy). The
// shapes are the game's own SDFRoundedRectangle graphics (track in the panel text colour, fill and
// handle border in the game's slider pink); without that class plain Images, styled by a loaded
// SliderStyle asset through the game's SliderStyler when there is one.
//
// Input: the details panel is a nested Canvas without a GraphicRaycaster, so the root (or the row) gets its own
// Canvas (sorting order one above the panel's) + GraphicRaycaster; mouse click (jump) / drag then go
// to Unity's Slider (navigation Mode.None: a click does not move the EventSystem selection). Gamepad
// d-pad / left stick and keyboard arrows left / right: the menu's item grid (UniformGridView) would
// move its selection on them, so the module detours its OnNavigate and hands left / right to Step()
// while the slider is shown; the slider also polls pad / keys (fallback, de-duplicated per press).
// Which pad / keys depends on spec.input (d-pad, left stick, arrows / right stick, Shift + arrows).
// Steps only while the slider is interactable and no PopupPrompt is open. The value is read by
// polling (no managed delegates).
//
// Lifetime: Create() replaces the previous slider (one instance per PlotMenuSlider); Destroy() removes
// it; Tick() destroys it when its GameObject died with the menu. GC handles (root, slider, label,
// remembered selection) are freed only from posted main-thread jobs, never inside a hook.
//
// Threading: Setup on the worker thread; everything else on the Unity main thread. Every game call
// goes through Resolver::Invoke (managed exceptions caught; a native fault marks `module` faulted).
// Callers run Create / Tick inside their module's DetourGuard.

#include <functional>
#include <string>

struct Il2CppObject;

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::custom_upgrades::ui {

// Which keys step a slider (two sliders shown together need different ones; mouse works on both).
enum class SliderInput {
    kPrimary,    // gamepad d-pad / left stick, keyboard arrows (also the menu's navigation events)
    kSecondary,  // gamepad right stick, keyboard Shift + arrows (navigation events while Shift is held)
};

struct SliderSpec {
    std::string name = "Slider";  // GameObject name suffix
    int min = 0;
    int max = 100;
    int step = 1;   // values are min + k * step (clamped to max)
    int value = 0;  // initial value (snapped + clamped)
    // Text above the bar for a value ("Collect above: 20 adults"); unset -> the number.
    std::function<std::string(int)> label;
    // Main thread: the user changed the value (snapped). Not called for the initial value.
    std::function<void(int)> onChange;
    // Side by side: `columns` sliders share one row, this one in `column` (0 = left). The first one
    // (Create without `rowOf`) builds the row in the cost row's slot; the others join it through
    // Create(..., rowOf = that slider).
    int column = 0;
    int columns = 1;
    SliderInput input = SliderInput::kPrimary;
};

class PlotMenuSlider {
public:
    PlotMenuSlider();
    ~PlotMenuSlider();
    PlotMenuSlider(const PlotMenuSlider&) = delete;
    PlotMenuSlider& operator=(const PlotMenuSlider&) = delete;

    // Worker thread: resolve classes / methods by name. false = sliders unavailable (`why`).
    bool Setup(sr2::il2cpp::Resolver& R, const char* module, std::string& why);
    bool Ready() const { return ready_; }

    // Main thread. Build the slider into `contentView` (a PurchaseItemDetailsContentView of a plot
    // purchase menu), replacing any previous one. With `rowOf` (a slider built just before with
    // columns > 1) it goes into that slider's row instead. false = nothing built (logged once).
    bool Create(Il2CppObject* contentView, const SliderSpec& spec, const PlotMenuSlider* rowOf = nullptr);
    // Main thread. Remove the slider (idempotent); the row's builder removes the whole row.
    void Destroy();
    bool Active() const;
    int Value() const;
    // Main thread, every frame (one pointer check while no slider exists).
    void Tick(float unscaledDt);
    // Main thread, from a detour of the menu's grid navigation (UniformGridView.OnNavigate): a left
    // (-1) / right (+1) navigation event while the slider is shown. true = the slider took it (the
    // caller then consumes the event so the grid does not move its selection); false also when the
    // event is for the other input (Shift held or not, SliderInput). A polled press that already
    // stepped is not stepped again; repeats then come from the input module's events.
    bool Step(int dir);
    // The navigation detour is installed (only for the build log).
    void SetNavigationSteps(bool on);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::ui
