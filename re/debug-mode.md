# Developer debug tooling left in SR2 1.3.0

What Monomi Park's in-game debug tooling was, what survived the 1.3.0 Steam release build (Unity 6000.0.73f1, IL2CPP), and what a native mod can switch back on. Sources: the IL2CPP dumps (`re/dump/il2cppinspectorredux/types.cs`, Cpp2IL diffable), IDA on `GameAssembly.dll` (offline copy, `re/ida/GameAssembly.i64`), and the Addressables catalog and bundles in `SlimeRancher2_Data/StreamingAssets/aa` (read with UnityPy from the scratchpad, never written). RVAs are 1.3.0 image offsets (VA - 0x180000000).

## How debug mode worked in development

There is no runtime "debug mode" flag. Debug features are compiled in or out with the `DEVELOPMENT_BUILD` / `UNITY_EDITOR` defines:

- `SRAssert.Development*` and `SRLogger.DevelopmentLog*` carry `[Conditional("DEVELOPMENT_BUILD")]`; their bodies are empty stubs (`0x4ED3F0`, a bare `ret`).
- `SceneLoader` has two Addressables scene references, `_gameDebugScene` ("GameDebugDirector") and `_sceneDebugScene` ("SceneDebugDirector"), with the tooltip "will only be loaded in debug builds". The release `EnqueueLoadSceneGroup` has no step that loads them, so they are never loaded.
- `RequiresDebugMode.Awake` (0x98E9A0) is `Destroyer.Destroy(gameObject, "RequiresDebugMode.Awake")` with no condition: anything tagged with it deletes itself in release.
- `GameDebugDirectorHelper.Awake` (0xA498A0) is `Object.Destroy(this)` with no condition.
- `PlayerDebugHudUI.Awake` (0x5D7EE0) shares a body that is just `gameObject.SetActive(false)`; its update code is gone.

The debug menu was opened with an input action (`DebugDirector._debugMenu`, `_repeatPrevious`), navigated with the "Debug Menu Horizontal/Vertical" and "Debug Tab Next/Previous" input events (assets still in the catalog), and laid out as pages of buttons (`DebugUI`: grid, button prefab, buttons per page). Commands were addressed by their button path, e.g. `progress/unlock map`.

## What exists, ranked by how enableable it is

