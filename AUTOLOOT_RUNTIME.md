# AutoLoot — runtime contract

Каноническое описание текущего native runtime. Документ фиксирует инфраструктуру и реализацию; подготовительные этапы и исторические названия этапов здесь не используются.

## Source resolution

Allowlist строится по class names, не по transient instance names. Для каждого кандидата runtime проверяет:

- source world/level и ownership layout;
- поддерживаемую пару source class/layout class;
- `bLooted`, `SavedItemsInShelves` и live `AttachedItems`;
- единственное semantic совпадение source record и attached record;
- item class, ItemData и положительный source amount. Amount не ограничивается исторически наблюдавшейся битовой маской.

Дескриптор содержит source identity, source index, shelf index, item class, amount и semantic fingerprint. UObject pointer не является identity.

## Revalidation and claim

В текущем коде descriptor повторно читается перед mutation. Изменение source/layout/array, stale record, fingerprint mismatch, duplicate projection или неоднозначное количество переводят текущую операцию в диагностируемое завершение без mutation.

Текущий scheduler использует один in-flight claim. Повторный trigger, повторный source key или unresolved durable intent приводят к `Halted`; это описание существующей реализации, которую можно менять при наличии подтверждённых данных.

Состояния scheduler: `Idle → Resolving → Claimed → Transaction → Committed` либо `Aborted/Halted`.

## One-record transaction

Для поддерживаемого container source порядок вызовов такой:

```text
resolve + live revalidate
  → claim
  → spawn pickup actor
  → SetInventoryDataAndUpdateMesh(amount, ItemData)
  → remove exactly one AttachedItems element
  → AHLootBoxBase:OnContainerRemovedItem(layout, FItemSpawnInfo, shelf_index)
  → AHInventoryPlayer:AddItemToInventory(actor, true)
  → LootSpawnSubsystem:ReturnItemToPool(actor)
  → verify postconditions
  → commit
```

`FItemSpawnInfo` содержит `ItemClass` и `Amount` и имеет размер 16 байт. Отражённый ABI callback ожидает именно эту 16-байтовую структуру; 48-байтовый `FContainerSpawnInfo` из массива `AttachedItems` имеет другой layout.

Одна успешная операция обязана дать ровно по одному spawn, initializer, source-array removal, removal callback, inventory call и pool return. Inventory grid напрямую не изменяется.

Postconditions:

- source/layout остаются валидными;
- `SavedItemsInShelves` и `AttachedItems` уменьшены ровно на один;
- `bLooted` не изменён некорректно;
- native inventory API вызван с notification enabled;
- actor возвращён в pool; при ином результате текущая реализация фиксирует незавершённую операцию;
- результат устойчив после save/reload.

## Collectors and dispatch

- Текущий scanner выполняется небольшими EngineTick slices: максимум `2048` UObject slots и `750 мкс` за кадр. После полной generation следующая начинается через `750 мс`.
- Обход использует `UObjectArray::IndexToObject`; production auto-loop сейчас не вызывает полный `ForEachUObjectInRange`. Полные scans доступны для диагностики и экспериментов.
- Source, floor pickup и world pickup складываются в bounded weak-очереди ёмкостью `4096`. Inventory, pool и active source хранятся как `FWeakObjectPtr` и проверяются перед использованием.
- Готовая работа выбирается в порядке active source → source queue → floor pickup queue → generic world-pickup queue. За EngineTick выполняется максимум одна one-record transaction.
- После успешной container transaction тот же source остаётся active, поэтому оставшиеся записи обрабатываются по одной на следующих кадрах без нового глобального поиска.
- Текущий layout resolution использует доказанную связь `LootboxShelf.AttachedContainers`; fallback «любой layout того же level» сейчас отсутствует.
- Для pool reuse сейчас используются exact class resolution и semantic checks; общий all-Actor scan можно применять для сбора данных или экспериментальной реализации.
- После неуспешной попытки source quarantine позволяет продолжить работу с другими источниками без повторного риска для того же объекта.
- `Ctrl+F9` — trigger; в текущем коде callback планирует работу для EngineTick.
- `Ctrl+F10` — auto-loop on/off; `Ctrl+F11` — runtime native-log output on/off для A/B-диагностики.

## Supported catalog

Текущие source/layout и ItemData/pickup mappings находятся в `AUTOLOOT_TRACE_AGENT_INDEX.md` и дублируются в `AutoLootNativeProbe/src/orchestration.hpp`. Runtime проверяет весь tuple, а не только имя item asset. Для подтверждённой пары ItemData/pickup class разрешён любой положительный amount.

Подтверждена world-pickup пара `DA_Consumable_aid_small` / `BP_Pickable_consumable_aid_small_C`. Для `BP_VovCharacter_C` подтверждён отдельный `LootBoxComponent` surface; removal/commit callback пока не найден и требует дополнительного trace или дизассемблинга.

## Исследование и изменения

- Проект не задаёт обязательных test-only путей, типа сохранения, safety gates или ограничений на способ runtime-исследования.
- Разрешены изменения threading-модели, direct inventory writes, batch/multi-item issuance, полные обходы, Lua/C++ прототипы и изменение guards, если это самый быстрый способ подтвердить причину или получить рабочую анимацию.
- Неподтверждённые адреса, структуры и контракты сначала измеряются через логирование, runtime inspection, тест или дизассемблинг; догадки не фиксируются как реализация.
- Quest, unique и non-stackable items пока являются неисследованной частью реализации, а не запрещённой областью.

## Проверка

Standalone `AutoLootOrchestrationSelfTest` проверяет allowlist, uncapped positive amount, duplicate/stale/thread/save guards, bounded counters, commit shape и halt после unresolved intent. Runtime-прогон G5 подтвердил отсутствие прежних статтеров, корректную выдачу, persistence после save/reload и отсутствие повторной выдачи после перезапуска. Активных runtime-блокеров нет. Текущий marker: `ALNP_G5_INCREMENTAL_SCAN_20260727_C838A8AC`.
