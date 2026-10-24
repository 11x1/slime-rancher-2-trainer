#pragma once
// custom_upgrades: more level circles in the Fabricator for an upgrade with more levels than the
// prefab has circles (Tank Booster I-XII; vacpack_upgrades.cpp extends the vanilla definition).
// SR2 1.3.0. Design + findings: modules/custom_upgrades/NOTES.md "Vacpack upgrades".
//
// Vanilla (menulayer_fabricator scene, UnityPy; disassembly):
//   * The details panel's UpgradeChain ("Upgrade Chain") has 8 circles Pip0..Pip7 (_levels,
//     SDFGraphic[], 42 x 22 at x = 21 + 52 i, absolute positions, no layout group), an empty _rects
//     and a highlight (54 x 34). UpgradeChain.Show(current, count) (0x82ED90) shows the first `count`
//     circles and moves the highlight onto _levels[current + 1]: with 12 levels and Tank Booster VIII
//     owned that index is 8 -> IndexOutOfRangeException, the panel breaks.
//   * Every grid button has a FabricatorItemButtonPips with 8 pips (22 x 11, two rows of four, pitch
//     24 / -14). FabricatorItemButton.ConfigurePips(unlocked, total) (0x806CE0, Configure inlined)
//     only walks the array, so a 12-level upgrade would show 8 pips.
// The pre-hooks give an instance with too few circles / pips clones of its last one (same parent)
// and a longer array, then lay them out for the count being shown: more than the prefab had ->
// narrower and closer, in the prefab's width (chain: one row; pips: the prefab's rows, more
// columns); otherwise the prefab's own positions and sizes. Nothing changes for upgrades with up to
// 8 levels. Main thread (the hooks run there).

#include <string>

namespace sr2::core {
class ModuleContext;
}

namespace sr2::modules::custom_upgrades::fabricator {

class FabricatorLevels {
public:
    // Worker thread: resolve by name, create the two hooks. `chainOk` = the details panel's hook is
    // in place (without it an upgrade must not get more than 8 levels).
    bool Setup(sr2::core::ModuleContext& ctx, const char* module, bool& chainOk, std::string& why, std::string& report);
};

}  // namespace sr2::modules::custom_upgrades::fabricator
