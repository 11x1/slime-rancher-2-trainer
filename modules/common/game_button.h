#pragma once
// A game-styled button for runtime uGUI dialogs: a clone of the game's own popup button
// (GameContext.UITemplates.popupPrompt -> PopupPrompt._positiveActionButton / _negativeActionButton, the
// prefab TextButton_NoneSelectable), the look of the confirm popup and of the silo Sell-All receipt.
// Shared by the silo receipt (custom_upgrades), the drone station pills and dialogs (drone_price_limit) and
// anything else that wants the game's button.
//
// What the game's button does by itself (gamecore_scenes bundle + IDA, re/ui.md "TextButton_NoneSelectable"):
//   * Root: Button (no transition, the colours are ButtonAnimator's), ButtonAnimator, EventTrigger
//     (PointerEnter -> ButtonAnimator.OnPointerEnter, PointerExit -> OnPointerExit, Submit -> OnSubmit),
//     DetailsMoreButtonView (label LocalizeStringEvent), InputEventButton, EncapsulateChildren, a reversed
//     HorizontalLayout (spacing -30: the key glyph overlaps the pill's left end), a UI audio component (the
//     "ButtonHover" cue on pointer enter).
//   * Children: "Label Background" (SDF pill, JoyousPink, label 42 pt) and "Icon Background" (SDF, the same
//     pink, around an InputEventDisplay showing the bound key or pad button, 64 px icons).
//   * ButtonAnimator: colours tween the two SDFs (Normal = Selected = Color_JoyousPink (0.87, 0.31, 0.52),
//     MouseOver = Color_DeepBlue (0.12, 0.23, 0.42), Disabled = Color_BoringGrey (0.56, 0.60, 0.68), 0.05 s,
//     DOTween, unscaled time); pointer enter / exit / press play scale bounces (TextButton_RectBounce*).
//     OnPointerEnter only reacts while its own `interactable` copy is true and the button is not "selected";
//     that copy follows Button.interactable in Update, and the disabled colour is applied only when the copy
//     changes AFTER Awake. Setting Button.interactable before the clone is first activated therefore leaves
//     it pink and dead to hover (the silo receipt's Sell with nothing to sell, round 11). SetInteractable
//     calls ButtonAnimator.OnInteractableChanged once the clone is active, so the grey shows.
//
// What the clone gets here: InputEventButton off (it would perform the popup's InputEvent on pointer down /
// up), the label's LocalizeStringEvent off (our text instead), navigation None (a pointer press must not
// select it in the EventSystem: ButtonAnimator.OnSelect would mark it "selected" and stop the hover look), a
// LayoutManager (the game's layout behaviours only run under one; the popup has it on its Panel), and a
// zero-size "slot" parent that carries the scale (the ButtonAnimator bounces the button's own scale).
// The click is polled (no managed delegates): Selectable.IsPressed() last frame, not now, pointer inside.
//
// Key glyph (Spec.key / padGlyph / popupGlyph):
//   key ""   + popupGlyph false: no glyph (Icon Background hidden; the pill alone).
//   key ""   + popupGlyph true : the source button's own binding (the popup's confirm / cancel key, the pad
//                                button while a pad is used), as the game shows it.
//   key "C"  + padGlyph false  : always the key cap "C" (InputEventDisplay off, the first InputIcon set by
//                                hand as InputIcon.SetKey does).
//   key "C"  + padGlyph true   : the source binding's display stays (pad button glyph with a pad), but a
//                                keyboard key cap reads "C" (the silo receipt's Cancel).
//
// Threading: Resolve on the worker thread (by name, idempotent); everything else on the Unity main thread
// inside the caller's fault guard. A native exception marks the owning module faulted (Spec's module).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/il2cpp_api.h"

namespace sr2::il2cpp {
class Resolver;
}

