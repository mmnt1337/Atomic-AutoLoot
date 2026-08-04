#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Helpers/String.hpp>
#include <Input/KeyDef.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>

namespace
{
    using namespace RC;
    using namespace RC::Unreal;

    constexpr auto Marker = STR("ALNT_LOOT_TRIGGER_V1_20260805_C838A8AC");
    constexpr int32 MaxTraceEvents = 4000;
    constexpr int32 MaxProcessEventLogs = 800;
    constexpr size_t MaxParamDumpChars = 512;

    // Confirmed loot / interaction / animation surfaces from AutoLootTrace + NativeProbe.
    constexpr const TCHAR* CoreTracePaths[] = {
        STR("/Script/AtomicHeart.AHPickupableItem:SetInventoryDataAndUpdateMesh"),
        STR("/Script/AtomicHeart.AHInventoryPlayer:AddItemToInventory"),
        STR("/Script/AtomicHeart.AHLootBoxBase:SpawnLoot"),
        STR("/Script/AtomicHeart.AHLootBoxBase:OnContainerRemovedItem"),
        STR("/Script/AtomicHeart.AHLootBoxBase:OnContainerPurged"),
        STR("/Script/AtomicHeart.AbilityTask_OpenShelf:OpenShelf"),
        STR("/Script/GameplayTasks.GameplayTask:ReadyForActivation"),
        STR("/Script/AtomicHeart.AHLootBoxBase:OnStartedAnimation"),
        STR("/Script/AtomicHeart.AHLootBoxBase:OnFinishedAnimation"),
        STR("/Script/AtomicHeart.AHLootBoxBase:OnLootboxLooted"),
        STR("/Script/AtomicHeart.AHLootBoxBase:UpdateLootboxShelveGroups"),
        STR("/Script/AtomicHeart.ContinuousPickupAbility:OnGrabStateChanged"),
        STR("/Script/AtomicHeart.LootSpawnSubsystem:ReturnItemToPool"),
        STR("/Script/GameplayAbilities.GameplayAbility:ActivateAbility"),
        STR("/Script/GameplayAbilities.GameplayAbility:K2_ActivateAbility"),
        STR("/Script/GameplayAbilities.GameplayAbility:K2_ActivateAbilityFromEvent"),
        STR("/Script/GameplayAbilities.AbilitySystemComponent:TryActivateAbility"),
        STR("/Script/GameplayAbilities.AbilitySystemComponent:TryActivateAbilityByClass"),
        STR("/Script/GameplayAbilities.AbilitySystemComponent:InternalTryActivateAbility"),
        STR("/Script/Engine.AnimInstance:Montage_Play"),
        STR("/Script/Engine.AnimInstance:Montage_PlayWithBlendIn"),
        STR("/Script/Engine.AnimInstance:Montage_Stop"),
        STR("/Script/Engine.AnimInstance:Montage_Pause"),
        STR("/Script/Engine.AnimInstance:Montage_Resume"),
        STR("/Script/Engine.AnimInstance:Montage_JumpToSection"),
        STR("/Script/Engine.AnimInstance:PlaySlotAnimationAsDynamicMontage"),
        STR("/Script/Engine.AnimInstance:StopSlotAnimation"),
        STR("/Script/Engine.SkeletalMeshComponent:PlayAnimation"),
        STR("/Script/Engine.SkeletalMeshComponent:SetAnimation"),
        STR("/Script/Engine.SkeletalMeshComponent:SetAnimationMode"),
        STR("/Script/Engine.SkeletalMeshComponent:Stop"),
        STR("/Script/Engine.TimelineComponent:Play"),
        STR("/Script/Engine.TimelineComponent:PlayFromStart"),
        STR("/Script/Engine.TimelineComponent:Reverse"),
        STR("/Script/Engine.TimelineComponent:Stop"),
        STR("/Script/Engine.TimelineComponent:SetPlaybackPosition"),
        STR("/Script/MovieScene.MovieSceneSequencePlayer:Play"),
        STR("/Script/MovieScene.MovieSceneSequencePlayer:PlayReverse"),
        STR("/Script/MovieScene.MovieSceneSequencePlayer:Stop"),
        STR("/Script/MovieScene.MovieSceneSequencePlayer:GoToEndAndStop"),
        // Visual helpers (SetVisibility/SetHidden/SetComponentTickEnabled) are intentionally
        // NOT hard-hooked: without a locked target they flood. PE hunt still catches them
        // when the context name matches InterestNeedles (loot/cardfile/vov/...).
    };

