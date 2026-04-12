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

// roll structure and helpers
enum class RollMode : uint8_t {
    Normal,
    Advantage,
    Disadvantage,
    Unknown
};

static const char* RollModeName(RollMode m)
{
    switch (m) {
    case RollMode::Normal:       return "normal";
    case RollMode::Advantage:    return "advantage";
    case RollMode::Disadvantage: return "disadvantage";
    default:                     return "unknown";
    }
}

struct DialogueRollSession
{
    bool active = false;          // saw uiFlag=1
    bool waitingForResolution = false;
    bool rerollPending = false;   // saw Reroll_Hook
    uint32_t generation = 0;

    RollMode mode = RollMode::Unknown;
    uint32_t dc = 0;
    int32_t modifier = 0;
    int64_t lastPromptState = 0;
};


static std::mutex g_dialogueRollMutex;
static DialogueRollSession g_dialogueRoll{}; // all initialized to zero

static void ResetDialogueRollSession()
{
    g_dialogueRoll.active = false;
    g_dialogueRoll.waitingForResolution = false;
    g_dialogueRoll.rerollPending = false;
    g_dialogueRoll.mode = RollMode::Unknown;
    g_dialogueRoll.dc = 0;
    g_dialogueRoll.modifier = 0;
    g_dialogueRoll.lastPromptState = 0;
}


#pragma pack(push, 1)
struct DialogueRollState
{
    uint8_t  pad00[0x40];

    uint8_t  rawMode;             // +0x40  0=normal, 1=advantage, 2=disadvantage
    uint8_t  difficultyClass;     // +0x41
    uint8_t  finalKeptDie;        // +0x42
    uint8_t  finalOtherDie;       // +0x43
    uint8_t  finalModifier;       // +0x44

    uint8_t  pad45[0x07];
    uint8_t  finalSuccess;        // +0x4C

    uint8_t  pad4D[0x5F];
    int32_t  modifier;            // +0xAC

    uint8_t  padB0[0x18];
    int32_t  finalTotal;          // +0xC8
    int32_t  keptNaturalRoll;     // +0xCC
    int32_t  otherNaturalRoll;    // +0xD0
};
#pragma pack(pop)

static_assert(offsetof(DialogueRollState, rawMode) == 0x40);
static_assert(offsetof(DialogueRollState, difficultyClass) == 0x41);
static_assert(offsetof(DialogueRollState, finalSuccess) == 0x4C);
static_assert(offsetof(DialogueRollState, modifier) == 0xAC);
static_assert(offsetof(DialogueRollState, finalTotal) == 0xC8);
static_assert(offsetof(DialogueRollState, keptNaturalRoll) == 0xCC);
static_assert(offsetof(DialogueRollState, otherNaturalRoll) == 0xD0);
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

using _ResolveDialogueRoll = void(__fastcall*)( //FUN_14328e080
    int64_t rollContext,        // param_1
    int64_t* ecsOrClientCtx,    // param_2
    int64_t rollState,          // param_3
    char isReplayOrUiFlag       // param_4
);

using _Reroll = void(__fastcall *)(long long a1);

    //"48 8b 3d ? ? ? ? 48 8b d3 48 8b 8f 98 00 00 00"

    //"48 89 5c 24 18 55 56 57 41 54 41 55 41 56 41 57 "
    //"48 8d ac 24 80 fd ff ff 48 81 ec 80 03 00 00 48 "
    //"8b 05 5a aa 6b 04 48 33 c4"

RVA<_ResolveDialogueRoll>
ResolveDialogueRoll (
    "48 89 5c 24 20 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8d ac 24 a0 f5 ff ff 48 81 ec 60 0b 00 00 0f "
    "29 b4 24"
);
_ResolveDialogueRoll ResolveDialogueRoll_Original = nullptr;


