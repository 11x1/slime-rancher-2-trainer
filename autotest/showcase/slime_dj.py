"""The Slime DJ tab: the song library, the queue and the Corral radios card."""
TITLE = "Slime DJ"
CAPTION = ("Your own songs from the corral Music Box: drop .ogg, .mp3 or .wav files in a folder, build a queue, "
           "and give corrals radios with playlists of their own.")
FEATURE = "slime_dj"
SLOT = 5
SETTINGS = {"slime_dj.enabled": True, "custom_upgrades.enabled": True}
MENU = {"cat": "ranch", "tab": "Slime DJ"}


def setup(t):
    # A radio playing, so the Corral radios card has a row (skipped as "needs harness" until the
    # slime_dj probes exist; then the tab shows without a radio).
    from _common import radio_corral
    try:
        radio_corral(t)
    except Exception as e:  # noqa: BLE001 - the tab is worth a shot without a radio too
        t.note(f"no radio for the shot: {e}")
