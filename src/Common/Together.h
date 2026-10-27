#pragma once

// What both plugins share: their log file, and finding Skyrim Together's Plugin API V2.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

// CommonLib refuses Windows headers that come before it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "SkyrimTogetherAPI.h"

namespace Together
{
namespace STAPI = SkyrimTogetherAPI;

// The local player's form ID, which Skyrim Together syncs like any actor.
inline constexpr std::uint32_t kPlayerFormId = 0x14;

inline void InitLog(const char* acpFileName)
{
    auto path = SKSE::log::log_directory();
    if (!path)
        return;
    *path /= acpFileName;
    auto logger = std::make_shared<spdlog::logger>("global", std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true));
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

// Skyrim Together exports kRequestPluginAPIName from its own executable. Null when not running under Skyrim
// Together, or under one without Plugin API V2 or with the old export name (before 1.8-playtest cd296c28).
inline STAPI::IVSkyrimTogether2* FindApi()
{
    const auto pRequest = reinterpret_cast<STAPI::RequestPluginAPI_t>(GetProcAddress(GetModuleHandleW(nullptr), STAPI::kRequestPluginAPIName));
    if (!pRequest)
    {
        SKSE::log::info("{} not found: not running under a Skyrim Together build with it, staying inactive", STAPI::kRequestPluginAPIName);
        return nullptr;
    }

    auto* pApi = static_cast<STAPI::IVSkyrimTogether2*>(pRequest(STAPI::InterfaceVersion::V2));
    if (!pApi)
        SKSE::log::info("Skyrim Together does not offer Plugin API V2, staying inactive");
    return pApi;
}

// Plugin API V3 from the same Skyrim Together build, or null when it predates V3 (do without what V3 adds).
inline STAPI::IVSkyrimTogether3* FindApi3()
{
    const auto pRequest = reinterpret_cast<STAPI::RequestPluginAPI_t>(GetProcAddress(GetModuleHandleW(nullptr), STAPI::kRequestPluginAPIName));
    return pRequest ? static_cast<STAPI::IVSkyrimTogether3*>(pRequest(STAPI::InterfaceVersion::V3)) : nullptr;
}

namespace Detail
{
// Reads 8 bytes, or returns false instead of crashing on an address that is not mapped.
inline bool TryRead(std::uintptr_t aAddress, std::uintptr_t& aValue) noexcept
{
    __try
    {
        aValue = *reinterpret_cast<const std::uintptr_t*>(aAddress);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// True when apObject's vtable is the game's BSScript::Internal::VirtualMachine one, read from the game's own RTTI.
inline bool IsScriptEngine(std::uintptr_t aObject) noexcept
{
    const auto cBase = REL::Module::get().base();
    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(cBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(cBase)->e_lfanew);
    const auto cEnd = cBase + pNt->OptionalHeader.SizeOfImage;
    const auto inImage = [&](std::uintptr_t aAddress) { return aAddress >= cBase && aAddress < cEnd; };

    // The object's vtable, the complete object locator just before it, then the type descriptor's mangled name.
    std::uintptr_t vtable = 0, locator = 0;
    if (!aObject || !TryRead(aObject, vtable) || !inImage(vtable) || !TryRead(vtable - 8, locator) || !inImage(locator))
        return false;
    const auto cTypeRva = *reinterpret_cast<const std::uint32_t*>(locator + 12);
    const auto cType = cBase + cTypeRva;
    if (!inImage(cType + 0x10))
        return false;
    constexpr std::string_view cName = ".?AVVirtualMachine@Internal@BSScript@@";
    return std::memcmp(reinterpret_cast<const char*>(cType + 0x10), cName.data(), cName.size() + 1) == 0;
}
} // namespace Detail

// The Papyrus script engine, or null. SkyrimVM keeps it at 0x200 on 1.6 and at 0x210 on 1.7 (Skyrim Together's
// GameVM.h), while this CommonLib reads 0x200 on every runtime: on 1.7 its VirtualMachine::GetSingleton() hands back a
// different object, whose calls do nothing. Only an object whose RTTI names the script engine is returned.
inline RE::BSScript::Internal::VirtualMachine* GetVirtualMachine()
{
    static std::atomic<std::ptrdiff_t> s_offset{-1};
    static std::atomic_flag s_logged;

    const auto cSkyrimVm = reinterpret_cast<std::uintptr_t>(RE::SkyrimVM::GetSingleton());
    if (!cSkyrimVm)
        return nullptr;

    if (const auto cOffset = s_offset.load(std::memory_order_relaxed); cOffset >= 0)
        return *reinterpret_cast<RE::BSScript::Internal::VirtualMachine**>(cSkyrimVm + cOffset);

    // The runtime's own offset first, then the other one in case a runtime moved it again.
    const bool cIs17 = !REL::Module::IsVR() && REL::Module::get().version() >= REL::Version(1, 7, 0, 0);
    const std::ptrdiff_t cOffsets[] = {cIs17 ? 0x210 : 0x200, cIs17 ? 0x200 : 0x210};
    for (const auto cOffset : cOffsets)
    {
        std::uintptr_t object = 0;
        if (Detail::TryRead(cSkyrimVm + cOffset, object) && Detail::IsScriptEngine(object))
        {
            s_offset.store(cOffset, std::memory_order_relaxed);
            SKSE::log::info("Papyrus VM found at SkyrimVM+0x{:X} (Skyrim {})", cOffset, REL::Module::get().version().string("."));
            return reinterpret_cast<RE::BSScript::Internal::VirtualMachine*>(object);
        }
    }

    if (!s_logged.test_and_set())
        SKSE::log::error("Papyrus VM not found at SkyrimVM+0x200 or +0x210 (Skyrim {}): Papyrus calls are skipped", REL::Module::get().version().string("."));
    return nullptr;
}

// Arguments for a Papyrus call (DispatchStaticCall), packed with the script engine from GetVirtualMachine(). Use this
// instead of RE::MakeFunctionArguments for anything with an Actor or another form: CommonLib packs forms with its own
// VirtualMachine::GetSingleton(), which reads SkyrimVM+0x200 on every runtime, another object on 1.7.x. There, a form
// argument arrives as None (Mfg Fix logs "No actor selected"), or the call crashes inside the script engine. Packed
// like CommonLib's PackHandle otherwise. Takes forms (pointers to TESForm types), std::int32_t, bool, float, strings.
namespace Detail
{
using Variable = RE::BSScript::Variable;
using Vm = RE::BSScript::Internal::VirtualMachine;

inline void PackForm(Vm& aVm, Variable& aDst, const void* apForm, RE::VMTypeID aTypeId)
{
    aDst.SetNone();
    if (!apForm)
        return;

    RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> classPtr;
    auto* pPolicy = aVm.GetObjectHandlePolicy();
    if (!aVm.GetScriptObjectType(aTypeId, classPtr) || !classPtr || !pPolicy)
        return;

    const auto cHandle = pPolicy->GetHandleForObject(aTypeId, apForm);
    RE::BSTSmartPointer<RE::BSScript::Object> object;
    if (!aVm.FindBoundObject(cHandle, classPtr->GetName(), object) && aVm.CreateObject(classPtr->GetName(), object) && object)
    {
        // A new script object for this form: bind it to the form's handle.
        RE::VMTypeID typeId = 0;
        if (aVm.GetTypeIDForScriptObject(classPtr->GetName(), typeId) && pPolicy->HandleIsType(typeId, cHandle) && pPolicy->IsHandleObjectAvailable(cHandle))
        {
            if (auto* pBindPolicy = aVm.GetObjectBindPolicy())
                pBindPolicy->BindObject(object, cHandle);
        }
    }
    if (object)
        aDst.SetObject(object, classPtr->GetRawType());
}

inline void PackArgument(Vm&, Variable& aDst, std::int32_t aValue) { aDst.SetSInt(aValue); }
inline void PackArgument(Vm&, Variable& aDst, bool aValue) { aDst.SetBool(aValue); }
inline void PackArgument(Vm&, Variable& aDst, float aValue) { aDst.SetFloat(aValue); }
inline void PackArgument(Vm&, Variable& aDst, const std::string& acValue) { aDst.SetString(acValue); }
inline void PackArgument(Vm&, Variable& aDst, const RE::BSFixedString& acValue) { aDst.SetString(acValue.c_str() ? acValue.c_str() : ""); }

template <class T>
    requires std::is_base_of_v<RE::TESForm, T>
void PackArgument(Vm& aVm, Variable& aDst, T* apForm)
{
    PackForm(aVm, aDst, apForm, static_cast<RE::VMTypeID>(T::FORMTYPE));
}

template <class... Args>
class Arguments final : public RE::BSScript::IFunctionArguments
{
public:
    explicit Arguments(Args... aArgs)
        : m_args(std::move(aArgs)...)
    {
    }

    // Runs when the script engine starts the call.
    bool operator()(RE::BSScrapArray<Variable>& aDst) const override
    {
        auto* pVm = GetVirtualMachine();
        if (!pVm)
            return false;
        aDst.resize(sizeof...(Args));
        std::size_t index = 0;
        std::apply([&](const auto&... acArgs) { (PackArgument(*pVm, aDst[index++], acArgs), ...); }, m_args);
        return true;
    }

private:
    std::tuple<Args...> m_args;
};
} // namespace Detail

template <class... Args>
RE::BSScript::IFunctionArguments* MakeArguments(Args... aArgs)
{
    return new Detail::Arguments<Args...>(std::move(aArgs)...);
}

inline bool StartsWithNoCase(std::string_view aText, std::string_view aPrefix) noexcept
{
    return aText.size() >= aPrefix.size() && _strnicmp(aText.data(), aPrefix.data(), aPrefix.size()) == 0;
}

inline const char* ToString(STAPI::SendResult aResult) noexcept
{
    switch (aResult)
    {
    case STAPI::SendResult::Ok: return "ok";
    case STAPI::SendResult::NotConnected: return "not connected";
    case STAPI::SendResult::NotInParty: return "not in a party";
    case STAPI::SendResult::TooLarge: return "too large";
    case STAPI::SendResult::BadChannel: return "bad channel";
    case STAPI::SendResult::NotSupported: return "server without Plugin API V2";
    case STAPI::SendResult::NotOwner: return "not the owner";
    }
    return "?";
}
} // namespace Together