namespace sr2::modules::game_button {

struct Color {
    float r, g, b, a;
};

// The game's colours of the button (ScriptableColor assets of the prefab).
inline constexpr Color kPink{0.871f, 0.310f, 0.522f, 1.0f};      // Color_JoyousPink (normal)
inline constexpr Color kDeepBlue{0.118f, 0.227f, 0.420f, 1.0f};  // Color_DeepBlue (pointer over)
inline constexpr Color kGrey{0.561f, 0.600f, 0.682f, 1.0f};      // Color_BoringGrey (disabled)
inline constexpr Color kTeal{0.063f, 0.675f, 0.757f, 1.0f};      // the menus' "selected" teal (our selected look)

// Worker thread, idempotent. False + why when the popup button can't be used (callers fall back to their
// own drawn buttons). Optional parts (layout, glyph, tint) are reported in `why` too but still return true.
bool Resolve(sr2::il2cpp::Resolver& R, std::string& why);
bool Available();

enum class Source { kPositive, kNegative };  // which popup button is cloned (only the glyph binding differs)

struct Spec {
    std::string name = "sr2mods_GameButton";
    std::string label;
    std::string key;          // key cap text ("C"), "" = see popupGlyph
    bool padGlyph = false;    // with a key: the source binding's display while a pad is used
    bool popupGlyph = false;  // without a key: the source binding's own glyph
    Source source = Source::kNegative;
    float scale = 0.72f;      // popup button (pill 59 high, glyph 78, 42 pt label) -> pill ~42, glyph ~56, 30 pt
    float minWidth = 0.0f;    // minimum label width in parent units (label centred), 0 = fit the label
    const char* module = "game_button";  // fault attribution and log prefix
};

class Button {
public:
    Button() = default;
    ~Button();  // frees the GC handles (posted); never destroys the object (it dies with its parent)
    Button(const Button&) = delete;
    Button& operator=(const Button&) = delete;
    Button(Button&& o) noexcept;
    Button& operator=(Button&& o) noexcept;

    // Main thread. Clones the popup button under `parentTr` (inside a slot object). False + why.
    bool Create(Il2CppObject* parentTr, const Spec& spec, std::string& why);
    bool Exists() const { return slot_ != nullptr; }
    bool Alive() const;

    // Position: the button's pivot (pivotX, pivotY; 0 = left / bottom, 1 = right / top) sits on the point
    // (x, y) measured from the parent's top-left corner, y downwards.
    void Place(float x, float y, float pivotX, float pivotY = 0.5f);
    // General form: the slot anchored at `anchor` (both min and max) of the parent at anchoredPosition `pos`.
    void PlaceAnchored(float anchorX, float anchorY, float posX, float posY, float pivotX, float pivotY);

    void SetLabel(std::string_view text);
    void SetKey(std::string_view key);  // "" hides the glyph (unless popupGlyph)
    void SetInteractable(bool on);      // false = the game's grey, no hover, no click
    // Normal colour (nullptr = the game's pink); the hover colour stays the game's DeepBlue.
    void SetTint(const Color* normal);
    void SetSelected(bool on) { SetTint(on ? &kTeal : nullptr); }
    void SetActive(bool on);

    // Main thread, every frame while it exists: re-applies key / label / placement for a few frames after a
    // change (the layout and the InputEventDisplay may still configure themselves) and polls the click.
    // True once per click (pressed, released with the pointer still on it; never while not interactable).
    bool Tick();
    bool Hovered() const;

    // Laid-out size in parent units (scale included); {0, 0} before the first layout.
    void Size(float& w, float& h) const;

    void Destroy();  // destroys the slot (and the button) and frees the handles
    void Forget();   // drops everything without IL2CPP calls (after a fault / the parent died); handles posted

    Il2CppObject* GameObject() const { return go_; }
    Il2CppObject* Slot() const { return slot_; }
    Il2CppObject* Selectable() const { return button_; }  // the uGUI Button

private:
    void Apply();
    void ApplyGlyph();
    void PadGlyphTick();
    void Release();

    std::string module_ = "game_button";
    Il2CppObject* slot_ = nullptr, *go_ = nullptr, *button_ = nullptr, *animator_ = nullptr, *label_ = nullptr;
    Il2CppObject* origNormal_ = nullptr, *origSelected_ = nullptr;  // the animator's own ScriptableColors (pink)
    Il2CppObject* iconBg_ = nullptr, *display_ = nullptr;   // "Icon Background" (GameObject), its InputEventDisplay
    std::vector<Il2CppObject*> icons_;                      // InputIcon components of the glyph
    Il2CppObject* padKeyText_ = nullptr, *padKeyRoot_ = nullptr;  // padGlyph: first icon's key text / root
    std::vector<Il2CppGCHandle> handles_;
    std::string label_text_, key_;
    bool padGlyph_ = false, popupGlyph_ = false;
    bool interactable_ = true, colorDirty_ = true, wasPressed_ = false;
    bool tinted_ = false;
    Color tint_{};
    float anchor_[2] = {0, 1}, pos_[2] = {0, 0}, pivot_[2] = {0.5f, 0.5f};
    float scale_ = 1.0f;
    int reapply_ = 0;
};

}  // namespace sr2::modules::game_button
