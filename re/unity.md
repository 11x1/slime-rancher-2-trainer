# Unity / IL2CPP engine facts

Game: SR2 **1.3.0**, Steam buildid **24131940**, Unity 6000.0.73f1, IL2CPP metadata v31, D3D11.
GameAssembly.dll SHA-256 `786006ede0d198f6e15a2c01f90ef9108afe694745fa0ad894e1c3ed342385b7` (86,650,368 bytes); hashes of all inputs in `re/input/HASHES.txt`.
Image base 0x180000000: IDA VA = 0x180000000 + RVA. IDA database `re/ida/GameAssembly.i64` (IDA 9.4, Il2CppInspectorRedux `il2cpp_ida.py` applied: names, types, string literals).

## Resolving at runtime

- The mod never uses RVAs: `il2cpp_domain_assembly_open("Assembly-CSharp")` -> `il2cpp_class_from_name(image, ns, name)` -> `il2cpp_class_get_method_from_name(klass, name, argc)` / `il2cpp_class_get_field_from_name` + `il2cpp_field_get_offset`. Wrapped by `core/resolver.h` (`Resolver::Class/Method/MethodByParams/Field/FieldOffset/MethodPointer/Invoke`).
- Nested classes: `il2cpp_class_get_nested_types` (e.g. `PlortCollector.JointReference`, private).
- Overloads with the same argc: `Resolver::MethodByParams(klass, name, {"Full.Type.Name", ...})` (e.g. `PediaDirector.IsUnlocked(PediaEntry)` vs `(IdentifiableType)`).
- Explicit interface implementations are named `Ns.IInterface.Method` in metadata (e.g. `GadgetFabricatableItem` -> `MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate`). `re/re_lookup.py` prints the full name.
- Virtual methods: `il2cpp_runtime_invoke` with a base-class `MethodInfo` does NOT dispatch; look the method up on the object's own class (e.g. `Identifiable.get_AmmoMetadata`, `PediaEntry.get_Icon`).
- Generic methods (e.g. `OptionsItemDefinition.CreateInstance<T>` 0xF73740, shared `<object>` instantiation): call through an inflated `MethodInfo`, or reproduce the body.
- Shared generic code (`ListUIRoot`4`, `AbilityBehavior<T>`, `ScriptedValue<T>`, `GenericDebugTextHUD<T>`): one body for all `__Canon` instantiations. Hooking it hits every instantiation: filter by class in the detour (custom_upgrades `RebuildUI`, sell_upgrades alt-action hooks) or don't hook.
- Inspector `metadata.json` caveat: its `exports`/`apis` addresses are **0xC00 too high** (gives `il2cpp_gchandle_free` 0x432AA0; real export 0x431EA0). `methodDefinitions` addresses are correct and match `.pdata`.

## Folded bodies: never hook these

IL2CPP/ICF folds identical bodies; `re/re_lookup.py --rva <hex>` lists every method at an address (`[shared xN]` in its output).

| RVA | Body | Examples |
|---|---|---|
| 0x4ED3F0 | empty `ret` (2000+ methods) | `OptionsItemDefinition.Awake`, `AbilityBehavior<T>.Update`, `SRAssert.Development*`, `SRLogger.DevelopmentLog*` |
| 0x4E3350 | `return true` | `OptionsItemDefinition.get_Cacheable/ShouldEnable`, `JetpackAbilityBehavior.get_UsesEnergy` |
| 0x4DD840 | `return *(this+0x10)` | `InputActionMapReference.get_Map`, `MapDirector.Marker.get_Source` |
| 0x4E3AE0 | `return *(this+0x18)` | `ResourceNodeDirectorConfig.get_ResourceNodeWeightMap`, iterator `get_Current` |
| 0x4DCFB0 / 0x516AE0 / 0x531320 / 0x52D890 / 0x4F9D90 | `return *(this+0x20/0x28/0x30/0x38/0x40)` | `OptionsItemDefinition.get_Label`, `PlayerZoneTracker.GetLatestZone` (0x4F9D90), `ResourceNodeDirector.get_NodeSpawners` (0x52D890) |
| 0x5732F0, 0x4EE590, 0x4EBExx, 0x539Cxx | int/float field getters | `ResourceNodeDirectorConfig.get_MaxActiveNodes`, all `CharacterControllerParameters` getters |
| 0x4DCFC0, 0x4E3290 | empty ctor / ScriptableObject base ctor | every `<>c` ctor, `AbilityData()` |
| 0x510250 | `return false` | `DebugFlyAbilityBehavior.CanStart` |
| 0x1EC9xxx-0x1ECAxxx, 0x1715xxx, 0x171Bxxx | generic `AbilityBehavior<T>`, `SRAbilityBehavior<T>`, `ScriptedValue<T>` | call only |

Patch data instead of hooking a shared getter. Small getters may also be inlined into callers (e.g. `GadgetFabricateCategory.GetItems` is inlined into the runtime category ctor: re/fabricator.md).

## GC handles

- `Il2CppGCHandle` is **`void*`** in Unity 6 / metadata v31 (64-bit). Declaring it `uint32_t` truncates and `il2cpp_gchandle_free` / `get_target` then AV inside `il2cpp::gc::GCHandle::Free` (0x44AEE0; export 0x431EA0 is `jmp 0x44AEE0`; it does `cmp byte [handle & ~0x1FFF + 0x20], 4`). Crash report: `re/crash-2026-10-22-1855.md`.
- Use `core/unity.h` (`NewGCHandle` / `GCHandleTarget` / `FreeGCHandle`) only; never free a handle inside a hook (store it, free on the main thread tick).

## Icalls (stripped managed wrappers)

- `Texture.GetNativeTexturePtr()` is stripped from the managed code; the engine still registers `UnityEngine.Texture::GetNativeTexturePtr_Injected`. Unity 6 convention (from `Texture.get_height` 0x34126C0): load `m_CachedPtr` (+0x10 of the UnityEngine.Object), NRE if null, resolve `"UnityEngine.Texture::<Name>_Injected"` via `il2cpp_resolve_icall`, call `fn(nativeSelf)` (no MethodInfo). On D3D11 it returns `ID3D11Resource*`. Used by `ui/slime_icons.cpp`, `ui/item_icons.cpp`.

## Version guard (core/version_guard.cpp)

| Method | RVA | Note |
|---|---|---|
| `UnityEngine.Application.get_version()` static | 0x339D370 | "1.3.0" (bundle version in globalgamemanagers); call via `il2cpp_resolve_icall("UnityEngine.Application::get_version")` or the MethodInfo |
| `Application.get_buildGUID()` | 0x339C700 | per-build GUID |
| `Application.get_unityVersion()` | 0x339D250 | "6000.0.73f1" |
| `SceneSavedGameInfoProvider.GetVersion()` | 0x991B60 | version written into saves (`GameSummaryV03.GameVersion` +0x10) |
| `LocalizedVersionText.OnEnable` (MP.SR.UI.MainMenu) | 0x726E80 | main-menu version label (ui/modded_label.cpp) |

## ScriptedValue (MonomiPark.ScriptedValue)

`ScriptedValue<T>` / `ScriptedBool` / `ScriptedFloat` / `ScriptedInt` ScriptableObjects hold most option values; systems subscribe to them.
- fields: `OnValueChanged` +0x18, `DefaultValue` +0x20, `_value` (bool +0x21 / float +0x24), `_valueInitialized` (bool +0x22 / float +0x28).
- `set_Value` bool 0x171B6B0 / float 0x171B690, `get_Value` bool 0x171B340 / float 0x171B2E0: **shared generic, never hook**. Read `_value` or subscribe `add_OnValueChanged`.

## Misc engine facts

- `UnityEngine.Object.Instantiate(Object, Transform, bool)` 0x343E190; generic `Instantiate<Object>` 0xF71640. `Object.Instantiate(childOfPrefab, parent)` copies just that sub-hierarchy (used for all world clones: re/plots.md).
- `il2cpp_runtime_invoke` path: export 0x470950 -> `jmp 0x41C170`; invoker frame 0x41C6D0.
- `LocalizedReference.get_IsEmpty()` 0x2DAF4A0; `LocalizedString.GetLocalizedString()` 0x2DB1FC0.
- Unity's HDRP Rendering Debugger types ship, but its runtime UI only starts in development builds.

## From the custom_upgrades notes

### Materials / shaders
- `Material.GetPropertyNames(MaterialPropertyType.Vector)`; `Shader.GetPropertyCount / GetPropertyType / GetPropertyNameId`, `Material.GetColor / SetColor`; `Renderer.get_materials` returns instanced copies
- precedence: animated material properties (Animator curves) > MaterialPropertyBlock > material values; StaticColorizer / `ColorPalette.SetColors(Renderer)` write property blocks
- `RanchTech01` palette shader (corral tech, button cap): 24 slots `_Color00` .. `_Color72` picked per vertex via a UV2 mask; `ApplyPalette.Start` / `ColorPaletteApplicator.SetupForTarget` maps shared materials only (keeps unknown ones), Apply only recolours mapped materials
- `HDRP/Lit`: `Shader.Find`, else FindObjectsOfTypeAll(Shader) by name; `_BaseColor`, metallic, smoothness
- game textures are not readable: recolour a sprite with Graphics.Blit into an sRGB RenderTexture + ReadPixels of its textureRect + Sprite.Create (same pivot / ppu)
- mod: silo_world.cpp (gold button), quantum_link.cpp / gadget_look.cpp (TintTree, TintedSprite)

### Physics / layers
- layer 22 = CaveTrigger boxes; 26 = overlay layer of the gadget placeholder; 27 = gadget / decoration colliders (desk, bench, boombox); 29 = corral / coop barriers and Air Net
- Actor + ActorIgnorePlayer layers = mask 0x8800
- `RigidbodyConstraints` 16 | 64 = FreezeRotationX | FreezeRotationZ
- `Object.DestroyImmediate` so a new component can be added in the same tick

### IL2CPP
- private nested classes: `il2cpp_class_get_nested_types`; runtime_invoke does not do virtual dispatch (resolve the method on the object's own class)

### Input
- `Keyboard.current` / `Gamepad.current` `ButtonControl.wasPressedThisFrame`; `Mouse.scroll.y`

## From the minimap, map_tools, quick_travel, wall_markers, collectibles notes

### IL2CPP runtime
- gotchas: `il2cpp_gchandle_*` handles are `void*` in this build, not `uint32_t` (truncated handles caused the 2026-10-22 faults/crash; re/crash-2026-10-22-1855.md); never free/create GC handles inside hooks
- gotchas: `il2cpp_class_array_element_size` takes the ELEMENT class (like mono_class_array_element_size); passing the array class returns 8. Struct elements: require `array_element_size == value_size`
- gotchas: Dictionary layout (corefx 2.x): `_entries`, `_count`, free slot hashCode -1; Entry<string, ref> = 24 bytes (hashCode +0, next +4, key +8, value +16)
- gotchas: inner offset of a struct field = `field_offset - (instance_size - value_size)` (field offsets include the object header)
- gotchas: managed arrays: element data after the 4-pointer array header (kIl2CppSizeOfArray)
- gotchas: interface methods: `il2cpp_object_get_virtual_method` on the instance
- gotchas: extra exports used: `il2cpp_object_new`, `il2cpp_array_new`, `il2cpp_class_get_nested_types`, `il2cpp_class_instance_size`, `il2cpp_class_value_size`, `il2cpp_field_get_type`, `il2cpp_class_get_element_class`, `il2cpp_gc_wbarrier_set_field`, `il2cpp_class_get_static_field_data`
- gotchas: x64 wrapper ABIs: Vector2 (8 bytes) in registers; Rect / Vector3 / Vector4 / Matrix4x4 by hidden pointer; verified for Texture2D ctor, Sprite.Create, SetParent, get_scene
- gotchas: mod PDB symbolization: identical-code folding gives misleading names (e.g. `vector<Ref>::operator=`)
- mod: MM (ugui.*), shared by MT/LH

### Unity engine / uGUI
- gotchas: uGUI bakes Graphic colour x every parent CanvasGroup alpha into vertex colour; `CanvasGroup.ignoreParentGroups = true` isolates a subtree (SetActive unaffected)
- gotchas: a CanvasRenderer added at runtime has `cullTransparentMesh` on (Unity 6 default): a Graphic whose vertex colours are all transparent is not drawn at all, so an SDF "frame" with a transparent fill and a coloured border (SDFGraphic border via material) is invisible. Use an opaque shape (e.g. a larger ring behind the element) or `CanvasRenderer.set_cullTransparentMesh(false)`. This was why the drone programs dialog's focus frame never showed in rounds 12-13 (drone_price_limit) (inferred from the code + test reports)
- gotchas: setting a Transform/Graphic to the SAME value still dirties it: the canvas rebuilds; set only on change. Canvas rebuild/batching happens in `Canvas.willRenderCanvases`, outside per-module timing; nested Canvases isolate per-frame changes from rebatching the parent
- gotchas: uGUI batches vertices in the ROOT canvas's space (nested Canvas without overrideSorting); for ScreenSpaceOverlay shader positions are canvas space, for Camera/World modes world space (MM assumption for the NegativeMask matrix)
- gotchas: uGUI `Mask` is a stencil mask: binary edge regardless of sprite AA; materials need `_Stencil` to be clipped
- gotchas: `TransformPoint`/`InverseTransformPoint` on prefab asset transforms give the editor layout (assumed, works in practice)
- gotchas: `Resources.FindObjectsOfTypeAll(Type)` ~25 ms, only finds loaded assets; `Object.FindFirstObjectByType(Type, FindObjectsInactive)` scene objects only
- gotchas: Physics: rays skip one-sided mesh colliders / terrain from inside; `Physics.queriesHitBackfaces` toggles backface hits (global; set for one ray and restore); `Physics.OverlapSphere` (4 args) with triggers
- gotchas: GPU read-back of a non-readable texture: `RenderTexture.GetTemporary` + `Graphics.Blit(Texture, RenderTexture)` + `Texture2D(w,h,RGBA32,false,linear).ReadPixels` + `GetPixels32` from the frame callback (assumed to work under HDRP; sandbox prefab icons do the same)
- gotchas: Camera matrices: `get_worldToCameraMatrix` 0x33A9760, `get_projectionMatrix` 0x33A8370 return Matrix4x4 by hidden pointer (verified); camera position = -R^T t; Unity/GL clip space, w > 0 in front
- mod: MM, MT, WM

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

- **IL2CPP value-type field offsets include the 0x10 object header**: subtract `2*sizeof(void*)` for inline struct fields (e.g. CharacterGroundingReport: IsStableOnGround reported 0x11 -> motor+0x98+1 = 0x99). spawn_scaling detects the convention at runtime from the first field's offset. Inline struct sizes: exports `il2cpp_class_value_size` / `il2cpp_class_array_element_size` (GetProcAddress, sanity-checked). Boxed returns from runtime_invoke: value at `2*sizeof(void*)`.
- nested classes resolve as `Outer/Inner` with `il2cpp_class_from_name` (assumed; works for the nested types used)
- static enum values: `il2cpp_field_static_get_value` (ResourceNode.NodeState)
- `List<T>`: `_items`, `_size`, `_version`, `set_Capacity(int)`; in-place element stores use `il2cpp_gc_wbarrier_set_field` when exported
- **Folded body**: `RaycastHit.get_normal` is folded with `ParticleCollisionEvent.get_normal` (RVA 0x339E700, 66 direct callers) - detour must pass through unless scoped (free_placement)
- **Shared stub**: `FabricatorConfirmPopup.ShouldSkipAnimations` = `return false` shared 3-byte stub at 0x510250 (unhookable)
- **Explicit interface implementations** have metadata names `Namespace.IInterface.Method` (e.g. `MonomiPark.SlimeRancher.UI.Fabricator.IFabricatableItem.CheckCanFabricate`); look that up before the plain name
- `Object.FindObjectsByType` / `Resources.FindObjectsOfTypeAll` walk every loaded object and allocate a managed array; cost follows scene size (FindObjectsByType 8-14 ms on the ranch; five FindObjectsOfTypeAll in one frame ~80-90 ms) - never periodic; time-slice or cache
- `Resources.UnloadUnusedAssets` (scene loads) destroys runtime-created assets without `HideFlags.DontUnloadUnusedAsset` (32); a strong GC handle does not protect them. **inferred (log)**: loaded assets too - a vanilla plot-menu item custom_upgrades had taken out of its list (only our GC handle left) was found destroyed (round 11 log, "Music Box II: vanilla menu item of upgrade 2 not found"); custom_upgrades `KeepLoaded` sets the flag on such items
- `Object.DontDestroyOnLoad(go)` keeps a GameObject across scene-group changes (debug_tools)
- `MaterialPropertyBlock`: `.ctor()`, `Clear`, `SetFloat(int, float)`, `SetColor(int, Color)`; `Renderer.GetPropertyBlock(mpb)` / `SetPropertyBlock(mpb)` / `get_sharedMaterial`; `Material.HasProperty/GetFloat/GetColor(int)`; `Shader.PropertyToID(string)`; GetPropertyBlock before writing keeps what the game put there, a snapshot block restores (slime_dj walls.cpp)
- `Transform.SetParent(Transform, bool)`, `get_position/right/forward`, `Rotate`; `GameObject(string)` ctor, `AddComponent(Type)`; `Component.GetComponentsInChildren(Type, bool)`, `GetComponentInParent(Type, bool)`; `Object.Destroy(Object)`, `set_hideFlags`; `Behaviour.set_enabled` - resolved by params in slime_dj
- `il2cpp_object_new` / `il2cpp_array_new` exports (slime_dj managed float[] for SetData)
- `KinematicCharacterMotor.SetPosition(Vector3, bool)` for teleport (hl2 fall recovery); `SetRotation` (teleporters)

## From the sandbox, sell_upgrades, collectibles, instant_build notes

- Prefab rendering for icons (sb prefab_icons.cpp): never instantiate prefabs (scripts register actors); copy enabled MeshRenderer/SkinnedMeshRenderer meshes as bare MeshFilter+MeshRenderer on layer 31 at 10 km up; disabled orthographic Camera culling layer 31, magenta clear, no HDR/MSAA; `Camera.Render` into a 128x128 RenderTexture (5-arg ctor ARGB32/sRGB, 3-arg fallback); `Texture2D.ReadPixels`, `Apply`, `Sprite.Create`; `DestroyImmediate` everything; `HideFlags.DontUnloadUnusedAsset` on the sprite. HDRP lights work in lux vs scene exposure (1.15 lux light renders dim; sb uses 3 directional lights, key 3, auto boost). LOD children named `_LOD01` / `_LOD02` (skip any LOD<n> n>0); skip `Quad RGB Interaction` / `Quad A Interaction` (grass-bending quads)
- `Texture.GetNativeTexturePtr` value may be released/recreated by Unity while the managed Texture lives: take a COM reference (`QueryInterface` ID3D11Texture2D) right after the icall (ui/item_icons, ui/slime_icons)
- Destroyed UnityEngine.Object = `m_CachedPtr` null (used for delegate targets); `Delegate.GetInvocationList`, `Delegate.get_Target`
- `FindObjectOfType` (Unity 6 `FindFirstObjectByType(Type)`) costs 6-14 ms; `FindObjectsOfTypeAll` ~25 ms frames; avoid timers on them
- `il2cpp_format_stack_trace` (optional export) gives managed frames for exceptions from runtime_invoke
- `Transform.SetSiblingIndex` / `SetAsLastSibling` (LayoutBehaviour re-lays out on OnTransformChildrenChanged)
- Nullable<T> layout: bool hasValue at +0, value at +8 (double, GameMetadata)
- ImGui (vendored): `stbrp__skyline_find_best_pos` reads null for rect width <= 0 (bad font size from AddText/CalcTextSizeA/GetFontBaked); patched in third_party/imgui (ui/NOTES.md "ImGui checks") - not game RE, stays in ui/NOTES

## From the ui/ (overlay, Options tab, icons, input) notes

### Texture / Sprite (UnityEngine, CoreModule)
- methods: `Sprite.get_texture`, `get_textureRect` (throws for tightly packed sprites), `get_packed`, `get_packingMode` (SpritePackingMode Tight = 0), `get_uv` (use its bounds when packed+Tight), `get_rect`, `get_pixelsPerUnit`; `Texture.get_width/get_height`
- methods: icall `UnityEngine.Texture::GetNativeTexturePtr_Injected(void* m_CachedPtr)` via `il2cpp_resolve_icall` -> ID3D11Resource* (convention under "Icalls" above); call under SEH, check class + `m_CachedPtr` first
- gotchas: a GC handle keeps the managed Texture, not its GPU resource -> take your own COM ref (`QueryInterface` ID3D11Texture2D) on the main thread right after the icall; never hand Unity's raw pointer to the render thread (Sandbox page overlay crash)
- gotchas: Unity textures are bottom-up rows on D3D11 (assumed for logo UVs; unverified for the logo)
- gotchas: typeless formats sample as UNORM; typed sRGB formats need the sRGB curve restored in the pixel shader
- mod: ui/slime_icons.cpp, ui/item_icons.cpp

### Texture2D / Sprite creation (UnityEngine, CoreModule)
- methods: `Texture2D..ctor(int, int, TextureFormat, bool mipChain)` (resolve by params), `SetPixels32(Color32[])`, `Apply(bool updateMips, bool makeNoLongerReadable)`, `Texture.set_filterMode` (Trilinear = 2), `set_wrapMode` (Clamp = 1), `Sprite.Create(Texture2D, Rect, Vector2, float)`, `Object.set_hideFlags` (DontUnloadUnusedAsset = 32), `Object.set_name`
- gotchas: SetPixels32 expects the bottom row first (flip stb_image output); construct with `il2cpp_object_new` + ctor via invoke
- gotchas: the script ctors (`Texture2D(int,int,TextureFormat,bool[,bool])` 0x340E380 -> 0x340DE80, IDA) pass a default `MipmapLimitDescriptor` -> `Internal_Create(..., ignoreMipmapLimit = !useMipmapLimit = true)`: runtime textures are NOT cut by the game's Texture Quality (`OptionsDirector.SetTextureQuality` 0x9C6210: LOW `QualitySettings.globalTextureMipmapLimit` 2 + aniso off, MEDIUM 1 + aniso on, HIGH 0 + ForceEnable)
- gotchas: sharpness in UI: `Texture.set_mipMapBias` (0x3413EB0). The game's full art uses -1 (data, below); with 0 and trilinear a 512 px sprite in the 440 px Fabricator details box blends ~20 % of mip 1 (round 12 "blurry"). mod_icons and gadget_look `TintedSprite` set mips + trilinear + bias -1
- data (UnityPy, `fullarticons_..._icondecorboombox.png` bundle): full art `iconDecorBoombox` = Texture2D 512x512 DXT5 (fmt 12), 10 mips, filter Trilinear, aniso 1, **mip bias -1**, clamp, sRGB, `m_IgnoreMipmapLimit` 1; Sprite rect 512x512, 50 px/unit
- mod: ui/mod_icons.cpp

### Cursor (UnityEngine, CoreModule)
- methods: `get_lockState`, `set_lockState` (CursorLockMode None 0, Locked 1), `set_visible`
- gotchas: `Cursor.get_visible` is stripped in this build (use CursorManager.ShouldCursorBeVisible for the current value)
- mod: ui/game_input.cpp

### IL2CPP runtime
- `Il2CppGCHandle` is pointer-sized in this build (Unity 6, metadata v31): `il2cpp_gchandle_new(obj, pinned)` / `il2cpp_gchandle_free(handle)` (re/dump/il2cppinspectorredux/cpp/appdata/il2cpp-api-functions.h). A uint32 declaration truncated handles and freeing faulted (3rd in-game run). core/il2cpp_api.h now declares it `void*`
- `il2cpp_format_exception` calls managed ToString: must be guarded (core/resolver.cpp now guards it; ui::il::Invoke builds the text itself from class name + `_message`)
- the GC scans stacks conservatively: a volatile stack reference keeps a freshly allocated managed array alive until returned
- IL2CPP folds identical method bodies: check hook targets for duplicate addresses (options_tab checks its 9 targets pairwise; hook targets must be declared on the class itself, not inherited)
- `il2cpp_object_new`, `il2cpp_object_unbox`, `il2cpp_array_new`, `il2cpp_resolve_icall` are exports not carried by core's table (resolved by name in ui/il2cpp_util.cpp, ui/mod_icons.cpp)
- `UnityEngine.Object.m_CachedPtr` = native object; 0 when destroyed (IsAlive)
- mod: ui/il2cpp_util.cpp, ui/options_tab.cpp

### Transform / Object / Behaviour
- methods used by name: `Transform.Find(string)`, `Transform.get_parent()`, `Object.Destroy(Object)`, `Object.Instantiate`, `Behaviour.get_enabled/set_enabled`, `TMPro.TMP_Text.set_text(string)` (Unity.TextMeshPro), `RectTransform` anchors/pivot (minimap placement: anchors = pivot = (x/100, 1-y/100), anchoredPosition 0 puts the left edge at x% of (parent width - size))
- mod: ui/modded_label.cpp, ui/options_tab.cpp, modules/minimap (placement via ui/hud_edit)

---

## From round 11 (custom_upgrades buildables, debug_tools)

- Sprites: a tightly packed / trimmed atlas sprite has `textureRect` smaller than `rect`; `textureRectOffset` is where the trimmed pixels sit in the rect. A copy of only `textureRect` is a different shape and a UI Image that stretches to its box draws it wider or taller (the Quantum Storage Link's tinted drone source icon). gadget_look.cpp `TintedSprite` copies into the full rect (`Texture2D.ReadPixels(rect, destX, destY)` at the offset) and can pad to a square.
- `ParticleSystem.MainModule` is a struct `{ParticleSystem m_ParticleSystem}`; its getters / setters are icalls taking a pointer to that struct (`il2cpp_resolve_icall("UnityEngine.ParticleSystem/MainModule::get_scalingMode()")`, called with `&ps`). Scaling mode Local (1) = a system uses only its own transform's scale, so scaling a cloned FX root does not shrink its child systems (quantum_link.cpp `ScaleParticles`).
- `PhysicsMaterial` (Unity 6 name; PhysicsModule): `new PhysicsMaterial(string)`, `set_dynamicFriction` / `set_staticFriction` / `set_bounciness` / `set_frictionCombine` / `set_bounceCombine` (PhysicsMaterialCombine Average 0, Minimum 2, Multiply 1, Maximum 3); `Collider.sharedMaterial`. `Rigidbody.linearDamping` (`drag` is the same body). Used by dj_buildables.cpp for the Slime Radio.
- `Component.GetComponentInParent(Type)` is stripped in 1.3.0; only `GetComponentInParent(Type, bool includeInactive)` 0x342EFD0 exists (debug_tools hitboxes failed setup on it in round 11). `GameObject` has both.
- `Rigidbody.set_centerOfMass(Vector3)` 0x3519D90 (local space; turns automatic centre of mass off) / `ResetCenterOfMass()` 0x3517930; `set_angularDamping` 0x3519A10 (`angularDrag` the same body). The Slime Radio's low centre of mass (dj_buildables.cpp, round 12).
- `Physics.OverlapSphere(Vector3, float, int layerMask, QueryTriggerInteraction)` 0x3511930 returns a new Collider[]: a physics query, fine every 0.5 s (debug_tools hitboxes.cpp). `Collider.get_bounds` = world AABB; `Transform.get_localToWorldMatrix` is column-major like Matrix4x4 memory.
- HDRP lights (Unity 6 / HDRP 17): `HDAdditionalLightData.get_intensity` 0x30F6D50 / `set_intensity` 0x30F9D30 just forward to `Light.get_intensity` / `set_intensity` of `legacyLight` (IDA), so `Light.intensity` is the one value to animate (checked for slime_dj lamps; the Electric Pillar Lamp turned out to have no Light at all).