| # | Piece | What it shows / does | Code in 1.3.0 | Assets in 1.3.0 | Enableable? |
|---|---|---|---|---|---|
| 1 | `UI.Debug.FontsUsedDebugUI` | IMGUI overlay: over every visible TMP text, a dark box listing the TMP font assets its characters were drawn with (fallbacks included), brighter near the mouse | complete (`StartDebugUI` 0x83C210, `OnEnable`, `OnGUI`, `TrackedText`); nothing calls it | none needed (creates its own GameObject) | **yes, done**: `debug_tools` > Font debug overlay |
| 2 | `GenericDebugTextHUD<T>` / `GenericDebugTextHUDController<T>` world-space debug labels (TreasurePod, RanchDrone, GhostDrone, GrowableFood, ResourceGrower, SanctuaryRoom, SprinkleCanister, Caretaker, RegionInfo, DisruptionArea, PrismaActorDisruption, ActorSDF) | a billboard label floating over each object within `Radius` of the player, with text from the object's `IDebugTextProvider<T>.GetDebugHUDText()` and a colour from `IDebugColorProvider` | generic code complete (shared `__Canon` bodies: controller `Start` 0x2135BB0, `Update` 0x2135CA0, `OnDisable` 0x2135970; display `Setup` 0x2136890, `UpdateTransform` 0x2136CD0, ...); every controller's `GetAllPossibleTargets` present. Some text providers survive, some were reduced to `""` (table below) | controller and display prefabs ship, but only inside the SceneDebugDirector scene bundle | **possible, not small**: see "Reviving the debug HUD labels" |
| 3 | `PathDebugDisplay` | draws lists of points as LineRenderers, can animate them | complete (`AddPaths`, `DisplayPath`, `AnimateAllPaths`, ...) | needs a `pathObjectPrefab`; no caller found | only as a building block (we would have to supply the paths) |
| 4 | `VFXDebugToggles`, `VFXDeformationToggle` | toggles subsurface scattering globals and deformation meshes | complete | used by the VFX setup | trivial, but not a debug viewer (shader globals) |
| 5 | `DebugSpawnGadgetsInGrid` | spawns every gadget of a group in a grid at its position (`Start` -> `SpawnGadgetsInGrid`) | complete | test-scene component only | not useful in play (would place real gadgets in the save) |
| 6 | `RadiantBagVisualiserCore` / `Display` | text panel with the radiant-slime spawn "bag" (shuffle-bag RNG) state | `StartTool`, `StopTool`, `UpdateBagData` present, but no code calls `UpdateBagData` any more | display prefab only in the SceneDebugDirector bundle | no: the data feed is stripped |
| 7 | `SceneDebugDirectorHelper` (the debug command menu: unlock all, add currency/gadgets, time forward/back, toggle switches, force save, teleport/tours, spawn tornado/cyclone, open Slime Studio, collision debug, ...) | the big developer menu | **stripped**: only the constructor and one property getter remain; no `Awake`/`Update`/command code | the scene ships (one GameObject: `SceneDebugDirectorHelper` + `OptionsHandler`; config, composite commands, tours, location sets, all HUD prefabs) | no: logic is gone |
| 8 | `DebugDirector`, `DebugUI` (menu window), `DebugTeleportDestination` | the menu host and its paged button UI | **stripped** (constructors only) | `DebugDirector`/`DebugUI` objects were not found in the two debug scenes; the input events exist | no |
| 9 | `PlayerDebugHudUI` | player velocity, horizontal velocity, slope, active abilities, location, look input, cell | `Awake` = `SetActive(false)`, `OnDestroy` empty, no update | - | no (we already have our own speed panel) |
| 10 | `SlimeDebugDisplayTool`, `SlimeDebugDisplay`, `SlimeDebugBehaviorDisplay`, `ToolsCommon` | per-slime floating panel: emotions, animator state, current behaviour and its fields, radiant state, within a radius | **stripped** (constructors and the `InitializeForPlayInEditor` static resets only) | prefab not in the debug bundles | no |
| 11 | `DebugFlyAbilityBehavior` / `DebugFlyAbilityData` | noclip-style fly (Fly Up/Down, sprint multiplier); input events `DebugFlyToggle`, `DebugFlyUp`, `DebugFlyDown` | `CanStart` returns `false` (shared stub 0x510250), update stripped | ability data asset exists | no (hl2_movement / sandbox already cover flying) |
| 12 | `MusicDebugOverlay`, `DebugGamepadFeedbackDisplay`, `GameDebugDirectorConfiguration` (screenshot input, music list) | music state overlay; gamepad rumble visualiser | constructors only | GameDebugDirector bundle ships (helper + gamepad display, 7 music definitions) | no |
| 13 | `FirstPersonScreenEffectsDebugTest` | key-driven test of damage/bite/snow screen splashes | `Update` present (0xA578D0) | test component, not in gameplay scenes | not worth it |
| 14 | `ZoneDebugLoadLocation`, `SceneGroup._isEditor`, `MainMenuDebugDirectorHelper` (Slime Appearance preview scene group), `MinimalSceneDebugDirectorHelper`, `MapDebug` (reveal fog / discover icons), `KCCTestingTool` | editor/test helpers | data only (no methods) | assets in catalog | no |

Unity's own HDRP Rendering Debugger types (`DebugManager`, `DebugUI.*`, `DebugDisplaySettings*`) are also in the build. Its runtime UI only starts in development builds (`Debug.isDebugBuild`); not investigated further.

### Text providers for the HUD labels

