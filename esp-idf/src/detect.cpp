/**
 * detect.cpp — is the hardware under this firmware a LilyGo T-Deck Plus?
 *
 * The board's one self-assertion. It answers about THIS board only: its own
 * name when the anchor peripheral and the radio both answer on T-Deck pins, and
 * NULL otherwise. Nothing here enumerates other boards — that comparison belongs
 * to whoever calls it.
 *
 * Two callers, one body:
 *
 *   * spangap-core, at the top of spangapInit(), before any bus is claimed. A
 *     board straddle is staged because the image was built for that board, so a
 *     NULL here means the image is on the wrong hardware and the platform halts
 *     rather than driving someone else's pins for the rest of the boot.
 *   * flashmon's standalone detector, which carries a copy of this function
 *     renamed `detect_hw_lilygo_tdeck` and calls it alongside every other
 *     board's, to identify a chip whose firmware is unknown.
 *
 * The copy is manual and deliberately so — see detect_probe.h. Change this,
 * change flashmon/esp-idf/main/detect.c.
 *
 * The rail: this drives the peripheral power rail, because the keyboard cannot
 * answer without it. It releases the rail ONLY when the probe fails. On success
 * the rail is wanted either way round — the firmware is about to use it (and
 * TdeckBoard::onStart drove it already, so this is idempotent), and the detector
 * is reset into real firmware straight after.
 *
 * The bus: the SX1262 shares SCK/MOSI/MISO with the ST7789 panel and the SD
 * card, and both of those have their MISO wired. So their chip-selects are
 * parked HIGH for the radio probe, on the same terms as the rail — released on
 * failure, left parked on success, where TdeckBoard::onStart parks them again
 * before the SD mount for exactly this reason.
 */
#include "detect_probe.h"
#include "tdeck.h"

/* LoRa header. The pins live in straddle.yaml as CONFIG_LORA0_* — but those
 * symbols only exist when iface-lora is staged, and this probe has to work in an
 * image built without a radio stack at all, so they are written out here. */
#define DETECT_LORA_SCK   40
#define DETECT_LORA_MOSI  41
#define DETECT_LORA_MISO  38
#define DETECT_LORA_CS     9
#define DETECT_LORA_RST   17
#define DETECT_LORA_BUSY  13

/* The two other devices on that same bus, whose CS the radio probe parks.
 * straddle.yaml calls them CONFIG_LCD_CS_PIN and CONFIG_SPANGAP_SDCARD_SPI_PIN_CS,
 * written out here for the same reason as the LoRa pins above. */
#define DETECT_LCD_CS     12
#define DETECT_SD_CS      39

/* GNSS RX. Lives in straddle.yaml as CONFIG_GPS_RX_PIN for the same reason —
 * the symbol only exists when gps is staged. */
#define DETECT_GPS_RX     44

extern "C" const char* detect_hw(void)
{
    /* 16 MB flash or it is not a T-Deck Plus — cheapest possible rejection, and
     * it touches no pin at all. */
    if (!detect_flash_mb(16)) return NULL;

    detect_rail_drive(BOARD_POWER_EN_PIN, BOARD_POWER_EN_ACTIVE ? 1 : 0);
    detect_cs_park(DETECT_LCD_CS);
    detect_cs_park(DETECT_SD_CS);
    vTaskDelay(pdMS_TO_TICKS(150));            /* 3.3 V rail settle */

    /* Anchor: the ESP32-C3 QWERTY keyboard. Unique to this board among the ones
     * spangap knows, and a plain ACK is all it offers — it has no ID register.
     * POLLED, not probed once: the keyboard is its own MCU booting its own
     * firmware off this rail, and from a cold rail it takes several hundred ms
     * to reach its I2C loop — one ACK attempt at 150 ms reads a healthy board
     * as absent. A warm board still answers on the first try. */
    bool kb = false;
    for (int i = 0; i < 12; i++) {
        if ((kb = detect_ack(BOARD_TOUCH_I2C_SDA, BOARD_TOUCH_I2C_SCL, BOARD_KB_ADDR)))
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!kb) {
        detect_miss("no keyboard at 0x%02X — not a T-Deck", BOARD_KB_ADDR);
        detect_cs_release(DETECT_SD_CS);
        detect_cs_release(DETECT_LCD_CS);
        detect_rail_release(BOARD_POWER_EN_PIN);
        return NULL;
    }
    /* Confirm with the radio: a keyboard alone could be a bare C3 on a bench. */
    if (!detect_radio(DETECT_LORA_SCK, DETECT_LORA_MOSI, DETECT_LORA_MISO,
                      DETECT_LORA_CS, DETECT_LORA_RST, DETECT_LORA_BUSY, NULL)) {
        detect_miss("keyboard answered but no radio — not a T-Deck");
        detect_cs_release(DETECT_SD_CS);
        detect_cs_release(DETECT_LCD_CS);
        detect_rail_release(BOARD_POWER_EN_PIN);
        return NULL;
    }

#if DETECT_EXTRAS
    /* Fitted either way round, so they confirm nothing and are logged for the
     * person reading rather than tested: touch, the audio ADC, the RTC, the GNSS
     * receiver. The Plus has GNSS and the plain T-Deck does not, and both are
     * this straddle. The GNSS autobaud alone listens 1.2 s per rate, which is
     * why none of this runs in the firmware — see DETECT_EXTRAS. */
    detect_gt911(BOARD_TOUCH_I2C_SDA, BOARD_TOUCH_I2C_SCL);
    detect_es7210(BOARD_TOUCH_I2C_SDA, BOARD_TOUCH_I2C_SCL);
    detect_ack(BOARD_TOUCH_I2C_SDA, BOARD_TOUCH_I2C_SCL, 0x51);      /* PCF8563 RTC */
    detect_gps(DETECT_GPS_RX, NULL);
#endif

    detect_found("hw_lilygo_tdeck");
    return "hw-lilygo-tdeck";
}
