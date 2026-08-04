#include <cassert>

#include "../src/orchestration.hpp"

using namespace autoloot::post7c3;

static auto fixture() -> Descriptor
{
    return Descriptor{
        AllowlistV0.source_class,
        AllowlistV0.layout,
        AllowlistV0.item_class,
        AllowlistV0.inventory_data,
        AllowlistV0.amount,
        0x84BD904B4C26CB88ull,
    };
}

int main()
{
    {
        perf::AutoLoopControl control{true};
        assert(control.admit_auto());
        control.set(false);
        assert(!control.admit_auto());
        assert(control.toggle());
        assert(control.admit_auto());
    }
    {
        const auto* box = source_profile("BP_LootBox_Box_C");
        const auto* crate = source_profile("BP_LootBox_Crate_C");
        const auto* commode = source_profile("BP_LootBox_Commode_C");
        assert(box && box->trace_complete && box->single_record_safe);
        assert(crate && crate->trace_complete && crate->single_record_safe);
        assert(commode && commode->trace_complete && commode->single_record_safe);
        assert(source_profile("BP_LootBox_Table_C")->single_record_safe);
        assert(source_profile("BP_LootBox_Wardrobe_24_C")->single_record_safe);
        assert(source_profile("BP_Lootbox_Wardrobe_23_C")->single_record_safe);
        assert(source_profile("BP_LootBox_Cardfile_Shelf_C")->single_record_safe);
        assert(source_profile("BP_VovCharacter_Invalid_C")->single_record_safe);
        assert(source_profile("BP_VovCharacter_Invalid1_C")->single_record_safe);
        assert(source_profile("BP_VovCharacter_Invalid2_C")->single_record_safe);
        assert(source_profile("BP_VovCharacter_Invalid3_C")->single_record_safe);
        assert(source_profile("BP_VovCharacter_C")->single_record_safe);
        assert(source_profile("BP_VovCharacter_Fast_C")->single_record_safe);
        assert(source_profile("BP_DestructibleChicken_C") && !source_profile("BP_DestructibleChicken_C")->single_record_safe);
        assert(source_profile("BP_DestructibleUtka_C") && !source_profile("BP_DestructibleUtka_C")->single_record_safe);
        assert(source_profile("BP_DestructiblePchela_Blue_C") && !source_profile("BP_DestructiblePchela_Blue_C")->single_record_safe);
        assert(source_profile("BP_LootBox_Box_2") == nullptr);
    }
    {
        assert(layout_supported("BP_LootBox_Crate_C", "BP_ItemContainerLayout_Crate_WeaponLoot_C"));
        assert(layout_supported("BP_LootBox_Wardrobe_24_C", "BP_ItemContainerLayout_Table_C"));
        assert(layout_supported("BP_LootBox_Cardfile_Shelf_C", "BP_PickupableItemContainer_ShelfLoot_C"));
        assert(layout_supported("BP_VovCharacter_Invalid_C", "BP_PickupableItemContainer_VovaInvalid_C"));
        assert(layout_supported("BP_VovCharacter_Invalid1_C", "BP_PickupableItemContainer_Vova_C"));
        assert(layout_supported("BP_VovCharacter_Invalid2_C", "BP_PickupableItemContainer_Vova_C"));
        assert(layout_supported("BP_VovCharacter_Invalid3_C", "BP_PickupableItemContainer_Vova_C"));
        assert(layout_supported("BP_VovCharacter_C", "BP_PickupableItemContainer_Vova_C"));
        assert(layout_supported("BP_VovCharacter_Fast_C", "BP_PickupableItemContainer_VovaBlack_C"));
        assert(!layout_supported("BP_LootBox_Crate_C", "BP_ItemContainerLayout_Box_C"));
        assert(item_supported("DA_Item_PistolAmmo", "BP_Pickable_Ammo_Pistol_C", 1));
        assert(item_supported("DA_Item_PistolAmmo", "BP_Pickable_Ammo_Pistol_C", 6));
        assert(item_supported("DA_Item_ShotgunAmmo", "BP_Pickupable_Shotgun_Ammo_single_C", 13));
        assert(item_supported("DA_Item_ShotgunAmmo", "BP_Pickupable_Shotgun_Ammo_Magazine_C", 5));
        assert(item_supported("DA_Item_AK47Ammo", "BP_Pickupable_AK47_Ammo_Magazine_C", 15));
        assert(item_supported("DA_Item_Resource_Biomaterial", "BP_Pickable_Resource_Biomaterial_C", 2));
        assert(item_supported("DA_Item_Resource_Microelectronics", "BP_Pickupable_Resource_Microelectronics_C", 1));
        assert(item_supported("DA_Item_Energy_Cell_Resource", "BP_Pickupable_Energy_Cell_Resource_C", 1));
        assert(item_supported("DA_Consumable_aid_small", "BP_Pickable_consumable_aid_small_C", 1));
        assert(item_supported("DA_Consumable_aid_medium", "BP_Pickable_consumable_aid_medium_C", 1));
        assert(item_class_for_data("DA_Item_Energy_Cell_Resource") == "BP_Pickupable_Energy_Cell_Resource_C");
        assert(item_supported("DA_Consumable_aid_big", "BP_Pickable_consumable_aid_big_C", 1));
        assert(item_supported("DA_Consumable_sguschenka", "BP_Pickable_sguschenka_C", 1));
        assert(item_supported("DA_Consumable_sguschenka", "BP_Pickupable_Sguschenka_C", 1));
        assert(item_class_for_data("DA_Consumable_sguschenka") == "BP_Pickable_sguschenka_C");
        assert(item_supported("DA_Cassette_Electricity_Recipe", "BP_Pickable_Cassette_Electricity_Recipe_C", 1));
        assert(item_supported("DA_Item_Pashtet_Recipe", "BP_Pickable_Cassette_Electricity_Recipe_C", 1));
        assert(item_supported("DA_Item_Cassette_FIre", "BP_Pickable_Cassette_Fire_C", 1));
        assert(item_class_for_data("DA_Item_Cassette_FIre") == "BP_Pickable_Cassette_Fire_C");
        assert(!item_supported("DA_Item_PistolAmmo", "BP_Pickable_Ammo_Pistol_C", 0));
        assert(!item_supported("DA_Item_PistolAmmo", "Unknown", 7));
        assert(item_data_supported("DA_Item_PistolAmmo", 1024));
        assert(item_data_supported("DA_Item_Energy_Cell_Resource", 1));
        assert(!item_data_supported("DA_Item_PistolAmmo", -1));
    }
    {
        const auto descriptor = fixture();
        assert(allowlisted(descriptor));
        assert(classify_admission(0, descriptor, true) == AdmissionResult::NoCandidate);
        assert(classify_admission(1, descriptor, true) == AdmissionResult::Supported);
        assert(classify_admission(2, descriptor, true) == AdmissionResult::Ambiguous);
        auto uncapped = descriptor;
        uncapped.amount = 64;
        assert(classify_admission(1, uncapped, true) == AdmissionResult::Supported);
        auto unsupported = descriptor;
        unsupported.item_class = "Unknown";
        assert(classify_admission(1, unsupported, true) == AdmissionResult::Unsupported);
        assert(classify_admission(1, descriptor, false) == AdmissionResult::Stale);
    }
    {
        std::atomic_bool in_flight{false};
        {
            InFlightGuard first{in_flight};
            assert(first.acquired());
            InFlightGuard second{in_flight};
            assert(!second.acquired());
        }
        assert(!in_flight.load());
        InFlightGuard third{in_flight};
        assert(third.acquired());
    }
    return 0;
}
