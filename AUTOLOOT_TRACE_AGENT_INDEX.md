# AutoLoot — trace reference

Справочник подтверждённых UE4SS trace-наблюдений. Он нужен для сверки runtime allowlist и callback-контрактов, а не для планирования новых подготовительных этапов.

## Источник

Основной trace: `AUTOLOOT_TRACE_FULL_FRAGMENTS.log` и отфильтрованный `AUTOLOOT_TRACE_RELEVANT_EVENTS.log`. В анализе использованы события `SOURCE_IDENTITY`, `LAYOUT_SURFACE`, `SOURCE_RECORD`, `EXISTING_PICKUP_INIT`, `CALL_TRACE`, `OPERATION_*` и `SPAWN_OBSERVATION`.

## Source/layout catalog

| Source class | Observed layout / surface | Runtime status |
|---|---|---|
| `BP_LootBox_Box_C` | `BP_ItemContainerLayout_Box_C` | поддерживаемый one-record container contract |
| `BP_LootBox_Crate_C` | `BP_ItemContainerLayout_Crate_WeaponLoot_C` | class/layout mapping подтверждён; item tuple проверяется отдельно |
| `BP_LootBox_Commode_C` | `BP_ItemContainerLayout_Commode_C` | class/layout mapping подтверждён |
| `BP_LootBox_Table_C` | `BP_ItemContainerLayout_Table_C` | class/layout mapping подтверждён |
| `BP_LootBox_Wardrobe_24_C` | callback сообщает `BP_ItemContainerLayout_Table_C` | учитывать фактический callback layout |
| `BP_Lootbox_Wardrobe_23_C` | `BP_ItemContainerLayout_Wardrobe_23_C` | class/layout mapping подтверждён |
| `BP_LootBox_Cardfile_Shelf_C` | `BP_PickupableItemContainer_ShelfLoot_C` | распознан; текущая one-record validation доступна как исходная реализация |
| `BP_VovCharacter_Invalid_C` | `LootBoxComponent` + `BP_PickupableItemContainer_VovaInvalid_C` | corpse surface; R47 seq3 one-record path |
| `BP_VovCharacter_Invalid3_C` | `LootBoxComponent` + `BP_PickupableItemContainer_Vova_C` | corpse surface; R47 seq4 one-record path |
| `BP_VovCharacter_C` | `LootBoxComponent` | отдельная surface; enable after in-game confirmation |
| `BP_DestructibleChicken_C` | специальная destructible surface | callback-контракт пока не найден; требуется дополнительное исследование |
| `BP_DestructibleUtka_C` | world-drop после разрушения (F11 2026-08-04) | `AddItemToInventory` → `ReturnItemToPool` на `BP_Pickable_CapsuleJelly_C` + `BP_Pickupable_Metal_Parts_C`; `loot_component=nil`; allowlist container/corpse surface нет |
| `BP_DestructiblePchela_Blue_C` | world-drop destructible (F11 2026-08-04) | запись без дельты инвентаря; surface как у Chicken/Utka (`loot_component=nil`) |

Имена экземпляров вроде `BP_LootBox_Box_2` — это map data, а не class identity. Их можно использовать как конкретные runtime targets и точки диагностического наблюдения.

## ItemData → pickup class

| ItemData | Pickup class | Наблюдавшиеся amounts |
|---|---|---:|
| `DA_Item_PistolAmmo` | `BP_Pickable_Ammo_Pistol_C` | 1, 2, 6 |
| `DA_Item_ShotgunAmmo` | `BP_Pickupable_Shotgun_Ammo_single_C` | 1, 2, 3, 13 |
| `DA_Item_AK47Ammo` | `BP_Pickupable_AK47_Ammo_Magazine_C` | 1, 15 |
| `DA_Item_Jelly_Resource` | `BP_Pickable_CapsuleJelly_C` | 1, 4 |
| `DA_Item_Resources_Metal_Parts` | `BP_Pickupable_Metal_Parts_C` | 1 |
| `DA_Item_Synthetic_Material_Resource` | `BP_Pickable_Synthetic_Material_Resource_C` | 1, 4 |
| `DA_Item_Resource_Biomaterial` | `BP_Pickable_Resource_Biomaterial_C` | 2 |
| `DA_Item_Resources_Chemicals` | `BP_Pickupable_Resources_Chemicals_C` | 1 |
| `DA_Item_Resource_Superconductor` | `BP_Pickupable_Resource_Superconductor_C` | 1 |
| `DA_Item_Resource_Microelectronics` | `BP_Pickupable_Resource_Microelectronics_C` | 1 |
| `DA_Item_Energy_Cell_Resource` | `BP_Pickupable_Energy_Cell_Resource_C` | 1 |
| `DA_Consumable_aid_small` | `BP_Pickable_consumable_aid_small_C` | 1 |
| `DA_Cassette_Electricity_Recipe` | `BP_Pickable_Cassette_Electricity_Recipe_C` | 1 |
| `DA_Item_Pashtet_Recipe` | `BP_Pickable_Cassette_Electricity_Recipe_C` | 1; mapping требует отдельной проверки |
| `DA_Item_Cassette_FIre` | `BP_Pickable_Cassette_Fire_C` | 1; F11 Box11_2 2026-08-04 (asset typo FIre) |
| `DA_Consumable_sguschenka` | `BP_Pickable_sguschenka_C` / `BP_Pickupable_Sguschenka_C` | floor/world R53; container tuple R54 |

