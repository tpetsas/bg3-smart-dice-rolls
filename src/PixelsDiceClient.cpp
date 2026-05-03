/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#include "PixelsDiceClient.h"
#include "SmartDiceResult.h"
#include "Logger.h"
#include "Config.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <cmath>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>
#include <Xinput.h>
#include "minhook/include/MinHook.h"

// shared state (defined in SmartDiceRolls.cpp)
extern std::mutex g_dialogueRollMutex;
extern SmartDiceResult g_smartDiceResult;
extern Config g_config;

static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\PixelsDiceRoll";
static constexpr int kMaxConnectAttempts = 5;
static constexpr int kConnectRetryMs = 2000;
static constexpr const wchar_t* kTrayExePath = L"./mods/PixelsDiceTray/PixelsTray.exe";
static constexpr const wchar_t* kTrayProcessName = L"PixelsTray.exe";

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

// forward declarations
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t CreateFileW_Original;
static HWND findBG3Window();

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

// check if a process is already running by name
static bool isProcessRunning(const wchar_t* processName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, processName) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// tray app launcher — CreateProcessW with DETACHED_PROCESS so the child
// is fully independent from BG3 (no inherited handles, no shared job object,
// no interaction with hooked shell APIs).
static bool launchTray()
{
    wchar_t fullPath[MAX_PATH];
    if (!GetFullPathNameW(kTrayExePath, MAX_PATH, fullPath, nullptr))
    {
        _LOG("[PipeClient] Failed to resolve tray exe path");
        return false;
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        fullPath, nullptr, nullptr, nullptr,
        FALSE,  // do NOT inherit handles
        DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr, &si, &pi);

    if (!ok)
    {
        _LOG("[PipeClient] CreateProcessW failed: %lu", GetLastError());
        return false;
    }

    _LOG("[PipeClient] PixelsTray launched (PID %lu)", pi.dwProcessId);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// pipe connection
static bool connectPipe()
{
    for (int attempt = 0; attempt < kMaxConnectAttempts; attempt++)
    {
        // Use the original (unhooked) CreateFileW if available, so the pipe
        // connection never travels through our HID hook layer.
        auto openFn = CreateFileW_Original ? CreateFileW_Original : CreateFileW;
        HANDLE pipe = openFn(
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
            if (isProcessRunning(kTrayProcessName))
            {
                _LOG("[PipeClient] PixelsTray is running but pipe not ready yet, waiting...");
            }
            else
            {
                _LOG("[PipeClient] Pipe not found, launching tray app...");
                launchTray();
            }
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

#define FAKE_MOUSE_DOWN_HINPUT ((HRAWINPUT)0xFEEDFACE1111)
#define FAKE_MOUSE_UP_HINPUT   ((HRAWINPUT)0xFEEDFACE2222)

static void requestMouseClick()
{
    std::thread([]
    {
        HWND hw = findBG3Window();
        if (!hw) return;

        RECT rc{};
        if (!GetClientRect(hw, &rc)) return;

        // Client-space coordinates for WM_LBUTTONDOWN lParam
        LONG cx = static_cast<LONG>(std::lround((rc.right - rc.left) * g_config.mouseClickNormX));
        LONG cy = static_cast<LONG>(std::lround((rc.bottom - rc.top) * g_config.mouseClickNormY));

        // Screen-space coordinates for SendInput cursor move
        POINT screenPt{cx, cy};
        if (!ClientToScreen(hw, &screenPt)) return;

        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw <= 0 || sh <= 0) return;
        LONG ax = static_cast<LONG>(std::lround(screenPt.x * 65535.0 / (sw - 1)));
        LONG ay = static_cast<LONG>(std::lround(screenPt.y * 65535.0 / (sh - 1)));

        // Focus and move cursor so BG3 registers the hover.
        SetForegroundWindow(hw);
        Sleep(100);
        INPUT move{};
        move.type = INPUT_MOUSE;
        move.mi.dx = ax; move.mi.dy = ay;
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &move, sizeof(INPUT));
        Sleep(200);

        // Re-focus, then loop-inject over a 400ms window — same idea as Xbox injection:
        // keep firing until BG3's message loop picks it up on the right frame.
        SetForegroundWindow(hw);
        Sleep(50);

        ULONGLONG deadline = GetTickCount64() + 400;
        int clicks = 0;
        while (GetTickCount64() < deadline)
        {
            PostMessageW(hw, WM_INPUT, MAKEWPARAM(RIM_INPUT, 0), (LPARAM)FAKE_MOUSE_DOWN_HINPUT);
            Sleep(50);
            PostMessageW(hw, WM_INPUT, MAKEWPARAM(RIM_INPUT, 0), (LPARAM)FAKE_MOUSE_UP_HINPUT);
            Sleep(60);
            clicks++;
        }
        _LOG("[Mouse] Injected %d raw clicks to hwnd=%p (client %ld,%ld)", clicks, hw, cx, cy);
    }).detach();
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

static std::atomic<bool> g_hasHidController{false};  // true once we see any supported controller
static HANDLE g_dualsenseDevice = nullptr;             // cached RawInput device handle (diagnostic only)

// Xbox device handle tracking — a set rather than a single handle because the kernel
// can present a different HANDLE value for the same physical device across resets
// (e.g. when BG3 re-registers raw input during UI transitions).
static std::mutex                 g_xboxDevicesMutex;
static std::unordered_set<HANDLE> g_xboxRawDevices;   // confirmed Xbox device handles
static std::unordered_set<HANDLE> g_checkedDevices;   // handles already classified (avoid repeat syscalls)
static std::atomic<bool>          g_hasXboxDevice{false};
static HANDLE                     g_lastXboxDevice = nullptr; // first discovered Xbox handle (for synthetic reports)

// Sentinel HRAWINPUT for synthetic Xbox Y-press injection.
// Value is non-aligned (LSB set) so it can never be a real kernel pointer.
#define FAKE_XBOX_HINPUT ((HRAWINPUT)0xCAFEBABE1337)

// Neutral-stick Xbox HID report (16 bytes) with Y button pressed (byte[11] = 0x08).
// Layout: [0]=header, [1-2]=LX, [3-4]=LY, [5-6]=RX, [7-8]=RY,
//         [9]=LT, [10]=status(0x80), [11]=face buttons, [12-15]=other.
static const BYTE kSyntheticXboxYReport[16] = {
    0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00,
    0x80, 0x00, 0x80, 0x08, 0x00, 0x00, 0x00, 0x00
};

// Time-based injection window — inject into ALL reads until the deadline passes.
// Multiple handles (Steam + SDL) may read concurrently; a frame counter would be
// consumed by whichever completes first. A time window ensures all handles see it.
static std::atomic<ULONGLONG> g_injectUntilTick{0};
static std::atomic<bool>     g_injectLogged{false};       // log only first injection per window
static constexpr ULONGLONG kInjectWindowMs       = 50;   // 50ms window — covers several polling cycles
static constexpr int  kTriangleButtonByteUSB     = 8;    // USB:       report ID 0x01, 64 bytes, buttons at byte 8
static constexpr int  kTriangleButtonByteBTSimple= 5;    // BT Simple: report ID 0x01, 78 bytes, buttons at byte 5
static constexpr int  kTriangleButtonByteBTFull  = 9;    // BT Full:   report ID 0x31, 78 bytes, buttons at byte 9
static constexpr BYTE kTriangleButtonMask        = 0x80; // bit 7 = Triangle

// CRC32 for DualSense BT Full reports — SDL validates this and discards on mismatch.
// Standard CRC32 (polynomial 0xEDB88320, same as zlib/Ethernet).
static uint32_t crc32Table[256];
static bool     crc32TableBuilt = false;

static void buildCrc32Table()
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32Table[i] = c;
    }
    crc32TableBuilt = true;
}

static uint32_t crc32Compute(uint32_t crc, const BYTE* data, size_t len)
{
    if (!crc32TableBuilt) buildCrc32Table();
    for (size_t i = 0; i < len; i++)
        crc = crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Recompute and patch the CRC32 at the end of a BT Full report (78 bytes).
// Seed: CRC32 of [0xA1], then hash report bytes [0..73], store in [74..77] LE.
static void fixBtFullCrc(BYTE* report, DWORD reportSize)
{
    if (reportSize < 78) return;
    BYTE seed = 0xA1;  // BT HID input report header
    uint32_t crc = crc32Compute(0xFFFFFFFF, &seed, 1);
    crc = crc32Compute(crc, report, 74);
    crc ^= 0xFFFFFFFF;
    report[74] = (BYTE)(crc);
    report[75] = (BYTE)(crc >> 8);
    report[76] = (BYTE)(crc >> 16);
    report[77] = (BYTE)(crc >> 24);
}

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
    return (deviceName.find(L"vid_054c") != std::wstring::npos) &&
           (deviceName.find(L"pid_0ce6") != std::wstring::npos ||
            deviceName.find(L"pid_0df2") != std::wstring::npos);
}

// Check if a raw input device is an Xbox controller (Microsoft VID 045E)
static bool isXboxDevice(HANDLE hDevice)
{
    UINT nameSize = 0;
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &nameSize) != 0)
        return false;
    std::wstring name(nameSize, L'\0');
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, &name[0], &nameSize) == (UINT)-1)
        return false;
    for (auto& ch : name) ch = towlower(ch);
    return name.find(L"vid_045e") != std::wstring::npos;
}