    constexpr const char* InterestNeedles[] = {
        "loot", "inventory", "pickup", "shelf", "container", "grab", "ability",
        "interact", "openshelf", "spawnloot", "purge", "pool", "cardfile",
        "saved", "montage", "timeline", "anim", "vovcharacter", "itemcontainer",
        "continuouspickup", "returnitem", "additem", "remov", "gameplaytask",
        "activateability", "tryactivate", "onstarted", "onfinished", "onloot",
        "updateloot", "setinventory", "playanimation", "sequenceplayer",
    };

    constexpr const char* NoiseNeedles[] = {
        "receivetick",
        "blueprintupdatestart",
        "blueprintupdateanimation",
        "nativeupdateanimation",
        "receivetransformedupdated",
        "onaniminitialized",
    };

    struct ArmedHook
    {
        StringType path{};
        std::pair<int, int> ids{-1, -1};
    };

    std::atomic_bool g_trace_active{false};
    std::atomic_bool g_pe_hunt{true};
    std::atomic_bool g_verbose{false};
    std::atomic_int32_t g_event_count{0};
    std::atomic_int32_t g_pe_count{0};
    std::atomic_bool g_overflow_logged{false};
    std::vector<ArmedHook> g_armed_hooks{};
    std::atomic<Hook::GlobalCallbackId> g_pe_pre{Hook::ERROR_ID};
    std::atomic<Hook::GlobalCallbackId> g_pe_post{Hook::ERROR_ID};

    template <typename... Arguments>
    auto emit_log(const TCHAR* format, Arguments&&... arguments) -> void
    {
        RC::Output::send(format, std::forward<Arguments>(arguments)...);
    }

    auto to_lower_ascii(std::string text) -> std::string
    {
        for (char& ch : text)
        {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        }
        return text;
    }

    auto narrow(const StringType& text) -> std::string
    {
        std::string out;
        out.reserve(text.size());
        for (const auto ch : text) out.push_back(static_cast<char>(ch));
        return out;
    }

    auto number_string(int32 value) -> StringType
    {
        return ensure_str(std::to_string(value));
    }

    auto number_string(float value) -> StringType
    {
        return ensure_str(std::to_string(value));
    }

    auto contains_ci(std::string_view haystack_lower, std::string_view needle_lower) -> bool
    {
        return haystack_lower.find(needle_lower) != std::string_view::npos;
    }

    auto is_noise_path(std::string_view path_lower) -> bool
    {
        for (const char* needle : NoiseNeedles)
        {
            if (contains_ci(path_lower, needle)) return true;
        }
        return false;
    }

    auto is_interest_path(std::string_view path_lower, std::string_view context_lower) -> bool
    {
        for (const char* needle : InterestNeedles)
        {
            if (contains_ci(path_lower, needle) || contains_ci(context_lower, needle)) return true;
        }
        return false;
    }

    auto describe_object(UObject* object) -> StringType
    {
        if (!object) return STR("<null>");
        return object->GetFullName();
    }

    auto function_path_no_type(UFunction* function) -> StringType
    {
        if (!function) return STR("<null>");
        const auto full = function->GetFullName();
        const auto space = full.find(STR(" "));
        if (space != StringType::npos && space + 1 < full.size())
        {
            return full.substr(space + 1);
        }
        return full;
    }

