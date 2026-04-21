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
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include "minhook/include/MinHook.h"

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

// ── GetRawInputData hook — triangle injection for DualSense controller ──
//
// DualSense USB HID report (64 bytes, report ID 0x01):
//   Byte 0: Report ID (0x01)
//   Byte 1: Left stick X
//   Byte 2: Left stick Y
//   Byte 3: Right stick X
//   Byte 4: Right stick Y
//   Byte 5: D-pad (bits 0-3) | Square(4) | Cross(5) | Circle(6) | Triangle(7)
//
// Triangle = byte 5, bit 7 = 0x80

using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT hRawInput, UINT uiCommand,
    LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
static GetRawInputData_t GetRawInputData_Original = nullptr;

static std::atomic<bool> g_hasHidController{false};  // true once we see DualSense input
static std::atomic<int>  g_injectFrames{0};           // >0 = inject triangle for N more reports
static HANDLE g_dualsenseDevice = nullptr;             // cached device handle for DualSense

static constexpr int kTrianglePressReports  = 3;   // ~12ms at 250Hz — quick tap, avoids fast-forward
static constexpr int kTriangleButtonByte    = 8;   // offset in HID report (byte 8 = dpad + face buttons)
static constexpr BYTE kTriangleButtonMask   = 0x80; // bit 7 = Triangle

// Check if a raw input device is a DualSense controller
static bool isDualSenseDevice(HANDLE hDevice)
{
    // Get device name (path contains VID/PID)
    UINT nameSize = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &nameSize) != 0)
        return false;

    std::wstring deviceName(nameSize, L'\0');
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, &deviceName[0], &nameSize) == (UINT)-1)
        return false;

    // Convert to lowercase for matching
    for (auto& ch : deviceName) ch = towlower(ch);

    // DualSense VID=054C PID=0CE6, DualSense Edge VID=054C PID=0DF2
    bool isDualSense = (deviceName.find(L"vid_054c") != std::wstring::npos) &&
                       (deviceName.find(L"pid_0ce6") != std::wstring::npos ||
                        deviceName.find(L"pid_0df2") != std::wstring::npos);

    // Log device info for diagnostics
    char narrowName[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, deviceName.c_str(), -1, narrowName, sizeof(narrowName), nullptr, nullptr);
    _LOG("[RawInput] HID device: %s → %s", narrowName, isDualSense ? "DUALSENSE" : "other");

    return isDualSense;
}

static UINT WINAPI GetRawInputData_Hook(HRAWINPUT hRawInput, UINT uiCommand,
    LPVOID pData, PUINT pcbSize, UINT cbSizeHeader)
{
    UINT result = GetRawInputData_Original(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    if (pData && uiCommand == RID_INPUT && result != (UINT)-1)
    {
        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);

        if (raw->header.dwType == RIM_TYPEHID &&
            raw->data.hid.dwSizeHid > static_cast<DWORD>(kTriangleButtonByte) &&
            raw->data.hid.dwCount > 0)
        {
            // Identify the DualSense device on first HID report
            if (!g_hasHidController.load(std::memory_order_relaxed))
            {
                if (isDualSenseDevice(raw->header.hDevice))
                {
                    g_dualsenseDevice = raw->header.hDevice;
                    g_hasHidController.store(true, std::memory_order_relaxed);
                    _LOG("[RawInput] DualSense detected — triangle injection available (handle=%p)",
                        raw->header.hDevice);

                    // Dump first report for verification
                    BYTE* reportData = raw->data.hid.bRawData;
                    char hex[256] = {};
                    int pos = 0;
                    UINT sz = raw->data.hid.dwSizeHid;
                    for (UINT i = 0; i < sz && i < 20 && pos < 250; i++)
                        pos += sprintf_s(hex + pos, sizeof(hex) - pos, "%02X ", reportData[i]);
                    _LOG("[RawInput] Report (size=%u): %s", sz, hex);
                }
            }

            // NOTE: injection via GetRawInputData doesn't affect SDL's input.
            // SDL reads DualSense via hidapi (ReadFile). Injection is done
            // in ReadFile_Hook / GetOverlappedResult_Hook instead.
        }
    }

    return result;
}

