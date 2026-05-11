/*
 * Copyright (C) 2024 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#include "Config.h"
#include "Logger.h"
#include <string>
#include <windows.h>

/*
 * sample INI content:
 *
 * [app]
 * debug=true
 *
 * [display]
 * ; Normalized position (0.0-1.0) of the dice roll button within the game window.
 * ; Default values are calibrated for 16:9. Adjust if the click lands off-target.
 * ; Advantage/disadvantage X is derived automatically (click_norm_x - 0.03).
 * click_norm_x=0.50
 * click_norm_y=0.43
 *
 */

static float readFloat(const char* iniPath, const char* section, const char* key, float defaultVal)
{
    char buf[64] = "";
    char defStr[64];
    snprintf(defStr, sizeof(defStr), "%f", defaultVal);
    GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), iniPath);
    try { return std::stof(buf); }
    catch (...) { return defaultVal; }
}

Config::Config (const char *iniPath) {
    char value[255] = "";

    GetPrivateProfileStringA("app", "debug", "false", value, sizeof(value), iniPath);
    isDebugMode = strcmp(value, "true") == 0;

    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
        _LOG("%s is not an INI file; using config defaults...", iniPath);
        isDebugMode = false;
        return;
    }

    clickNormX = readFloat(iniPath, "display", "click_norm_x", 0.50f);
    clickNormY = readFloat(iniPath, "display", "click_norm_y", 0.43f);

    memset(value, 0, sizeof(value));
}

void Config::print() {
    _LOG("Config: [debug mode: %s] [click_norm: %.3f, %.3f]",
        isDebugMode ? "true" : "false",
        clickNormX, clickNormY
    );
}