    auto dump_params(UFunction* function, void* parms) -> StringType
    {
        if (!function || !parms) return STR("{}");
        StringType out = STR("{");
        bool first = true;
        try
        {
            for (FProperty* property : function->ForEachProperty())
            {
                if (!property) continue;
                if (property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
                if (out.size() > MaxParamDumpChars)
                {
                    out += STR(",...");
                    break;
                }
                if (!first) out += STR(", ");
                first = false;
                out += property->GetName();
                out += STR("=");
                void* value_ptr = property->ContainerPtrToValuePtr<void>(parms);
                if (auto* object_property = CastField<FObjectPropertyBase>(property))
                {
                    out += describe_object(object_property->GetObjectPropertyValue(value_ptr));
                }
                else if (auto* bool_property = CastField<FBoolProperty>(property))
                {
                    out += bool_property->GetPropertyValue(value_ptr) ? STR("true") : STR("false");
                }
                else if (auto* int_property = CastField<FIntProperty>(property))
                {
                    out += number_string(int_property->GetPropertyValue(value_ptr));
                }
                else if (auto* float_property = CastField<FFloatProperty>(property))
                {
                    out += number_string(float_property->GetPropertyValue(value_ptr));
                }
                else if (auto* name_property = CastField<FNameProperty>(property))
                {
                    out += name_property->GetPropertyValue(value_ptr).ToString();
                }
                else
                {
                    out += property->GetClass().GetName();
                }
            }
        }
        catch (...)
        {
            out += STR("ERR");
        }
        out += STR("}");
        return out;
    }

    auto snapshot_loot_hints(UObject* context) -> StringType
    {
        if (!context) return STR("");
        StringType hints;
        auto append_bool = [&](const TCHAR* name) {
            if (auto* property = CastField<FBoolProperty>(context->GetPropertyByNameInChain(name)))
            {
                if (!hints.empty()) hints += STR(" ");
                hints += name;
                hints += STR("=");
                hints += property->GetPropertyValueInContainer(context) ? STR("true") : STR("false");
            }
        };
        auto append_array_num = [&](const TCHAR* name) {
            if (auto* property = CastField<FArrayProperty>(context->GetPropertyByNameInChain(name)))
            {
                if (!hints.empty()) hints += STR(" ");
                hints += name;
                hints += STR(".Num=");
                hints += number_string(FScriptArrayHelper_InContainer{property, context}.Num());
            }
        };
        append_bool(STR("bLooted"));
        append_bool(STR("bIsEnabled"));
        append_bool(STR("LootBoxIsUsed"));
        append_array_num(STR("SavedItemsInShelves"));
        append_array_num(STR("AttachedContainers"));
        append_array_num(STR("SavedItems"));
        append_array_num(STR("AttachedItems"));
        append_array_num(STR("AttachedItemContainers"));
        if (hints.empty()) return STR("");
        return STR(" hints={") + hints + STR("}");
    }

    auto try_consume_event_slot() -> bool
    {
        const int32 next = g_event_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (next <= MaxTraceEvents) return true;
        bool expected = false;
        if (g_overflow_logged.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_LIMIT_REACHED marker={} limit={} further_events_suppressed=true\n"),
                     Marker, MaxTraceEvents);
        }
        return false;
    }

    auto log_call(bool process_event, bool is_post, const StringType& path, UObject* context, UFunction* function, void* parms)
        -> void
    {
        if (!g_trace_active.load(std::memory_order_acquire)) return;
        if (!try_consume_event_slot()) return;

        const int32 sequence = g_event_count.load(std::memory_order_relaxed);
        const auto phase = is_post ? STR("EXIT") : STR("ENTER");
        StringType params;
        StringType hints;
        if (!is_post)
        {
            params = dump_params(function, parms);
            hints = snapshot_loot_hints(context);
        }

        const StringType params_part =
            (!is_post && !params.empty() && params != STR("{}")) ? (STR(" params=") + params) : STR("");

        if (process_event)
        {
            emit_log(STR("[AutoLootNativeTrace] PROCESS_EVENT sequence={} phase={} function={} context={}{}{}\n"),
                     sequence, phase, path, describe_object(context), params_part, hints);
        }
        else
        {
            emit_log(STR("[AutoLootNativeTrace] CALL_TRACE sequence={} phase={} path={} context={}{}{}\n"),
                     sequence, phase, path, describe_object(context), params_part, hints);
        }
    }

    auto on_script_hook(UnrealScriptFunctionCallableContext& context, void* custom, bool is_post) -> void
    {
        if (!g_trace_active.load(std::memory_order_acquire)) return;
        auto* path = static_cast<const StringType*>(custom);
        UFunction* function = context.TheStack.Node();
        void* parms = context.TheStack.Locals();
        log_call(false, is_post, path ? *path : function_path_no_type(function), context.Context, function, parms);
    }

    auto should_log_process_event(UFunction* function, UObject* context) -> bool
    {
        if (!g_pe_hunt.load(std::memory_order_relaxed)) return false;
        if (!function) return false;

        const auto path = function_path_no_type(function);
        const auto path_lower = to_lower_ascii(narrow(path));
        if (!g_verbose.load(std::memory_order_relaxed) && is_noise_path(path_lower)) return false;

        const auto context_lower = to_lower_ascii(narrow(describe_object(context)));
        if (!is_interest_path(path_lower, context_lower)) return false;

        const int32 pe = g_pe_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        return pe <= MaxProcessEventLogs;
    }

