#pragma once
// custom_upgrades: a reusable integer slider for the game's plot purchase menu (LandPlotUIRoot)
// details panel, built at runtime from uGUI parts (no managed code of ours).
//
// Layout: one root GameObject "sr2mods_Slider_<name>" placed into the bound
// PurchaseItemDetailsContentView, in the slot of its cost row (CraftCostView; the game hides that row
// for owned items): with a LayoutGroup on the row's parent it is inserted right after the row with a
// LayoutElement, else it copies the row's RectTransform. Inside: a TextMeshPro label (font, material,
// size and colour copied from the panel's description text) above a UnityEngine.UI.Slider
// ("Background" / "Fill Area/Fill" / "Handle Slide Area/Handle", the standard uGUI hierarchy). The
// game's look comes from a SliderStyle asset (MonomiPark.SlimeRancher.UI.UIStyling) applied by the
// game's own SliderStyler component; without a loaded SliderStyle plain game-like colours are used.
//
// Input: mouse click / drag through the menu's own GraphicRaycaster (Unity's Slider). Gamepad d-pad /
// left stick and keyboard arrows left / right step the value (with key repeat) while the slider is
// shown, interactable and no PopupPrompt is open. The EventSystem selection a mouse drag moves onto
// the slider is handed back to the previously selected object when the button is released, so the
// menu's gamepad navigation keeps working. The value is read by polling (no managed delegates).
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
    // purchase menu), replacing any previous one. false = nothing built (logged once).
    bool Create(Il2CppObject* contentView, const SliderSpec& spec);
    // Main thread. Remove the slider (idempotent).
    void Destroy();
    bool Active() const;
    int Value() const;
    // Main thread, every frame (one pointer check while no slider exists).
    void Tick(float unscaledDt);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
};

}  // namespace sr2::modules::custom_upgrades::ui
