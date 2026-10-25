# Shop (ProntoMart) and economy

`MP.SR` = `MonomiPark.SlimeRancher`. Tags: **code** = decompiled; **data** = asset bundles. Mod use: `modules/custom_upgrades/` (silo Sell-All: `silo_world.cpp`, `silo_receipt.cpp`; `radio_shop.cpp`: Slime Radio in Daily Deals), `modules/drone_price_limit/`. Hooks: re/hooks.md.

## Plort economy (code)

- `PlortEconomyDirector.TrySell(id, count, ignoreMarketShutdown, showUiNotification, source)` 0x9E52A0 (`SceneContext.PlortEconomyDirector`): false if the market is shut down (`IsMarketShutdown`, time-of-day) or the type has no market value; else `PlayerState.AddCurrency(null = Newbucks, value x count, showUi)` + `RegisterSold(id, count, total)` (saturation: `worldModel` dictionary += count, OnSold event, MetaGame stats, session counts). Same calls as the Plort Market and ranch drones (`ScorePlort.Deposit`). Price = rounded current value; saturation lowers it at the next price update.
- `GetCurrValue(id)`, `TryGetMarketValues`, `GetMarketValue(id)`; `PlortEconomySettings.PlortsTable` `InitialValue` (= `CurrValueEntry.BaseValue`).
- Sell order for a silo: TrySell first, then `AmmoSlotManager.Decrement` (never lose plorts without pay). Only `IdentifiableType.IsPlort` types with a market value.
- `SpendableDirector.Spend(cost)`; `PlayerState.AddCurrency`; `CurrencyDefinition` (`LookupDirector.CurrencyList`).

## Shop UI (data + code; from re/debug-mode.md)

- `ShopUIBase` (shop UI scene, exists only while open): `_shopMusicList` +0xF8 (`ShopMusicListDefinition`), `Update` waits `_musicDelaySeconds` then `ShopMusicListDefinition.PickSong(ShopUIBase.GetWorldTime(), eventDirector)`.
- `ShopAudioListDefinitionBase<T>.PickAudio`: `(int)worldTime % 86400`, entries whose `Condition` is true and whose `ScheduledTimes` slot contains the time (start incl., end excl.), weighted by `OccurrenceWeight`; unscheduled entries only as fallback. Song table (Secret 00:00-00:05, Chip weight 0.025, ...): re/debug-mode.md.
- `ShopNightMarketStyle.OverrideMusicTracks`; Caretaker shop has its own empty list.
- `ShopNpcBehaviour` (the cat): `TryClickOn` -> `CanInteractWith` -> `InteractWith` (`_clickedCue`, trigger `CLICKED`, `_onCatInteracted`, `MetaGameDirector.Grant(YouCanPetIt)`); annoyance 0-3. `ShopDirector.GetOrCalcNpcColorPaletteIndex` (random per session).
- Real-date schedules: `ShopDayOfYear`, `ShopDate`, `ShopRealDateYearlyScheduleDefinition`, `ShopRealDateScheduleDefinition`; only `NightMarket_DailySchedule` ships.

## From the custom_upgrades notes

### Shop data (scriptableobjectlookupdata_assets_all_*.bundle, UnityPy)
- toys in category "DailyDeals" (ShopItemCategoryDescription, analytics name "DailyDeals"): rule set `DailyDeals_BaseToysRuleSet` (one toy per restock) over `DailyDeals_BaseToysTable` (22 toys, 500..2000 Newbucks, each limited to 1)
- Night Light entry: GUID 6b6134bfd2a4907479a87c894d40324c, reference / unique id `ToyDefinition.NightLight`, filter flags 12 (TOY | SINGLE_ITEM), 500 Newbucks
- "RangeExchange" (Coo-Coo Corner) category also has toy entries (158 items in a test)
- `ToyShopUI` / `ToyShopUIActivator` are empty shells; ranch shop = generic ShopDirector / ShopRuntime (per kiosk) / ShopCategoryRuntime / ShopRuntimeItem; UI ShopContentPage / ShopItemButton / ShopPurchasePopup / ShopItemDescriptionBlurb

### ShopCategoryRuntime
- methods: `GetRuntimeItems` 0x8B37C0 - when `_cachedItemList` empty builds one ShopRuntimeItem per provider entry, sorts, returns the list itself; callers shop UI and `ShopRuntime.GetInStockItemCount`
- fields: `_cachedItemList`, `_lastSeedUsed` (seed of its random restock)
- mod: radio_shop.cpp (post detour: radio replaces the daily toy at one restock in 4)

