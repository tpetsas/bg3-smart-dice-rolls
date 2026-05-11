/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#include "SmartDiceRolls.h"
#include "SmartDiceResult.h"
#include "PixelsDiceClient.h"

#include "Logger.h"
#include "Config.h"
#include "Utils.h"
#include "rva/RVA.h"
#include "minhook/include/MinHook.h"

// for getting the caller
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <windows.h>

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

// clamp the value into the valid d20 range
static uint32_t ClampD20(uint32_t x)
{
    if (x < 1) return 1;
    if (x > 20) return 20;
    return x;
}

struct DialogueRollSession
{
    bool active = false;               // we are inside a dialogue-roll lifecycle
    bool waitingForResolution = false; // waiting for the followup uiFlag==0 call
    bool rerollPending = false;        // player clicked Roll Again (this triggers our Reroll_Hook)
    uint32_t generation = 0;           // increments per attempt

    RollMode mode = RollMode::Unknown;
    uint32_t dc = 0;
    int32_t modifier = 0;

};


std::mutex g_dialogueRollMutex;
static DialogueRollSession g_dialogueRoll{}; // all initialized to zero
SmartDiceResult g_smartDiceResult{};

static void ResetDialogueRollSession()
{
    g_dialogueRoll.active = false;
    g_dialogueRoll.waitingForResolution = false;
    g_dialogueRoll.rerollPending = false;
    // Keep mode, dc, modifier — Reroll_Hook needs them if the player
    // clicks "Roll Again" after a failed check. They are overwritten
    // when the next isDialoguePrompt fires.
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

    void Reroll_Hook(long long a1)
    {
        {
            std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
            // populate dialogue roll session data
            g_dialogueRoll.rerollPending = true;
            g_dialogueRoll.active = true;
            g_dialogueRoll.waitingForResolution = true;
            g_dialogueRoll.generation++;

            // populate smart dice context for the tray app
            g_smartDiceResult.ready = false;
            g_smartDiceResult.generation = g_dialogueRoll.generation;
            g_smartDiceResult.die1 = 0;
            g_smartDiceResult.die2 = 0;

            // Notify tray app â€” reuse the mode from the current session
            PixelsDiceClient::notifyTrayForRoll(RollModeName(g_dialogueRoll.mode), g_dialogueRoll.generation);
        }

        _LOG("Reroll hook");
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

    // Log call stack when the dice roll is triggered (flag=0)
    if (isReplayOrUiFlag == 0) {
        void* stack[32];
        USHORT frames = CaptureStackBackTrace(0, 32, stack, nullptr);
        _LOG("[CallStack] ResolveDialogueRoll flag=0, %u frames (base=%p):", frames, (void*)g_base);
        for (USHORT i = 0; i < frames; i++) {
            uintptr_t addr = reinterpret_cast<uintptr_t>(stack[i]);
            uintptr_t rva = addr - g_base;
            _LOG("[CallStack]   [%u] addr=%p  RVA=+0x%llX", i, stack[i], (unsigned long long)rva);
        }
    }

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
        // populate dialogue roll session data
        g_dialogueRoll.active = true;
        g_dialogueRoll.waitingForResolution = true;
        g_dialogueRoll.rerollPending = false;
        g_dialogueRoll.generation++;
        g_dialogueRoll.mode = GetModeFromRawMode(rollState->rawMode);
        g_dialogueRoll.dc = rollState->difficultyClass;
        g_dialogueRoll.modifier = rollState->modifier;

        // populate smart dice context for the tray app
        g_smartDiceResult.ready = false; // waiting for tray app
        g_smartDiceResult.generation = g_dialogueRoll.generation;
        g_smartDiceResult.die1 = 0;
        g_smartDiceResult.die2 = 0;

        // Notify tray app to start collecting rolls
        PixelsDiceClient::notifyTrayForRoll(RollModeName(g_dialogueRoll.mode), g_dialogueRoll.generation);
        _LOG("Notify tray app for rolling! mode=%s gen=%u",
            RollModeName(g_dialogueRoll.mode), g_dialogueRoll.generation);
    }
    _LOG("* isDialoguePrompt: %d, isDialogueFollowup: %d", isDialoguePrompt, isDialogueFollowup);

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

    SmartDiceResult dice{};
    DialogueRollSession session{};


    // perhaps this should be done during patching?
    //if (isDialogueFollowup) {
    //    ResetDialogueRollSession();
    //}

    // This is a dialogue follow up

    // patch only if you have pending physical dice for the current dialogue session
    {
        std::lock_guard<std::mutex> lock(g_dialogueRollMutex);

        // if it is a real dialogue followup and the smart dice result is ready
        // for the same generation, patch rollState
        session = g_dialogueRoll;
        dice = g_smartDiceResult;

        if (!session.active || !session.waitingForResolution)
            return;

        if (!dice.ready || dice.generation != session.generation)
            return;

        // TODO: we can make this a separate function
        uint32_t d1 = ClampD20(dice.die1);
        uint32_t d2 = ClampD20(dice.die2);

        uint32_t keptDie = d1;
        uint32_t otherDie = d2;

        switch (session.mode) {
        case RollMode::Advantage:
            keptDie = (d1 >= d2) ? d1 : d2;
            otherDie = (d1 >= d2) ? d2 : d1;
            break;

        case RollMode::Disadvantage:
            keptDie = (d1 <= d2) ? d1 : d2;
            otherDie = (d1 <= d2) ? d2 : d1;
            break;

        case RollMode::Normal:
        default:
            keptDie = d1;
            otherDie = d2;
            break;
        }

        // TODO: make the patch a separate function
        // The original call already ran and wrote finalTotal = virtualDie + modifier + bonuses
        // (Guidance, Bless, Bardic Inspiration, etc. selected by the player after the prompt).
        // Derive the non-die component from that so we don't lose anything the player picked.
        const int modifierAndBonuses = rollState->finalTotal - rollState->keptNaturalRoll;
        const int total = static_cast<int>(keptDie) + modifierAndBonuses;
        const bool success = total >= static_cast<int>(session.dc);

        rollState->keptNaturalRoll = keptDie;
        rollState->otherNaturalRoll = otherDie;
        rollState->finalTotal = total;

        rollState->finalKeptDie = static_cast<uint8_t>(keptDie);
        rollState->finalOtherDie = static_cast<uint8_t>(otherDie);
        // finalModifier already set correctly by the original call — leave it
        rollState->finalSuccess = success ? 1 : 0;


    _LOG("[Patched roll] ctx=%p state=%p uiFlag=%d rawMode=%u dc=%u mod=%d",
        (void*)rollContext,
        (void*)rollStatePtr,
        (int)isReplayOrUiFlag,
        (unsigned)rollState->rawMode,
        (unsigned)rollState->difficultyClass,
        (int)rollState->modifier);

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

        // Start the pipe client thread to communicate with PixelsTray
        PixelsDiceClient::start();

        _LOG("Ready.");
    }

}
