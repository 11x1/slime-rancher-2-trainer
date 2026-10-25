"""A corral with a Corral Radio playing its own playlist, walls dancing to the beat."""
TITLE = "Corral Radio"
CAPTION = ("A corral upgrade that plays its own playlist. The energy walls ripple and glow to the beat, and "
           "only that corral hears it.")
FEATURE = "slime_dj"
SLOT = 5
SETTINGS = {"slime_dj.enabled": True, "custom_upgrades.enabled": True}
PLACE = "showcase_corral"
PLACE_HINT = "Stand a few metres from the radio corral, the walls and the slimes in view."
TIME = 19.5
HIDE_HUD = True


def setup(t):
    from _common import radio_corral
    radio_corral(t)
