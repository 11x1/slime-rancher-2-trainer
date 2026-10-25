"""A corral's plot menu with the mod's custom upgrade cards next to the game's own.

The icons: the user's drawings once .claude/notes/custom-icons.md is done (vanilla icons meanwhile), so
retake this shot after that lands.
"""
TITLE = "Custom plot upgrades"
CAPTION = ("New plot upgrades in the game's own menus: Corral Radio, Music Box II, Faster Hens, Sprinkler tiers "
           "and more, each with its own card.")
FEATURE = "custom_upgrades"
SLOT = 5
SETTINGS = {"custom_upgrades.enabled": True}
TIME = 12
PLOT_MENU = "corral"


def setup(t):
    t.exec("custom_upgrades.set_on", on=True)
