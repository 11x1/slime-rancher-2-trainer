// drone_price_limit: Quantum Drone station price-limit action + dialog (see station_dialog.h).
// SR2 1.3.0, everything by name; RVAs for reference only.
//
// Research (Cpp2IL / Il2CppInspector dump, IDA, the menu scene bundle
// uiscenes_..._menulayer_dronestationmenu.unity read with UnityPy):
//   * The station menu is the scene MenuLayer_DroneStationMenu: DroneUIDisplay (1470 x 980, centred)
//     > DroneMenuRoot (DroneMenuUI) > ContentContainer > TopViewSwitcher > ProgrammingPage
//     (DroneProgramMenu) > PageContainer (RectMask2D) > ProgramViewSwitcher > OptionSubPage >
//     ProgramSettings > ButtonRow > Target / Source / DestinationProgramOption
//     (DroneProgramOptionButton, 445 x 380). DroneProgramMenu.SetViewModel(DroneProgramViewModel)
//     (0x81BE10) binds the page whenever the menu opens for a station.
//   * The "Change" action: each option has PromptContainer (= DroneProgramOptionButton._promptContainer,
//     inactive in the prefab): a pill (SDFRoundedRectangle, cyan (0.32, 0.78, 0.84), radius 50, anchored
//     bottom-left at (132, 0), 264 x 56, EncapsulateChildren minimum width 264, HorizontalLayout) with
//     InputEventDisplay (InputIconGroup with 4 InputIcons: Button image / Key (Border, Background, Text,
//     Image) / DoubleTap) + InputLabel (TMP + LocalizeStringEvent "Change", UI table key
//     263830960522432512). The OptionButton's FocusEventTriggers SetActive(true / false) the pill on
//     focus. InputIcon.SetKey (0x7C6FE0) = buttonRoot off, keyRoot on, keyText enabled / keyImage
//     disabled, keyLocalizeStringEvent disabled, keyText.text = HumanReadableControl.
//     We clone the destination's pill next to it, disable the clone's InputEventDisplay and
//     LocalizeStringEvent, and draw the C key and our label the same way.
//   * The page's legend (DroneProgramInputLegendConfiguration): Start Program, Clear All, Close. In the
//     PlayControls "Paused" map the C key is only MapZoomIn (map screen), so C is free here; the pad
//     right stick click only marks Pedia entries as seen.
//   * Destination: DroneProgramViewModel._destinationForTarget._currentSelection
//     (DroneDestinationOptionEntry).get_Type() (0x815070) = DroneTaskSinkType, PLORT_MARKET = 5. The
//     station: DroneProgramViewModel._station (DroneStationGadgetModel) .actorId (IdentifiableModel,
//     saved as PlacedGadgetV06.ActorId -> stable per save).
//   * Input while the dialog is open: menu navigation / submit / cancel all go through the static
//     FocusManager (FocusManagerBehaviour's move / submit / cancel actions call
//     NavigateInDirection(MoveDirection, InteractionCause, AxisEventData) (0x7926D0),
//     SendSubmitActionToFocus(InputAction) (0x793BE0), SendCancelActionToFocus(InputAction) (0x7939A0)).
//     We detour the three: while the dialog is open they drive the dialog and report "handled";
//     otherwise one atomic load and the original. The page's own hotkeys (Start Program / Clear All)
//     come from InputEvents bound to DroneProgramMenu.HandleActivateDrone / HandleClearAllOptions:
//     detoured to do nothing while the dialog is open.
//   * Cheat sheet: DroneProgramMenu._marketConfig (MarketUIConfiguration._plorts, PlortEntry.IdentType)
//     is the market terminal's plort order (MarketUI uses the same asset type); the ranch terminal's
//     PriceEntry shows item icon + coin icon + amount, so a cell is icon, name, coin + base price. Coin =
//     LookupDirector._currencyList._currencies[0]._icon (the default currency, Newbucks).
//
// Programs dialog (custom_upgrades drone_programs.h): opened by a "[P] Programs" / "[P] Program 2/3" entry
// in the page's own key bar (DroneProgramMenu._inputLegend, next to Start Program / Clear All / Close; a
// legend_hint clone with a transparent click target), key P / pad left stick click. Fallback when the bar
// can't take it: a game button right of the TARGET's "Change" prompt (DroneProgramMenu._targetButton).
// Its dialog shares the frame (BuildShell), the input routing (dialogKind) and the click toggles with
// the price-limit dialog; it is a list of long buttons (see "programs dialog" below). The page's Clear All
// event (Z) is its Delete. Edit calls drone_programs::Run (DroneStation.AssignRanchTask) and then
// DroneProgramViewModel.RevertChangesToLastAppliedProgram (0x822E80): it compares the page's target /
// source / destination with the station's task data and resets the ones that differ, so the page shows
// the program that runs now. Names / icons: the view model's _definitionSource
// (DroneUIDefinitionSource.FindTargetDefinition / FindSourceDefinition / FindDestinationDefinition ->
// DroneUIProgramDefinition<T> _displayName / _icon; our sources' names through text_override), a
// SINGLE target shows its IdentifiableType's icon and localized name. While the station menu is open drone_programs::NoteMenuOpen pauses
// that station's rotation, and FollowStation keeps the page on the station's program: closing the menu
// asks to discard "changes" (DroneUIDisplay.OnCancel -> _unappliedChangesConfirm) whenever the page
// differs from the station's task data, also when the station changed and not the page.
//
// Pointer input without managed delegates: every clickable part (our prompt, mode buttons, Close, the
// dimmed backdrop) carries a UnityEngine.UI.Toggle (navigation None, no transition) that we poll and
// reset. The dialog root has its own Canvas (sorted above the menu) + GraphicRaycaster.
//
// Lifetime: the prompt and the dialog are children of the menu, so they die with it; we hold GC handles
// to everything we touch across frames and free them only from posted main-thread jobs.

#include "modules/drone_price_limit/station_dialog.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "core/detour_guard.h"
#include "core/il2cpp_api.h"
#include "core/log.h"
#include "core/main_thread.h"
#include "core/module.h"
#include "core/resolver.h"
#include "core/text_override.h"
#include "core/unity.h"
#include "modules/common/game_button.h"
#include "modules/common/legend_hint.h"
#include "modules/custom_upgrades/drone_programs.h"
#include "modules/custom_upgrades/plot_menu_slider.h"
#include "modules/minimap/ugui.h"

namespace sr2::modules::drone_price_limit {

std::string Describe(const StationLimit& l) {
    if (!l.Active()) return "no limit";
    if (l.mode == LimitMode::kFraction) return std::format("{}% of the base price", l.value);
    return std::format("{} newbucks", l.value);
}

std::string Serialize(const StationLimit& l) {
    if (!l.Active()) return {};
    return std::format("{}:{}", l.mode == LimitMode::kFraction ? "fraction" : "price", l.value);
}

bool Parse(const std::string& s, StationLimit& out) {
    out = {};
    if (s.empty()) return true;
    const size_t c = s.find(':');
    if (c == std::string::npos) return false;
    const std::string kind = s.substr(0, c);
    int v = 0;
    try {
        v = std::stoi(s.substr(c + 1));
    } catch (...) {
        return false;
    }
    if (kind == "price") out.mode = LimitMode::kPrice;
    else if (kind == "fraction") out.mode = LimitMode::kFraction;
    else return false;
    out.value = std::clamp(v, 0, 1000000);
    return true;
}

namespace {

namespace unity = sr2::core::unity;
namespace main_thread = sr2::core::main_thread;
namespace sui = sr2::modules::custom_upgrades::ui;
namespace progs = sr2::modules::custom_upgrades::drone_programs;
namespace drones = sr2::modules::custom_upgrades::drones;
namespace gbn = sr2::modules::game_button;
using sr2::modules::custom_upgrades::drones::ProgramData;
using sr2::il2cpp::api;
using sr2::il2cpp::Resolver;

constexpr const char* kImage = "Assembly-CSharp";
constexpr const char* kCore = "UnityEngine.CoreModule";
constexpr const char* kUi = "UnityEngine.UI";
constexpr const char* kTmp = "Unity.TextMeshPro";
constexpr const char* kInput = "Unity.InputSystem";
constexpr const char* kDroneUiNs = "MonomiPark.SlimeRancher.UI.Drone";
constexpr const char* kDroneDataNs = "MonomiPark.SlimeRancher.UI.Drone.Data";

constexpr int kSinkPlortMarket = 5;   // DroneTaskSinkType.PLORT_MARKET
constexpr int kMoveLeft = 0, kMoveUp = 1, kMoveRight = 2, kMoveDown = 3;  // EventSystems.MoveDirection
constexpr int kAlignLeft = 513, kAlignCenter = 514, kAlignRight = 516, kAlignTopLeft = 257;  // TMP
constexpr int kStateFrames = 10;      // menu state (destination / station) re-read every N frames
constexpr int kReapplyFrames = 4;     // re-assert the cloned prompt's key / label for a few frames
constexpr float kPillPadX = 22.0f;    // price-limit pill: inner padding left and right of its key + label
constexpr float kPillScale = 0.8f;    // game buttons on the page (popup button: pill 59, glyph 78 -> 47 / 62)
constexpr float kDialogScale = 0.72f; // game buttons in the dialogs (pill ~42, glyph ~56 = kRowH)
constexpr uint64_t kOtherTabMs = 120000;  // another tab of the station menu counts as "open" this long

struct Vec2 {
    float x, y;
};
struct Color {
    float r, g, b, a;
};

// The drone menu's palette (scene data).
constexpr Color kNavy{0.267f, 0.325f, 0.478f, 1.0f};      // text
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};           // content panel
constexpr Color kLavender{0.737f, 0.686f, 0.824f, 1.0f};  // content border
constexpr Color kBacker{0.875f, 0.902f, 0.945f, 1.0f};    // inner boxes
constexpr Color kTeal{0.063f, 0.675f, 0.757f, 1.0f};      // highlight / selected
constexpr Color kCyan{0.322f, 0.776f, 0.839f, 1.0f};      // prompt pill

// Dialog layout (DroneUIDisplay units).
constexpr float kPad = 40.0f;
constexpr float kPanelW = 1300.0f;
constexpr float kRowH = 56.0f;
constexpr float kCellH = 62.0f;
constexpr float kCellGap = 6.0f;
constexpr float kProgRowH = 80.0f;  // programs dialog: one program
constexpr float kProgRowGap = 10.0f;
constexpr float kProgSelW = 480.0f;  // the options selector: programs dialog (prefab 280: "After each delivery" wrapped)
constexpr float kLimitSelW = 480.0f; // price-limit dialog ("Fraction of base price")
constexpr float kSelArrowPad = 30.0f;  // selector arrows: centre this far from the pill's ends
constexpr int kTargetSingle = 1;    // DroneTaskTargetType.SINGLE
constexpr int kDlgLimit = 1, kDlgPrograms = 2;  // which dialog is open

std::atomic<bool> g_open{false};  // dialog open: the FocusManager detours route input to it

}  // namespace

struct StationUi::Impl {
    const char* module = "drone_price_limit";
    DialogHost host;

    // ---- classes ----
    Il2CppClass* goClass = nullptr, *rtClass = nullptr, *imageClass = nullptr, *toggleClass = nullptr;
    Il2CppClass* canvasClass = nullptr, *raycasterClass = nullptr, *tmpTextClass = nullptr, *tmpUguiClass = nullptr;
    Il2CppClass* sdfRectClass = nullptr, *lseClass = nullptr, *lsClass = nullptr;
    Il2CppClass* menuClass = nullptr, *optionButtonClass = nullptr, *displayClass = nullptr, *entryClass = nullptr;
    Il2CppClass* inputIconClass = nullptr, *inputEventDisplayClass = nullptr, *encapsulateClass = nullptr;
    Il2CppClass* identClass = nullptr, *vmClass = nullptr, *destVmClass = nullptr, *stationModelClass = nullptr;

    // ---- offsets ----
    size_t offMenuVm = 0, offMenuDest = 0, offMenuPicker = 0, offMenuHeader = 0, offMenuDesc = 0, offMenuMarket = 0;
    size_t offOptPrompt = 0, offVmDest = 0, offVmStation = 0, offDestSel = 0, offActorId = 0;
    size_t offMenuTarget = 0, offVmDefs = 0;
    size_t offMarketPlorts = 0, offPlortEntryType = 0, offIdentIcon = 0, offIdentName = 0, offIdentIsPlort = 0;
    size_t offIconButton = 0, offIconKey = 0, offIconKeyText = 0, offIconKeyImage = 0, offIconKeyLse = 0, offIconDouble = 0;
    size_t offEncMinW = 0, offEncPadX = 0;  // optional
    // optional: the key glyph of the silo receipt (the game's popup button glyph, see UsePopupGlyph)
    Il2CppClass* popupClass = nullptr;
    size_t offTplPopup = 0, offPopupNegative = 0;
    const MethodInfo* gcGetTemplates = nullptr, *trGetSibling = nullptr, *trSetSibling = nullptr;
    bool popupGlyphOk = false, loggedGlyph = false;
    // optional: the game's popup button (modules/common/game_button.h) for the pills and every dialog button
    bool gameButtonsOk = false, loggedGameButtonMiss = false;
    const MethodInfo* rtGetAnchorMin = nullptr, *rtGetPivot = nullptr;
    size_t offGcLookup = 0, offLookupCurrencies = 0, offCurrencyArr = 0, offCurrencyIcon = 0;  // optional (coin)

    // ---- methods ----
    const MethodInfo* goCtor = nullptr, *goAddComponent = nullptr, *goGetLayer = nullptr, *goSetLayer = nullptr;
    const MethodInfo* trSetParent = nullptr, *trFind = nullptr, *trGetChild = nullptr, *trChildCount = nullptr;
    const MethodInfo* rtSetAnchorMin = nullptr, *rtSetAnchorMax = nullptr, *rtSetPivot = nullptr, *rtSetSize = nullptr,
                    *rtSetPos = nullptr, *rtGetSize = nullptr, *rtGetPos = nullptr;
    const MethodInfo* objDestroy = nullptr, *behSetEnabled = nullptr, *compInParent = nullptr;
    const MethodInfo* gfxSetColor = nullptr, *gfxSetRaycast = nullptr, *imgSetSprite = nullptr, *imgSetPreserve = nullptr;
    const MethodInfo* tmpSetText = nullptr, *tmpGetFont = nullptr, *tmpSetFont = nullptr, *tmpGetMat = nullptr,
                    *tmpSetMat = nullptr, *tmpSetSize = nullptr, *tmpSetColor = nullptr, *tmpSetAlign = nullptr,
                    *tmpSetWrap = nullptr, *tmpSetOverflow = nullptr, *tmpGetText = nullptr;
    const MethodInfo* cvGetOrder = nullptr, *cvSetOrder = nullptr, *cvGetLayer = nullptr, *cvSetLayer = nullptr,
                    *cvSetOverride = nullptr;
    const MethodInfo* sdfSetRadius = nullptr, *sdfSetBorder = nullptr, *sdfSetBorderColor = nullptr;
    const MethodInfo* tglGetOn = nullptr, *tglSetOnQuiet = nullptr, *selSetTransition = nullptr, *selSetNav = nullptr;
    const MethodInfo* lsGet = nullptr, *entryGetType = nullptr, *gcInstance = nullptr;
    const MethodInfo* kbCurrent = nullptr, *kbC = nullptr, *padCurrent = nullptr, *padR3 = nullptr, *btnPressedThisFrame = nullptr;
    const MethodInfo* kbP = nullptr, *padL3 = nullptr, *vmRevert = nullptr, *vmHasChanges = nullptr;
    size_t offMenuActiveSprite = 0;  // optional: DroneProgramMenu._activeStateSprite (the page's "active" icon)
    using ObjectNewFn = Il2CppObject* (*)(Il2CppClass*);
    ObjectNewFn objectNew = nullptr;

    // ---- live state (main thread) ----
    struct Held {  // object + GC handle (freed by a posted job)
        Il2CppObject* p = nullptr;
        Il2CppGCHandle h = nullptr;
    };
    Held menu;                 // DroneProgramMenu last bound
    Il2CppObject* boundMenu = nullptr;  // raw pointer from the SetViewModel hook (identity, set inside the hook)
    std::vector<Il2CppGCHandle> dialogHandles;
    // A cloned "Change" prompt with our key and label (price limit: next to the destination's; programs:
    // next to the target's).
    struct Pill {
        Il2CppObject* go = nullptr, *toggle = nullptr, *label = nullptr, *keyText = nullptr;
        Il2CppObject* orig = nullptr;          // the option's PromptContainer (RectTransform) we cloned
        std::vector<Il2CppObject*> icons;      // InputIcon transforms of the clone
        Il2CppObject* ied = nullptr, *group = nullptr;  // their parents (Transforms)
        std::vector<Il2CppGCHandle> handles;
        int reapply = 0;
        bool shown = false, logged = false, loggedFail = false;
        std::string text;                      // label last set
        std::string key, want;                 // dialog pills: key glyph ("" = none) and label to re-assert
        gbn::Button gb;                        // the game's button (gb.Exists(): used instead of the clone)
        float placed[4] = {-1e9f, -1e9f, -1e9f, -1e9f};  // gb: last anchor x / y and position x / y
    };
    Pill limitPill, progPill;
    std::vector<Pill> dialogPills;  // fallback dialog buttons: clones of the Change prompt
    std::vector<gbn::Button> dialogButtons;  // the dialogs' game buttons (index = closeGb / PillButton fallbacks)
    std::vector<char> dialogClicks;          // their clicks this frame (Tick polls every one once per frame)
    int closeGb = -1;
    Held menuDisplay;               // the menu's DroneUIDisplay (open = any tab of the station menu shown)
    bool displayActive = false;
    uint64_t lastPageMs = 0;        // the program page was last seen active (GetTickCount64)
    // The station's program as last seen while its menu was open (the page follows it, see FollowStation).
    ProgramData pageProg;
    int64_t pageStation = 0;
    bool pageProgKnown = false;
    int stateCountdown = 0;
    bool marketSelected = false, pageActive = false, menuActive = false;
    int64_t station = 0, labelStation = 0;
    bool haveStation = false;
    bool loggedOpen = false, loggedOpenFail = false, loggedProgOpen = false;
    int dialogKind = 0;  // kDlgLimit / kDlgPrograms while g_open

    // dialog
    Il2CppObject* root = nullptr, *panelRt = nullptr, *blockerToggle = nullptr, *closeToggle = nullptr;
    Il2CppObject* modePrev = nullptr, *modeNext = nullptr, *modeValue = nullptr;  // "Limit by" selector (toggles, TMP)
    Il2CppObject* focusFrame = nullptr, *noteText = nullptr;
    struct Cell {
        Il2CppObject* price = nullptr;  // TMP
        int base = 0;
    };
    std::vector<Cell> cells;
    Il2CppObject* titleSrc = nullptr, *bodySrc = nullptr;  // TMP style sources (menu labels), this open only
    sui::PlotMenuSlider slider;
    bool sliderReady = false;
    float sliderY = 0.0f, modeRowY = 0.0f;
    int64_t openStation = 0;
    LimitMode mode = LimitMode::kPrice;
    int values[2] = {0, 0};
    int focusRow = 1;  // 0 = mode, 1 = value
    int maxPrice = 100;