static std::atomic<int> g_readFile64Count{0};  // count 64-byte reads for diagnostics

static void requestTrianglePress()
{
    g_injectFrames.store(kTrianglePressReports, std::memory_order_relaxed);
    _LOG("[RawInput] Triangle press requested (%d reports), 64-byte reads so far: %d",
        kTrianglePressReports, g_readFile64Count.load(std::memory_order_relaxed));
}

// ── HID file hooks — intercept SDL's hidapi reads from the DualSense ──
//
// SDL reads the DualSense via hidapi which uses ReadFile with overlapped I/O.
// We hook CreateFileW to identify the DualSense handle, then ReadFile +
// GetOverlappedResult to modify completed reads.

using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResult_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);

static CreateFileA_t CreateFileA_Original = nullptr;
static CreateFileW_t CreateFileW_Original = nullptr;
static ReadFile_t ReadFile_Original = nullptr;
static GetOverlappedResult_t GetOverlappedResult_Original = nullptr;

// SDL opens the DualSense multiple times — track all handles
static std::mutex g_dsHandlesMutex;
static std::vector<HANDLE> g_dualsenseHandles;

static bool isDualsenseHandle(HANDLE h)
{
    std::lock_guard<std::mutex> lock(g_dsHandlesMutex);
    for (auto& dh : g_dualsenseHandles)
        if (dh == h) return true;
    return false;
}

// Track ONE pending overlapped DualSense read (SDL only has one at a time)
static std::atomic<LPOVERLAPPED> g_pendingOl{nullptr};
static std::atomic<LPVOID>       g_pendingBuf{nullptr};