| Provider (target) | Display | 1.3.0 `GetDebugHUDText` |
|---|---|---|
| `SprinkleCanister` | SprinkleCanister | `rate={0:F2}/min`, `combos=`, `boosts=` (intact) |
| `SanctuaryRoomMetadata` | SanctuaryRoom | `SanctuaryRoomContents.GetDebugDisplayString(combo, boost)` (intact) |
| `CaretakerSpawner` | Caretaker | `pose={0}\nexpires_in_hrs={1:F1}` (intact) |
| `DisruptableRegionMember` | PrismaActorDisruption | `Disruption Level: <enum>` (intact) |
| `DisruptableResource`, `UnstableResource` | PrismaActorDisruption | `Unstable Progress: {0:0.00}`, `BurstProgress: {0:0.00}` (intact) |
| `ComponentAcquisitionDroneController` | GhostDrone | `# AWARDED`, `# IS MOVING` ... (intact) |
| `ExplorerDroneDebugTextProvider`, `RanchDroneDebugTextProvider` | RanchDrone | `# Resources`, `# Resource Types` ...; ranch drone 0x1E0 bytes (present) |
| `ActorSDFDebugDisplay.GetUpdateText` | ActorSDF | `Global:`, `Rain:`, `Wind:`, `Pollen:` insideness (intact) |
| `DisruptionRegion` | DisruptionArea | `""` (stub) |
| `Region` | RegionInfo | joins an empty list (stub) |
| `SpawnResource` | ResourceGrower | `""` (stub) |
| `ResourceCycle` | GrowableFood | `""` (stub) |
| (TreasurePod has no provider; the controller filters ALL/OPENED/UNOPENED pods) | TreasurePod | marker only |

## What ships in the debug scene bundles

`debugscenes_..._gamedebugdirector.unity_*.bundle` (21 KB): `GameDebugDirectorHelper` (destroys itself), `DebugGamepadFeedbackDisplay`, `GameDebugDirectorConfiguration`, 7 `MusicDefinition`s.

`debugscenes_..._scenedebugdirector.unity_*.bundle` (3.1 MB). The scene itself has one GameObject, `SceneDebugDirectorHelper` (with an `OptionsHandler`). Everything else is prefab/asset data it references:

- HUD prefabs: `<X>DebugDisplayController` + `<X>DebugDisplay` for TreasurePod, RanchDrone, GhostDrone, GrowableFood, ResourceGrower, SanctuaryRoom, SprinkleCanister, Caretaker, RegionInfo, DisruptionArea, PrismaActorDisruption, ActorSDF; each display is a small world-space Canvas with a `TextMeshProUGUI` on a `RawImage` background.
- `RadiantBagVisualiserDisplay` (3-column text panel), `PerfCaptureToolPrefab` (`CaptureToolClose`, 63 `CaptureBookmarksSet`s for performance captures), `TourGuide`.
- `SceneDebugDirectorConfiguration`, `TestCommStationConfig`, a "debug only" comm station with the Caretaker and a "BrokenRobot" conversation, `AllCharacterCallouts` (Gigi boss callouts), debug popup configs, `OutroAndCreditsSequence`, 16 music lists (`AllMusicPlayerMusicDefinitions`, per zone, `RadioMusicDefinitions`, `SpecialMusicDefinitions`, `WeatherMusicDefinitions`).
- `DebugLocations` (`EditorLocationData`) and 5 `LocationSet`s (Fields, Strand, Valley, Bluffs, Conservatory) of named teleport bookmarks (e.g. `Fields01` at ConservatoryFields 324.9, 14.8, 352.1).
- Tour `Treasure pods`: a list of treasure pod destinations with names of their contents and positions (`SceneGroup.ConservatoryFields|317.35,-1.09,348.00|0,234.58,0` "Tank Guard Component", ...).
- Composite commands (the "Custom" page of the menu), which show the button paths the menu used:
  - "setup for boss fight": `progress/unlock map`, `progress/unlock upgrades`, `progress/activate all world switches`, `inventory/science/add gadgets & unlock variants`, `ui/comm station conversations/gigi/GigiCore_Intro/Apply State Changes Without Conversation`, same for `GigiCore_StartFight`.
  - "open shop with all items": `progress/shop - all items available`, `ui/standalone uis/shop`.
  - "unlock music player tracks": `progress/unlock all`, `inventory/science/add gadgets & unlock variants`, `progress/complete outro`, `progress/tutorials - complete all`.

