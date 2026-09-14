/**
 * tdeck_lcd.h — the T-Deck's on-device input HAL, as a Service.
 *
 * TdeckLcdInput brackets spangap-lcd's bring-up: onStart (start band) registers
 * the touch/trackball/button input HAL before lcdInit(); onInit (init band)
 * brings up the QWERTY keyboard once the lcd task exists. when:-gated on
 * spangap-lcd via the straddle.yaml `services:` entry, so the whole slice (this
 * header + tdeck_lcd.cpp) compiles only in an LCD build. Declared here (global)
 * for the generated trampoline; defined in tdeck_lcd.cpp.
 */
#pragma once

#include "service.h"

class TdeckLcdInput : public Service {
public:
    void onStart() override;   /* input HAL register, before lcdInit() */
    void onInit() override;    /* keyboard bring-up, once the lcd task exists */
};
