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
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
#include <shellapi.h>

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

struct SmartDiceResult
{
    bool ready = false;          // tray app has provided a result
    uint32_t generation = 0;     // which dialogue-roll attempt this belongs to

    uint32_t die1 = 0;           // normal: use die1 only
    uint32_t die2 = 0;           // advantage/disadvantage: second die
};

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


static std::mutex g_dialogueRollMutex;
static DialogueRollSession g_dialogueRoll{}; // all initialized to zero
static SmartDiceResult g_smartDiceResult{};

static void ResetDialogueRollSession()
{
    g_dialogueRoll.active = false;
    g_dialogueRoll.waitingForResolution = false;
    g_dialogueRoll.rerollPending = false;
    g_dialogueRoll.mode = RollMode::Unknown;
    g_dialogueRoll.dc = 0;
    g_dialogueRoll.modifier = 0;
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

    // Forward declaration — defined further below with the pipe client code
    static void notifyTrayForRoll(const char* mode, uint32_t generation);

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

            // Notify tray app — reuse the mode from the current session
            notifyTrayForRoll(RollModeName(g_dialogueRoll.mode), g_dialogueRoll.generation);
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


    // ── Pipe client for PixelsTray RollServer ──────────────────────────

    static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\PixelsDiceRoll";
    static constexpr int kMaxConnectAttempts = 5;
    static constexpr int kConnectRetryMs = 2000;
    static constexpr const char* kTrayTaskName = "PixelsTray Service";
    static constexpr const wchar_t* kTrayExePath = L"./mods/PixelsTray/PixelsTray.exe";

    static HANDLE g_pipe = INVALID_HANDLE_VALUE;
    static std::mutex g_pipeMutex;
    static std::thread g_pipeListenerThread;
    static std::atomic<bool> g_pipeRunning{false};

    // Minimal JSON helpers for the flat protocol
    static std::string jsonStringValue(const std::string& json, const std::string& key)
    {
        std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }

    static int jsonIntValue(const std::string& json, const std::string& key, int def = 0)
    {
        std::string needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) return def;
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) return def;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        try { return std::stoi(json.substr(pos)); }
        catch (...) { return def; }
    }

    // Try to launch the tray app via scheduled task or direct elevation
    static bool scheduledTaskExists(const char* taskName)
    {
        std::string query = "schtasks /query /TN \"" + std::string(taskName) + "\" >nul 2>&1";
        int result = WinExec(query.c_str(), SW_HIDE);
        return (result > 31);
    }

    static bool launchTrayElevated()
    {
        wchar_t fullPath[MAX_PATH];
        if (!GetFullPathNameW(kTrayExePath, MAX_PATH, fullPath, nullptr))
        {
            _LOG("[PipeClient] Failed to resolve tray exe path");
            return false;
        }

        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = fullPath;
        sei.nShow = SW_HIDE;
        sei.fMask = SEE_MASK_NO_CONSOLE;

        if (!ShellExecuteExW(&sei))
        {
            _LOG("[PipeClient] ShellExecuteEx failed: %lu", GetLastError());
            return false;
        }
        return true;
    }

    static bool launchTrayTaskOrElevated()
    {
        if (scheduledTaskExists(kTrayTaskName))
        {
            std::string cmd = "schtasks /run /TN \"" + std::string(kTrayTaskName) + "\" /I";
            _LOG("[PipeClient] Running task: %s", cmd.c_str());
            int result = WinExec(cmd.c_str(), SW_HIDE);
            if (result > 31)
            {
                _LOG("[PipeClient] Tray task launched successfully");
                return true;
            }
            _LOG("[PipeClient] Task run failed (code %d), falling back to elevation", result);
        }
        else
        {
            _LOG("[PipeClient] Scheduled task not found, falling back to elevation");
        }

        return launchTrayElevated();
    }

    // Connect to the named pipe, optionally launching the tray app first
    static bool connectPipe()
    {
        for (int attempt = 0; attempt < kMaxConnectAttempts; attempt++)
        {
            HANDLE pipe = CreateFileW(
                kPipeName,
                GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);

            if (pipe != INVALID_HANDLE_VALUE)
            {
                std::lock_guard<std::mutex> lock(g_pipeMutex);
                g_pipe = pipe;
                _LOG("[PipeClient] Connected to tray app (attempt %d)", attempt + 1);
                return true;
            }

            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND && attempt == 0)
            {
                _LOG("[PipeClient] Pipe not found, attempting to launch tray app...");
                launchTrayTaskOrElevated();
            }
            else if (err == ERROR_PIPE_BUSY)
            {
                _LOG("[PipeClient] Pipe busy, waiting...");
                WaitNamedPipeW(kPipeName, 5000);
                continue;
            }

            _LOG("[PipeClient] Connect attempt %d failed (error %lu), retrying in %dms...",
                attempt + 1, err, kConnectRetryMs);
            Sleep(kConnectRetryMs);
        }

        _LOG("[PipeClient] Failed to connect after %d attempts", kMaxConnectAttempts);
        return false;
    }

    static void disconnectPipe()
    {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        if (g_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }
    }

    // Send a JSON request and read the JSON response (newline-delimited)
    static bool sendAndReceive(const std::string& request, std::string& response)
    {
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        if (g_pipe == INVALID_HANDLE_VALUE) return false;

        std::string msg = request + "\n";
        DWORD written = 0;
        if (!WriteFile(g_pipe, msg.c_str(), static_cast<DWORD>(msg.size()), &written, nullptr))
        {
            _LOG("[PipeClient] WriteFile failed: %lu", GetLastError());
            return false;
        }
        FlushFileBuffers(g_pipe);

        response.clear();
        char buf[4096];
        while (true)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(g_pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) || bytesRead == 0)
            {
                _LOG("[PipeClient] ReadFile failed: %lu", GetLastError());
                return false;
            }
            buf[bytesRead] = '\0';
            response += buf;
            if (response.find('\n') != std::string::npos) break;
        }

        // Trim
        while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
            response.pop_back();

        return true;
    }

    // Send a roll request to the tray app and block until the result arrives
    static bool requestRoll(const char* mode, uint32_t generation, SmartDiceResult& outResult)
    {
        char json[256];
        snprintf(json, sizeof(json),
            "{\"mode\": \"%s\", \"generation\": %u}", mode, generation);

        _LOG("[PipeClient] Sending: %s", json);

        std::string response;
        if (!sendAndReceive(json, response))
        {
            _LOG("[PipeClient] Request failed");
            return false;
        }

        _LOG("[PipeClient] Response: %s", response.c_str());

        outResult.die1 = static_cast<uint32_t>(jsonIntValue(response, "die1"));
        outResult.die2 = static_cast<uint32_t>(jsonIntValue(response, "die2"));
        outResult.generation = static_cast<uint32_t>(jsonIntValue(response, "generation"));
        outResult.ready = true;
        return true;
    }

    // Send the "ready" command so the tray app clicks the BG3 dice icon
    static void sendReady()
    {
        std::string response;
        sendAndReceive("{\"mode\": \"ready\"}", response);
        _LOG("[PipeClient] Ready response: %s", response.c_str());
    }

    // Background thread that waits for roll requests from the hooks,
    // communicates with the tray app, and fills g_smartDiceResult.
    static std::condition_variable g_rollRequestCv;
    static std::mutex g_rollRequestMutex;
    static bool g_rollRequestPending = false;
    static std::string g_rollRequestMode;
    static uint32_t g_rollRequestGeneration = 0;

    static void notifyTrayForRoll(const char* mode, uint32_t generation)
    {
        {
            std::lock_guard<std::mutex> lock(g_rollRequestMutex);
            g_rollRequestMode = mode;
            g_rollRequestGeneration = generation;
            g_rollRequestPending = true;
        }
        g_rollRequestCv.notify_one();
    }

    static void pipeListenerThread()
    {
        _LOG("[PipeClient] Listener thread started");

        if (!connectPipe())
        {
            _LOG("[PipeClient] Listener thread exiting — no connection");
            return;
        }

        while (g_pipeRunning)
        {
            std::string mode;
            uint32_t generation = 0;

            // Wait for a roll request from the hooks
            {
                std::unique_lock<std::mutex> lock(g_rollRequestMutex);
                g_rollRequestCv.wait(lock, []()
                {
                    return g_rollRequestPending || !g_pipeRunning;
                });
                if (!g_pipeRunning) break;

                mode = g_rollRequestMode;
                generation = g_rollRequestGeneration;
                g_rollRequestPending = false;
            }

            // Send the roll request to the tray app and wait for the result
            SmartDiceResult result{};
            if (requestRoll(mode.c_str(), generation, result))
            {
                // Fill g_smartDiceResult so the next followup call can patch
                {
                    std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
                    g_smartDiceResult = result;
                }
                _LOG("[PipeClient] Roll result ready: gen=%u die1=%u die2=%u",
                    result.generation, result.die1, result.die2);

                // Send "ready" so the tray app clicks the BG3 dice icon
                sendReady();
            }
            else
            {
                _LOG("[PipeClient] Roll request failed, attempting reconnect...");
                disconnectPipe();
                if (!connectPipe())
                {
                    _LOG("[PipeClient] Reconnect failed, listener stopping");
                    break;
                }
            }
        }

        disconnectPipe();
        _LOG("[PipeClient] Listener thread stopped");
    }

    static void startPipeClient()
    {
        g_pipeRunning = true;
        g_pipeListenerThread = std::thread(pipeListenerThread);
    }

    static void stopPipeClient()
    {
        g_pipeRunning = false;
        g_rollRequestCv.notify_all();
        if (g_pipeListenerThread.joinable())
            g_pipeListenerThread.join();
        disconnectPipe();
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
        notifyTrayForRoll(RollModeName(g_dialogueRoll.mode), g_dialogueRoll.generation);
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
        int total = static_cast<int>(keptDie) + session.modifier;
        bool success = total >= static_cast<int>(session.dc);

        rollState->keptNaturalRoll = keptDie;
        rollState->otherNaturalRoll = otherDie;
        rollState->finalTotal = total;

        rollState->finalKeptDie = static_cast<uint8_t>(keptDie);
        rollState->finalOtherDie = static_cast<uint8_t>(otherDie);
        rollState->finalModifier = static_cast<uint8_t>(session.modifier);
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
        startPipeClient();

        _LOG("Ready.");
    }

}
