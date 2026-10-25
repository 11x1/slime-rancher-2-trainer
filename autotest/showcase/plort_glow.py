"""Loose plorts near a playing speaker glowing to the music, at night."""
TITLE = "Plorts glow to the music"
CAPTION = "Plorts near a playing corral brighten with every beat. Best at night."
FEATURE = "slime_dj"
SLOT = 5
SETTINGS = {"slime_dj.enabled": True, "custom_upgrades.enabled": True}
PLACE = "showcase_plorts"
PLACE_HINT = "Stand next to the radio corral, looking down at the ground in front of you."
TIME = 22
HIDE_HUD = True


def setup(t):
    from _common import radio_corral
    radio_corral(t)
    t.set_setting("sandbox.enabled", True)  # spawn goes through the Sandbox
    t.spawn("Pink Plort", 12)
    t.spawn("Phosphor Plort", 8)
