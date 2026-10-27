#pragma once

// The part of RaceMenu's (SKEE's) plugin interfaces this plugin uses, declared in RaceMenu's order: the virtual
// functions must match skee64/IPluginInterface.h (expired6978/SKSE64Plugins) slot for slot. RaceMenu hands out its
// interface map to any plugin that sends SKEE the exchange message.

#include <cstdint>

namespace RE
{
class TESObjectREFR;
class NiAVObject;
class BSGeometry;
class NiSkinPartition;
class NiBinaryExtraData;
} // namespace RE

namespace Skee
{
class IPluginInterface
{
public:
    virtual ~IPluginInterface() = default;
    virtual std::uint32_t GetVersion() = 0;
    virtual void Revert() = 0;
};

class IInterfaceMap
{
public:
    virtual IPluginInterface* QueryInterface(const char* acpName) = 0;
    virtual bool AddInterface(const char* acpName, IPluginInterface* apInterface) = 0;
    virtual IPluginInterface* RemoveInterface(const char* acpName) = 0;
};

struct InterfaceExchangeMessage
{
    static constexpr std::uint32_t kMessage_ExchangeInterface = 0x9E3779B9;
    IInterfaceMap* InterfaceMap = nullptr;
};

// "BodyMorph". Declared as of version 5 (current RaceMenu for 1.6 and 1.7); an older version may lay out its slots
// differently, so it is not called at all.
class IBodyMorphInterface : public IPluginInterface
{
public:
    static constexpr std::uint32_t kMinimumVersion = 5;

    class MorphKeyVisitor;
    class StringVisitor;
    class ActorVisitor;
    class MorphValueVisitor;
    class MorphVisitor;

    virtual void SetMorph(RE::TESObjectREFR* apRef, const char* acpMorphName, const char* acpMorphKey, float aRelative) = 0;
    virtual float GetMorph(RE::TESObjectREFR* apRef, const char* acpMorphName, const char* acpMorphKey) = 0;
    virtual void ClearMorph(RE::TESObjectREFR* apRef, const char* acpMorphName, const char* acpMorphKey) = 0;

    virtual float GetBodyMorphs(RE::TESObjectREFR* apRef, const char* acpMorphName) = 0;
    virtual void ClearBodyMorphNames(RE::TESObjectREFR* apRef, const char* acpMorphName) = 0;

    virtual void VisitMorphs(RE::TESObjectREFR* apRef, MorphVisitor& aVisitor) = 0;
    virtual void VisitKeys(RE::TESObjectREFR* apRef, const char* acpName, MorphKeyVisitor& aVisitor) = 0;
    virtual void VisitMorphValues(RE::TESObjectREFR* apRef, MorphValueVisitor& aVisitor) = 0;

    virtual void ClearMorphs(RE::TESObjectREFR* apRef) = 0;

    virtual void ApplyVertexDiff(RE::TESObjectREFR* apRef, RE::NiAVObject* apRootNode, bool aErase = false) = 0;

    virtual void ApplyBodyMorphs(RE::TESObjectREFR* apRef, bool aDeferUpdate = true) = 0;
    virtual void UpdateModelWeight(RE::TESObjectREFR* apRef, bool aImmediate = false) = 0;

    virtual void SetCacheLimit(std::uint64_t aLimit) = 0;
    virtual bool HasMorphs(RE::TESObjectREFR* apRef) = 0;
    virtual std::uint32_t EvaluateBodyMorphs(RE::TESObjectREFR* apRef) = 0;

    virtual bool HasBodyMorph(RE::TESObjectREFR* apRef, const char* acpMorphName, const char* acpMorphKey) = 0;
    virtual bool HasBodyMorphName(RE::TESObjectREFR* apRef, const char* acpMorphName) = 0;
    virtual bool HasBodyMorphKey(RE::TESObjectREFR* apRef, const char* acpMorphKey) = 0;
};
} // namespace Skee
