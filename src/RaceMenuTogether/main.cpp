// RaceMenu Together 2: shows each player's RaceMenu appearance on the other players, as a standalone SKSE plugin on
// Skyrim Together's Plugin API (V2, plus V3 for face tints). Nothing is patched into Skyrim Together.
//
// The appearance is RaceMenu's own preset, plus the player's face tints: exported with CharGen.SaveCharacterPreset
// after joining and whenever RaceMenu closes, compressed, and sent in pieces as messages about the player's
// character. The other clients put it on their copy of that player with CharGen.LoadCharacterPresetEx. Both are
// called as ordinary Papyrus calls, queued on the script engine, so RaceMenu runs them as it runs them for any
// script; and only one copy is loaded at a time, as each load rebuilds a head, body morphs and overlays (and with
// SMP, physics). After a load:
//   - the face tints go back on the copy through Plugin API V3, as loading the preset drops them;
//   - the copy keeps its own hair color form, so it does not take the hair color set last on this machine;
//   - overlays are rebuilt if they went missing, and body morphs that BodyGen or OBody put on the copy (it is an NPC
//     to them) are replaced with the owner's, checked every few seconds.
// Each of those is skipped, with one line in the log, on a client where what it needs is missing (no Plugin API
// V3, an older RaceMenu, no overlays in the preset); everything else is still synced.
//
// Skyrim Together relays messages about a character to the players who have it in range, and stores nothing. So
// that players coming into range (or joining later) get an appearance too, each client announces which revision of
// its appearance it has, and anyone in range without that revision asks for it:
//
//   A|<revision>                          announce, unreliable, every few seconds
//   Q|<server id, hex>|<revision>         please send it (a message about the asking player's own character)
//   D|<revision>|<piece>|<pieces>|<bytes> one piece of the appearance
//
// The appearance is "RMS2", the preset's size, the face tints, then the preset deflated with zlib (see EncodeBlob).
//
// Replaces mailamea's RaceMenuTogether.dll 1.x, which switched on the same work built into the Skyrim Together exe.
// It does not export that version's descriptor, so the built-in copy (in builds that have it) stays off.

#include "Common/Safety.h"
#include "RaceMenuTogether/SkeeInterfaces.h"

