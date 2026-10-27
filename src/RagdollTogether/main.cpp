// Ragdoll Together 2: makes a knocked down, dying or dead actor's ragdoll look the same on every screen, as a
// standalone SKSE plugin on Skyrim Together's Plugin API (V3). Nothing is patched into Skyrim Together.
//
// Skyrim Together does not run the physics of actors another client owns, so a copy that is knocked down has no
// ragdoll and lies in the skeleton's rest pose. Running the physics on every client would not help either: each
// simulation would land the body somewhere else. So the owner's simulation is the only one. While an actor this client
// owns is ragdolled, the world transforms of the skeleton bones that carry its ragdoll bodies are sampled as the
// renderer begins each frame (after the frame's animation, physics and ragdoll updates) and sent as messages about the
// actor, stamped with Skyrim Together's network tick. The other clients play them back a little behind the copy's
// movement (its playback tick) and, at that same point of the frame so that nothing overwrites them before they are
// drawn, put each of those bones where the owner has it, filling in the bones between them from the copy's own pose. A
// body at rest keeps its pose, also for players who arrive later. The owner keeps streaming through the get-up, and
// once the actor is back on its feet the copy blends back to its animation.
//
// Moving poses go unreliable (Skyrim Together keeps only the newest per actor waiting, under the server's limit), rest
// and end poses reliable. Needs Plugin API V3 (network and playback ticks); without it, or on a server without
// messages about actors, ragdolls are not shared and nothing else changes.
//
// Replaces mailamea's RagdollTogether.dll 1.x, which switched on the same work built into the Skyrim Together exe.
// It does not export that version's descriptor, so the built-in copy (in builds that have it) stays off. The pose
// format is hers.

#include "Common/Safety.h"

#include <MinHook.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using namespace Together;
using Clock = std::chrono::steady_clock;

constexpr const char* kPluginName = "RagdollTogether";
constexpr const char* kChannelName = "RagdollTogether.v2";
constexpr std::uint8_t kWireVersion = 1;

// BSGraphics::Renderer::Begin(uint32_t windowId), Address Library ID on 1.6 and 1.7.
constexpr std::uint64_t kRendererBeginId = 77245;

// How often poses go out while a body moves, at an even pace; playback interpolates between them.
constexpr double kSendInterval = 1.0 / 15.0;
// Owned bodies are sampled this often as frames begin, twice the send rate, rather than on every frame.
constexpr double kSampleInterval = 1.0 / 30.0;
// A still body waits this long before it is sent as at rest.
constexpr double kRestAfter = 0.75;
// A body at rest is sent again now and then, for players who come into range or reload it.
constexpr double kRestKeepAliveAlive = 1.0;
constexpr double kRestKeepAliveDead = 5.0;
// After the ragdoll the owner keeps sending what it draws, so the copies show the same get-up; the ragdoll ends once
// the actor has been back on its feet this long (a short break between two knock states does not end it), or after the
// longest get-up.
constexpr double kEndAfterNormal = 0.75;
constexpr double kMaxGetUp = 8.0;
// Warned about once a ragdoll has had no bones for this long (the first frame has not been sampled yet).
constexpr double kMissingWarnAfter = 0.5;
// Below these a body counts as still.
constexpr float kStillDistance = 0.25f;
constexpr float kStillRotationDot = 0.9998f;
// Blending the copy onto the owner's pose, and back to its animation once the ragdoll ends.
constexpr float kBlendInSeconds = 0.12f;
constexpr float kBlendOutSeconds = 0.3f;
// A copy that stops hearing about a ragdoll gives it up after this long (a body at rest: longer, unless dead).
constexpr double kStreamTimeout = 6.0;
constexpr double kRestTimeout = 15.0;
constexpr size_t kMaxSnapshots = 32;
// Never play back further behind the newest pose than this, should the movement clock stall.
constexpr double kMaxPlaybackLag = 500.0;
// Poses play back this far (ms) behind the copy's movement, which is about one movement interval behind its owner: a
// pose interval more, so the next pose has normally arrived. One that is late is extrapolated for a short while.
constexpr double kPlaybackDelay = 70.0;
constexpr double kMaxExtrapolation = 100.0;
// Only between poses this close (ms): across a longer gap the movement is not known well enough.
constexpr double kMaxExtrapolationSpan = 200.0;
// A skeleton has fewer ragdoll bodies than this.
constexpr size_t kMaxBones = 48;
// Offsets from the first bone, in 1/64 game units: +-512 units covers any skeleton.
constexpr float kOffsetScale = 64.f;
// A frame longer than this is a stall or a loading screen, not time for the blends to move on by.
constexpr double kMaxFrameDelta = 0.25;

enum PoseFlags : std::uint8_t
{
    // The ragdoll ended (the actor is back on its feet); blend back to the animation.
    kEnd = 1 << 0,
    // The bodies came to rest; this pose holds until the next one.
    kRest = 1 << 1,
};

// The ragdoll bodies of a skeleton hang off its bones as blend collision objects.
constexpr const char* kRagdollCollisionRtti = "bhkBlendCollisionObject";

struct Bone
{
    // 16-bit hash of the bone's name; bones are matched across clients by it.
    std::uint16_t NameHash{};
    float Position[3]{};
    // x, y, z, w
    float Rotation[4]{0.f, 0.f, 0.f, 1.f};
};
using Bones = std::vector<Bone>;

