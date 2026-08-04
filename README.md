# Atomic AutoLoot

Native C++ UE4SS-мод для *Atomic Heart*. Runtime умеет находить поддерживаемый loot, повторно читать live descriptor и выполнять one-item inventory transaction через штатные API игры.

## Текущий статус

- Базовая выдача предметов работает: class-based allowlist, одна semantic match и один in-flight claim на операцию.
- Scanner и item transfer идут из EngineTick; inventory пишется только через `AHInventoryPlayer:AddItemToInventory`.
- Обычные ящики (`BP_LootBox_Box_C`) открываются штатным `AbilityTask_OpenShelf` с учётом длительности `OpenAnimation`.
- Актуальный фокус — визуально корректная анимация cardfile (`BP_LootBox_Cardfile_Shelf_C`). Выдача без полной анимации для cardfile допустима как временный fallback.
- Для corpse source `BP_VovCharacter_C` surface `LootBoxComponent` подтверждён; точный removal/commit callback пока не найден.

Приоритет: сначала подтвердить анимацию в игре на воспроизводимом сценарии. Рефакторинг, производительность и защитные механизмы — после этого.

Подробный runtime-контракт: `AUTOLOOT_RUNTIME.md`. Актуальное состояние исследования: `memory.md`, `TASK_STATE.md`.

## Управление

| Клавиша | Действие |
| --- | --- |
| `Ctrl+F9` | Ручной trigger (fallback) |
| `Ctrl+F10` | Auto-loop on/off |
| `Ctrl+F11` | Native-логи on/off (A/B по статтерам) |

Floor pickup collector обслуживается перед generic world-pickup collector.

## Требования

- Windows x64
- Visual Studio с C++ workload и Windows SDK
- CMake 3.22+ и Ninja
- Rust toolchain (зависимость RE-UE4SS)
- UE4SS 3.0.1 Beta #0; submodule `third_party/RE-UE4SS-c838a8ac`

Исходники: `AutoLootNativeProbe/src`. Контракт scheduler/allowlist: `AutoLootNativeProbe/src/orchestration.hpp`. Self-test: `AutoLootNativeProbe/tests`.

## Сборка

Из корня репозитория:

```powershell
cmake -S AutoLootNativeProbe -B AutoLootNativeProbe/build-shipping -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
cmake --build AutoLootNativeProbe/build-shipping --target AutoLootNativeProbe AutoLootOrchestrationSelfTest
ctest --test-dir AutoLootNativeProbe/build-shipping --output-on-failure
```

Готовая DLL:

```text
AutoLootNativeProbe/build-shipping/Game__Shipping__Win64/bin/AutoLootNativeProbe/main.dll
```

## Установка

1. Установить UE4SS в каталог `Binaries/Win64` игры (или вложенный `ue4ss`, в зависимости от используемой раскладки).
2. При остановленной игре скопировать DLL в:

```text
<Game>/Binaries/Win64/ue4ss/Mods/AutoLootNativeProbe/dlls/main.dll
```

Пример копирования (подставить свой путь к игре):

```powershell
$source = "AutoLootNativeProbe/build-shipping/Game__Shipping__Win64/bin/AutoLootNativeProbe/main.dll"
$target = "<Game>/Binaries/Win64/ue4ss/Mods/AutoLootNativeProbe/dlls/main.dll"
Copy-Item -LiteralPath $source -Destination $target -Force
```

3. Включить `AutoLootNativeProbe` в конфигурации UE4SS (`mods.txt` / enabled mods).
4. После запуска проверить свежий `UE4SS.log` рядом с UE4SS DLL.

Снимок последнего runtime-лога лежит в корне репозитория: `UE4SS.log`.

## Критерии готовности

- Визуально корректная анимация подтверждена в игре на воспроизводимом сценарии.
- Логи или runtime inspection объясняют animation path и итоговое состояние.
- Выдача и source cleanup работают в том же сценарии.