#include <zlib.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace
{
using namespace Together;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr const char* kPluginName = "RaceMenuTogether";
constexpr const char* kChannelName = "RaceMenuTogether.v2";

// Pieces fit one message with room for the header, and leave at a pace under the server's rate limit.
constexpr size_t kPieceBytes = 1100;
constexpr std::uint32_t kMaxPieces = 200;
constexpr double kPiecesPerSecond = 40.0;
constexpr double kPieceBurst = 20.0;
constexpr char kBlobMagic[4] = {'R', 'M', 'S', '2'};
constexpr std::uint32_t kMaxPresetBytes = 16 * 1024 * 1024;

constexpr auto kTick = 100ms;
constexpr auto kApplyInterval = 500ms;
constexpr auto kAnnounceInterval = 5s;
// Give the character a moment to be assigned after joining before the first export.
constexpr auto kJoinDelay = 5s;
// Requests for one revision are answered at most this often, and asked at most this often.
constexpr auto kResendInterval = 3s;
constexpr auto kRequestInterval = 4s;
// A second load once RaceMenu's queued overlay builds and the copy's equipment have settled, and how often a copy's
// body morphs and overlays are checked.
constexpr auto kSettleDelay = 4s;
constexpr auto kMorphCheckInterval = 5s;
// A load whose result never came back is given up after this long (and leaves a dump).
constexpr auto kLoadTimeout = 15s;
constexpr std::uint32_t kMaxLoadAttempts = 5;

// RaceMenu reads and writes presets here, relative to the game folder (Mod Organizer redirects new files into its
// overwrite folder). The files only exist for the duration of one save or one load.
constexpr const char* kPresetFolder = "Data\\SKSE\\Plugins\\CharGen\\Presets\\";
constexpr const char* kLocalPresetName = "RaceMenuTogether_Local";
// RaceMenu's ApplyTypes: face, overrides, body morphs, transforms and skin overrides.
constexpr std::int32_t kPresetApplyAll = 0x0F;

// Body morph keys that other mods put on NPCs: BodyGen (RaceMenu's own randomizer), OBody and OBody NG. A copy of
// another player counts as an NPC to them, so morphs under these keys on a copy are cleared and the owner's put back,
// unless the owner's own appearance uses that key.
constexpr const char* kProtectedMorphKeys[] = {"RSMBodyGen", "OBody", "OBodyNG"};

STAPI::IVSkyrimTogether2* g_pApi = nullptr;
STAPI::IVSkyrimTogether3* g_pApi3 = nullptr;
STAPI::ChannelHandle g_channel = 0;
// RaceMenu (skee64.dll) is loaded; known from PostPostLoad on. Without it this client neither sends nor applies.
std::atomic<bool> g_raceMenu{false};
Skee::IBodyMorphInterface* g_pBodyMorph = nullptr;

struct Tint
{
    std::string Texture;
    float Alpha;
    std::uint32_t Color;
    std::uint32_t Type;
};

struct BodyMorph
{
    std::string Name;
    std::string Key;
    float Value;
};

struct Remote
{
    // Pieces of the revision arriving now.
    std::uint32_t IncomingRevision{};
    std::uint32_t IncomingCount{};
    std::uint32_t IncomingReceived{};
    std::vector<std::string> IncomingPieces;

    // The last complete appearance, and what was read out of it.
    std::uint32_t Revision{};
    std::string Json;
    std::vector<Tint> Tints;
    std::vector<BodyMorph> BodyMorphs;
    bool HasOverlayNodes{};

    // What it was last put on, so a respawned copy or a reloaded body gets it again.
    std::uint32_t AppliedRevision{};
    RE::FormID AppliedFormId{};
    const void* AppliedRoot{};
    std::uint32_t FailedAttempts{};
    std::optional<Clock::time_point> SettleAt;
    Clock::time_point MorphCheckAt{};

    // This copy's own hair color form. RaceMenu edits the color form it is handed in place, and every copy points at
    // the same form from RaceMenu.esp, so without its own each copy takes the hair color set last on this machine.
    RE::BGSColorForm* HairColor{};
    // Whether the copy was seen holding its own hair color form, so losing it again means Skyrim Together rebuilt the
    // copy's base (and the appearance is loaded again) rather than RaceMenu never taking it (no reload loop).
    bool HairColorHeld{};

    Clock::time_point LastRequest{};
};

// Shared between Skyrim Together's thread (messages), the pacing thread, script threads and the main thread.
std::mutex g_lock;
std::unordered_map<std::uint32_t, Remote> g_remotes; // by the server ID of that player's character
std::string g_blob;                                  // our appearance as sent
std::uint32_t g_revision = 0;
std::deque<std::string> g_outgoing;
Clock::time_point g_lastFullSend{};

// Main thread only.
bool g_loadInFlight = false;
Clock::time_point g_loadStarted{};

// ---- Helpers ---------------------------------------------------------------------------------------------------

std::filesystem::path PresetPath(const std::string& acName)
{
    return std::filesystem::path(kPresetFolder) / (acName + ".jslot");
}

std::string RemotePresetName(std::uint32_t aServerId)
{
    return std::format("RaceMenuTogether_{:X}", aServerId);
}

bool ReadFile(const std::filesystem::path& acPath, std::string& aOut)
{
    std::ifstream file(acPath, std::ios::binary);
    if (!file)
        return false;
    aOut.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !aOut.empty();
}

bool WriteFile(const std::filesystem::path& acPath, const std::string& acData)
{
    std::error_code ec;
    std::filesystem::create_directories(acPath.parent_path(), ec);
    std::ofstream file(acPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(acData.data(), static_cast<std::streamsize>(acData.size()));
    return static_cast<bool>(file);
}

void RemoveFile(const std::filesystem::path& acPath)
{
    std::error_code ec;
    std::filesystem::remove(acPath, ec);
}

template <class T> bool ParseNumber(std::string_view aText, T& aOut, int aBase = 10) noexcept
{
    const auto cResult = std::from_chars(aText.data(), aText.data() + aText.size(), aOut, aBase);
    return cResult.ec == std::errc{} && cResult.ptr == aText.data() + aText.size();
}

// Splits off the next '|'-terminated field.
bool NextField(std::string_view& aRest, std::string_view& aField) noexcept
{
    const auto cBar = aRest.find('|');
    if (cBar == std::string_view::npos)
        return false;
    aField = aRest.substr(0, cBar);
    aRest.remove_prefix(cBar + 1);
    return true;
}

// ---- Appearance encoding ------------------------------------------------------------------------------------------

void PutU32(std::string& aOut, std::uint32_t aValue)
{
    for (int i = 0; i < 4; ++i)
        aOut.push_back(static_cast<char>((aValue >> (8 * i)) & 0xFF));
}

struct ByteReader
{
    const char* p;
    const char* end;

    bool U32(std::uint32_t& aOut) noexcept
    {
        if (end - p < 4)
            return false;
        aOut = 0;
        for (int i = 0; i < 4; ++i)
            aOut |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[i])) << (8 * i);
        p += 4;
        return true;
    }

    bool Bytes(size_t aCount, std::string& aOut) noexcept
    {
        if (static_cast<size_t>(end - p) < aCount)
            return false;
        aOut.assign(p, p + aCount);
        p += aCount;
        return true;
    }
};