    // programs dialog (round 14): rows that start with the game's buttons - per program [Space] Edit and
    // [Z] Delete, then [Space] Add, then "Next program" with a clone of the game's options selector. The focused
    // button shows the game's hover look (game_button SetFocused); Up / Down move between rows, Left / Right
    // between Edit and Delete (or step the selector).
    enum ItemKind { kItemProgram = 1, kItemAdd, kItemMode };
    struct PItem {
        int kind = 0, index = 0;           // index: the program (kItemProgram)
        int gb[2] = {-1, -1};              // its game buttons (dialogButtons index): Edit / Add, Delete
        Il2CppObject* tgl[2] = {};         // fallback buttons (Change prompt clones): their click toggles
        Il2CppObject* ring = nullptr;      // focus ring behind the row (selector row, fallback buttons)
        int delBtn = -1;                   // the Delete game button (also the greyed one of a single program)
        Il2CppObject* prev = nullptr, *next = nullptr;  // the selector's arrows (mode)
        int Cols() const { return (gb[1] >= 0 || tgl[1]) ? 2 : 1; }
    };
    std::vector<PItem> pItems;
    int pFocus = 0, pCol = 0, pRunning = 0, pCount = 0, pHover = -1;
    int pLayoutFrames = 0;                 // Delete buttons follow their Edit button's laid-out width
    int pMode = 0;                         // progs::Mode shown by the selector
    bool pExplorer = false;                // the station is in Explorer mode (programs = targets only)
    Il2CppObject* pHint = nullptr;         // the Add row's text (TMP)
    Il2CppObject* pModeText = nullptr;     // the selector's value (TMP)
    bool pDeletePending = false;           // the page's Clear All event (Z / its pad button) while open
    uint64_t pLastDeleteMs = 0;
    const MethodInfo* selPointerInside = nullptr, *kbZ = nullptr;
    bool loggedNav = false;                // the first navigation input that reached a dialog (log once)
    // The game's options-menu selector ("< value >"): PresetOptionsItemDefinition._controlPrefab with an
    // OptionsPresetSelector (found once per session), cloned with its behaviour off.
    Il2CppClass* presetDefClass = nullptr, *presetSelClass = nullptr;
    size_t offDefPrefab = 0, offSelPrev = 0, offSelNext = 0, offSelText = 0;
    const MethodInfo* trSetLocalScale = nullptr, *rtGetRect = nullptr;
    Held selPrefab;
    bool selSearched = false, loggedSelector = false;
    // "Save Program" (round 14): the page's Start Program key bar entry while custom upgrades are on.
    size_t offMenuActivate = 0, offHintConfig = 0, offHintLabelText = 0, offCfgLabel = 0;
    Held saveLabel;                        // the entry's LocalizedString registered in text_override
    Il2CppObject* saveText = nullptr;      // the entry's label TMP (last seen)
    std::string saveOrig;                  // its text before
    bool saveLogged = false;

    // ---- the [P] Programs entry in the page's key bar (round 13: "Move the 'Program X/Y' to the same row as
    // '[X] Start Program', '[Z] Clear All', '[ESC] Close'") ----
    bool legendOk = false;                 // legend_hint resolved and DroneProgramMenu._inputLegend known
    size_t offMenuLegend = 0;
    Il2CppClass* layoutElemClass = nullptr;
    const MethodInfo* leSetIgnore = nullptr;
    legend_hint::Clone progHint;
    Il2CppObject* progHintToggle = nullptr;  // click target over the entry (child of the clone)
    std::vector<Il2CppGCHandle> progHintHandles;
    std::string progHintText;
    uint64_t progHintSince = 0;            // wanted since (no bar entry to clone yet)
    bool progHintFailed = false, progHintLogged = false;

    // ---- helpers ----
    bool Faulted() const { return sr2::core::ModuleFaulted(module); }
    Il2CppObject* Call(const MethodInfo* m, void* obj, std::initializer_list<void*> args, const char* what,
                       bool* ok = nullptr) {
        if (!m) {
            if (ok) *ok = false;
            return nullptr;
        }
        std::vector<void*> params(args);
        std::string err;
        Il2CppObject* r = Resolver::Get().Invoke(m, obj, params.empty() ? nullptr : params.data(), &err);
        const bool good = err.empty();
        if (ok) *ok = good;
        if (!good) {
            if (err.rfind("native exception", 0) == 0) sr2::core::MarkModuleFaulted(module, std::format("{}: {}", what, err));
            else SR2_WARN("[{}] station ui: {} failed: {}", module, what, err);
        }
        return r;
    }
    template <class T>
    static T Unbox(Il2CppObject* boxed, T def = T{}) {
        return boxed ? *reinterpret_cast<T*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*)) : def;
    }
    bool IsA(Il2CppObject* obj, Il2CppClass* k) const {
        const auto& a = api();
        if (!obj || !k || !a.il2cpp_object_get_class) return false;
        Il2CppClass* c = a.il2cpp_object_get_class(obj);
        if (c == k) return true;
        return c && a.il2cpp_class_is_assignable_from && a.il2cpp_class_is_assignable_from(k, c);
    }
    static void Keep(std::vector<Il2CppGCHandle>& into, Il2CppObject* o) {
        if (Il2CppGCHandle h = o ? unity::NewGCHandle(o) : nullptr) into.push_back(h);
    }
    static void FreeLater(std::vector<Il2CppGCHandle>& handles) {
        std::vector<Il2CppGCHandle> doomed;
        doomed.swap(handles);
        if (!doomed.empty()) main_thread::Post([doomed] {
            for (Il2CppGCHandle h : doomed) unity::FreeGCHandle(h);
        });
    }
    static void Release(Held& h) {
        h.p = nullptr;
        if (Il2CppGCHandle g = h.h) {
            h.h = nullptr;
            main_thread::Post([g] { unity::FreeGCHandle(g); });
        }
    }
    Il2CppObject* AddComponent(Il2CppObject* go, Il2CppClass* k, const char* what) {
        if (!unity::IsAlive(go) || !k) return nullptr;
        bool ok = false;
        Il2CppObject* c = Call(goAddComponent, go, {unity::TypeOf(k)}, what, &ok);
        return ok && unity::IsAlive(c) && IsA(c, k) ? c : nullptr;
    }
    void SetVec(const MethodInfo* m, Il2CppObject* rt, Vec2 v, const char* what) { Call(m, rt, {&v}, what); }
    Vec2 GetVec(const MethodInfo* m, Il2CppObject* rt, const char* what) {
        bool ok = false;
        Il2CppObject* b = Call(m, rt, {}, what, &ok);
        return ok ? Unbox<Vec2>(b) : Vec2{0, 0};
    }
    void Rect(Il2CppObject* rt, Vec2 amin, Vec2 amax, Vec2 pivot, Vec2 pos, Vec2 size) {
        SetVec(rtSetAnchorMin, rt, amin, "RectTransform.set_anchorMin");
        SetVec(rtSetAnchorMax, rt, amax, "RectTransform.set_anchorMax");
        SetVec(rtSetPivot, rt, pivot, "RectTransform.set_pivot");
        SetVec(rtSetSize, rt, size, "RectTransform.set_sizeDelta");
        SetVec(rtSetPos, rt, pos, "RectTransform.set_anchoredPosition");
    }
    // Top-left anchored rect inside the parent (y grows downwards).
    void At(Il2CppObject* rt, float x, float y, float w, float h) {
        Rect(rt, {0, 1}, {0, 1}, {0, 1}, {x, -y}, {w, h});
    }
    Il2CppObject* NewUi(const std::string& name, Il2CppObject* parentTr, int layer) {
        Il2CppObject* go = objectNew ? objectNew(goClass) : nullptr;
        if (!go) return nullptr;
        bool ok = false;
        Call(goCtor, go, {unity::NewString(name)}, "GameObject..ctor", &ok);
        if (!ok || !unity::IsAlive(go)) return nullptr;
        if (!AddComponent(go, rtClass, "AddComponent(RectTransform)")) {
            Destroy(go);
            return nullptr;
        }
        bool stays = false;
        Call(trSetParent, unity::TransformOf(go), {parentTr, &stays}, "Transform.SetParent", &ok);
        if (!ok) {
            Destroy(go);
            return nullptr;
        }
        if (layer >= 0) Call(goSetLayer, go, {&layer}, "GameObject.set_layer");
        return go;
    }
    void Destroy(Il2CppObject* go) {
        if (unity::IsAlive(go)) Call(objDestroy, nullptr, {go}, "Object.Destroy");
    }
    void SetEnabled(Il2CppObject* behaviour, bool on) {
        if (unity::IsAlive(behaviour)) Call(behSetEnabled, behaviour, {&on}, "Behaviour.set_enabled");
    }
    void SetColor(Il2CppObject* g, Color c) {
        if (unity::IsAlive(g)) Call(gfxSetColor, g, {&c}, "Graphic.set_color");
    }
    void SetRaycast(Il2CppObject* g, bool on) {
        if (unity::IsAlive(g)) Call(gfxSetRaycast, g, {&on}, "Graphic.set_raycastTarget");
    }
    void SetText(Il2CppObject* tmp, const std::string& s) {
        if (unity::IsAlive(tmp)) Call(tmpSetText, tmp, {unity::NewString(s)}, "TMP_Text.set_text");
    }
    // Rounded shape (the game's SDFRoundedRectangle, else a plain Image).
    Il2CppObject* Shape(Il2CppObject* go, Color c, float radius, float border = 0.0f, Color borderColor = {}) {
        if (sdfRectClass) {
            if (Il2CppObject* g = AddComponent(go, sdfRectClass, "AddComponent(SDFRoundedRectangle)")) {
                SetColor(g, c);
                Call(sdfSetRadius, g, {&radius}, "SDFRoundedRectangle.SetCornerRadius");
                if (border > 0.0f && sdfSetBorder && sdfSetBorderColor) {
                    Call(sdfSetBorder, g, {&border}, "SDFGraphic.set_BorderSize");
                    Call(sdfSetBorderColor, g, {&borderColor}, "SDFGraphic.set_BorderColor");
                }
                return g;
            }
        }
        Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
        SetColor(img, c);
        return img;
    }
    Il2CppObject* Box(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, Color c, float radius,
                      float border = 0.0f, Color borderColor = {}) {
        Il2CppObject* go = NewUi(name, parentRt, -1);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* g = Shape(go, c, radius, border, borderColor);
        SetRaycast(g, false);
        return go;
    }
    // TMP text; style (font / material) copied from `src` when given.
    Il2CppObject* Text(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, const std::string& s,
                       float size, Color col, int align, Il2CppObject* src, bool wrap = false, bool ellipsis = false) {
        Il2CppObject* go = NewUi(name, parentRt, -1);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* tmp = AddComponent(go, tmpUguiClass, "AddComponent(TextMeshProUGUI)");
        if (!tmp) return nullptr;
        if (unity::IsAlive(src)) {
            if (Il2CppObject* font = Call(tmpGetFont, src, {}, "TMP_Text.get_font"); unity::IsAlive(font))
                Call(tmpSetFont, tmp, {font}, "TMP_Text.set_font");
            if (Il2CppObject* mat = Call(tmpGetMat, src, {}, "TMP_Text.get_fontSharedMaterial"); unity::IsAlive(mat))
                Call(tmpSetMat, tmp, {mat}, "TMP_Text.set_fontSharedMaterial");
        }
        Call(tmpSetSize, tmp, {&size}, "TMP_Text.set_fontSize");
        Call(tmpSetColor, tmp, {&col}, "TMP_Text.set_color");
        int32_t a = align, wrapMode = wrap ? 1 : 0, overflow = 1;  // TextWrappingModes.Normal / NoWrap; Ellipsis
        Call(tmpSetAlign, tmp, {&a}, "TMP_Text.set_alignment");
        if (tmpSetWrap) Call(tmpSetWrap, tmp, {&wrapMode}, "TMP_Text.set_textWrappingMode");
        if (ellipsis && tmpSetOverflow) Call(tmpSetOverflow, tmp, {&overflow}, "TMP_Text.set_overflowMode");
        SetRaycast(tmp, false);
        SetText(tmp, s);
        return tmp;
    }
    Il2CppObject* Sprite(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, Il2CppObject* sprite) {
        if (!unity::IsAlive(sprite)) return nullptr;
        Il2CppObject* go = NewUi(name, parentRt, -1);
        if (!go) return nullptr;
        At(unity::TransformOf(go), x, y, w, h);
        Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
        if (!img) return nullptr;
        Call(imgSetSprite, img, {sprite}, "Image.set_sprite");
        bool keep = true;
        if (imgSetPreserve) Call(imgSetPreserve, img, {&keep}, "Image.set_preserveAspect");
        SetRaycast(img, false);
        return img;
    }
    // A pollable click target on `go` (whose graphics must be raycast targets).
    Il2CppObject* Clickable(Il2CppObject* go) {
        Il2CppObject* t = AddComponent(go, toggleClass, "AddComponent(Toggle)");
        if (!t) return nullptr;
        int32_t none = 0;  // Selectable.Transition.None
        alignas(8) unsigned char nav[64] = {};  // Navigation.Mode.None
        Call(selSetTransition, t, {&none}, "Selectable.set_transition");
        Call(selSetNav, t, {nav}, "Selectable.set_navigation");
        bool off = false;
        Call(tglSetOnQuiet, t, {&off}, "Toggle.SetIsOnWithoutNotify");
        return t;
    }
    // A dialog button's click this frame: its game button (index gbi) or its toggle.
    bool Clicked(Il2CppObject* toggle, int gbi) {
        if (gbi >= 0) return gbi < static_cast<int>(dialogClicks.size()) && dialogClicks[static_cast<size_t>(gbi)];
        return Clicked(toggle);
    }
    // true once per click.
    bool Clicked(Il2CppObject* toggle) {
        if (!unity::IsAlive(toggle)) return false;
        bool ok = false;
        const bool on = Unbox<bool>(Call(tglGetOn, toggle, {}, "Toggle.get_isOn", &ok));
        if (!ok || !on) return false;
        bool off = false;
        Call(tglSetOnQuiet, toggle, {&off}, "Toggle.SetIsOnWithoutNotify");
        return true;
    }
    Il2CppObject* Find(Il2CppObject* tr, const char* path) {
        if (!unity::IsAlive(tr)) return nullptr;
        bool ok = false;
        Il2CppObject* r = Call(trFind, tr, {unity::NewString(path)}, "Transform.Find", &ok);
        return ok && unity::IsAlive(r) ? r : nullptr;
    }
    Il2CppObject* TmpOf(Il2CppObject* goOrComp) {
        if (!unity::IsAlive(goOrComp)) return nullptr;
        Il2CppObject* t = unity::GetComponent(goOrComp, tmpTextClass);
        return unity::IsAlive(t) ? t : nullptr;
    }
    bool KeyPressed(const MethodInfo* current, const MethodInfo* control) {
        if (!current || !control || !btnPressedThisFrame) return false;
        bool ok = false;
        Il2CppObject* dev = Call(current, nullptr, {}, "InputDevice.get_current", &ok);
        if (!ok || !dev) return false;
        Il2CppObject* ctl = Call(control, dev, {}, "InputDevice control getter", &ok);
        if (!ok || !ctl) return false;
        return Unbox<bool>(Call(btnPressedThisFrame, ctl, {}, "ButtonControl.get_wasPressedThisFrame", &ok)) && ok;
    }
    std::string Localized(Il2CppObject* ls) {
        if (!ls || !lsGet || !IsA(ls, lsClass)) return {};
        bool ok = false;
        Il2CppObject* r = Call(lsGet, ls, {}, "LocalizedString.GetLocalizedString", &ok);
        return ok && r ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(r)) : std::string();
    }
    Il2CppObject* CoinSprite() {
        if (!gcInstance || !offGcLookup || !offLookupCurrencies || !offCurrencyArr || !offCurrencyIcon) return nullptr;
        bool ok = false;
        Il2CppObject* gc = Call(gcInstance, nullptr, {}, "GameContext.Instance", &ok);
        if (!ok || !unity::IsAlive(gc)) return nullptr;
        Il2CppObject* lookup = unity::Field<Il2CppObject*>(gc, offGcLookup);
        if (!unity::IsAlive(lookup)) return nullptr;
        Il2CppObject* list = unity::Field<Il2CppObject*>(lookup, offLookupCurrencies);
        if (!unity::IsAlive(list)) return nullptr;
        void* arr = unity::Field<void*>(list, offCurrencyArr);
        auto* first = arr && unity::ArrayLength(arr) > 0 ? static_cast<Il2CppObject*>(unity::ArrayAt(arr, 0)) : nullptr;
        Il2CppObject* icon = unity::IsAlive(first) ? unity::Field<Il2CppObject*>(first, offCurrencyIcon) : nullptr;
        return unity::IsAlive(icon) ? icon : nullptr;
    }

    // ---- menu state ----
    void ForgetMenu();
    void ReadState();
    Il2CppObject* StationModel();
    // ---- prompts ----
    bool ClonePrompt(Pill& P, Il2CppObject* orig, Il2CppObject* parentTr, const char* name);
    bool UsePopupGlyph(Pill& P, Il2CppObject* pillTr);
    bool GamePill(Pill& P, Il2CppObject* orig, Il2CppObject* parentTr, const char* name, const char* key, const char* label);
    bool PillClicked(Pill& P);
    void EnsurePill(Pill& P, size_t buttonOff, const char* name, const char* what, const char* key);
    void SyncPill(Pill& P, bool want, size_t buttonOff, const char* name, const char* what, const char* key);
    void ApplyPillLook(Pill& P, const char* key);
    void PlacePill(Pill& P);
    void SetPillText(Pill& P, const std::string& text);
    void UpdatePillLabel();
    void UpdateProgramsLabel();
    void DestroyPill(Pill& P);
    // ---- dialogs ----
    bool BuildShell(const char* name, float panelH, int& layer);
    Il2CppObject* Button(Il2CppObject* prt, const char* name, float x, float y, float w, float h, const std::string& label,
                         Color bg, Color fg, int layer, float border = 0.0f);
    Il2CppObject* PillButton(Il2CppObject* prt, const char* name, float x, float y, float w, float h, const char* key,
                             const std::string& label, const Color* bg, const Color* fg, int layer);
    // A dialog button: the game's popup button (returns its dialogButtons index in gbi, toggle nullptr), else
    // PillButton. `selected` = the teal selected look; `right` = (x + w) is its right edge.
    Il2CppObject* DialogButton(Il2CppObject* prt, const char* name, float x, float y, float w, float h, const char* key,
                               const std::string& label, bool selected, int layer, int& gbi, bool right = false,
                               const Color* fallbackBg = nullptr, const Color* fallbackFg = nullptr,
                               const char* control = nullptr);
    void FollowStation();
    bool Open();
    // ---- programs dialog ----
    struct Label {
        Il2CppObject* icon = nullptr;
        std::string name;
    };
    std::string LocalizedText(Il2CppObject* ls);
    Label DefLabel(Il2CppObject* defs, const char* method, int32_t type, const char* what);
    Label TargetLabel(Il2CppObject* defs, const ProgramData& p);
    bool OpenPrograms(int focus);
    Il2CppObject* Row(Il2CppObject* prt, const char* name, float x, float y, float w, float h, bool running, int layer,
                      PItem& it);
    Il2CppObject* ClickArea(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h, int layer);
    Il2CppObject* SelectorPrefab();
    bool CloneSelector(Il2CppObject* rowRt, float selW, float rowH, Il2CppObject*& prev, Il2CppObject*& next,
                       Il2CppObject*& text);
    // The game's selector (else a drawn one) at the right end of a row, selW wide; prev / next = click toggles.
    void Selector(Il2CppObject* rowRt, float rowW, float rowH, float selW, int layer, Il2CppObject*& prev,
                  Il2CppObject*& next, Il2CppObject*& text);
    void SyncSaveLabel(bool want);
    void ProgFocus(int i, int col = -1);
    void ProgPress(const PItem& it, int col);
    void ProgNavigate(int dir);
    void ProgSubmit();
    void ProgEdit(int index);
    void ProgDelete(int index);
    void ProgAdd();
    void ProgStepMode(int dir);
    void ProgInput();
    bool PointerInside(Il2CppObject* selectable);
    void RevertPage();
    void SyncProgHint(bool want);
    void DropProgHint();
    void Close(const char* why);
    void BuildSlider();
    void SetMode(LimitMode m);
    void SetFocus(int row);
    void ShowMode();  // the "Limit by" selector's text
    void UpdateSheet();
    void Changed();
    void Navigate(int dir);
    void Submit();
    void Tick(float dt);
};