// Classify dev on first sight; return true if it is an Xbox device.
// Subsequent calls for the same handle are O(1) — no repeated system calls.
static bool isXboxRawDevice(HANDLE dev)
{
    {
        std::lock_guard<std::mutex> lk(g_xboxDevicesMutex);
        if (g_xboxRawDevices.count(dev)) return true;
        if (g_checkedDevices.count(dev)) return false;
        g_checkedDevices.insert(dev);
    }
    if (isXboxDevice(dev))
    {
        _LOG("[RawInput] Xbox device discovered (handle=%p) — Y injection available", dev);
        g_hasHidController.store(true, std::memory_order_relaxed);
        g_hasXboxDevice.store(true, std::memory_order_relaxed);
        g_lastXboxDevice = dev;
        std::lock_guard<std::mutex> lk(g_xboxDevicesMutex);
        g_xboxRawDevices.insert(dev);
        return true;
    }
    return false;
}

static UINT WINAPI GetRawInputData_Hook(HRAWINPUT hRawInput, UINT uiCommand,
    LPVOID pData, PUINT pcbSize, UINT cbSizeHeader)
{
    // Handle synthetic Xbox Y-press: we posted WM_INPUT with FAKE_XBOX_HINPUT so this hook
    // fires even when the controller is idle (no real WM_INPUT would have arrived otherwise).
    if (hRawInput == FAKE_XBOX_HINPUT)
    {
        constexpr UINT kReportBytes = sizeof(kSyntheticXboxYReport);
        UINT fullSize = cbSizeHeader + sizeof(DWORD) + sizeof(DWORD) + kReportBytes;

        if (uiCommand == RID_HEADER)
        {
            // Caller querying just the header (some SDKs do this before the full read)
            if (!pData) { *pcbSize = cbSizeHeader; return 0; }
            if (*pcbSize < cbSizeHeader) { *pcbSize = cbSizeHeader; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
            RAWINPUTHEADER* hdr = reinterpret_cast<RAWINPUTHEADER*>(pData);
            hdr->dwType  = RIM_TYPEHID;
            hdr->dwSize  = fullSize;
            hdr->hDevice = g_lastXboxDevice;
            hdr->wParam  = RIM_INPUT;
            *pcbSize = cbSizeHeader;
            return cbSizeHeader;
        }

        if (uiCommand == RID_INPUT)
        {
            if (!pData) { *pcbSize = fullSize; return 0; }
            if (*pcbSize < fullSize) { *pcbSize = fullSize; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
            RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(pData);
            ZeroMemory(ri, fullSize);
            ri->header.dwType       = RIM_TYPEHID;
            ri->header.dwSize       = fullSize;
            ri->header.hDevice      = g_lastXboxDevice;
            ri->header.wParam       = RIM_INPUT;
            ri->data.hid.dwSizeHid  = kReportBytes;
            ri->data.hid.dwCount    = 1;
            memcpy(ri->data.hid.bRawData, kSyntheticXboxYReport, kReportBytes);
            *pcbSize = fullSize;
            _LOG("[RawInput] Synthetic Xbox Y report served (size=%u)", fullSize);
            return fullSize;
        }

        // Unknown command — let the original handle it (will fail, that's fine)
    }

    // Synthetic raw mouse left-click (no-controller fallback).
    if (hRawInput == FAKE_MOUSE_DOWN_HINPUT || hRawInput == FAKE_MOUSE_UP_HINPUT)
    {
        UINT fullSize = cbSizeHeader + sizeof(RAWMOUSE);
        bool isDown   = (hRawInput == FAKE_MOUSE_DOWN_HINPUT);

        if (uiCommand == RID_HEADER)
        {
            if (!pData) { *pcbSize = cbSizeHeader; return 0; }
            if (*pcbSize < cbSizeHeader) { *pcbSize = cbSizeHeader; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
            RAWINPUTHEADER* hdr = reinterpret_cast<RAWINPUTHEADER*>(pData);
            hdr->dwType = RIM_TYPEMOUSE; hdr->dwSize = fullSize;
            hdr->hDevice = nullptr;      hdr->wParam  = RIM_INPUT;
            *pcbSize = cbSizeHeader; return cbSizeHeader;
        }

        if (uiCommand == RID_INPUT)
        {
            if (!pData) { *pcbSize = fullSize; return 0; }
            if (*pcbSize < fullSize) { *pcbSize = fullSize; SetLastError(ERROR_INSUFFICIENT_BUFFER); return (UINT)-1; }
            RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(pData);
            ZeroMemory(ri, fullSize);
            ri->header.dwType    = RIM_TYPEMOUSE;
            ri->header.dwSize    = fullSize;
            ri->header.hDevice   = nullptr;
            ri->header.wParam    = RIM_INPUT;
            ri->data.mouse.usButtonFlags = isDown ? RI_MOUSE_LEFT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_UP;
            *pcbSize = fullSize;
            _LOG("[RawInput] Synthetic mouse %s injected", isDown ? "LEFT_DOWN" : "LEFT_UP");
            return fullSize;
        }
    }

    UINT result = GetRawInputData_Original(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    if (pData && uiCommand == RID_INPUT && result != (UINT)-1)
    {
        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);

        if (raw->header.dwType == RIM_TYPEHID &&
            raw->data.hid.dwSizeHid > static_cast<DWORD>(kTriangleButtonByteBTFull) &&
            raw->data.hid.dwCount > 0)
        {
            // Identify the DualSense device on first HID report
            if (!g_hasHidController.load(std::memory_order_relaxed))
            {
                if (isDualSenseDevice(raw->header.hDevice))
                {
                    g_dualsenseDevice = raw->header.hDevice;
                    g_hasHidController.store(true, std::memory_order_relaxed);
                    _LOG("[RawInput] DualSense detected (handle=%p)", raw->header.hDevice);
                }
            }

            // DualSense: injection happens in ReadFile_Hook / GetOverlappedResult_Hook
            // because SDL reads it via hidapi, not raw input.

            // Xbox: BG3 reads the XInput ig_00 HID interface via GetRawInputData.
            // Face buttons are in byte[11]: A=0x01, B=0x02, X=0x04, Y=0x08.
            HANDLE dev = raw->header.hDevice;
            if (isXboxRawDevice(dev))
            {
                BYTE* rd = raw->data.hid.bRawData;
                UINT  sz = raw->data.hid.dwSizeHid;

                ULONGLONG now      = GetTickCount64();
                ULONGLONG deadline = g_injectUntilTick.load(std::memory_order_relaxed);
                if (now < deadline && sz >= 12)
                {
                    rd[11] |= 0x08;  // Y button: byte 11, bit 3
                    if (!g_injectLogged.exchange(true, std::memory_order_relaxed))
                        _LOG("[RawInput] Xbox Y injected (handle=%p)", dev);
                }
            }
        }
    }

    return result;
}

// ── HID file hooks — intercept SDL's hidapi reads from the DualSense ──
//
// SDL reads the DualSense via hidapi which uses ReadFile with overlapped I/O.
// We hook CreateFileW to identify the DualSense handle, then ReadFile +
// GetOverlappedResult to modify completed reads.

using CreateFileA_t        = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFile_t            = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResult_t = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using CloseHandle_t         = BOOL(WINAPI*)(HANDLE);
using XInputGetState_t      = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

static CreateFileA_t        CreateFileA_Original         = nullptr;
static ReadFile_t            ReadFile_Original            = nullptr;
static GetOverlappedResult_t GetOverlappedResult_Original = nullptr;
static CloseHandle_t         CloseHandle_Original         = nullptr;
static XInputGetState_t      XInputGetState_Original      = nullptr;
static std::atomic<DWORD>    g_xInputFakePacket{0x40000000}; // injection-window packet counter

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

// Track every pending overlapped DualSense read, keyed by OVERLAPPED*.
// Multiple controllers issue async reads in parallel, each with its own OVERLAPPED* —
// we must remember the buffer for each so GetOverlappedResult can find and inject into it.
struct PendingRead { LPVOID buffer; HANDLE handle; };
static std::mutex                                    g_pendingMutex;
static std::unordered_map<LPOVERLAPPED, PendingRead> g_pendingReads;

static void requestTrianglePress()
{
    ULONGLONG deadline = GetTickCount64() + kInjectWindowMs;
    g_injectLogged.store(false, std::memory_order_relaxed);
    g_injectUntilTick.store(deadline, std::memory_order_relaxed);
    _LOG("[RawInput] Triangle press requested (%llums window)", kInjectWindowMs);

    // Xbox: BG3 reads controller input via GetRawInputData (WM_INPUT path), not XInput.
    // If the stick is idle when injection opens, no real WM_INPUT arrives → hook never fires.
    // Fix: post a synthetic WM_INPUT. We first query which HWND BG3 registered for raw input
    // (usually a hidden SDL background window, not the visible game window), then post there.
    if (g_hasXboxDevice.load(std::memory_order_relaxed))
    {
        // Find every registered raw-input target and log / post to all HID ones.
        UINT numDevices = 0;
        GetRegisteredRawInputDevices(nullptr, &numDevices, sizeof(RAWINPUTDEVICE));
        if (numDevices > 0)
        {
            std::vector<RAWINPUTDEVICE> rids(numDevices);
            UINT got = GetRegisteredRawInputDevices(rids.data(), &numDevices, sizeof(RAWINPUTDEVICE));
            if (got != (UINT)-1)
            {
                for (auto& rid : rids)
                {
                    // Post to every registered HID window (usUsagePage 1 = Generic Desktop)
                    HWND target = rid.hwndTarget;
                    if (!target) target = findBG3Window(); // null hwndTarget → follows focus
                    if (target && rid.usUsagePage == 0x01)
                    {
                        PostMessageW(target, WM_INPUT, MAKEWPARAM(RIM_INPUT, 0), (LPARAM)FAKE_XBOX_HINPUT);
                        _LOG("[RawInput] Posted synthetic WM_INPUT (Xbox Y) to hwnd=%p (usage=0x%X)",
                            target, rid.usUsage);
                    }
                }
            }
        }
        else
        {
            // Fallback: post to the visible game window
            HWND hwnd = findBG3Window();
            if (hwnd)
            {
                PostMessageW(hwnd, WM_INPUT, MAKEWPARAM(RIM_INPUT, 0), (LPARAM)FAKE_XBOX_HINPUT);
                _LOG("[RawInput] Posted synthetic WM_INPUT (Xbox Y) to game window %p (fallback)", hwnd);
            }
        }
    }
}

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
        }
    }

    return result;
}