// "RMS2", the preset's size, the tint count, per tint alpha (float bits), color, type, texture path length and path,
// then the preset deflated with zlib. All numbers 4 bytes, little endian.
bool EncodeBlob(const std::string& acJson, const std::vector<Tint>& acTints, std::string& aBlob)
{
    uLongf size = compressBound(static_cast<uLong>(acJson.size()));
    std::string compressed(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(compressed.data()), &size, reinterpret_cast<const Bytef*>(acJson.data()), static_cast<uLong>(acJson.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
        return false;
    compressed.resize(size);

    aBlob.assign(kBlobMagic, sizeof(kBlobMagic));
    PutU32(aBlob, static_cast<std::uint32_t>(acJson.size()));
    PutU32(aBlob, static_cast<std::uint32_t>(acTints.size()));
    for (const auto& cTint : acTints)
    {
        PutU32(aBlob, std::bit_cast<std::uint32_t>(cTint.Alpha));
        PutU32(aBlob, cTint.Color);
        PutU32(aBlob, cTint.Type);
        PutU32(aBlob, static_cast<std::uint32_t>(cTint.Texture.size()));
        aBlob += cTint.Texture;
    }
    aBlob += compressed;
    return true;
}

bool DecodeBlob(const std::string& acBlob, std::string& aJson, std::vector<Tint>& aTints)
{
    ByteReader reader{acBlob.data(), acBlob.data() + acBlob.size()};
    std::string magic;
    std::uint32_t size = 0, tintCount = 0;
    if (!reader.Bytes(sizeof(kBlobMagic), magic) || std::memcmp(magic.data(), kBlobMagic, sizeof(kBlobMagic)) != 0 || !reader.U32(size) || size == 0 || size > kMaxPresetBytes ||
        !reader.U32(tintCount) || tintCount > STAPI::kMaxFaceTints)
        return false;

    aTints.clear();
    aTints.reserve(tintCount);
    for (std::uint32_t i = 0; i < tintCount; ++i)
    {
        std::uint32_t alpha = 0, color = 0, type = 0, length = 0;
        Tint tint;
        if (!reader.U32(alpha) || !reader.U32(color) || !reader.U32(type) || !reader.U32(length) || length > STAPI::kMaxFaceTintTextureSize || !reader.Bytes(length, tint.Texture))
            return false;
        tint.Alpha = std::bit_cast<float>(alpha);
        if (!std::isfinite(tint.Alpha))
            return false;
        tint.Color = color;
        tint.Type = type;
        aTints.push_back(std::move(tint));
    }

    aJson.resize(size);
    uLongf outSize = size;
    if (uncompress(reinterpret_cast<Bytef*>(aJson.data()), &outSize, reinterpret_cast<const Bytef*>(reader.p), static_cast<uLong>(reader.end - reader.p)) != Z_OK || outSize != size)
    {
        aJson.clear();
        return false;
    }
    return true;
}

// The player's face tints as the game holds them (the overlay set, when the game uses one).
std::vector<Tint> ReadPlayerTints()
{
    std::vector<Tint> tints;
    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    if (!pPlayer)
        return tints;

    // PlayerCharacter's tint masks and overlay tint masks sit where Skyrim Together's own PlayerCharacter has them
    // (static_asserts in its PlayerCharacter.h), not where this CommonLib checkout's AE layout says (B10): the runtimes
    // after 1.6.640 grew the structure. On any other runtime no tints are sent, and copies keep Skyrim Together's.
    const auto cVersion = REL::Module::get().version();
    std::ptrdiff_t baseOffset = 0;
    if (cVersion == REL::Version(1, 6, 1170, 0))
        baseOffset = 0xB18;
    else if (cVersion >= REL::Version(1, 7, 0, 0) && !REL::Module::IsVR())
        baseOffset = 0xB20;
    if (baseOffset == 0)
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            SKSE::log::warn("Face tints are not read on runtime {}: copies keep the tints Skyrim Together painted", cVersion.string());
        return tints;
    }
    const auto& cBaseMasks = REL::RelocateMember<RE::BSTArray<RE::TintMask*>>(pPlayer, baseOffset);
    const auto* pOverlayMasks = REL::RelocateMember<RE::BSTArray<RE::TintMask*>*>(pPlayer, baseOffset + 0x18);
    const auto& cMasks = pOverlayMasks ? *pOverlayMasks : cBaseMasks;
    for (const RE::TintMask* pMask : cMasks)
    {
        if (!pMask || tints.size() >= STAPI::kMaxFaceTints)
            continue;
        Tint tint;
        tint.Texture = pMask->texture && pMask->texture->textureName.c_str() ? pMask->texture->textureName.c_str() : "";
        if (tint.Texture.size() > STAPI::kMaxFaceTintTextureSize)
            continue;
        tint.Alpha = pMask->alpha;
        static_assert(sizeof(pMask->color) == sizeof(tint.Color));
        std::memcpy(&tint.Color, &pMask->color, sizeof(tint.Color));
        tint.Type = static_cast<std::uint32_t>(pMask->type.underlying());
        tints.push_back(std::move(tint));
    }
    return tints;
}

// ---- RaceMenu's preset files: just enough JSON to read the "bodyMorphs" array --------------------------------------

struct JsonReader
{
    const char* p;
    const char* end;

    void SkipWs() noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }

    bool Consume(char aC) noexcept
    {
        SkipWs();
        if (p < end && *p == aC)
        {
            ++p;
            return true;
        }
        return false;
    }

    bool Peek(char aC) noexcept
    {
        SkipWs();
        return p < end && *p == aC;
    }

    bool ReadString(std::string& aOut)
    {
        aOut.clear();
        if (!Consume('"'))
            return false;
        while (p < end && *p != '"')
        {
            if (*p == '\\' && p + 1 < end)
            {
                ++p;
                switch (*p)
                {
                case 'n': aOut += '\n'; break;
                case 't': aOut += '\t'; break;
                case 'r': aOut += '\r'; break;
                case 'b': aOut += '\b'; break;
                case 'f': aOut += '\f'; break;
                case 'u':
                    // Morph names and keys are ASCII; keep the escape's low byte.
                    if (end - p >= 5)
                    {
                        std::uint32_t code = 0;
                        ParseNumber(std::string_view(p + 1, 4), code, 16);
                        aOut += static_cast<char>(code & 0xFF);
                        p += 4;
                    }
                    break;
                default: aOut += *p; break;
                }
                ++p;
                continue;
            }
            aOut += *p++;
        }
        return Consume('"');
    }

    bool ReadNumber(double& aOut) noexcept
    {
        SkipWs();
        const char* start = p;
        while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
            ++p;
        if (p == start)
            return false;
        const auto cResult = std::from_chars(start, p, aOut);
        return cResult.ec == std::errc{};
    }

    bool SkipValue(int aDepth = 0)
    {
        if (aDepth > 64)
            return false;
        SkipWs();
        if (p >= end)
            return false;
        if (*p == '"')
        {
            std::string ignored;
            return ReadString(ignored);
        }
        if (*p == '{' || *p == '[')
        {
            const char cClose = *p == '{' ? '}' : ']';
            const bool cObject = *p == '{';
            ++p;
            if (Consume(cClose))
                return true;
            do
            {
                if (cObject)
                {
                    std::string key;
                    if (!ReadString(key) || !Consume(':'))
                        return false;
                }
                if (!SkipValue(aDepth + 1))
                    return false;
            } while (Consume(','));
            return Consume(cClose);
        }
        // Number or literal.
        const char* start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t')
            ++p;
        return p != start;
    }
};