// NiTransform: rotation (3x3, acting on column vectors), translation, scale.
struct Transform
{
    float Rotation[3][3];
    float Position[3];
    float Scale;
};
static_assert(sizeof(Transform) == sizeof(RE::NiTransform));

Transform& Local(RE::NiAVObject* apObject) noexcept
{
    return *reinterpret_cast<Transform*>(&apObject->local);
}

Transform& WorldTransform(RE::NiAVObject* apObject) noexcept
{
    return *reinterpret_cast<Transform*>(&apObject->world);
}

// The RTTI of bhkBlendCollisionObject once one has been seen, so later nodes compare a pointer, not a name.
const RE::NiRTTI* s_pRagdollRtti = nullptr;

bool IsRagdollBone(const RE::NiAVObject* apObject) noexcept
{
    const RE::NiCollisionObject* pCollision = apObject->collisionObject.get();
    if (!pCollision)
        return false;
    const RE::NiRTTI* pRtti = pCollision->GetRTTI();
    if (!pRtti)
        return false;
    if (pRtti == s_pRagdollRtti)
        return true;
    if (s_pRagdollRtti || !pRtti->name || std::strcmp(pRtti->name, kRagdollCollisionRtti) != 0)
        return false;
    s_pRagdollRtti = pRtti;
    return true;
}

std::uint16_t HashName(const char* acpName) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const char* p = acpName; p && *p; ++p)
    {
        hash ^= static_cast<std::uint8_t>(*p);
        hash *= 16777619u;
    }
    return static_cast<std::uint16_t>(hash ^ (hash >> 16));
}

// ---- Rotation math; matrices act on column vectors (Gamebryo), quaternions are x, y, z, w ----------------------------

void MatrixToQuat(const float m[3][3], float q[4]) noexcept
{
    const float cTrace = m[0][0] + m[1][1] + m[2][2];
    if (cTrace > 0.f)
    {
        const float s = std::sqrt(cTrace + 1.f) * 2.f;
        q[3] = 0.25f * s;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const float s = std::sqrt(1.f + m[0][0] - m[1][1] - m[2][2]) * 2.f;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        const float s = std::sqrt(1.f + m[1][1] - m[0][0] - m[2][2]) * 2.f;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        const float s = std::sqrt(1.f + m[2][2] - m[0][0] - m[1][1]) * 2.f;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25f * s;
    }

    const float cLength = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] /= cLength > 1e-6f ? cLength : 1.f;
}

void QuatToMatrix(const float q[4], float m[3][3]) noexcept
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1.f - 2.f * (y * y + z * z);
    m[0][1] = 2.f * (x * y - z * w);
    m[0][2] = 2.f * (x * z + y * w);
    m[1][0] = 2.f * (x * y + z * w);
    m[1][1] = 1.f - 2.f * (x * x + z * z);
    m[1][2] = 2.f * (y * z - x * w);
    m[2][0] = 2.f * (x * z - y * w);
    m[2][1] = 2.f * (y * z + x * w);
    m[2][2] = 1.f - 2.f * (x * x + y * y);
}

float QuatDot(const float a[4], const float b[4]) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

void Slerp(const float a[4], const float b[4], float t, float out[4]) noexcept
{
    float cosTheta = QuatDot(a, b);
    float target[4] = {b[0], b[1], b[2], b[3]};
    if (cosTheta < 0.f)
    {
        cosTheta = -cosTheta;
        for (float& value : target)
            value = -value;
    }

    float wa, wb;
    if (cosTheta > 0.9995f)
    {
        wa = 1.f - t;
        wb = t;
    }
    else
    {
        const float cTheta = std::acos(cosTheta);
        const float cSin = std::sin(cTheta);
        wa = std::sin((1.f - t) * cTheta) / cSin;
        wb = std::sin(t * cTheta) / cSin;
    }

    float length = 0.f;
    for (int i = 0; i < 4; ++i)
    {
        out[i] = wa * a[i] + wb * target[i];
        length += out[i] * out[i];
    }
    length = std::sqrt(length);
    for (int i = 0; i < 4; ++i)
        out[i] /= length > 1e-6f ? length : 1.f;
}

void Multiply(const float a[3][3], const float b[3][3], float out[3][3]) noexcept
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

// out = a^T * b
void MultiplyTransposed(const float a[3][3], const float b[3][3], float out[3][3]) noexcept
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r][c] = a[0][r] * b[0][c] + a[1][r] * b[1][c] + a[2][r] * b[2][c];
}

Transform Compose(const Transform& acParent, const Transform& acLocal) noexcept
{
    Transform result;
    Multiply(acParent.Rotation, acLocal.Rotation, result.Rotation);
    for (int r = 0; r < 3; ++r)
    {
        result.Position[r] = acParent.Position[r] +
                             acParent.Scale * (acParent.Rotation[r][0] * acLocal.Position[0] + acParent.Rotation[r][1] * acLocal.Position[1] + acParent.Rotation[r][2] * acLocal.Position[2]);
    }
    result.Scale = acParent.Scale * acLocal.Scale;
    return result;
}

// ---- The wire: one message per pose, little-endian ------------------------------------------------------------------
//   u8 version, u64 owner tick, u8 flags, u8 bone count, then per bone: u16 name hash, the position (the first bone as
//   three f32, the others as three i16 offsets from it in 1/64 units) and the rotation as its three smallest components
//   (u8 index of the largest, three i16). 48 bones come to 731 bytes, under kMaxNearMessageSize.

