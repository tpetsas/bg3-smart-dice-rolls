/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 *
 * Pipe client for communicating with the PixelsTray RollServer.
 * Sends roll requests, receives results, and auto-launches the tray app.
 */

#pragma once

#include <cstdint>
#include <string>

namespace PixelsDiceClient
{
    // start the background pipe client thread (connects to tray app).
    void start();

    // stop the pipe client thread and close the pipe.
    void stop();

    // queue a roll request to be sent to the tray app.
    // The result will be written into g_smartDiceResult when ready.
    void notifyTrayForRoll(const char* mode, uint32_t generation);
}