// The owner's body morphs (name, key, value) in a preset; stops at anything it cannot read, keeping what it read.
std::vector<BodyMorph> ParseBodyMorphs(const std::string& acJson)
{
    std::vector<BodyMorph> morphs;
    const auto cKey = acJson.find("\"bodyMorphs\"");
    if (cKey == std::string::npos)
        return morphs;

    JsonReader reader{acJson.data() + cKey + 12, acJson.data() + acJson.size()};
    if (!reader.Consume(':') || !reader.Consume('[') || reader.Consume(']'))
        return morphs;

    do
    {
        if (!reader.Consume('{'))
            return morphs;

        std::string name;
        std::vector<std::pair<std::string, float>> keys;
        if (!reader.Peek('}'))
        {
            do
            {
                std::string member;
                if (!reader.ReadString(member) || !reader.Consume(':'))
                    return morphs;

                if (member == "name")
                {
                    if (!reader.ReadString(name))
                        return morphs;
                }
                else if (member == "value") // presets from before morph keys
                {
                    double value;
                    if (!reader.ReadNumber(value))
                        return morphs;
                    keys.emplace_back("RSMLegacy", static_cast<float>(value));
                }
                else if (member == "keys")
                {
                    if (!reader.Consume('['))
                        return morphs;
                    if (!reader.Consume(']'))
                    {
                        do
                        {
                            if (!reader.Consume('{'))
                                return morphs;
                            std::string key;
                            double value = 0.0;
                            do
                            {
                                std::string field;
                                if (!reader.ReadString(field) || !reader.Consume(':'))
                                    return morphs;
                                if (field == "key")
                                {
                                    if (!reader.ReadString(key))
                                        return morphs;
                                }
                                else if (field == "value")
                                {
                                    if (!reader.ReadNumber(value))
                                        return morphs;
                                }
                                else if (!reader.SkipValue())
                                    return morphs;
                            } while (reader.Consume(','));
                            if (!reader.Consume('}'))
                                return morphs;
                            if (!key.empty())
                                keys.emplace_back(std::move(key), static_cast<float>(value));
                        } while (reader.Consume(','));
                        if (!reader.Consume(']'))
                            return morphs;
                    }
                }
                else if (!reader.SkipValue())
                    return morphs;
            } while (reader.Consume(','));
        }
        if (!reader.Consume('}'))
            return morphs;

        if (!name.empty())
        {
            for (auto& [key, value] : keys)
                morphs.push_back({name, std::move(key), value});
        }
    } while (reader.Consume(','));
    return morphs;
}

// ---- Papyrus calls ---------------------------------------------------------------------------------------------

class Callback final : public RE::BSScript::IStackCallbackFunctor
{
public:
    explicit Callback(std::function<void(const RE::BSScript::Variable&)> aFunction)
        : m_function(std::move(aFunction))
    {
    }

    void operator()(RE::BSScript::Variable aResult) override
    {
        if (m_function)
            m_function(aResult);
    }
    void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

private:
    std::function<void(const RE::BSScript::Variable&)> m_function;
};

// Queues a global Papyrus function; aDone runs on a script thread with its result. False if it could not be queued.
bool CallStatic(const char* acpClass, const char* acpFunction, RE::BSScript::IFunctionArguments* apArguments, std::function<void(const RE::BSScript::Variable&)> aDone = {})
{
    auto* pVm = Together::GetVirtualMachine();
    if (!pVm)
    {
        delete apArguments;
        return false;
    }
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new Callback(std::move(aDone)));
    return pVm->DispatchStaticCall(acpClass, acpFunction, apArguments, callback);
}

// ---- Our appearance ----------------------------------------------------------------------------------------------

void QueueFullSend(const std::string& acBlob, std::uint32_t aRevision)
{
    const auto cPieces = static_cast<std::uint32_t>((acBlob.size() + kPieceBytes - 1) / kPieceBytes);
    if (cPieces == 0 || cPieces > kMaxPieces)
        return;

    // A newer send replaces whatever of an older one has not gone out yet.
    g_outgoing.clear();
    for (std::uint32_t i = 0; i < cPieces; ++i)
    {
        std::string wire = std::format("D|{}|{}|{}|", aRevision, i, cPieces);
        const size_t cOffset = static_cast<size_t>(i) * kPieceBytes;
        wire.append(acBlob, cOffset, std::min(kPieceBytes, acBlob.size() - cOffset));
        g_outgoing.push_back(std::move(wire));
    }
    g_lastFullSend = Clock::now();
}

// Main thread.
void Export(const char* acpReason)
{
    auto* pPlayer = RE::PlayerCharacter::GetSingleton();
    if (!pPlayer)
        return;

    // Read on the main thread, where RaceMenu changes them.
    std::vector<Tint> tints = ReadPlayerTints();

    RemoveFile(PresetPath(kLocalPresetName));
    const std::string cReason = acpReason;
    const bool cQueued = CallStatic("CharGen", "SaveCharacterPreset", MakeArguments(static_cast<RE::Actor*>(pPlayer), RE::BSFixedString(kLocalPresetName)),
                                    [cReason, tints = std::move(tints)](const RE::BSScript::Variable&)
                                    {
                                        const auto cPath = PresetPath(kLocalPresetName);
                                        std::string json;
                                        const bool cRead = ReadFile(cPath, json);
                                        RemoveFile(cPath);
                                        if (!cRead)
                                        {
                                            SKSE::log::warn("RaceMenu did not write the player's preset ({})", cReason);
                                            return;
                                        }

                                        std::string blob;
                                        if (!EncodeBlob(json, tints, blob) || blob.size() > kPieceBytes * kMaxPieces)
                                        {
                                            SKSE::log::warn("The player's preset is too large to send ({} bytes)", json.size());
                                            return;
                                        }

                                        std::lock_guard lock(g_lock);
                                        g_blob = std::move(blob);
                                        ++g_revision;
                                        QueueFullSend(g_blob, g_revision);
                                        SKSE::log::info("Sending the player's appearance ({}), revision {}, {} bytes, {} face tints ({} bytes in all)", cReason, g_revision, json.size(),
                                                        tints.size(), g_blob.size());
                                    });
    if (!cQueued)
        SKSE::log::warn("Could not call CharGen.SaveCharacterPreset: is RaceMenu installed?");
}