The catalog also names debug input events (`DebugFlyToggle/Up/Down`, `DebugCopyLocation`, `DebugPasteLocation`, `DebugLogCharacter`, `DebugSkip`, the menu navigation events), `DebugAmmoGroup`/`DebugSpawnableGroup`, test assets (`Test Tutorial`, `Test death tutorial`, `TestLabyrinth Area 1..10`, `Cube_Switch_DeleteMeAfterTest`, `KCCTestingTool`) and test zone scene names (`zoneLabyrinthTest`, `zoneLabyrinthDisruptionTest`, `zone*LayerTest`); no bundle for those test scenes is shipped.

## Reviving the debug HUD labels (not done)

All the code needed for the floating labels runs in release, only the prefabs are locked in a scene bundle (assets cannot be loaded one by one from a scene bundle). A mod would have to:

1. load `SceneDebugDirector` additively (Addressables `LoadSceneAsync(key, Additive)` with the key/`AssetReference` from `SceneLoader._sceneDebugScene`), on the main thread, after a gameplay scene group is up;
2. read the controller prefab fields of the loaded `SceneDebugDirectorHelper` (`_treasurePodDebugControllerPrefab` 0x270, `_sprinkleCanisterDebugControllerPrefab` 0x260, `_caretakerDebugControllerPrefab` 0x268, `_sanctuaryRoomDebugControllerPrefab` 0x258, `_ranchDroneDebugControllerPrefab` 0x280, `_ghostDroneDebugControllerPrefab` 0x288, `_prismaActorDisruptionDebugControllerPrefab` 0x248, `_actorSDFPositionDebugControllerPrefab` 0x240, ...) and `Instantiate` the ones wanted. The controller's `Start` takes the player from `SceneContext.Instance` as look target; its `Update` (every `UpdateFrameInterval` frames) instantiates one display under each target in `Radius`;
3. destroy the controllers and unload the scene when switched off and before every scene-group change (the displays are parented to game objects that get saved/unloaded).

