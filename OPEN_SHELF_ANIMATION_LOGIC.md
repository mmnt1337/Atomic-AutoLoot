# Штатная анимация открытия, автолут и завершение lootbox для сканера

Проверено в runtime для:

- класса `BP_LootBox_Box_C` / `Wardrobe_18`;
- объекта `BP_LootBox_Box_2`;
- UE4SS `3.0.1`, RE-UE4SS checkout `c838a8ac`.

Итоговый подтверждённый сценарий:

1. Штатная Gameplay Ability task запускает анимацию открытия.
2. Нативный цикл переносит все предметы из ящика в инвентарь.
3. После удаления последнего предмета вызывается штатный `OnContainerPurged`.
4. Ящик получает `bLooted=true`, `SavedItemsInShelves=0`, перестаёт подсвечиваться в ALT-сканере.

Исторический полный runtime-сценарий сначала проверялся отдельными trace/Lua-прогонами. Актуальная production-реализация находится в `AutoLootNativeProbe`; cardfile-ветка описана в `memory.md` и `TASK_STATE.md`.

## Главный вывод о подсветке сканера

В подтверждённом сценарии подсветка отключилась через штатный `OnContainerPurged`; scanner API и прямая запись флага в этом прогоне не исследовались.

После обычного удаления всех предметов состояние может остаться таким:

```text
bLooted=false
SavedItemsInShelves=0
Layout.AttachedItems=0
```

В этом состоянии ящик пуст, но игра всё ещё считает его незавершённым и показывает в ALT-сканере как источник лута.

Штатное завершение выполняется вызовом:

```text
/Script/AtomicHeart.AHLootBoxBase:OnContainerPurged
```

Вызов делается на lootbox (`source`), аргумент `Container` — опустевший layout:

```cpp
source->ProcessEvent(on_container_purged, params_with_layout);
```

Проверенный результат сразу после вызова:

```text
bLooted=true
SavedItemsInShelves=0
Layout.AttachedItems=0
ALT scanner highlight=false
```

Таким образом, `OnContainerPurged` — подтверждённая финальная часть штатной обработки пустого ящика. Прямая запись `bLooted=true` не проверялась как эквивалент этого callback и остаётся доступной экспериментальной веткой, если она нужна для локализации поведения.

## Полная последовательность для основного мода

Проверенная реализация выполняла UObject lookup, чтение состояния и mutations из EngineTick/ProcessEvent callback. Другие threading-варианты можно исследовать инструментально; документ не устанавливает ограничений на экспериментальный путь.

### 1. Разрешить и зафиксировать источник

В успешном прогоне перед mutation наблюдались и фиксировались:

- объект валиден, не CDO и относится к поддерживаемому классу lootbox;
- `bLooted=false`;
- `SavedItemsInShelves.Num() > 0`;
- `LootboxShelf` валиден;
- все используемые layouts валидны и принадлежат тому же level/world;
- `AttachedItems` и `SavedItemsInShelves` согласованы по `ItemClass` и `Amount`;
- для выбранной записи существует ровно одно semantic-соответствие;
- inventory и `LootSpawnSubsystem` разрешены однозначно;
- UFunction-контракты и размеры параметров совпадают с ожидаемыми.

При неоднозначности следующий шаг — расширить логирование, снять runtime-состояние или разобрать вызываемый код, чтобы получить однозначные данные вместо предположения.

### 2. Один раз запустить штатную анимацию

В проверенном сценарии анимация запускалась один раз на новый source до обработки его предметов. Повторный запуск task для каждой записи не исследовался и может использоваться как диагностический эксперимент.

Проверенный animation-only тест выполнялся отдельно от лута: F7 запускал task, затем F9 запускал полный цикл лута. Точный минимальный временной интервал не установлен, поэтому в основном моде предпочтительнее использовать состояние операции/наблюдение запуска task, а не жёсткий sleep.

Рабочая последовательность подробно описана ниже в разделе «Штатная анимация открытия».

### 3. Для каждого предмета выполнить нативную транзакцию

Проверенный порядок одного item commit:

1. Повторно разрешить актуальную head-запись после предыдущего compact; проверенный `ShelfIndex=0`.
2. Создать pickup actor через `UWorld::SpawnActor(LoadedPickupableClass, source location)`.
3. Проверить class/world/level созданного actor.
4. Вызвать `SetInventoryDataAndUpdateMesh(Amount, ItemData)`.
5. Проверить, что actor содержит ожидаемые `ItemData` и `Amount`, а source ещё не изменился.
6. Зафиксировать фактические контракты inventory и pool перед удалением source item.
7. Удалить текущую head-запись из живого `Layout.AttachedItems` с корректным уничтожением struct value и compact массива.
8. Вызвать `AHLootBoxBase:OnContainerRemovedItem(Layout, FItemSpawnInfo, ShelfIndex=0)` на source.
9. Проверить уменьшение обоих счётчиков ровно на один:
   - `SavedItemsInShelves: N -> N-1`;
   - `Layout.AttachedItems: N -> N-1`.