static std::mutex                 g_loggedHandlesMutex;
static std::unordered_set<HANDLE> g_readFileLoggedHandles;
static std::unordered_set<HANDLE> g_gorLoggedHandles;

static BOOL WINAPI ReadFile_Hook(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped)
{
    BOOL  result   = ReadFile_Original(hFile, lpBuffer, nNumberOfBytesToRead,
                                       lpNumberOfBytesRead, lpOverlapped);
    DWORD savedErr = GetLastError();

    // Accept 64-byte (USB) and 78-byte (BT) DualSense HID reports
    if ((nNumberOfBytesToRead != 64 && nNumberOfBytesToRead != 78) || !lpBuffer)
    {
        SetLastError(savedErr);
        return result;
    }

    if (result)
    {
        // Synchronous completion
        BYTE* buf = static_cast<BYTE*>(lpBuffer);
        DWORD bytes = lpNumberOfBytesRead ? *lpNumberOfBytesRead : nNumberOfBytesToRead;

        int  buttonByte = -1;
        const char* conn = "?";
        if (buf[0] == 0x31)                    { buttonByte = kTriangleButtonByteBTFull;   conn = "BT-Full"; }
        else if (buf[0] == 0x01 && bytes > 64) { buttonByte = kTriangleButtonByteBTSimple; conn = "BT-Simple"; }
        else if (buf[0] == 0x01)               { buttonByte = kTriangleButtonByteUSB;      conn = "USB"; }

        if (buttonByte >= 0)
        {
            g_hasHidController.store(true, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(g_loggedHandlesMutex);
                if (g_readFileLoggedHandles.insert(hFile).second)
                    _LOG("[HID] DualSense %s handle=%p (sync)", conn, hFile);
            }

            // Inject on sync path too — SDL may use sync reads for BT
            ULONGLONG now = GetTickCount64();
            ULONGLONG deadline = g_injectUntilTick.load(std::memory_order_relaxed);
            if (now < deadline)
            {
                buf[buttonByte] |= kTriangleButtonMask;
                if (buf[0] == 0x31) fixBtFullCrc(buf, bytes);  // BT Full has CRC32
                if (!g_injectLogged.exchange(true, std::memory_order_relaxed))
                    _LOG("[HID] Triangle injected (%s, handle=%p)", conn, hFile);
            }
        }
    }
    else if (lpOverlapped && savedErr == ERROR_IO_PENDING)
    {
        // Async — record buffer keyed by OVERLAPPED* for GetOverlappedResult to find
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingReads[lpOverlapped] = PendingRead{ lpBuffer, hFile };
    }

    SetLastError(savedErr);
    return result;
}

