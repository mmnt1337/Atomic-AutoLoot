#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace autoloot::post7c3
{
    namespace perf
    {
        class AutoLoopControl final
        {
          public:
            explicit AutoLoopControl(bool enabled = false) noexcept : m_enabled(enabled) {}
            [[nodiscard]] auto enabled() const noexcept -> bool { return m_enabled.load(std::memory_order_acquire); }
            [[nodiscard]] auto admit_auto() const noexcept -> bool { return enabled(); }
            auto set(bool enabled) noexcept -> void { m_enabled.store(enabled, std::memory_order_release); }
            auto toggle() noexcept -> bool
            {
                bool current = m_enabled.load(std::memory_order_acquire);
                while (!m_enabled.compare_exchange_weak(current, !current, std::memory_order_acq_rel)) {}
                return !current;
            }

          private:
            std::atomic_bool m_enabled;
        };
    }

    struct AllowlistedTuple
    {
        std::string source_class;
        std::string layout;
        std::string item_class;
        std::string inventory_data;
        int32_t amount{};
    };

    // Source classification is deliberately class-based. Instance names are transient
    // map data and must not be part of the production allowlist.
    struct SourceProfile
    {
        std::string_view source_class;
        bool trace_complete;
        bool single_record_safe;
    };

    inline constexpr SourceProfile SourceProfiles[] = {
        {"BP_LootBox_Box_C", true, true},
        {"BP_LootBox_Crate_C", true, true},
        {"BP_LootBox_Commode_C", true, true},
        {"BP_LootBox_Table_C", true, true},
        {"BP_LootBox_Wardrobe_24_C", true, true},
        {"BP_Lootbox_Wardrobe_23_C", true, true},
        {"BP_LootBox_Cardfile_Shelf_C", true, true},
        // Corpse LootBoxComponent surface (R47 seq3/4 + 2026-08-04 UE4SS F11 recordings).
        {"BP_VovCharacter_Invalid_C", true, true},
        {"BP_VovCharacter_Invalid1_C", true, true},
        {"BP_VovCharacter_Invalid2_C", true, true},
        {"BP_VovCharacter_Invalid3_C", true, true},
        {"BP_VovCharacter_C", true, true},
        {"BP_VovCharacter_Fast_C", true, true},
        {"BP_DestructibleChicken_C", false, false},
        // 2026-08-04 F11: world-drop after destroy; no LootBoxComponent. Drops already collected via WORLD_PICKUP.
        {"BP_DestructibleUtka_C", false, false},
        {"BP_DestructiblePchela_Blue_C", false, false},
    };

    inline auto source_profile(std::string_view source_class) noexcept -> const SourceProfile*
    {
        for (const auto& profile : SourceProfiles)
        {
            if (profile.source_class == source_class)
            {
                return &profile;
            }
        }
        return nullptr;
    }

    inline const AllowlistedTuple AllowlistV0{
        "BP_LootBox_Box_C",
        "BP_ItemContainerLayout_Box_C",
        "BP_Pickable_Ammo_Pistol_C",
        "DA_Item_PistolAmmo",
        2,
    };

    struct LayoutProfile
    {
        std::string_view source_class;
        std::string_view layout_class;
    };

    inline constexpr LayoutProfile LayoutProfiles[] = {
        {"BP_LootBox_Box_C", "BP_ItemContainerLayout_Box_C"},
        {"BP_LootBox_Crate_C", "BP_ItemContainerLayout_Crate_WeaponLoot_C"},
        {"BP_LootBox_Commode_C", "BP_ItemContainerLayout_Commode_C"},
        {"BP_LootBox_Table_C", "BP_ItemContainerLayout_Table_C"},
        // The trace callback for Wardrobe_24 explicitly reports the table layout class.
        {"BP_LootBox_Wardrobe_24_C", "BP_ItemContainerLayout_Table_C"},
        {"BP_Lootbox_Wardrobe_23_C", "BP_ItemContainerLayout_Wardrobe_23_C"},
        {"BP_LootBox_Cardfile_Shelf_C", "BP_PickupableItemContainer_ShelfLoot_C"},
        // R47 seq3 Invalid10_8 → VovaInvalid; seq4 Invalid5_2 → Vova.
        // 2026-08-04 F11: VovCharacter/Invalid1/Invalid2 → Vova; Fast → VovaBlack.
        {"BP_VovCharacter_Invalid_C", "BP_PickupableItemContainer_VovaInvalid_C"},
        {"BP_VovCharacter_Invalid1_C", "BP_PickupableItemContainer_Vova_C"},
        {"BP_VovCharacter_Invalid2_C", "BP_PickupableItemContainer_Vova_C"},
        {"BP_VovCharacter_Invalid3_C", "BP_PickupableItemContainer_Vova_C"},
        {"BP_VovCharacter_C", "BP_PickupableItemContainer_Vova_C"},
        {"BP_VovCharacter_Fast_C", "BP_PickupableItemContainer_VovaBlack_C"},
    };

    struct ItemProfile
    {
        std::string_view inventory_data;
        std::string_view item_class;
    };

    // The catalog constrains the semantic item/class pair. Stack size is runtime data
    // from the source record and is intentionally not capped by trace observations.
    inline constexpr ItemProfile ItemProfiles[] = {
        {"DA_Item_PistolAmmo", "BP_Pickable_Ammo_Pistol_C"},
        {"DA_Item_ShotgunAmmo", "BP_Pickupable_Shotgun_Ammo_single_C"},
        // 2026-08-04 F11 loose world pickup: Magazine variant of the same ItemData.
        {"DA_Item_ShotgunAmmo", "BP_Pickupable_Shotgun_Ammo_Magazine_C"},
        {"DA_Item_AK47Ammo", "BP_Pickupable_AK47_Ammo_Magazine_C"},
        // 2026-08-04 F11 seq2 Utka world drop — class admitted for WORLD_PICKUP via catalog_pickup_name.
        // ItemData pair unknown; world path does not require it when class is catalogued.
        {"DA_Item_Jelly_Resource", "BP_Pickable_CapsuleJelly_C"},
        {"DA_Item_Resources_Metal_Parts", "BP_Pickupable_Metal_Parts_C"},
        {"DA_Item_Synthetic_Material_Resource", "BP_Pickable_Synthetic_Material_Resource_C"},
        {"DA_Item_Resource_Biomaterial", "BP_Pickable_Resource_Biomaterial_C"},
        {"DA_Item_Resources_Chemicals", "BP_Pickupable_Resources_Chemicals_C"},
        {"DA_Item_Resource_Superconductor", "BP_Pickupable_Resource_Superconductor_C"},
        {"DA_Item_Resource_Microelectronics", "BP_Pickupable_Resource_Microelectronics_C"},
        // R47 Forester Crate2 BEFORE 2026-08-04: LoadedPickupableClass on AttachedItems[1].
        {"DA_Item_Energy_Cell_Resource", "BP_Pickupable_Energy_Cell_Resource_C"},
        // The 2026-07-27 world-pickup trace observed two initialized small-aid actors.
        {"DA_Consumable_aid_small", "BP_Pickable_consumable_aid_small_C"},
        {"DA_Consumable_aid_medium", "BP_Pickable_consumable_aid_medium_C"},
        {"DA_Consumable_aid_big", "BP_Pickable_consumable_aid_big_C"},
        // 2026-08-04 object dump + floor/world R53: both pickup class variants share this ItemData.
        {"DA_Consumable_sguschenka", "BP_Pickable_sguschenka_C"},
        {"DA_Consumable_sguschenka", "BP_Pickupable_Sguschenka_C"},
        {"DA_Cassette_Electricity_Recipe", "BP_Pickable_Cassette_Electricity_Recipe_C"},
        // Trace 7 observed the Pashtet recipe using the cassette pickup implementation.
        {"DA_Item_Pashtet_Recipe", "BP_Pickable_Cassette_Electricity_Recipe_C"},
        // 2026-08-04 F11 Box11_2: only attached row was Fire cassette; blocked SAVED_ONLY of the rest.
        // Asset name keeps the game typo "FIre".
        {"DA_Item_Cassette_FIre", "BP_Pickable_Cassette_Fire_C"},
    };

    inline auto layout_supported(std::string_view source_class, std::string_view layout_class) noexcept -> bool
    {
        for (const auto& profile : LayoutProfiles)
        {
            if (profile.source_class == source_class && profile.layout_class == layout_class) return true;
        }
        return false;
    }

    inline auto item_supported(std::string_view inventory_data, std::string_view item_class, int32_t amount) noexcept -> bool
    {
        if (amount <= 0) return false;
        for (const auto& profile : ItemProfiles)
        {
            if (profile.inventory_data == inventory_data && profile.item_class == item_class)
            {
                return true;
            }
        }
        return false;
    }

    inline auto item_data_supported(std::string_view inventory_data, int32_t amount) noexcept -> bool
    {
        if (amount <= 0) return false;
        for (const auto& profile : ItemProfiles)
        {
            if (profile.inventory_data == inventory_data) return true;
        }
        return false;
    }

    inline auto item_class_for_data(std::string_view inventory_data) noexcept -> std::string_view
    {
        for (const auto& profile : ItemProfiles)
        {
            if (profile.inventory_data == inventory_data) return profile.item_class;
        }
        return {};
    }

    struct Descriptor
    {
        std::string source_class;
        std::string layout;
        std::string item_class;
        std::string inventory_data;
        int32_t amount{};
        uint64_t fingerprint{};
    };

    inline auto allowlisted(const Descriptor& descriptor) noexcept -> bool
    {
        const auto* profile = source_profile(descriptor.source_class);
        return profile && profile->single_record_safe && layout_supported(descriptor.source_class, descriptor.layout) &&
               item_supported(descriptor.inventory_data, descriptor.item_class, descriptor.amount);
    }

    enum class AdmissionResult : uint8_t
    {
        NoCandidate,
        Supported,
        Ambiguous,
        Unsupported,
        Stale,
    };

    inline auto classify_admission(int32_t matches, const Descriptor& descriptor, bool descriptor_is_live) noexcept -> AdmissionResult
    {
        if (matches <= 0) return AdmissionResult::NoCandidate;
        if (matches > 1) return AdmissionResult::Ambiguous;
        if (!descriptor_is_live || descriptor.fingerprint == 0) return AdmissionResult::Stale;
        if (!allowlisted(descriptor)) return AdmissionResult::Unsupported;
        return AdmissionResult::Supported;
    }

    class InFlightGuard final
    {
      public:
        explicit InFlightGuard(std::atomic_bool& flag) noexcept : m_flag(&flag), m_owned(!flag.exchange(true, std::memory_order_acq_rel)) {}
        ~InFlightGuard() { if (m_owned) m_flag->store(false, std::memory_order_release); }
        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;
        [[nodiscard]] auto acquired() const noexcept -> bool { return m_owned; }
      private:
        std::atomic_bool* m_flag;
        bool m_owned;
    };
}