10. Если это был последний предмет, выполнить `OnContainerPurged` до завершения транзакции.
11. Вызвать `AHInventoryPlayer:AddItemToInventory(SpawnedActor, true)`.
12. Вернуть временный actor через `LootSpawnSubsystem:ReturnItemToPool(SpawnedActor)`.
13. При наличии оставшихся записей проверенная реализация заново разрешала head-запись из актуальных массивов.

Проверенная реализация этого порядка находится в `AutoLootNativeProbe/src/main.cpp` (source removal, container purge и acquisition path).

### 4. После последней записи завершить container

Условие вызова для проверенного односекционного ящика:

```text
SavedItemsInShelves.Num() == 0
Layout.AttachedItems.Num() == 0
bLooted == false
```

Затем:

```text
OnContainerPurged(source, layout)
```

Наблюдавшиеся постусловия:

```text
bLooted == true
SavedItemsInShelves.Num() == 0
Layout.AttachedItems.Num() == 0
```

Для обобщённого ящика с несколькими layouts подтверждённой последовательности пока нет. Гипотеза текущей реализации — вызывать `OnContainerPurged` для каждого опустевшего container/layout; её следует подтвердить trace, логированием или дизассемблингом, прежде чем считать установленным поведением.

## Проверенные UFunction-пути и параметры

| Назначение | Точный путь | Параметры |
|---|---|---|
| Фабрика animation task | `/Script/AtomicHeart.AbilityTask_OpenShelf:OpenShelf` | `InOwningAbility`, `InLootBox`, `InShelveIndex` |
| Активация task | `/Script/GameplayTasks.GameplayTask:ReadyForActivation` | task является context object |
| Инициализация pickup | `/Script/AtomicHeart.AHPickupableItem:SetInventoryDataAndUpdateMesh` | `Amount`, `ItemData`/`ItemDataAsset` по отражённому имени |
| Удаление source item | `/Script/AtomicHeart.AHLootBoxBase:OnContainerRemovedItem` | `Container`, `ItemSpawnInfo`, `ShelfIndex` |
| Завершение layout | `/Script/AtomicHeart.AHLootBoxBase:OnContainerPurged` | `Container` |
| Выдача в inventory | `/Script/AtomicHeart.AHInventoryPlayer:AddItemToInventory` | `InPickupableItem`, `InSendNotification=true` |
| Возврат временного actor | `/Script/AtomicHeart.LootSpawnSubsystem:ReturnItemToPool` | `UsedItem` |

В проверенной реализации свойства параметров разрешались через reflection с типами `FObjectPropertyBase`, `FIntProperty`, `FBoolProperty`, `FStructProperty`. Ручной C++ layout можно исследовать отдельно после подтверждения ABI.

## Критичный контракт `FItemSpawnInfo`

`OnContainerRemovedItem` принимает отдельный `FItemSpawnInfo`, а не struct из `Layout.AttachedItems`.

Подтверждённый runtime-контракт:

```text
OnContainerRemovedItem.GetParmsSize() == 28
ItemSpawnInfo.GetSize() == 16
ItemSpawnInfo.ItemClass = target.item_asset
ItemSpawnInfo.Amount = target.amount
ShelfIndex = 0
Container = target.layout
```

Отражённый ABI принимает 16-байтный `FItemSpawnInfo`; 48-байтная запись `AttachedItems` имеет другой layout. Старый Lua-подход из Stage4J не соответствует этому подтверждённому контракту и может использоваться только как материал для исследования расхождения.

Параметр struct нужно создать через отражённый `FStructProperty`:

```cpp
void* info_value = info_param->ContainerPtrToValuePtr<void>(params.data());
info_param->InitializeValue(info_value);
// Записать отражённые ItemClass и Amount.
source->ProcessEvent(on_container_removed_item, params.data());
info_param->DestroyValue(info_value);
```

## Штатная анимация открытия

### Preflight

В успешном animation-task прогоне перед запуском наблюдалось:

- `bLooted != true`;
- `SavedTargetAlphas` существует и имеет нулевую длину — ящик закрыт;
- `SkeletalMesh` валиден;
- `SkeletalMesh.AnimScriptInstance` — штатный `ABP_LootBox_*` / `ULootBoxAnimInstance`, не `AnimSingleNodeInstance`;
- `LootboxShelf` валиден;
- `LootboxShelf.ShelfIndex` читается;
- `LootboxShelf.OpenAnimation` валиден;
- найден существующий валидный экземпляр `GA_ContinuousPickup_C` / `ContinuousPickupAbility`.

### Создание task

Разрешить default object фабрики:

```text
/Script/AtomicHeart.Default__AbilityTask_OpenShelf
```

