// The Skyrim Together side of LipSync Together: what the 0.8 add-on did inside the Skyrim Together exe, on the Plugin
// API instead (V2 for the messages, V3 for the voice mix and the server's address).
//
// The plugin part (plugin/) turns the microphone into five mouth shapes. This reads them 30 times a second, puts them
// on the player's own face and sends them as a message about the player's character, which Skyrim Together relays to
// the players in range (unreliable: the next one replaces a lost one) and never stores. Other clients hold what they
// receive for a short delay to even out the network, smooth it, and put it on that player's copy; when the speaker goes
// quiet the copy's mouth eases shut and is left alone again.
//
// Faces are moved with Mfg Fix's MfgConsoleFunc.SetPhonemeModifier, called directly (see "Faces" below), or as a
// Papyrus call when it can't be. Only phonemes are written, never expressions or modifiers, and
// only the phonemes this plugin raised are put back to zero. Without Mfg Fix, mouth states are still sent; faces just
// do not move on that machine.
//
// With voice chat on, the plugin part also carries the voices, through its own relay on the Skyrim Together server's
// host: this tells it the server and the local player, and 30 times a second how far away and on which side each
// other player's character stands. Each player's voice key, which lets the relay admit only players of this server,
// goes to every player through Skyrim Together. Mouths of players whose voice is heard follow that voice as it plays.

#include "Common/Together.h"

#include "Plugin.h"
#include "Window.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
using namespace Together;
using Clock = std::chrono::steady_clock;

constexpr const char* kChannelName = "LipSyncTogether.v2";
// Voice keys go to every player as "K<32 hex digits>", again now and then for players who join later.
constexpr char kKeyPrefix = 'K';
constexpr double kVoiceKeyRepeat = 20.0;

// A mouth state is three characters: the jaw (0-7), the strongest lip shape (N none, W wide, R round, L lips closed,
// T teeth) and its strength (0 for N, else 1-3). "-" means the speaker went quiet.
constexpr const char* kQuietState = "-";
constexpr int kJawLevels = 7;
constexpr int kShapeLevels = 3;

// At most 30 states a second, an unchanged state at most every 0.2 s to show the speaker is still talking, and a copy
// that hears nothing for 0.6 s closes its mouth.
constexpr double kMinSendInterval = 1.0 / 30.0;
constexpr double kRepeatInterval = 0.2;
constexpr double kRemoteTimeout = 0.6;
constexpr size_t kMaxQueued = 32;

// How often the main thread runs this (faces, states, the voice mix).
constexpr auto kTick = std::chrono::milliseconds(33);

// Mfg Fix's SetPhonemeModifier mode for phonemes, and Skyrim's phoneme ids.
constexpr std::int32_t kModePhoneme = 0;
enum Phoneme : std::uint32_t
{
    kAah = 0,
    kBigAah = 1,
    kBMP = 2,
    kChJSh = 3,
    kDST = 4,
    kEee = 5,
    kEh = 6,
    kFV = 7,
    kI = 8,
    kOh = 11,
    kOohQ = 12,
    kPhonemeCount = 16,
};

struct Shape
{
    float Open{};
    float Wide{};
    float Round{};
    float Lips{};
    float Teeth{};

    [[nodiscard]] bool IsClosed() const noexcept { return Open < 0.01f && Wide < 0.01f && Round < 0.01f && Lips < 0.01f && Teeth < 0.01f; }
};

float Clamp01(float aValue) noexcept
{
    return std::clamp(aValue, 0.f, 1.f);
}

std::string EncodeState(const Shape& acMouth)
{
    const int cJaw = std::clamp(static_cast<int>(std::lround(acMouth.Open * kJawLevels)), 0, kJawLevels);

    const std::pair<char, float> cShapes[] = {{'W', acMouth.Wide}, {'R', acMouth.Round}, {'L', acMouth.Lips}, {'T', acMouth.Teeth}};
    const auto cStrongest = std::max_element(std::begin(cShapes), std::end(cShapes), [](const auto& a, const auto& b) { return a.second < b.second; });

    std::string state(1, static_cast<char>('0' + cJaw));
    if (cStrongest->second < 0.2f)
        return state + "N0";
    state.push_back(cStrongest->first);
    state.push_back(static_cast<char>('0' + std::clamp(static_cast<int>(std::lround(cStrongest->second * kShapeLevels)), 1, kShapeLevels)));
    return state;
}

