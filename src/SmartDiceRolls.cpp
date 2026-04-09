/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#include "SmartDiceRolls.h"

#include "Logger.h"
#include "Config.h"
#include "Utils.h"
#include "rva/RVA.h"
#include "minhook/include/MinHook.h"

// for getting the caller
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <mutex>

#define INI_LOCATION "./mods/smart-dice-rolls-mod.ini"

// Globals
Config g_config;
Logger g_logger;
HMODULE g_bg3BaseAddr = nullptr;
static uintptr_t g_base = 0;

// helper functions for dumping bytes from functions to create
// signatures

static void DumpBytes(const char* name, const void* addr, size_t count)
{
    auto p = reinterpret_cast<const uint8_t*>(addr);

    char line[512]{};
    size_t pos = 0;

    pos += snprintf(line + pos, sizeof(line) - pos, "%s at %p:", name, addr);

    for (size_t i = 0; i < count && pos + 4 < sizeof(line); ++i) {
        pos += snprintf(line + pos, sizeof(line) - pos, " %02X", p[i]);
    }

    _LOG("%s", line);
}

// roll structure and helpers
enum class RollMode : uint8_t {
    Normal,
    Advantage,
    Disadvantage,
    Unknown
};

static RollMode GetModeFromFlags(uint8_t flags10, uint8_t outAdv, uint8_t outDis)
{
    if (flags10 & 0x10) return RollMode::Advantage;
    if (flags10 & 0x20) return RollMode::Disadvantage;
    if (outAdv) return RollMode::Advantage;
    if (outDis) return RollMode::Disadvantage;
    return RollMode::Normal;
}

static RollMode GetModeFromRollState(const uint8_t* p)
{
    if (!p) return RollMode::Unknown;

    uint8_t advType = *(p + 0x40);
    switch (advType) {
    case 1: return RollMode::Advantage;
    case 2: return RollMode::Disadvantage;
    default: return RollMode::Normal;
    }
}

static const char* RollModeName(RollMode m)
{
    switch (m) {
    case RollMode::Normal:       return "normal";
    case RollMode::Advantage:    return "advantage";
    case RollMode::Disadvantage: return "disadvantage";
    default:                     return "unknown";
    }
}

#pragma pack(push, 1)

struct SmallRollData
{
    uint32_t total;          // +0x00  likely final total
    uint32_t keptDie;        // +0x04  likely main/kept die
    uint32_t otherDie;       // +0x08  other roll-related value / second die slot
    uint8_t  state;          // +0x0C  phase/state byte
    uint8_t  pad0D[3];
    uint64_t packedDiceInfo; // +0x10  low32 looks like die type (20 => d20)
    uint64_t extraPtr;       // +0x18
    uint32_t extraCap;       // +0x20
    uint32_t extraCount;     // +0x24
};

struct DialogueRollOutput
{
    uint8_t  hdr00;              // +0x00 unknown
    uint8_t  hdr01;              // +0x01 unknown
    uint8_t  hdr02;              // +0x02 unknown
    uint8_t  hdr03;              // +0x03 unknown
    uint32_t hdr04;              // +0x04 unknown
    uint32_t hdr08;              // +0x08 unknown
    uint8_t  uiFlags;            // +0x0C usually 8 in your logs
    uint8_t  isAdvantageFlag;    // +0x0D written from param10 bit 0x10
    uint8_t  isDisadvantageFlag; // +0x0E written from param10 bit 0x20
    uint8_t  hdr0F;              // +0x0F unknown
    uint8_t  unk10_1F[0x10];     // +0x10..+0x1F filled by FUN_140e732b0
    SmallRollData roll;          // +0x20 copied by FUN_140e72c30
};

#pragma pack(pop)

static_assert(sizeof(SmallRollData) == 0x28, "SmallRollData size mismatch");
static_assert(offsetof(DialogueRollOutput, roll) == 0x20, "DialogueRollOutput.roll offset mismatch");

struct RollSummary
{
    RollMode mode;
    uint32_t dcOrTarget;
    uint32_t total;
    uint32_t keptDie;
    uint32_t otherDie;
    uint8_t  state;
    uint32_t dieType;
};

static RollSummary g_lastSummary{ RollMode::Unknown, 0, 0, 0, 0, 0, 0 };