Наблюдение в trace подтверждает catalog mapping. Текущая реализация дополнительно проверяет tuple и принимает любой положительный amount для известной пары; новые пары и source следует подтверждать дополнительным trace, логированием или дизассемблингом.

## Container transaction surface

Для обычного container trace подтверждает цепочку:

```text
SetInventoryDataAndUpdateMesh(amount, ItemData)
  → OnContainerRemovedItem(Container, FItemSpawnInfo, ShelfIndex)
  → AddItemToInventory(InPickupableItem, true)
  → ReturnItemToPool(UsedItem)
```

`FItemSpawnInfo` — 16-байтовая структура с `ItemClass` и `Amount`. `FContainerSpawnInfo` из `AttachedItems` имеет размер 48 байт и другой ABI-layout.

После удаления одной записи ожидаются:

- `SavedItemsInShelves` уменьшается на один;
- live `AttachedItems` уменьшается на один;
- `bLooted` не меняется преждевременно;
- ровно один inventory call с notification и один pool return;
- direct inventory-grid writes отсутствуют.

## Особые источники

- `BP_VovCharacter_Invalid_C` / `BP_VovCharacter_Invalid3_C`: R47 seq3/4 подтвердили commit через grab `SpawnedItem` → `AddItemToInventory` → `ReturnItemToPool`. `AHLootBoxBase:OnContainerRemovedItem` не вызывается. Runtime one-record чистит `LootBoxComponent.SavedItems` + `PickupableItemContainer.AttachedItems` (и пустой `AttachedItemContainers`) без AHLootBoxBase removal и без ContinuousPickup grab.
- `BP_VovCharacter_C`: тот же `LootBoxComponent` surface; в allowlist есть, `single_record_safe=false` до подтверждения базового класса в игре.
- `BP_DestructibleChicken_C`: surface scan обнаружил свойства, но не callable loot/removal callback. Следующий полезный шаг — расширенный trace или дизассемблинг обработчика destructible loot.
- `BP_DestructibleUtka_C` (2026-08-04 F11 seq1/5): после разрушения loot идёт как world pickup через ContinuousPickup → `AddItemToInventory`/`ReturnItemToPool` (Jelly + Metal Parts). Мод уже подбирает эти drops через `WORLD_PICKUP`. Отдельного LootBoxComponent/OnContainerRemovedItem нет.
- `BP_DestructiblePchela_Blue_C` (2026-08-04 F11 seq4): тот же unknown/no-loot-component surface; в этой записи инвентарь не изменился.
- SAVED_ONLY containers (2026-08-04 crash): при `AttachedItems=0` вызов одного `OnContainerRemovedItem` не уменьшает `SavedItemsInShelves` → `source_removal_invariants` FAIL и retry-loop. Fix: удалять строку из `SavedItemsInShelves` напрямую (как live AttachedItems surgery).
- Маленькая аптечка (`BP_Pickable_consumable_aid_small_C`) наблюдалась как готовый world pickup с `AddItemToInventory`; в trace нет полного init/remove/pool цикла. Это граница имеющихся данных, которую можно расширить новым наблюдением.

## Границы имеющихся данных

- Порядок событий и содержимое `SOURCE_RECORD` относятся к разным наблюдениям: pooled pickup actor может не соответствовать ближайшей source-записи по имени или порядку.
- `EXISTING_PICKUP_INIT` и `SPAWN_OBSERVATION` подтверждают только наблюдавшиеся классы.
- При stale, duplicate или неполном trace требуется собрать дополнительный факт; проектных запретов на способ получения этого факта нет.
