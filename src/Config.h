/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#pragma once

class Config
{
public:
    bool  isDebugMode   = false;
    float clickNormX    = 0.50f;  // normalized X of the dice button (normal roll)
    float clickNormY    = 0.43f;  // normalized Y of the dice button
    Config() : isDebugMode(false) {};
    Config(const char *iniPath);
    void print();
};