Risks: the extra scene brings a second `OptionsHandler` (Awake/Setup only collects the option models into a list) and an `SRSingleton<SceneDebugDirectorHelper>` with no logic; several providers return `""`, so some labels would be empty boxes. Worth it only for a few useful labels (sprinkle canister rate/combos, sanctuary room contents, caretaker pose/expiry, drone state, labyrinth disruption progress). An ImGui re-host is the safer alternative: poll the targets every few seconds (the controllers' own static lists, e.g. `SprinkleCanisterDebugDisplayController.AllSprinkleCanisters`, `CaretakerDebugDisplayController.AllCaretakerSpawners`; not checked whether release code still fills them), call the intact `GetDebugHUDText()` and draw the strings at the projected positions in our overlay.

## Easter eggs and secrets

Nothing like a Konami code, cheat console or hidden command was found: input handling for debug is all behind the stripped debug director, and there is no text-entry console class. What exists:

- **Secret styles** (`SlimeAppearanceDirector.MaybeDiscoverSecretStyle`, `SecretStyleDiscoveriesV01` in the save): the shipped "extremely rare subspecies" cosmetic feature from the Secret Style Pack, not a hidden easter egg.
- **Gigi's secret conversation** (`GigiSecret`, `GigiSecret_Blocks/_Creative/_Goodbye/_Pretend`, `GigiSecretOptions*`, `Gigi Hidden Settings`, `LabSecretgate_A`, `LabSecretgate_Geyser`, `LabyrinthSecretEntryDoor_Switch`): most likely the hidden Labyrinth spot behind the secret gate (inferred from the asset names), where Gigi says "It seems you're awfully good at finding secrets. Took me forever to find this spot." with lines such as "Speaking of secrets, why did you pretend to be an AI?" (the string table has no speaker; it reads like a dialogue option). A real in-game secret, not unused content.
- Leftover text: the Early Access boot notice ("Slime Rancher 2 is currently in a pre-release state ..."), the "BrokenRobot" debug-only Caretaker conversation, `GigiEndTemp` ("Gigi/EndTemp"), `GigiTestCall*` conversations, `allzones ea placeholder.png`.

### Ranked list (second pass)

Sources: IDA on `GameAssembly.dll`, the shop UI scene bundle (`uiscenes_..._menulayer_shop.unity`), `scriptableobjectlookupdata_assets_all`, `sfx_assets_`, `music_assets_assets_audio_music`, `gamecore_assets_` and the English string tables (all read with UnityPy).

**1. Midnight shop song (`musicShopSecret`). Confirmed.** This is a ProntoMart shop UI track, not a music player track. The chain:

- `ShopUIBase._shopMusicList` (+0xF8) in the shop scene points to the `ShopMusicListDefinition` asset (scriptableobjectlookupdata).
- The `ShopMusicSecret` entry in that list is a `MusicDefinition` (priority 900 = HOUSE, loops) whose `_cue` is the SECTR cue `musicShopSecret` (sfx bundle, PathID 6823199633913212233). That cue plays `musicShopSecret.ogg` (186.4 s, the same length as `musicShopNorm`).
- `ShopUIBase.Update` waits until the music start time (`_musicDelaySeconds` = 0), then calls `ShopMusicListDefinition.PickSong(ShopUIBase.GetWorldTime(), eventDirector)`.
- `ShopAudioListDefinitionBase<T>.PickAudio` takes `(int)worldTime % 86400` (game time of day in seconds) and keeps the entries whose `Condition` is true and whose `ScheduledTimes` slot contains that time (start inclusive, end exclusive). It then picks one at random by `OccurrenceWeight`. Only when no scheduled entry matches does it fall back to unscheduled entries.

The song is picked once per shop opening. The list is:

| entry (cue) | game time slots | weight |
|---|---|---|
| ShopMusicNorm | 06:00-18:00 | 0.6 |
| ShopMusicHalf | 06:00-10:00 | 0.4 |
| ShopMusicFast | 12:00-18:00 | 0.4 |
| ShopMusicSlow | 18:00-24:00, 00:05-02:00, 04:00-06:00 | 1.0 |
| ShopMusicRev | 02:00-03:59 (ends at 14340 s, not 14400) | 1.0 |
| **ShopMusicSecret** | **00:00-00:05** (0-299 s) | 1.0 |
| **ShopMusicChip** | 00:05-24:00 | **0.025** |

All conditions are empty.

- **How to trigger the secret song:** open the ProntoMart shop UI (home page, Coo-Coo Corner / Daily Deals or Polestar Provisions) while the in-game clock reads 00:00 to 00:04. It is the only entry scheduled then, so the chance is 100%. With the usual 24-minute day (`TimeDirector.SecsPerGameDay`, value not read), that window is about 5 real seconds.
- The Night Market style (`ShopNightMarketStyle.OverrideMusicTracks` = `ShopNightMarketMusicListDefinition`, one unscheduled `ShopMusicNightMarket` entry) overrides the shop music on that page. The Caretaker shop has its own empty list.

**2. Rare chiptune shop song (`musicShopChip`). Confirmed.** It is weighted 0.025 against the normal songs, so it is a rare variant:

- about 2.4% per opening at most times (0.025 / 1.025);
- 4% from 10:00 to 12:00 (only Norm 0.6 + Chip are active);
- **100% from 03:59:00 to 03:59:59**, because `ShopMusicRev` ends at 14340 s while `ShopMusicSlow` starts at 14400 s. That gap looks like a deliberate one-minute window.

`ShopMusicRev` (02:00-04:00, 211 s) is probably a reversed version of the theme for the small hours (the name suggests it; the audio was not checked). It is not rare.

**3. "You Can Pet the Cat" (achievement `YouCanPetIt` = 26, "Pet the ProntoMart cat"). Confirmed.** The shop NPC (`ShopNpcBehaviour`) is a pixel cat on the shop UI. Clicking it (`TryClickOn` → `CanInteractWith` → `InteractWith`) does the following:

- plays `_clickedCue` and sets the animator trigger `CLICKED`;
- fires `_onCatInteracted`;
- calls `MetaGameDirector.Grant(YouCanPetIt)`.

Petting is refused while the cat sleeps (`IS_SLEEPING`), while `BLOCK_INTERACT` is set, during the `_secondsBetweenClicks` cooldown, or once `_annoyance` ≥ 1. A refused click, and the cat being bounced back and forth (direction reversals in `Update`), add `_bounceAnnoyanceGainRate` to an annoyance value (0-3, animator float `ANNOYANCE_LEVEL`). Spam-clicking therefore makes the cat visibly annoyed. Separately, `_onCatFlipped(bool)` is a flip event.

The cat's colour palette is chosen once per game session by `ShopDirector.GetOrCalcNpcColorPaletteIndex`: (ms + 1000*(s + 60*min) of `DateTime.Now`, plus `abs(deviceUniqueIdentifier.GetHashCode())`) mod the palette count. The cat is a different colour each launch. This is random, not tied to the date.

**4. "Secret Behind the Waterfall" (achievement 14, "Discover Powderfall Bluffs' entrance"). Confirmed; a hidden-area achievement, not a joke.** `MetaGameDirector.CheckForBehindTheWaterfall` grants it once all three puzzle slots in `AchievementGameplayData._puzzleSlotIDsForBehindTheWaterfall` are filled: `puz0260113737`, `puz1565498717`, `puz0406679521`. These are the plort locks of the Powderfall Bluffs gate (the location in Rainbow Fields is inferred from the name, not traced). `AchievementGameplayData` also links query achievement 14. Trigger: fill the three plort slots at the gate behind the waterfall.

**5. Unused achievements (cut content, not reachable).** The string table still names `FriendFromTheFuture` ("Your Future Friend [UNUSED]": interact with every GG Research Drone and every Gigi projector), `Automation` ("Droning On"), `ShadowSureShot`, `SlimeChef`, `TrashPanda` (feed 20 items to a full Ringtail), and `TarrTarrRange` (disperse a tarr with water from more than 80 m). None of them is in the 1.3.0 `Achievement` enum (0-26), so they cannot be earned.

**6. Twinkle slime / chimes (Pedia text only; SR1 holdover).** `Pedia_en` has `echo_note_gordo_slime` ("Twinkle Slime ... related to the passing of time, or a change in seasons") and `echo_notes` ("Chimes"). No code (`EchoNote*` / `Twinkle*` classes) and no addressable asset exists in 1.3.0, so nothing spawns it.

**Searched and not found (negative results):**

- **Real-date events.** In game code, only the shop uses `DateTime.Now` month/day (`ShopDayOfYear`, `ShopDate`, `ShopRealDateYearlyScheduleDefinition`, `ShopRealDateScheduleDefinition`). The only schedule asset in the catalog is `NightMarket_DailySchedule`, with no yearly or holiday schedule. The other `DateTime.Now` callers are analytics, saves, screenshots and the cat palette. There is no April Fools, holiday or birthday code.
- **Name-based triggers.** Every Assembly-CSharp `String.Equals` / `op_Equality` / `Contains` / `StartsWith` call next to a string literal was checked. They only compare internal tokens ("Fireball", "Idle", "FlapWings", ...), with no save or rancher name comparisons.
- **Konami or cheat codes.** None (see above).
- **"Spooky" items.** `EndcapSpooky*` cues (ghosties that hide and unhide) and the "tall ashwood ... perfect for a spooky holiday" decoration are ordinary sanctuary or shop cosmetics, not date-gated.
- **Developer cameos.** Nothing hidden: the names are only in the About/credits text (`m.about_note`).
- The Options help for "Event Music" says the music may change for tarr, weather "or other, secret things!" (`MusicPriority` includes `SLIME_HOOP` = 101). The shop songs use priority 900, so they are probably not affected by that toggle (not verified).
