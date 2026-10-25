"""r10cr3 Corral radio playlist.

Checklist steps:
  1. Overlay Ranch > Slime DJ: "Corral radios" lists "Corral NNNN" with its distance, Off.
  2. Add songs, press + on 3 songs: they join the radio's playlist (queue unchanged); Done.
  3. Switch the radio on: its song plays from that corral only.
  4. Play another song on the global queue: other corrals play it; the radio corral keeps its own.
  5. Skip, Shuffle, click a row, move up/down, remove all work. Switch off: vanilla tune there.
Semi: "plays from that corral only" is asked once (the user listens near and away from the corral);
everything else is checked through probes.

Needs harness:
  probe plots                    -> [{id, type, upgrades, pos}]          (wave-1 generic)
  exec  plots.build              {plot, type}       turn an empty plot into a corral (as the plot menu)
  exec  plots.buy_upgrade        {plot, upgrade}    vanilla plot upgrade ("MUSIC_BOX")
  exec  custom_upgrades.buy_plot {plot, id}         custom plot upgrade ("corral_radio")
  probe slime_dj.library         -> {songs: [..]}   songs in the music folder
  probe slime_dj.radios          -> [{plot, name, distance, on, playlist: [..], playing, status}]
                                    what the Slime DJ tab's "Corral radios" card shows
  probe slime_dj.queue           -> {songs: [..], playing}
  exec  slime_dj.radio           {plot, op, ...}    op: add {songs} | on | off | skip | shuffle |
                                    play {index} | move {index, by} | remove {index}
  exec  slime_dj.queue_play      {song}             play a song on the global queue
  probe slime_dj.corral_audio    {plot} -> {source: "radio"|"queue"|"vanilla", song}
"""
import re

CASE = "r10cr3"
TITLE = "Corral radio playlist"
WHERE = "ranch: corral (slot 5)"
NEEDS = ["probe plots", "exec plots.build", "exec plots.buy_upgrade", "exec custom_upgrades.buy_plot",
         "probe slime_dj.library", "probe slime_dj.radios", "probe slime_dj.queue", "exec slime_dj.radio",
         "exec slime_dj.queue_play", "probe slime_dj.corral_audio"]


def corrals(t):
    return [p for p in t.plots() if str(p.get("type", "")).lower() == "corral"]


def ensure_radio_corrals(t, n):
    """At least n corrals; the first owns Music Box + Corral Radio. Returns the corral list."""
    cs = corrals(t)
    if len(cs) < n:
        empty = [p for p in t.plots() if str(p.get("type", "")).lower() in ("empty", "none", "")]
        t.give_newbucks(20000)
        for p in empty[:n - len(cs)]:
            t.exec("plots.build", plot=p["id"], type="corral")
        cs = corrals(t)
    t.check(len(cs) >= n, f"need {n} corrals, have {len(cs)}")
    t.give_newbucks(10000)
    for i, c in enumerate(cs[:2]):  # both play music (Music Box); the first is the radio
        ups = [str(u).lower() for u in c.get("upgrades", [])]
        if "music_box" not in ups:
            t.exec("plots.buy_upgrade", plot=c["id"], upgrade="MUSIC_BOX")
        if i == 0 and "corral_radio" not in ups:
            t.exec("custom_upgrades.buy_plot", plot=c["id"], id="corral_radio")
    return corrals(t)


def radio(t, plot_id):
    r = t.probe("slime_dj.radios")
    for x in (r.get("radios", []) if isinstance(r, dict) else r or []):
        if x.get("plot") == plot_id:
            return x
    return None


def audio(t, plot_id):
    return t.probe("slime_dj.corral_audio", plot=plot_id) or {}