static bool SameSummary(const RollSummary& a, const RollSummary& b)
{
    return a.mode      == b.mode &&
           a.dcOrTarget == b.dcOrTarget &&
           a.total     == b.total &&
           a.keptDie   == b.keptDie &&
           a.otherDie  == b.otherDie &&
           a.state     == b.state &&
           a.dieType   == b.dieType;
}

enum class RollCopyStage : uint8_t {
    Unknown = 0,
    UpstreamBuild,
    DownstreamPropagate
};

static const char* RollCopyStageName(RollCopyStage s)
{
    switch (s) {
    case RollCopyStage::UpstreamBuild:      return "upstream-build";
    case RollCopyStage::DownstreamPropagate:return "downstream-propagate";
    default:                                return "unknown";
    }
}

#pragma pack(push, 1)
struct RollCopyData
{
    uint32_t total;          // +0x00 visible/result total
    uint32_t keptDie;        // +0x04 primary/kept die
    uint32_t otherDie;       // +0x08 secondary/discarded/aux value
    uint8_t  state;          // +0x0C
    uint8_t  pad0D[3];
    uint64_t packedDiceInfo; // +0x10 low32 == 20 for d20
    uint64_t extraPtr;       // +0x18
    uint32_t extraCap;       // +0x20
    uint32_t extraCount;     // +0x24
};
#pragma pack(pop)

static_assert(sizeof(RollCopyData) == 0x28, "RollCopyData size mismatch");

struct RollCopySummary
{
    RollCopyStage stage;
    uint32_t total;
    uint32_t keptDie;
    uint32_t otherDie;
    uint8_t  state;
    uint32_t dieType;
    uint32_t packedHi;
};

static RollCopySummary g_lastRollCopy{};

static uint32_t ReadU32(const void* p, size_t off)
{
    return *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(p) + off);
}

static uint64_t ReadU64(const void* p, size_t off)
{
    return *reinterpret_cast<const uint64_t*>(
        reinterpret_cast<const uint8_t*>(p) + off);
}

static bool SameRollCopy(const RollCopySummary& a, const RollCopySummary& b)
{
    return a.stage   == b.stage &&
           a.total   == b.total &&
           a.keptDie == b.keptDie &&
           a.otherDie== b.otherDie &&
           a.state   == b.state &&
           a.dieType == b.dieType &&
           a.packedHi== b.packedHi;
}

static RollCopyStage ClassifyRollCopyCaller(void* caller)
{
    uintptr_t c = reinterpret_cast<uintptr_t>(caller);

    // Current-build friendly labels from your logs
    if (c == g_base + 0x1185950) return RollCopyStage::UpstreamBuild;       // 0x...FAC5950
    if (c == g_base + 0x0F153A8) return RollCopyStage::DownstreamPropagate; // 0x...F8553A8

    return RollCopyStage::Unknown;
}

static bool TrySummarizeRollCopy(const void* obj, void* caller, RollCopySummary& out)
{
    if (!obj) return false;

    const auto* roll = reinterpret_cast<const RollCopyData*>(obj);

    uint32_t dieType = static_cast<uint32_t>(roll->packedDiceInfo & 0xFFFFFFFFull);
    uint32_t packedHi = static_cast<uint32_t>(roll->packedDiceInfo >> 32);

    if (dieType != 20) return false; // focus on d20 only

    out = RollCopySummary{
        ClassifyRollCopyCaller(caller),
        roll->total,
        roll->keptDie,
        roll->otherDie,
        roll->state,
        dieType,
        packedHi
    };

    if (SameRollCopy(out, g_lastRollCopy)) return false;
    g_lastRollCopy = out;
    return true;
}

static void PrintRollCopySummary(const RollCopySummary& s, void* caller)
{
    _LOG("[BG3] roll-copy: stage=%s caller=%p total=%u kept=%u other=%u state=%u dieType=d%u packedHi=0x%08X",
        RollCopyStageName(s.stage),
        caller,
        s.total,
        s.keptDie,
        s.otherDie,
        (unsigned)s.state,
        s.dieType,
        s.packedHi);
}

// Physical (smart) dice structures and functions

struct PendingPhysicalRoll
{
    bool valid = false;
    RollMode mode = RollMode::Unknown;
    uint32_t dc = 0;

    // For normal: use die1 only.
    // For advantage/disadvantage: use die1 and die2.
    uint32_t die1 = 0;
    uint32_t die2 = 0;
};

