#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <cstdint>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/World.hpp>

#include "orchestration.hpp"

namespace
{
    using namespace RC;
    using namespace RC::Unreal;

    constexpr auto Marker = STR("ALNP_7C_CONTAINER_X1C_20260725_C838A8AC");
    constexpr auto OrchestrationMarker = STR("ALNP_AUTO_X1_20260726_C838A8AC");
    constexpr auto BuildVariantMarker = STR("ALNP_G6_SKIP_DOUBLE_PURGE_20260804_C838A8AC");
    constexpr bool VerboseDiagnostics = false;
    constexpr int32 IncrementalScanMaxSlots = 2048;
    constexpr int64 IncrementalScanBudgetUs = 750;
    constexpr std::size_t IncrementalQueueCapacity = 4096;

    constexpr auto PickupBaseClassPath = STR("/Script/AtomicHeart.AHPickupableItem");
    constexpr auto InitializePickupPath = STR("/Script/AtomicHeart.AHPickupableItem:SetInventoryDataAndUpdateMesh");
    constexpr auto AddInventoryPath = STR("/Script/AtomicHeart.AHInventoryPlayer:AddItemToInventory");
    constexpr auto RemoveSourceItemPath = STR("/Script/AtomicHeart.AHLootBoxBase:OnContainerRemovedItem");
    constexpr auto PurgeContainerPath = STR("/Script/AtomicHeart.AHLootBoxBase:OnContainerPurged");
    constexpr auto ReturnToPoolPath = STR("/Script/AtomicHeart.LootSpawnSubsystem:ReturnItemToPool");

    struct AcquisitionTarget
    {
        UObject* source{};
        UWorld* source_world{};
        UObject* source_level{};
        UObject* layout{};
        UObject* item_asset{};
        UClass* loaded_class{};
        UObject* loot_component{};
        UObject* spawned_item{};
        int32 amount{};
        int32 shelf_index{-1};
        int32 source_index{-1};
        uint64_t record_fingerprint{};
        int32 shelves_before{};
        int32 attached_before{};
        bool looted_before{};
        bool corpse_surface{};
    };

    struct RuntimeScanView
    {
        AcquisitionTarget target{};
        UObject* inventory{};
        UObject* pool{};
        UObject* floor_pickup{};
        UObject* world_pickup{};
        StringType floor_identity{};
        StringType world_identity{};
    };

    struct QueuedPickup
    {
        FWeakObjectPtr object{};
        StringType identity{};
    };

    std::vector<StringType> ProcessedFloorPickups;
    std::vector<StringType> ProcessedWorldPickups;
    std::atomic_bool RuntimeLoggingEnabled{true};

    template <typename... Arguments>
    auto emit_log(const TCHAR* format, Arguments&&... arguments) -> void
    {
        if (!RuntimeLoggingEnabled.load(std::memory_order_relaxed)) return;
        RC::Output::send(format, std::forward<Arguments>(arguments)...);
    }

    auto source_record_fingerprint(const AcquisitionTarget& target) -> uint64_t
    {
        // Semantic identity only: never use a transient UObject address.
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        const auto mix_name = [&mix](UObject* object) {
            if (!object)
            {
                mix(0);
                return;
            }
            for (const auto character : object->GetFullName())
            {
                mix(static_cast<uint64_t>(character));
            }
            mix(0xff);
        };
        mix_name(target.source);
        mix_name(target.item_asset);
        mix(static_cast<uint64_t>(target.amount));
        mix(static_cast<uint64_t>(target.shelf_index));
        mix(static_cast<uint64_t>(target.source_index));
        return hash;
    }

    auto orchestration_source_key(const AcquisitionTarget& target) -> uint64_t
    {
        // Keep the confirmed 7C key untouched; this key explicitly includes the allowlist class/layout.
        uint64_t hash = target.record_fingerprint ^ 0x9E3779B97F4A7C15ull;
        const auto mix_name = [&hash](UObject* object) {
            if (!object)
            {
                hash ^= 0;
                return;
            }
            for (const auto character : object->GetFullName())
            {
                hash ^= static_cast<uint64_t>(character);
                hash *= 1099511628211ull;
            }
            hash ^= 0xff;
        };
        mix_name(target.layout);
        mix_name(target.loaded_class);
        return hash ? hash : 1;
    }

    struct SemanticSourceRecord
    {
        UObject* item_asset{};
        int32 amount{};
    };

    auto count_semantic_matches(const std::vector<SemanticSourceRecord>& records, UObject* item_asset, int32 amount, int32& matched_index) -> int32
    {
        int32 matches{};
        matched_index = -1;
        for (int32 index = 0; index < static_cast<int32>(records.size()); ++index)
        {
            if (records[index].item_asset == item_asset && records[index].amount == amount)
            {
                ++matches;
                matched_index = index;
            }
        }
        return matches;
    }

    struct ScriptArrayHeader
    {
        void* data{};
        int32 num{};
        int32 max{};
    };
    static_assert(sizeof(ScriptArrayHeader) == 16);

    auto exact_name(UObject* object) -> StringType
    {
        return object ? object->GetFullName() : STR("<null>");
    }

    auto catalog_source_name(UObject* source) -> std::string_view
    {
        if (!source || !source->GetClassPrivate()) return {};
        const auto name = source->GetClassPrivate()->GetName();
        if (name == STR("BP_LootBox_Box_C")) return "BP_LootBox_Box_C";
        if (name == STR("BP_LootBox_Crate_C")) return "BP_LootBox_Crate_C";
        if (name == STR("BP_LootBox_Commode_C")) return "BP_LootBox_Commode_C";
        if (name == STR("BP_LootBox_Table_C")) return "BP_LootBox_Table_C";
        if (name == STR("BP_LootBox_Wardrobe_24_C")) return "BP_LootBox_Wardrobe_24_C";
        if (name == STR("BP_Lootbox_Wardrobe_23_C")) return "BP_Lootbox_Wardrobe_23_C";
        if (name == STR("BP_LootBox_Cardfile_Shelf_C")) return "BP_LootBox_Cardfile_Shelf_C";
        if (name == STR("BP_VovCharacter_Invalid_C")) return "BP_VovCharacter_Invalid_C";
        if (name == STR("BP_VovCharacter_Invalid1_C")) return "BP_VovCharacter_Invalid1_C";
        if (name == STR("BP_VovCharacter_Invalid2_C")) return "BP_VovCharacter_Invalid2_C";
        if (name == STR("BP_VovCharacter_Invalid3_C")) return "BP_VovCharacter_Invalid3_C";
        if (name == STR("BP_VovCharacter_C")) return "BP_VovCharacter_C";
        if (name == STR("BP_VovCharacter_Fast_C")) return "BP_VovCharacter_Fast_C";
        return {};
    }

    auto catalog_layout_name(UObject* layout) -> std::string_view
    {
        if (!layout || !layout->GetClassPrivate()) return {};
        const auto name = layout->GetClassPrivate()->GetName();
        if (name == STR("BP_ItemContainerLayout_Box_C")) return "BP_ItemContainerLayout_Box_C";
        if (name == STR("BP_ItemContainerLayout_Crate_WeaponLoot_C")) return "BP_ItemContainerLayout_Crate_WeaponLoot_C";
        if (name == STR("BP_ItemContainerLayout_Commode_C")) return "BP_ItemContainerLayout_Commode_C";
        if (name == STR("BP_ItemContainerLayout_Table_C")) return "BP_ItemContainerLayout_Table_C";
        if (name == STR("BP_ItemContainerLayout_Wardrobe_23_C")) return "BP_ItemContainerLayout_Wardrobe_23_C";
        if (name == STR("BP_PickupableItemContainer_ShelfLoot_C")) return "BP_PickupableItemContainer_ShelfLoot_C";
        if (name == STR("BP_PickupableItemContainer_VovaInvalid_C")) return "BP_PickupableItemContainer_VovaInvalid_C";
        if (name == STR("BP_PickupableItemContainer_Vova_C")) return "BP_PickupableItemContainer_Vova_C";
        if (name == STR("BP_PickupableItemContainer_VovaBlack_C")) return "BP_PickupableItemContainer_VovaBlack_C";
        return {};
    }

    auto is_corpse_source_name(std::string_view source_class) -> bool
    {
        return source_class == "BP_VovCharacter_Invalid_C" || source_class == "BP_VovCharacter_Invalid1_C" ||
               source_class == "BP_VovCharacter_Invalid2_C" || source_class == "BP_VovCharacter_Invalid3_C" ||
               source_class == "BP_VovCharacter_C" || source_class == "BP_VovCharacter_Fast_C";
    }

    auto catalog_item_data_name(UObject* item_asset) -> std::string_view
    {
        if (!item_asset) return {};
        const auto name = item_asset->GetName();
        if (name == STR("DA_Item_PistolAmmo")) return "DA_Item_PistolAmmo";
        if (name == STR("DA_Item_ShotgunAmmo")) return "DA_Item_ShotgunAmmo";
        if (name == STR("DA_Item_AK47Ammo")) return "DA_Item_AK47Ammo";
        if (name == STR("DA_Item_Jelly_Resource")) return "DA_Item_Jelly_Resource";
        if (name == STR("DA_Item_Resources_Metal_Parts")) return "DA_Item_Resources_Metal_Parts";
        if (name == STR("DA_Item_Synthetic_Material_Resource")) return "DA_Item_Synthetic_Material_Resource";
        if (name == STR("DA_Item_Resource_Biomaterial")) return "DA_Item_Resource_Biomaterial";
        if (name == STR("DA_Item_Resources_Chemicals")) return "DA_Item_Resources_Chemicals";
        if (name == STR("DA_Item_Resource_Superconductor")) return "DA_Item_Resource_Superconductor";
        if (name == STR("DA_Item_Resource_Microelectronics")) return "DA_Item_Resource_Microelectronics";
        if (name == STR("DA_Item_Energy_Cell_Resource")) return "DA_Item_Energy_Cell_Resource";
        if (name == STR("DA_Consumable_aid_small")) return "DA_Consumable_aid_small";
        if (name == STR("DA_Consumable_aid_medium")) return "DA_Consumable_aid_medium";
        if (name == STR("DA_Consumable_aid_big")) return "DA_Consumable_aid_big";
        if (name == STR("DA_Consumable_sguschenka")) return "DA_Consumable_sguschenka";
        if (name == STR("DA_Cassette_Electricity_Recipe")) return "DA_Cassette_Electricity_Recipe";
        if (name == STR("DA_Item_Pashtet_Recipe")) return "DA_Item_Pashtet_Recipe";
        if (name == STR("DA_Item_Cassette_FIre")) return "DA_Item_Cassette_FIre";
        return {};
    }

    auto catalog_pickup_name(UClass* item_class) -> std::string_view
    {
        if (!item_class) return {};
        const auto name = item_class->GetName();
        if (name == STR("BP_Pickable_Ammo_Pistol_C")) return "BP_Pickable_Ammo_Pistol_C";
        if (name == STR("BP_Pickupable_Shotgun_Ammo_single_C")) return "BP_Pickupable_Shotgun_Ammo_single_C";
        if (name == STR("BP_Pickupable_Shotgun_Ammo_Magazine_C")) return "BP_Pickupable_Shotgun_Ammo_Magazine_C";
        if (name == STR("BP_Pickupable_AK47_Ammo_Magazine_C")) return "BP_Pickupable_AK47_Ammo_Magazine_C";
        if (name == STR("BP_Pickable_CapsuleJelly_C")) return "BP_Pickable_CapsuleJelly_C";
        if (name == STR("BP_Pickupable_Metal_Parts_C")) return "BP_Pickupable_Metal_Parts_C";
        if (name == STR("BP_Pickable_Synthetic_Material_Resource_C")) return "BP_Pickable_Synthetic_Material_Resource_C";
        if (name == STR("BP_Pickable_Resource_Biomaterial_C")) return "BP_Pickable_Resource_Biomaterial_C";
        if (name == STR("BP_Pickupable_Resources_Chemicals_C")) return "BP_Pickupable_Resources_Chemicals_C";
        if (name == STR("BP_Pickupable_Resource_Superconductor_C")) return "BP_Pickupable_Resource_Superconductor_C";
        if (name == STR("BP_Pickupable_Resource_Microelectronics_C")) return "BP_Pickupable_Resource_Microelectronics_C";
        if (name == STR("BP_Pickupable_Energy_Cell_Resource_C")) return "BP_Pickupable_Energy_Cell_Resource_C";
        if (name == STR("BP_Pickable_consumable_aid_small_C")) return "BP_Pickable_consumable_aid_small_C";
        if (name == STR("BP_Pickable_consumable_aid_medium_C")) return "BP_Pickable_consumable_aid_medium_C";
        if (name == STR("BP_Pickable_consumable_aid_big_C")) return "BP_Pickable_consumable_aid_big_C";
        if (name == STR("BP_Pickable_sguschenka_C")) return "BP_Pickable_sguschenka_C";
        if (name == STR("BP_Pickupable_Sguschenka_C")) return "BP_Pickupable_Sguschenka_C";
        if (name == STR("BP_Pickable_Cassette_Electricity_Recipe_C")) return "BP_Pickable_Cassette_Electricity_Recipe_C";
        if (name == STR("BP_Pickable_Cassette_Fire_C")) return "BP_Pickable_Cassette_Fire_C";
        return {};
    }

    auto ue_name_from_view(std::string_view text) -> StringType
    {
        StringType out;
        out.reserve(text.size());
        for (const unsigned char ch : text) out.push_back(static_cast<TCHAR>(ch));
        return out;
    }