bool DecodeState(std::string_view aState, Shape& aOut) noexcept
{
    aOut = Shape{};
    if (aState == kQuietState)
        return true;
    if (aState.size() != 3 || aState[0] < '0' || aState[0] > '0' + kJawLevels || aState[2] < '0' || aState[2] > '0' + kShapeLevels)
        return false;

    aOut.Open = static_cast<float>(aState[0] - '0') / kJawLevels;
    const float cStrength = static_cast<float>(aState[2] - '0') / kShapeLevels;
    switch (aState[1])
    {
    case 'N': break;
    case 'W': aOut.Wide = cStrength; break;
    case 'R': aOut.Round = cStrength; break;
    case 'L': aOut.Lips = cStrength; break;
    case 'T': aOut.Teeth = cStrength; break;
    default: return false;
    }
    return true;
}

// The five shapes as Skyrim phonemes, 0 to 100. The weights stay near what vanilla dialogue uses: ordinary speech is a
// modest Aah and Eh, spread and rounded lips only add some Eee/I or OohQ/Oh on top, and BigAah is for shouting.
void ToPhonemes(const Shape& acMouth, float aStrength, int (&aOut)[kPhonemeCount]) noexcept
{
    float weight[kPhonemeCount]{};
    const float cOpen = acMouth.Open * (1.f - 0.6f * acMouth.Lips);
    weight[kAah] = 0.55f * cOpen * (1.f - 0.5f * acMouth.Round) * (1.f - 0.4f * acMouth.Wide);
    weight[kBigAah] = 0.3f * Clamp01((acMouth.Open - 0.85f) / 0.15f);
    weight[kEh] = 0.3f * cOpen * (1.f - acMouth.Round);
    weight[kEee] = 0.4f * acMouth.Wide;
    weight[kI] = 0.25f * acMouth.Wide * acMouth.Open;
    weight[kOohQ] = 0.45f * acMouth.Round;
    weight[kOh] = 0.3f * acMouth.Round * acMouth.Open;
    weight[kBMP] = 0.5f * acMouth.Lips;
    weight[kFV] = 0.35f * acMouth.Teeth;
    weight[kDST] = 0.25f * acMouth.Teeth;
    weight[kChJSh] = 0.15f * acMouth.Teeth;

    for (std::uint32_t i = 0; i < kPhonemeCount; ++i)
        aOut[i] = std::clamp(static_cast<int>(std::lround(weight[i] * aStrength * 100.f)), 0, 100);
}

// Moves aCurrent toward aTarget, opening faster than closing. The sender has already smoothed the voice, so this only
// rounds off the steps between received states.
void SmoothToward(Shape& aCurrent, const Shape& acTarget, double aDelta) noexcept
{
    const float cAttack = 1.f - std::exp(static_cast<float>(-aDelta / 0.015));
    const float cRelease = 1.f - std::exp(static_cast<float>(-aDelta / 0.035));
    auto step = [&](float& aValue, float aGoal)
    {
        aValue += (aGoal - aValue) * (aGoal > aValue ? cAttack : cRelease);
        if (aValue < 0.005f)
            aValue = 0.f;
    };
    step(aCurrent.Open, acTarget.Open);
    step(aCurrent.Wide, acTarget.Wide);
    step(aCurrent.Round, acTarget.Round);
    step(aCurrent.Lips, acTarget.Lips);
    step(aCurrent.Teeth, acTarget.Teeth);
}

// ---- Faces (main thread) ---------------------------------------------------------------------------------------

// Faces move through Mfg Fix's MfgConsoleFunc.SetPhonemeModifier, called directly as LipSync Together 0.8's add-on in
// the Skyrim Together exe did: a direct call costs next to nothing, where a Papyrus call per phoneme would queue up to
// 16 calls per face 30 times a second on the script engine. The function is caught as the script engine binds it
// (BindNativeMethod, slot 0x18 of its vtable on 1.6 and 1.7; Skyrim Together binds its own natives through the same
// slot), and its C++ function is only called after the checks below. If they fail, the Papyrus call is used instead.

using SetPhonemeModifier_t = bool (*)(RE::StaticFunctionTag*, RE::Actor*, std::int32_t, std::uint32_t, std::int32_t);

std::atomic<RE::BSScript::IFunction*> g_pSetPhonemeNative{nullptr};
std::uintptr_t g_bindNativeMethod = 0;