struct PhysicalDiceReply
{
    bool ready = false;
    uint32_t die1 = 0;
    uint32_t die2 = 0;
};

struct PendingDialogueRoll
{
    bool active = false;
    int64_t rollState = 0;
    RollMode mode = RollMode::Unknown;
    uint32_t dc = 0;
    int32_t modifier = 0;
    uint32_t generation = 0;
};

static std::mutex g_rollMutex;
static PendingPhysicalRoll g_pendingRoll{};

static bool IsD20(const RollCopyData* r)
{
    return r && static_cast<uint32_t>(r->packedDiceInfo & 0xFFFFFFFFull) == 20;
}

static uint32_t ClampD20(uint32_t x)
{
    if (x < 1) return 1;
    if (x > 20) return 20;
    return x;
}

static void ApplyPhysicalRollOverride(RollCopyData* r, const PendingPhysicalRoll& p)
{
    if (!r || !p.valid) return;

    uint32_t kept = 0;
    uint32_t other = r->otherDie;

    uint32_t d1 = ClampD20(p.die1);
    uint32_t d2 = ClampD20(p.die2);

    switch (p.mode) {
    case RollMode::Advantage:
        kept = (d1 >= d2) ? d1 : d2;
        other = (d1 >= d2) ? d2 : d1;
        break;

    case RollMode::Disadvantage:
        kept = (d1 <= d2) ? d1 : d2;
        other = (d1 <= d2) ? d2 : d1;
        break;

    case RollMode::Normal:
        kept = d1;
        // keep existing otherDie unless you learn a better meaning for it
        break;

    default:
        return;
    }

    // Preserve whatever bonus/modifier pipeline the game already computed.
    int32_t bonus = static_cast<int32_t>(r->total) - static_cast<int32_t>(r->keptDie);

    r->keptDie = kept;
    if (p.mode != RollMode::Normal) {
        r->otherDie = other;
    }
    r->total = static_cast<uint32_t>(static_cast<int32_t>(kept) + bonus);
}


// Game functions

/*
void FUN_1410acc10(longlong *param_1,undefined8 param_2,undefined8 param_3,undefined4 param_4)

{
  undefined4 uVar1;
  longlong local_18;
  undefined8 local_10;
  
  FUN_144d58130(*(undefined8 *)(param_1[1] + 0x30));
  local_18 = *param_1;
  local_10 = *(undefined8 *)(local_18 + 8);
  FUN_140f05e30(&local_18,param_2);
  uVar1 = FUN_144d57970(*(undefined8 *)(param_1[1] + 0x30),param_4);
  FUN_144d58700(*(undefined8 *)(param_1[1] + 0x30),"NaturalRoll",0xb);
  FUN_144d58130(*(undefined8 *)(param_1[1] + 0x30));
  FUN_144d58b90(*(undefined8 *)(param_1[1] + 0x30),0xfffffffe,1);
  FUN_144d58e30(*(undefined8 *)(param_1[1] + 0x30),uVar1);
  return;
}
*/

using _ecl__GameStateMachine__Update = void(__fastcall*)(void *self, void *tick);
using _RollCopy = void* (__fastcall*)(void* dst, const void* src);
using _FUN_1411855f0 = void(__fastcall*)(
    uintptr_t a1,  uintptr_t a2,  uintptr_t a3,  uintptr_t a4,
    uintptr_t a5,  uintptr_t a6,  uintptr_t a7,  uintptr_t a8,
    uintptr_t a9,  uintptr_t a10, uintptr_t a11, uintptr_t a12,
    uintptr_t a13, uintptr_t a14, uintptr_t a15
);

using _ResolveDialogueRoll = void(__fastcall*)( //FUN_14328e080
    int64_t rollContext,        // param_1
    int64_t* ecsOrClientCtx,    // param_2
    int64_t rollState,          // param_3
    char isReplayOrUiFlag       // param_4
);

    //"48 8b 3d ? ? ? ? 48 8b d3 48 8b 8f 98 00 00 00"

    //"48 89 5c 24 18 55 56 57 41 54 41 55 41 56 41 57 "
    //"48 8d ac 24 80 fd ff ff 48 81 ec 80 03 00 00 48 "
    //"8b 05 5a aa 6b 04 48 33 c4"