static BOOL WINAPI GetOverlappedResult_Hook(HANDLE hFile, LPOVERLAPPED lpOverlapped,
    LPDWORD lpNumberOfBytesTransferred, BOOL bWait)
{
    BOOL  result   = GetOverlappedResult_Original(hFile, lpOverlapped,
                                                  lpNumberOfBytesTransferred, bWait);
    DWORD savedErr = GetLastError();

    if (!result || !lpOverlapped) { SetLastError(savedErr); return result; }

    // Pop the pending entry for this OVERLAPPED* — each controller has its own entry
    PendingRead pending{ nullptr, nullptr };
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        auto it = g_pendingReads.find(lpOverlapped);
        if (it != g_pendingReads.end())
        {
            pending = it->second;
            g_pendingReads.erase(it);
            found = true;
        }
    }

    if (found && pending.buffer)
    {
        BYTE* buf = static_cast<BYTE*>(pending.buffer);

        // Determine DualSense report format from report ID + transfer size:
        //   USB:       0x01, 64 bytes → buttons at byte 8
        //   BT Simple: 0x01, 78 bytes → buttons at byte 5
        //   BT Full:   0x31, 78 bytes → buttons at byte 9
        DWORD bytes = lpNumberOfBytesTransferred ? *lpNumberOfBytesTransferred : 0;
        int  buttonByte = -1;
        const char* conn = "?";

        if (buf[0] == 0x31)               { buttonByte = kTriangleButtonByteBTFull;   conn = "BT-Full"; }
        else if (buf[0] == 0x01 && bytes > 64) { buttonByte = kTriangleButtonByteBTSimple; conn = "BT-Simple"; }
        else if (buf[0] == 0x01)          { buttonByte = kTriangleButtonByteUSB;      conn = "USB"; }

        if (buttonByte >= 0)
        {
            g_hasHidController.store(true, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> lock(g_loggedHandlesMutex);
                if (g_gorLoggedHandles.insert(pending.handle).second)
                    _LOG("[HID] DualSense %s handle=%p (async)", conn, pending.handle);
            }

            // Inject triangle during the active time window
            ULONGLONG now = GetTickCount64();
            ULONGLONG deadline = g_injectUntilTick.load(std::memory_order_relaxed);
            if (now < deadline)
            {
                buf[buttonByte] |= kTriangleButtonMask;
                if (buf[0] == 0x31) fixBtFullCrc(buf, bytes);  // BT Full has CRC32
                if (!g_injectLogged.exchange(true, std::memory_order_relaxed))
                    _LOG("[HID] Triangle injected (%s, handle=%p, byte[%d]: 0x%02X)",
                        conn, pending.handle, buttonByte, buf[buttonByte]);
            }
        }
    }

    SetLastError(savedErr);
    return result;
}

