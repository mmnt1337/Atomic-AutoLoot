# Atomic Heart AutoLoot — актуальная память

Этот файл описывает только текущее состояние проекта. Подготовительные этапы, старые Stage/Gate-маршруты и историю экспериментов сюда не возвращать.

## Текущий фокус

- **Производительность:** G5 incremental scanner установлен и визуально подтверждён в игре — прежние статтеры исчезли. Полный `UObjectArray` pass остаётся доступным как диагностический инструмент, если он даст новые данные по анимации.
- **Игровые прогоны:** производительность, выдача, save/reload и повторный запуск подтверждены runtime-наблюдением; повторной выдачи после перезапуска нет.
- **Открытие ящиков:** обычный `BP_LootBox_Box_C` — штатный `AbilityTask_OpenShelf` + длительность `OpenAnimation`. Cardfile (`BP_LootBox_Cardfile_Shelf_C`): R42 возвращает release после hold; R41 подтвердил, что постоянный `LootBoxIsUsed=true` вызывает бесконечную interaction-тряску, поэтому следующий исправляющий путь должен завершать штатный per-shelf task, а не удерживать этот флаг. **Fallback:** loot-without-animation только для cardfile, без quarantine.
- **Приоритет:** сначала получить визуально корректное открытие всех поддерживаемых ящиков. Рефакторинг, сокращение кода, производительность и защитные механизмы рассматриваются после подтверждения результата в игре.

Подготовка инфраструктуры завершена. Новые подготовительные шаги или отдельные Stage-документы не нужны.

## Инфраструктура

- Игра: Atomic Heart на UE4.27; runtime: UE4SS 3.0.1 Beta #0.
- RE-UE4SS закреплён submodule `third_party/RE-UE4SS-c838a8ac` (ревизия `c838a8acaade1a0f860bdf249f039e58f4e10088`).
- Native-мод: `AutoLootNativeProbe/src/main.cpp`.
- Контракт scheduler/allowlist: `AutoLootNativeProbe/src/orchestration.hpp`.
- Standalone проверки: `AutoLootNativeProbe/tests/orchestration_selftest.cpp`.
- Сборка и CTest описаны в `AutoLootNativeProbe/CMakeLists.txt` и корневом `README.md`.

Сборка из корня проекта:

```powershell
cmake -S AutoLootNativeProbe -B AutoLootNativeProbe/build-shipping -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
cmake --build AutoLootNativeProbe/build-shipping --target AutoLootNativeProbe AutoLootOrchestrationSelfTest
ctest --test-dir AutoLootNativeProbe/build-shipping --output-on-failure
```

## Текущая runtime-модель

- Текущая реализация выполняет UObject-доступы и runtime mutations из EngineTick.
- Проект не задаёт обязательного тестового пути или типа сохранения; сценарий прогона выбирается по скорости получения нужных данных.
- Production auto-loop вызывается через EngineTick. На одном кадре scanner просматривает не более `2048` UObject slots и прекращает работу после `750 мкс`; законченная scan generation делает паузу `750 мс` перед следующим обходом.
- Scanner обращается к слотам через `UObjectArray::IndexToObject`, хранит найденные source/floor/world candidates, inventory, pool и active source только как `FWeakObjectPtr`. Сырые UObject pointers не переносятся между кадрами.
- Очереди source/floor/world bounded ёмкостью `4096`. Из готовой очереди выполняется не более одной one-record transaction за EngineTick; остальные записи того же active source разрешаются без нового глобального поиска.
- Клавиша `Ctrl+F9` — ручной trigger. Полные scans можно применять в диагностике и экспериментах, когда они быстрее дают подтверждённые данные.
- Текущая production-реализация item transfer написана на C++; Lua остаётся доступным для быстрых runtime-экспериментов.
- Текущая транзакция выдаёт одну запись за один claim; scheduler сейчас отклоняет повторный claim и параллельную операцию.

## Реализованный поток

