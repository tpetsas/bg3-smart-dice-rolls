/*
 * Copyright (C) 2026 Thanasis Petsas <thanpetsas@gmail.com>
 * Licence: MIT Licence
 */

#pragma once

#include <cstdint>

struct SmartDiceResult
{
    bool ready = false;          // tray app has provided a result
    uint32_t generation = 0;     // which dialogue-roll attempt this belongs to

    uint32_t die1 = 0;           // normal: use die1 only
    uint32_t die2 = 0;           // advantage/disadvantage: second die
};
