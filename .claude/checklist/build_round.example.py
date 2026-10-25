import json

cases = json.load(open('r10-cases.json', encoding='utf8'))

# Sections by where the user tests them, in walking order.
ORDER = [
    "Menu only",
    "Title screen",
    "Main menu -> load a save",
    "Ranch: anywhere (Sandbox on)",
    "Ranch: anywhere",
    "Ranch: Fabricator",
    "Ranch: gadget mode",
    "Ranch: placed Quantum Storage Link",
    "Ranch: DJ Booth placed",
    "Ranch: corral with a Music Box",
    "Ranch: corral",
    "Ranch: corral with a Solar Shield (day)",
    "Ranch: corral without a Solar Shield",
    "Ranch: any corral",
    "Ranch: coop",
    "Ranch: garden",
    "Ranch: silo",
    "Ranch: expansion door",
    "Ranch: drone station",
    "Shop",
    "Full map (Reveal everything off)",
    "Rainbow Fields",
    "Grey Labyrinth",
]
by = {}
for loc, cid, title, steps in cases:
    by.setdefault(loc, []).append([cid, title, steps])
unknown = [l for l in by if l not in ORDER]
assert not unknown, unknown

secs = [[loc, by[loc]] for loc in ORDER if loc in by]
secs += [
    ["Skipped / Menu and settings", ["r8dt2", "r9nr3"]],
    ["Skipped / Sandbox", ["r7sb8", "r9wx3", "r9wx4"]],
    ["Skipped / Fabricator and buildables", ["r7bd6", "r9rs3"]],
    ["Skipped / Drones and shadow slimes", ["r9dp4", "r9dp5", "r9sh4", "r9sh5", "r9sh7", "r7dr2", "r4dr3"]],
    ["Skipped / Map", ["r8tp3", "r8tp4", "r4mm4"]],
    ["Skipped / Vacpack, plots and upgrades", ["r7vu2", "r6vu4", "hc4", "p7", "ug2", "ug3", "sv2", "ut6"]],
]
json.dump(secs, open('r10-new.json', 'w', encoding='utf8'), ensure_ascii=False, indent=1)
print(len(cases), 'new cases in', len([s for s in secs if ' / ' not in s[0]]), 'places')
