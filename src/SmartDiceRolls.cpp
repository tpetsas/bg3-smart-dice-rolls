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


#define INI_LOCATION "./mods/smart-dice-rolls-mod.ini"

// Globals
Config g_config;
Logger g_logger;
HMODULE g_bg3BaseAddr = nullptr;
static uintptr_t g_base = 0;

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

RVA<_RollCopy>
RollCopy (
        "48 89 6C 24 18 56 57 41 56 48 83 EC 20 8B 02"
);
_RollCopy RollCopy_Original = nullptr;

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

        if (
            !ecl__GameStateMachine__Update ||
            !RollCopy
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

// helpers here
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

static uint8_t ReadU8(const void* p, size_t off)
{
    return *reinterpret_cast<const uint8_t*>(
        reinterpret_cast<const uint8_t*>(p) + off);
}

struct RollSig {
    uint32_t a00;
    uint32_t a04;
    uint32_t a08;
    uint8_t  a0C;
    uint64_t a10;
};

static RollSig g_last{};

static bool SameSig(const RollSig& x, const RollSig& y)
{
    return x.a00 == y.a00 &&
           x.a04 == y.a04 &&
           x.a08 == y.a08 &&
           x.a0C == y.a0C &&
           x.a10 == y.a10;
}

static void LogInterestingRoll(const void* obj)
{
    if (!obj) return;

    RollSig s{
        ReadU32(obj, 0x00),
        ReadU32(obj, 0x04),
        ReadU32(obj, 0x08),
        ReadU8 (obj, 0x0C),
        ReadU64(obj, 0x10)
    };

    uint32_t low10 = (uint32_t)(s.a10 & 0xFFFFFFFFull);

    if (low10 != 20) return; // only d20
    if (SameSig(s, g_last)) return;
    g_last = s;

    if (s.a08 == 9 && s.a0C == 0) {
        _LOG("[BG3] roll prompt: target=%u needed=%u packed=%016llX",
            s.a00, s.a04, (unsigned long long)s.a10);
    } else if (s.a08 == 4 && s.a0C == 1) {
        _LOG("[BG3] roll resolved: total=%u keptDie=%u packed=%016llX",
            s.a00, s.a04, (unsigned long long)s.a10);
    } else {
        _LOG("[BG3] roll state: +00=%u +04=%u +08=%u +0C=%u +10=%016llX",
            s.a00, s.a04, s.a08, s.a0C, (unsigned long long)s.a10);
    }
}

void* __fastcall RollCopy_Hook(void* dst, const void* src)
{
    void* ret = RollCopy_Original(dst, src);

    // for a copy helper, inspect the destination after the copy
    LogInterestingRoll(ret ? ret : dst);

    return ret;
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