namespace {

StationUi::Impl* g_ui = nullptr;
sr2::core::DetourGuard g_bindGuard{"drone_price_limit", "DroneProgramMenu.SetViewModel"};
sr2::core::DetourGuard g_navGuard{"drone_price_limit", "FocusManager (dialog input)"};
sr2::core::DetourGuard g_hotkeyGuard{"drone_price_limit", "DroneProgramMenu hotkeys"};

using BindFn = void (*)(Il2CppObject*, Il2CppObject*, const MethodInfo*);
using NavFn = bool (*)(int32_t, int32_t, Il2CppObject*, const MethodInfo*);
using ActionFn = bool (*)(Il2CppObject*, const MethodInfo*);
using HotkeyFn = void (*)(Il2CppObject*, void*, const MethodInfo*);
BindFn g_origBind = nullptr;
NavFn g_origNav = nullptr;
ActionFn g_origSubmit = nullptr, g_origCancel = nullptr;
HotkeyFn g_origClearAll = nullptr, g_origActivate = nullptr;

void BindDetour(Il2CppObject* self, Il2CppObject* vm, const MethodInfo* mi) {
    g_origBind(self, vm, mi);
    if (!g_ui) return;
    g_bindGuard.Run([&] {
        if (main_thread::IsMainThread()) g_ui->boundMenu = self;  // identity only; pinned in Tick
    });
}

bool NavDetour(int32_t dir, int32_t cause, Il2CppObject* evt, const MethodInfo* mi) {
    if (!g_open.load(std::memory_order_relaxed) || !g_ui) return g_origNav(dir, cause, evt, mi);
    bool handled = false;
    g_navGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        g_ui->Navigate(dir);
        handled = true;
    });
    return handled ? true : g_origNav(dir, cause, evt, mi);
}

bool SubmitDetour(Il2CppObject* action, const MethodInfo* mi) {
    if (!g_open.load(std::memory_order_relaxed) || !g_ui) return g_origSubmit(action, mi);
    bool handled = false;
    g_navGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        g_ui->Submit();
        handled = true;
    });
    return handled ? true : g_origSubmit(action, mi);
}

bool CancelDetour(Il2CppObject* action, const MethodInfo* mi) {
    if (!g_open.load(std::memory_order_relaxed) || !g_ui) return g_origCancel(action, mi);
    bool handled = false;
    g_navGuard.Run([&] {
        if (!main_thread::IsMainThread()) return;
        g_ui->Close("cancel");
        handled = true;
    });
    return handled ? true : g_origCancel(action, mi);
}

// The page's own hotkeys do nothing while the dialog covers the page. Clear All (Z, or its pad button) is
// the programs dialog's Delete: the next Tick deletes the focused program.
void ClearAllDetour(Il2CppObject* self, void* evt, const MethodInfo* mi) {
    if (g_open.load(std::memory_order_relaxed) && g_hotkeyGuard.Active()) {
        if (g_ui && g_ui->dialogKind == kDlgPrograms) g_ui->pDeletePending = true;
        return;
    }
    g_origClearAll(self, evt, mi);
}

void ActivateDetour(Il2CppObject* self, void* evt, const MethodInfo* mi) {
    if (g_open.load(std::memory_order_relaxed) && g_hotkeyGuard.Active()) return;
    g_origActivate(self, evt, mi);
}

}  // namespace

// ---------------------------------------------------------------- setup --

