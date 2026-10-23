# Slime DJ (branch feature/slime-dj)

New F8 tab "Slime DJ": custom songs played from the corral music box (slime speakers).
- Songs come from a new folder in the game directory for mods (e.g. `<game>\sr2mods_music\`). Decode with compiled-in C libs only (stb_vorbis / dr_mp3 / dr_wav), no managed code.
- Play through the game's audio (AudioClip.Create + SetData on the music box's AudioSource, positional like the vanilla music box).
- Song list; queue songs; shuffle (random) by default when the queue is empty. The queue shows in the right column.