static HANDLE WINAPI CreateFileW_Hook(LPCWSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    HANDLE result = CreateFileW_Original(lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (result != INVALID_HANDLE_VALUE && lpFileName)
    {
        std::wstring path(lpFileName);
        for (auto& ch : path) ch = towlower(ch);

        if (path.find(L"vid_054c") != std::wstring::npos &&
            (path.find(L"pid_0ce6") != std::wstring::npos ||
             path.find(L"pid_0df2") != std::wstring::npos))
        {
            {
                std::lock_guard<std::mutex> lock(g_dsHandlesMutex);
                g_dualsenseHandles.push_back(result);
            }
            g_hasHidController.store(true, std::memory_order_relaxed);

            char narrow[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, narrow, sizeof(narrow), nullptr, nullptr);
            _LOG("[HID] DualSense file opened: handle=%p path=%s", result, narrow);
        }
    }

    return result;
}

static HANDLE WINAPI CreateFileA_Hook(LPCSTR lpFileName, DWORD dwDesiredAccess,
    DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    HANDLE result = CreateFileA_Original(lpFileName, dwDesiredAccess, dwShareMode,
        lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (result != INVALID_HANDLE_VALUE && lpFileName)
    {
        std::string path(lpFileName);
        for (auto& ch : path) ch = tolower(ch);

        if (path.find("vid_054c") != std::string::npos &&
            (path.find("pid_0ce6") != std::string::npos ||
             path.find("pid_0df2") != std::string::npos))
        {
            {
                std::lock_guard<std::mutex> lock(g_dsHandlesMutex);
                g_dualsenseHandles.push_back(result);
            }
            g_hasHidController.store(true, std::memory_order_relaxed);
            _LOG("[HID] DualSense file opened (A): handle=%p path=%s", result, lpFileName);
        }
    }

    return result;
}

static std::atomic<bool> g_readFileLoggedOnce{false};

static BOOL WINAPI ReadFile_Hook(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    BOOL result = ReadFile_Original(hFile, lpBuffer, nNumberOfBytesToRead,
        lpNumberOfBytesRead, lpOverlapped);

    // Fast path: only care about 64-byte reads (DualSense USB HID report size)
    if (nNumberOfBytesToRead != 64 || !lpBuffer)
        return result;

    g_readFile64Count.fetch_add(1, std::memory_order_relaxed);

    if (result)
    {
        // Synchronous completion — check if it looks like a DualSense report
        BYTE* buf = static_cast<BYTE*>(lpBuffer);
        if (buf[0] == 0x01)  // DualSense USB report ID
        {
            if (!g_readFileLoggedOnce.exchange(true, std::memory_order_relaxed))
            {
                g_hasHidController.store(true, std::memory_order_relaxed);
                char hex[64] = {};
                int pos = 0;
                for (int i = 0; i < 10 && pos < 60; i++)
                    pos += sprintf_s(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
                _LOG("[HID] DualSense 64-byte read (sync): handle=%p data=%s", hFile, hex);
            }
            // NOTE: do NOT inject here — sync reads are consumed by Steam,
            // not SDL. Injection happens in GetOverlappedResult_Hook only.
        }
    }
    else if (lpOverlapped && GetLastError() == ERROR_IO_PENDING)
    {
        // Async — save for GetOverlappedResult (any 64-byte overlapped read)
        DWORD savedErr = GetLastError();
        g_pendingBuf.store(lpBuffer, std::memory_order_relaxed);
        g_pendingOl.store(lpOverlapped, std::memory_order_relaxed);
        SetLastError(savedErr);

        if (!g_readFileLoggedOnce.exchange(true, std::memory_order_relaxed))
            _LOG("[HID] 64-byte overlapped read pending: handle=%p", hFile);
    }

    return result;
}

static std::atomic<bool> g_gorLoggedOnce{false};

static BOOL WINAPI GetOverlappedResult_Hook(HANDLE hFile, LPOVERLAPPED lpOverlapped,
    LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
    BOOL result = GetOverlappedResult_Original(hFile, lpOverlapped,
        lpNumberOfBytesTransferred, bWait);

    if (result && lpOverlapped == g_pendingOl.load(std::memory_order_relaxed))
    {
        LPVOID buffer = g_pendingBuf.load(std::memory_order_relaxed);
        g_pendingOl.store(nullptr, std::memory_order_relaxed);

        if (buffer)
        {
            BYTE* buf = static_cast<BYTE*>(buffer);

            // Diagnostic: log once when async DualSense read completes
            if (buf[0] == 0x01 && !g_gorLoggedOnce.load(std::memory_order_relaxed))
            {
                g_gorLoggedOnce.store(true, std::memory_order_relaxed);
                DWORD bytes = lpNumberOfBytesTransferred ? *lpNumberOfBytesTransferred : 0;
                char hex[64] = {};
                int pos = 0;
                for (int i = 0; i < 10 && pos < 60; i++)
                    pos += sprintf_s(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
                _LOG("[HID] Async DualSense read completed: handle=%p bytes=%u data=%s",
                    hFile, bytes, hex);
                g_hasHidController.store(true, std::memory_order_relaxed);
            }

            int remaining = g_injectFrames.load(std::memory_order_relaxed);
            if (remaining > 0 && buf[0] == 0x01)
            {
                buf[kTriangleButtonByte] |= kTriangleButtonMask;
                g_injectFrames.fetch_sub(1, std::memory_order_relaxed);

                if (remaining == kTrianglePressReports)
                    _LOG("[HID] Triangle injection STARTED (async, byte[%d]: 0x%02X)",
                        kTriangleButtonByte, buf[kTriangleButtonByte]);
                if (remaining == 1)
                    _LOG("[HID] Triangle injection ENDED");
            }
        }
    }

    return result;
}

static void hookAllInput()
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

    // 1. GetRawInputData (covers raw input path)
    auto fnRaw = GetProcAddress(user32, "GetRawInputData");
    if (fnRaw) {
        if (MH_CreateHook(fnRaw, GetRawInputData_Hook,
                reinterpret_cast<LPVOID*>(&GetRawInputData_Original)) == MH_OK &&
            MH_EnableHook(fnRaw) == MH_OK)
            _LOG("[Hook] GetRawInputData hooked");
        else
            _LOG("[Hook] GetRawInputData hook FAILED");
    }

    // 2a. CreateFileA (SDL hidapi uses this to open HID devices)
    auto fnCreateA = GetProcAddress(kernel32, "CreateFileA");
    if (fnCreateA) {
        if (MH_CreateHook(fnCreateA, CreateFileA_Hook,
                reinterpret_cast<LPVOID*>(&CreateFileA_Original)) == MH_OK &&
            MH_EnableHook(fnCreateA) == MH_OK)
            _LOG("[Hook] CreateFileA hooked");
        else
            _LOG("[Hook] CreateFileA hook FAILED");
    }

    // 2b. CreateFileW (other code paths)
    auto fnCreate = GetProcAddress(kernel32, "CreateFileW");
    if (fnCreate) {
        if (MH_CreateHook(fnCreate, CreateFileW_Hook,
                reinterpret_cast<LPVOID*>(&CreateFileW_Original)) == MH_OK &&
            MH_EnableHook(fnCreate) == MH_OK)
            _LOG("[Hook] CreateFileW hooked");
        else
            _LOG("[Hook] CreateFileW hook FAILED");
    }

    // 3. ReadFile — try kernelbase first (actual implementation), fall back to kernel32
    HMODULE kernelbase = GetModuleHandleA("kernelbase.dll");
    FARPROC fnRead = kernelbase ? GetProcAddress(kernelbase, "ReadFile") : nullptr;
    if (!fnRead) fnRead = GetProcAddress(kernel32, "ReadFile");
    if (fnRead) {
        if (MH_CreateHook(fnRead, ReadFile_Hook,
                reinterpret_cast<LPVOID*>(&ReadFile_Original)) == MH_OK &&
            MH_EnableHook(fnRead) == MH_OK)
            _LOG("[Hook] ReadFile hooked (%s)", kernelbase ? "kernelbase" : "kernel32");
        else
            _LOG("[Hook] ReadFile hook FAILED");
    }

    // 4. GetOverlappedResult — try kernelbase first
    FARPROC fnOverlapped = kernelbase ? GetProcAddress(kernelbase, "GetOverlappedResult") : nullptr;
    if (!fnOverlapped) fnOverlapped = GetProcAddress(kernel32, "GetOverlappedResult");
    if (fnOverlapped) {
        if (MH_CreateHook(fnOverlapped, GetOverlappedResult_Hook,
                reinterpret_cast<LPVOID*>(&GetOverlappedResult_Original)) == MH_OK &&
            MH_EnableHook(fnOverlapped) == MH_OK)
            _LOG("[Hook] GetOverlappedResult hooked (%s)", kernelbase ? "kernelbase" : "kernel32");
        else
            _LOG("[Hook] GetOverlappedResult hook FAILED");
    }
}

// ── Utility ──
static HWND findBG3Window()
{
    HWND hw = FindWindowA("SDL_app", nullptr);
    if (!hw) hw = FindWindowA(nullptr, "Baldur's Gate 3");
    return hw;
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

                // trigger the dice roll: triangle injection if controller present, mouse click fallback
                if (g_hasHidController.load(std::memory_order_relaxed))
                {
                    _LOG("[PipeClient] Controller detected — using triangle injection");
                    requestTrianglePress();
                }
                else
                {
                    _LOG("[PipeClient] No controller — using mouse click via tray app");
                    sendReady();
                }
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
    // Hook input APIs for DualSense triangle injection + raw input diagnostics
    hookAllInput();

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