bool StationUi::Setup(sr2::core::ModuleContext& ctx, const char* module, DialogHost host, std::string& why) {
    Resolver& R = ctx.resolver();
    auto* p = new Impl();
    p->module = module;
    p->host = std::move(host);
    auto parentOf = [](Il2CppClass* c) { return api().il2cpp_class_get_parent ? api().il2cpp_class_get_parent(c) : nullptr; };
    auto cls = [&](const char* image, const char* ns, const char* name) -> Il2CppClass* {
        std::string err;
        Il2CppClass* k = R.Class(image, ns, name, &err);
        if (!k && why.empty()) why = err.empty() ? std::format("class {}.{} not found", ns, name) : err;
        return k;
    };
    auto method = [&](Il2CppClass* k, const char* name, int argc, bool required = true) -> const MethodInfo* {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (const MethodInfo* m = R.Method(c, name, argc, &err)) return m;
        if (required && why.empty()) why = std::format("method {}({}) not found", name, argc);
        return nullptr;
    };
    auto byParams = [&](Il2CppClass* k, const char* name, std::initializer_list<std::string_view> params,
                        bool required = true) -> const MethodInfo* {
        std::string err;
        const MethodInfo* m = k ? R.MethodByParams(k, name, params, &err) : nullptr;
        if (!m && required && why.empty()) why = std::format("{}: {}", name, err);
        return m;
    };
    auto off = [&](Il2CppClass* k, const char* field, size_t& out, bool required = true) {
        std::string err;
        for (Il2CppClass* c = k; c; c = parentOf(c))
            if (auto o = R.FieldOffset(c, field, &err)) {
                out = *o;
                return;
            }
        if (required && why.empty()) why = std::format("field {} not found", field);
    };
    auto fail = [&]() {
        delete p;
        return false;
    };

    HMODULE ga = api().module;
    p->objectNew = reinterpret_cast<Impl::ObjectNewFn>(ga ? GetProcAddress(ga, "il2cpp_object_new") : nullptr);
    if (!p->objectNew) why = "il2cpp_object_new export missing";

    p->goClass = cls(kCore, "UnityEngine", "GameObject");
    Il2CppClass* transform = cls(kCore, "UnityEngine", "Transform");
    Il2CppClass* component = cls(kCore, "UnityEngine", "Component");
    Il2CppClass* behaviour = cls(kCore, "UnityEngine", "Behaviour");
    Il2CppClass* uobject = cls(kCore, "UnityEngine", "Object");
    p->rtClass = cls(kCore, "UnityEngine", "RectTransform");
    p->canvasClass = cls("UnityEngine.UIModule", "UnityEngine", "Canvas");
    p->imageClass = cls(kUi, "UnityEngine.UI", "Image");
    Il2CppClass* graphic = cls(kUi, "UnityEngine.UI", "Graphic");
    Il2CppClass* selectable = cls(kUi, "UnityEngine.UI", "Selectable");
    p->toggleClass = cls(kUi, "UnityEngine.UI", "Toggle");
    p->raycasterClass = cls(kUi, "UnityEngine.UI", "GraphicRaycaster");
    p->tmpTextClass = cls(kTmp, "TMPro", "TMP_Text");
    p->tmpUguiClass = cls(kTmp, "TMPro", "TextMeshProUGUI");
    p->lseClass = cls("Unity.Localization", "UnityEngine.Localization.Components", "LocalizeStringEvent");
    p->lsClass = cls("Unity.Localization", "UnityEngine.Localization", "LocalizedString");
    p->menuClass = cls(kImage, kDroneUiNs, "DroneProgramMenu");
    p->optionButtonClass = cls(kImage, kDroneUiNs, "DroneProgramOptionButton");
    p->displayClass = cls(kImage, kDroneUiNs, "DroneUIDisplay");
    p->vmClass = cls(kImage, kDroneDataNs, "DroneProgramViewModel");
    p->destVmClass = cls(kImage, kDroneDataNs, "DroneProgramReadOnlyDestinationViewModel");
    p->stationModelClass = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "DroneStationGadgetModel");
    p->entryClass = cls(kImage, kDroneDataNs, "DroneDestinationOptionEntry");
    Il2CppClass* identModel = cls(kImage, "MonomiPark.SlimeRancher.DataModel", "IdentifiableModel");
    Il2CppClass* marketCfg = cls(kImage, "MonomiPark.SlimeRancher.UI", "MarketUIConfiguration");
    Il2CppClass* plortEntry = cls(kImage, "MonomiPark.SlimeRancher.UI", "PlortEntry");
    p->identClass = cls(kImage, "", "IdentifiableType");
    p->inputIconClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputIcon");
    p->inputEventDisplayClass = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputEventDisplay");
    Il2CppClass* focusMgr = cls(kImage, "MonomiPark.SlimeRancher.UI.Framework.Interaction.Focus", "FocusManager");
    if (!why.empty()) return fail();

    off(p->menuClass, "_viewModel", p->offMenuVm);
    off(p->menuClass, "_destinationButton", p->offMenuDest);
    off(p->menuClass, "_targetButton", p->offMenuTarget);
    off(p->menuClass, "_optionPicker", p->offMenuPicker);
    off(p->menuClass, "_headerLabel", p->offMenuHeader);
    off(p->menuClass, "_descriptionLabel", p->offMenuDesc);
    off(p->menuClass, "_marketConfig", p->offMenuMarket);
    off(p->optionButtonClass, "_promptContainer", p->offOptPrompt);
    off(p->vmClass, "_destinationForTarget", p->offVmDest);
    off(p->vmClass, "_station", p->offVmStation);
    off(p->vmClass, "_definitionSource", p->offVmDefs, false);  // optional: program names / icons in the programs dialog
    off(p->destVmClass, "_currentSelection", p->offDestSel);
    off(identModel, "actorId", p->offActorId);
    off(marketCfg, "_plorts", p->offMarketPlorts);
    off(plortEntry, "IdentType", p->offPlortEntryType);
    off(p->identClass, "icon", p->offIdentIcon);
    off(p->identClass, "localizedName", p->offIdentName);
    off(p->inputIconClass, "_buttonRoot", p->offIconButton);
    off(p->inputIconClass, "_keyRoot", p->offIconKey);
    off(p->inputIconClass, "_keyText", p->offIconKeyText);
    off(p->inputIconClass, "_keyImage", p->offIconKeyImage);
    off(p->inputIconClass, "_keyLocalizeStringEvent", p->offIconKeyLse);
    off(p->inputIconClass, "_doubleTapRoot", p->offIconDouble);
    if (!why.empty()) return fail();

    p->goCtor = byParams(p->goClass, ".ctor", {"System.String"});
    p->goAddComponent = byParams(p->goClass, "AddComponent", {"System.Type"});
    p->goGetLayer = method(p->goClass, "get_layer", 0);
    p->goSetLayer = method(p->goClass, "set_layer", 1);
    p->trSetParent = byParams(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    p->trFind = byParams(transform, "Find", {"System.String"});
    p->trGetChild = method(transform, "GetChild", 1);
    p->trChildCount = method(transform, "get_childCount", 0);
    p->rtSetAnchorMin = method(p->rtClass, "set_anchorMin", 1);
    p->rtSetAnchorMax = method(p->rtClass, "set_anchorMax", 1);
    p->rtSetPivot = method(p->rtClass, "set_pivot", 1);
    p->rtSetSize = method(p->rtClass, "set_sizeDelta", 1);
    p->rtSetPos = method(p->rtClass, "set_anchoredPosition", 1);
    p->rtGetSize = method(p->rtClass, "get_sizeDelta", 0);
    p->rtGetPos = method(p->rtClass, "get_anchoredPosition", 0);
    p->rtGetAnchorMin = method(p->rtClass, "get_anchorMin", 0, false);
    p->rtGetPivot = method(p->rtClass, "get_pivot", 0, false);
    p->objDestroy = byParams(uobject, "Destroy", {"UnityEngine.Object"});
    p->behSetEnabled = method(behaviour, "set_enabled", 1);
    p->compInParent = byParams(component, "GetComponentInParent", {"System.Type", "System.Boolean"});
    p->gfxSetColor = method(graphic, "set_color", 1);
    p->gfxSetRaycast = method(graphic, "set_raycastTarget", 1);
    p->imgSetSprite = method(p->imageClass, "set_sprite", 1);
    p->tmpSetText = method(p->tmpTextClass, "set_text", 1);
    p->tmpGetText = method(p->tmpTextClass, "get_text", 0);
    p->tmpGetFont = method(p->tmpTextClass, "get_font", 0);
    p->tmpSetFont = method(p->tmpTextClass, "set_font", 1);
    p->tmpGetMat = method(p->tmpTextClass, "get_fontSharedMaterial", 0);
    p->tmpSetMat = method(p->tmpTextClass, "set_fontSharedMaterial", 1);
    p->tmpSetSize = method(p->tmpTextClass, "set_fontSize", 1);
    p->tmpSetColor = method(p->tmpTextClass, "set_color", 1);
    p->tmpSetAlign = method(p->tmpTextClass, "set_alignment", 1);
    p->cvGetOrder = method(p->canvasClass, "get_sortingOrder", 0);
    p->cvSetOrder = method(p->canvasClass, "set_sortingOrder", 1);
    p->cvGetLayer = method(p->canvasClass, "get_sortingLayerID", 0);
    p->cvSetLayer = method(p->canvasClass, "set_sortingLayerID", 1);
    p->cvSetOverride = method(p->canvasClass, "set_overrideSorting", 1);
    p->tglGetOn = method(p->toggleClass, "get_isOn", 0);
    p->tglSetOnQuiet = method(p->toggleClass, "SetIsOnWithoutNotify", 1);
    p->selSetTransition = method(selectable, "set_transition", 1);
    p->selSetNav = method(selectable, "set_navigation", 1);
    p->entryGetType = method(p->entryClass, "get_Type", 0);
    const MethodInfo* bindMi = method(p->menuClass, "SetViewModel", 1);
    const MethodInfo* navMi = method(focusMgr, "NavigateInDirection", 3);
    const MethodInfo* submitMi = method(focusMgr, "SendSubmitActionToFocus", 1);
    const MethodInfo* cancelMi = method(focusMgr, "SendCancelActionToFocus", 1);
    const MethodInfo* clearAllMi = method(p->menuClass, "HandleClearAllOptions", 1);
    const MethodInfo* activateMi = method(p->menuClass, "HandleActivateDrone", 1);
    if (!why.empty()) return fail();

    // ---- optional parts ----
    std::string report;
    {
        std::string err;
        p->tmpSetWrap = R.Method(p->tmpTextClass, "set_textWrappingMode", 1, &err);
        p->tmpSetOverflow = R.Method(p->tmpTextClass, "set_overflowMode", 1, &err);
        p->imgSetPreserve = R.Method(p->imageClass, "set_preserveAspect", 1, &err);
        p->lsGet = R.Method(p->lsClass, "GetLocalizedString", 0, &err);
        p->sdfRectClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.SDFUI", "SDFRoundedRectangle", &err);
        if (p->sdfRectClass) {
            p->sdfSetRadius = R.MethodByParams(p->sdfRectClass, "SetCornerRadius", {"System.Single"}, &err);
            Il2CppClass* sdf = parentOf(p->sdfRectClass);  // SDFGraphic
            p->sdfSetBorder = sdf ? R.Method(sdf, "set_BorderSize", 1, &err) : nullptr;
            p->sdfSetBorderColor = sdf ? R.Method(sdf, "set_BorderColor", 1, &err) : nullptr;
            if (!p->sdfSetRadius) p->sdfRectClass = nullptr;
        }
        if (!p->sdfRectClass) report += " SDF shapes: NOT available (plain images);";
        p->encapsulateClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Framework.Layout", "EncapsulateChildren", &err);
        if (p->encapsulateClass)
            if (auto o = R.FieldOffset(p->encapsulateClass, "_minimumWidth", &err)) p->offEncMinW = *o;
        if (p->encapsulateClass)
            if (auto o = R.FieldOffset(p->encapsulateClass, "_horizontalPadding", &err)) p->offEncPadX = *o;
        // Coin icon: GameContext.Instance.LookupDirector._currencyList._currencies[0]._icon.
        Il2CppClass* gameCtx = R.Class(kImage, "", "GameContext", &err);
        Il2CppClass* lookup = R.Class(kImage, "", "LookupDirector", &err);
        Il2CppClass* curList = R.Class(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyList", &err);
        Il2CppClass* curDef = R.Class(kImage, "MonomiPark.SlimeRancher.Economy", "CurrencyDefinition", &err);
        if (gameCtx && lookup && curList && curDef) {
            if (Il2CppClass* single = parentOf(gameCtx)) p->gcInstance = R.Method(single, "get_Instance", 0, &err);
            if (auto o = R.FieldOffset(gameCtx, "<LookupDirector>k__BackingField", &err)) p->offGcLookup = *o;
            if (auto o = R.FieldOffset(lookup, "_currencyList", &err)) p->offLookupCurrencies = *o;
            if (auto o = R.FieldOffset(curList, "_currencies", &err)) p->offCurrencyArr = *o;
            if (auto o = R.FieldOffset(curDef, "_icon", &err)) p->offCurrencyIcon = *o;
        }
        if (!p->gcInstance || !p->offGcLookup || !p->offLookupCurrencies || !p->offCurrencyArr || !p->offCurrencyIcon)
            report += " coin icon: NOT available;";
        // Key glyphs like the silo receipt's: GameContext.UITemplates.popupPrompt -> PopupPrompt
        // ._negativeActionButton -> "Icon Background" (see UsePopupGlyph).
        if (gameCtx && !p->gcInstance)
            if (Il2CppClass* single = parentOf(gameCtx)) p->gcInstance = R.Method(single, "get_Instance", 0, &err);
        Il2CppClass* templates = R.Class(kImage, "MonomiPark.SlimeRancher.UI", "UITemplates", &err);
        p->popupClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Popup", "PopupPrompt", &err);
        p->gcGetTemplates = gameCtx ? R.Method(gameCtx, "get_UITemplates", 0, &err) : nullptr;
        p->trGetSibling = R.Method(transform, "GetSiblingIndex", 0, &err);
        p->trSetSibling = R.Method(transform, "SetSiblingIndex", 1, &err);
        if (templates)
            if (auto o = R.FieldOffset(templates, "popupPrompt", &err)) p->offTplPopup = *o;
        if (p->popupClass)
            if (auto o = R.FieldOffset(p->popupClass, "_negativeActionButton", &err)) p->offPopupNegative = *o;
        p->popupGlyphOk = p->gcInstance && p->gcGetTemplates && p->popupClass && p->offTplPopup && p->offPopupNegative &&
                          p->trGetSibling && p->trSetSibling;
        if (!p->popupGlyphOk) report += " silo-style key glyphs: NOT available (the Change prompt's glyph);";
        // Every button as the silo receipt's: the game's popup button (shared helper).
        {
            std::string gerr;
            p->gameButtonsOk = gbn::Resolve(R, gerr) && p->rtGetAnchorMin && p->rtGetPivot;
            if (!p->gameButtonsOk) report += " game buttons: NOT available (Change prompt clones" + (gerr.empty() ? std::string() : ": " + gerr) + ");";
            else if (!gerr.empty()) report += " game buttons:" + gerr;
        }
        // Hotkeys: C and the pad's right stick click.
        Il2CppClass* kb = R.Class(kInput, "UnityEngine.InputSystem", "Keyboard", &err);
        Il2CppClass* pad = R.Class(kInput, "UnityEngine.InputSystem", "Gamepad", &err);
        Il2CppClass* button = R.Class(kInput, "UnityEngine.InputSystem.Controls", "ButtonControl", &err);
        p->kbCurrent = kb ? R.Method(kb, "get_current", 0, &err) : nullptr;
        p->kbC = kb ? R.Method(kb, "get_cKey", 0, &err) : nullptr;
        p->padCurrent = pad ? R.Method(pad, "get_current", 0, &err) : nullptr;
        p->padR3 = pad ? R.Method(pad, "get_rightStickButton", 0, &err) : nullptr;
        p->btnPressedThisFrame = button ? R.Method(button, "get_wasPressedThisFrame", 0, &err) : nullptr;
        if (!p->btnPressedThisFrame || !p->kbCurrent || !p->kbC) report += " C key: NOT available (click only);";
        // Programs: P and the pad's left stick click.
        p->kbP = kb ? R.Method(kb, "get_pKey", 0, &err) : nullptr;
        p->padL3 = pad ? R.Method(pad, "get_leftStickButton", 0, &err) : nullptr;
        p->kbZ = kb ? R.Method(kb, "get_zKey", 0, &err) : nullptr;  // Delete (also the page's Clear All event)
        p->selPointerInside = method(selectable, "get_isPointerInside", 0, false);
        if (!p->selPointerInside) report += " programs dialog hover: NOT available;";
        // The [P] Programs entry in the page's key bar (else the old pill right of the target's Change).
        {
            std::string lerr;
            if (sr2::modules::minimap::ugui::Resolve(lerr) && legend_hint::Resolve(lerr)) {
                if (auto o = R.FieldOffset(p->menuClass, "_inputLegend", &lerr)) p->offMenuLegend = *o;
                p->legendOk = p->offMenuLegend != 0;
            }
            if (!p->legendOk) report += " programs key bar entry: NOT available (" + lerr + "; pill instead);";
            p->layoutElemClass = R.Class(kUi, "UnityEngine.UI", "LayoutElement", &err);
            p->leSetIgnore = p->layoutElemClass ? R.Method(p->layoutElemClass, "set_ignoreLayout", 1, &err) : nullptr;
            if (p->legendOk && !p->leSetIgnore) report += " programs key bar entry: not clickable (no LayoutElement);";
        }
        p->vmRevert = R.Method(p->vmClass, "RevertChangesToLastAppliedProgram", 0, &err);
        if (!p->vmRevert) report += " page refresh after a program switch: NOT available;";
        p->vmHasChanges = R.Method(p->vmClass, "HasAnyChanges", 0, &err);
        if (auto o = R.FieldOffset(p->menuClass, "_activeStateSprite", &err)) p->offMenuActiveSprite = *o;
        if (!p->offVmDefs) report += " program names: NOT available;";
        // The game's options selector for the programs dialog's "Next program" (else a drawn one).
        p->presetDefClass = R.Class(kImage, "MonomiPark.SlimeRancher.Options", "PresetOptionsItemDefinition", &err);
        p->presetSelClass = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Options", "OptionsPresetSelector", &err);
        p->trSetLocalScale = R.Method(transform, "set_localScale", 1, &err);
        p->rtGetRect = R.Method(p->rtClass, "get_rect", 0, &err);
        if (p->presetDefClass && p->presetSelClass) {
            off(p->presetDefClass, "_controlPrefab", p->offDefPrefab, false);
            off(p->presetSelClass, "_previousButton", p->offSelPrev, false);
            off(p->presetSelClass, "_nextButton", p->offSelNext, false);
            off(p->presetSelClass, "_currentSelectionText", p->offSelText, false);
        }
        if (!p->presetDefClass || !p->offDefPrefab || !p->offSelPrev || !p->offSelNext || !p->offSelText ||
            !p->trSetLocalScale || !p->rtGetRect)
            p->presetSelClass = nullptr;
        if (!p->presetSelClass) report += " the game's options selector: NOT available (drawn selector);";
        // "Save Program": DroneProgramMenu._activateEvent -> the key bar hint with that InputEvent ->
        // InputHint._configuration.Label (LocalizedString, text_override) / _labelText.
        {
            Il2CppClass* hint = R.Class(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputHint", &err);
            Il2CppClass* hcfg =
                R.Class(kImage, "MonomiPark.SlimeRancher.UI.Framework.CommonControls", "InputHintConfiguration", &err);
            off(p->menuClass, "_activateEvent", p->offMenuActivate, false);
            if (hint) off(hint, "_configuration", p->offHintConfig, false);
            if (hint) off(hint, "_labelText", p->offHintLabelText, false);
            if (hcfg) off(hcfg, "Label", p->offCfgLabel, false);
            if (!p->offMenuActivate || !p->offHintConfig || !p->offHintLabelText || !p->offCfgLabel || !p->legendOk)
                report += " \"Save Program\" label: NOT available;";
        }
    }

    // The slider: our own instance of the plot menu slider, built into the dialog.
    {
        std::string serr;
        p->sliderReady = p->slider.Setup(R, module, serr);
        if (!p->sliderReady) {
            why = "slider: " + serr;
            return fail();
        }
    }

    struct H {
        const char* name;
        const MethodInfo* mi;
        void* detour;
        void** orig;
    };
    const H hooks[] = {
        {"DroneProgramMenu.SetViewModel", bindMi, reinterpret_cast<void*>(&BindDetour), reinterpret_cast<void**>(&g_origBind)},
        {"FocusManager.NavigateInDirection", navMi, reinterpret_cast<void*>(&NavDetour), reinterpret_cast<void**>(&g_origNav)},
        {"FocusManager.SendSubmitActionToFocus", submitMi, reinterpret_cast<void*>(&SubmitDetour),
         reinterpret_cast<void**>(&g_origSubmit)},
        {"FocusManager.SendCancelActionToFocus", cancelMi, reinterpret_cast<void*>(&CancelDetour),
         reinterpret_cast<void**>(&g_origCancel)},
        {"DroneProgramMenu.HandleClearAllOptions", clearAllMi, reinterpret_cast<void*>(&ClearAllDetour),
         reinterpret_cast<void**>(&g_origClearAll)},
        {"DroneProgramMenu.HandleActivateDrone", activateMi, reinterpret_cast<void*>(&ActivateDetour),
         reinterpret_cast<void**>(&g_origActivate)},
    };
    std::string hookReport;
    for (const H& h : hooks) {
        std::string err;
        void* ptr = R.MethodPointer(h.mi, &err);
        if (!ptr) {
            why = std::format("{}: no native body ({})", h.name, err);
            return fail();
        }
        if (!ctx.HookAddress(h.name, ptr, h.detour, h.orig)) {
            why = std::format("hook {} failed", h.name);
            return fail();
        }
        hookReport += std::format(" {}@0x{:X}", h.name, Resolver::Rva(ptr));
    }

    impl_ = p;
    g_ui = p;
    ready_ = true;
    SR2_INFO("[{}] station ui ready:{}{}", module, hookReport, report.empty() ? "" : std::string(";") + report);
    return true;
}

// ---------------------------------------------------------------- menu state --

void StationUi::Impl::ForgetMenu() {
    if (g_open.load()) Close("menu gone");
    DestroyPill(limitPill);
    DestroyPill(progPill);
    DropProgHint();
    progHintSince = 0;
    saveText = nullptr;  // dies with the menu; the text override stays while custom upgrades are on
    Release(menu);
    Release(menuDisplay);
    marketSelected = pageActive = menuActive = displayActive = haveStation = false;
    pageProgKnown = false;
    station = 0;
}

void StationUi::Impl::ReadState() {
    marketSelected = false;
    haveStation = false;
    Il2CppObject* m = menu.p;
    pageActive = unity::ActiveInHierarchy(unity::GameObjectOf(m));
    menuActive = pageActive;
    // Any tab of the station menu (the program page is inactive on the Drone List / cloud tabs). A closed
    // display may stay loaded (and active?) until the UI load manager unloads its scene: it counts only
    // within kOtherTabMs of the program page being seen, so a lingering display can't hold the rotation.
    const uint64_t nowMs = GetTickCount64();
    if (pageActive) lastPageMs = nowMs;
    displayActive = pageActive || (unity::IsAlive(menuDisplay.p) &&
                                   unity::ActiveInHierarchy(unity::GameObjectOf(menuDisplay.p)) && nowMs - lastPageMs < kOtherTabMs);
    if (pageActive) {  // the option picker sub page covers the program page
        Il2CppObject* picker = unity::Field<Il2CppObject*>(m, offMenuPicker);
        if (unity::IsAlive(picker) && unity::ActiveInHierarchy(unity::GameObjectOf(picker))) pageActive = false;
    }
    Il2CppObject* vm = unity::Field<Il2CppObject*>(m, offMenuVm);
    if (!IsA(vm, vmClass)) return;
    if (Il2CppObject* st = unity::Field<Il2CppObject*>(vm, offVmStation); IsA(st, stationModelClass)) {
        station = unity::Field<int64_t>(st, offActorId);
        haveStation = station != 0;
    }
    Il2CppObject* dest = unity::Field<Il2CppObject*>(vm, offVmDest);
    Il2CppObject* sel = IsA(dest, destVmClass) ? unity::Field<Il2CppObject*>(dest, offDestSel) : nullptr;
    if (!IsA(sel, entryClass)) return;
    bool ok = false;
    const int type = Unbox<int32_t>(Call(entryGetType, sel, {}, "DroneDestinationOptionEntry.get_Type", &ok));
    marketSelected = ok && type == kSinkPlortMarket;
}

// ---------------------------------------------------------------- prompt --

Il2CppObject* StationUi::Impl::StationModel() {
    Il2CppObject* vm = menu.p ? unity::Field<Il2CppObject*>(menu.p, offMenuVm) : nullptr;
    if (!IsA(vm, vmClass)) return nullptr;
    Il2CppObject* st = unity::Field<Il2CppObject*>(vm, offVmStation);
    return IsA(st, stationModelClass) ? st : nullptr;
}

void StationUi::Impl::DestroyPill(Pill& P) {
    if (P.go && !Faulted()) Destroy(P.go);  // a game button's slot or the cloned prompt
    P.gb.Forget();
    for (float& f : P.placed) f = -1e9f;
    P.go = P.toggle = P.label = P.keyText = P.orig = P.ied = P.group = nullptr;
    P.icons.clear();
    P.shown = false;
    P.text.clear();
    FreeLater(P.handles);
}

void StationUi::Impl::EnsurePill(Pill& P, size_t buttonOff, const char* name, const char* what, const char* key) {
    if (P.go && unity::IsAlive(P.go) && (!P.gb.Exists() || P.gb.Alive())) return;
    if (P.go) DestroyPill(P);  // died with the menu
    Il2CppObject* button = unity::Field<Il2CppObject*>(menu.p, buttonOff);
    if (!unity::IsAlive(button) || !IsA(button, optionButtonClass)) return;
    Il2CppObject* orig = unity::Field<Il2CppObject*>(button, offOptPrompt);
    if (!unity::IsAlive(orig) || !IsA(orig, rtClass)) {
        if (!P.loggedFail) SR2_WARN("[{}] station ui: the option has no prompt container - no {} action", module, what);
        P.loggedFail = true;
        return;
    }
    // Round 11: "[P] has wrong style for button" - the game's popup button (as the silo receipt's), the
    // Change prompt clone only when that is not available.
    if (gameButtonsOk && GamePill(P, orig, unity::TransformOf(button), name, key, what)) {
        unity::SetActive(P.go, false);
        P.shown = false;
        P.reapply = kReapplyFrames;
        P.text.clear();
        if (!P.logged) {
            P.logged = true;
            SR2_INFO("[{}] station ui: {} action is a game popup button (key {})", module, what, key);
        }
        return;
    }
    if (!ClonePrompt(P, orig, unity::TransformOf(button), name)) {
        if (!P.loggedFail) SR2_WARN("[{}] station ui: cloning the Change prompt failed ({})", module, what);
        P.loggedFail = true;
        return;
    }
    unity::SetActive(P.go, false);
    P.shown = false;
    P.reapply = kReapplyFrames;
    P.text.clear();
    if (!P.logged) {
        P.logged = true;
        SR2_INFO("[{}] station ui: {} action cloned from a Change prompt ({} key icon(s), label {}, click {})", module, what,
                 P.icons.size(), P.label ? "ok" : "MISSING", P.toggle ? "ok" : "MISSING");
    }
}

// A copy of the game's "Change" prompt (`orig`) under `parentTr`, prepared for our own key and label
// (InputEventDisplay and the label's LocalizeStringEvent off), clickable, fitted to its label. Inactive
// state as cloned; the caller shows it.
bool StationUi::Impl::ClonePrompt(Pill& P, Il2CppObject* orig, Il2CppObject* parentTr, const char* name) {
    Il2CppObject* clone = unity::Instantiate(unity::GameObjectOf(orig), parentTr);
    if (!unity::IsAlive(clone)) return false;
    Il2CppObject* go = IsA(clone, goClass) ? clone : unity::GameObjectOf(clone);
    P.go = go;
    P.orig = orig;
    Keep(P.handles, go);
    Keep(P.handles, orig);
    unity::SetName(go, name);
    Il2CppObject* tr = unity::TransformOf(go);
    // Our own key and label: the clone must not follow the input device / localisation.
    if (Il2CppObject* ied = Find(tr, "InputEventDisplay")) {
        SetEnabled(unity::GetComponent(ied, inputEventDisplayClass), false);
        P.ied = ied;
        Keep(P.handles, ied);
    }
    if (UsePopupGlyph(P, tr)) {
        // the silo receipt's key glyph (P.ied / P.group / P.icons are its parts)
    } else if (Il2CppObject* group = Find(tr, "InputEventDisplay/Icon Group")) {
        P.group = group;
        Keep(P.handles, group);
        bool ok = false;
        const int n = Unbox<int32_t>(Call(trChildCount, group, {}, "Transform.get_childCount", &ok));
        for (int i = 0; ok && i < n && i < 8; ++i) {
            int32_t idx = i;
            Il2CppObject* child = Call(trGetChild, group, {&idx}, "Transform.GetChild");
            if (unity::IsAlive(child)) {
                Keep(P.handles, child);
                P.icons.push_back(child);
            }
        }
    }
    if (Il2CppObject* labelTr = Find(tr, "InputLabel")) {
        SetEnabled(unity::GetComponent(labelTr, lseClass), false);
        P.label = TmpOf(labelTr);
        Keep(P.handles, P.label);
        if (P.label && tmpSetWrap) {  // one line; the pill grows with the label instead
            int32_t noWrap = 0;
            Call(tmpSetWrap, P.label, {&noWrap}, "TMP_Text.set_textWrappingMode");
        }
    }
    // Clickable: the pill background catches the pointer, a Toggle reports the click.
    if (sdfRectClass) SetRaycast(unity::GetComponent(go, sdfRectClass), true);
    P.toggle = Clickable(go);
    Keep(P.handles, P.toggle);
    // Fit the pill to our label (no 264 minimum), with room inside its rounded ends.
    if (encapsulateClass)
        if (Il2CppObject* enc = unity::GetComponent(go, encapsulateClass); unity::IsAlive(enc)) {
            if (offEncMinW) unity::Field<float>(enc, offEncMinW) = 0.0f;
            if (offEncPadX) unity::Field<float>(enc, offEncPadX) = std::max(unity::Field<float>(enc, offEncPadX), kPillPadX);
        }
    return true;
}

// Round 10: "keybind icons still look off, use the same look as for silo dialog menus". The silo receipt's
// buttons are clones of the game's confirm-popup buttons, whose key glyph is their "Icon Background"
// (a background behind InputEventDisplay / Icon Group / InputIcons). The pill gets a clone of that
// element (from the popup template's negative button, the one the receipt's Cancel is cloned from) in
// place of the Change prompt's own InputEventDisplay (deactivated); ApplyPillLook then sets its first
// InputIcon to our key exactly as before. false = not available: the Change prompt's glyph stays.
bool StationUi::Impl::UsePopupGlyph(Pill& P, Il2CppObject* pillTr) {
    if (!popupGlyphOk || !unity::IsAlive(P.ied)) return false;
    auto miss = [&](const char* what) {
        if (!loggedGlyph) SR2_WARN("[{}] station ui: silo-style key glyph not cloned ({}) - the Change prompt's", module, what);
        loggedGlyph = true;
        return false;
    };
    bool ok = false;
    Il2CppObject* gc = Call(gcInstance, nullptr, {}, "GameContext.Instance", &ok);
    if (!ok || !unity::IsAlive(gc)) return miss("no GameContext");
    Il2CppObject* tpl = Call(gcGetTemplates, gc, {}, "GameContext.get_UITemplates", &ok);
    if (!ok || !unity::IsAlive(tpl)) return miss("no UITemplates");
    Il2CppObject* popupGo = unity::Field<Il2CppObject*>(tpl, offTplPopup);
    Il2CppObject* popup = unity::IsAlive(popupGo) ? unity::GetComponent(popupGo, popupClass) : nullptr;
    if (!unity::IsAlive(popup)) return miss("no PopupPrompt template");
    Il2CppObject* button = unity::Field<Il2CppObject*>(popup, offPopupNegative);
    if (!unity::IsAlive(button) || !IsA(button, goClass)) return miss("no popup button");
    Il2CppObject* src = Find(unity::TransformOf(button), "Icon Background");
    if (!src) return miss("no Icon Background on the popup button");
    Il2CppObject* clone = unity::Instantiate(unity::GameObjectOf(src), pillTr);
    if (!unity::IsAlive(clone)) return miss("Instantiate failed");
    Il2CppObject* cgo = IsA(clone, goClass) ? clone : unity::GameObjectOf(clone);
    Il2CppObject* ctr = unity::TransformOf(cgo);
    Il2CppObject* ied = Find(ctr, "InputEventDisplay");
    Il2CppObject* group = Find(ctr, "InputEventDisplay/Icon Group");
    if (!ied || !group) {
        Destroy(cgo);
        return miss("no InputEventDisplay / Icon Group in it");
    }
    unity::SetName(cgo, "sr2mods_KeyGlyph");
    SetEnabled(unity::GetComponent(ied, inputEventDisplayClass), false);  // our key, not the popup's binding
    // Where the Change prompt's glyph was (left of the label); that one goes away.
    int32_t idx = Unbox<int32_t>(Call(trGetSibling, P.ied, {}, "Transform.GetSiblingIndex", &ok));
    if (ok) Call(trSetSibling, ctr, {&idx}, "Transform.SetSiblingIndex");
    unity::SetActive(unity::GameObjectOf(P.ied), false);
    P.ied = ctr;
    Keep(P.handles, ctr);
    P.group = group;
    Keep(P.handles, group);
    const int n = Unbox<int32_t>(Call(trChildCount, group, {}, "Transform.get_childCount", &ok));
    for (int i = 0; ok && i < n && i < 8; ++i) {
        int32_t k = i;
        Il2CppObject* child = Call(trGetChild, group, {&k}, "Transform.GetChild");
        if (unity::IsAlive(child)) {
            Keep(P.handles, child);
            P.icons.push_back(child);
        }
    }
    if (!loggedGlyph) {
        loggedGlyph = true;
        SR2_INFO("[{}] station ui: key glyphs cloned from the popup button (as the silo receipt; {} key icon(s))", module,
                 P.icons.size());
    }
    return true;
}

// The page pill as the game's popup button under the option `parentTr`, right of its Change prompt `orig`
// (PlacePill). Label set by the caller (UpdatePillLabel / UpdateProgramsLabel).
bool StationUi::Impl::GamePill(Pill& P, Il2CppObject* orig, Il2CppObject* parentTr, const char* name, const char* key,
                               const char* what) {
    gbn::Spec spec;
    spec.name = name;
    spec.label = std::string(what) == "programs" ? "Programs" : "Limit";
    spec.key = key ? key : "";
    // The game's own key icon for the key (round 15 shared rule: our key hints match the game's icons).
    if (key && key[0] && !key[1]) spec.control = std::string("<Keyboard>/") + static_cast<char>(std::tolower(key[0]));
    spec.scale = kPillScale;
    spec.module = module;
    std::string why;
    if (!P.gb.Create(parentTr, spec, why)) {
        if (!loggedGameButtonMiss) SR2_WARN("[{}] station ui: game button not created ({}) - Change prompt clone", module, why);
        loggedGameButtonMiss = true;
        return false;
    }
    P.go = P.gb.Slot();
    P.orig = orig;
    Keep(P.handles, orig);
    P.key = spec.key;
    return true;
}

// A pill's click this frame (the game button is polled every frame while shown).
bool StationUi::Impl::PillClicked(Pill& P) {
    if (P.gb.Exists()) return P.gb.Tick();
    return Clicked(P.toggle);
}

void StationUi::Impl::SyncPill(Pill& P, bool want, size_t buttonOff, const char* name, const char* what, const char* key) {
    if (want) EnsurePill(P, buttonOff, name, what, key);
    if (P.go && !unity::IsAlive(P.go)) DestroyPill(P);
    if (P.go && want != P.shown) {
        unity::SetActive(P.go, want);
        P.shown = want;
        if (want) P.reapply = kReapplyFrames;
    }
    if (P.shown) PlacePill(P);
}

void StationUi::Impl::ApplyPillLook(Pill& P, const char* key) {
    if (P.gb.Exists()) {
        P.gb.SetKey(key ? key : "");
        return;
    }
    if (!key || !*key) {  // label only (a dialog button without a hotkey)
        if (unity::IsAlive(P.ied)) unity::SetActive(unity::GameObjectOf(P.ied), false);
        P.text.clear();
        return;
    }
    // InputIcon.SetKey by hand: the first icon shows our key, the others are hidden.
    if (unity::IsAlive(P.ied)) unity::SetActive(unity::GameObjectOf(P.ied), true);
    if (unity::IsAlive(P.group)) unity::SetActive(unity::GameObjectOf(P.group), true);
    for (size_t i = 0; i < P.icons.size(); ++i) {
        Il2CppObject* t = P.icons[i];
        if (!unity::IsAlive(t)) continue;
        if (i > 0) {
            unity::SetActive(unity::GameObjectOf(t), false);
            continue;
        }
        unity::SetActive(unity::GameObjectOf(t), true);
        Il2CppObject* icon = unity::GetComponent(t, inputIconClass);
        if (!unity::IsAlive(icon)) continue;
        Il2CppObject* btn = unity::Field<Il2CppObject*>(icon, offIconButton);
        Il2CppObject* keyRoot = unity::Field<Il2CppObject*>(icon, offIconKey);
        Il2CppObject* dbl = unity::Field<Il2CppObject*>(icon, offIconDouble);
        Il2CppObject* text = unity::Field<Il2CppObject*>(icon, offIconKeyText);
        if (unity::IsAlive(btn)) unity::SetActive(btn, false);
        if (unity::IsAlive(dbl)) unity::SetActive(dbl, false);
        if (unity::IsAlive(keyRoot)) unity::SetActive(keyRoot, true);
        SetEnabled(unity::Field<Il2CppObject*>(icon, offIconKeyImage), false);
        SetEnabled(unity::Field<Il2CppObject*>(icon, offIconKeyLse), false);
        if (unity::IsAlive(text)) {
            SetEnabled(text, true);
            SetText(text, key);
            P.keyText = text;
        }
    }
    P.text.clear();  // set the label again (the clone may have reset it)
}

void StationUi::Impl::SetPillText(Pill& P, const std::string& text) {
    if (P.gb.Exists()) {
        P.gb.SetLabel(text);
        P.text = text;
        return;
    }
    if (!P.label || text == P.text) return;
    SetText(P.label, text);
    P.text = text;
}

void StationUi::Impl::UpdatePillLabel() {
    if ((!limitPill.label && !limitPill.gb.Exists()) || !haveStation || !host.get) return;
    const StationLimit l = host.get(station);
    // Short label: "Price limit: 200%" overflowed the pill and wrapped.
    std::string s = "Limit";
    if (l.Active()) s += l.mode == LimitMode::kFraction ? std::format(": {}%", l.value) : std::format(": {}", l.value);
    SetPillText(limitPill, s);
}

void StationUi::Impl::UpdateProgramsLabel() {
    const bool hint = progHint.Alive();
    if (!hint && !progPill.label && !progPill.gb.Exists()) return;
    progs::View v;
    Il2CppObject* model = StationModel();
    if (!model || !progs::Get(model, v)) return;
    const size_t n = v.programs.size();
    const std::string text = n < 2 ? std::string("Programs") : std::format("Program {}/{}", v.running + 1, n);
    if (hint) {
        if (text != progHintText) {
            progHint.SetLook("P", text);
            progHintText = text;
        }
        return;
    }
    SetPillText(progPill, text);
}

// ---------------------------------------------------------------- key bar entry --

// The [P] Programs entry: a clone of one of the page's key bar hints (modules/common/legend_hint.h), placed
// before the bar's first entry (round 14: leftmost), with a transparent click target over it (the game's own
// entries are clickable too). The bar may rebuild its entries: a dead clone is made again on the next state read.
void StationUi::Impl::SyncProgHint(bool want) {
    if (progHint.Exists() && !progHint.Alive()) DropProgHint();  // died with the bar / the menu
    if (!want) {
        if (progHint.Exists()) DropProgHint();
        progHintSince = 0;
        return;
    }
    std::string chain;
    Il2CppObject* legend = legend_hint::FollowLegend(unity::Field<Il2CppObject*>(menu.p, offMenuLegend), menu.p,
                                                     "DroneProgramMenu._inputLegend", chain);
    const std::vector<Il2CppObject*> hints = legend_hint::Hints(legend, true);
    // Round 14: "Move it to the leftmost position" - before the bar's first entry.
    Il2CppObject* first = hints.empty() ? nullptr : hints.front();
    if (progHint.Alive()) {
        progHint.PlaceBefore(first);
        return;
    }
    const uint64_t now = GetTickCount64();
    if (!progHintSince) progHintSince = now;
    Il2CppObject* tmpl = legend_hint::Template(legend);
    std::string why = !legend ? "no InputLegend (" + chain + ")"
                      : !tmpl ? "none of the bar's entries is shown (" + chain + ")"
                              : "";
    if (!tmpl || !progHint.Create(tmpl, "sr2mods_ProgramsKeyHint", why)) {
        if (now - progHintSince >= 3000) {  // the old pill instead, for this session
            progHintFailed = true;
            SR2_WARN("[{}] station ui: no [P] Programs entry in the page's key bar ({}) - pill instead", module, why);
        }
        return;
    }
    progHintText.clear();
    UpdateProgramsLabel();
    if (progHintText.empty()) {
        progHint.SetLook("P", "Programs");
        progHintText = "Programs";
    }
    progHint.PlaceBefore(first);
    // Click target: a transparent raycast image over the whole entry, outside the entry's layout.
    if (leSetIgnore && layoutElemClass) {
        if (Il2CppObject* go = NewUi("Click", unity::TransformOf(progHint.GameObject()), -1)) {
            Rect(unity::TransformOf(go), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
            if (Il2CppObject* le = AddComponent(go, layoutElemClass, "AddComponent(LayoutElement)")) {
                bool on = true;
                Call(leSetIgnore, le, {&on}, "LayoutElement.set_ignoreLayout");
            }
            Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
            SetColor(img, Color{1, 1, 1, 0});
            SetRaycast(img, true);
            progHintToggle = Clickable(go);
            Keep(progHintHandles, progHintToggle);
        }
    }
    if (!progHintLogged) {
        progHintLogged = true;
        SR2_INFO("[{}] station ui: [P] Programs entry cloned from the page's key bar entry '{}' ({} key icon(s), label {}, "
                 "click {}; placed before '{}')",
                 module, unity::GetName(unity::GameObjectOf(tmpl)), progHint.IconCount(),
                 progHint.HasLabel() ? "ok" : "MISSING", progHintToggle ? "ok" : "NOT available",
                 first ? unity::GetName(unity::GameObjectOf(first)) : std::string("-"));
    }
}

void StationUi::Impl::DropProgHint() {
    if (Faulted()) progHint.Abandon();
    else progHint.Destroy();  // the click target goes with it
    progHintToggle = nullptr;
    progHintText.clear();
    FreeLater(progHintHandles);
}

void StationUi::Impl::PlacePill(Pill& P) {
    if (!unity::IsAlive(P.go) || !unity::IsAlive(P.orig)) return;
    if (P.gb.Exists()) {
        // In the Change prompt's anchor space: left edge 12 right of its right edge, vertically centred on it.
        const Vec2 amin = GetVec(rtGetAnchorMin, P.orig, "RectTransform.get_anchorMin");
        const Vec2 piv = GetVec(rtGetPivot, P.orig, "RectTransform.get_pivot");
        const Vec2 opos = GetVec(rtGetPos, P.orig, "RectTransform.get_anchoredPosition");
        const Vec2 osize = GetVec(rtGetSize, P.orig, "RectTransform.get_sizeDelta");
        const float want[4] = {amin.x, amin.y, opos.x + osize.x * (1.0f - piv.x) + 12.0f, opos.y + osize.y * (0.5f - piv.y)};
        bool same = true;
        for (int i = 0; i < 4; ++i) same = same && std::fabs(want[i] - P.placed[i]) < 0.5f;
        if (same) return;
        for (int i = 0; i < 4; ++i) P.placed[i] = want[i];
        P.gb.PlaceAnchored(want[0], want[1], want[2], want[3], 0.0f, 0.5f);
        return;
    }
    Il2CppObject* rt = unity::TransformOf(P.go);
    const Vec2 opos = GetVec(rtGetPos, P.orig, "RectTransform.get_anchoredPosition");
    const Vec2 osize = GetVec(rtGetSize, P.orig, "RectTransform.get_sizeDelta");
    const Vec2 size = GetVec(rtGetSize, rt, "RectTransform.get_sizeDelta");
    // Same anchors / pivot as the original (pivot x 0.5): to its right, 12 apart.
    const Vec2 want{opos.x + osize.x * 0.5f + 12.0f + size.x * 0.5f, opos.y};
    const Vec2 cur = GetVec(rtGetPos, rt, "RectTransform.get_anchoredPosition");
    if (std::fabs(cur.x - want.x) > 0.5f || std::fabs(cur.y - want.y) > 0.5f)
        SetVec(rtSetPos, rt, want, "RectTransform.set_anchoredPosition");
}

// ---------------------------------------------------------------- dialog --

// The dialog frame both dialogs share: own canvas above the menu (sorted 20 above it), a dimmed backdrop
// (a click on it closes), the lavender border and the white content panel (kPanelW x panelH, centred).
// Sets root, panelRt, blockerToggle, titleSrc / bodySrc. false = nothing usable (Close cleans up).
bool StationUi::Impl::BuildShell(const char* name, float panelH, int& layer) {
    Il2CppObject* m = menu.p;
    bool ok = false;
    bool inactive = true;
    Il2CppObject* display = Call(compInParent, m, {unity::TypeOf(displayClass), &inactive}, "GetComponentInParent(DroneUIDisplay)", &ok);
    if (!ok || !unity::IsAlive(display)) return false;
    Il2CppObject* displayTr = unity::TransformOf(display);
    layer = Unbox<int32_t>(Call(goGetLayer, unity::GameObjectOf(display), {}, "GameObject.get_layer"), -1);
    Il2CppObject* hdr = unity::Field<Il2CppObject*>(m, offMenuHeader);
    Il2CppObject* desc = unity::Field<Il2CppObject*>(m, offMenuDesc);
    titleSrc = TmpOf(hdr);
    bodySrc = TmpOf(desc);
    if (!bodySrc) bodySrc = titleSrc;
    Keep(dialogHandles, titleSrc);
    Keep(dialogHandles, bodySrc);
    Il2CppObject* r = NewUi(name, displayTr, layer);
    if (!r) return false;
    root = r;
    Keep(dialogHandles, r);
    unity::SetActive(r, false);
    Il2CppObject* rootRt = unity::TransformOf(r);
    Rect(rootRt, {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
    {
        Il2CppObject* parentCanvas = Call(compInParent, displayTr, {unity::TypeOf(canvasClass), &inactive}, "GetComponentInParent(Canvas)", &ok);
        if (!ok || !unity::IsAlive(parentCanvas)) parentCanvas = nullptr;
        Il2CppObject* canvas = AddComponent(r, canvasClass, "AddComponent(Canvas)");
        if (!canvas) return false;
        int32_t order = parentCanvas ? Unbox<int32_t>(Call(cvGetOrder, parentCanvas, {}, "Canvas.get_sortingOrder")) : 0;
        int32_t sortLayer = parentCanvas ? Unbox<int32_t>(Call(cvGetLayer, parentCanvas, {}, "Canvas.get_sortingLayerID")) : 0;
        bool over = true;
        order += 20;
        Call(cvSetOverride, canvas, {&over}, "Canvas.set_overrideSorting");
        Call(cvSetLayer, canvas, {&sortLayer}, "Canvas.set_sortingLayerID");
        Call(cvSetOrder, canvas, {&order}, "Canvas.set_sortingOrder");
        if (!AddComponent(r, raycasterClass, "AddComponent(GraphicRaycaster)")) return false;
    }
    // Backdrop: far beyond the display's rect (it is smaller than the screen); a click on it closes.
    if (Il2CppObject* bd = NewUi("Backdrop", rootRt, layer)) {
        Rect(unity::TransformOf(bd), {0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {4000, 4000});
        Il2CppObject* img = AddComponent(bd, imageClass, "AddComponent(Image)");
        SetColor(img, Color{0.05f, 0.07f, 0.12f, 0.55f});
        blockerToggle = Clickable(bd);
        Keep(dialogHandles, blockerToggle);
    }
    Il2CppObject* panel = NewUi("Panel", rootRt, layer);
    if (!panel) return false;
    Il2CppObject* prt = unity::TransformOf(panel);
    panelRt = prt;
    Keep(dialogHandles, prt);
    Rect(prt, {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {kPanelW, panelH});
    // Border + white content (the menu's ContentBorder / ContentContainer); the content catches clicks.
    Box(prt, "Border", -8, -8, kPanelW + 16, panelH + 16, kLavender, 34.0f);
    if (Il2CppObject* content = Box(prt, "Content", 0, 0, kPanelW, panelH, kWhite, 30.0f)) {
        SetRaycast(sdfRectClass ? unity::GetComponent(content, sdfRectClass) : unity::GetComponent(content, imageClass), true);
        Clickable(content);  // never polled: it only stops clicks on the panel from reaching the menu behind it
    }
    return !Faulted();
}

// A rounded button with a centred label; returns its click toggle (kept).
Il2CppObject* StationUi::Impl::Button(Il2CppObject* prt, const char* name, float x, float y, float w, float h,
                                     const std::string& label, Color bg, Color fg, int layer, float border) {
    Il2CppObject* b = NewUi(name, prt, layer);
    if (!b) return nullptr;
    At(unity::TransformOf(b), x, y, w, h);
    SetRaycast(Shape(b, bg, h * 0.5f, border, kLavender), true);
    Text(unity::TransformOf(b), "Label", 0, 0, w, h, label, 24.0f, fg, kAlignCenter, titleSrc);
    Il2CppObject* t = Clickable(b);
    Keep(dialogHandles, t);
    return t;
}

// A button that is the game's own prompt pill (a clone of the target option's "Change" prompt, the same
// as the [P] Programs action): the game's rounded cyan shape, font, padding and key glyph (`key`, or none).
// At least w wide (it grows with a longer label), h is the pill's own height. `bg` / `fg` recolour it
// (selected states). Falls back to our own Button when the prompt can't be cloned. Returns the toggle.
Il2CppObject* StationUi::Impl::PillButton(Il2CppObject* prt, const char* name, float x, float y, float w, float h,
                                         const char* key, const std::string& label, const Color* bg, const Color* fg,
                                         int layer) {
    Il2CppObject* button = menu.p ? unity::Field<Il2CppObject*>(menu.p, offMenuTarget) : nullptr;
    Il2CppObject* orig = IsA(button, optionButtonClass) ? unity::Field<Il2CppObject*>(button, offOptPrompt) : nullptr;
    Pill P;
    if (!unity::IsAlive(orig) || !IsA(orig, rtClass) || !ClonePrompt(P, orig, prt, name) || !P.label) {
        if (P.go) Destroy(P.go);
        FreeLater(P.handles);
        return Button(prt, name, x, y, w, h, label, bg ? *bg : kCyan, fg ? *fg : kNavy, layer);
    }
    Il2CppObject* rt = unity::TransformOf(P.go);
    At(rt, x, y, w, h);
    if (encapsulateClass)
        if (Il2CppObject* enc = unity::GetComponent(P.go, encapsulateClass); unity::IsAlive(enc) && offEncMinW)
            unity::Field<float>(enc, offEncMinW) = w;
    unity::SetActive(P.go, true);
    P.shown = true;
    P.key = key ? key : "";
    P.want = label;
    ApplyPillLook(P, P.key.c_str());
    SetPillText(P, P.want);
    if (bg && sdfRectClass) SetColor(unity::GetComponent(P.go, sdfRectClass), *bg);
    if (fg) Call(tmpSetColor, P.label, {const_cast<Color*>(fg)}, "TMP_Text.set_color");
    P.reapply = kReapplyFrames;
    Il2CppObject* t = P.toggle;
    dialogPills.push_back(std::move(P));
    return t;
}

// Round 11: "use Silo's GUI buttons, theyre the best ones we have rn that look similiar to SR2's style" -
// every dialog button is the game's popup button (game_button: pink pill, hover colour and bounce, the key
// glyph on the left when `key` is set), placed with its left edge at x (right: its right edge at x + w) and
// centred on the row (y, h); the label is at least w wide (minus the pill's ends). Selected = teal.
Il2CppObject* StationUi::Impl::DialogButton(Il2CppObject* prt, const char* name, float x, float y, float w, float h,
                                           const char* key, const std::string& label, bool selected, int layer, int& gbi,
                                           bool right, const Color* fallbackBg, const Color* fallbackFg,
                                           const char* control) {
    gbi = -1;
    if (gameButtonsOk) {
        gbn::Spec spec;
        spec.name = name;
        spec.label = label;
        spec.key = key ? key : "";
        spec.control = control ? control : "";
        spec.scale = kDialogScale;
        // The pill adds ~54 px (label inset 30 + padding 24) around the label; a glyph overlaps its left end.
        spec.minWidth = std::max(0.0f, w - (key && *key ? 110.0f : 54.0f) * kDialogScale);
        spec.module = module;
        gbn::Button b;
        std::string why;
        if (b.Create(prt, spec, why)) {
            b.Place(right ? x + w : x, y + h * 0.5f, right ? 1.0f : 0.0f);
            if (selected) b.SetSelected(true);
            dialogButtons.push_back(std::move(b));
            dialogClicks.push_back(0);
            gbi = static_cast<int>(dialogButtons.size()) - 1;
            return nullptr;
        }
        if (!loggedGameButtonMiss) SR2_WARN("[{}] station ui: game button not created ({}) - Change prompt clone", module, why);
        loggedGameButtonMiss = true;
    }
    const Color* bg = selected ? &kTeal : fallbackBg;
    const Color* fg = selected ? &kWhite : fallbackFg;
    return PillButton(prt, name, x, y, w, h, key, label, bg, fg, layer);
}

// The station's program changed while its menu is open (the rotation, a drone source falling back to
// Coop, ...): the page still shows the old one, and closing the menu would ask whether to discard
// "changes" the player never made (DroneUIDisplay.OnCancel -> _unappliedChangesConfirm when
// HasAnyChanges). The page follows the station then. A program the player applied (Start Program)
// matches the page already: HasAnyChanges is false and nothing happens.
void StationUi::Impl::FollowStation() {
    Il2CppObject* model = StationModel();
    ProgramData live;
    if (!model || !drones::ReadProgram(model, live)) return;
    const bool changed = pageProgKnown && pageStation == station && live != pageProg;
    pageProg = live;
    pageStation = station;
    pageProgKnown = true;
    if (!changed) return;
    Il2CppObject* vm = menu.p ? unity::Field<Il2CppObject*>(menu.p, offMenuVm) : nullptr;
    if (!IsA(vm, vmClass)) return;
    bool ok = false;
    const bool differs = !vmHasChanges || (Unbox<bool>(Call(vmHasChanges, vm, {}, "DroneProgramViewModel.HasAnyChanges", &ok)) && ok);
    if (!differs) return;
    RevertPage();
    SR2_INFO("[{}] station ui: station {}: its program changed while the menu was open - the page shows it again", module,
             station);
}

bool StationUi::Impl::Open() {
    if (g_open.load() || !haveStation || !host.get || !host.set) return false;
    Il2CppObject* m = menu.p;
    auto failOpen = [&](const char* what) {
        if (!loggedOpenFail) SR2_WARN("[{}] station ui: dialog not opened: {}", module, what);
        loggedOpenFail = true;
        Close("open failed");  // removes what was built, frees the handles
        return false;
    };

    // Plorts in the market terminal's order, with their base price.
    struct Row {
        Il2CppObject* icon;
        std::string name;
        int base;
    };
    std::vector<Row> rows;
    float maxBase = 0.0f;
    if (Il2CppObject* cfg = unity::Field<Il2CppObject*>(m, offMenuMarket); unity::IsAlive(cfg)) {
        void* arr = unity::Field<void*>(cfg, offMarketPlorts);
        const size_t n = arr ? unity::ArrayLength(arr) : 0;
        for (size_t i = 0; i < n && i < 128; ++i) {
            auto* e = static_cast<Il2CppObject*>(unity::ArrayAt(arr, i));
            Il2CppObject* id = e ? unity::Field<Il2CppObject*>(e, offPlortEntryType) : nullptr;
            if (!IsA(id, identClass) || !unity::IsAlive(id)) continue;
            const float base = host.baseValue ? host.baseValue(id) : 0.0f;
            std::string name = Localized(unity::Field<Il2CppObject*>(id, offIdentName));
            if (name.empty()) name = unity::GetName(id);
            rows.push_back({unity::Field<Il2CppObject*>(id, offIdentIcon), name, static_cast<int>(std::lround(base))});
            maxBase = std::max(maxBase, base);
        }
    }
    maxPrice = std::max(100, static_cast<int>(std::ceil(maxBase * 2.0f / 10.0f)) * 10);

    // Current limit of this station.
    openStation = station;
    const StationLimit cur = host.get(station);
    mode = cur.mode;
    values[0] = values[1] = 0;
    values[static_cast<int>(mode)] = cur.value;
    focusRow = 1;

    // ---- layout ----
    const int n = static_cast<int>(rows.size());
    const int cols = n > 30 ? 6 : n > 20 ? 5 : 4;
    const int nrows = (n + cols - 1) / cols;
    const float sheetX = kPad, sheetW = kPanelW - 2 * kPad;
    const float cellW = (sheetW - 32.0f - (cols - 1) * kCellGap) / cols;
    const float titleY = 28.0f, subY = 84.0f;
    modeRowY = 156.0f;
    sliderY = modeRowY + kRowH + 22.0f;
    const float sheetY = sliderY + sui::PlotMenuSlider::Height() + 30.0f;
    const float sheetH = 56.0f + nrows * (kCellH + kCellGap) + 10.0f;
    const float footY = sheetY + sheetH + 18.0f;
    const float panelH = footY + kRowH + 26.0f;

    int layer = -1;
    if (!BuildShell("sr2mods_DronePriceLimitDialog", panelH, layer)) return failOpen("dialog frame not built");
    Il2CppObject* r = root;
    Il2CppObject* prt = panelRt;

    Text(prt, "Title", kPad, titleY, kPanelW - 2 * kPad, 50, "Plort Market price limit", 40.0f, kNavy, kAlignLeft, titleSrc);
    const bool persistent = host.persistent && host.persistent();
    noteText = Text(prt, "Subtitle", kPad, subY, kPanelW - 2 * kPad, 60,
                    std::string("This station's drones only sell a plort while its market price is at or above the "
                                "limit. Below it they keep the plorts and sell once the price recovers.") +
                        (persistent ? "" : " (Not saved: custom_upgrades is off, the limit lasts until the game is closed.)"),
                    22.0f, kNavy, kAlignTopLeft, bodySrc, true);

    // Focus frame (behind the focused row). Opaque: a shape whose vertex colours are all transparent is culled
    // by its CanvasRenderer (cullTransparentMesh), so the old transparent-fill frame never showed.
    focusFrame = Box(prt, "Focus", kPad - 14, 0, kPanelW - 2 * kPad + 28, kRowH + 16, Color{0.86f, 0.95f, 0.97f, 1.0f},
                     20.0f, 4.0f, kTeal);
    Keep(dialogHandles, focusFrame);

    // Mode row (round 15: "Change limit by to the same component as mode selection in programs"): label + the
    // game's options selector, as the programs dialog's "Next program".
    Text(prt, "ModeLabel", kPad, modeRowY, 230, kRowH, "Limit by", 26.0f, kNavy, kAlignLeft, bodySrc);
    {
        constexpr float kModeRowW = 260.0f + kLimitSelW;
        Il2CppObject* row = NewUi("ModeRow", prt, layer);
        if (!row) return failOpen("mode row failed");
        Il2CppObject* rowRt = unity::TransformOf(row);
        At(rowRt, kPad, modeRowY, kModeRowW, kRowH);
        Selector(rowRt, kModeRowW, kRowH, kLimitSelW, layer, modePrev, modeNext, modeValue);
        if (!modeValue) return failOpen("mode selector failed");
    }

    // Base-price cheat sheet.
    if (Il2CppObject* sheet = Box(prt, "BasePrices", sheetX, sheetY, sheetW, sheetH, kBacker, 10.0f)) {
        Il2CppObject* srt = unity::TransformOf(sheet);
        Text(srt, "Header", 16, 10, sheetW - 32, 36, "Base prices (Plort Market)", 26.0f, kNavy, kAlignLeft, titleSrc);
        Il2CppObject* coin = CoinSprite();
        cells.clear();
        for (int i = 0; i < n; ++i) {
            const float cx = 16.0f + (i % cols) * (cellW + kCellGap);
            const float cy = 56.0f + (i / cols) * (kCellH + kCellGap);
            Il2CppObject* cell = Box(srt, "Cell", cx, cy, cellW, kCellH, kWhite, 10.0f);
            if (!cell) continue;
            Il2CppObject* crt = unity::TransformOf(cell);
            Sprite(crt, "Icon", 6, 5, 52, 52, rows[i].icon);
            Text(crt, "Name", 64, 3, cellW - 70, 28, rows[i].name, 20.0f, kNavy, kAlignLeft, bodySrc, false, true);
            float px = 64.0f;
            if (Sprite(crt, "Coin", 64, 33, 24, 24, coin)) px = 92.0f;
            Il2CppObject* price = Text(crt, "Price", px, 30, cellW - px - 6, 30, rows[i].base > 0 ? std::to_string(rows[i].base) : "?",
                                       24.0f, kNavy, kAlignLeft, titleSrc, false, true);
            Keep(dialogHandles, price);
            cells.push_back({price, rows[i].base});
        }
    }

    // Footer: Close only (round 15: the input help line is gone).
    if (gameButtonsOk)
        closeToggle = DialogButton(prt, "Close", kPanelW - kPad - 220, footY, 220, kRowH, "C", "Close", false, layer, closeGb,
                                   true, nullptr, nullptr, "<Keyboard>/c");
    Il2CppObject* cb = closeGb < 0 && !closeToggle ? NewUi("Close", prt, layer) : nullptr;
    if (cb) {
        At(unity::TransformOf(cb), kPanelW - kPad - 220, footY, 220, kRowH);
        SetRaycast(Shape(cb, kCyan, 28.0f), true);
        Text(unity::TransformOf(cb), "Label", 0, 0, 220, kRowH, "Close", 26.0f, kNavy, kAlignCenter, titleSrc);
        closeToggle = Clickable(cb);
        Keep(dialogHandles, closeToggle);
    }
    if (Faulted()) return failOpen("faulted");

    unity::SetActive(r, true);
    dialogKind = kDlgLimit;
    g_open.store(true);
    BuildSlider();
    ShowMode();
    SetFocus(focusRow);
    UpdateSheet();
    if (!loggedOpen) {
        loggedOpen = true;
        SR2_INFO("[{}] station ui: dialog opened for station {} ({} plorts, {} columns, price range 0..{}, limit {})",
                 module, station, n, cols, maxPrice, Describe(cur));
    }
    return true;
}

void StationUi::Impl::Close(const char* why) {
    const bool was = g_open.exchange(false);
    const int kind = dialogKind;
    dialogKind = 0;
    if (sliderReady) slider.Destroy();
    if (root && !Faulted()) Destroy(root);
    root = panelRt = blockerToggle = closeToggle = focusFrame = noteText = pHint = pModeText = nullptr;
    modePrev = modeNext = modeValue = nullptr;
    pHover = -1;
    pCol = 0;
    pLayoutFrames = 0;
    pDeletePending = false;
    titleSrc = bodySrc = nullptr;
    cells.clear();
    pItems.clear();
    for (Pill& P : dialogPills) FreeLater(P.handles);  // destroyed with root
    dialogPills.clear();
    for (gbn::Button& b : dialogButtons) b.Forget();  // destroyed with root
    dialogButtons.clear();
    dialogClicks.clear();
    closeGb = -1;
    FreeLater(dialogHandles);
    if (!was || !why) return;  // why == nullptr: rebuilt in place, no line
    if (kind == kDlgPrograms) SR2_INFO("[{}] station ui: programs dialog closed ({}), station {}", module, why, openStation);
    else
        SR2_INFO("[{}] station ui: dialog closed ({}), station {}: {}", module, why, openStation,
                 host.get ? Describe(host.get(openStation)) : std::string("?"));
}

void StationUi::Impl::BuildSlider() {
    if (!sliderReady || !panelRt) return;
    const bool fraction = mode == LimitMode::kFraction;
    sui::SliderSpec spec;
    spec.name = "DronePriceLimit";
    spec.min = 0;
    spec.max = fraction ? 200 : maxPrice;
    spec.step = fraction ? 5 : (maxPrice <= 200 ? 1 : maxPrice <= 1000 ? 5 : 10);
    spec.value = values[fraction ? 1 : 0];
    // Round 15: "slider should have a speed up if key held, holding shift scrolls by +-10, holding ctrl scrolls to
    // start/end".
    spec.fastKeys = true;
    spec.shiftStep = 10;
    spec.label = [fraction](int v) {
        if (v <= 0) return std::string(fraction ? "Minimum fraction of the base price: no limit" : "Minimum price: no limit");
        return fraction ? std::format("Minimum: {}% of the base price", v) : std::format("Minimum price: {} newbucks", v);
    };
    spec.onChange = [this](int v) {
        values[static_cast<int>(mode)] = v;
        Changed();
    };
    sui::SliderHost h;
    h.parent = panelRt;
    h.x = kPad;
    h.y = sliderY;
    h.width = kPanelW - 2 * kPad;
    h.textStyle = bodySrc;
    h.fontSize = 26.0f;
    slider.CreateIn(h, spec);
    slider.SetPolling(focusRow == 1);
}

void StationUi::Impl::Changed() {
    StationLimit l;
    l.mode = mode;
    l.value = values[static_cast<int>(mode)];
    host.set(openStation, l);
    UpdateSheet();
    UpdatePillLabel();
}

void StationUi::Impl::SetMode(LimitMode m) {
    if (m == mode) return;
    if (slider.Active()) values[static_cast<int>(mode)] = slider.Value();
    mode = m;
    BuildSlider();
    ShowMode();
    Changed();
}

void StationUi::Impl::ShowMode() {
    SetText(modeValue, mode == LimitMode::kFraction ? "Fraction of base price" : "Minimum price");
}

void StationUi::Impl::SetFocus(int row) {
    focusRow = std::clamp(row, 0, 1);
    if (unity::IsAlive(focusFrame)) {
        const float y = focusRow == 0 ? modeRowY - 8.0f : sliderY - 6.0f;
        const float h = focusRow == 0 ? kRowH + 16.0f : sui::PlotMenuSlider::Height() + 12.0f;
        At(unity::TransformOf(focusFrame), kPad - 14, y, kPanelW - 2 * kPad + 28, h);
    }
    if (sliderReady) slider.SetPolling(focusRow == 1);
}

void StationUi::Impl::UpdateSheet() {
    const int v = values[static_cast<int>(mode)];
    for (const Cell& c : cells) {
        if (!unity::IsAlive(c.price)) continue;
        std::string s = c.base > 0 ? std::to_string(c.base) : "?";
        if (mode == LimitMode::kFraction && v > 0 && c.base > 0) {
            const int lim = static_cast<int>(std::ceil(c.base * (v / 100.0) - 1e-4));
            s += std::format("  <size=75%>min {}</size>", lim);
        }
        SetText(c.price, s);
    }
}

void StationUi::Impl::Navigate(int dir) {
    if (!g_open.load()) return;
    if (!loggedNav) {  // proof for the test: the game's navigation (arrows / WASD / d-pad / stick) reaches us
        loggedNav = true;
        SR2_INFO("[{}] station ui: navigation input reached the {} dialog (direction {})", module,
                 dialogKind == kDlgPrograms ? "programs" : "price-limit", dir);
    }
    if (dialogKind == kDlgPrograms) {
        ProgNavigate(dir);
        return;
    }
    if (dir == kMoveUp) SetFocus(focusRow - 1);
    else if (dir == kMoveDown) SetFocus(focusRow + 1);
    else if (dir == kMoveLeft || dir == kMoveRight) {  // two modes: the selector steps (wraps) to the other one
        if (focusRow == 0) SetMode(mode == LimitMode::kPrice ? LimitMode::kFraction : LimitMode::kPrice);
        else if (sliderReady) slider.Step(dir == kMoveRight ? 1 : -1);
    }
}

void StationUi::Impl::Submit() {
    if (!g_open.load()) return;
    if (dialogKind == kDlgPrograms) {
        ProgSubmit();
        return;
    }
    if (focusRow == 0) SetMode(mode == LimitMode::kPrice ? LimitMode::kFraction : LimitMode::kPrice);
}

// ---------------------------------------------------------------- per frame --

void StationUi::Impl::Tick(float dt) {
    // A menu bound since the last frame: pin it (GC handle) and start over.
    if (Il2CppObject* b = boundMenu) {
        boundMenu = nullptr;
        if (b != menu.p && IsA(b, menuClass) && unity::IsAlive(b)) {
            ForgetMenu();
            menu.p = b;
            menu.h = unity::NewGCHandle(b);
            if (!menu.h) menu.p = nullptr;
            if (menu.p) {  // its DroneUIDisplay: "the station menu is open" on every tab
                bool ok = false, inactive = true;
                Il2CppObject* d = Call(compInParent, menu.p, {unity::TypeOf(displayClass), &inactive},
                                       "GetComponentInParent(DroneUIDisplay)", &ok);
                if (ok && unity::IsAlive(d)) {
                    menuDisplay.p = d;
                    menuDisplay.h = unity::NewGCHandle(d);
                    if (!menuDisplay.h) menuDisplay.p = nullptr;
                }
            }
        }
        stateCountdown = 0;
    }
    if (!menu.p) return;
    if (!unity::IsAlive(menu.p)) {
        ForgetMenu();
        return;
    }
    if (--stateCountdown <= 0 || g_open.load()) {
        stateCountdown = kStateFrames;
        ReadState();
        const bool wantLimit = pageActive && marketSelected && haveStation;
        // Programs: only with the Drone Program Scheduler upgrade (custom_upgrades; vanilla one program without).
        const bool wantProg = pageActive && haveStation && progs::Owned();
        SyncPill(limitPill, wantLimit, offMenuDest, "sr2mods_PriceLimitPrompt", "price-limit", "C");
        // The [P] Programs entry sits in the page's key bar; the old pill right of the target's Change prompt
        // only when the bar can't take it.
        const bool useHint = legendOk && !progHintFailed;
        if (useHint) SyncProgHint(wantProg);
        SyncPill(progPill, wantProg && !useHint, offMenuTarget, "sr2mods_ProgramsPrompt", "programs", "P");
        if (pageActive || saveLabel.p) SyncSaveLabel(pageActive && progs::Available());
        // No program switch while the station menu is open (any tab: closing it from the Drone List tab
        // also asks about unapplied page changes); and the page follows the station's program.
        if ((menuActive || displayActive) && haveStation && progs::Available()) {
            progs::NoteMenuOpen(station);
            FollowStation();
        }
        if (limitPill.shown && station != labelStation) {  // the same menu re-bound for another station
            labelStation = station;
            UpdatePillLabel();
        }
        if (progPill.shown || progHint.Alive()) UpdateProgramsLabel();
        // The page went away (tab switch, picker, destination changed, other station): close.
        if (g_open.load()) {
            const bool want = dialogKind == kDlgPrograms ? wantProg : wantLimit;
            if (!want || station != openStation || !unity::IsAlive(root)) Close("page changed");
        }
    }
    if (limitPill.shown && limitPill.reapply > 0) {
        --limitPill.reapply;
        ApplyPillLook(limitPill, "C");
        UpdatePillLabel();
    }
    if (progPill.shown && progPill.reapply > 0) {
        --progPill.reapply;
        ApplyPillLook(progPill, "P");
        UpdateProgramsLabel();
    }
    if (progHint.Alive()) progHint.Tick();
    const bool progShown = progPill.shown || progHint.Alive();
    for (Pill& P : dialogPills)  // the dialog's pill buttons: key and label again for a few frames
        if (P.reapply > 0 && unity::IsAlive(P.go)) {
            --P.reapply;
            ApplyPillLook(P, P.key.c_str());
            SetPillText(P, P.want);
        }

    // Hotkeys: C (or the pad's right stick click) toggles the price-limit dialog, P (left stick click)
    // the programs dialog; a click on a prompt opens its dialog.
    const bool open = g_open.load();
    const bool hotC = (limitPill.shown || (open && dialogKind == kDlgLimit)) &&
                      (KeyPressed(kbCurrent, kbC) || KeyPressed(padCurrent, padR3));
    const bool hotP = (progShown || (open && dialogKind == kDlgPrograms)) &&
                      (KeyPressed(kbCurrent, kbP) || KeyPressed(padCurrent, padL3));
    const bool clickedLimit = limitPill.shown && PillClicked(limitPill);
    const bool clickedProg = (progPill.shown && PillClicked(progPill)) || (progHint.Alive() && Clicked(progHintToggle));
    if (open) {
        // Every game button once per frame (its press / release edge and the reapplied look).
        dialogClicks.resize(dialogButtons.size());
        for (size_t i = 0; i < dialogButtons.size(); ++i) dialogClicks[i] = dialogButtons[i].Tick() ? 1 : 0;
        const bool hot = dialogKind == kDlgPrograms ? hotP : hotC;
        if (hot || Clicked(closeToggle, closeGb) || Clicked(blockerToggle)) {
            Close(hot ? (dialogKind == kDlgPrograms ? "P" : "C") : "click");
            return;
        }
        if (dialogKind == kDlgPrograms) {
            ProgInput();
            return;
        }
        const bool prev = Clicked(modePrev), next = Clicked(modeNext);
        if (prev || next) {  // the selector's arrows: the other mode
            SetFocus(0);
            SetMode(mode == LimitMode::kPrice ? LimitMode::kFraction : LimitMode::kPrice);
        }
        if (sliderReady) slider.Tick(dt);
    } else if ((hotC || clickedLimit) && limitPill.shown) {
        Open();
    } else if ((hotP || clickedProg) && progShown) {
        OpenPrograms(-1);
    }
}

// ---------------------------------------------------------------- programs dialog --

std::string StationUi::Impl::LocalizedText(Il2CppObject* ls) {
    std::string s;
    if (ls && sr2::core::text_override::Lookup(ls, s)) return s;  // our own sources (Hen Collector, ...)
    return Localized(ls);
}

// A source / destination / target-type definition of the menu's IDroneUIDefinitionSource: icon + name.
StationUi::Impl::Label StationUi::Impl::DefLabel(Il2CppObject* defs, const char* method, int32_t type, const char* what) {
    Label l;
    if (defs) {
        std::string err;
        Resolver& R = Resolver::Get();
        if (const MethodInfo* m = R.Method(api().il2cpp_object_get_class(defs), method, 1, &err)) {
            bool ok = false;
            Il2CppObject* d = Call(m, defs, {&type}, method, &ok);
            if (ok && d) {
                Il2CppClass* k = api().il2cpp_object_get_class(d);
                auto oIcon = R.FieldOffset(k, "_icon", &err), oName = R.FieldOffset(k, "_displayName", &err);
                if (oIcon) l.icon = unity::Field<Il2CppObject*>(d, *oIcon);
                if (oName) l.name = LocalizedText(unity::Field<Il2CppObject*>(d, *oName));
                if (!unity::IsAlive(l.icon)) l.icon = nullptr;
            }
        }
    }
    if (l.name.empty()) l.name = type == 0 ? std::string("(none)") : std::format("{} {}", what, type);
    return l;
}

StationUi::Impl::Label StationUi::Impl::TargetLabel(Il2CppObject* defs, const ProgramData& p) {
    if (p.target == kTargetSingle && IsA(p.ident, identClass) && unity::IsAlive(p.ident)) {
        Label l;
        l.icon = unity::Field<Il2CppObject*>(p.ident, offIdentIcon);
        if (!unity::IsAlive(l.icon)) l.icon = nullptr;
        l.name = Localized(unity::Field<Il2CppObject*>(p.ident, offIdentName));
        if (l.name.empty()) l.name = unity::GetName(p.ident);
        return l;
    }
    return DefLabel(defs, "FindTargetDefinition", p.target, "target");
}

// Round 14: "Not using the game's buttons as rows ... separate the Edit and delete buttons to the start of each
// program row (i like this idea) as the game's buttons", "'[SPACE] Add program' can be renamed to '[SPACE] Add',
// use the game's key icons please", "Next program mode selection doesnt align with options menu ... the ingame
// select component looks different". Each program row starts with two game buttons (game_button: [Space] Edit,
// [Z] Delete, the key icons are the game's own KeyboardMouseIcon entries), then the program; [Space] Add; "Next
// program" with a clone of the game's options selector. The focused button shows the game's hover look.
//
// Why the arrows / WASD seemed dead in rounds 12 and 13: the input arrived (FocusManagerBehaviour's move action
// -> FocusManager.NavigateInDirection, our detour) and the focus index moved, but the focus frame never showed.
// It was an SDF shape with a fully transparent fill and a border; a CanvasRenderer added at runtime culls a mesh
// whose vertex colours are all transparent (cullTransparentMesh, on by default in Unity 6), so the frame was
// never drawn, before (behind the rows) or after (above them). The focus now is the game button's own hover
// look, and the selector row's ring is an opaque shape behind the row.

// A row: an opaque focus ring behind it (hidden) and the row's rounded box. Returns the row's transform.
Il2CppObject* StationUi::Impl::Row(Il2CppObject* prt, const char* name, float x, float y, float w, float h, bool running,
                                   int layer, PItem& it) {
    it.ring = Box(prt, "FocusRing", x - 6.0f, y - 6.0f, w + 12.0f, h + 12.0f, kTeal, 22.0f);
    Keep(dialogHandles, it.ring);
    if (it.ring) unity::SetActive(it.ring, false);
    Il2CppObject* go = NewUi(name, prt, layer);
    if (!go) return nullptr;
    Il2CppObject* rt = unity::TransformOf(go);
    At(rt, x, y, w, h);
    Shape(go, running ? Color{0.80f, 0.93f, 0.95f, 1.0f} : kBacker, 16.0f, running ? 3.0f : 0.0f, kTeal);
    return rt;
}

// A transparent click target inside a row (above the row's own click). Returns its Toggle.
Il2CppObject* StationUi::Impl::ClickArea(Il2CppObject* parentRt, const char* name, float x, float y, float w, float h,
                                         int layer) {
    Il2CppObject* go = NewUi(name, parentRt, layer);
    if (!go) return nullptr;
    At(unity::TransformOf(go), x, y, w, h);
    Il2CppObject* img = AddComponent(go, imageClass, "AddComponent(Image)");
    SetColor(img, Color{1, 1, 1, 0});
    SetRaycast(img, true);
    Il2CppObject* t = Clickable(go);
    Keep(dialogHandles, t);
    return t;
}

// The game's options-menu selector prefab: the control prefab of a PresetOptionsItemDefinition whose root is an
// OptionsPresetSelector (the "< value >" stepper of the Settings rows). One asset scan per session.
Il2CppObject* StationUi::Impl::SelectorPrefab() {
    if (selPrefab.p && unity::IsAlive(selPrefab.p)) return selPrefab.p;
    if (selSearched || !presetSelClass) return nullptr;
    selSearched = true;
    std::string defName;
    int defs = 0;
    for (Il2CppObject* def : unity::FindObjectsOfTypeAll(presetDefClass)) {
        if (!IsA(def, presetDefClass) || !unity::IsAlive(def)) continue;
        ++defs;
        Il2CppObject* prefab = unity::Field<Il2CppObject*>(def, offDefPrefab);
        if (!unity::IsAlive(prefab) || !IsA(prefab, goClass)) continue;
        Il2CppObject* sel = unity::GetComponent(prefab, presetSelClass);
        if (!unity::IsAlive(sel) || api().il2cpp_object_get_class(sel) != presetSelClass) continue;  // not a toggle
        if (!unity::IsAlive(unity::Field<Il2CppObject*>(sel, offSelPrev)) ||
            !unity::IsAlive(unity::Field<Il2CppObject*>(sel, offSelNext)) ||
            !unity::IsAlive(unity::Field<Il2CppObject*>(sel, offSelText)))
            continue;
        selPrefab.p = prefab;
        selPrefab.h = unity::NewGCHandle(prefab);
        if (!selPrefab.h) selPrefab.p = nullptr;
        defName = unity::GetName(def);
        break;
    }
    if (selPrefab.p)
        SR2_INFO("[{}] station ui: options selector prefab '{}' (from the setting '{}', {} preset settings seen)", module,
                 unity::GetName(selPrefab.p), defName, defs);
    else
        SR2_WARN("[{}] station ui: no options selector prefab among {} preset settings - drawn selector", module, defs);
    return selPrefab.p;
}

// A clone of the game's selector at the right end of the row: its OptionsPresetSelector off (it would bind to a
// settings model), the value's LocalizeStringEvent off (our text), the arrows' SRButtons kept (their hover look)
// with a polled Toggle each. The dialog root is still inactive here, so the clone never wakes up with its
// behaviour on.
//
// Prefab OptionPresetControl (bundle data, round 15): root 280 x 56 with a HorizontalLayoutGroup (padding 64 left
// and right, spacing 16, controls child sizes), "Background" (SDFRoundedRectangle, radius 200, black at 20 %
// alpha, ignoreLayout), "PreviousPreset" / "NextPreset" (SRButton, anchored to the left / right edge, 64 / 70
// wide, ignoreLayout; child "Icon" = a 27 x 17 SDFTriangle 8.5 px from the inner side, white, hover cyan through
// the style asset OptionPresetStepButtonStyles) and "PresetDisplay/PresetLabel" (TMP, white, auto size 16..32,
// word wrap). Round 15 feedback: "the < > are too close to the option, should be at the edges and then some side
// padding", "also looks disabled rn" (white on 20 % black over our light row reads as greyed out), "option pill
// can be made wider as 'After each delivery' gets wrapped text". So: selW wide (the text gets selW - 128), the
// arrow icons kSelArrowPad from the pill's outer ends, the background the menu's navy, no word wrap.
bool StationUi::Impl::CloneSelector(Il2CppObject* rowRt, float selW, float rowH, Il2CppObject*& prev,
                                    Il2CppObject*& next, Il2CppObject*& text) {
    Il2CppObject* prefab = SelectorPrefab();
    if (!prefab) return false;
    Il2CppObject* clone = unity::Instantiate(prefab, rowRt);
    Il2CppObject* go = clone && IsA(clone, goClass) ? clone : unity::GameObjectOf(clone);
    if (!unity::IsAlive(go)) return false;
    Keep(dialogHandles, go);
    unity::SetName(go, "sr2mods_OptionsSelector");
    Il2CppObject* sel = unity::GetComponent(go, presetSelClass);
    Il2CppObject* lse = unity::IsAlive(sel) ? unity::Field<Il2CppObject*>(sel, offSelText) : nullptr;
    Il2CppObject* prevBtn = unity::IsAlive(sel) ? unity::Field<Il2CppObject*>(sel, offSelPrev) : nullptr;
    Il2CppObject* nextBtn = unity::IsAlive(sel) ? unity::Field<Il2CppObject*>(sel, offSelNext) : nullptr;
    Il2CppObject* tmp = unity::IsAlive(lse) ? TmpOf(lse) : nullptr;
    if (!unity::IsAlive(sel) || !tmp || !unity::IsAlive(prevBtn) || !unity::IsAlive(nextBtn)) {
        Destroy(go);
        return false;
    }
    SetEnabled(sel, false);
    SetEnabled(lse, false);
    // Its own height (the Settings row's control), scaled down to the row's height when taller; our width.
    Il2CppObject* rt = unity::TransformOf(go);
    bool ok = false;
    Il2CppObject* boxed = Call(rtGetRect, rt, {}, "RectTransform.get_rect", &ok);
    float w = 0, h = 0;
    if (ok && boxed) {
        const float* r = reinterpret_cast<const float*>(reinterpret_cast<char*>(boxed) + 2 * sizeof(void*));
        w = r[2];
        h = r[3];
    }
    const float ow = w, oh = h;
    if (h < 20.0f || h > 240.0f) h = rowH - 8.0f;
    w = std::max(selW, w < 120.0f || w > 1000.0f ? 0.0f : w);
    const float sc = std::min(1.0f, (rowH - 8.0f) / h);
    Rect(rt, {1, 0.5f}, {1, 0.5f}, {1, 0.5f}, {-18.0f, 0}, {w, h});
    float s3[3] = {sc, sc, 1.0f};
    Call(trSetLocalScale, rt, {s3}, "Transform.set_localScale");
    // Look: navy pill, arrows at the outer ends, the value on one line.
    int restyled = 0;
    if (Il2CppObject* bg = Find(rt, "Background")) {
        Il2CppObject* g = sdfRectClass ? unity::GetComponent(bg, sdfRectClass) : nullptr;
        if (!unity::IsAlive(g)) g = unity::GetComponent(bg, imageClass);
        if (unity::IsAlive(g)) {
            SetColor(g, kNavy);
            ++restyled;
        }
    }
    const float side[2] = {0.0f, 1.0f};
    Il2CppObject* btns[2] = {prevBtn, nextBtn};
    for (int i = 0; i < 2; ++i) {
        Il2CppObject* icon = Find(unity::TransformOf(btns[i]), "Icon");
        if (!icon || !IsA(icon, rtClass)) continue;
        // Icon centre kSelArrowPad from the pill's end (the buttons sit on the root's edges).
        SetVec(rtSetAnchorMin, icon, {side[i], 0.5f}, "RectTransform.set_anchorMin");
        SetVec(rtSetAnchorMax, icon, {side[i], 0.5f}, "RectTransform.set_anchorMax");
        SetVec(rtSetPos, icon, {i == 0 ? kSelArrowPad : -kSelArrowPad, 0.0f}, "RectTransform.set_anchoredPosition");
        ++restyled;
    }
    if (tmpSetWrap) {
        int32_t noWrap = 0;  // TextWrappingModes.NoWrap (auto size shrinks it to the width instead)
        Call(tmpSetWrap, tmp, {&noWrap}, "TMP_Text.set_textWrappingMode");
    }
    prev = Clickable(unity::GameObjectOf(prevBtn));
    next = Clickable(unity::GameObjectOf(nextBtn));
    Keep(dialogHandles, prev);
    Keep(dialogHandles, next);
    text = tmp;
    if (!loggedSelector) {
        loggedSelector = true;
        SR2_INFO("[{}] station ui: options selector cloned from the game's (rect {:.0f} x {:.0f} -> {:.0f} x {:.0f}, scale "
                 "{:.2f}; {}/3 parts restyled: navy background, arrows {:.0f} px from the ends)",
                 module, ow, oh, w, h, sc, restyled, kSelArrowPad);
    }
    return !Faulted();
}

void StationUi::Impl::Selector(Il2CppObject* rowRt, float rowW, float rowH, float selW, int layer, Il2CppObject*& prev,
                               Il2CppObject*& next, Il2CppObject*& text) {
    prev = next = text = nullptr;
    if (!CloneSelector(rowRt, selW, rowH, prev, next, text)) {  // drawn fallback: white pill, teal arrows
        const float arrowW = 64.0f, sx = rowW - 18.0f - selW;
        Box(rowRt, "Selector", sx, 8.0f, selW, rowH - 16.0f, kWhite, (rowH - 16.0f) * 0.5f);
        Text(rowRt, "Prev", sx, 0, arrowW, rowH, "<", 34.0f, kTeal, kAlignCenter, titleSrc);
        Text(rowRt, "Next", sx + selW - arrowW, 0, arrowW, rowH, ">", 34.0f, kTeal, kAlignCenter, titleSrc);
        text = Text(rowRt, "Value", sx + arrowW, 0, selW - 2 * arrowW, rowH, "", 26.0f, kNavy, kAlignCenter, titleSrc);
        prev = ClickArea(rowRt, "PrevClick", sx, 0, arrowW, rowH, layer);
        next = ClickArea(rowRt, "NextClick", sx + selW - arrowW, 0, arrowW, rowH, layer);
    }
    Keep(dialogHandles, text);
}

bool StationUi::Impl::OpenPrograms(int focus) {
    if (g_open.load() || !haveStation) return false;
    Il2CppObject* model = StationModel();
    progs::View view;
    if (!model || !progs::Get(model, view) || view.programs.empty()) return false;
    auto failOpen = [&](const char* what) {
        if (!loggedOpenFail) SR2_WARN("[{}] station ui: programs dialog not opened: {}", module, what);
        loggedOpenFail = true;
        Close("open failed");
        return false;
    };
    const int n = static_cast<int>(view.programs.size());
    const bool full = n >= progs::kMaxPrograms;
    pCount = n;
    pRunning = view.running;
    pMode = static_cast<int>(view.mode);
    pExplorer = view.explorer;
    // Explorer mode: a program is its target only and runs until another is chosen here (no rotation, so no
    // "Next program" selector).
    const bool withMode = n >= 2 && !view.explorer;

    // ---- layout ----
    const float titleY = 28.0f, subY = 84.0f;
    const float rowsY = 196.0f;
    const float addH = 72.0f, modeH = 72.0f, gap = 16.0f;
    const float addY = rowsY + n * (kProgRowH + kProgRowGap) + 6.0f;
    const float modeY = addY + (full ? kRowH : addH) + gap;
    const float footY = (withMode ? modeY + modeH : addY + (full ? kRowH : addH)) + 28.0f;
    const float panelH = footY + kRowH + 26.0f;
    int layer = -1;
    if (!BuildShell("sr2mods_DroneProgramsDialog", panelH, layer)) return failOpen("dialog frame not built");
    Il2CppObject* prt = panelRt;
    openStation = station;
    pItems.clear();

    Text(prt, "Title", kPad, titleY, kPanelW - 2 * kPad, 50, "Drone programs", 40.0f, kNavy, kAlignLeft, titleSrc);
    Text(prt, "Subtitle", kPad, subY, kPanelW - 2 * kPad, 100,
         view.explorer
             ? std::format("This explorer drone can have up to {} programs (what it gathers). Edit: that program runs "
                           "now, until you pick another here, and this page shows it - change it here and press Save "
                           "Program. The game's save keeps program 1; the mod keeps the others.",
                           progs::kMaxPrograms)
             : std::format("This station's drone can have up to {} programs and runs them in turn. Edit: that program "
                           "runs now and this page shows it - change it here and press Save Program. The game's save "
                           "keeps program 1; the mod keeps the others.",
                           progs::kMaxPrograms),
         22.0f, kNavy, kAlignTopLeft, bodySrc, true);

    const float rowW = kPanelW - 2 * kPad;
    const float btnX = 16.0f, editW = 190.0f, delW = 210.0f;
    const char* kSpace = "<Keyboard>/space";

    // ---- one row per program: [Space] Edit, [Z] Delete, then the program ----
    Il2CppObject* defs = offVmDefs ? unity::Field<Il2CppObject*>(unity::Field<Il2CppObject*>(menu.p, offMenuVm), offVmDefs)
                                   : nullptr;
    Il2CppObject* activeSprite = offMenuActiveSprite ? unity::Field<Il2CppObject*>(menu.p, offMenuActiveSprite) : nullptr;
    const float midY = kProgRowH * 0.5f;
    const float numX = 470.0f, activeX = 512.0f, cellX0 = 562.0f, sepW = 24.0f;
    for (int i = 0; i < n; ++i) {
        const ProgramData& pd = view.programs[i];
        const float ry = rowsY + i * (kProgRowH + kProgRowGap);
        const bool running = i == view.running;
        PItem it;
        it.kind = kItemProgram;
        it.index = i;
        Il2CppObject* rrt = Row(prt, "Program", kPad, ry, rowW, kProgRowH, running, layer, it);
        if (!rrt) return failOpen("row");
        it.tgl[0] = DialogButton(rrt, "Edit", btnX, 0, editW, kProgRowH, "Space", "Edit", false, layer, it.gb[0], false,
                                 nullptr, nullptr, kSpace);
        // Delete: always shown (greyed with one program), a focus stop only with two or more.
        int delGb = -1;
        Il2CppObject* delTgl = DialogButton(rrt, "Delete", btnX + editW + 14.0f, 0, delW, kProgRowH, "Z", "Delete", false,
                                            layer, delGb, false, nullptr, nullptr, "<Keyboard>/z");
        it.delBtn = delGb;
        if (n >= 2) {
            it.gb[1] = delGb;
            it.tgl[1] = delTgl;
        } else if (delGb >= 0) {
            dialogButtons[static_cast<size_t>(delGb)].SetInteractable(false);
        }
        Text(rrt, "Number", numX, 0, 40, kProgRowH, std::to_string(i + 1), 34.0f, kNavy, kAlignCenter, titleSrc);
        if (running) Sprite(rrt, "Active", activeX, midY - 20.0f, 40, 40, activeSprite);
        const float cellsEnd = rowW - 20.0f;
        const float cellW = (cellsEnd - cellX0 - 2 * sepW) / 3.0f;
        Label cells3[3] = {TargetLabel(defs, pd), {}, {}};
        if (view.explorer) {
            cells3[1].name = "Explorer drone";  // no source / destination
        } else {
            cells3[1] = DefLabel(defs, "FindSourceDefinition", pd.source, "source");
            cells3[2] = DefLabel(defs, "FindDestinationDefinition", pd.sink, "destination");
        }
        for (int c = 0; c < (view.explorer ? 2 : 3); ++c) {
            const float cx = cellX0 + c * (cellW + sepW);
            float tx = cx;
            if (Sprite(rrt, "Icon", cx, 14, 52, 52, cells3[c].icon)) tx = cx + 58.0f;
            Text(rrt, "Name", tx, 4, cx + cellW - tx, kProgRowH - 8, cells3[c].name, 21.0f, kNavy, kAlignLeft, bodySrc, true, true);
            if (c < 2 && !view.explorer)
                Text(rrt, "Arrow", cx + cellW, 0, sepW, kProgRowH, ">", 26.0f, kNavy, kAlignCenter, titleSrc);
        }
        pItems.push_back(it);
    }

    // ---- [Space] Add ----
    if (!full) {
        PItem it;
        it.kind = kItemAdd;
        Il2CppObject* art = Row(prt, "Add", kPad, addY, rowW, addH, false, layer, it);
        if (!art) return failOpen("add row");
        it.tgl[0] = DialogButton(art, "Add", btnX, 0, editW, addH, "Space", "Add", false, layer, it.gb[0], false, nullptr,
                                 nullptr, kSpace);
        pHint = Text(art, "AddHint", btnX + editW + 30.0f, 0, rowW - editW - 66.0f, addH,
                     "A copy of the running program; it runs now - change it on this page and press Save Program.", 20.0f,
                     kNavy, kAlignLeft, bodySrc, true);
        pItems.push_back(it);
    } else {
        pHint = Text(prt, "AddHint", kPad, addY, rowW, kRowH,
                     std::format("A station can have up to {} programs.", progs::kMaxPrograms), 22.0f, kNavy, kAlignLeft,
                     bodySrc, true);
    }
    Keep(dialogHandles, pHint);

    // ---- when to take the next program: the game's own options selector (two or more programs) ----
    if (withMode) {
        PItem it;
        it.kind = kItemMode;
        Il2CppObject* mrt = Row(prt, "Mode", kPad, modeY, rowW, modeH, false, layer, it);
        if (!mrt) return failOpen("mode row");
        Text(mrt, "ModeLabel", 24.0f, 0, 400.0f, modeH, "Next program", 26.0f, kNavy, kAlignLeft, titleSrc);
        Selector(mrt, rowW, modeH, kProgSelW, layer, it.prev, it.next, pModeText);
        SetText(pModeText, pMode == 0 ? "When idle" : "After each delivery");
        pItems.push_back(it);
    }

    // ---- footer: Close only ----
    closeToggle = DialogButton(prt, "Close", kPanelW - kPad - 220, footY, 220, kRowH, "P", "Close", false, layer, closeGb, true,
                               nullptr, nullptr, "<Keyboard>/p");
    if (Faulted()) return failOpen("faulted");

    unity::SetActive(root, true);
    dialogKind = kDlgPrograms;
    g_open.store(true);
    pHover = -1;
    pLayoutFrames = 40;
    ProgFocus(focus < 0 ? view.running : focus, 0);
    if (!loggedProgOpen) {
        loggedProgOpen = true;
        SR2_INFO("[{}] station ui: programs dialog opened for station {} ({} program(s), program {} runs, {}; {} rows, "
                 "{} game buttons)",
                 module, station, n, view.running + 1, view.explorer ? "explorer mode" : progs::ModeName(view.mode),
                 pItems.size(), dialogButtons.size());
    }
    return true;
}

// The focus: row i, column col (-1 = keep; clamped to the row's buttons). The focused game button shows the
// game's hover look; the selector row (and rows of fallback buttons) its ring.
void StationUi::Impl::ProgFocus(int i, int col) {
    if (pItems.empty()) return;
    pFocus = std::clamp(i, 0, static_cast<int>(pItems.size()) - 1);
    const PItem& f = pItems[static_cast<size_t>(pFocus)];
    pCol = std::clamp(col < 0 ? pCol : col, 0, f.Cols() - 1);
    for (int j = 0; j < static_cast<int>(pItems.size()); ++j) {
        const PItem& it = pItems[static_cast<size_t>(j)];
        // Round 15: "the programs row should only show the buttons enabled when in that row, otherwise gets
        // confusing" - a program row's Edit / Delete have the game's disabled grey unless the row has the focus
        // (a pointer on one still focuses its row first, see ProgInput, so a click works). The greyed Delete of a
        // single program (not in gb[1]) stays disabled.
        for (int c = 0; c < 2; ++c) {
            if (it.gb[c] < 0 || it.gb[c] >= static_cast<int>(dialogButtons.size())) continue;
            gbn::Button& b = dialogButtons[static_cast<size_t>(it.gb[c])];
            const bool rowOn = it.kind != kItemProgram || j == pFocus;
            if (!rowOn) b.SetFocused(false);  // before greying it: the hover look is dropped while it can still react
            b.SetInteractable(rowOn);
            if (rowOn) b.SetFocused(j == pFocus && c == pCol);
        }
        const bool ring = j == pFocus && (it.kind == kItemMode || it.gb[pCol] < 0);
        if (unity::IsAlive(it.ring)) unity::SetActive(it.ring, ring);
    }
}

void StationUi::Impl::ProgNavigate(int dir) {
    if (pItems.empty()) return;
    if (dir == kMoveUp || dir == kMoveDown) {
        ProgFocus(pFocus + (dir == kMoveDown ? 1 : -1));
        return;
    }
    if (dir != kMoveLeft && dir != kMoveRight) return;
    if (pItems[static_cast<size_t>(pFocus)].kind == kItemMode) ProgStepMode(dir == kMoveRight ? 1 : -1);
    else ProgFocus(pFocus, pCol + (dir == kMoveRight ? 1 : -1));
}

// A button pressed (Submit on the focus, or a click): the press animation, then its action.
void StationUi::Impl::ProgPress(const PItem& it, int col) {
    if (col >= 0 && col < 2 && it.gb[col] >= 0 && it.gb[col] < static_cast<int>(dialogButtons.size()))
        dialogButtons[static_cast<size_t>(it.gb[col])].PlayPress();
    if (it.kind == kItemProgram) {
        if (col == 1) ProgDelete(it.index);
        else ProgEdit(it.index);
    } else if (it.kind == kItemAdd) {
        ProgAdd();
    } else if (it.kind == kItemMode) {
        ProgStepMode(1);
    }
}

void StationUi::Impl::ProgSubmit() {
    if (pFocus < 0 || pFocus >= static_cast<int>(pItems.size())) return;
    const PItem it = pItems[static_cast<size_t>(pFocus)];
    ProgPress(it, pCol);
}

// The page shows the station's program again (after we changed which one runs).
void StationUi::Impl::RevertPage() {
    Il2CppObject* vm = menu.p ? unity::Field<Il2CppObject*>(menu.p, offMenuVm) : nullptr;
    if (vmRevert && IsA(vm, vmClass)) Call(vmRevert, vm, {}, "DroneProgramViewModel.RevertChangesToLastAppliedProgram");
}

// Edit: that program runs now and the page shows it; the dialog closes.
void StationUi::Impl::ProgEdit(int index) {
    Il2CppObject* model = StationModel();
    if (!model) return;
    if (index != pRunning && !progs::Run(model, index)) {
        SetText(pHint, "That program could not be started (see the log).");
        return;
    }
    if (index != pRunning) RevertPage();  // the running one: keep what the page has (maybe not applied yet)
    UpdateProgramsLabel();
    Close(index != pRunning ? "program chosen" : "edit the running program");
}

// Delete (2+ programs): the dialog is rebuilt in place, the focus stays on that row (or the one above).
void StationUi::Impl::ProgDelete(int index) {
    Il2CppObject* model = StationModel();
    if (!model || pCount < 2) return;
    const int focus = pFocus;
    if (!progs::Remove(model, index)) return;
    if (index == pRunning) RevertPage();  // another program runs now
    UpdateProgramsLabel();
    SR2_INFO("[{}] station ui: station {}: program {} deleted in the programs dialog", module, openStation, index + 1);
    Close(nullptr);
    OpenPrograms(std::min(focus, pCount - 2));
}

void StationUi::Impl::ProgAdd() {
    Il2CppObject* model = StationModel();
    if (!model) return;
    if (!progs::Add(model)) {
        SetText(pHint, pExplorer ? "Choose what the explorer drone gathers on this page first."
                                 : "Set up a complete program on this page first (target, source and destination).");
        return;
    }
    UpdateProgramsLabel();
    Close("program added");
}

// The selector: the next / previous mode (wraps, as the game's settings selectors); applied at once.
void StationUi::Impl::ProgStepMode(int dir) {
    Il2CppObject* model = StationModel();
    if (!model || pCount < 2) return;
    const int m = ((pMode + dir) % 2 + 2) % 2;
    if (!progs::SetMode(model, static_cast<progs::Mode>(m))) return;
    pMode = m;
    SetText(pModeText, m == 0 ? "When idle" : "After each delivery");
}

bool StationUi::Impl::PointerInside(Il2CppObject* selectable) {
    if (!selPointerInside || !unity::IsAlive(selectable)) return false;
    bool ok = false;
    return Unbox<bool>(Call(selPointerInside, selectable, {}, "Selectable.get_isPointerInside", &ok)) && ok;
}

// Every frame while the programs dialog is open: clicks, hover focus, the Delete key, the Delete buttons'
// placement after the Edit buttons' layout.
void StationUi::Impl::ProgInput() {
    if (pLayoutFrames > 0) {
        --pLayoutFrames;
        for (const PItem& it : pItems) {
            if (it.kind != kItemProgram || it.gb[0] < 0 || it.delBtn < 0) continue;
            const size_t e = static_cast<size_t>(it.gb[0]), d = static_cast<size_t>(it.delBtn);
            if (e >= dialogButtons.size() || d >= dialogButtons.size()) continue;
            float w = 0, h = 0;
            dialogButtons[e].Size(w, h);
            if (w > 40.0f) dialogButtons[d].Place(16.0f + w + 14.0f, kProgRowH * 0.5f, 0.0f);
        }
    }
    for (int i = 0; i < static_cast<int>(pItems.size()); ++i) {
        const PItem it = pItems[static_cast<size_t>(i)];
        const bool prev = Clicked(it.prev), next = Clicked(it.next);
        if (prev || next) {  // the selector's arrows
            ProgFocus(i, 0);
            ProgStepMode(prev ? -1 : 1);
            return;
        }
        for (int c = 0; c < 2; ++c)
            if ((it.gb[c] >= 0 || it.tgl[c]) && Clicked(it.tgl[c], it.gb[c])) {
                ProgFocus(i, c);
                ProgPress(it, c);  // Edit / Add / Delete may close or rebuild the dialog: stop here
                return;
            }
    }
    // Hover: the button (or selector arrow) under the pointer takes the focus when the pointer moves onto it.
    int h = -1;
    for (int i = 0; i < static_cast<int>(pItems.size()) && h < 0; ++i) {
        const PItem& it = pItems[static_cast<size_t>(i)];
        for (int c = 0; c < 2 && h < 0; ++c) {
            if (it.gb[c] >= 0 && it.gb[c] < static_cast<int>(dialogButtons.size()) &&
                dialogButtons[static_cast<size_t>(it.gb[c])].Hovered())
                h = i * 2 + c;
            else if (it.gb[c] < 0 && it.tgl[c] && PointerInside(it.tgl[c]))
                h = i * 2 + c;
        }
        if (h < 0 && (PointerInside(it.prev) || PointerInside(it.next))) h = i * 2;
    }
    if (h != pHover) {
        pHover = h;
        if (h >= 0 && (h / 2 != pFocus || h % 2 != pCol)) ProgFocus(h / 2, h % 2);
    }
    // Delete: Z (polled) or the page's Clear All event (its pad button); one per 300 ms (both may fire).
    const bool z = KeyPressed(kbCurrent, kbZ);
    if (z || pDeletePending) {
        pDeletePending = false;
        const uint64_t now = GetTickCount64();
        if (now - pLastDeleteMs > 300 && pFocus >= 0 && pFocus < static_cast<int>(pItems.size()) &&
            pItems[static_cast<size_t>(pFocus)].kind == kItemProgram && pCount >= 2) {
            pLastDeleteMs = now;
            const PItem it = pItems[static_cast<size_t>(pFocus)];
            if (it.gb[1] >= 0 && it.gb[1] < static_cast<int>(dialogButtons.size()))
                dialogButtons[static_cast<size_t>(it.gb[1])].PlayPress();
            ProgDelete(it.index);
        }
    }
}

// "Save Program" (round 14: "rename 'Start Program' to 'Save Program' makes more sense imo how the button
// actually functions"): while custom upgrades are on, the page's key bar entry bound to its activate event
// (DroneProgramMenu._activateEvent, the X key) reads "Save Program". Its LocalizedString (the entry's
// InputHintConfiguration.Label, an asset shared by nothing else) is registered in text_override, so the game's
// own LocalizeStringEvent writes our text whenever it refreshes; the label TMP is set too in case it already
// shows the old text. Undone when custom upgrades go off.
void StationUi::Impl::SyncSaveLabel(bool want) {
    if (!offMenuActivate || !offHintConfig || !offHintLabelText || !offCfgLabel || !legendOk) return;
    if (!want) {
        if (saveLabel.p) {
            sr2::core::text_override::Remove(saveLabel.p);
            if (unity::IsAlive(saveText) && !saveOrig.empty()) SetText(saveText, saveOrig);
            Release(saveLabel);
            saveText = nullptr;
        }
        return;
    }
    if (!menu.p) return;
    std::string chain;
    Il2CppObject* legend = legend_hint::FollowLegend(unity::Field<Il2CppObject*>(menu.p, offMenuLegend), menu.p,
                                                     "DroneProgramMenu._inputLegend", chain);
    Il2CppObject* evt = unity::Field<Il2CppObject*>(menu.p, offMenuActivate);
    Il2CppObject* hint = legend && evt ? legend_hint::HintFor(legend, evt) : nullptr;
    if (!hint) return;
    Il2CppObject* cfg = unity::Field<Il2CppObject*>(hint, offHintConfig);
    Il2CppObject* ls = cfg ? unity::Field<Il2CppObject*>(cfg, offCfgLabel) : nullptr;
    Il2CppObject* text = unity::Field<Il2CppObject*>(hint, offHintLabelText);
    if (ls && IsA(ls, lsClass) && ls != saveLabel.p) {
        if (saveLabel.p) sr2::core::text_override::Remove(saveLabel.p);
        Release(saveLabel);
        saveLabel.p = ls;
        saveLabel.h = unity::NewGCHandle(ls);
        sr2::core::text_override::Set(ls, "Save Program");
    }
    if (!unity::IsAlive(text)) return;
    bool ok = false;
    Il2CppObject* cur = Call(tmpGetText, text, {}, "TMP_Text.get_text", &ok);
    const std::string now = ok && cur ? unity::ToUtf8(reinterpret_cast<Il2CppString*>(cur)) : std::string();
    if (now == "Save Program") return;
    if (!now.empty()) saveOrig = now;
    saveText = text;
    SetText(text, "Save Program");
    if (!saveLogged) {
        saveLogged = true;
        SR2_INFO("[{}] station ui: key bar entry '{}' reads \"Save Program\" (text override {})", module, saveOrig,
                 sr2::core::text_override::SinkActive() ? "active" : "NOT active - label set directly");
    }
}


// ---------------------------------------------------------------- public --

void StationUi::Tick(float dt) {
    if (!impl_ || !main_thread::IsMainThread()) return;
    if (sr2::core::ModuleFaulted(impl_->module)) return;
    impl_->Tick(dt);
}

void StationUi::Shutdown() {
    if (!impl_ || !main_thread::IsMainThread()) return;
    Impl& p = *impl_;
    if (sr2::core::ModuleFaulted(p.module)) {  // no game calls after a fault: forget, leak nothing we can't free
        g_open.store(false);
        p.root = p.limitPill.go = p.progPill.go = nullptr;
        p.dialogKind = 0;
        Impl::FreeLater(p.dialogHandles);
        Impl::FreeLater(p.limitPill.handles);
        Impl::FreeLater(p.progPill.handles);
        for (auto& P : p.dialogPills) Impl::FreeLater(P.handles);
        p.dialogPills.clear();
        for (auto& b : p.dialogButtons) b.Forget();
        p.dialogButtons.clear();
        p.limitPill.gb.Forget();
        p.progPill.gb.Forget();
        p.progHint.Abandon();
        p.progHintToggle = nullptr;
        p.pItems.clear();
        Impl::FreeLater(p.progHintHandles);
        if (p.saveLabel.p) sr2::core::text_override::Remove(p.saveLabel.p);
        Impl::Release(p.saveLabel);
        Impl::Release(p.menu);
        Impl::Release(p.menuDisplay);
        return;
    }
    p.SyncSaveLabel(false);
    p.ForgetMenu();
    p.boundMenu = nullptr;
}

}  // namespace sr2::modules::drone_price_limit