RVA<_Reroll>
Reroll (
    "40 53 48 83 ec 20 80 b9 28 09 00 00 00 48 8b d9 74 57"
);
_Reroll Reroll_Original = nullptr;

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

        _LOG("ResolveDialogueRoll at %p",
            ResolveDialogueRoll.GetUIntPtr()
        );

        _LOG("Reroll at %p",
            Reroll.GetUIntPtr()
        );

        if (
            !ResolveDialogueRoll ||
            !Reroll
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

void Reroll_Hook(long long a1)
{
    {
        std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
        g_dialogueRoll.rerollPending = true;
        g_dialogueRoll.active = true;
        g_dialogueRoll.waitingForResolution = true;
        g_dialogueRoll.lastPromptState = 0;
        g_dialogueRoll.generation++;
    }

    // clear previous pending physical dice here and notify tray app
    // for new roll

    _LOG("Reroll hook");
    _LOG("Notify tray app for rolling!");
    Reroll_Original(a1);
}

// TODO: move this to helpers
static RollMode GetModeFromRollState(const uint8_t* p)
{
    if (!p) return RollMode::Unknown;

    uint8_t advType = *(p + 0x40);

    switch (advType) {
    case 1:
        return RollMode::Advantage;
    case 2:
        return RollMode::Disadvantage;
    default:
        return RollMode::Normal;
    }
}

#if 0
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
#endif


static RollMode GetModeFromRawMode(uint8_t rawMode)
{
    switch (rawMode) {
    case 0: return RollMode::Normal;
    case 1: return RollMode::Advantage;
    case 2: return RollMode::Disadvantage;
    default: return RollMode::Unknown;
    }
}

void __fastcall ResolveDialogueRoll_Hook(
    int64_t rollContext,
    int64_t* ecsOrClientCtx,
    int64_t rollStatePtr,
    char isReplayOrUiFlag)
{


    ResolveDialogueRoll_Original(
        rollContext,
        ecsOrClientCtx,
        rollStatePtr,
        isReplayOrUiFlag
    );


    auto* rollState = reinterpret_cast<DialogueRollState*>(rollStatePtr);
    if (!rollState) return;



    const bool isDialoguePrompt = (isReplayOrUiFlag == 1);
    bool isDialogueFollowup = false;

    {
        std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
        isDialogueFollowup =
            (isReplayOrUiFlag == 0) &&
            (g_dialogueRoll.active || g_dialogueRoll.rerollPending);
    }


    if (isDialoguePrompt) {
        std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
        g_dialogueRoll.active = true;
        g_dialogueRoll.waitingForResolution = true;
        g_dialogueRoll.rerollPending = false;
        g_dialogueRoll.generation++;
        g_dialogueRoll.mode = GetModeFromRawMode(rollState->rawMode);
        g_dialogueRoll.dc = rollState->difficultyClass;
        g_dialogueRoll.modifier = rollState->modifier;
        g_dialogueRoll.lastPromptState = rollStatePtr;

        // notify tray app here
        _LOG("Notify tray app for rolling!");
    }

    if (!isDialogueFollowup && !isDialoguePrompt) {

        _LOG("[Combat | Non-dialogue roll] ctx=%p state=%p uiFlag=%d rawMode=%u dc=%u mod=%d",
            (void*)rollContext,
            (void*)rollStatePtr,
            (int)isReplayOrUiFlag,
            (unsigned)rollState->rawMode,
            (unsigned)rollState->difficultyClass,
            (int)rollState->modifier);
            return; // probably combat/non-dialogue noise
    }

    if (isDialoguePrompt)
        return;

    _LOG("[Dialogue roll] ctx=%p state=%p uiFlag=%d rawMode=%u dc=%u mod=%d",
        (void*)rollContext,
        (void*)rollStatePtr,
        (int)isReplayOrUiFlag,
        (unsigned)rollState->rawMode,
        (unsigned)rollState->difficultyClass,
        (int)rollState->modifier);

    // patch only if you have pending physical dice for the current dialogue session
    {
        std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
        ResetDialogueRollSession();
    }




#if 0
_LOG("[ResolveRoll] ctx=%p state=%p uiFlag=%d rawMode=%u dc=%u mod=%d",
    (void*)rollContext,
    (void*)rollStatePtr,
    (int)isReplayOrUiFlag,
    (unsigned)rollState->rawMode,
    (unsigned)rollState->difficultyClass,
    (int)rollState->modifier);
    return;


    int dc = rollState->difficultyClass;
    int modifier = rollState->modifier;
    RollMode mode = GetModeFromRawMode(rollState->rawMode);

    //int keptDie = 1;
    //int otherDie = 4;
    //int total = keptDie + modifier;
    //bool success = total >= dc;

    rollState->keptNaturalRoll = keptDie;
    rollState->otherNaturalRoll = otherDie;
    rollState->finalTotal = total;

    rollState->finalKeptDie = static_cast<uint8_t>(keptDie);
    rollState->finalOtherDie = static_cast<uint8_t>(otherDie);
    rollState->finalModifier = static_cast<uint8_t>(modifier);
    rollState->finalSuccess = success ? 1 : 0;

    _LOG("[Dialogue Roll] rawMode:%u mode:%s dc:%d modifier:%d kept:%d other:%d total:%d result:%s",
        static_cast<unsigned>(rollState->rawMode),
        RollModeName(mode),
        dc, modifier, keptDie, otherDie, total,
        success ? "success" : "failure");
#endif
}

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
            ResolveDialogueRoll,
            ResolveDialogueRoll_Hook,
            reinterpret_cast<LPVOID *>(&ResolveDialogueRoll_Original)
        );
        if (MH_EnableHook(ResolveDialogueRoll) != MH_OK) {
            _LOG("FATAL: Failed to install ResolveDialogueRoll hook.");
            return false;
        }

        MH_CreateHook (
            Reroll,
            Reroll_Hook,
            reinterpret_cast<LPVOID *>(&Reroll_Original)
        );
        if (MH_EnableHook(Reroll) != MH_OK) {
            _LOG("FATAL: Failed to install Reroll hook.");
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

        _LOG("Ready.");
    }

}