template <class T> void Put(std::string& aOut, T aValue)
{
    char bytes[sizeof(T)];
    std::memcpy(bytes, &aValue, sizeof(T));
    aOut.append(bytes, sizeof(T));
}

template <class T> bool Take(const char* apIn, size_t aLength, size_t& aPosition, T& aValue) noexcept
{
    if (aPosition + sizeof(T) > aLength)
        return false;
    std::memcpy(&aValue, apIn + aPosition, sizeof(T));
    aPosition += sizeof(T);
    return true;
}

std::int16_t ToInt16(float aValue) noexcept
{
    return static_cast<std::int16_t>(std::lround(std::clamp(aValue, -32767.f, 32767.f)));
}

// Components other than the largest lie within +-1/sqrt(2).
constexpr float kQuatScale = 32767.f * 1.41421356f;

void PutRotation(std::string& aOut, const float acQ[4])
{
    std::uint8_t largest = 0;
    for (std::uint8_t i = 1; i < 4; ++i)
    {
        if (std::fabs(acQ[i]) > std::fabs(acQ[largest]))
            largest = i;
    }
    const float cSign = acQ[largest] < 0.f ? -1.f : 1.f;
    Put<std::uint8_t>(aOut, largest);
    for (std::uint8_t i = 0; i < 4; ++i)
    {
        if (i != largest)
            Put<std::int16_t>(aOut, ToInt16(acQ[i] * cSign * kQuatScale));
    }
}

bool TakeRotation(const char* apIn, size_t aLength, size_t& aPosition, float aQ[4]) noexcept
{
    std::uint8_t largest = 0;
    if (!Take(apIn, aLength, aPosition, largest) || largest > 3)
        return false;

    float sum = 0.f;
    for (std::uint8_t i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        std::int16_t value = 0;
        if (!Take(apIn, aLength, aPosition, value))
            return false;
        aQ[i] = static_cast<float>(value) / kQuatScale;
        sum += aQ[i] * aQ[i];
    }
    aQ[largest] = std::sqrt(std::max(0.f, 1.f - sum));
    return true;
}

struct Snapshot
{
    std::uint64_t Tick{};
    std::uint8_t Flags{};
    Bones Bones{};
};

bool ParsePose(const char* apBytes, size_t aLength, Snapshot& aOut) noexcept
{
    size_t position = 0;
    std::uint8_t version = 0, flags = 0, boneCount = 0;
    std::uint64_t tick = 0;
    if (!apBytes || !Take(apBytes, aLength, position, version) || version != kWireVersion || !Take(apBytes, aLength, position, tick) || !Take(apBytes, aLength, position, flags) ||
        !Take(apBytes, aLength, position, boneCount) || boneCount == 0 || boneCount > kMaxBones)
        return false;

    aOut.Tick = tick;
    aOut.Flags = flags;
    aOut.Bones.resize(boneCount);
    for (std::uint8_t b = 0; b < boneCount; ++b)
    {
        Bone& bone = aOut.Bones[b];
        if (!Take(apBytes, aLength, position, bone.NameHash))
            return false;
        if (b == 0)
        {
            for (float& value : bone.Position)
            {
                if (!Take(apBytes, aLength, position, value) || !std::isfinite(value))
                    return false;
            }
        }
        else
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                std::int16_t offset = 0;
                if (!Take(apBytes, aLength, position, offset))
                    return false;
                bone.Position[axis] = aOut.Bones[0].Position[axis] + static_cast<float>(offset) / kOffsetScale;
            }
        }
        if (!TakeRotation(apBytes, aLength, position, bone.Rotation))
            return false;
    }
    return true;
}

std::string EncodePose(std::uint64_t aTick, std::uint8_t aFlags, const Bones& acBones)
{
    const size_t cBones = std::min(acBones.size(), kMaxBones);
    std::string wire;
    wire.reserve(12 + cBones * 15);
    Put<std::uint8_t>(wire, kWireVersion);
    Put<std::uint64_t>(wire, aTick);
    Put<std::uint8_t>(wire, aFlags);
    Put<std::uint8_t>(wire, static_cast<std::uint8_t>(cBones));
    for (size_t b = 0; b < cBones; ++b)
    {
        const auto& bone = acBones[b];
        Put<std::uint16_t>(wire, bone.NameHash);
        if (b == 0)
        {
            for (const float cValue : bone.Position)
                Put<float>(wire, cValue);
        }
        else
        {
            for (int axis = 0; axis < 3; ++axis)
                Put<std::int16_t>(wire, ToInt16((bone.Position[axis] - acBones[0].Position[axis]) * kOffsetScale));
        }
        PutRotation(wire, bone.Rotation);
    }
    return wire;
}

// ---- Poses on the skeleton (main thread, as the renderer begins a frame) ---------------------------------------------

struct HookTarget
{
    Bones Bones;
    float Weight{};
};

const Bone* FindBone(const Bones& acBones, std::uint16_t aHash, size_t aHint) noexcept
{
    if (aHint < acBones.size() && acBones[aHint].NameHash == aHash)
        return &acBones[aHint];
    for (const auto& bone : acBones)
    {
        if (bone.NameHash == aHash)
            return &bone;
    }
    return nullptr;
}