class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* apEvent, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        // RaceMenu closing is when sculpting, an imported head or a loaded preset is final.
        if (apEvent && !apEvent->opening && apEvent->menuName == RE::RaceSexMenu::MENU_NAME && g_pApi && g_pApi->IsNearSupported())
            Export("RaceMenu closed");
        return RE::BSEventNotifyControl::kContinue;
    }
};

// ---- Other players' copies (main thread) --------------------------------------------------------------------------

RE::Actor* FindCopy(std::uint32_t aServerId)
{
    if (g_pApi->IsOwnedLocally(aServerId))
        return nullptr;
    const auto cFormId = g_pApi->GetFormId(aServerId);
    auto* pActor = cFormId ? RE::TESForm::LookupByID<RE::Actor>(cFormId) : nullptr;
    return pActor && !pActor->IsPlayerRef() ? pActor : nullptr;
}

void SendFaceTints(std::uint32_t aServerId, const std::vector<Tint>& acTints)
{
    if (acTints.empty())
        return;
    if (!g_pApi3)
    {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            SKSE::log::info("This Skyrim Together build has no Plugin API V3: copies keep the face tints Skyrim Together painted before RaceMenu loaded");
        return;
    }

    std::vector<STAPI::FaceTint> tints;
    tints.reserve(acTints.size());
    for (const auto& cTint : acTints)
        tints.push_back({cTint.Alpha, cTint.Color, cTint.Type, cTint.Texture.c_str()});
    if (!g_pApi3->SetFaceTints(aServerId, tints.data(), tints.size()))
        SKSE::log::warn("Skyrim Together refused the face tints of {:X}", aServerId);
}

// Replaces morphs BodyGen or OBody put on a copy, and morphs that drifted from the owner's, with the owner's.
void EnforceBodyMorphs(std::uint32_t aServerId, RE::Actor* apActor, const std::vector<BodyMorph>& acMorphs)
{
    if (!g_pBodyMorph)
        return;

    const char* pReason = nullptr;
    for (const char* cpKey : kProtectedMorphKeys)
    {
        const bool cOwnersKey = std::any_of(acMorphs.begin(), acMorphs.end(), [cpKey](const BodyMorph& acMorph) { return acMorph.Key == cpKey; });
        if (!cOwnersKey && g_pBodyMorph->HasBodyMorphKey(apActor, cpKey))
        {
            pReason = cpKey;
            break;
        }
    }
    if (!pReason)
    {
        for (const auto& cMorph : acMorphs)
        {
            if (std::fabs(g_pBodyMorph->GetMorph(apActor, cMorph.Name.c_str(), cMorph.Key.c_str()) - cMorph.Value) > 0.001f)
            {
                pReason = "drift";
                break;
            }
        }
    }
    if (!pReason)
        return;

    g_pBodyMorph->ClearMorphs(apActor);
    for (const auto& cMorph : acMorphs)
        g_pBodyMorph->SetMorph(apActor, cMorph.Name.c_str(), cMorph.Key.c_str(), cMorph.Value);
    g_pBodyMorph->UpdateModelWeight(apActor, false);

    static std::uint32_t s_logged = 0;
    if (s_logged < 20 || s_logged % 50 == 0)
        SKSE::log::info("Put back {} body morphs of {:X} on {:08X} ({})", acMorphs.size(), aServerId, apActor->GetFormID(), pReason);
    ++s_logged;
}

// RaceMenu builds overlays on a task after a load; if a copy whose owner has overlays ends up without them, build them.
void EnsureOverlays(std::uint32_t aServerId, RE::Actor* apActor)
{
    const RE::ActorHandle cHandle = apActor->GetHandle();
    CallStatic("NiOverride", "HasOverlays", MakeArguments(static_cast<RE::TESObjectREFR*>(apActor)),
               [aServerId, cHandle](const RE::BSScript::Variable& acResult)
               {
                   if (!acResult.IsBool() || acResult.GetBool())
                       return;
                   SKSE::GetTaskInterface()->AddTask(
                       [aServerId, cHandle]()
                       {
                           const auto pActor = cHandle.get();
                           if (!pActor)
                               return;
                           // AddOverlays builds the nodes on a task, and the build applies the copy's stored node overrides.
                           CallStatic("NiOverride", "AddOverlays", MakeArguments(static_cast<RE::TESObjectREFR*>(pActor.get())),
                                      [cHandle](const RE::BSScript::Variable&)
                                      {
                                          SKSE::GetTaskInterface()->AddTask(
                                              [cHandle]()
                                              {
                                                  if (const auto pActor = cHandle.get())
                                                      CallStatic("NiOverride", "ApplyNodeOverrides", MakeArguments(static_cast<RE::TESObjectREFR*>(pActor.get())));
                                              });
                                      });
                           SKSE::log::info("Overlays were missing on the copy of {:X}, rebuilt them", aServerId);
                       });
               });
}

bool HoldsOwnHairColor(RE::Actor* apActor, RE::BGSColorForm* apHairColor)
{
    auto* pNpc = apActor->GetActorBase();
    return apHairColor && pNpc && pNpc->headRelatedData && pNpc->headRelatedData->hairColor == apHairColor;
}