RVA<_ecl__GameStateMachine__Update>
ecl__GameStateMachine__Update (
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D AC 24 80 FD FF FF "
    "48 81 EC 80 03 00 00 "
    "48 8B 05 ? ? ? ? "
    "48 33 C4 "
);
_ecl__GameStateMachine__Update ecl__GameStateMachine__Update_Original = nullptr;


RVA<_FUN_1411855f0>
FUN_1411855f0(
    "4C 89 4C 24 20 4C 89 44 24 18 55 53 56 57 "
    "41 54 41 55 41 56 41 57 48 8D AC 24 E8 FE FF FF"
);
_FUN_1411855f0 FUN_1411855f0_Original = nullptr;

RVA<_RollCopy>
RollCopy (
        "48 89 6C 24 18 56 57 41 56 48 83 EC 20 8B 02"
);
_RollCopy RollCopy_Original = nullptr;


RVA<_ResolveDialogueRoll>
ResolveDialogueRoll (
    "48 89 5c 24 20 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8d ac 24 a0 f5 ff ff 48 81 ec 60 0b 00 00 0f "
    "29 b4 24"
);
_ResolveDialogueRoll ResolveDialogueRoll_Original = nullptr;

static const char* GetClientStateName(uint32_t s)
{
    switch (s) {
    case 0: return "Unknown";
    case 1: return "Init";
    case 2: return "InitMenu";
    case 3: return "InitNetwork";
    case 4: return "InitConnection";
    case 5: return "Idle";
    case 6: return "LoadMenu";
    case 7: return "Menu";
    case 8: return "Exit";
    case 9: return "SwapLevel";
    case 10: return "LoadLevel";
    case 11: return "LoadModule";
    case 12: return "LoadSession";
    case 13: return "UnloadLevel";
    case 14: return "UnloadModule";
    case 15: return "UnloadSession";
    case 16: return "Paused";
    case 17: return "PrepareRunning";
    case 18: return "Running";
    case 19: return "Disconnect";
    case 20: return "Join";
    case 21: return "Save";
    case 22: return "StartLoading";
    case 23: return "StopLoading";
    case 24: return "StartServer";
    case 25: return "Movie";
    case 26: return "Installation";
    case 27: return "ModReceiving";
    case 28: return "Lobby";
    case 29: return "BuildStory";
    case 32: return "GeneratePsoCache";
    case 33: return "LoadPsoCache";
    case 34: return "AnalyticsSessionEnd";
    default: return "ERROR";
    }
}

namespace SmartDiceRolls {