### ShopRuntimeItem
- methods: ctor 0x8C1960 (`_owningCategory`, `_sourceProvider`, `_transactor` from ShopCategoryContext; UpdateWithImpl: `_itemEntry`, `_assetFilterFlags` = asset ref flags | SINGLE_ITEM, `_runtimeHelper` = CreateRuntimeHelper(entry))
- methods: `get_Asset` 0x8C1E20 (loaded handle result) feeds get_Title / get_Icon / get_SmallIcon / get_FullArtReference / get_RequiresFullArt (asset as IUIDisplayData / IFullArtAndIconSource; icon and art only while `_loadHandle` valid and done), get_ItemType / GetAcquirableCount / CanAcquireIgnoringCosts (runtime helper + asset); `get_Description` 0x8C1EC0 (helper.GetDescription(asset) = pedia entry text); `get_AssetGuid`
- methods: `TryPurchase(count, spawnHandlerId, session)` 0x8C1040 - only caller ShopContentPage's popup (page `_spawnHandlerId`): CanAcquireIgnoringCosts, transactor.CanAfford, `AcquisitionUtility.Acquire_SpawnOrStore(asset)` (callback queues at `ShopDataModel.QueueToSpawnItem(spawnHandlerId)` + PendingSpawnItemsChanged = `ShopDirector.QueueToSpawnItem` 0x8B9530; queues under whatever id, empty included), `ShopRuntimeTransactor.Purchase` 0x8DE1C0 (Spend(PurchaseCost, count) + `ShopDirector.TrackItemPurchased` = `ShopDataModel.TrackItemPurchase(category, unique id)`, saved), NotifyPurchaseComplete, category updater
- fields: `_isSoldOut`; `ReleaseResources` releases the AssetReference
- gotchas: `AssetInterface<T>.LoadAsset` returns the AssetReference's own operation when valid -> two items must not share one AssetReference object; a sprite inside a texture is addressed by GUID + `m_SubObjectName` / `m_SubObjectType`
- mod: radio_shop.cpp (get_Asset, get_Description, TryPurchase detours; pointer compares)

### ToyShopItemHelper
- GetTypeInfo = TOY_INDIVIDUAL, CanAcquireIgnoringCosts true, GetAcquirableCount 999, GetOwnedCount / GetMaxCanOwnCount -1, `GetPediaEntry` casts the asset to ToyDefinition (throws otherwise); `ToyShopItemHelper.Instance`

### ShopDataModel / ShopItemSpawner
- saves `_shopPurchasesByCategory` and `_itemsPendingSpawn` (Dictionary<string, Queue<IdentifiableType>>)
- `ShopItemSpawner.DispenseNextItem` 0x8BB210 -> `ShopDirector.SpawnNextItem` 0x8B9590 (dequeue, InstantiateActor at the spawner, out identSpawned) every few seconds
- mod: radio_shop.cpp (SpawnNextItem post detour lists the Night Light as a radio)

### PlortEconomyDirector
- `TryGetMarketValues` for the receipt quote (current price)

## From the slime_dj, spawn_scaling, hl2_movement, quick_start, free_placement, misc_tweaks, plot_upgrades, drone_price_limit notes

### PlortEconomyDirector
- `GetCurrValue(id)` = `RoundToInt(_currValueMap[id].CurrValue)` (null without entry); `CurrValueEntry.BaseValue` = `PlortEconomySettings.PlortsTable` InitialValue (disasm)
- mod: drone_price_limit

### MarketUIConfiguration
- `_plorts` (PlortEntry `IdentType`): the market terminal's plort order (MarketUI uses the same asset type); ranch terminal `PriceEntry` = item icon + coin icon + amount
- coin icon: `LookupDirector._currencyList._currencies[0]._icon` (default currency, Newbucks)
- the Fabricator / Slimepedia / shop Newbucks sprite is `iconNewBuck` (asset copies under ui/fabricator, ui/pedia, ui/shop, lookupdata; the caretaker shop's `iconNewbuck` is a different one) (data). sandbox's `NewbuckIcon` takes the currency's `_icon` when it has that name, else finds it in its one-time sprite scan; it logs `Newbucks icon: iconNewBuck from ...` or the currency icon's real name (log line still to be seen)
- mod: drone_price_limit (cheat sheet)

