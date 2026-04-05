/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#pragma once

class Config
{
public:
    bool isDebugMode = false;
    Config() : isDebugMode(false) {};
    Config(const char *iniPath);
    void print();
};