    auto on_process_event(bool is_post, UObject* context, UFunction* function, void* parms) -> void
    {
        if (!g_trace_active.load(std::memory_order_acquire)) return;
        if (!should_log_process_event(function, context)) return;
        log_call(true, is_post, function_path_no_type(function), context, function, parms);
    }

    auto disarm_trace(const TCHAR* reason) -> void
    {
        g_trace_active.store(false, std::memory_order_release);

        const auto pe_pre = g_pe_pre.exchange(Hook::ERROR_ID, std::memory_order_acq_rel);
        if (pe_pre != Hook::ERROR_ID) Hook::UnregisterCallback(pe_pre);
        const auto pe_post = g_pe_post.exchange(Hook::ERROR_ID, std::memory_order_acq_rel);
        if (pe_post != Hook::ERROR_ID) Hook::UnregisterCallback(pe_post);

        int32 removed = 0;
        int32 failed = 0;
        for (auto& hook : g_armed_hooks)
        {
            try
            {
                UObjectGlobals::UnregisterHook(hook.path, hook.ids);
                ++removed;
            }
            catch (...)
            {
                ++failed;
            }
        }
        const int32 events = g_event_count.load(std::memory_order_relaxed);
        g_armed_hooks.clear();
        emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_DISARMED marker={} reason={} removed={} failed={} events={}\n"),
                 Marker, reason, removed, failed, events);
    }

    auto arm_trace() -> bool
    {
        if (!g_armed_hooks.empty() || g_pe_pre.load(std::memory_order_relaxed) != Hook::ERROR_ID)
        {
            disarm_trace(STR("rearm"));
        }

        g_event_count.store(0, std::memory_order_release);
        g_pe_count.store(0, std::memory_order_release);
        g_overflow_logged.store(false, std::memory_order_release);
        g_armed_hooks.clear();
        g_armed_hooks.reserve(std::size(CoreTracePaths));

        int32 armed = 0;
        int32 skipped = 0;
        for (const TCHAR* path : CoreTracePaths)
        {
            StringType path_string = path;
            auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, path_string.c_str(), false);
            if (!function)
            {
                ++skipped;
                emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_HOOK_SKIPPED path={} reason=not_found\n"), path_string);
                continue;
            }

            try
            {
                g_armed_hooks.push_back(ArmedHook{path_string, {-1, -1}});
                auto* stored = &g_armed_hooks.back().path;
                const auto ids = UObjectGlobals::RegisterHook(
                    function,
                    [](UnrealScriptFunctionCallableContext& ctx, void* custom) { on_script_hook(ctx, custom, false); },
                    [](UnrealScriptFunctionCallableContext& ctx, void* custom) { on_script_hook(ctx, custom, true); },
                    stored);
                g_armed_hooks.back().ids = ids;
                ++armed;
            }
            catch (const std::exception& ex)
            {
                ++skipped;
                g_armed_hooks.pop_back();
                emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_HOOK_SKIPPED path={} reason=register_failed what={}\n"),
                         path_string, ensure_str(ex.what()));
            }
            catch (...)
            {
                ++skipped;
                g_armed_hooks.pop_back();
                emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_HOOK_SKIPPED path={} reason=register_failed what=unknown\n"),
                         path_string);
            }
        }

        Hook::FCallbackOptions pe_options{};
        pe_options.bReadonly = true;
        pe_options.OwnerModName = STR("AutoLootNativeTrace");
        pe_options.HookName = STR("LootTriggerProcessEventPre");
        const auto pe_pre = Hook::RegisterProcessEventPreCallback(
            [](Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void* parms) {
                on_process_event(false, context, function, parms);
            },
            pe_options);
        pe_options.HookName = STR("LootTriggerProcessEventPost");
        const auto pe_post = Hook::RegisterProcessEventPostCallback(
            [](Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void* parms) {
                on_process_event(true, context, function, parms);
            },
            pe_options);

        g_pe_pre.store(pe_pre, std::memory_order_release);
        g_pe_post.store(pe_post, std::memory_order_release);

        if (armed == 0 && pe_pre == Hook::ERROR_ID)
        {
            emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_ARM_ERROR marker={} reason=no_hooks_armed skipped={}\n"),
                     Marker, skipped);
            return false;
        }

        g_trace_active.store(true, std::memory_order_release);
        emit_log(STR("[AutoLootNativeTrace] CALL_TRACE_ARMED marker={} hooks={} skipped={} pe_hunt={} verbose={} event_limit={} pe_limit={}\n"),
                 Marker,
                 armed,
                 skipped,
                 g_pe_hunt.load(std::memory_order_relaxed),
                 g_verbose.load(std::memory_order_relaxed),
                 MaxTraceEvents,
                 MaxProcessEventLogs);
        emit_log(STR("[AutoLootNativeTrace] RECORDING_INSTRUCTION interact_normally_then_press_F6_again=true\n"));
        return true;
    }

    class AutoLootNativeTrace final : public CppUserModBase
    {
      public:
        AutoLootNativeTrace()
        {
            ModName = STR("AutoLootNativeTrace");
            ModVersion = STR("v1.loot-trigger");
            ModDescription = STR("Native loot trigger/event tracer (F6 toggle)");
            ModAuthors = STR("Atomic Heart AutoLoot research");

            emit_log(STR("[AutoLootNativeTrace] START marker={} hotkey=F6 pe_toggle=CTRL+F6 verbose_toggle=CTRL+F7\n"), Marker);

            register_keydown_event(Input::Key::F6, []() {
                if (g_trace_active.load(std::memory_order_acquire))
                {
                    disarm_trace(STR("F6_stop"));
                    emit_log(STR("[AutoLootNativeTrace] RECORDING_DONE marker={} events={}\n"),
                             Marker, g_event_count.load(std::memory_order_relaxed));
                    return;
                }
                emit_log(STR("[AutoLootNativeTrace] HOTKEY_PRESSED F6 TOGGLE_RECORDING marker={}\n"), Marker);
                if (!arm_trace())
                {
                    emit_log(STR("[AutoLootNativeTrace] RECORDING_ABORT marker={} reason=no_hooks_armed\n"), Marker);
                }
                else
                {
                    emit_log(STR("[AutoLootNativeTrace] RECORDING_ACTIVE marker={}\n"), Marker);
                }
            });

            register_keydown_event(Input::Key::F6, {Input::ModifierKey::CONTROL}, []() {
                const bool enabled = !g_pe_hunt.load(std::memory_order_relaxed);
                g_pe_hunt.store(enabled, std::memory_order_release);
                emit_log(STR("[AutoLootNativeTrace] PE_HUNT_{} marker={} control=CTRL+F6\n"),
                         enabled ? STR("ENABLED") : STR("DISABLED"), Marker);
            });

            register_keydown_event(Input::Key::F7, {Input::ModifierKey::CONTROL}, []() {
                const bool enabled = !g_verbose.load(std::memory_order_relaxed);
                g_verbose.store(enabled, std::memory_order_release);
                emit_log(STR("[AutoLootNativeTrace] VERBOSE_{} marker={} control=CTRL+F7\n"),
                         enabled ? STR("ENABLED") : STR("DISABLED"), Marker);
            });
        }

        ~AutoLootNativeTrace() override
        {
            if (g_trace_active.load(std::memory_order_acquire) || !g_armed_hooks.empty() ||
                g_pe_pre.load(std::memory_order_relaxed) != Hook::ERROR_ID)
            {
                disarm_trace(STR("mod_unload"));
            }
            emit_log(STR("[AutoLootNativeTrace] STOP marker={}\n"), Marker);
        }

        auto on_unreal_init() -> void override
        {
            emit_log(STR("[AutoLootNativeTrace] UNREAL_INIT marker={} ready=true press_F6_to_record=true\n"), Marker);
        }
    };
}

#define AUTOLOOT_NATIVE_TRACE_API __declspec(dllexport)

extern "C"
{
    AUTOLOOT_NATIVE_TRACE_API RC::CppUserModBase* start_mod()
    {
        RC::Output::send(STR("[AutoLootNativeTrace] LOAD marker={} export=start_mod\n"), Marker);
        return new AutoLootNativeTrace();
    }

    AUTOLOOT_NATIVE_TRACE_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        RC::Output::send(STR("[AutoLootNativeTrace] UNINSTALL marker={} export=uninstall_mod\n"), Marker);
        delete mod;
    }
}