    // Read and populate offsets and addresses from game code
    bool PopulateOffsets() {

        _LOG("ecl__GameStateMachine__Update at %p",
            ecl__GameStateMachine__Update.GetUIntPtr()
        );


        _LOG("RollCopy at %p",
            RollCopy.GetUIntPtr()
        );

        _LOG("ResolveDialogueRoll at %p",
            ResolveDialogueRoll.GetUIntPtr()
        );

        _LOG("FUN_1411855f0 at %p",
            FUN_1411855f0.GetUIntPtr()
        );

        if (
            !ecl__GameStateMachine__Update ||
            !RollCopy ||
            !FUN_1411855f0 ||
            !ResolveDialogueRoll
        ) return false;

        if (!g_bg3BaseAddr) {
            _LOGD("Baldur's Gate3 base address is not set!");
            return false;
        }

        return true;
    }


static bool IsValidClientState(uint32_t s)
{
    switch (s) {
    case 0:  case 1:  case 2:  case 3:  case 4:
    case 5:  case 6:  case 7:  case 8:  case 9:
    case 10: case 11: case 12: case 13: case 14:
    case 15: case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23: case 24:
    case 25: case 26: case 27: case 28: case 29:
    case 32: case 33: case 34:
        return true;
    default:
        return false;
    }
}

void ecl__GameStateMachine__Update_Hook(void* self, void* tick)
{
    auto base = reinterpret_cast<uintptr_t>(self);

    auto objBefore  = *reinterpret_cast<uintptr_t*>(base + 0x00);
    auto liveBefore = objBefore ? *reinterpret_cast<uint32_t*>(objBefore + 0x10) : 0xFFFFFFFFu;

    ecl__GameStateMachine__Update_Original(self, tick);

    auto objAfter  = *reinterpret_cast<uintptr_t*>(base + 0x00);
    auto liveAfter = objAfter ? *reinterpret_cast<uint32_t*>(objAfter + 0x10) : 0xFFFFFFFFu;

    if (objBefore && objAfter &&
        IsValidClientState(liveBefore) &&
        IsValidClientState(liveAfter) &&
        liveBefore != liveAfter)
    {
        _LOG("[BG3] CLIENT STATE SWAP - from: %s (%u), to: %s (%u)",
            GetClientStateName(liveBefore), liveBefore,
            GetClientStateName(liveAfter),  liveAfter);
    }
}

using _RollCopy = void* (__fastcall*)(void* dst, const void* src);
_RollCopy RollCopy_Original = nullptr;

static bool IsDownstreamCaller(void* caller)
{
    return reinterpret_cast<uintptr_t>(caller) == g_base + 0x0F153A8; // current-build F8553A8
}

void* __fastcall RollCopy_Hook(void* dst, const void* src)
{
    void* realCaller = _ReturnAddress();

    void* ret = RollCopy_Original(dst, src);

    const void* obj = ret ? ret : dst;

    RollCopySummary s{};
    if (TrySummarizeRollCopy(obj, realCaller, s)) {
        PrintRollCopySummary(s, realCaller);
    }

    return ret;
}

void ResolveDialogueRoll_Hook ( 
    int64_t rollContext, int64_t* ecsOrClientCtx,
    int64_t rollState, char isReplayOrUiFlag) {

    _LOG("ResolveDialogueRoll Hook!");

    // observed / likely
    // +0x40  adv/disadv selector enum
    // +0x41  difficultyClass
    // +0x42  finalKeptDieCompact
    // +0x43  finalOtherDieCompact
    // +0x44  finalModifierCompact
    // +0x4C  finalSuccess
    // +0x54  roll mode/type
    // +0x88  uiReplayFlag
    // +0xAC  modifier
    // +0xB5  advFlagResolved
    // +0xB6  disFlagResolved
    // +0xC8  finalTotal
    // +0xCC  keptNaturalRoll
    // +0xD0  otherNaturalRoll

    ResolveDialogueRoll_Original (
        rollContext, ecsOrClientCtx,
        rollState, isReplayOrUiFlag
    );

    // quick and dirty static patching
    auto p = reinterpret_cast<uint8_t*>(rollState);

    int dc       = *(uint8_t*)(p + 0x41);
    int modifier = *reinterpret_cast<int*>(p + 0xAC);

    int keptDie  = 1;
    int otherDie = 4;
    int total    = keptDie + modifier;
    bool success = total >= dc;
    
    uint8_t rawMode = *(p + 0x40);
    RollMode mode = GetModeFromRollState(p);

    *reinterpret_cast<int*>(p + 0xCC) = keptDie;
    *reinterpret_cast<int*>(p + 0xD0) = otherDie;
    *reinterpret_cast<int*>(p + 0xC8) = total;

    *(p + 0x42) = static_cast<uint8_t>(keptDie);
    *(p + 0x43) = static_cast<uint8_t>(otherDie);
    *(p + 0x44) = static_cast<uint8_t>(modifier);
    *(p + 0x4C) = success ? 1 : 0;

    _LOG (
        "[Dialogue Roll] rawMode: %u, mode:%s, dc: %d, modifier: %d, kept die: %d, other die: %d, "
        "total: %d, result: %s",
        static_cast<unsigned>(rawMode),
        RollModeName(mode),
        dc, modifier, keptDie, otherDie, total, success ? "success" : "failure"
    );
}


#if 0
void* __fastcall RollCopy_Hook(void* dst, const void* src)
{
    void* caller = _ReturnAddress();
    void* ret = RollCopy_Original(dst, src);

    auto* roll = reinterpret_cast<RollCopyData*>(ret ? ret : dst);
    if (!roll || !IsD20(roll) || !IsDownstreamCaller(caller)) {
        return ret;
    }

    PendingPhysicalRoll pending;
    {
        std::lock_guard<std::mutex> lock(g_rollMutex);
        pending = g_pendingRoll;
    }

    pending.die1 = 8;
    pending.die2 = 16;
    pending.valid = true;

    if (pending.valid) {
        uint32_t beforeTotal = roll->total;
        uint32_t beforeKept  = roll->keptDie;
        uint32_t beforeOther = roll->otherDie;

        ApplyPhysicalRollOverride(roll, pending);

        _LOG("[BG3] applied physical roll: mode=%u before(total=%u kept=%u other=%u) after(total=%u kept=%u other=%u)",
            static_cast<unsigned>(pending.mode),
            beforeTotal, beforeKept, beforeOther,
            roll->total, roll->keptDie, roll->otherDie);

        // consume once
        std::lock_guard<std::mutex> lock(g_rollMutex);
        g_pendingRoll.valid = false;
    }

    const void* obj = ret ? ret : dst;

    RollCopySummary s{};
    if (TrySummarizeRollCopy(obj, caller, s)) {
        PrintRollCopySummary(s, caller);
    }

    return ret;
}
#endif
#if 0
void __fastcall FUN_1411855f0_Hook(
    uintptr_t a1,  uintptr_t a2,  uintptr_t a3,  uintptr_t a4,
    uintptr_t a5,  uintptr_t a6,  uintptr_t a7,  uintptr_t a8,
    uintptr_t a9,  uintptr_t a10, uintptr_t a11, uintptr_t a12,
    uintptr_t a13, uintptr_t a14, uintptr_t a15)
{
    auto param10 = reinterpret_cast<const uint8_t*>(a10);
    auto out     = reinterpret_cast<DialogueRollOutput*>(a15);

    uint8_t flags10 = param10 ? *param10 : 0;
    uint32_t dcOrTarget = a6 ? *reinterpret_cast<uint32_t*>(a6 + 0xD4) : 0;

    FUN_1411855f0_Original(
        a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15
    );

    if (!out)
        return;

    uint32_t dieType = static_cast<uint32_t>(out->roll.packedDiceInfo & 0xFFFFFFFFull);
    if (dieType != 20) // only d20 for now
        return;

    // after original, once you have output flags and dc
    RollMode mode = GetModeFromFlags(flags10, out->isAdvantageFlag, out->isDisadvantageFlag);
    uint32_t difficultyClass = *reinterpret_cast<uint32_t*>(a6 + 0xD4);

    // send to tray app here
    // e.g. localhost UDP / named pipe / HTTP:
    // { "type":"roll_prompt", "mode":"advantage", "dc":10 }

    {
        std::lock_guard<std::mutex> lock(g_rollMutex);
        g_pendingRoll.valid = false; // wait for tray app to fill it
        g_pendingRoll.mode = mode;
        g_pendingRoll.dc = difficultyClass;
    }

    RollSummary s{
        mode,
        dcOrTarget,
        out->roll.total,
        out->roll.keptDie,
        out->roll.otherDie,
        out->roll.state,
        dieType
    };

    if (SameSummary(s, g_lastSummary))
        return;

    g_lastSummary = s;

    _LOG("[BG3] dialogue roll: mode=%s flags=0x%02X outAdv=%u outDis=%u dc=%u total=%u kept=%u other=%u state=%u dieType=d%u",
        RollModeName(mode),
        flags10,
        (unsigned)out->isAdvantageFlag,
        (unsigned)out->isDisadvantageFlag,
        s.dcOrTarget,
        s.total,
        s.keptDie,
        s.otherDie,
        (unsigned)s.state,
        s.dieType);
}
#endif
void __fastcall FUN_1411855f0_Hook(
    uintptr_t a1,  uintptr_t a2,  uintptr_t a3,  uintptr_t a4,
    uintptr_t a5,  uintptr_t a6,  uintptr_t a7,  uintptr_t a8,
    uintptr_t a9,  uintptr_t a10, uintptr_t a11, uintptr_t a12,
    uintptr_t a13, uintptr_t a14, uintptr_t a15)
{
    auto param10 = reinterpret_cast<const uint8_t*>(a10);
    uint8_t flags10 = param10 ? *param10 : 0;

    if (flags10 & 0x10) {
        _LOG("[Advantage roll]");
    } else if (flags10 & 0x20) {
        _LOG("[Disadvantage roll]");
    } else {
        _LOG("[Normal roll]");
    }

    FUN_1411855f0_Original(
        a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15
    );
}

#if 0
    void ecl__GameStateMachine__Update_Hook (void *self, void *tick) {
        _LOG("ecl::GameStateMachine::Update hook!");

        auto sm = reinterpret_cast<uint8_t*>(self);
        uint32_t before = *reinterpret_cast<uint32_t*>(sm + 0x08);
        
        ecl__GameStateMachine__Update_Original(self, tick);

        uint32_t after = *reinterpret_cast<uint32_t*>(sm + 0x08);

        if (before != after) {
            _LOG("[BG3] CLIENT STATE SWAP - from: %s (%u), to: %s (%u)\n",
                GetClientStateName(before), before,
                GetClientStateName(after),  after
            );
        }
        uint32_t pendingCount = *reinterpret_cast<uint32_t*>(sm + 0x1C);
        printf("[BG3] gsm=%p state=%u pending=%u\n", self, after, pendingCount);
    }
#endif