// Walks the skeleton from the 3D root: puts the bones with a target where the target has them (blended by weight) and
// keeps every world transform below consistent, so the frame renders the result whether or not the engine's own world
// update already ran.
void ApplyPose(RE::NiAVObject* apObject, const Transform& acParentWorld, const HookTarget& acTarget, size_t& aBoneIndex, int aDepth) noexcept
{
    if (!apObject || aDepth > 64)
        return;

    Transform& local = Local(apObject);

    if (IsRagdollBone(apObject))
    {
        if (const auto* pBone = FindBone(acTarget.Bones, HashName(apObject->name.c_str()), aBoneIndex))
        {
            // Desired local = parent world^-1 * target world.
            float targetRotation[3][3];
            QuatToMatrix(pBone->Rotation, targetRotation);

            Transform desired = local;
            MultiplyTransposed(acParentWorld.Rotation, targetRotation, desired.Rotation);
            const float cDelta[3] = {pBone->Position[0] - acParentWorld.Position[0], pBone->Position[1] - acParentWorld.Position[1], pBone->Position[2] - acParentWorld.Position[2]};
            const float cInvScale = acParentWorld.Scale != 0.f ? 1.f / acParentWorld.Scale : 1.f;
            for (int c = 0; c < 3; ++c)
                desired.Position[c] = (acParentWorld.Rotation[0][c] * cDelta[0] + acParentWorld.Rotation[1][c] * cDelta[1] + acParentWorld.Rotation[2][c] * cDelta[2]) * cInvScale;

            if (acTarget.Weight >= 0.999f)
            {
                std::memcpy(local.Rotation, desired.Rotation, sizeof(local.Rotation));
                std::memcpy(local.Position, desired.Position, sizeof(local.Position));
            }
            else
            {
                float from[4], to[4], blended[4];
                MatrixToQuat(local.Rotation, from);
                MatrixToQuat(desired.Rotation, to);
                Slerp(from, to, acTarget.Weight, blended);
                QuatToMatrix(blended, local.Rotation);
                for (int c = 0; c < 3; ++c)
                    local.Position[c] += (desired.Position[c] - local.Position[c]) * acTarget.Weight;
            }
        }
        ++aBoneIndex;
    }

    const Transform cWorld = Compose(acParentWorld, local);
    WorldTransform(apObject) = cWorld;

    if (RE::NiNode* pNode = apObject->AsNode())
    {
        for (auto& child : pNode->GetChildren())
            ApplyPose(child.get(), cWorld, acTarget, aBoneIndex, aDepth + 1);
    }
}

void SamplePose(RE::NiAVObject* apObject, Bones& aBones, int aDepth)
{
    if (!apObject || aDepth > 64 || aBones.size() >= kMaxBones)
        return;

    if (IsRagdollBone(apObject))
    {
        const Transform& world = WorldTransform(apObject);
        Bone bone;
        bone.NameHash = HashName(apObject->name.c_str());
        std::memcpy(bone.Position, world.Position, sizeof(bone.Position));
        MatrixToQuat(world.Rotation, bone.Rotation);
        aBones.push_back(bone);
    }

    if (RE::NiNode* pNode = apObject->AsNode())
    {
        for (auto& child : pNode->GetChildren())
            SamplePose(child.get(), aBones, aDepth + 1);
    }
}

// The actor's third-person root (copies and NPCs only have that one).
RE::NiAVObject* Root(RE::Actor* apActor) noexcept
{
    return apActor ? apActor->Get3D(false) : nullptr;
}

void ApplyTarget(RE::Actor* apActor, const HookTarget& acTarget) noexcept
{
    RE::NiAVObject* pRoot = Root(apActor);
    RE::NiNode* pRootNode = pRoot ? pRoot->AsNode() : nullptr;
    if (!pRootNode || acTarget.Bones.empty() || acTarget.Weight <= 0.f)
        return;

    const Transform cRootWorld = WorldTransform(pRoot);
    size_t boneIndex = 0;
    for (auto& child : pRootNode->GetChildren())
        ApplyPose(child.get(), cRootWorld, acTarget, boneIndex, 1);
}

// Knock states: 1 and 2 are the fall, 3 and 4 a knockout, 6 the get-up that follows (5 is the short wait between the
// fall and the get-up, 0 standing). Life states: 1 dying, 2 dead, 3 unconscious.
std::uint32_t GetKnockState(RE::Actor* apActor) noexcept
{
    return static_cast<std::uint32_t>(apActor->AsActorState()->GetKnockState());
}

bool IsDeadOrDying(RE::Actor* apActor) noexcept
{
    const auto cLifeState = static_cast<std::uint32_t>(apActor->AsActorState()->GetLifeState());
    return cLifeState == 1 || cLifeState == 2;
}

// Whether the actor's own simulation has it in a ragdoll.
bool IsRagdolled(RE::Actor* apActor) noexcept
{
    const auto cLifeState = static_cast<std::uint32_t>(apActor->AsActorState()->GetLifeState());
    const std::uint32_t cKnockState = GetKnockState(apActor);
    const bool cKnocked = cKnockState == 1 || cKnockState == 2 || cKnockState == 3 || cKnockState == 4 || cKnockState == 6;
    const bool cLimp = cLifeState == 1 || cLifeState == 2 || cLifeState == 3;
    return cKnocked || cLimp;
}