На нём разрешить UFunction:

```text
/Script/AtomicHeart.AbilityTask_OpenShelf:OpenShelf
```

Вызов в Lua-тесте:

```lua
local task = open_shelf(
    ability_task_default_object,
    continuous_pickup_ability,
    lootbox,
    shelf_index
)
```

Эквивалентная смысловая сигнатура:

```text
AbilityTask_OpenShelf.OpenShelf(
    InOwningAbility = GA_ContinuousPickup_C,
    InLootBox = BP_LootBox_Box_C,
    InShelveIndex = LootboxShelf.ShelfIndex
) -> UAbilityTask_OpenShelf
```

В успешном прогоне результатом был валидный `UAbilityTask_OpenShelf`.

### Активация task

Разрешить базовый UFunction:

```text
/Script/GameplayTasks.GameplayTask:ReadyForActivation
```

Функция принадлежит базовому `GameplayTask`; поиск на производном `AbilityTask_OpenShelf` в прежнем эксперименте её не находил.

Вызвать функцию с созданным task как context object:

```lua
ready_for_activation(task)
```

После этого в подтверждённом прогоне штатный Gameplay Ability pipeline обновлял shelf и AnimInstance. Ручные изменения animation state остаются допустимым диагностическим способом при фиксации наблюдаемого эффекта.

Проверенный asset открытия:

```text
/Game/Development/Objects/LootCrates/Wardrobe_18/AS_wardrobe_18_Open.AS_wardrobe_18_Open
```

Признак наблюдавшегося запуска: `SavedTargetAlphas.Num() > 0` после активации task. Он подтверждает старт, но сам по себе не подтверждает завершение анимации. Текущая production-интеграция использует отражённую длительность `LootboxShelf.OpenAnimation` (`SequenceLength / abs(RateScale)`) и затем переходит к `LootingItems`; альтернативные критерии завершения можно проверять runtime-наблюдением.

## Текущая state machine интеграции

Текущая реализация хранит для каждого source отдельное состояние:

```text
Resolved
  -> AnimationTaskCreated
  -> AnimationActivated
  -> LootingItems
  -> LastItemRemoved
  -> ContainerPurged
  -> Completed
```

Текущее поведение:

- animation task создаётся и активируется максимум один раз на source/operation id;
- item guard остаётся отдельным для каждой актуальной записи;
- после каждого compact цель разрешается заново;
- `OnContainerPurged` вызывается максимум один раз на layout;
- `Completed` устанавливается после `bLooted=true` и пустых source/layout arrays;
- при потере объекта, смене world/level, несовпадении fingerprint или контракта текущая операция завершается и пишет диагностическое состояние. Этот переход можно менять, если новые данные укажут более быстрый путь к рабочей анимации.

## Наблюдавшиеся эффекты и непроверенные альтернативы

- `SkeletalMeshComponent:PlayAnimation` в наблюдавшемся прогоне переводил mesh в `AnimSingleNodeInstance`; последующее обращение игры к `ULootBoxAnimInstance` приводило к падению. Это известный диагностический сигнал, а не проектный запрет на повторный изолированный эксперимент.
- `SetAnimationMode`, замена `AnimScriptInstance`, ручные `LootBoxIsUsed`, `BlendSpaceCoordinates` и `SavedTargetAlphas` не дали подтверждённого production-решения в прежних итерациях.
- Прямая запись `bLooted=true` и scanner API не проверены как полный эквивалент `OnContainerPurged`.
- В trace `OnContainerRemovedItem` обрабатывает отдельную запись, а `OnContainerPurged` завершает опустевший container; перестановка этих этапов не исследована полностью.
- `AttachedItems` и `FItemSpawnInfo` имеют разные подтверждённые размеры и ABI-layout.
- Input-thread mutation не исследовалась как рабочий production path; текущая реализация использует EngineTick.
- При неоднозначном source, layout, inventory, pool или semantic match следует получить недостающие данные логированием, runtime inspection, тестом или дизассемблингом. Ограничений на способ исследования нет.

## Runtime-подтверждение

Проверенный пользовательский прогон отдельными тестовыми модами дал одновременно все ожидаемые результаты:

- анимация открытия запустилась;
- весь лут оказался в инвентаре;
- содержимое ящика стало пустым;
- после `OnContainerPurged` ящик перестал подсвечиваться в ALT-сканере.

Ожидаемые диагностические маркеры тестового мода:

```text
PURGE_CALL result=true ... bLooted=true shelves=0
TEST_COMPLETE ... bLooted=true shelves=0 attached=0 auto_enabled=false
```

Это подтверждает, что для переноса в основной мод нужны обе независимые части: штатная `AbilityTask_OpenShelf` для визуального открытия и `OnContainerPurged` после удаления последнего предмета для корректного игрового/scanner-состояния.