static BOOL WINAPI CloseHandle_Hook(HANDLE hObject)
{
    bool wasDualsense = false;
    {
        std::lock_guard<std::mutex> lock(g_dsHandlesMutex);
        auto it = std::find(g_dualsenseHandles.begin(), g_dualsenseHandles.end(), hObject);
        if (it != g_dualsenseHandles.end())
        {
            g_dualsenseHandles.erase(it);
            wasDualsense = true;
        }
    }
    if (wasDualsense)
    {
        {
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            for (auto it = g_pendingReads.begin(); it != g_pendingReads.end(); )
                it = (it->second.handle == hObject) ? g_pendingReads.erase(it) : std::next(it);
        }
        {
            std::lock_guard<std::mutex> lock(g_loggedHandlesMutex);
            g_readFileLoggedHandles.erase(hObject);
            g_gorLoggedHandles.erase(hObject);
        }
    }
    return CloseHandle_Original(hObject);
}

static DWORD WINAPI XInputGetState_Hook(DWORD dwUserIndex, XINPUT_STATE* pState)
{
    DWORD result = XInputGetState_Original(dwUserIndex, pState);
    if (result == ERROR_SUCCESS && pState)
    {
        if (!g_hasHidController.load(std::memory_order_relaxed))
        {
            g_hasHidController.store(true, std::memory_order_relaxed);
            _LOG("[XInput] Xbox controller detected (user index %lu) — Y injection available", dwUserIndex);
        }
        ULONGLONG now = GetTickCount64();
        if (now < g_injectUntilTick.load(std::memory_order_relaxed))
        {
            pState->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
            // SDL skips button processing when dwPacketNumber is unchanged (idle controller).
            // Bump it each call so SDL sees a new state and reads the injected buttons.
            pState->dwPacketNumber = g_xInputFakePacket.fetch_add(1, std::memory_order_relaxed);
            if (!g_injectLogged.exchange(true, std::memory_order_relaxed))
                _LOG("[XInput] Y injected (user index %lu)", dwUserIndex);
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

    // 5. CloseHandle — prune stale DualSense handle entries on close
    auto fnClose = GetProcAddress(kernel32, "CloseHandle");
    if (fnClose) {
        if (MH_CreateHook(fnClose, CloseHandle_Hook,
                reinterpret_cast<LPVOID*>(&CloseHandle_Original)) == MH_OK &&
            MH_EnableHook(fnClose) == MH_OK)
            _LOG("[Hook] CloseHandle hooked");
        else
            _LOG("[Hook] CloseHandle hook FAILED");
    }

    // 6. XInputGetState — Xbox controller (USB + Wireless, both covered by XInput).
    // Must load the real system DLL explicitly: GetModuleHandleA("xinput1_4.dll") would
    // return o-negative's proxy, not the real implementation.
    {
        char sysDir[MAX_PATH];
        GetSystemDirectoryA(sysDir, MAX_PATH);
        std::string realXInputPath = std::string(sysDir) + "\\xinput1_4.dll";
        HMODULE hRealXInput = LoadLibraryA(realXInputPath.c_str());
        FARPROC fnXInput = hRealXInput ? GetProcAddress(hRealXInput, "XInputGetState") : nullptr;
        if (fnXInput) {
            if (MH_CreateHook(fnXInput, XInputGetState_Hook,
                    reinterpret_cast<LPVOID*>(&XInputGetState_Original)) == MH_OK &&
                MH_EnableHook(fnXInput) == MH_OK)
                _LOG("[Hook] XInputGetState hooked (real xinput1_4.dll)");
            else
                _LOG("[Hook] XInputGetState hook FAILED");
        } else {
            _LOG("[Hook] XInputGetState: could not load real xinput1_4.dll from %s", sysDir);
        }
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
                    _LOG("[PipeClient] No controller — using mouse click");
                    requestMouseClick();
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