bool IsStill(const Bones& acNow, const Bones& acBefore) noexcept
{
    if (acNow.size() != acBefore.size())
        return false;
    for (size_t i = 0; i < acNow.size(); ++i)
    {
        const auto& a = acNow[i];
        const auto& b = acBefore[i];
        if (a.NameHash != b.NameHash)
            return false;
        const float dx = a.Position[0] - b.Position[0], dy = a.Position[1] - b.Position[1], dz = a.Position[2] - b.Position[2];
        if (dx * dx + dy * dy + dz * dz > kStillDistance * kStillDistance)
            return false;
        if (std::fabs(QuatDot(a.Rotation, b.Rotation)) < kStillRotationDot)
            return false;
    }
    return true;
}

void Interpolate(const Bones& acFrom, const Bones& acTo, float aT, Bones& aOut)
{
    aOut = acTo;
    for (size_t i = 0; i < aOut.size(); ++i)
    {
        const auto* pFrom = FindBone(acFrom, aOut[i].NameHash, i);
        if (!pFrom)
            continue;
        for (int c = 0; c < 3; ++c)
            aOut[i].Position[c] = pFrom->Position[c] + (acTo[i].Position[c] - pFrom->Position[c]) * aT;
        Slerp(pFrom->Rotation, acTo[i].Rotation, aT, aOut[i].Rotation);
    }
}

// ---- State --------------------------------------------------------------------------------------------------------

STAPI::IVSkyrimTogether2* g_pApi = nullptr;
STAPI::IVSkyrimTogether3* g_pApi3 = nullptr;
STAPI::ChannelHandle g_channel = 0;

// An actor this client owns.
struct LocalRagdoll
{
    RE::FormID FormId{};
    bool Active{};
    // Taken over from the previous owner rather than simulated here (a body at rest whose owner left).
    bool Inherited{};
    Bones LastSent{};
    // The pose a still body is measured against, so that slow movement adds up instead of counting as still.
    Bones StillAnchor{};
    double SinceSend{};
    double StillFor{};
    bool RestSent{};
    // Out of the ragdoll (getting up), and back on its feet.
    double OutFor{};
    double NormalFor{};
    double MissingFor{};
};

// An actor another client owns.
struct RemoteRagdoll
{
    std::vector<Snapshot> Snapshots{};
    bool Ended{};
    float Weight{};
    double SinceArrival{};
    // The owner tick shown last; playback never goes back from it.
    double RenderTick{};
};

// Poses arrive on Skyrim Together's thread and wait here for the next frame.
std::mutex g_inboxLock;
std::vector<std::pair<std::uint32_t, Snapshot>> g_inbox;

// Main thread only, all of it touched as the renderer begins a frame.
std::unordered_map<std::uint32_t, LocalRagdoll> g_local;   // by server ID
std::unordered_map<std::uint32_t, RemoteRagdoll> g_remote; // by server ID
std::unordered_map<RE::FormID, HookTarget> g_targets;
std::unordered_set<RE::FormID> g_sampleWanted;
std::unordered_map<RE::FormID, Bones> g_samples;
Clock::time_point g_sampleTime{};
Clock::time_point g_lastFrame{};
double g_sendTimer{};
bool g_wasSupported{};
bool g_loggedSend{};
bool g_loggedReceive{};

void Reset() noexcept
{
    g_local.clear();
    g_remote.clear();
    g_targets.clear();
    g_sampleWanted.clear();
    g_samples.clear();
    std::lock_guard lock(g_inboxLock);
    g_inbox.clear();
}

RE::Actor* LookUp(RE::FormID aFormId) noexcept
{
    return aFormId ? RE::TESForm::LookupByID<RE::Actor>(aFormId) : nullptr;
}

// ---- Receiving ----------------------------------------------------------------------------------------------------

void OnNearMessage(std::uint32_t, std::uint32_t aServerId, const char* apBytes, size_t aLength)
{
    Snapshot snapshot;
    if (!ParsePose(apBytes, aLength, snapshot))
        return;
    std::lock_guard lock(g_inboxLock);
    if (g_inbox.size() < 4096)
        g_inbox.emplace_back(aServerId, std::move(snapshot));
}

void DrainInbox()
{
    std::vector<std::pair<std::uint32_t, Snapshot>> inbox;
    {
        std::lock_guard lock(g_inboxLock);
        inbox.swap(g_inbox);
    }

    for (auto& [serverId, snapshot] : inbox)
    {
        // An actor this client does not show, or one it took over since.
        if (g_pApi->IsOwnedLocally(serverId) || !LookUp(g_pApi->GetFormId(serverId)))
            continue;

        if (!g_loggedReceive)
        {
            g_loggedReceive = true;
            SKSE::log::info("First ragdoll pose from another player, for {:X} ({} bones)", serverId, snapshot.Bones.size());
        }

        auto& remote = g_remote[serverId];
        remote.SinceArrival = 0.0;

        // A new ragdoll after the last one ended.
        if (remote.Ended && !(snapshot.Flags & kEnd))
        {
            remote.Ended = false;
            remote.Snapshots.clear();
        }

        // Streaming poses can arrive out of order.
        auto it = std::find_if(remote.Snapshots.begin(), remote.Snapshots.end(), [&](const Snapshot& acOther) { return acOther.Tick >= snapshot.Tick; });
        if (it != remote.Snapshots.end() && it->Tick == snapshot.Tick)
            *it = std::move(snapshot);
        else
            remote.Snapshots.insert(it, std::move(snapshot));

        while (remote.Snapshots.size() > kMaxSnapshots)
            remote.Snapshots.erase(remote.Snapshots.begin());
    }
}