void OnLoaded(std::uint32_t aServerId, std::uint32_t aRevision, RE::FormID aFormId, const void* apRoot, bool aSettlePass, bool aLoaded)
{
    g_loadInFlight = false;
    RemoveFile(PresetPath(RemotePresetName(aServerId)));

    std::vector<Tint> tints;
    std::vector<BodyMorph> morphs;
    bool overlays = false;
    RE::BGSColorForm* pHairColor = nullptr;
    {
        std::lock_guard lock(g_lock);
        auto it = g_remotes.find(aServerId);
        if (it == g_remotes.end())
            return;
        auto& remote = it->second;

        SKSE::log::info("Appearance revision {} of {:X} {} on {:08X}{}", aRevision, aServerId, aLoaded ? "applied" : "was refused by RaceMenu", aFormId, aSettlePass ? " (second pass)" : "");
        if (!aLoaded)
        {
            if (++remote.FailedAttempts == kMaxLoadAttempts)
                SKSE::log::warn("Giving up on the appearance of {:X} after {} attempts", aServerId, kMaxLoadAttempts);
            return;
        }

        remote.AppliedRevision = aRevision;
        remote.AppliedFormId = aFormId;
        remote.AppliedRoot = apRoot;
        remote.FailedAttempts = 0;
        remote.MorphCheckAt = Clock::now() + kMorphCheckInterval;
        // The first load after a change gets a second pass once things have settled; the second pass does not
        // schedule another.
        if (aSettlePass)
            remote.SettleAt.reset();
        else
            remote.SettleAt = Clock::now() + kSettleDelay;

        if (remote.Revision == aRevision)
        {
            tints = remote.Tints;
            morphs = remote.BodyMorphs;
            overlays = remote.HasOverlayNodes;
        }
        pHairColor = remote.HairColor;
    }

    auto* pActor = RE::TESForm::LookupByID<RE::Actor>(aFormId);
    if (!pActor || pActor != FindCopy(aServerId))
        return;

    {
        std::lock_guard lock(g_lock);
        if (auto it = g_remotes.find(aServerId); it != g_remotes.end())
            it->second.HairColorHeld = HoldsOwnHairColor(pActor, pHairColor);
    }

    // Rebuilding the head dropped the tints Skyrim Together painted on it.
    SendFaceTints(aServerId, tints);
    EnforceBodyMorphs(aServerId, pActor, morphs);
    if (aSettlePass && overlays)
        EnsureOverlays(aServerId, pActor);
}

// What one load needs, copied out of g_remotes so that no lock is held while RaceMenu is called.
struct LoadJob
{
    std::uint32_t ServerId;
    std::uint32_t Revision;
    std::string Json;
    bool HasOverlayNodes;
    RE::BGSColorForm* HairColor;
    bool SettlePass;
};

void StartLoad(const LoadJob& acJob, RE::Actor* apActor)
{
    const auto cServerId = acJob.ServerId;
    const auto cRevision = acJob.Revision;
    const auto cSettlePass = acJob.SettlePass;
    const auto cFormId = apActor->GetFormID();
    const void* pRoot = apActor->Get3D();

    const std::string cName = RemotePresetName(cServerId);
    if (!WriteFile(PresetPath(cName), acJob.Json))
    {
        SKSE::log::warn("Could not write {} for RaceMenu to load", PresetPath(cName).string());
        OnLoaded(cServerId, cRevision, cFormId, pRoot, cSettlePass, false);
        return;
    }

    auto* pHairColor = acJob.HairColor;
    const RE::ActorHandle cHandle = apActor->GetHandle();
    auto load = [=]()
    {
        const auto pActor = cHandle.get();
        const bool cQueued = pActor && CallStatic("CharGen", "LoadCharacterPresetEx",
                                                  MakeArguments(pActor.get(), RE::BSFixedString(cName.c_str()), static_cast<RE::BGSColorForm*>(pHairColor), std::int32_t{kPresetApplyAll}),
                                                  [=](const RE::BSScript::Variable& acResult)
                                                  {
                                                      const bool cLoaded = acResult.IsBool() && acResult.GetBool();
                                                      SKSE::GetTaskInterface()->AddTask([=]() { OnLoaded(cServerId, cRevision, cFormId, pRoot, cSettlePass, cLoaded); });
                                                  });
        if (!cQueued)
            OnLoaded(cServerId, cRevision, cFormId, pRoot, cSettlePass, false);
    };

    g_loadInFlight = true;
    g_loadStarted = Clock::now();

    // RaceMenu only builds overlays on a copy it has not registered, and builds them on a task after the load. Dropping
    // the registration first makes the load queue a fresh build ahead of the overlay overrides.
    if (acJob.HasOverlayNodes)
    {
        const bool cQueued = CallStatic("NiOverride", "RemoveOverlays", MakeArguments(static_cast<RE::TESObjectREFR*>(apActor)),
                                        [load](const RE::BSScript::Variable&) { SKSE::GetTaskInterface()->AddTask(load); });
        if (cQueued)
            return;
    }
    load();
}

