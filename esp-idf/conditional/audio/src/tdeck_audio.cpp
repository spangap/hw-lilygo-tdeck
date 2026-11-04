/**
 * tdeck_audio — T-Deck Plus ES7210 mic control shim for spangap/audio.
 *
 * The spangap/audio engine owns the I2S read/write engine; a board contributes
 * only Kconfig pins and, for an I2C-controlled input codec, this slice. The
 * ES7210 is a quad-channel mic ADC on the shared I2C0 bus (addr 0x40) running
 * as an I2S *slave*: the S3 (audio engine, I2S0 master) supplies MCLK/BCLK/WS,
 * the ES7210 just clocks ADC samples onto DIN once its registers are programmed.
 * So in_init() is pure I2C register programming — no I2S here.
 *
 * Compiled and linked ONLY when spangap/audio is staged (it lives under
 * esp-idf/conditional/audio/), and reached through the `when: spangap/audio`
 * TdeckAudio service, whose onInit just stows the ops pointer with the engine.
 *
 * The register table follows the vendor's ES7210 bring-up (Espressif's driver
 * as LilyGo ships it for this board): 16-bit I2S slave, all four PGAs at
 * 37.5 dB, HPF on. The board's one microphone is MIC1, so the audio is ADC1 in
 * the left slot of SDOUT1, and CLOCK_OFF (reg 0x01) must end at 0x00: with
 * 0x14 left in it ADC1 gets no clock and the capture is near-silence (Opus
 * frames of ~16 B on speech). The serial clocks come from BCLK/WS; the ADC runs
 * off MCLK through MAINCLK (reg 0x02), and with the engine's MCLK at 256 fs,
 * MAINCLK 0xC1 (DLL, doubler, divide by 1) and OSR 32 give it 512 fs at any
 * rate, so the sequence is rate-independent.
 */
#include "audio.h"
#include "tdeck.h"
#include "tdeck_audio.h"
#include "log.h"
#include "driver/i2c_master.h"
#include <cstdint>

#define ES7210_ADDR  0x40

static i2c_master_dev_handle_t s_dev = nullptr;

static i2c_master_dev_handle_t es7210Dev(void) {
  if (s_dev) return s_dev;
  i2c_master_bus_handle_t bus = tdeckI2cBus();
  if (!bus) return nullptr;
  i2c_device_config_t dcfg = {};
  dcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dcfg.device_address  = ES7210_ADDR;
  dcfg.scl_speed_hz    = 100000;
  if (i2c_master_bus_add_device(bus, &dcfg, &s_dev) != ESP_OK) {
    warn("es7210: i2c add device failed (no ES7210?)\n");
    s_dev = nullptr;
  }
  return s_dev;
}

static bool es7210Write(uint8_t reg, uint8_t val) {
  i2c_master_dev_handle_t dev = es7210Dev();
  if (!dev) return false;
  uint8_t buf[2] = { reg, val };
  return i2c_master_transmit(dev, buf, sizeof(buf), 50) == ESP_OK;
}

/* ES7210 16-bit I2S-slave init: reset with the clocks gated, HPF, analog
 * bias, the ADC clock, the serial port, all four mics at a fixed gain, then
 * the clocks and power on. */
static const uint8_t kEs7210Init[][2] = {
  {0x00, 0xFF},  /* RESET: assert reset on all blocks */
  {0x00, 0x41},  /* RESET: release; core powered */
  {0x01, 0x3F},  /* CLOCK_OFF: everything gated while it is set up */
  {0x09, 0x30},  /* TCT0: chip initial-state period */
  {0x0A, 0x30},  /* TCT1: power-up state period */
  {0x23, 0x2A},  /* ADC12 HPF stage2 */
  {0x22, 0x0A},  /* ADC12 HPF stage1 */
  {0x20, 0x0A},  /* ADC34 HPF stage2 */
  {0x21, 0x2A},  /* ADC34 HPF stage1 */
  {0x08, 0x10},  /* MODE_CFG: I2S slave */
  {0x40, 0x43},  /* ANALOG: VDDA 3.3 V, VMID 5 kΩ start */
  {0x41, 0x70},  /* MIC1/2 bias 2.87 V */
  {0x42, 0x70},  /* MIC3/4 bias 2.87 V */
  {0x07, 0x20},  /* ADC OSR 32 */
  {0x02, 0xC1},  /* MAINCLK: DLL, doubler, ADC div 1 — ADC clock 2 × MCLK (= 512 fs at MCLK 256 fs) */
  {0x11, 0x60},  /* SDP_CFG1: I2S format, 16-bit word length */
  {0x12, 0x00},  /* SDP_CFG2: ADC1/2 on SDOUT1, ADC3/4 on SDOUT2, no TDM */
  {0x43, 0x1E},  /* MIC1 PGA on, 37.5 dB */
  {0x44, 0x1E},  /* MIC2 PGA on, 37.5 dB */
  {0x45, 0x1E},  /* MIC3 PGA on, 37.5 dB */
  {0x46, 0x1E},  /* MIC4 PGA on, 37.5 dB */
  {0x4B, 0x00},  /* MIC1/2 bias, ADC and PGA powered */
  {0x4C, 0x00},  /* MIC3/4 bias, ADC and PGA powered */
  {0x01, 0x00},  /* CLOCK_OFF: every clock running (0x14 starves ADC1: silence) */
  {0x06, 0x00},  /* POWER_DOWN: nothing powered down */
  {0x47, 0x08},  /* MIC1 power */
  {0x48, 0x08},  /* MIC2 power */
  {0x49, 0x08},  /* MIC3 power */
  {0x4A, 0x08},  /* MIC4 power */
};

static uint32_t es7210InInit(uint32_t rate) {
  if (!es7210Dev()) return 0;
  for (auto& kv : kEs7210Init) {
    if (!es7210Write(kv[0], kv[1])) { warn("es7210: i2c write failed\n"); return 0; }
  }
  info("es7210: configured (16-bit I2S slave)\n");
  return rate;   /* slave mode: the chip clocks to whatever the engine drives */
}

static void es7210InDeinit(void) {
  es7210Write(0x06, 0xFF);   /* DIGITAL_PDN: power the ADC blocks down */
  es7210Write(0x01, 0x7F);   /* CLOCK_OFF */
}

static const audio_codec_ops_t es7210_ops = {
  es7210InInit, es7210InDeinit, nullptr, nullptr,
};

/* onInit — register the board codec with the audio straddle. Service gated
 * when: spangap/audio (whole slice compiles only when audio is staged). */
void TdeckAudio::onInit() {
  audioRegisterCodec(&es7210_ops);
}