// ---- Owned actors -------------------------------------------------------------------------------------------------

struct OutgoingPose
{
    std::uint32_t ServerId{};
    std::uint8_t Flags{};
    Bones Bones{};
};

void Send(const std::vector<OutgoingPose>& acStreaming, const std::vector<OutgoingPose>& acHeld, double aSampleAgeMs)
{
    // Stamped with the network tick of the frame the poses were taken from, not the time they leave.
    const std::uint64_t cNow = g_pApi3->GetNetworkTick();
    if (cNow == 0)
        return;
    const auto cAge = static_cast<std::uint64_t>(std::clamp(aSampleAgeMs, 0.0, 100.0));
    const std::uint64_t cTick = cNow > cAge ? cNow - cAge : cNow;

    const auto send = [cTick](const OutgoingPose& acPose, STAPI::Delivery aDelivery)
    {
        const std::string cWire = EncodePose(cTick, acPose.Flags, acPose.Bones);
        const auto cResult = g_pApi->SendNear(g_channel, acPose.ServerId, aDelivery, cWire.data(), cWire.size());
        if (cResult == STAPI::SendResult::Ok && !g_loggedSend)
        {
            g_loggedSend = true;
            SKSE::log::info("First ragdoll pose sent, for {:X} ({} bones)", acPose.ServerId, acPose.Bones.size());
        }
    };

    // Streaming poses can be lost (the next one replaces them); the ones that end a ragdoll or hold a body at rest
    // must arrive.
    for (const auto& cPose : acHeld)
        send(cPose, STAPI::Delivery::Reliable);
    for (const auto& cPose : acStreaming)
        send(cPose, STAPI::Delivery::Unreliable);
}

// The actors near the player that this client could own: the player and the high process list.
std::vector<RE::Actor*> NearbyActors()
{
    std::vector<RE::Actor*> actors;
    if (auto* pPlayer = RE::PlayerCharacter::GetSingleton())
        actors.push_back(pPlayer);
    if (auto* pLists = RE::ProcessLists::GetSingleton())
    {
        for (auto& handle : pLists->highActorHandles)
        {
            if (auto pActor = handle.get())
                actors.push_back(pActor.get());
        }
    }
    return actors;
}

void UpdateLocal(double aDelta)
{
    // At an even pace: the copies time their playback by the gaps between poses.
    g_sendTimer += aDelta;
    const bool cSendNow = g_sendTimer >= kSendInterval;
    if (cSendNow)
        g_sendTimer = std::min(g_sendTimer - kSendInterval, kSendInterval);

    std::vector<OutgoingPose> streaming;
    std::vector<OutgoingPose> held;
    std::unordered_set<std::uint32_t> seen;

    for (RE::Actor* pActor : NearbyActors())
    {
        if (!Root(pActor))
            continue;
        const RE::FormID cFormId = pActor->GetFormID();
        const std::uint32_t cServerId = g_pApi->GetServerId(cFormId);
        if (cServerId == 0 || !g_pApi->IsOwnedLocally(cServerId))
            continue;

        const bool cRagdolled = IsRagdolled(pActor);
        if (!cRagdolled && !g_local.contains(cServerId))
            continue;

        seen.insert(cServerId);
        auto& local = g_local[cServerId];

        if (cRagdolled && !local.Active)
        {
            local = LocalRagdoll{};
            local.FormId = cFormId;
            local.Active = true;

            // A body that lay here as another client's copy keeps the pose its owner gave it; this client's engine
            // never simulated it.
            auto remoteIt = g_remote.find(cServerId);
            if (remoteIt != g_remote.end() && !remoteIt->second.Snapshots.empty() && !remoteIt->second.Ended)
            {
                local.Inherited = true;
                g_targets[cFormId] = HookTarget{remoteIt->second.Snapshots.back().Bones, 1.f};
            }
            if (remoteIt != g_remote.end())
                g_remote.erase(remoteIt);

            g_sampleWanted.insert(cFormId);
        }

        local.OutFor = cRagdolled ? 0.0 : local.OutFor + aDelta;
        local.NormalFor = !cRagdolled && GetKnockState(pActor) == 0 ? local.NormalFor + aDelta : 0.0;
        const bool cEnded = !cRagdolled && (local.NormalFor >= kEndAfterNormal || local.OutFor >= kMaxGetUp);

        if (!cSendNow)
            continue;

        Bones bones;
        if (auto sampleIt = g_samples.find(local.FormId); sampleIt != g_samples.end())
            bones = sampleIt->second;

        if (cEnded)
        {
            // Back on its feet: the last pose goes out so every copy blends back from the same place.
            OutgoingPose pose{cServerId, kEnd, bones.empty() ? local.LastSent : bones};
            if (!pose.Bones.empty())
                held.push_back(std::move(pose));

            g_sampleWanted.erase(local.FormId);
            g_samples.erase(local.FormId);
            g_targets.erase(local.FormId);
            g_local.erase(cServerId);
            continue;
        }

        if (bones.empty())
        {
            const bool cWasWarned = local.MissingFor >= kMissingWarnAfter;
            local.MissingFor += kSendInterval;
            if (!cWasWarned && local.MissingFor >= kMissingWarnAfter)
                SKSE::log::warn("No ragdoll bones found on {:08X}; its ragdoll is not shared", local.FormId);
            local.SinceSend += kSendInterval;
            continue;
        }

        // Every pose goes out while the body moves, however slowly; it counts as still (and is then sent as at rest)
        // only while it stays close to where it was when it stopped. A get-up is never at rest.
        local.SinceSend += kSendInterval;
        if (cRagdolled && IsStill(bones, local.StillAnchor))
        {
            local.StillFor += kSendInterval;
        }
        else
        {
            local.StillAnchor = bones;
            local.StillFor = 0.0;
            local.RestSent = false;
        }

        OutgoingPose pose{cServerId, 0, bones};
        if (local.StillFor >= kRestAfter)
        {
            const double cKeepAlive = IsDeadOrDying(pActor) ? kRestKeepAliveDead : kRestKeepAliveAlive;
            if (!local.RestSent || local.SinceSend >= cKeepAlive)
            {
                local.RestSent = true;
                local.SinceSend = 0.0;
                local.LastSent = bones;
                pose.Flags = kRest;
                held.push_back(std::move(pose));
            }
        }
        else
        {
            local.SinceSend = 0.0;
            local.LastSent = bones;
            streaming.push_back(std::move(pose));
        }
    }

    // Actors no longer owned here (or no longer near): whoever owns them now carries on.
    for (auto it = g_local.begin(); it != g_local.end();)
    {
        if (seen.contains(it->first))
        {
            ++it;
            continue;
        }
        g_sampleWanted.erase(it->second.FormId);
        g_samples.erase(it->second.FormId);
        if (it->second.Inherited)
            g_targets.erase(it->second.FormId);
        it = g_local.erase(it);
    }

    if (!streaming.empty() || !held.empty())
        Send(streaming, held, std::chrono::duration<double, std::milli>(Clock::now() - g_sampleTime).count());
}