    auto find_catalog_pickup_class(std::string_view pickup_name, UClass* pickup_base_class) -> UClass*
    {
        if (pickup_name.empty() || !pickup_base_class) return nullptr;
        const auto wanted = ue_name_from_view(pickup_name);
        if (auto* found = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, wanted.c_str(), false))
        {
            if (found->IsChildOf(pickup_base_class) && !catalog_pickup_name(found).empty()) return found;
        }
        const int32 count = UObjectArray::GetNumElements();
        for (int32 index = 0; index < count; ++index)
        {
            auto* item = UObjectArray::IndexToObject(index);
            if (!item || !item->IsValid(false)) continue;
            auto* object = item->GetUObject();
            if (!object || !object->IsA(UClass::StaticClass())) continue;
            auto* candidate = static_cast<UClass*>(object);
            if (candidate->GetName() != wanted || !candidate->IsChildOf(pickup_base_class) ||
                catalog_pickup_name(candidate).empty())
            {
                continue;
            }
            return candidate;
        }
        return nullptr;
    }

    auto catalog_descriptor(const AcquisitionTarget& target) -> autoloot::post7c3::Descriptor
    {
        return {
            std::string{catalog_source_name(target.source)},
            std::string{catalog_layout_name(target.layout)},
            std::string{catalog_pickup_name(target.loaded_class)},
            std::string{catalog_item_data_name(target.item_asset)},
            target.amount,
            target.record_fingerprint,
        };
    }

    auto runtime_target_supported(const AcquisitionTarget& target) -> bool
    {
        return target.source && target.layout && target.item_asset && target.loaded_class && target.amount > 0 &&
               !catalog_source_name(target.source).empty() && !catalog_layout_name(target.layout).empty() &&
               autoloot::post7c3::layout_supported(catalog_source_name(target.source), catalog_layout_name(target.layout));
    }

    auto require_class(const TCHAR* role, const TCHAR* path) -> UClass*
    {
        auto* value = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, path, false);
        if (!value)
        {
            emit_log(STR("[AutoLootNativeProbe] MISSING role={} kind=class path={}\n"), role, path);
        }
        return value;
    }

    template <typename PropertyType>
    auto require_property(UObject* owner, const TCHAR* owner_role, const TCHAR* name) -> PropertyType*
    {
        auto* property = owner ? CastField<PropertyType>(owner->GetPropertyByNameInChain(name)) : nullptr;
        if (!property)
        {
            emit_log(STR("[AutoLootNativeProbe] MISSING role={} kind=property name={}\n"), owner_role, name);
        }
        return property;
    }

    auto read_object(FObjectPropertyBase* property, void* container) -> UObject*
    {
        return property ? property->GetObjectPropertyValue(property->ContainerPtrToValuePtr<void>(container)) : nullptr;
    }

    auto read_int(FIntProperty* property, void* container) -> int32
    {
        return property ? property->GetPropertyValueInContainer(container) : 0;
    }

    auto read_actor_item(UObject* actor, UObject*& item_asset, int32& amount) -> bool
    {
        // Silent: absence of Item is normal for non-pickup UObjects in the scan hot path.
        auto* item_property = actor ? CastField<FStructProperty>(actor->GetPropertyByNameInChain(STR("Item"))) : nullptr;
        if (!item_property || !item_property->GetStruct())
        {
            return false;
        }
        auto* item_struct = item_property->GetStruct().Get();
        auto* amount_property = item_struct ? CastField<FIntProperty>(item_struct->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
        auto* asset_property =
            item_struct ? CastField<FObjectPropertyBase>(item_struct->GetPropertyByNameInChain(STR("ItemDataAsset"))) : nullptr;
        if (!amount_property || !asset_property)
        {
            return false;
        }
        void* item_data = item_property->ContainerPtrToValuePtr<void>(actor);
        amount = read_int(amount_property, item_data);
        item_asset = read_object(asset_property, item_data);
        return item_asset != nullptr && amount > 0;
    }

    // World pickups are admitted by concrete pickup class OR by catalog ItemData on
    // AHPickupableItem.Item (2026-08-04 F11: Shotgun Magazine uses DA_Item_ShotgunAmmo).
    auto world_pickup_admissible(UObject* pickup) -> bool
    {
        if (!pickup || !pickup->GetClassPrivate()) return false;
        auto* pickup_class = static_cast<UClass*>(pickup->GetClassPrivate());
        if (!catalog_pickup_name(pickup_class).empty()) return true;
        // ItemData fallback only for AHPickupableItem children — never probe every UObject.
        static UClass* pickup_base = nullptr;
        if (!pickup_base)
        {
            pickup_base = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, PickupBaseClassPath, false);
            if (!pickup_base) return false;
        }
        if (!pickup_class->IsChildOf(pickup_base)) return false;
        UObject* item_asset{};
        int32 amount{};
        if (!read_actor_item(pickup, item_asset, amount)) return false;
        const auto inventory_data = catalog_item_data_name(item_asset);
        return !inventory_data.empty() && autoloot::post7c3::item_data_supported(inventory_data, amount);
    }

    auto read_source_state(UObject* source, bool& looted, int32& shelves) -> bool
    {
        auto* looted_property = CastField<FBoolProperty>(source ? source->GetPropertyByNameInChain(STR("bLooted")) : nullptr);
        auto* shelves_property = CastField<FArrayProperty>(source ? source->GetPropertyByNameInChain(STR("SavedItemsInShelves")) : nullptr);
        if (!looted_property || !shelves_property)
        {
            return false;
        }
        looted = looted_property->GetPropertyValueInContainer(source);
        shelves = FScriptArrayHelper_InContainer{shelves_property, source}.Num();
        return true;
    }

    auto get_loot_box_component(UObject* source) -> UObject*
    {
        auto* property = CastField<FObjectPropertyBase>(source ? source->GetPropertyByNameInChain(STR("LootBoxComponent")) : nullptr);
        return property ? read_object(property, source) : nullptr;
    }

    // R47 corpse AFTER: depleted means LootBoxComponent.SavedItems == 0 (no bLooted on Vov actors).
    auto read_corpse_source_state(UObject* source, UObject* loot_component, bool& looted, int32& shelves) -> bool
    {
        auto* component = loot_component ? loot_component : get_loot_box_component(source);
        auto* saved_property = CastField<FArrayProperty>(component ? component->GetPropertyByNameInChain(STR("SavedItems")) : nullptr);
        if (!component || !saved_property) return false;
        shelves = FScriptArrayHelper_InContainer{saved_property, component}.Num();
        looted = shelves == 0;
        return true;
    }

    auto read_layout_count(UObject* layout, int32& count) -> bool
    {
        auto* attached_property = require_property<FArrayProperty>(layout, STR("layout"), STR("AttachedItems"));
        if (!attached_property)
        {
            return false;
        }
        count = FScriptArrayHelper_InContainer{attached_property, layout}.Num();
        return true;
    }

    auto remove_script_array_element(FArrayProperty* array_property, UObject* owner, int32 index, const TCHAR* role) -> bool
    {
        if (!array_property || !owner || !array_property->GetInner()) return false;
        FScriptArrayHelper_InContainer helper{array_property, owner};
        if (index < 0 || index >= helper.Num()) return false;

        const bool uses_memory_image_allocator = !!(array_property->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator);
        auto* array_header = static_cast<ScriptArrayHeader*>(array_property->ContainerPtrToValuePtr<void>(owner));
        const int32 element_size = array_property->GetInner()->GetElementSize();
        if (uses_memory_image_allocator || !array_header || array_header->data != helper.GetRawPtr(0) || array_header->num != helper.Num() ||
            array_header->max < array_header->num || element_size <= 0)
        {
            emit_log(STR("[AutoLootNativeProbe] ARRAY_REMOVE_FAIL role={} reason=header memory_image={} num={} index={}\n"),
                     role, uses_memory_image_allocator, helper.Num(), index);
            return false;
        }

        void* target = helper.GetRawPtr(index);
        array_property->GetInner()->DestroyValue(target);
        if (index < array_header->num - 1)
        {
            std::memmove(static_cast<uint8*>(array_header->data) + static_cast<size_t>(index) * static_cast<size_t>(element_size),
                         static_cast<uint8*>(array_header->data) + static_cast<size_t>(index + 1) * static_cast<size_t>(element_size),
                         static_cast<size_t>(array_header->num - index - 1) * static_cast<size_t>(element_size));
        }
        --array_header->num;
        std::memset(static_cast<uint8*>(array_header->data) + static_cast<size_t>(array_header->num) * static_cast<size_t>(element_size), 0,
                    static_cast<size_t>(element_size));
        return helper.Num() == array_header->num;
    }

    auto clear_script_array(FArrayProperty* array_property, UObject* owner, const TCHAR* role) -> bool
    {
        if (!array_property || !owner) return false;
        while (FScriptArrayHelper_InContainer{array_property, owner}.Num() > 0)
        {
            const int32 last = FScriptArrayHelper_InContainer{array_property, owner}.Num() - 1;
            if (!remove_script_array_element(array_property, owner, last, role)) return false;
        }
        return true;
    }

    // FInventoryData contains TSoftClassPtr — do not memmove/memset destroyed soft paths.
    // Empty by destroying the last element only (no relocate), matching UE Empty without shrink.
    auto empty_inventory_data_array(FArrayProperty* array_property, UObject* owner) -> bool
    {
        if (!array_property || !owner || !array_property->GetInner()) return false;
        const bool uses_memory_image_allocator = !!(array_property->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator);
        auto* array_header = static_cast<ScriptArrayHeader*>(array_property->ContainerPtrToValuePtr<void>(owner));
        FScriptArrayHelper_InContainer helper{array_property, owner};
        if (uses_memory_image_allocator || !array_header ||
            (array_header->num > 0 && array_header->data != helper.GetRawPtr(0)) || array_header->num != helper.Num() ||
            array_header->max < array_header->num)
        {
            emit_log(STR("[AutoLootNativeProbe] INVENTORY_DATA_EMPTY_FAIL reason=header memory_image={} num={}\n"),
                     uses_memory_image_allocator, helper.Num());
            return false;
        }
        while (array_header->num > 0)
        {
            const int32 last = array_header->num - 1;
            array_property->GetInner()->DestroyValue(helper.GetRawPtr(last));
            --array_header->num;
        }
        return array_header->num == 0;
    }

    auto return_actor_to_pool(UObject* subsystem, UObject* actor) -> bool;

    auto read_actor_hidden(UObject* actor, bool& hidden) -> bool
    {
        auto* hidden_property = actor ? CastField<FBoolProperty>(actor->GetPropertyByNameInChain(STR("bHidden"))) : nullptr;
        if (!hidden_property) return false;
        hidden = hidden_property->GetPropertyValueInContainer(actor);
        return true;
    }

    // Confirmed R47: ReturnItemToPool leaves world ammo alive with GrabComponent grabable.
    // ProcessEvent hide path used wrong param name (NewHidden vs bNewHidden) and always failed.
    auto deactivate_pickup_actor(UObject* pickup, bool& hidden_after, bool& collision_after) -> bool
    {
        hidden_after = false;
        collision_after = true;
        if (!pickup) return false;
        auto* actor = static_cast<AActor*>(pickup);
        actor->SetActorHiddenInGame(true);
        actor->SetActorEnableCollision(false);
        const bool hidden_ok = read_actor_hidden(pickup, hidden_after);
        collision_after = actor->GetActorEnableCollision();
        return hidden_ok && hidden_after && !collision_after;
    }

    auto cleanup_collected_pickup(UObject* pool, UObject* pickup, bool& pooled, bool& deactivated, bool& destroyed) -> bool
    {
        pooled = false;
        deactivated = false;
        destroyed = false;
        if (!pickup) return false;

        FWeakObjectPtr weak{pickup};
        pooled = return_actor_to_pool(pool, pickup);

        auto* still = weak.Get();
        bool hidden_post_pool{};
        const bool read_hidden_post_pool = still && read_actor_hidden(still, hidden_post_pool);
        const bool collision_post_pool = still ? static_cast<AActor*>(still)->GetActorEnableCollision() : false;
        emit_log(STR("[AutoLootNativeProbe] POST_POOL still_known={} pooled={} bHidden={} collision={} object={}\n"),
                 still != nullptr, pooled, read_hidden_post_pool ? (hidden_post_pool ? 1 : 0) : -1,
                 still ? (collision_post_pool ? 1 : 0) : -1, exact_name(pickup));

        if (still)
        {
            bool hidden_after{};
            bool collision_after = true;
            deactivated = deactivate_pickup_actor(still, hidden_after, collision_after);
            still = weak.Get();
            // 2026-08-04 R53: ContinuousPickup keeps GrabbedActor on map world pickups that only
            // got pool+deactivate (Energy_Cell / Magazine), then re-enters AddItemToInventory.
            // Always destroy the live actor after inventory so grab cannot reuse it.
            if (still)
            {
                static_cast<AActor*>(still)->K2_DestroyActor();
                destroyed = true;
                still = weak.Get();
            }
        }

        const bool gone = weak.Get() == nullptr;
        bool hidden_final{};
        const bool read_hidden_final = !gone && read_actor_hidden(weak.Get(), hidden_final);
        const bool collision_final = !gone && static_cast<AActor*>(weak.Get())->GetActorEnableCollision();
        const bool cleanup_ok = gone || (read_hidden_final && hidden_final && !collision_final);
        emit_log(STR("[AutoLootNativeProbe] POST_CLEANUP ok={} gone={} destroyed={} deactivated={} bHidden={} collision={}\n"),
                 cleanup_ok, gone, destroyed, deactivated, read_hidden_final ? (hidden_final ? 1 : 0) : -1,
                 gone ? -1 : (collision_final ? 1 : 0));
        return cleanup_ok;
    }

    auto collect_floor_aid(UObject* pickup, const StringType& identity, UObject* inventory, UObject* pool) -> bool
    {
        if (!pickup) return false;

        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, AddInventoryPath, false);
        auto* item_param = require_property<FObjectPropertyBase>(function, STR("floor_aid.AddItemToInventory"), STR("InPickupableItem"));
        auto* notify_param = require_property<FBoolProperty>(function, STR("floor_aid.AddItemToInventory"), STR("InSendNotification"));
        if (!inventory || !function || !item_param || !notify_param || !pool || function->GetParmsSize() == 0 ||
            function->GetParmsSize() > 256)
        {
            emit_log(STR("[AutoLootNativeProbe] FLOOR_PICKUP result=FAIL reason=inventory_contract object={}\n"), exact_name(pickup));
            return false;
        }
        std::vector<uint8> params(function->GetParmsSize(), 0);
        item_param->SetObjectPropertyValue(item_param->ContainerPtrToValuePtr<void>(params.data()), pickup);
        notify_param->SetPropertyValueInContainer(params.data(), true);
        const auto class_name = pickup->GetClassPrivate()->GetName();
        const int32 pickup_index = pickup->GetInternalIndex();
        inventory->ProcessEvent(function, params.data());

        // 2026-08-04 sguschenka: ContinuousPickup re-grabs map actors that only got
        // pool+deactivate. Prefer destroy when the actor remains after inventory.
        bool pooled = false;
        bool deactivated = false;
        bool destroyed = false;
        bool cleanup_ok = cleanup_collected_pickup(pool, pickup, pooled, deactivated, destroyed);
        auto* still_item = pickup_index >= 0 && pickup_index < UObjectArray::GetNumElements()
                               ? UObjectArray::IndexToObject(pickup_index)
                               : nullptr;
        auto* still = still_item && still_item->IsValid(false) ? still_item->GetUObject() : nullptr;
        if (still == pickup)
        {
            static_cast<AActor*>(pickup)->K2_DestroyActor();
            destroyed = true;
            still_item = pickup_index >= 0 && pickup_index < UObjectArray::GetNumElements()
                             ? UObjectArray::IndexToObject(pickup_index)
                             : nullptr;
            still = still_item && still_item->IsValid(false) ? still_item->GetUObject() : nullptr;
            cleanup_ok = still != pickup;
        }
        if (cleanup_ok) ProcessedFloorPickups.push_back(identity);
        emit_log(STR("[AutoLootNativeProbe] FLOOR_PICKUP result={} class={} object={} notification=true pooled={} deactivated={} destroyed={} cleanup_ok={}\n"),
                     cleanup_ok, class_name, identity, pooled, deactivated, destroyed, cleanup_ok);
        return cleanup_ok;
    }

    // R47 corpse commit surface: ContinuousPickup grabs SpawnedItem → AddItem/ReturnToPool.
    // AHLootBoxBase removal never fires. Clear LootBoxComponent.SavedItems + container.AttachedItems
    // (and drop empty AttachedItemContainers entry) without that callback.
    auto invoke_corpse_removal(const AcquisitionTarget& target) -> bool
    {
        if (!target.corpse_surface || !target.source || !target.layout || !target.loot_component) return false;

        auto* attached_property = CastField<FArrayProperty>(target.layout->GetPropertyByNameInChain(STR("AttachedItems")));
        auto* saved_property = CastField<FArrayProperty>(target.loot_component->GetPropertyByNameInChain(STR("SavedItems")));
        auto* containers_property = CastField<FArrayProperty>(target.loot_component->GetPropertyByNameInChain(STR("AttachedItemContainers")));
        auto* container_inner = containers_property ? CastField<FObjectPropertyBase>(containers_property->GetInner()) : nullptr;
        if (!attached_property || !saved_property || !containers_property || !container_inner) return false;

        FScriptArrayHelper_InContainer attached{attached_property, target.layout};
        FScriptArrayHelper_InContainer saved{saved_property, target.loot_component};
        if (attached.Num() != target.attached_before || target.shelf_index < 0 || target.shelf_index >= attached.Num() ||
            target.source_index < 0 || target.source_index >= saved.Num())
        {
            emit_log(STR("[AutoLootNativeProbe] CORPSE_REMOVE_FAIL reason=index attached={} saved={} shelf={} source_index={}\n"),
                     attached.Num(), saved.Num(), target.shelf_index, target.source_index);
            return false;
        }

        if (!remove_script_array_element(attached_property, target.layout, target.shelf_index, STR("corpse.AttachedItems")))
        {
            return false;
        }
        if (!remove_script_array_element(saved_property, target.loot_component, target.source_index, STR("corpse.SavedItems")))
        {
            return false;
        }

        FScriptArrayHelper_InContainer attached_after{attached_property, target.layout};
        if (attached_after.Num() == 0)
        {
            FScriptArrayHelper_InContainer containers{containers_property, target.loot_component};
            int32 container_index = -1;
            for (int32 index = 0; index < containers.Num(); ++index)
            {
                if (read_object(container_inner, containers.GetRawPtr(index)) == target.layout)
                {
                    container_index = index;
                    break;
                }
            }
            if (container_index >= 0 &&
                !remove_script_array_element(containers_property, target.loot_component, container_index, STR("corpse.AttachedItemContainers")))
            {
                return false;
            }
        }

        bool looted{};
        int32 shelves{};
        int32 attached_count{};
        const bool ok = read_corpse_source_state(target.source, target.loot_component, looted, shelves) &&
                        shelves == target.shelves_before - 1 && read_layout_count(target.layout, attached_count) &&
                        attached_count == target.attached_before - 1;
        // 2026-08-04 F11 after auto-loot: SavedItems=0/bIsEnabled=false still left
        // LootBoxComponent.InventoryData with one residual entry and the corpse stayed
        // ALT-highlighted. Clear InventoryData when depleted.
        if (ok && shelves == 0)
        {
            if (auto* enabled_property =
                    CastField<FBoolProperty>(target.loot_component->GetPropertyByNameInChain(STR("bIsEnabled"))))
            {
                enabled_property->SetPropertyValueInContainer(target.loot_component, false);
                emit_log(STR("[AutoLootNativeProbe] CORPSE_DISABLE_SCANNER source={} bIsEnabled=false shelves=0\n"),
                         exact_name(target.source));
            }
            if (auto* inventory_data_property =
                    CastField<FArrayProperty>(target.loot_component->GetPropertyByNameInChain(STR("InventoryData"))))
            {
                const int32 inventory_before = FScriptArrayHelper_InContainer{inventory_data_property, target.loot_component}.Num();
                if (inventory_before > 0)
                {
                    // Soft-class-safe empty: DestroyValue last-only, no memmove/memset of FInventoryData.
                    const bool cleared = empty_inventory_data_array(inventory_data_property, target.loot_component);
                    const int32 inventory_after = FScriptArrayHelper_InContainer{inventory_data_property, target.loot_component}.Num();
                    emit_log(STR("[AutoLootNativeProbe] CORPSE_CLEAR_INVENTORY_DATA source={} result={} before={} after={}\n"),
                             exact_name(target.source), cleared, inventory_before, inventory_after);
                }
            }
        }
        emit_log(STR("[AutoLootNativeProbe] CORPSE_REMOVE result={} shelves_before={} shelves_after={} attached_before={} attached_after={} looted={}\n"),
                 ok, target.shelves_before, shelves, target.attached_before, attached_count, looted);
        return ok;
    }

    auto invoke_source_removal(const AcquisitionTarget& target) -> bool
    {
        if (target.corpse_surface) return invoke_corpse_removal(target);
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, RemoveSourceItemPath, false);
        auto* container_param = require_property<FObjectPropertyBase>(function, STR("OnContainerRemovedItem"), STR("Container"));
        auto* info_param = require_property<FStructProperty>(function, STR("OnContainerRemovedItem"), STR("ItemSpawnInfo"));
        auto* shelf_param = require_property<FIntProperty>(function, STR("OnContainerRemovedItem"), STR("ShelfIndex"));
        auto* info_type = info_param && info_param->GetStruct() ? info_param->GetStruct().Get() : nullptr;
        auto* item_class_field = require_property<FObjectPropertyBase>(info_type, STR("OnContainerRemovedItem.ItemSpawnInfo"), STR("ItemClass"));
        auto* amount_field = require_property<FIntProperty>(info_type, STR("OnContainerRemovedItem.ItemSpawnInfo"), STR("Amount"));
        if (!function || !container_param || !info_param || !shelf_param || !info_type || !item_class_field || !amount_field ||
            function->GetParmsSize() != 28 || info_param->GetSize() != 16)
        {
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=contract function={} parms_size={} info_type={} info_size={} item_class_field={} amount_field={}\n"),
                         function != nullptr, function ? function->GetParmsSize() : 0, exact_name(info_type), info_param ? info_param->GetSize() : 0,
                         item_class_field != nullptr, amount_field != nullptr);
            return false;
        }

        std::vector<uint8> params(function->GetParmsSize(), 0);
        container_param->SetObjectPropertyValue(container_param->ContainerPtrToValuePtr<void>(params.data()), target.layout);
        shelf_param->SetPropertyValueInContainer(params.data(), target.shelf_index);
        void* info_value = info_param->ContainerPtrToValuePtr<void>(params.data());
        info_param->InitializeValue(info_value);
        item_class_field->SetObjectPropertyValue(item_class_field->ContainerPtrToValuePtr<void>(info_value), target.item_asset);
        amount_field->SetPropertyValueInContainer(info_value, target.amount);

        if (target.attached_before == 0)
        {
            // 2026-08-04 UE4SS: OnContainerRemovedItem alone does not shrink SavedItemsInShelves when
            // AttachedItems is already empty (Wardrobe_24 shotgun / Table synthetic). The callback was
            // retried in a spawn/destroy loop until the process died mid-call. Mirror the live path's
            // array surgery on SavedItemsInShelves; skip the empty-attached callback.
            int32 attached_now{};
            if (!read_layout_count(target.layout, attached_now) || attached_now != 0)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=saved_only_attached_not_empty attached={}\n"), attached_now);
                info_param->DestroyValue(info_value);
                return false;
            }
            auto* source_items_property = require_property<FArrayProperty>(target.source, STR("saved_only"), STR("SavedItemsInShelves"));
            auto* source_inner = source_items_property ? CastField<FStructProperty>(source_items_property->GetInner()) : nullptr;
            auto* source_type = source_inner && source_inner->GetStruct() ? source_inner->GetStruct().Get() : nullptr;
            auto* source_amount_property = require_property<FIntProperty>(source_type, STR("saved_only.item"), STR("Amount"));
            auto* source_asset_property = require_property<FObjectPropertyBase>(source_type, STR("saved_only.item"), STR("ItemClass"));
            if (!source_items_property || !source_amount_property || !source_asset_property)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=saved_only_source_contract\n"));
                info_param->DestroyValue(info_value);
                return false;
            }
            FScriptArrayHelper_InContainer source_items{source_items_property, target.source};
            if (target.source_index < 0 || target.source_index >= source_items.Num() || source_items.Num() != target.shelves_before)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=saved_only_index index={} count={} shelves_before={}\n"),
                         target.source_index, source_items.Num(), target.shelves_before);
                info_param->DestroyValue(info_value);
                return false;
            }
            void* source_entry = source_items.GetRawPtr(target.source_index);
            if (read_object(source_asset_property, source_entry) != target.item_asset ||
                read_int(source_amount_property, source_entry) != target.amount)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=saved_only_entry_mismatch item_asset={} amount={}\n"),
                         exact_name(read_object(source_asset_property, source_entry)), read_int(source_amount_property, source_entry));
                info_param->DestroyValue(info_value);
                return false;
            }
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_SAVED_ONLY path=SavedItemsInShelves container={} source_index={} item_asset={} amount={}\n"),
                     exact_name(target.layout), target.source_index, exact_name(target.item_asset), target.amount);
            const bool removed = remove_script_array_element(source_items_property, target.source, target.source_index, STR("saved_only.SavedItemsInShelves"));
            info_param->DestroyValue(info_value);
            if (!removed)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=saved_only_array_remove\n"));
                return false;
            }
            return true;
        }

        auto* attached_property = require_property<FArrayProperty>(target.layout, STR("layout"), STR("AttachedItems"));
        auto* attached_inner = attached_property ? CastField<FStructProperty>(attached_property->GetInner()) : nullptr;
        if (!attached_property || !attached_inner || !attached_inner->GetStruct())
        {
            info_param->DestroyValue(info_value);
            return false;
        }
        FScriptArrayHelper_InContainer attached{attached_property, target.layout};
        if (attached.Num() != target.attached_before)
        {
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=attached_count count={} expected={}\n"), attached.Num(), target.attached_before);
            info_param->DestroyValue(info_value);
            return false;
        }
        auto* attached_type = attached_inner->GetStruct().Get();
        auto* attached_item_field = require_property<FObjectPropertyBase>(attached_type, STR("AttachedItems.inner"), STR("ItemClass"));
        auto* attached_amount_field = require_property<FIntProperty>(attached_type, STR("AttachedItems.inner"), STR("Amount"));
        if (target.shelf_index < 0 || target.shelf_index >= attached.Num())
        {
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=shelf_index index={} count={}\n"), target.shelf_index, attached.Num());
            info_param->DestroyValue(info_value);
            return false;
        }
        void* attached_target = attached.GetRawPtr(target.shelf_index);
        if (!attached_item_field || !attached_amount_field || read_object(attached_item_field, attached_target) != target.item_asset ||
            read_int(attached_amount_field, attached_target) != target.amount)
        {
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=attached_target_mismatch item_asset={} amount={}\n"),
                         exact_name(attached_item_field ? read_object(attached_item_field, attached_target) : nullptr),
                         attached_amount_field ? read_int(attached_amount_field, attached_target) : -1);
            info_param->DestroyValue(info_value);
            return false;
        }

        const bool uses_memory_image_allocator = !!(attached_property->GetArrayFlags() & EArrayPropertyFlags::UsesMemoryImageAllocator);
        auto* array_header = static_cast<ScriptArrayHeader*>(attached_property->ContainerPtrToValuePtr<void>(target.layout));
        const int32 element_size = attached_property->GetInner()->GetElementSize();
        if (uses_memory_image_allocator || !array_header || array_header->data != attached.GetRawPtr(0) || array_header->num != attached.Num() ||
            array_header->max < array_header->num || element_size != attached_inner->GetSize())
        {
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=array_header_contract memory_image={} data_match={} num={} max={} helper_num={} element_size={} struct_size={}\n"),
                         uses_memory_image_allocator, array_header && array_header->data == attached.GetRawPtr(0), array_header ? array_header->num : -1,
                         array_header ? array_header->max : -1, attached.Num(), element_size, attached_inner->GetSize());
            info_param->DestroyValue(info_value);
            return false;
        }
        attached_property->GetInner()->DestroyValue(attached_target);
        if (target.shelf_index < array_header->num - 1)
        {
            std::memmove(static_cast<uint8*>(array_header->data) + static_cast<size_t>(target.shelf_index) * static_cast<size_t>(element_size),
                         static_cast<uint8*>(array_header->data) + static_cast<size_t>(target.shelf_index + 1) * static_cast<size_t>(element_size),
                         static_cast<size_t>(array_header->num - target.shelf_index - 1) * static_cast<size_t>(element_size));
        }
        --array_header->num;
        std::memset(static_cast<uint8*>(array_header->data) + static_cast<size_t>(array_header->num) * static_cast<size_t>(element_size), 0,
                    static_cast<size_t>(element_size));
        const bool live_removed = attached.Num() == target.attached_before - 1;
        if constexpr (VerboseDiagnostics)
        {
            emit_log(STR("[AutoLootNativeProbe] LIVE_SOURCE_REMOVE result={} field=AttachedItems index={} count_before={} count_after={} source_array_writes=1\n"),
                         live_removed, target.shelf_index, target.attached_before, attached.Num());
        }
        if (!live_removed)
        {
            info_param->DestroyValue(info_value);
            return false;
        }
        if constexpr (VerboseDiagnostics)
        {
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_BEGIN path={} container={} shelf_index={} info_type={} info_size={} item_asset={} amount={}\n"),
                         RemoveSourceItemPath, exact_name(target.layout), target.shelf_index, info_type->GetFullName(), info_param->GetSize(), exact_name(target.item_asset), target.amount);
        }
        target.source->ProcessEvent(function, params.data());
        info_param->DestroyValue(info_value);

        // 2026-08-04 UE4SS: after live AttachedItems surgery, OnContainerRemovedItem sometimes
        // leaves SavedItemsInShelves unchanged while wiping the rest of AttachedItems
        // (FAIL source_removal_invariants shelves_before=N shelves_after=N attached_after=0).
        // Recover the SavedItems row surgically so the item is not stranded.
        bool looted_after{};
        int32 shelves_after{};
        if (!read_source_state(target.source, looted_after, shelves_after) ||
            shelves_after != target.shelves_before - 1)
        {
            auto* source_items_property = CastField<FArrayProperty>(target.source->GetPropertyByNameInChain(STR("SavedItemsInShelves")));
            auto* source_inner = source_items_property ? CastField<FStructProperty>(source_items_property->GetInner()) : nullptr;
            auto* source_type = source_inner && source_inner->GetStruct() ? source_inner->GetStruct().Get() : nullptr;
            auto* source_amount_property = source_type ? CastField<FIntProperty>(source_type->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
            auto* source_asset_property =
                source_type ? CastField<FObjectPropertyBase>(source_type->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
            if (!source_items_property || !source_amount_property || !source_asset_property)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=post_callback_saved_contract shelves={}\n"), shelves_after);
                return false;
            }
            FScriptArrayHelper_InContainer source_items{source_items_property, target.source};
            if (target.source_index < 0 || target.source_index >= source_items.Num())
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=post_callback_saved_index index={} count={} shelves={}\n"),
                         target.source_index, source_items.Num(), shelves_after);
                return false;
            }
            void* source_entry = source_items.GetRawPtr(target.source_index);
            if (read_object(source_asset_property, source_entry) != target.item_asset ||
                read_int(source_amount_property, source_entry) != target.amount)
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=post_callback_saved_mismatch item_asset={} amount={} shelves={}\n"),
                         exact_name(read_object(source_asset_property, source_entry)),
                         read_int(source_amount_property, source_entry),
                         shelves_after);
                return false;
            }
            emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_RECOVER_SAVED path=SavedItemsInShelves container={} source_index={} item_asset={} amount={} shelves_before_callback={}\n"),
                     exact_name(target.layout), target.source_index, exact_name(target.item_asset), target.amount, shelves_after);
            if (!remove_script_array_element(source_items_property, target.source, target.source_index, STR("recover.SavedItemsInShelves")))
            {
                emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_FAIL reason=post_callback_saved_array_remove\n"));
                return false;
            }
        }
        if constexpr (VerboseDiagnostics) emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_END call_count=1\n"));
        return true;
    }

    auto invoke_container_purge(UObject* source, UObject* layout, int32 expected_shelves) -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, PurgeContainerPath, false);
        auto* container_param = require_property<FObjectPropertyBase>(function, STR("OnContainerPurged"), STR("Container"));
        if (!source || !layout || expected_shelves < 0 || !function || !container_param ||
            function->GetParmsSize() == 0 || function->GetParmsSize() > 64)
        {
            emit_log(STR("[AutoLootNativeProbe] PURGE_FAIL reason=contract function={} container_param={} parms_size={}\n"),
                     exact_name(function), container_param != nullptr, function ? function->GetParmsSize() : 0);
            return false;
        }

        int32 attached_before{};
        if (!read_layout_count(layout, attached_before) || attached_before != 0)
        {
            emit_log(STR("[AutoLootNativeProbe] PURGE_FAIL reason=layout_not_empty attached={}\n"), attached_before);
            return false;
        }

        std::vector<uint8> params(function->GetParmsSize(), 0);
        container_param->SetObjectPropertyValue(container_param->ContainerPtrToValuePtr<void>(params.data()), layout);
        source->ProcessEvent(function, params.data());

        bool looted{};
        int32 shelves{};
        int32 attached_after{};
        const bool final_layout = expected_shelves == 0;
        const bool state_ok = read_source_state(source, looted, shelves) && shelves == expected_shelves &&
                              read_layout_count(layout, attached_after) && attached_after == 0 &&
                              (final_layout ? looted : !looted);
        emit_log(STR("[AutoLootNativeProbe] PURGE_CALL result={} source={} layout={} final_layout={} bLooted={} shelves={} attached={}\n"),
                 state_ok, exact_name(source), exact_name(layout), final_layout, looted, shelves, attached_after);
        return state_ok;
    }

    auto return_actor_to_pool(UObject* subsystem, UObject* actor) -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, ReturnToPoolPath, false);
        auto* used_item = require_property<FObjectPropertyBase>(function, STR("ReturnItemToPool"), STR("UsedItem"));
        if (!subsystem || !function || !used_item || function->GetParmsSize() == 0 || function->GetParmsSize() > 64)
        {
            return false;
        }
        std::vector<uint8> params(function->GetParmsSize(), 0);
        used_item->SetObjectPropertyValue(used_item->ContainerPtrToValuePtr<void>(params.data()), actor);
        subsystem->ProcessEvent(function, params.data());
        if constexpr (VerboseDiagnostics) emit_log(STR("[AutoLootNativeProbe] POOL_RETURN path={} actor={} call_count=1\n"), ReturnToPoolPath, exact_name(actor));
        return true;
    }

    auto collect_existing_catalog_pickup(UObject* pickup, const StringType& identity, UObject* inventory, UObject* pool) -> bool
    {
        if (!pickup) return false;
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, AddInventoryPath, false);
        auto* item_param = require_property<FObjectPropertyBase>(function, STR("world_pickup.AddItemToInventory"), STR("InPickupableItem"));
        auto* notify_param = require_property<FBoolProperty>(function, STR("world_pickup.AddItemToInventory"), STR("InSendNotification"));
        if (!inventory || !function || !item_param || !notify_param || !pool || function->GetParmsSize() == 0 || function->GetParmsSize() > 256)
        {
            emit_log(STR("[AutoLootNativeProbe] WORLD_PICKUP result=FAIL reason=contract object={}\n"), exact_name(pickup));
            return false;
        }
        std::vector<uint8> params(function->GetParmsSize(), 0);
        item_param->SetObjectPropertyValue(item_param->ContainerPtrToValuePtr<void>(params.data()), pickup);
        notify_param->SetPropertyValueInContainer(params.data(), true);
        inventory->ProcessEvent(function, params.data());
        bool pooled = false;
        bool deactivated = false;
        bool destroyed = false;
        const bool cleanup_ok = cleanup_collected_pickup(pool, pickup, pooled, deactivated, destroyed);
        if (cleanup_ok) ProcessedWorldPickups.push_back(identity);
        emit_log(STR("[AutoLootNativeProbe] WORLD_PICKUP result={} object={} pooled={} deactivated={} destroyed={} cleanup_ok={} notification=true\n"),
                 cleanup_ok, identity, pooled, deactivated, destroyed, cleanup_ok);
        return cleanup_ok;
    }

    // R47 Crate2: AttachedItems can be a lazy subset of SavedItemsInShelves. Taking the last
    // live AttachedItems entry makes the game OnContainerPurged/bLooted while shelves remain.
    // Recover by clearing premature bLooted and resolving the next allowlisted SavedItems row
    // even when AttachedItems is empty (attached_before=0 transaction path).
    auto clear_premature_looted_flag(UObject* source) -> bool
    {
        bool looted{};
        int32 shelves{};
        if (!read_source_state(source, looted, shelves) || !looted || shelves <= 0) return false;
        auto* looted_property = CastField<FBoolProperty>(source->GetPropertyByNameInChain(STR("bLooted")));
        if (!looted_property) return false;
        looted_property->SetPropertyValueInContainer(source, false);
        emit_log(STR("[AutoLootNativeProbe] CLEAR_PREMATURE_LOOTED source={} shelves={}\n"), exact_name(source), shelves);
        return true;
    }

    auto resolve_saved_only_target(UObject* source,
                                   UObject* layout,
                                   AActor* actor,
                                   bool looted,
                                   int32 shelves,
                                   FArrayProperty* source_items_property,
                                   FIntProperty* source_amount_property,
                                   FObjectPropertyBase* source_asset_property,
                                   UClass* pickup_base_class) -> AcquisitionTarget
    {
        AcquisitionTarget target{};
        if (!source || !layout || !actor || !source_items_property || !source_amount_property || !source_asset_property ||
            !pickup_base_class)
        {
            return target;
        }

        FScriptArrayHelper_InContainer source_items{source_items_property, source};
        for (int32 source_index = 0; source_index < source_items.Num(); ++source_index)
        {
            auto* entry = source_items.GetRawPtr(source_index);
            auto* asset = read_object(source_asset_property, entry);
            const int32 amount = read_int(source_amount_property, entry);
            const auto inventory_data = catalog_item_data_name(asset);
            const auto pickup_name = autoloot::post7c3::item_class_for_data(inventory_data);
            if (amount <= 0 || inventory_data.empty() || pickup_name.empty() ||
                !autoloot::post7c3::item_supported(inventory_data, pickup_name, amount))
            {
                continue;
            }
            auto* loaded = find_catalog_pickup_class(pickup_name, pickup_base_class);
            if (!loaded) continue;

            int32 matched_index{-1};
            std::vector<SemanticSourceRecord> records;
            records.reserve(source_items.Num());
            for (int32 index = 0; index < source_items.Num(); ++index)
            {
                auto* record_entry = source_items.GetRawPtr(index);
                records.push_back({read_object(source_asset_property, record_entry), read_int(source_amount_property, record_entry)});
            }
            const int32 semantic_matches = count_semantic_matches(records, asset, amount, matched_index);
            if (semantic_matches != 1 || matched_index != source_index) continue;

            AcquisitionTarget candidate{};
            candidate.source = source;
            candidate.source_world = actor->GetWorld();
            candidate.source_level = actor->GetLevel();
            candidate.layout = layout;
            candidate.item_asset = asset;
            candidate.loaded_class = loaded;
            candidate.amount = amount;
            candidate.shelf_index = 0;
            candidate.source_index = source_index;
            candidate.shelves_before = shelves;
            candidate.attached_before = 0;
            candidate.looted_before = looted;
            candidate.record_fingerprint = source_record_fingerprint(candidate);

            auto descriptor = catalog_descriptor(candidate);
            descriptor.fingerprint = orchestration_source_key(candidate);
            if (!runtime_target_supported(candidate) || !autoloot::post7c3::allowlisted(descriptor)) continue;

            emit_log(STR("[AutoLootNativeProbe] SAVED_ONLY_CANDIDATE source={} item_asset={} loaded_class={} amount={} source_index={}\n"),
                     exact_name(source), exact_name(asset), exact_name(loaded), amount, source_index);
            return candidate;
        }
        return {};
    }

    auto resolve_corpse_target(UObject* source, UClass* pickup_base_class) -> AcquisitionTarget
    {
        AcquisitionTarget target{};
        const auto source_name = catalog_source_name(source);
        if (!source || !pickup_base_class || source_name.empty() || !is_corpse_source_name(source_name) ||
            !autoloot::post7c3::source_profile(source_name) ||
            !autoloot::post7c3::source_profile(source_name)->single_record_safe)
        {
            return target;
        }

        auto* loot_component = get_loot_box_component(source);
        bool looted{};
        int32 shelves{};
        if (!read_corpse_source_state(source, loot_component, looted, shelves) || looted || shelves <= 0)
        {
            return target;
        }

        auto* actor = static_cast<AActor*>(source);
        auto* containers_property = CastField<FArrayProperty>(loot_component->GetPropertyByNameInChain(STR("AttachedItemContainers")));
        auto* container_inner = containers_property ? CastField<FObjectPropertyBase>(containers_property->GetInner()) : nullptr;
        auto* saved_property = CastField<FArrayProperty>(loot_component->GetPropertyByNameInChain(STR("SavedItems")));
        auto* saved_inner = saved_property ? CastField<FStructProperty>(saved_property->GetInner()) : nullptr;
        auto* saved_info = saved_inner && saved_inner->GetStruct() ? saved_inner->GetStruct().Get() : nullptr;
        auto* saved_amount_property = saved_info ? CastField<FIntProperty>(saved_info->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
        auto* saved_asset_property = saved_info ? CastField<FObjectPropertyBase>(saved_info->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
        if (!containers_property || !container_inner || !saved_property || !saved_amount_property || !saved_asset_property)
        {
            return target;
        }

        FScriptArrayHelper_InContainer saved_items{saved_property, loot_component};
        std::vector<SemanticSourceRecord> records;
        records.reserve(saved_items.Num());
        for (int32 index = 0; index < saved_items.Num(); ++index)
        {
            auto* entry = saved_items.GetRawPtr(index);
            records.push_back({read_object(saved_asset_property, entry), read_int(saved_amount_property, entry)});
        }

        FScriptArrayHelper_InContainer containers{containers_property, loot_component};
        for (int32 container_index = 0; container_index < containers.Num(); ++container_index)
        {
            auto* layout = read_object(container_inner, containers.GetRawPtr(container_index));
            if (!layout || !autoloot::post7c3::layout_supported(source_name, catalog_layout_name(layout))) continue;

            auto* attached_property = CastField<FArrayProperty>(layout->GetPropertyByNameInChain(STR("AttachedItems")));
            auto* attached_inner = attached_property ? CastField<FStructProperty>(attached_property->GetInner()) : nullptr;
            auto* spawn_info = attached_inner && attached_inner->GetStruct() ? attached_inner->GetStruct().Get() : nullptr;
            auto* amount_property = spawn_info ? CastField<FIntProperty>(spawn_info->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
            auto* asset_property = spawn_info ? CastField<FObjectPropertyBase>(spawn_info->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
            auto* loaded_property = spawn_info ? CastField<FObjectPropertyBase>(spawn_info->GetPropertyByNameInChain(STR("LoadedPickupableClass"))) : nullptr;
            auto* spawned_property = spawn_info ? CastField<FObjectPropertyBase>(spawn_info->GetPropertyByNameInChain(STR("SpawnedItem"))) : nullptr;
            if (!attached_property || !amount_property || !asset_property || !loaded_property) continue;

            FScriptArrayHelper_InContainer attached{attached_property, layout};
            for (int32 shelf_index = 0; shelf_index < attached.Num(); ++shelf_index)
            {
                auto* entry = attached.GetRawPtr(shelf_index);
                const int32 amount = read_int(amount_property, entry);
                auto* asset = read_object(asset_property, entry);
                auto* loaded = static_cast<UClass*>(read_object(loaded_property, entry));
                if ((!loaded || !loaded->IsChildOf(pickup_base_class)) && asset)
                {
                    const auto inventory_data = catalog_item_data_name(asset);
                    const auto pickup_name = autoloot::post7c3::item_class_for_data(inventory_data);
                    if (!pickup_name.empty() && autoloot::post7c3::item_supported(inventory_data, pickup_name, amount))
                    {
                        loaded = find_catalog_pickup_class(pickup_name, pickup_base_class);
                    }
                }
                auto* spawned = spawned_property ? read_object(spawned_property, entry) : nullptr;
                int32 source_index{-1};
                const int32 semantic_matches = count_semantic_matches(records, asset, amount, source_index);
                if (semantic_matches > 1) return {};
                if (amount <= 0 || !asset || !loaded || !loaded->IsChildOf(pickup_base_class) || semantic_matches != 1)
                {
                    continue;
                }

                AcquisitionTarget candidate{};
                candidate.source = source;
                candidate.source_world = actor->GetWorld();
                candidate.source_level = actor->GetLevel();
                candidate.layout = layout;
                candidate.item_asset = asset;
                candidate.loaded_class = loaded;
                candidate.loot_component = loot_component;
                candidate.spawned_item = spawned;
                candidate.amount = amount;
                candidate.shelf_index = shelf_index;
                candidate.source_index = source_index;
                candidate.shelves_before = shelves;
                candidate.attached_before = attached.Num();
                candidate.looted_before = looted;
                candidate.corpse_surface = true;
                candidate.record_fingerprint = source_record_fingerprint(candidate);

                auto descriptor = catalog_descriptor(candidate);
                descriptor.fingerprint = orchestration_source_key(candidate);
                if (!runtime_target_supported(candidate) || !autoloot::post7c3::allowlisted(descriptor)) continue;

                emit_log(STR("[AutoLootNativeProbe] CORPSE_CANDIDATE source={} layout={} item_asset={} loaded_class={} amount={} source_index={} shelf_index={} spawned={}\n"),
                         exact_name(source), exact_name(layout), exact_name(asset), exact_name(loaded), amount, source_index, shelf_index,
                         exact_name(spawned));
                return candidate;
            }
        }
        return {};
    }

    auto resolve_target_from_source(UObject* source, UClass* pickup_base_class) -> AcquisitionTarget
    {
        AcquisitionTarget target{};
        if (!source || !pickup_base_class || source->HasAnyFlags(RF_ClassDefaultObject) ||
            catalog_source_name(source).empty())
        {
            return target;
        }

        if (is_corpse_source_name(catalog_source_name(source)))
        {
            return resolve_corpse_target(source, pickup_base_class);
        }

        clear_premature_looted_flag(source);

        bool looted{};
        int32 shelves{};
        if (!read_source_state(source, looted, shelves) || looted || shelves <= 0)
        {
            return target;
        }
        auto* actor = static_cast<AActor*>(source);
        auto* shelf_property = CastField<FObjectPropertyBase>(source->GetPropertyByNameInChain(STR("LootboxShelf")));
        auto* shelf = shelf_property ? read_object(shelf_property, source) : nullptr;
        auto* containers_property = CastField<FArrayProperty>(shelf ? shelf->GetPropertyByNameInChain(STR("AttachedContainers")) : nullptr);
        auto* container_inner = containers_property ? CastField<FObjectPropertyBase>(containers_property->GetInner()) : nullptr;
        if (!shelf || !containers_property || !container_inner)
        {
            return target;
        }

        FScriptArrayHelper_InContainer containers{containers_property, shelf};
        for (int32 container_index = 0; container_index < containers.Num(); ++container_index)
        {
            auto* layout = read_object(container_inner, containers.GetRawPtr(container_index));
            if (!layout || layout->GetOuterPrivate() != actor->GetLevel() ||
                !autoloot::post7c3::layout_supported(catalog_source_name(source), catalog_layout_name(layout)))
            {
                continue;
            }
            auto* attached_property = CastField<FArrayProperty>(layout->GetPropertyByNameInChain(STR("AttachedItems")));
            auto* attached_inner = attached_property ? CastField<FStructProperty>(attached_property->GetInner()) : nullptr;
            auto* spawn_info = attached_inner && attached_inner->GetStruct() ? attached_inner->GetStruct().Get() : nullptr;
            auto* amount_property = spawn_info ? CastField<FIntProperty>(spawn_info->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
            auto* asset_property = spawn_info ? CastField<FObjectPropertyBase>(spawn_info->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
            auto* loaded_property = spawn_info ? CastField<FObjectPropertyBase>(spawn_info->GetPropertyByNameInChain(STR("LoadedPickupableClass"))) : nullptr;
            auto* source_items_property = CastField<FArrayProperty>(source->GetPropertyByNameInChain(STR("SavedItemsInShelves")));
            auto* source_inner = source_items_property ? CastField<FStructProperty>(source_items_property->GetInner()) : nullptr;
            auto* source_info = source_inner && source_inner->GetStruct() ? source_inner->GetStruct().Get() : nullptr;
            auto* source_amount_property = source_info ? CastField<FIntProperty>(source_info->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
            auto* source_asset_property = source_info ? CastField<FObjectPropertyBase>(source_info->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
            if (!attached_property || !amount_property || !asset_property || !loaded_property || !source_items_property ||
                !source_amount_property || !source_asset_property)
            {
                continue;
            }

            FScriptArrayHelper_InContainer attached{attached_property, layout};
            FScriptArrayHelper_InContainer source_items{source_items_property, source};
            std::vector<SemanticSourceRecord> records;
            records.reserve(source_items.Num());
            for (int32 index = 0; index < source_items.Num(); ++index)
            {
                auto* entry = source_items.GetRawPtr(index);
                records.push_back({read_object(source_asset_property, entry), read_int(source_amount_property, entry)});
            }

            for (int32 shelf_index = 0; shelf_index < attached.Num(); ++shelf_index)
            {
                auto* entry = attached.GetRawPtr(shelf_index);
                const int32 amount = read_int(amount_property, entry);
                auto* asset = read_object(asset_property, entry);
                auto* loaded = static_cast<UClass*>(read_object(loaded_property, entry));
                if ((!loaded || !loaded->IsChildOf(pickup_base_class)) && asset)
                {
                    const auto inventory_data = catalog_item_data_name(asset);
                    const auto pickup_name = autoloot::post7c3::item_class_for_data(inventory_data);
                    if (!pickup_name.empty() && autoloot::post7c3::item_supported(inventory_data, pickup_name, amount))
                    {
                        loaded = find_catalog_pickup_class(pickup_name, pickup_base_class);
                    }
                }
                int32 source_index{-1};
                const int32 semantic_matches = count_semantic_matches(records, asset, amount, source_index);
                if (semantic_matches > 1) return {};
                if (amount <= 0 || !asset || !loaded || !loaded->IsChildOf(pickup_base_class) || semantic_matches != 1)
                {
                    continue;
                }

                AcquisitionTarget candidate{};
                candidate.source = source;
                candidate.source_world = actor->GetWorld();
                candidate.source_level = actor->GetLevel();
                candidate.layout = layout;
                candidate.item_asset = asset;
                candidate.loaded_class = loaded;
                candidate.amount = amount;
                candidate.shelf_index = shelf_index;
                candidate.source_index = source_index;
                candidate.shelves_before = shelves;
                candidate.attached_before = attached.Num();
                candidate.looted_before = looted;
                candidate.record_fingerprint = source_record_fingerprint(candidate);

                auto descriptor = catalog_descriptor(candidate);
                descriptor.fingerprint = orchestration_source_key(candidate);
                if (!runtime_target_supported(candidate) || !autoloot::post7c3::allowlisted(descriptor))
                {
                    emit_log(STR("[AutoLootNativeProbe] ITEM_SKIPPED_UNSUPPORTED source={} shelf_index={} item_asset={} loaded_class={} amount={} action=continue_supported_items\n"),
                             exact_name(source), shelf_index, exact_name(asset), exact_name(loaded), amount);
                    continue;
                }
                return candidate;
            }

            // R47/Box11: AttachedItems can be a lazy subset. If every attached row is unsupported
            // (or attached is empty), still resolve allowlisted SavedItemsInShelves rows.
            auto saved_only = resolve_saved_only_target(source,
                                                       layout,
                                                       actor,
                                                       looted,
                                                       shelves,
                                                       source_items_property,
                                                       source_amount_property,
                                                       source_asset_property,
                                                       pickup_base_class);
            if (saved_only.source) return saved_only;
        }
        return {};
    }

    auto validate_target_descriptor(const AcquisitionTarget& target) -> bool
    {
        if (!target.source || !target.layout || !target.item_asset || target.shelf_index < 0 || target.source_index < 0 ||
            target.attached_before < 0 || source_record_fingerprint(target) != target.record_fingerprint ||
            !runtime_target_supported(target))
        {
            return false;
        }

        if (target.corpse_surface)
        {
            if (!target.loot_component) return false;
            bool looted{};
            int32 shelves{};
            if (!read_corpse_source_state(target.source, target.loot_component, looted, shelves) || looted ||
                shelves != target.shelves_before)
            {
                return false;
            }

            auto* saved_property = CastField<FArrayProperty>(target.loot_component->GetPropertyByNameInChain(STR("SavedItems")));
            auto* saved_inner = saved_property ? CastField<FStructProperty>(saved_property->GetInner()) : nullptr;
            auto* saved_type = saved_inner && saved_inner->GetStruct() ? saved_inner->GetStruct().Get() : nullptr;
            auto* saved_amount = saved_type ? CastField<FIntProperty>(saved_type->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
            auto* saved_asset = saved_type ? CastField<FObjectPropertyBase>(saved_type->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
            auto* attached_property = CastField<FArrayProperty>(target.layout->GetPropertyByNameInChain(STR("AttachedItems")));
            auto* attached_inner = attached_property ? CastField<FStructProperty>(attached_property->GetInner()) : nullptr;
            auto* attached_type = attached_inner && attached_inner->GetStruct() ? attached_inner->GetStruct().Get() : nullptr;
            auto* attached_amount = attached_type ? CastField<FIntProperty>(attached_type->GetPropertyByNameInChain(STR("Amount"))) : nullptr;
            auto* attached_asset = attached_type ? CastField<FObjectPropertyBase>(attached_type->GetPropertyByNameInChain(STR("ItemClass"))) : nullptr;
            if (!saved_property || !saved_amount || !saved_asset || !attached_property || !attached_amount || !attached_asset)
            {
                return false;
            }

            FScriptArrayHelper_InContainer saved_items{saved_property, target.loot_component};
            FScriptArrayHelper_InContainer attached{attached_property, target.layout};
            if (target.source_index >= saved_items.Num() || target.shelf_index >= attached.Num() ||
                attached.Num() != target.attached_before)
            {
                return false;
            }
            void* saved_entry = saved_items.GetRawPtr(target.source_index);
            void* attached_entry = attached.GetRawPtr(target.shelf_index);
            return read_int(saved_amount, saved_entry) == target.amount &&
                   read_object(saved_asset, saved_entry) == target.item_asset &&
                   read_int(attached_amount, attached_entry) == target.amount &&
                   read_object(attached_asset, attached_entry) == target.item_asset;
        }

        bool looted{};
        int32 shelves{};
        if (!read_source_state(target.source, looted, shelves) || looted || shelves != target.shelves_before)
        {
            return false;
        }

        auto* source_items_property = require_property<FArrayProperty>(target.source, STR("source_guard"), STR("SavedItemsInShelves"));
        auto* source_inner = source_items_property ? CastField<FStructProperty>(source_items_property->GetInner()) : nullptr;
        auto* source_type = source_inner && source_inner->GetStruct() ? source_inner->GetStruct().Get() : nullptr;
        auto* source_amount_property = require_property<FIntProperty>(source_type, STR("source_guard.item"), STR("Amount"));
        auto* source_asset_property = require_property<FObjectPropertyBase>(source_type, STR("source_guard.item"), STR("ItemClass"));
        if (!source_items_property || !source_inner || !source_amount_property || !source_asset_property)
        {
            return false;
        }
        FScriptArrayHelper_InContainer source_items{source_items_property, target.source};
        if (target.source_index >= source_items.Num())
        {
            return false;
        }
        void* source_entry = source_items.GetRawPtr(target.source_index);
        if (read_int(source_amount_property, source_entry) != target.amount || read_object(source_asset_property, source_entry) != target.item_asset)
        {
            return false;
        }

        auto* attached_property = require_property<FArrayProperty>(target.layout, STR("layout_guard"), STR("AttachedItems"));
        auto* attached_inner = attached_property ? CastField<FStructProperty>(attached_property->GetInner()) : nullptr;
        auto* attached_type = attached_inner && attached_inner->GetStruct() ? attached_inner->GetStruct().Get() : nullptr;
        auto* attached_amount_property = require_property<FIntProperty>(attached_type, STR("layout_guard.item"), STR("Amount"));
        auto* attached_asset_property = require_property<FObjectPropertyBase>(attached_type, STR("layout_guard.item"), STR("ItemClass"));
        if (!attached_property || !attached_inner || !attached_amount_property || !attached_asset_property)
        {
            return false;
        }
        FScriptArrayHelper_InContainer attached{attached_property, target.layout};
        if (attached.Num() != target.attached_before)
        {
            return false;
        }
        if (target.attached_before == 0)
        {
            return true;
        }
        if (target.shelf_index >= attached.Num())
        {
            return false;
        }
        void* attached_entry = attached.GetRawPtr(target.shelf_index);
        return read_int(attached_amount_property, attached_entry) == target.amount &&
               read_object(attached_asset_property, attached_entry) == target.item_asset;
    }

    class AutoLootNativeProbe final : public CppUserModBase
    {
      public:
        AutoLootNativeProbe()
        {
            ProcessedFloorPickups.clear();
            ProcessedWorldPickups.clear();
            ModName = STR("AutoLootNativeProbe");
            ModVersion = STR("autox1.prototype");
            ModDescription = STR("Automatic one-shot AutoLoot prototype");
            ModAuthors = STR("Atomic Heart AutoLoot research");
            emit_log(STR("[AutoLootNativeProbe] BUILD_VARIANT marker={} orchestration_marker={} verbose=false\n"),
                         BuildVariantMarker, OrchestrationMarker);
            emit_log(STR("[AutoLootNativeProbe] START marker={} stage=auto.x1 fallback_hotkey=CTRL+F9 auto_toggle_hotkey=CTRL+F10 log_toggle_hotkey=CTRL+F11\n"), OrchestrationMarker);
            register_keydown_event(Input::Key::F9, {Input::ModifierKey::CONTROL}, [this]() {
                if (!m_stopping.load(std::memory_order_acquire))
                {
                    bool expected = false;
                    if (!m_dispatch_armed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                    {
                        emit_log(STR("[AutoLootNativeProbe] TRIGGER_REJECTED_BUSY marker={} reason=BUSY mutation=0\n"), OrchestrationMarker);
                        return;
                    }
                    arm_game_thread_dispatch();
                }
            });
            register_keydown_event(Input::Key::F10, {Input::ModifierKey::CONTROL}, [this]() {
                if (m_stopping.load(std::memory_order_acquire)) return;
                const bool enabled = m_auto_control.toggle();
                emit_log(STR("[AutoLootNativeProbe] {} marker={} control=CTRL+F10 uobject_access=0 mutation=0\n"),
                             enabled ? STR("AUTO_ENABLED") : STR("AUTO_DISABLED"), BuildVariantMarker);
            });
            register_keydown_event(Input::Key::F11, {Input::ModifierKey::CONTROL}, [this]() {
                if (m_stopping.load(std::memory_order_acquire)) return;
                const bool enabled = !RuntimeLoggingEnabled.load(std::memory_order_relaxed);
                RuntimeLoggingEnabled.store(enabled, std::memory_order_release);
                RC::Output::send(STR("[AutoLootNativeProbe] {} marker={} control=CTRL+F11\n"),
                                 enabled ? STR("LOGGING_ENABLED") : STR("LOGGING_DISABLED"), BuildVariantMarker);
            });
        }

        ~AutoLootNativeProbe() override
        {
            m_stopping.store(true, std::memory_order_release);
            m_dispatch_armed.store(false, std::memory_order_release);
            const auto engine_tick_callback = m_engine_tick_callback.exchange(Hook::ERROR_ID, std::memory_order_acq_rel);
            if (engine_tick_callback != Hook::ERROR_ID)
            {
                Hook::UnregisterCallback(engine_tick_callback);
            }
            const auto callback = m_dispatch_callback.exchange(Hook::ERROR_ID, std::memory_order_acq_rel);
            if (callback != Hook::ERROR_ID)
            {
                const bool removed = Hook::UnregisterCallback(callback);
                emit_log(STR("[AutoLootNativeProbe] CALLBACK_REMOVED marker={} process_event={}\n"), OrchestrationMarker, removed);
            }
            emit_log(STR("[AutoLootNativeProbe] STOP marker={}\n"), OrchestrationMarker);
        }

        auto on_unreal_init() -> void override
        {
            m_auto_control.set(true);
            emit_log(STR("[AutoLootNativeProbe] AUTO_ENABLED marker={} control=initial_state uobject_access=0 mutation=0\n"), BuildVariantMarker);
            arm_engine_tick_dispatch();
        }

        auto on_update() -> void override
        {
        }

      private:

        auto pickup_processed(const std::vector<StringType>& processed, const StringType& identity) const -> bool
        {
            for (const auto& existing : processed)
            {
                if (existing == identity) return true;
            }
            return false;
        }

        auto source_queued(UObject* object) -> bool
        {
            if (!object) return false;
            if (m_active_source.Get() == object) return true;
            for (const auto& candidate : m_source_candidates)
            {
                if (candidate.Get() == object) return true;
            }
            return false;
        }

        auto pickup_queued(const std::deque<QueuedPickup>& queue, UObject* object) -> bool
        {
            for (const auto& candidate : queue)
            {
                if (candidate.object.Get() == object) return true;
            }
            return false;
        }

        auto inspect_incremental_object(UObject* object) -> void
        {
            if (!object || object->HasAnyFlags(RF_ClassDefaultObject) || !object->GetClassPrivate()) return;
            const auto class_name = object->GetClassPrivate()->GetName();

            if (class_name == STR("BP_PlayerInventory_C") || class_name == STR("BP_PlayerInventory_C_0"))
            {
                if (object->GetName() == STR("BP_PlayerInventory_C_0"))
                {
                    ++m_scan_inventory_matches;
                    if (m_scan_inventory_matches == 1) m_scan_inventory = object;
                }
            }
            if (class_name == STR("BP_LootSpawnSubsystem_C") || class_name == STR("BP_LootSpawnSubsystem_C_0"))
            {
                if (object->GetName() == STR("BP_LootSpawnSubsystem_C_0"))
                {
                    ++m_scan_pool_matches;
                    if (m_scan_pool_matches == 1) m_scan_pool = object;
                }
            }

            if (!catalog_source_name(object).empty() && !source_queued(object) &&
                m_source_candidates.size() < IncrementalQueueCapacity)
            {
                m_source_candidates.emplace_back(object);
            }

            const bool floor = class_name == STR("BP_Pickable_consumable_aid_small_C") ||
                               class_name == STR("BP_Pickable_consumable_aid_medium_C") ||
                               class_name == STR("BP_Pickable_sguschenka_C") ||
                               class_name == STR("BP_Pickupable_Sguschenka_C");
            if (floor)
            {
                if (m_floor_candidates.size() >= IncrementalQueueCapacity || pickup_queued(m_floor_candidates, object)) return;
                // Use actor GetName(): FullName outer path changes when ContinuousPickup moves
                // the actor (sguschenka Forester_House_* → Atomic_World_01), which re-queued it.
                const auto identity = object->GetName();
                if (!pickup_processed(ProcessedFloorPickups, identity))
                {
                    m_floor_candidates.push_back({FWeakObjectPtr{object}, identity});
                }
                return;
            }

            if (world_pickup_admissible(object) && m_world_candidates.size() < IncrementalQueueCapacity &&
                !pickup_queued(m_world_candidates, object))
            {
                const auto identity = object->GetFullName();
                if (!pickup_processed(ProcessedWorldPickups, identity))
                {
                    m_world_candidates.push_back({FWeakObjectPtr{object}, identity});
                }
            }
        }

        auto complete_scan_generation(std::chrono::steady_clock::time_point now) -> void
        {
            if (m_scan_inventory_matches == 1 && m_scan_inventory.Get()) m_cached_inventory = m_scan_inventory.Get();
            else m_cached_inventory.Reset();
            if (m_scan_pool_matches == 1 && m_scan_pool.Get()) m_cached_pool = m_scan_pool.Get();
            else m_cached_pool.Reset();
            m_scan_inventory.Reset();
            m_scan_pool.Reset();
            m_scan_inventory_matches = 0;
            m_scan_pool_matches = 0;
            m_scan_cursor = 0;
            m_scan_limit = 0;
            m_next_engine_tick_scan = now + std::chrono::milliseconds(750);
        }

        auto scan_incremental_slice(std::chrono::steady_clock::time_point now) -> void
        {
            if (m_scan_limit == 0)
            {
                if (now < m_next_engine_tick_scan) return;
                m_scan_cursor = 0;
                m_scan_limit = UObjectArray::GetNumElements();
                m_scan_inventory.Reset();
                m_scan_pool.Reset();
                m_scan_inventory_matches = 0;
                m_scan_pool_matches = 0;
                if (m_scan_limit <= 0)
                {
                    complete_scan_generation(now);
                    return;
                }
            }

            const auto started = std::chrono::steady_clock::now();
            const int32 current_count = UObjectArray::GetNumElements();
            if (current_count < m_scan_limit) m_scan_limit = current_count;
            int32 scanned{};
            while (m_scan_cursor < m_scan_limit && scanned < IncrementalScanMaxSlots)
            {
                const int32 index = m_scan_cursor++;
                ++scanned;
                auto* item = UObjectArray::IndexToObject(index);
                if (item && item->IsValid(false)) inspect_incremental_object(item->GetUObject());
                if ((scanned & 63) == 0)
                {
                    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started).count();
                    if (elapsed_us >= IncrementalScanBudgetUs) break;
                }
            }
            if (m_scan_cursor >= m_scan_limit) complete_scan_generation(now);
        }

        auto take_runtime_work() -> RuntimeScanView
        {
            RuntimeScanView view{};
            view.inventory = m_cached_inventory.Get();
            view.pool = m_cached_pool.Get();
            if (!view.inventory || !view.pool) return view;

            auto* pickup_base_class = require_class(STR("pickup_base"), PickupBaseClassPath);
            if (!pickup_base_class) return view;
            if (auto* active_source = m_active_source.Get())
            {
                view.target = resolve_target_from_source(active_source, pickup_base_class);
                if (view.target.source) return view;
                m_active_source.Reset();
            }

            for (int32 attempts = 0; attempts < 4 && !m_source_candidates.empty(); ++attempts)
            {
                auto candidate = m_source_candidates.front();
                m_source_candidates.pop_front();
                auto* source = candidate.Get();
                if (!source) continue;
                view.target = resolve_target_from_source(source, pickup_base_class);
                if (view.target.source)
                {
                    m_active_source = source;
                    return view;
                }
            }

            for (int32 attempts = 0; attempts < 8 && !m_floor_candidates.empty(); ++attempts)
            {
                auto candidate = std::move(m_floor_candidates.front());
                m_floor_candidates.pop_front();
                auto* pickup = candidate.object.Get();
                if (!pickup || pickup_processed(ProcessedFloorPickups, candidate.identity)) continue;
                const auto class_name = pickup->GetClassPrivate() ? pickup->GetClassPrivate()->GetName() : StringType{};
                if (class_name != STR("BP_Pickable_consumable_aid_small_C") &&
                    class_name != STR("BP_Pickable_consumable_aid_medium_C") &&
                    class_name != STR("BP_Pickable_sguschenka_C") &&
                    class_name != STR("BP_Pickupable_Sguschenka_C"))
                {
                    continue;
                }
                view.floor_pickup = pickup;
                view.floor_identity = std::move(candidate.identity);
                return view;
            }

            for (int32 attempts = 0; attempts < 8 && !m_world_candidates.empty(); ++attempts)
            {
                auto candidate = std::move(m_world_candidates.front());
                m_world_candidates.pop_front();
                auto* pickup = candidate.object.Get();
                if (!pickup || !pickup->GetClassPrivate() || pickup_processed(ProcessedWorldPickups, candidate.identity) ||
                    !world_pickup_admissible(pickup)) continue;
                view.world_pickup = pickup;
                view.world_identity = std::move(candidate.identity);
                return view;
            }
            return view;
        }

        auto runtime_work_available(const RuntimeScanView& view) const noexcept -> bool
        {
            return view.target.source || view.floor_pickup || view.world_pickup;
        }

        auto release_failed_attempt(UObject* source) noexcept -> void
        {
            if (m_active_source.Get() == source) m_active_source.Reset();
            m_guard_claimed = false;
            m_guard_source = nullptr;
            m_guard_source_index = -1;
            m_guard_fingerprint = 0;
            m_terminal_attempt.store(false, std::memory_order_release);
        }

        auto force_scan_generation() -> void
        {
            const auto now = std::chrono::steady_clock::now();
            m_scan_cursor = 0;
            m_scan_limit = UObjectArray::GetNumElements();
            m_scan_inventory.Reset();
            m_scan_pool.Reset();
            m_scan_inventory_matches = 0;
            m_scan_pool_matches = 0;
            if (m_scan_limit <= 0)
            {
                complete_scan_generation(now);
                return;
            }
            while (m_scan_cursor < m_scan_limit)
            {
                auto* item = UObjectArray::IndexToObject(m_scan_cursor++);
                if (item && item->IsValid(false)) inspect_incremental_object(item->GetUObject());
            }
            complete_scan_generation(now);
        }

        auto arm_engine_tick_dispatch() noexcept -> void
        {
            try
            {
                Hook::FCallbackOptions options{};
                options.bOnce = false;
                options.bReadonly = false;
                options.OwnerModName = STR("AutoLootNativeProbe");
                options.HookName = STR("AutoLootFastGameThreadTick");
                const auto callback = Hook::RegisterEngineTickPreCallback(
                    [this](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
                        if (m_stopping.load(std::memory_order_acquire))
                        {
                            return;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (!m_auto_control.admit_auto() || m_terminal_attempt.load(std::memory_order_acquire)) return;
                        scan_incremental_slice(now);
                        auto work = take_runtime_work();
                        if (!runtime_work_available(work)) return;
                        begin_acquisition(true, &work);
                    },
                    options);
                if (callback == Hook::ERROR_ID)
                {
                    emit_log(STR("[AutoLootNativeProbe] ABORTED_SAFE marker={} reason=ENGINE_TICK_REGISTER_FAILED mutation=0\n"), OrchestrationMarker);
                    return;
                }
                m_engine_tick_callback.store(callback, std::memory_order_release);
                emit_log(STR("[AutoLootNativeProbe] ENGINE_TICK_ARMED marker={}\n"), OrchestrationMarker);
            }
            catch (...)
            {
                emit_log(STR("[AutoLootNativeProbe] ABORTED_SAFE marker={} reason=ENGINE_TICK_REGISTER_EXCEPTION mutation=0\n"), OrchestrationMarker);
            }
        }

        auto arm_game_thread_dispatch() noexcept -> void
        {
            try
            {
                Hook::FCallbackOptions options{};
                options.bOnce = true;
                options.bReadonly = false;
                options.OwnerModName = STR("AutoLootNativeProbe");
                options.HookName = STR("RunPost7C3OneShotOnGameThread");
                const auto callback = Hook::RegisterProcessEventPreCallback(
                    [this](Hook::TCallbackIterationData<void>& iteration, UObject*, UFunction*, void*) {
                        const auto dispatch_event = ++m_dispatch_events;
                        iteration.RemoveSelf();
                        m_dispatch_callback.store(Hook::ERROR_ID, std::memory_order_release);
                        if (dispatch_event < 3)
                        {
                            emit_log(STR("[AutoLootNativeProbe] DISPATCH_DEFERRED marker={} event={} reason=ENGINE_TICK_NOT_READY\n"),
                                         OrchestrationMarker, dispatch_event);
                            m_dispatch_armed.store(false, std::memory_order_release);
                            return;
                        }
                        if (!m_stopping.load(std::memory_order_acquire))
                        {
                            emit_log(STR("[AutoLootNativeProbe] AUTO_TRIGGER marker={} dispatch_event={}\n"), OrchestrationMarker, dispatch_event);
                            begin_acquisition(false);
                        }
                        m_dispatch_armed.store(false, std::memory_order_release);
                    },
                    options);
                if (callback == Hook::ERROR_ID)
                {
                    m_dispatch_armed.store(false, std::memory_order_release);
                    emit_log(STR("[AutoLootNativeProbe] ABORTED_SAFE marker={} reason=DISPATCH_REGISTER_FAILED mutation=0\n"), OrchestrationMarker);
                    return;
                }
                m_dispatch_callback.store(callback, std::memory_order_release);
                emit_log(STR("[AutoLootNativeProbe] TRIGGER_ACCEPTED marker={} dispatch=ProcessEvent automatic=true retry_seconds=2\n"),
                             OrchestrationMarker);
            }
            catch (...)
            {
                m_dispatch_armed.store(false, std::memory_order_release);
                emit_log(STR("[AutoLootNativeProbe] ABORTED_SAFE marker={} reason=DISPATCH_REGISTER_EXCEPTION mutation=0\n"), OrchestrationMarker);
            }
        }

        auto begin_acquisition(bool automatic, const RuntimeScanView* prepared_scan = nullptr) noexcept -> void
        {
            (void)automatic;
            autoloot::post7c3::InFlightGuard in_flight{m_auto_attempt_inflight};
            if (!in_flight.acquired())
            {
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] ADMISSION_REJECT reason=IN_FLIGHT mutation=0\n"));
                }
                return;
            }
            AcquisitionTarget target{};
            try
            {
                const bool game_thread = IsInGameThread();
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] BEGIN marker={} stage=7C.container.x1c game_thread={} spawn_actor_calls=1 initializer_calls=1 source_remove_calls=1 inventory_calls=1 pool_calls=1 source_array_writes=1 inventory_direct_writes=0\n"), Marker, game_thread);
                }
                if (!game_thread)
                {
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason={}\n"), Marker, game_thread ? STR("probe_busy") : STR("not_game_thread"));
                    return;
                }

                RuntimeScanView local_scan{};
                const RuntimeScanView* scan_ptr = prepared_scan;
                if (!scan_ptr)
                {
                    force_scan_generation();
                    local_scan = take_runtime_work();
                    scan_ptr = &local_scan;
                }
                const auto& scan = *scan_ptr;
                target = scan.target;
                if (!target.source || !target.loaded_class || !target.item_asset)
                {
                    if (collect_floor_aid(scan.floor_pickup, scan.floor_identity, scan.inventory, scan.pool))
                    {
                        emit_log(STR("[AutoLootNativeProbe] END marker={} result=PASS stage=floor_pickup inventory_calls=1\n"), Marker);
                        return;
                    }
                    if (collect_existing_catalog_pickup(scan.world_pickup, scan.world_identity, scan.inventory, scan.pool))
                    {
                        emit_log(STR("[AutoLootNativeProbe] END marker={} result=PASS stage=world_pickup inventory_calls=1\n"), Marker);
                        return;
                    }
                    if constexpr (VerboseDiagnostics)
                    {
                        emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=target_resolution\n"), Marker);
                    }
                    return;
                }
                auto descriptor = catalog_descriptor(target);
                descriptor.fingerprint = orchestration_source_key(target);
                const auto admission = autoloot::post7c3::classify_admission(1, descriptor, validate_target_descriptor(target));
                if (admission != autoloot::post7c3::AdmissionResult::Supported)
                {
                    if constexpr (VerboseDiagnostics)
                    {
                        emit_log(STR("[AutoLootNativeProbe] ADMISSION_REJECT reason={} source_key={:016X} mutation=0\n"),
                                 admission == autoloot::post7c3::AdmissionResult::Unsupported ? STR("UNSUPPORTED") : STR("STALE"),
                                 descriptor.fingerprint);
                    }
                    if (m_active_source.Get() == target.source) m_active_source.Reset();
                    return;
                }
                auto* inventory = scan.inventory;
                if (!inventory)
                {
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=inventory_resolution\n"), Marker);
                    return;
                }
                if (!validate_target_descriptor(target))
                {
                    emit_log(STR("[AutoLootNativeProbe] GUARD_REJECT marker={} reason=stale_or_ambiguous_source source_key={:016X} mutation=0\n"),
                                 Marker, target.record_fingerprint);
                    return;
                }
                auto* initializer = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, InitializePickupPath, false);
                auto* amount_property = require_property<FIntProperty>(initializer, STR("pickup_initializer"), STR("Amount"));
                auto* item_property = require_property<FObjectPropertyBase>(initializer, STR("pickup_initializer"), STR("ItemData"));
                if (!initializer || !amount_property || !item_property || initializer->GetParmsSize() != 16)
                {
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=initializer_contract\n"), Marker);
                    return;
                }
                if (m_guard_claimed)
                {
                    const bool same_key = m_guard_source == target.source && m_guard_source_index == target.source_index &&
                                          m_guard_fingerprint == target.record_fingerprint;
                    emit_log(STR("[AutoLootNativeProbe] GUARD_REJECT marker={} reason=duplicate_claim same_key={} source_key={:016X} mutation=0\n"),
                                 Marker, same_key, target.record_fingerprint);
                    return;
                }
                m_guard_claimed = true;
                m_guard_source = target.source;
                m_guard_source_index = target.source_index;
                m_guard_fingerprint = target.record_fingerprint;
                m_terminal_attempt.store(true, std::memory_order_release);
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] GUARD_CLAIM marker={} source_key={:016X} source_index={} shelf_index={} state=claimed\n"),
                                 Marker, target.record_fingerprint, target.source_index, target.shelf_index);
                }

                if (target.corpse_surface)
                {
                    auto* add_function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, AddInventoryPath, false);
                    FObjectPropertyBase* item_param{};
                    FBoolProperty* notify_param{};
                    if (add_function)
                    {
                        for (auto* property : add_function->ForEachProperty())
                        {
                            const auto name = property->GetName();
                            if (name == STR("InPickupableItem")) item_param = CastField<FObjectPropertyBase>(property);
                            else if (name == STR("InSendNotification")) notify_param = CastField<FBoolProperty>(property);
                        }
                    }
                    auto* pool_subsystem = scan.pool;
                    if (!add_function || !item_param || !notify_param || !pool_subsystem || add_function->GetParmsSize() == 0 ||
                        add_function->GetParmsSize() > 256)
                    {
                        emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=corpse_inventory_contract\n"), Marker);
                        release_failed_attempt(target.source);
                        return;
                    }

                    UObject* pickup = target.spawned_item;
                    bool spawned_fresh = false;
                    auto* pickup_base_class = UObjectGlobals::StaticFindObject<UClass*>(nullptr, nullptr, PickupBaseClassPath, false);
                    if (!pickup || !pickup_base_class || !pickup->GetClassPrivate() ||
                        !static_cast<UClass*>(pickup->GetClassPrivate())->IsChildOf(pickup_base_class))
                    {
                        auto* initializer = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, InitializePickupPath, false);
                        auto* amount_property = require_property<FIntProperty>(initializer, STR("corpse_initializer"), STR("Amount"));
                        auto* item_property = require_property<FObjectPropertyBase>(initializer, STR("corpse_initializer"), STR("ItemData"));
                        if (!initializer || !amount_property || !item_property || initializer->GetParmsSize() != 16)
                        {
                            emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=corpse_initializer_contract\n"), Marker);
                            release_failed_attempt(target.source);
                            return;
                        }
                        auto location = static_cast<AActor*>(target.source)->K2_GetActorLocation();
                        pickup = target.source_world->SpawnActor(target.loaded_class, &location, nullptr);
                        if (!pickup)
                        {
                            emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=corpse_spawn_null\n"), Marker);
                            release_failed_attempt(target.source);
                            return;
                        }
                        spawned_fresh = true;
                        std::vector<uint8> init_params(initializer->GetParmsSize(), 0);
                        amount_property->SetPropertyValueInContainer(init_params.data(), target.amount);
                        item_property->SetObjectPropertyValue(item_property->ContainerPtrToValuePtr<void>(init_params.data()), target.item_asset);
                        pickup->ProcessEvent(initializer, init_params.data());
                    }

                    if (!invoke_corpse_removal(target))
                    {
                        if (spawned_fresh) static_cast<AActor*>(pickup)->K2_DestroyActor();
                        emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=corpse_removal\n"), Marker);
                        release_failed_attempt(target.source);
                        return;
                    }

                    const auto pickup_identity = exact_name(pickup);
                    std::vector<uint8> add_params(add_function->GetParmsSize(), 0);
                    item_param->SetObjectPropertyValue(item_param->ContainerPtrToValuePtr<void>(add_params.data()), pickup);
                    notify_param->SetPropertyValueInContainer(add_params.data(), true);
                    inventory->ProcessEvent(add_function, add_params.data());

                    bool pooled = false;
                    bool deactivated = false;
                    bool destroyed = false;
                    const bool cleanup_ok = cleanup_collected_pickup(pool_subsystem, pickup, pooled, deactivated, destroyed);
                    if (cleanup_ok && !pickup_processed(ProcessedWorldPickups, pickup_identity))
                    {
                        ProcessedWorldPickups.push_back(pickup_identity);
                    }

                    bool looted_post{};
                    int32 shelves_post{};
                    int32 attached_post{};
                    const bool source_post = read_corpse_source_state(target.source, target.loot_component, looted_post, shelves_post);
                    const bool layout_post = read_layout_count(target.layout, attached_post);
                    const bool pass = cleanup_ok && source_post && shelves_post == target.shelves_before - 1 && layout_post &&
                                     attached_post == target.attached_before - 1;
                    if (pass)
                    {
                        if (shelves_post == 0) m_active_source.Reset();
                        else m_active_source = target.source;
                        m_guard_claimed = false;
                        m_guard_source = nullptr;
                        m_guard_source_index = -1;
                        m_guard_fingerprint = 0;
                        m_terminal_attempt.store(false, std::memory_order_release);
                    }
                    else
                    {
                        release_failed_attempt(target.source);
                    }
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result={} stage=corpse.lootbox_component inventory_calls=1 source_remove_calls=1 pool_calls={} spawned_fresh={} shelves_before={} shelves_after={} attached_before={} attached_after={} notification=true\n"),
                             Marker, pass ? STR("PASS") : STR("FAIL"), pooled ? 1 : 0, spawned_fresh, target.shelves_before, shelves_post,
                             target.attached_before, attached_post);
                    return;
                }

                auto location = static_cast<AActor*>(target.source)->K2_GetActorLocation();
                emit_log(STR("[AutoLootNativeProbe] CONTAINER_COMMIT source={} layout={} item_asset={} loaded_class={} amount={} source_index={} shelf_index={} attached_before={} shelves_before={}\n"),
                         exact_name(target.source), exact_name(target.layout), exact_name(target.item_asset), exact_name(target.loaded_class),
                         target.amount, target.source_index, target.shelf_index, target.attached_before, target.shelves_before);
                auto* spawned = target.source_world->SpawnActor(target.loaded_class, &location, nullptr);
                if (!spawned)
                {
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=spawn_actor_null\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }
                const bool spawned_contract = spawned->GetClassPrivate() == target.loaded_class && spawned->GetWorld() == target.source_world && spawned->GetLevel() == target.source_level;
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] SPAWN_ACTOR object={} class_match={} world_match={} level_match={} location=source_actor\n"),
                                 exact_name(spawned), spawned_contract && spawned->GetClassPrivate() == target.loaded_class, spawned->GetWorld() == target.source_world, spawned->GetLevel() == target.source_level);
                }
                if (!spawned_contract)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=spawn_actor_contract cleanup_calls=1\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }
                std::vector<uint8> params(initializer->GetParmsSize(), 0);
                amount_property->SetPropertyValueInContainer(params.data(), target.amount);
                item_property->SetObjectPropertyValue(item_property->ContainerPtrToValuePtr<void>(params.data()), target.item_asset);
                spawned->ProcessEvent(initializer, params.data());
                UObject* initialized_asset{};
                int32 initialized_amount{};
                const bool initialized = read_actor_item(spawned, initialized_asset, initialized_amount) && initialized_asset == target.item_asset && initialized_amount == target.amount;
                bool looted_after{};
                int32 shelves_after{};
                const bool source_unchanged = read_source_state(target.source, looted_after, shelves_after) && !looted_after && shelves_after == target.shelves_before;
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] INITIALIZED object={} result={} item_asset={} amount={} source_unchanged={} shelves_after={}\n"),
                                 exact_name(spawned), initialized, exact_name(initialized_asset), initialized_amount, source_unchanged, shelves_after);
                }
                if (!initialized || !source_unchanged)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=bootstrap_invariants cleanup_calls=1\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }

                auto* add_function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, AddInventoryPath, false);
                FObjectPropertyBase* item_param{};
                FBoolProperty* notify_param{};
                FProperty* return_param{};
                if (add_function)
                {
                    for (auto* property : add_function->ForEachProperty())
                    {
                        const auto name = property->GetName();
                        if (name == STR("InPickupableItem")) item_param = CastField<FObjectPropertyBase>(property);
                        else if (name == STR("InSendNotification")) notify_param = CastField<FBoolProperty>(property);
                        else if (name == STR("ReturnValue")) return_param = property;
                    }
                }
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] INVENTORY_CONTRACT function={} parms_size={} item_param={} notify_param={} return_param={} return_size={} return_offset={}\n"),
                                 exact_name(add_function), add_function ? add_function->GetParmsSize() : 0, item_param != nullptr, notify_param != nullptr,
                                 return_param != nullptr, return_param ? return_param->GetSize() : 0, return_param ? return_param->GetOffset_Internal() : 0);
                }
                if (!add_function || !item_param || !notify_param || !return_param || add_function->GetParmsSize() == 0 || add_function->GetParmsSize() > 256)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=inventory_contract cleanup_calls=1\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }
                auto* pool_subsystem = scan.pool;
                auto* pool_function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, ReturnToPoolPath, false);
                auto* pool_item_param = require_property<FObjectPropertyBase>(pool_function, STR("ReturnItemToPool"), STR("UsedItem"));
                if (!pool_subsystem || !pool_function || !pool_item_param)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=pool_contract cleanup_calls=1\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }

                if (!invoke_source_removal(target))
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=source_removal_call cleanup_calls=1\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }
                bool looted_removed{};
                int32 shelves_removed{};
                int32 attached_removed{};
                bool source_state_ok = read_source_state(target.source, looted_removed, shelves_removed);
                if (source_state_ok && looted_removed && shelves_removed > 0)
                {
                    clear_premature_looted_flag(target.source);
                    source_state_ok = read_source_state(target.source, looted_removed, shelves_removed);
                }
                const bool source_removed = source_state_ok && (!looted_removed || shelves_removed == 0) && shelves_removed == target.shelves_before - 1 &&
                                            read_layout_count(target.layout, attached_removed) &&
                                            (attached_removed == (target.attached_before > 0 ? target.attached_before - 1 : 0) ||
                                             // Callback wiped remaining AttachedItems; leftover SavedItems rows continue via SAVED_ONLY.
                                             (target.attached_before > 0 && attached_removed == 0));
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] SOURCE_REMOVE_CHECK result={} bLooted={} shelves_before={} shelves_after={} attached_before={} attached_after={}\n"),
                                 source_removed, looted_removed, target.shelves_before, shelves_removed, target.attached_before, attached_removed);
                }
                if (!source_removed)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=source_removal_invariants cleanup_calls=1 inventory_calls=0 bLooted={} shelves_before={} shelves_after={} attached_before={} attached_after={} source_ok={}\n"),
                             Marker, looted_removed, target.shelves_before, shelves_removed, target.attached_before, attached_removed, source_state_ok);
                    release_failed_attempt(target.source);
                    return;
                }

                bool purge_ok = true;
                if (shelves_removed == 0 && attached_removed == 0)
                {
                    // 2026-08-04 F11 Box11_2 manual last-item path: OnContainerRemovedItem already
                    // chains into OnContainerPurged. Calling purge again after a successful last-item
                    // removal crashed the process on the next tick (log ends at PURGE_CALL+PASS).
                    if (looted_removed)
                    {
                        emit_log(STR("[AutoLootNativeProbe] PURGE_SKIP reason=already_looted_by_removal source={} layout={} shelves=0 attached=0\n"),
                                 exact_name(target.source), exact_name(target.layout));
                    }
                    else
                    {
                        purge_ok = invoke_container_purge(target.source, target.layout, shelves_removed);
                    }
                }
                if (!purge_ok)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] END marker={} result=FAIL reason=purge_callback cleanup_calls=1\n"), Marker);
                    release_failed_attempt(target.source);
                    return;
                }

                std::vector<uint8> add_params(add_function->GetParmsSize(), 0);
                item_param->SetObjectPropertyValue(item_param->ContainerPtrToValuePtr<void>(add_params.data()), spawned);
                notify_param->SetPropertyValueInContainer(add_params.data(), true);
                const int32 spawned_index = spawned->GetInternalIndex();
                inventory->ProcessEvent(add_function, add_params.data());
                const auto* return_address = return_param->ContainerPtrToValuePtr<void>(add_params.data());
                uint64_t return_word{};
                std::memcpy(&return_word, return_address, (return_param->GetSize() < sizeof(return_word)) ? return_param->GetSize() : sizeof(return_word));
                auto* spawned_item = spawned_index >= 0 && spawned_index < UObjectArray::GetNumElements()
                                         ? UObjectArray::IndexToObject(spawned_index)
                                         : nullptr;
                const bool actor_still_known = spawned_item && spawned_item->IsValid(false) && spawned_item->GetUObject() == spawned;
                bool looted_post{};
                int32 shelves_post{};
                int32 attached_post{};
                const bool source_post = read_source_state(target.source, looted_post, shelves_post);
                if (source_post && looted_post && shelves_post > 0)
                {
                    clear_premature_looted_flag(target.source);
                    read_source_state(target.source, looted_post, shelves_post);
                }
                const bool layout_post = read_layout_count(target.layout, attached_post);
                if constexpr (VerboseDiagnostics)
                {
                    emit_log(STR("[AutoLootNativeProbe] INVENTORY_CALL return_word={} actor_still_known={} actor_name={} source_ok={} bLooted={} shelves={} layout_ok={} attached={} notification=true\n"),
                                 return_word, actor_still_known, actor_still_known ? exact_name(spawned) : STR("<released>"), source_post, looted_post, shelves_post, layout_post, attached_post);
                }
                const bool pool_returned = actor_still_known && return_actor_to_pool(pool_subsystem, spawned);
                if (actor_still_known && !pool_returned)
                {
                    spawned->K2_DestroyActor();
                    emit_log(STR("[AutoLootNativeProbe] CLEANUP object={} fallback_destroy_called=true\n"), exact_name(spawned));
                }
                const bool completion_state = shelves_post == 0 ? looted_post : !looted_post;
                const int32 expected_attached_after = target.attached_before > 0 ? target.attached_before - 1 : 0;
                const bool attached_ok =
                    attached_post == expected_attached_after || (target.attached_before > 0 && attached_post == 0);
                const bool source_expected_after = source_post && completion_state && shelves_post == target.shelves_before - 1 &&
                                                   layout_post && attached_ok;
                const bool pass = source_expected_after && pool_returned;
                if (pass)
                {
                    if (shelves_post == 0 && looted_post)
                    {
                        m_active_source.Reset();
                    }
                    else
                    {
                        m_active_source = target.source;
                    }
                    m_guard_claimed = false;
                    m_guard_source = nullptr;
                    m_guard_source_index = -1;
                    m_guard_fingerprint = 0;
                    m_terminal_attempt.store(false, std::memory_order_release);
                    if constexpr (VerboseDiagnostics)
                    {
                        emit_log(STR("[AutoLootNativeProbe] NEXT_TARGET_ARMED source={} remaining_items={}\n"),
                                     exact_name(target.source), shelves_post);
                    }
                }
                else
                {
                    release_failed_attempt(target.source);
                }
                emit_log(STR("[AutoLootNativeProbe] END marker={} result={} stage=7C.container.x1c inventory_calls=1 source_remove_calls=1 pool_calls={} source_expected={} shelves_before={} shelves_after={} attached_before={} attached_after={} source_array_writes=1 inventory_direct_writes=0\n"),
                             Marker, pass ? STR("PASS") : STR("FAIL"), pool_returned ? 1 : 0, source_expected_after, target.shelves_before, shelves_post, target.attached_before, attached_post);
            }
            catch (const std::exception&)
            {
                release_failed_attempt(target.source);
                emit_log(STR("[AutoLootNativeProbe] END marker={} result=EXCEPTION what=std_exception\n"), Marker);
            }
            catch (...)
            {
                release_failed_attempt(target.source);
                emit_log(STR("[AutoLootNativeProbe] END marker={} result=EXCEPTION what=unknown\n"), Marker);
            }
        }
        std::atomic_bool m_stopping{false};
        autoloot::post7c3::perf::AutoLoopControl m_auto_control{};
        std::atomic_bool m_terminal_attempt{false};
        std::atomic_bool m_auto_attempt_inflight{false};
        std::atomic_bool m_dispatch_armed{false};
        std::atomic<Hook::GlobalCallbackId> m_dispatch_callback{Hook::ERROR_ID};
        std::atomic<Hook::GlobalCallbackId> m_engine_tick_callback{Hook::ERROR_ID};
        uint32_t m_dispatch_events{};
        std::chrono::steady_clock::time_point m_next_engine_tick_scan{};
        int32 m_scan_cursor{};
        int32 m_scan_limit{};
        int32 m_scan_inventory_matches{};
        int32 m_scan_pool_matches{};
        FWeakObjectPtr m_scan_inventory{};
        FWeakObjectPtr m_scan_pool{};
        FWeakObjectPtr m_cached_inventory{};
        FWeakObjectPtr m_cached_pool{};
        FWeakObjectPtr m_active_source{};
        std::deque<FWeakObjectPtr> m_source_candidates{};
        std::deque<QueuedPickup> m_floor_candidates{};
        std::deque<QueuedPickup> m_world_candidates{};
        bool m_guard_claimed{};
        UObject* m_guard_source{};
        int32 m_guard_source_index{-1};
        uint64_t m_guard_fingerprint{};
    };
}
#define AUTOLOOT_NATIVE_PROBE_API __declspec(dllexport)

extern "C"
{
    AUTOLOOT_NATIVE_PROBE_API RC::CppUserModBase* start_mod()
    {
        RC::Output::send(STR("[AutoLootNativeProbe] LOAD marker={} export=start_mod\n"), Marker);
        return new AutoLootNativeProbe();
    }

    AUTOLOOT_NATIVE_PROBE_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        RC::Output::send(STR("[AutoLootNativeProbe] UNINSTALL marker={} export=uninstall_mod\n"), Marker);
        delete mod;
    }
}