def run(t):
    t.set_setting("custom_upgrades.enabled", True)
    t.set_setting("slime_dj.enabled", True)
    t.in_slot(5)
    t.exec("custom_upgrades.set_on", on=True)
    t.teleport(to="ranch")
    cs = ensure_radio_corrals(t, 2)
    plot, other = cs[0]["id"], cs[1]["id"]
    songs = list((t.probe("slime_dj.library") or {}).get("songs", []))
    if len(songs) < 4:
        from sr2test import NeedsUser
        raise NeedsUser(f"the music folder has {len(songs)} song(s); the case needs 4")

    t.step(1, "Corral radios card: 'Corral NNNN', distance, Off")
    r = t.wait_until(lambda: radio(t, plot), 10, "the radio in the Corral radios card")
    if r.get("on"):
        t.exec("slime_dj.radio", plot=plot, op="off")
        r = radio(t, plot)
    t.check(re.fullmatch(r"Corral \d{4}", str(r.get("name"))), f"radio name {r.get('name')!r}")
    t.check(isinstance(r.get("distance"), (int, float)), f"no distance: {r}")
    t.check(not r.get("on"), "radio is on")
    for i in range(len(r.get("playlist", []))):  # start from an empty playlist
        t.exec("slime_dj.radio", plot=plot, op="remove", index=0)

    t.step(2, "+ on 3 songs: playlist has them, queue unchanged")
    queue_before = (t.probe("slime_dj.queue") or {}).get("songs", [])
    t.exec("slime_dj.radio", plot=plot, op="add", songs=songs[:3])
    r = radio(t, plot)
    t.eq(list(r.get("playlist", [])), songs[:3], "radio playlist")
    t.eq((t.probe("slime_dj.queue") or {}).get("songs", []), queue_before, "global queue")

    t.step(3, "radio on: its song plays from that corral")
    t.exec("slime_dj.radio", plot=plot, op="on")
    a = t.wait_until(lambda: (lambda x: x if x.get("source") == "radio" else None)(audio(t, plot)), 10,
                     "the radio corral playing its radio")
    t.check(a.get("song") in songs[:3], f"radio corral plays {a.get('song')!r}, not from its playlist")
    pos = cs[0].get("pos")
    if pos:
        t.teleport(pos=[pos[0] + 6, pos[1] + 1, pos[2] + 6])
    t.confirm("Do you hear the radio's song at this corral (and only near it when you walk away)?")

    t.step(4, "global queue song: other corrals play it, the radio corral keeps its own")
    t.exec("slime_dj.queue_play", song=songs[3])
    t.wait_until(lambda: audio(t, other).get("song") == songs[3], 10, "the other corral playing the queue song")
    a = audio(t, plot)
    t.check(a.get("source") == "radio" and a.get("song") in songs[:3], f"radio corral now plays {a}")

    t.step(5, "skip, shuffle, click a row, move, remove; off -> vanilla")
    before = radio(t, plot).get("playing")
    t.exec("slime_dj.radio", plot=plot, op="skip")
    t.wait_until(lambda: radio(t, plot).get("playing") != before, 10, "a different song after Skip")
    t.exec("slime_dj.radio", plot=plot, op="shuffle")
    t.check(radio(t, plot).get("shuffle"), "shuffle not on")
    t.exec("slime_dj.radio", plot=plot, op="shuffle")
    t.exec("slime_dj.radio", plot=plot, op="play", index=2)
    t.wait_until(lambda: radio(t, plot).get("playing") == songs[2], 10, "row 3 playing after the click")
    t.exec("slime_dj.radio", plot=plot, op="move", index=0, by=1)
    t.eq(radio(t, plot).get("playlist", [])[:2], [songs[1], songs[0]], "playlist after moving row 1 down")
    t.exec("slime_dj.radio", plot=plot, op="move", index=1, by=-1)
    t.eq(radio(t, plot).get("playlist", [])[:2], songs[:2], "playlist after moving it back up")
    t.exec("slime_dj.radio", plot=plot, op="remove", index=0)
    t.eq(radio(t, plot).get("playlist", []), songs[1:3], "playlist after removing row 1")
    t.exec("slime_dj.radio", plot=plot, op="off")
    # Off: the corral leaves the radio channel. With the global queue still playing (step 4) it joins
    # the global song; with nothing queued it is the vanilla tune.
    a = t.wait_until(lambda: (lambda x: x if x.get("source") != "radio" else None)(audio(t, plot)), 10,
                     "the radio corral off its radio")
    q = (t.probe("slime_dj.queue") or {}).get("playing")
    t.check(a.get("source") == "vanilla" or (a.get("source") == "queue" and a.get("song") == q),
            f"radio corral after Off plays {a} (global queue: {q!r})")