// ---- Copies -------------------------------------------------------------------------------------------------------

void UpdateRemote(double aDelta)
{
    std::unordered_map<RE::FormID, HookTarget> targets;
    std::unordered_set<std::uint32_t> present;

    for (auto& [serverId, remote] : g_remote)
    {
        if (g_pApi->IsOwnedLocally(serverId))
            continue;
        auto* pActor = LookUp(g_pApi->GetFormId(serverId));
        if (!pActor)
            continue;

        present.insert(serverId);
        remote.SinceArrival += aDelta;
        if (remote.Snapshots.empty())
            continue;

        // Play back on the copy's movement timeline, so the body and the actor's position agree, a little further
        // behind so that the pose after the one shown has normally arrived. Never back in time, unless the movement
        // clock jumped back a long way.
        double renderTick = static_cast<double>(remote.Snapshots.back().Tick);
        if (double playbackTick = 0.0; g_pApi3->GetPlaybackTick(serverId, &playbackTick))
            renderTick = std::max<double>(playbackTick - kPlaybackDelay, renderTick - kMaxPlaybackLag);
        if (renderTick < remote.RenderTick && remote.RenderTick - renderTick < kMaxPlaybackLag)
            renderTick = remote.RenderTick;
        remote.RenderTick = renderTick;

        const Snapshot* pFrom = &remote.Snapshots.front();
        const Snapshot* pTo = pFrom;
        for (const auto& snapshot : remote.Snapshots)
        {
            if (static_cast<double>(snapshot.Tick) <= renderTick)
                pFrom = &snapshot;
            pTo = &snapshot;
            if (static_cast<double>(snapshot.Tick) > renderTick)
                break;
        }

        HookTarget target;
        if (pTo != pFrom && pTo->Tick > pFrom->Tick)
        {
            const float cT = static_cast<float>(std::clamp((renderTick - static_cast<double>(pFrom->Tick)) / static_cast<double>(pTo->Tick - pFrom->Tick), 0.0, 1.0));
            Interpolate(pFrom->Bones, pTo->Bones, cT, target.Bones);
        }
        else if (const double cAhead = renderTick - static_cast<double>(pFrom->Tick); cAhead > 0.0 && remote.Snapshots.size() >= 2 && !(pFrom->Flags & (kRest | kEnd)))
        {
            // The next pose is late: keep the bodies going the way they were for a moment instead of stopping them,
            // then hold.
            const Snapshot& before = remote.Snapshots[remote.Snapshots.size() - 2];
            const double cSpan = static_cast<double>(pFrom->Tick) - static_cast<double>(before.Tick);
            if (cSpan > 0.0 && cSpan <= kMaxExtrapolationSpan)
                Interpolate(before.Bones, pFrom->Bones, static_cast<float>(1.0 + std::min(cAhead, kMaxExtrapolation) / cSpan), target.Bones);
            else
                target.Bones = pFrom->Bones;
        }
        else
        {
            target.Bones = pFrom->Bones;
        }

        // The ragdoll ends once playback reaches the owner's end pose; a stream that stopped without one ends after a
        // while, unless the body is at rest on a dead actor.
        const bool cReachedEnd = (pFrom->Flags & kEnd) != 0;
        const bool cResting = (remote.Snapshots.back().Flags & kRest) != 0;
        const bool cTimedOut = remote.SinceArrival > (cResting ? kRestTimeout : kStreamTimeout) && !IsDeadOrDying(pActor);
        if (cReachedEnd || cTimedOut)
            remote.Ended = true;

        const float cRate = static_cast<float>(aDelta) / (remote.Ended ? -kBlendOutSeconds : kBlendInSeconds);
        remote.Weight = std::clamp(remote.Weight + cRate, 0.f, 1.f);
        target.Weight = remote.Weight;

        if (target.Weight > 0.f)
            targets[pActor->GetFormID()] = std::move(target);
    }

    // Done blending out, or the actor left for good.
    for (auto it = g_remote.begin(); it != g_remote.end();)
    {
        const bool cPresent = present.contains(it->first);
        const bool cFinished = it->second.Ended && it->second.Weight <= 0.f;
        const bool cGone = !cPresent && it->second.SinceArrival > kRestTimeout * 4;
        if (!cPresent)
            it->second.SinceArrival += aDelta;
        it = (cFinished || cGone) ? g_remote.erase(it) : std::next(it);
    }

    // Keep the targets of bodies this client inherited.
    for (const auto& [serverId, local] : g_local)
    {
        if (!local.Inherited)
            continue;
        if (auto it = g_targets.find(local.FormId); it != g_targets.end())
            targets[local.FormId] = it->second;
    }
    g_targets = std::move(targets);
}

