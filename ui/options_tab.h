#pragma once
// "Mods" tab inside the game's own Options menu.
//
// The SR2 Options menu is data-driven (MonomiPark.SlimeRancher.Options):
// OptionsConfiguration (ListAsset<OptionsItemCategory>) -> one tab per
// OptionsItemCategory (ListAsset<OptionsItemDefinition>) -> one row per
// definition, rendered by the definition's _controlPrefab. Rather than cloning
// UI GameObjects we inject DATA, so the game's own adapters build the tab and
// the rows (native look, gamepad navigation and focus handling for free):
//
//  * OptionsUIRoot.BindCategories (hook): a runtime clone of an existing
//    OptionsItemCategory ("Mods") is appended to the configuration's list for
//    the duration of the call, so the tab strip and `categories` include it.
//  * Each registered setting gets a runtime clone of an existing
//    ToggleOptionDefinition (Bool) or ScriptedValueRangeOptionDefinition
//    (Float / Int / Enum, as a left/right stepper with the Def's step), with a
//    unique _referenceId and private ScriptedBool/ScriptedFloat targets.
//  * OptionsViewModel.GetOptionsItemsForCategory (hook): for the Mods tab it
//    returns models created from our definitions (never registered in the
//    game's OptionsModel, so nothing reaches the game's settings file).
//  * Toggle/ScriptedValueRange ApplyPresetSelection (hooks): for our
//    definitions, the selection is written with settings::Set only.
//  * Labels bypass localization: CategoryTabViewHolder.BindData,
//    OptionsItemViewHolder.BindData, OptionsPresetSelector.OnSelectionChanged
//    and OptionsDetailsView.BindData (hooks) disable the LocalizeStringEvent of
//    our rows and set the TMP text directly (re-enabled for game rows).
//
// Key-kind settings cannot be expressed as a stepper and are overlay-only.

#include "ui.h"