void ApplyTick()
{
    if (g_loadInFlight)
    {
        if (Clock::now() - g_loadStarted < kLoadTimeout)
            return;
        // RaceMenu or the script engine is stuck on it: leave the evidence, then move on.
        SKSE::log::error("A RaceMenu load did not report back within {} s", std::chrono::duration_cast<std::chrono::seconds>(kLoadTimeout).count());
        Safety::WriteDump("RaceMenu load did not report back");
        g_loadInFlight = false;
    }

    struct Check
    {
        std::uint32_t ServerId;
        RE::Actor* Actor;
        std::vector<BodyMorph> Morphs;
        bool Overlays;
    };
    std::optional<LoadJob> job;
    RE::Actor* pTarget = nullptr;
    std::vector<Check> checks;
    {
        std::lock_guard lock(g_lock);
        const auto cNow = Clock::now();
        for (auto& [serverId, remote] : g_remotes)
        {
            if (remote.Json.empty() || remote.FailedAttempts >= kMaxLoadAttempts)
                continue;

            // Wait for the body: RaceMenu rebuilds the head and morphs on the loaded 3D.
            auto* pActor = FindCopy(serverId);
            if (!pActor || !pActor->Get3D())
                continue;

            bool apply = remote.AppliedRevision != remote.Revision || remote.AppliedFormId != pActor->GetFormID() || remote.AppliedRoot != pActor->Get3D();

            // Skyrim Together rebuilding the copy's base from the owner's appearance points its hair back at the shared form.
            if (!apply && remote.HairColorHeld && !HoldsOwnHairColor(pActor, remote.HairColor))
            {
                remote.HairColorHeld = false;
                SKSE::log::info("The copy of {:X} lost its own hair color, loading the appearance again", serverId);
                apply = true;
            }

            const bool cSettle = !apply && remote.SettleAt && cNow >= *remote.SettleAt;
            if (apply || cSettle)
            {
                // One load at a time; the others wait for a later tick.
                if (job)
                    continue;

                if (!remote.HairColor)
                {
                    if (auto* pFactory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>())
                    {
                        if (auto* pColor = static_cast<RE::BGSColorForm*>(pFactory->Create()))
                        {
                            pColor->color.red = pColor->color.green = pColor->color.blue = 0;
                            pColor->color.alpha = 0xFF;
                            remote.HairColor = pColor;
                        }
                    }
                    if (!remote.HairColor)
                        SKSE::log::warn("Could not create a hair color form for {:X}; its copy keeps the shared one", serverId);
                }

                job = LoadJob{serverId, remote.Revision, remote.Json, remote.HasOverlayNodes, remote.HairColor, cSettle};
                pTarget = pActor;
                continue;
            }

            if (cNow >= remote.MorphCheckAt)
            {
                remote.MorphCheckAt = cNow + kMorphCheckInterval;
                checks.push_back({serverId, pActor, remote.BodyMorphs, remote.HasOverlayNodes});
            }
        }
    }

    if (job)
    {
        StartLoad(*job, pTarget);
        return; // the checks wait for a tick without a load
    }
    for (const auto& cCheck : checks)
    {
        EnforceBodyMorphs(cCheck.ServerId, cCheck.Actor, cCheck.Morphs);
        if (cCheck.Overlays)
            EnsureOverlays(cCheck.ServerId, cCheck.Actor);
    }
}

// ---- Messages (Skyrim Together's thread) --------------------------------------------------------------------------

void OnPiece(std::uint32_t aServerId, std::string_view aRest)
{
    std::string_view field;
    std::uint32_t revision = 0, piece = 0, pieces = 0;
    if (!NextField(aRest, field) || !ParseNumber(field, revision) || !NextField(aRest, field) || !ParseNumber(field, piece) || !NextField(aRest, field) || !ParseNumber(field, pieces) ||
        pieces == 0 || pieces > kMaxPieces || piece >= pieces || aRest.empty())
        return;

    std::string blob;
    {
        std::lock_guard lock(g_lock);
        auto& remote = g_remotes[aServerId];
        if (remote.Revision == revision && !remote.Json.empty())
            return; // sent again for somebody else: we have it

        if (remote.IncomingRevision != revision || remote.IncomingCount != pieces)
        {
            remote.IncomingRevision = revision;
            remote.IncomingCount = pieces;
            remote.IncomingReceived = 0;
            remote.IncomingPieces.assign(pieces, {});
        }
        if (!remote.IncomingPieces[piece].empty())
            return;
        remote.IncomingPieces[piece].assign(aRest);
        if (++remote.IncomingReceived < remote.IncomingCount)
            return;

        for (const auto& cPiece : remote.IncomingPieces)
            blob += cPiece;
        remote.IncomingPieces.clear();
        remote.IncomingCount = remote.IncomingReceived = 0;
    }

    // Unpacked and parsed outside the lock.
    std::string json;
    std::vector<Tint> tints;
    if (!DecodeBlob(blob, json, tints))
    {
        SKSE::log::warn("Could not unpack the appearance of {:X}", aServerId);
        return;
    }
    std::vector<BodyMorph> morphs = ParseBodyMorphs(json);
    const bool cOverlays = json.find("[Ovl") != std::string::npos || json.find("[SOvl") != std::string::npos;
    const auto cJsonSize = json.size();

    std::lock_guard lock(g_lock);
    auto& remote = g_remotes[aServerId];
    remote.Revision = revision;
    remote.Json = std::move(json);
    remote.Tints = std::move(tints);
    remote.BodyMorphs = std::move(morphs);
    remote.HasOverlayNodes = cOverlays;
    remote.FailedAttempts = 0;
    remote.SettleAt.reset();
    SKSE::log::info("Received the appearance of {:X}, revision {}, {} bytes, {} face tints, {} body morphs", aServerId, revision, cJsonSize, remote.Tints.size(), remote.BodyMorphs.size());
}

void OnNearMessage(std::uint32_t, std::uint32_t aServerId, const char* apBytes, size_t aLength)
{
    if (!g_raceMenu.load(std::memory_order_relaxed) || !apBytes || aLength < 2 || aLength > STAPI::kMaxNearMessageSize)
        return;
    std::string_view rest(apBytes, aLength);
    std::string_view kind;
    if (!NextField(rest, kind) || kind.size() != 1)
        return;

    if (kind == "D")
    {
        OnPiece(aServerId, rest);
    }
    else if (kind == "A")
    {
        // Someone in range has this revision: ask for it unless we have it (or asked a moment ago).
        std::uint32_t revision = 0;
        if (!ParseNumber(rest, revision))
            return;
        const auto cOurServerId = g_pApi->GetServerId(kPlayerFormId);
        if (cOurServerId == 0)
            return;
        {
            std::lock_guard lock(g_lock);
            auto& remote = g_remotes[aServerId];
            if ((remote.Revision == revision && !remote.Json.empty()) || Clock::now() - remote.LastRequest < kRequestInterval)
                return;
            remote.LastRequest = Clock::now();
        }
        const auto cRequest = std::format("Q|{:X}|{}", aServerId, revision);
        g_pApi->SendNear(g_channel, cOurServerId, STAPI::Delivery::Reliable, cRequest.data(), cRequest.size());
    }
    else if (kind == "Q")
    {
        // Somebody in range asks for our appearance.
        std::string_view field;
        std::uint32_t target = 0, revision = 0;
        if (!NextField(rest, field) || !ParseNumber(field, target, 16) || !ParseNumber(rest, revision))
            return;
        if (target != g_pApi->GetServerId(kPlayerFormId))
            return;
        std::lock_guard lock(g_lock);
        if (revision == g_revision && !g_blob.empty() && Clock::now() - g_lastFullSend >= kResendInterval)
            QueueFullSend(g_blob, g_revision);
    }
}