// ---- The frame -----------------------------------------------------------------------------------------------------

// As the renderer begins a frame, after the frame's animation, physics and ragdoll updates: whatever the bones hold now
// is what gets drawn, and what an owned body is sampled at is what the owner sees.
void OnFrameBegin()
{
    const auto cNow = Clock::now();
    const double cDelta = g_lastFrame.time_since_epoch().count() == 0 ? 0.0 : std::min(std::chrono::duration<double>(cNow - g_lastFrame).count(), kMaxFrameDelta);
    g_lastFrame = cNow;

    const bool cSupported = g_pApi->IsNearSupported();
    if (cSupported != g_wasSupported)
    {
        g_wasSupported = cSupported;
        Reset();
    }
    if (!cSupported)
        return;

    DrainInbox();
    UpdateLocal(cDelta);
    UpdateRemote(cDelta);

    const bool cSample = !g_sampleWanted.empty() && std::chrono::duration<double>(cNow - g_sampleTime).count() >= kSampleInterval;
    if (cSample)
        g_sampleTime = cNow;

    for (const auto& [formId, target] : g_targets)
        ApplyTarget(LookUp(formId), target);

    if (!cSample)
        return;
    for (const RE::FormID cFormId : g_sampleWanted)
    {
        Bones& bones = g_samples[cFormId];
        bones.clear();
        SamplePose(Root(LookUp(cFormId)), bones, 0);
    }
}

using TRendererBegin = void (*)(void* apRenderer, std::uint32_t aWindowId);
TRendererBegin g_realRendererBegin = nullptr;

void HookRendererBegin(void* apRenderer, std::uint32_t aWindowId)
{
    if (g_channel)
        OnFrameBegin();
    g_realRendererBegin(apRenderer, aWindowId);
}

bool InstallFrameHook()
{
    if (REL::Module::IsSE() || REL::Module::IsVR())
    {
        SKSE::log::warn("The renderer hook is only known on 1.6 and 1.7: ragdolls are not shared on this runtime");
        return false;
    }

    void* pTarget = reinterpret_cast<void*>(REL::ID(kRendererBeginId).address());
    if (const auto cStatus = MH_Initialize(); cStatus != MH_OK && cStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(pTarget, reinterpret_cast<void*>(&HookRendererBegin), reinterpret_cast<void**>(&g_realRendererBegin)) != MH_OK || MH_EnableHook(pTarget) != MH_OK)
    {
        SKSE::log::error("Could not hook the renderer (Address Library ID {}): ragdolls are not shared", kRendererBeginId);
        return false;
    }
    return true;
}

void OnSkseMessage(SKSE::MessagingInterface::Message* apMessage)
{
    if (apMessage->type == SKSE::MessagingInterface::kDataLoaded)
        Safety::StartFreezeWatchdog(g_pApi);
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface* apSkse)
{
    SKSE::Init(apSkse);
    InitLog("RagdollTogether.log");
    Safety::InstallTerminateHandler(kPluginName);

    g_pApi = FindApi();
    if (!g_pApi)
        return true;

    g_pApi3 = FindApi3();
    if (!g_pApi3)
    {
        SKSE::log::warn("This Skyrim Together build has no Plugin API V3 (network and playback ticks): ragdolls are not shared");
        return true;
    }

    if (!InstallFrameHook())
        return true;

    g_channel = g_pApi->RegisterChannel(kChannelName, nullptr, nullptr);
    if (!g_channel || !g_pApi->SetNearMessageCallback(g_channel, OnNearMessage))
    {
        SKSE::log::error("Could not register channel {}", kChannelName);
        g_channel = 0;
        return true;
    }

    SKSE::GetMessagingInterface()->RegisterListener(OnSkseMessage);
    SKSE::log::info("Registered channel {}; ragdolls are shared", kChannelName);
    return true;
}
