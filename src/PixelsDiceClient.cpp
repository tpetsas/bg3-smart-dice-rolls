/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#include "PixelsDiceClient.h"
#include "SmartDiceResult.h"
#include "Logger.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
#include <shellapi.h>

// shared state (defined in SmartDiceRolls.cpp)
extern std::mutex g_dialogueRollMutex;
extern SmartDiceResult g_smartDiceResult;

static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\PixelsDiceRoll";
static constexpr int kMaxConnectAttempts = 5;
static constexpr int kConnectRetryMs = 2000;
static constexpr const char* kTrayTaskName = "PixelsTray Service";
static constexpr const wchar_t* kTrayExePath = L"./mods/PixelsTray/PixelsTray.exe";

// internal state
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static std::mutex g_pipeMutex;
static std::thread g_pipeListenerThread;
static std::atomic<bool> g_pipeRunning{false};

static std::condition_variable g_rollRequestCv;
static std::mutex g_rollRequestMutex;
static bool g_rollRequestPending = false;
static std::string g_rollRequestMode;
static uint32_t g_rollRequestGeneration = 0;

// minimal JSON helpers
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

// tray app launcher
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

// pipe connection
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

    // trim white spaces and new lines
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    return true;
}

// roll request / ready command
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

    // check for error response (e.g. timeout, unknown mode).
    // the pipe is fine — the server just couldn't fulfil the request.
    // return true (no reconnect needed) but leave outResult.ready = false.
    if (response.find("\"error\"") != std::string::npos)
    {
        _LOG("[PipeClient] Server returned error, not updating result");
        outResult.ready = false;
        return true;
    }

    outResult.die1 = static_cast<uint32_t>(jsonIntValue(response, "die1"));
    outResult.die2 = static_cast<uint32_t>(jsonIntValue(response, "die2"));
    outResult.generation = static_cast<uint32_t>(jsonIntValue(response, "generation"));
    outResult.ready = true;
    return true;
}

static void sendReady()
{
    std::string response;
    sendAndReceive("{\"mode\": \"ready\"}", response);
    _LOG("[PipeClient] Ready response: %s", response.c_str());
}

// background listener thread
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

        // wait for a roll request from the hooks
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

        // send the roll request to the tray app and wait for the result
        SmartDiceResult result{};
        if (requestRoll(mode.c_str(), generation, result))
        {
            if (result.ready)
            {
                // fill g_smartDiceResult so the next followup call can patch
                {
                    std::lock_guard<std::mutex> lock(g_dialogueRollMutex);
                    g_smartDiceResult = result;
                }
                _LOG("[PipeClient] Roll result ready: gen=%u die1=%u die2=%u",
                    result.generation, result.die1, result.die2);

                // send "ready" so the tray app clicks the BG3 dice icon
                sendReady();
            }
            // else: server returned an error (e.g. timeout) — pipe is fine, just skip
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

// public API
void PixelsDiceClient::start()
{
    g_pipeRunning = true;
    g_pipeListenerThread = std::thread(pipeListenerThread);
}

void PixelsDiceClient::stop()
{
    g_pipeRunning = false;
    g_rollRequestCv.notify_all();
    if (g_pipeListenerThread.joinable())
        g_pipeListenerThread.join();
    disconnectPipe();
}

void PixelsDiceClient::notifyTrayForRoll(const char* mode, uint32_t generation)
{
    {
        std::lock_guard<std::mutex> lock(g_rollRequestMutex);
        g_rollRequestMode = mode;
        g_rollRequestGeneration = generation;
        g_rollRequestPending = true;
    }
    g_rollRequestCv.notify_one();
}
