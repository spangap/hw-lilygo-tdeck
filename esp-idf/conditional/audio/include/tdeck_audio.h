/**
 * tdeck_audio.h — the T-Deck's audio codec bring-up, as a Service.
 *
 * TdeckAudio::onInit registers the board's I2S codec with the audio straddle.
 * when:-gated on spangap/audio via the straddle.yaml `services:` entry, so this
 * slice (header + tdeck_audio.cpp) compiles only when audio is staged. Declared
 * here (global) for the generated trampoline; defined in tdeck_audio.cpp.
 */
#pragma once

#include "service.h"

class TdeckAudio : public Service {
public:
    void onInit() override;   /* was tdeckAudioInit */
};