1. Incremental scanner распределяет обход текущего snapshot `UObjectArray` между кадрами и складывает подходящие source/floor/world objects в weak-очереди. Inventory и pool публикуются в cache только при единственном live совпадении за законченную generation.
2. Dispatcher выбирает active source, затем source queue, floor queue и generic world queue. Невалидный weak handle отбрасывается без dereference stale pointer.
3. Collector разрешает поддерживаемый source/layout/item tuple только через `LootboxShelf.AttachedContainers`; глобальный fallback-поиск layout в production отсутствует.
4. Descriptor содержит source identity, source/shelf index, class, amount и semantic fingerprint. Текущая реализация повторно читает descriptor перед mutation.
5. При единственном semantic совпадении создаётся claim. Ноль, несколько совпадений, stale descriptor, duplicate или неизвестный tuple в текущей реализации завершаются без mutation; неизменный неизвестный source однократно карантинится и больше не создаёт retry/log storm.
6. Перед первым item commit нового закрытого source один раз создаётся и активируется штатный `AbilityTask_OpenShelf`. `SavedTargetAlphas` подтверждает старт; item transaction начинается только после истечения отражённой длительности `OpenAnimation` с учётом `RateScale`. Ожидание неблокирующее, повторная task для следующих записей того же source не создаётся.
7. Native transaction: spawn/init pickup → удалить одну запись `AttachedItems` → вызвать `AHLootBoxBase:OnContainerRemovedItem` с `FItemSpawnInfo` → после опустошения layout вызвать `OnContainerPurged` → вызвать `AHInventoryPlayer:AddItemToInventory(..., true)` → вернуть actor через `LootSpawnSubsystem:ReturnItemToPool`.
8. При успешном commit source остаётся active через weak handle, поэтому следующая запись обрабатывается на следующем EngineTick без нового scan. Текущий признак завершения source — `bLooted=true` и пустые source/layout arrays. Direct inventory-grid writes в текущем коде отсутствуют.

Коллектор floor pickup выполняется перед generic world-pickup collector, чтобы найденный floor item не голодал из-за общей очереди. В каталоге подтверждена пара `DA_Consumable_aid_small` / `BP_Pickable_consumable_aid_small_C`.

## Подтверждённые профили и открытые области

- Поддерживаются class-based source profiles для traced loot-box/layout поверх общего one-record контракта; exact tuple для базового self-test — `BP_LootBox_Box_C` + `BP_ItemContainerLayout_Box_C` + `BP_Pickable_Ammo_Pistol_C` + `DA_Item_PistolAmmo`.
- Item catalog содержит подтверждённые пары ItemData/pickup class для ammo, resources, recipes и small aid. Для известной пары принимается любой положительный source `amount`; старого лимита по observed amount mask больше нет. Для неизвестных пар требуется получить mapping наблюдением, логированием или дизассемблингом.
- `BP_VovCharacter_C` использует `LootBoxComponent`, а не `AHLootBoxBase`. Нативный removal/commit callback для corpse пока не установлен; это открытая исследовательская область, а не запрет на реализацию.
- Quest, unique и non-stackable items пока не исследованы полностью.
- Для диагностики и исправления нет проектных ограничений на batch issuance, direct grid writes, циклы, полные scans, Lua/C++ эксперименты или изменение существующих guards. Любое изменение должно опираться на подтверждённые runtime-данные.

## Диагностика

- Runtime markers и bounded side-effect counters выводятся из native-мода в UE4SS log.
- Текущий build marker: `ALNP_G8_DETACHED_LAYOUT_FALLBACK_20260804_C838A8AC`. F11 OfficeHouse: Table/Wardrobe_23/Wardrobe_24 не лутались из‑за `AttachedContainers=0` (классы уже в allowlist). G8: same-level layout fallback → SAVED_ONLY. Cardfile-телеметрия по-прежнему пишет `LootBoxIsUsed`/`DrawerMoveDistanceAlpha`/alphas даже при выключенном общем log.
- Runtime G5 подтверждён в игре: прежние лаги отсутствуют, предметы переживают save/reload, повторный запуск не дублирует выдачу.
- Cardfile: release-branch alphas не двигают pose; в наблюдении ContinuousPickup движение связано с `PullAlpha>0`. Поведение `bShouldOpenShelvesInstantly` следует оценивать по runtime-результату, если этот путь используется в новой гипотезе.
- Для анализа traced mappings и callback surface использовать `AUTOLOOT_TRACE_AGENT_INDEX.md`.
- Завершённые исторические логи и архивная память не являются инструкциями для текущей работы.