bool BindNativeMethodHook(RE::BSScript::IVirtualMachine* apThis, RE::BSScript::IFunction* apFunction)
{
    if (apFunction && !g_pSetPhonemeNative.load())
    {
        const char* pType = apFunction->GetObjectTypeName().c_str();
        const char* pName = apFunction->GetName().c_str();
        if (pType && pName && !_stricmp(pType, "MfgConsoleFunc") && !_stricmp(pName, "SetPhonemeModifier"))
            g_pSetPhonemeNative.store(apFunction);
    }
    return reinterpret_cast<bool (*)(RE::BSScript::IVirtualMachine*, RE::BSScript::IFunction*)>(g_bindNativeMethod)(apThis, apFunction);
}

bool IsReadable(const void* apAddress, size_t aSize) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!apAddress || !VirtualQuery(apAddress, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    const auto cEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return reinterpret_cast<uintptr_t>(apAddress) + aSize <= cEnd;
}

bool IsExecutable(const void* apAddress) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (!apAddress || !VirtualQuery(apAddress, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// The decorated class name MSVC's RTTI gives a vtable, or nullptr when it has none.
const char* VtableClassName(const void* apVtable) noexcept
{
    struct CompleteObjectLocator
    {
        uint32_t Signature; // 1 on x64
        uint32_t Offset;
        uint32_t ConstructorOffset;
        int32_t TypeDescriptor; // image-relative
        int32_t ClassDescriptor;
        int32_t Self;
    };

    const auto* pSlot = reinterpret_cast<const uint8_t*>(apVtable) - sizeof(void*);
    if (!IsReadable(pSlot, sizeof(void*)))
        return nullptr;
    const auto* pLocator = *reinterpret_cast<const CompleteObjectLocator* const*>(pSlot);
    if (!IsReadable(pLocator, sizeof(CompleteObjectLocator)) || pLocator->Signature != 1)
        return nullptr;

    const auto cImage = reinterpret_cast<uintptr_t>(pLocator) - static_cast<uintptr_t>(pLocator->Self);
    const auto* pName = reinterpret_cast<const char*>(cImage + pLocator->TypeDescriptor + 2 * sizeof(void*));
    if (!IsReadable(pName, 8) || pName[0] != '.')
        return nullptr;
    return pName;
}

// Mfg Fix NG (like other CommonLib plugins) registers a native whose callback is a std::function at offset 0x50, where
// a plain SKSE native keeps its function pointer. MSVC's std::function stores a function pointer in place: the
// implementation object (vtable, then the pointer) at the start of its 64 bytes, and a pointer to that object in its
// last 8. Both are checked, with the implementation's RTTI name and signature, before anything is called.
SetPhonemeModifier_t ResolveSetPhonemeModifier(RE::BSScript::IFunction* apFunction, std::string& aHow) noexcept
{
    auto* pObject = reinterpret_cast<uint8_t*>(apFunction);
    constexpr size_t kCallbackOffset = 0x50;
    constexpr size_t kFunctionSize = 0x40;
    if (!IsReadable(pObject + kCallbackOffset, kFunctionSize))
    {
        aHow = "its function object cannot be read";
        return nullptr;
    }

    void* pFirst = *reinterpret_cast<void**>(pObject + kCallbackOffset);
    if (IsExecutable(pFirst))
    {
        aHow = "plain SKSE native";
        return reinterpret_cast<SetPhonemeModifier_t>(pFirst);
    }

    const char* pName = VtableClassName(pFirst);
    // std::_Func_impl_no_alloc<bool (*)(RE::StaticFunctionTag*, RE::Actor*, int, unsigned int, int), ...>
    constexpr const char* kSignature = "P6A_NPEAUStaticFunctionTag@RE@@PEAVActor@2@HIH@Z";
    if (!pName || !std::strstr(pName, "_Func_impl") || !std::strstr(pName, kSignature))
    {
        aHow = std::string("its callback is not a function pointer kept in a std::function (") + (pName ? pName : "no RTTI") + ")";
        return nullptr;
    }

    void* pImpl = *reinterpret_cast<void**>(pObject + kCallbackOffset + kFunctionSize - sizeof(void*));
    if (pImpl != pObject + kCallbackOffset)
    {
        aHow = "its std::function does not keep the callback in place";
        return nullptr;
    }

    void* pCallee = *reinterpret_cast<void**>(pObject + kCallbackOffset + sizeof(void*));
    if (!IsExecutable(pCallee))
    {
        aHow = "the function pointer in its std::function is not code";
        return nullptr;
    }

    aHow = "CommonLib native";
    return reinterpret_cast<SetPhonemeModifier_t>(pCallee);
}

// Only into the script engine's own vtable, checked by its RTTI name; anything else leaves the Papyrus call in use.
void InstallNativeWatch()
{
    REL::Relocation<std::uintptr_t> vtable{RE::VTABLE_BSScript__Internal__VirtualMachine[0]};
    const char* pName = VtableClassName(reinterpret_cast<const void*>(vtable.address()));
    if (!pName || std::strcmp(pName, ".?AVVirtualMachine@Internal@BSScript@@") != 0)
    {
        LipSync::Log("The script engine's vtable was not found (%s); faces are moved with Papyrus calls", pName ? pName : "no RTTI");
        return;
    }
    g_bindNativeMethod = vtable.write_vfunc(0x18, &BindNativeMethodHook);
}

class Callback final : public RE::BSScript::IStackCallbackFunctor
{
public:
    void operator()(RE::BSScript::Variable) override {}
    void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
};

bool g_faceUnavailable = false;
bool g_faceResolved = false;
SetPhonemeModifier_t g_pSetPhonemeModifier = nullptr;

void SetPhoneme(RE::Actor* apActor, std::uint32_t aId, int aValue)
{
    if (g_faceUnavailable)
        return;

    if (!g_faceResolved)
    {
        g_faceResolved = true;
        if (auto* pNative = g_pSetPhonemeNative.load())
        {
            std::string how;
            g_pSetPhonemeModifier = ResolveSetPhonemeModifier(pNative, how);
            if (g_pSetPhonemeModifier)
                LipSync::Log("Faces are moved by calling MfgConsoleFunc.SetPhonemeModifier directly (%s)", how.c_str());
            else
                LipSync::Log("MfgConsoleFunc.SetPhonemeModifier is not called directly (%s); faces are moved with Papyrus calls instead", how.c_str());
        }
        else
            LipSync::Log("MfgConsoleFunc.SetPhonemeModifier was not seen being bound; faces are moved with Papyrus calls if it exists");
    }

    if (g_pSetPhonemeModifier)
    {
        g_pSetPhonemeModifier(nullptr, apActor, kModePhoneme, aId, aValue);
        return;
    }

    auto* pVm = Together::GetVirtualMachine();
    if (!pVm)
    {
        g_faceUnavailable = true;
        LipSync::Log("The Papyrus VM was not found on this Skyrim version; nobody's mouth moves on this machine (mouth states are still sent)");
        return;
    }
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new Callback());
    auto* pArguments = MakeArguments(static_cast<RE::Actor*>(apActor), std::int32_t{kModePhoneme}, std::int32_t(aId), std::int32_t{aValue});
    if (!pVm->DispatchStaticCall("MfgConsoleFunc", "SetPhonemeModifier", pArguments, callback))
    {
        g_faceUnavailable = true;
        LipSync::Log("MfgConsoleFunc.SetPhonemeModifier cannot be called. Install Mfg Fix NG; until then nobody's mouth moves on this machine "
                     "(mouth states are still sent)");
    }
}

// The phonemes this plugin last wrote on one actor; -1 for one it has not touched.
struct FaceState
{
    RE::FormID FormId{};
    std::int8_t Written[kPhonemeCount];
    bool Owned{};

    FaceState() noexcept { Forget(); }
    void Forget() noexcept
    {
        std::fill(std::begin(Written), std::end(Written), static_cast<std::int8_t>(-1));
        Owned = false;
    }
};

void WriteFace(RE::Actor* apActor, FaceState& aFace, const Shape& acMouth, float aStrength)
{
    if (!apActor)
        return;

    // A respawned copy is a new face.
    if (aFace.FormId != apActor->GetFormID())
    {
        aFace.Forget();
        aFace.FormId = apActor->GetFormID();
    }

    int values[kPhonemeCount];
    ToPhonemes(acMouth, aStrength, values);

    for (std::uint32_t i = 0; i < kPhonemeCount; ++i)
    {
        // Leave alone a phoneme this plugin never raised and has no reason to now.
        if (aFace.Written[i] == values[i] || (aFace.Written[i] < 0 && values[i] == 0))
            continue;
        SetPhoneme(apActor, i, values[i]);
        aFace.Written[i] = static_cast<std::int8_t>(values[i]);
        aFace.Owned = true;
    }
}

void ReleaseFace(RE::Actor* apActor, FaceState& aFace)
{
    if (!aFace.Owned)
        return;

    // Put back to zero only what this plugin raised, then forget the face so other mods have it to themselves.
    if (apActor && aFace.FormId == apActor->GetFormID())
    {
        for (std::uint32_t i = 0; i < kPhonemeCount; ++i)
        {
            if (aFace.Written[i] > 0)
                SetPhoneme(apActor, i, 0);
        }
    }
    aFace.Forget();
}

// ---- State --------------------------------------------------------------------------------------------------------

STAPI::IVSkyrimTogether2* g_pApi = nullptr;
STAPI::IVSkyrimTogether3* g_pApi3 = nullptr;
STAPI::ChannelHandle g_channel = 0;
const STRLipSync::Descriptor* g_pDescriptor = nullptr;

struct Received
{
    Clock::time_point Time;
    Shape Mouth;
};

struct Remote
{
    std::vector<Received> Queue{};
    Shape Target{};
    Shape Current{};
    Clock::time_point LastReceived{};
    // The server ID of that player's character, from the last state (when Plugin API V3 cannot list players).
    std::uint32_t ServerId{};
    FaceState Face{};

    std::uint32_t ReceivedCount{};
    double LargestGap{};
    std::string LastState{};
};

// Shared with Skyrim Together's thread (messages); everything else is the main thread's.
std::mutex g_lock;
std::unordered_map<std::uint32_t, Remote> g_remotes; // by player ID

FaceState g_localFace{};
std::string g_lastSentState;
Clock::time_point g_lastSendTime{};
bool g_sentSpeaking = false;
bool g_wasOnline = false;
bool g_openKeyDown = false;

// Voice chat: the session handed to the plugin part, and the mix sent to it.
bool g_voiceSession = false;
std::string g_voiceHost;
std::uint32_t g_voicePlayer = 0;
Clock::time_point g_lastKeySent{};
bool g_keySent = false;
std::vector<STRLipSync::MixEntry> g_mix;

// Timing, logged once a second with LogLevels.
double g_reportTimer = 0.0;
std::uint32_t g_sentCount = 0;

const STRLipSync::Settings& Config() noexcept
{
    return *g_pDescriptor->Config;
}

double Seconds(Clock::duration aDuration) noexcept
{
    return std::chrono::duration<double>(aDuration).count();
}

struct PlayerCopy
{
    std::uint32_t PlayerId;
    RE::Actor* Actor;
};

// The other players' characters loaded here, with their 3D.
std::vector<PlayerCopy> PlayerCopies()
{
    std::vector<PlayerCopy> copies;
    if (g_pApi3)
    {
        STAPI::PlayerInfo players[64];
        const size_t cCount = std::min<size_t>(g_pApi3->GetPlayers(players, std::size(players)), std::size(players));
        for (size_t i = 0; i < cCount; ++i)
        {
            auto* pActor = RE::TESForm::LookupByID<RE::Actor>(players[i].FormId);
            if (pActor && !pActor->IsPlayerRef())
                copies.push_back({players[i].PlayerId, pActor});
        }
        return copies;
    }

    // Without V3: the players whose states arrived, by their character's server ID.
    std::lock_guard lock(g_lock);
    for (const auto& [playerId, remote] : g_remotes)
    {
        auto* pActor = remote.ServerId ? RE::TESForm::LookupByID<RE::Actor>(g_pApi->GetFormId(remote.ServerId)) : nullptr;
        if (pActor && !pActor->IsPlayerRef())
            copies.push_back({playerId, pActor});
    }
    return copies;
}

void Reset()
{
    std::lock_guard lock(g_lock);
    g_remotes.clear();
    g_lastSentState.clear();
    g_lastSendTime = {};
    g_sentSpeaking = false;
}

// ---- Messages (Skyrim Together's thread) --------------------------------------------------------------------------

void OnState(std::uint32_t aPlayerId, std::uint32_t aServerId, std::string_view aState)
{
    if (!g_pDescriptor || aPlayerId == g_pApi->GetLocalPlayerId())
        return;

    Shape mouth;
    if (!DecodeState(aState, mouth))
        return;

    const auto cNow = Clock::now();
    std::lock_guard lock(g_lock);
    auto& remote = g_remotes[aPlayerId];
    remote.ServerId = aServerId;
    // Gaps within a stretch of talking; the silence before someone starts is not a delay.
    if (remote.LastReceived.time_since_epoch().count() != 0 && Seconds(cNow - remote.LastReceived) < 1.0)
        remote.LargestGap = std::max(remote.LargestGap, Seconds(cNow - remote.LastReceived));
    ++remote.ReceivedCount;

    if (Config().Debug && aState != remote.LastState)
        LipSync::Log("received %.*s from player %X", static_cast<int>(aState.size()), aState.data(), aPlayerId);
    remote.LastState = aState;

    if (remote.Queue.size() >= kMaxQueued)
        remote.Queue.erase(remote.Queue.begin());
    remote.Queue.push_back(Received{cNow, mouth});
    remote.LastReceived = cNow;
}

void OnNearMessage(std::uint32_t aSenderId, std::uint32_t aServerId, const char* apBytes, size_t aLength)
{
    if (apBytes && aLength > 0 && aLength <= 8)
        OnState(aSenderId, aServerId, std::string_view(apBytes, aLength));
}

// A voice key from another player: "K" and 32 hex digits.
void OnMessage(std::uint32_t aSenderId, const char* apBytes, size_t aLength)
{
    if (!g_pDescriptor || !apBytes || aLength != 33 || apBytes[0] != kKeyPrefix)
        return;
    std::string key(apBytes + 1, 32);
    if (!std::all_of(key.begin(), key.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
        return;
    g_pDescriptor->AddVoiceKey(aSenderId, key.c_str());
}

// ---- Main thread --------------------------------------------------------------------------------------------------

void SendVoiceKey()
{
    char key[33]{};
    if (!g_pDescriptor->GetVoiceKey(key, sizeof(key)) || std::strlen(key) != 32)
        return;
    const std::string cWire = std::string(1, kKeyPrefix) + key;
    if (g_pApi->SendToAll(g_channel, cWire.data(), cWire.size()) == STAPI::SendResult::Ok)
    {
        g_lastKeySent = Clock::now();
        g_keySent = true;
    }
}

void SendLocal(const Shape& acMouth, bool aSpeaking)
{
    if (!g_pApi->IsNearSupported())
        return;

    const bool cActive = aSpeaking || !acMouth.IsClosed();
    const auto cNow = Clock::now();
    const double cSince = g_lastSendTime.time_since_epoch().count() == 0 ? 1e9 : Seconds(cNow - g_lastSendTime);

    std::string state;
    if (cActive)
    {
        state = EncodeState(acMouth);
        if (cSince < kMinSendInterval || (state == g_lastSentState && cSince < kRepeatInterval))
            return;
    }
    else
    {
        if (!g_sentSpeaking)
            return;
        state = kQuietState;
    }

    // About the player's character: only the players who can see it get it, and a lost one is replaced.
    const auto cServerId = g_pApi->GetServerId(kPlayerFormId);
    if (cServerId == 0 || g_pApi->SendNear(g_channel, cServerId, STAPI::Delivery::Unreliable, state.data(), state.size()) != STAPI::SendResult::Ok)
        return;
    ++g_sentCount;

    if (Config().Debug && state != g_lastSentState)
        LipSync::Log("sent %s", state.c_str());

    g_lastSentState = state;
    g_lastSendTime = cNow;
    g_sentSpeaking = cActive;
}

void UpdateLocal(bool aOnline)
{
    STRLipSync::Frame frame{};
    if (!g_pDescriptor->GetFrame(&frame))
        frame = STRLipSync::Frame{};

    const Shape cMouth{Clamp01(frame.Open), Clamp01(frame.Wide), Clamp01(frame.Round), Clamp01(frame.Lips), Clamp01(frame.Teeth)};
    const bool cSpeaking = frame.Speaking != 0;

    if (aOnline)
        SendLocal(cMouth, cSpeaking);

    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    if (!pPlayer)
        return;

    // The player's face is the dialogue's business while a conversation is open.
    auto* pUI = RE::UI::GetSingleton();
    const bool cInDialogue = pUI && pUI->IsMenuOpen(RE::DialogueMenu::MENU_NAME);

    if (Config().AnimateSelf && !cInDialogue && pPlayer->Get3D(false) && (cSpeaking || !cMouth.IsClosed()))
        WriteFace(pPlayer, g_localFace, cMouth, Config().MouthStrength);
    else
        ReleaseFace(pPlayer, g_localFace);
}

void UpdateVoice(bool aOnline)
{
    if (!Config().VoiceChat)
    {
        // Switched off in the settings: leave the voice session.
        if (g_voiceSession)
        {
            g_pDescriptor->SetSession(nullptr, 0);
            g_voiceSession = false;
            g_keySent = false;
            g_voiceHost.clear();
            LipSync::Log("Voice chat switched off");
        }
        return;
    }

    // The session follows the connection: the server's host, where the relay runs, and our player id.
    std::string host;
    if (char buffer[256]{}; aOnline && g_pApi3 && g_pApi3->GetServerAddress(buffer, sizeof(buffer), nullptr))
        host = buffer;
    const std::uint32_t cPlayerId = g_pApi->GetLocalPlayerId();
    if (aOnline && !host.empty())
    {
        if (!g_voiceSession || g_voiceHost != host || g_voicePlayer != cPlayerId)
        {
            g_pDescriptor->SetSession(host.c_str(), cPlayerId);
            g_voiceSession = true;
            g_voiceHost = host;
            g_voicePlayer = cPlayerId;
            g_keySent = false;
            LipSync::Log("Voice chat on the server at %s as player %X", host.c_str(), cPlayerId);
        }
    }
    else if (g_voiceSession)
    {
        g_pDescriptor->SetSession(nullptr, 0);
        g_voiceSession = false;
        g_keySent = false;
        g_voiceHost.clear();
        LipSync::Log("Voice chat stopped (left the server)");
    }
    else if (aOnline && !g_pApi3)
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            LipSync::Log("This Skyrim Together build has no Plugin API V3 (server address, players): voice chat is off; mouths are still shared");
    }

    // Publish our random voice key, so the relay host lets our voice in; again now and then for players who join later.
    if (g_voiceSession && g_pApi->IsNearSupported() && (!g_keySent || Seconds(Clock::now() - g_lastKeySent) >= kVoiceKeyRepeat))
        SendVoiceKey();

    if (!g_voiceSession)
        return;

    // Who might be heard and from which side, panned by where they stand relative to the way the player faces. The
    // plugin part turns the distance into a volume with the range each voice carries right now (a yell reaches further),
    // so everyone up to the furthest a yell carries is listed.
    const float cReach = std::max(Config().FarDistance, Config().NearDistance + 1.f) * std::max(1.f, Config().MaxRangeScale);
    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    g_mix.clear();
    if (pPlayer)
    {
        const RE::NiPoint3 cOwn = pPlayer->GetPosition();
        const float cHeading = pPlayer->GetAngleZ();
        for (const auto& [playerId, pActor] : PlayerCopies())
        {
            if (!pActor->Get3D(false))
                continue;
            const RE::NiPoint3 cPos = pActor->GetPosition();
            const float cDx = cPos.x - cOwn.x, cDy = cPos.y - cOwn.y, cDz = cPos.z - cOwn.z;
            const float cDistance = std::sqrt(cDx * cDx + cDy * cDy + cDz * cDz);
            if (cDistance >= cReach)
                continue;

            // Skyrim's heading: 0 faces +Y (north), growing clockwise; the angle to the speaker the same way.
            const float cPan = 0.8f * std::sin(std::atan2(cDx, cDy) - cHeading);
            g_mix.push_back(STRLipSync::MixEntry{playerId, cDistance, cPan});

            // So the voice-driven mouth is applied even before any relayed mouth state arrives.
            std::lock_guard lock(g_lock);
            g_remotes[playerId];
        }
    }
    g_pDescriptor->SetMix(g_mix.data(), static_cast<std::uint32_t>(g_mix.size()));
}

void UpdateRemotes(double aDelta)
{
    const double cDelay = Config().RemoteDelaySeconds;
    const float cMaxDistance = Config().MaxDistance;
    const auto cNow = Clock::now();

    std::vector<PlayerCopy> copies = PlayerCopies();

    std::lock_guard lock(g_lock);
    for (auto& [playerId, remote] : g_remotes)
    {
        // A player whose voice is heard: the mouth follows the voice as it plays.
        STRLipSync::Frame voiced{};
        if (g_voiceSession && g_pDescriptor->GetRemoteMouth && g_pDescriptor->GetRemoteMouth(playerId, &voiced))
        {
            remote.Queue.clear();
            remote.Target = Shape{Clamp01(voiced.Open), Clamp01(voiced.Wide), Clamp01(voiced.Round), Clamp01(voiced.Lips), Clamp01(voiced.Teeth)};
            remote.Current = remote.Target;
            continue;
        }

        // Show what arrived a moment ago, so uneven arrival does not make the mouth stutter.
        size_t due = 0;
        while (due < remote.Queue.size() && Seconds(cNow - remote.Queue[due].Time) >= cDelay)
            remote.Target = remote.Queue[due++].Mouth;
        if (due > 0)
            remote.Queue.erase(remote.Queue.begin(), remote.Queue.begin() + static_cast<std::ptrdiff_t>(due));

        if (Seconds(cNow - remote.LastReceived) > kRemoteTimeout + cDelay)
        {
            remote.Target = Shape{};
            remote.Queue.clear();
        }

        SmoothToward(remote.Current, remote.Target, aDelta);
    }

    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    for (const auto& [playerId, pActor] : copies)
    {
        auto it = g_remotes.find(playerId);
        if (it == g_remotes.end())
            continue;
        auto& remote = it->second;

        bool inRange = pActor->Get3D(false) != nullptr;
        if (inRange && pPlayer)
        {
            const RE::NiPoint3 cDelta = pActor->GetPosition() - pPlayer->GetPosition();
            inRange = cDelta.x * cDelta.x + cDelta.y * cDelta.y + cDelta.z * cDelta.z <= cMaxDistance * cMaxDistance;
        }

        if (inRange && !remote.Current.IsClosed())
            WriteFace(pActor, remote.Face, remote.Current, Config().MouthStrength);
        else
            ReleaseFace(pActor, remote.Face);
    }
}

void ReportTiming(double aDelta)
{
    if (!Config().Debug)
        return;
    g_reportTimer += aDelta;
    if (g_reportTimer < 1.0)
        return;

    LipSync::Log("last %.1f s: %u states sent", g_reportTimer, g_sentCount);
    std::lock_guard lock(g_lock);
    for (auto& [playerId, remote] : g_remotes)
    {
        LipSync::Log("  player %X: %u states received, longest gap %.0f ms, %zu waiting, jaw %.2f", playerId, remote.ReceivedCount, remote.LargestGap * 1000.0,
                     remote.Queue.size(), remote.Current.Open);
        remote.ReceivedCount = 0;
        remote.LargestGap = 0.0;
    }
    g_reportTimer = 0.0;
    g_sentCount = 0;
}

// The open key (Shift+V by default) toggles the settings window, only while the game has the focus and the window does
// not wait for a key to assign.
void UpdateWindowKey()
{
    const std::uint32_t cOpenKey = Config().OpenKey;
    if (!cOpenKey || Window::IsCapturingKey())
        return;

    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    const bool cFocus = processId == GetCurrentProcessId();
    const bool cDown = cFocus && (GetAsyncKeyState(static_cast<int>(cOpenKey)) & 0x8000) != 0;
    const bool cShift = !Config().OpenKeyShift || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (cDown && !g_openKeyDown && cShift)
        Window::Toggle();
    g_openKeyDown = cDown;
}

Clock::time_point g_lastTick{};

void Tick()
{
    const auto cNow = Clock::now();
    const double cDelta = g_lastTick.time_since_epoch().count() == 0 ? 0.0 : std::min(Seconds(cNow - g_lastTick), 0.25);
    g_lastTick = cNow;

    const bool cOnline = g_pApi->GetLocalPlayerId() != 0;
    if (cOnline != g_wasOnline)
    {
        g_wasOnline = cOnline;
        Reset();
    }

    ReportTiming(cDelta);
    UpdateWindowKey();
    UpdateLocal(cOnline);
    UpdateVoice(cOnline);
    if (cOnline)
        UpdateRemotes(cDelta);
}
} // namespace

namespace Host
{
bool Start()
{
    g_pDescriptor = LipSync::GetDescriptor();
    g_pApi = FindApi();
    if (!g_pApi || !g_pDescriptor)
        return false;
    g_pApi3 = FindApi3();
    // Before the script engine starts and Mfg Fix binds its natives.
    InstallNativeWatch();

    g_channel = g_pApi->RegisterChannel(kChannelName, &OnMessage, nullptr);
    if (!g_channel || !g_pApi->SetNearMessageCallback(g_channel, &OnNearMessage))
    {
        LipSync::Log("Could not register the channel %s", kChannelName);
        g_channel = 0;
        return false;
    }
    LipSync::Log("Registered channel %s; Plugin API V3 (voice chat): %s", kChannelName, g_pApi3 ? "yes" : "no");
    return true;
}

// Once the game's data is loaded: the main thread runs Tick 30 times a second from here on.
void Run()
{
    if (!g_channel)
        return;
    std::thread(
        []
        {
            for (;;)
            {
                std::this_thread::sleep_for(kTick);
                SKSE::GetTaskInterface()->AddTask([] { Tick(); });
            }
        })
        .detach();
}
} // namespace Host