    bool InitAddresses() {
        _LOG("Sigscan start");
        RVAUtils::Timer tmr; tmr.start();
        RVAManager::UpdateAddresses(0);
        _LOG("Sigscan elapsed: %llu ms.", tmr.stop());

        // Check if all addresses were resolved
        for (auto rvaData : RVAManager::GetAllRVAs()) {
            if (!rvaData->effectiveAddress) {
                _LOG("Signature: %s was not resolved!", rvaData->sig);
            }
        }
        if (!RVAManager::IsAllResolved())
            return false;

        return true;
    }

    bool ApplyHooks() {
        _LOG("Applying hooks...");
        // Hook loadout type registration to obtain pointer to the model handle
        MH_Initialize();

        MH_CreateHook (
            ecl__GameStateMachine__Update,
            ecl__GameStateMachine__Update_Hook,
            reinterpret_cast<LPVOID *>(&ecl__GameStateMachine__Update_Original)
        );
        if (MH_EnableHook(ecl__GameStateMachine__Update) != MH_OK) {
            _LOG("FATAL: Failed to install ecl__GameStateMachine__Update hook.");
            return false;
        }

        MH_CreateHook (
            RollCopy,
            RollCopy_Hook,
            reinterpret_cast<LPVOID *>(&RollCopy_Original)
        );
        if (MH_EnableHook(RollCopy) != MH_OK) {
            _LOG("FATAL: Failed to install RollCopy hook.");
            return false;
        }

        MH_CreateHook (
            ResolveDialogueRoll,
            ResolveDialogueRoll_Hook,
            reinterpret_cast<LPVOID *>(&ResolveDialogueRoll_Original)
        );
        if (MH_EnableHook(ResolveDialogueRoll) != MH_OK) {
            _LOG("FATAL: Failed to install ResolveDialogueRoll hook.");
            return false;
        }

        MH_CreateHook (
            FUN_1411855f0,
            FUN_1411855f0_Hook,
            reinterpret_cast<LPVOID *>(&FUN_1411855f0_Original)
        );
        if (MH_EnableHook(FUN_1411855f0) != MH_OK) {
            _LOG("FATAL: Failed to install FUN_1411855f0 hook.");
            return false;
        }

        _LOG("Hooks applied successfully!");

        return true;
    }