// ---- Pacing thread -----------------------------------------------------------------------------------------------

void PacingLoop()
{
    double budget = kPieceBurst;
    bool wasSupported = false;
    std::optional<Clock::time_point> exportAt;
    auto nextAnnounce = Clock::now();
    auto nextApply = Clock::now();

    for (;;)
    {
        std::this_thread::sleep_for(kTick);
        const auto cNow = Clock::now();

        // A new connection: forget the last one's players, and export once the character has settled.
        const bool cSupported = g_pApi->IsNearSupported();
        if (cSupported != wasSupported)
        {
            wasSupported = cSupported;
            std::lock_guard lock(g_lock);
            g_remotes.clear();
            g_outgoing.clear();
            g_blob.clear();
            exportAt.reset();
            if (cSupported)
                exportAt = cNow + kJoinDelay;
        }
        if (!cSupported)
            continue;

        if (exportAt && cNow >= *exportAt)
        {
            exportAt.reset();
            SKSE::GetTaskInterface()->AddTask([]() { Export("joined"); });
        }

        const auto cServerId = g_pApi->GetServerId(kPlayerFormId);
        budget = std::min(kPieceBurst, budget + std::chrono::duration<double>(kTick).count() * kPiecesPerSecond);
        if (cServerId != 0)
        {
            std::lock_guard lock(g_lock);
            while (!g_outgoing.empty() && budget >= 1.0)
            {
                const auto& cWire = g_outgoing.front();
                if (g_pApi->SendNear(g_channel, cServerId, STAPI::Delivery::Reliable, cWire.data(), cWire.size()) != STAPI::SendResult::Ok)
                    break;
                g_outgoing.pop_front();
                budget -= 1.0;
            }

            if (cNow >= nextAnnounce && g_revision != 0 && !g_blob.empty())
            {
                nextAnnounce = cNow + kAnnounceInterval;
                const auto cAnnounce = std::format("A|{}", g_revision);
                g_pApi->SendNear(g_channel, cServerId, STAPI::Delivery::Unreliable, cAnnounce.data(), cAnnounce.size());
            }
        }

        if (cNow >= nextApply)
        {
            nextApply = cNow + kApplyInterval;
            SKSE::GetTaskInterface()->AddTask([]() { ApplyTick(); });
        }
    }
}

// ---- Setup -------------------------------------------------------------------------------------------------------

void FindBodyMorphInterface()
{
    Skee::InterfaceExchangeMessage exchange;
    SKSE::GetMessagingInterface()->Dispatch(Skee::InterfaceExchangeMessage::kMessage_ExchangeInterface, &exchange, sizeof(exchange), "SKEE");
    if (!exchange.InterfaceMap)
    {
        SKSE::log::warn("RaceMenu did not hand out its interfaces: BodyGen and OBody morphs on copies are not replaced");
        return;
    }

    auto* pInterface = static_cast<Skee::IBodyMorphInterface*>(exchange.InterfaceMap->QueryInterface("BodyMorph"));
    const auto cVersion = pInterface ? pInterface->GetVersion() : 0;
    if (cVersion < Skee::IBodyMorphInterface::kMinimumVersion)
    {
        SKSE::log::warn("RaceMenu's body morph interface is version {} (needs {}): BodyGen and OBody morphs on copies are not replaced", cVersion,
                        Skee::IBodyMorphInterface::kMinimumVersion);
        return;
    }
    g_pBodyMorph = pInterface;
    SKSE::log::info("RaceMenu body morph interface version {}", cVersion);
}

void OnSkseMessage(SKSE::MessagingInterface::Message* apMessage)
{
    switch (apMessage->type)
    {
    case SKSE::MessagingInterface::kPostPostLoad:
        // Every SKSE plugin is loaded by now. RaceMenu answers from PostLoad on.
        if (!GetModuleHandleW(L"skee64.dll"))
        {
            SKSE::log::warn("RaceMenu (skee64.dll) is not loaded: appearances are neither sent nor applied on this machine");
            break;
        }
        g_raceMenu = true;
        FindBodyMorphInterface();
        break;
    case SKSE::MessagingInterface::kDataLoaded:
    {
        if (!g_raceMenu)
            break;
        static MenuSink s_menuSink;
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&s_menuSink);
        Safety::StartFreezeWatchdog(g_pApi);
        // Runs until the process exits.
        std::thread(PacingLoop).detach();
        break;
    }
    default: break;
    }
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface* apSkse)
{
    SKSE::Init(apSkse);
    InitLog("RaceMenuTogether.log");
    Safety::InstallTerminateHandler(kPluginName);

    g_pApi = FindApi();
    if (!g_pApi)
        return true;

    g_pApi3 = FindApi3();
    SKSE::log::info("Plugin API V3 (face tints on copies): {}", g_pApi3 ? "yes" : "no, copies keep Skyrim Together's own face tints");

    g_channel = g_pApi->RegisterChannel(kChannelName, nullptr, nullptr);
    if (!g_channel || !g_pApi->SetNearMessageCallback(g_channel, OnNearMessage))
    {
        SKSE::log::error("Could not register channel {}", kChannelName);
        g_channel = 0;
        return true;
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnSkseMessage);
    SKSE::log::info("Registered channel {}", kChannelName);
    return true;
}
