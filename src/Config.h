/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#pragma once

class Config
{
public:
    bool isDebugMode = false;
    float mouseClickNormX = 0.50f;  // normalized X position of the dice roll button
    float mouseClickNormY = 0.50f;  // normalized Y position of the dice roll button
    Config() = default;
    Config(const char *iniPath);
    void print();
};