    void Init() {
        g_logger.Open("./mods/smart-dice-rolls.log");
        _LOG(
            "Baldur's Gate 3 Smart Dice Rolls Mod v1.0 by Thanos Petsas (SkyExplosionist)");
        g_bg3BaseAddr = GetModuleHandle(NULL);
        g_base = reinterpret_cast<uintptr_t>(g_bg3BaseAddr);
        _LOG("Module base: %p (g_base=%p)", g_bg3BaseAddr, (void*)g_base);
        _LOG("Module base: %p", g_bg3BaseAddr);

        // Sigscan
        if (!InitAddresses() || !PopulateOffsets()) {
            MessageBoxA (
                NULL,
                "Smart Dice Rolls Mod is not compatible with this version of"
                " Baldur's Gate 3.\nPlease visit the mod page for updates.",
                "Smart Dice Rolls Mod",
                MB_OK | MB_ICONEXCLAMATION
            );
            _LOG("FATAL: Incompatible version");
            return;
        }

        _LOG("Addresses set");

        // init config
        g_config = Config(INI_LOCATION);
        g_config.print();
        if (!ApplyHooks()) {
            MessageBoxA (
                NULL,
                "Smart Dice Rolls Mod is not compatible with this version of"
                " Baldur's Gate 3.\nPlease visit the mod page for updates.",
                "Smart Dice Rolls Mod",
                MB_OK | MB_ICONEXCLAMATION
            );
            _LOG("FATAL: Incompatible version");
            return;
        }

        //uintptr_t fac5950 = g_base + 0x1185950;
        //DumpBytes("FAC5950", reinterpret_cast<void*>(fac5950), 32);

        _LOG("Ready.");
    }

}
