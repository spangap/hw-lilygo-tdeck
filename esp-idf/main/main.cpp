/**
 * reticulous — ESP-IDF entry point.
 *
 * Almost everything this device runs is a staged straddle that declares an
 * `init:` hook in its straddle.yaml, so app_main is now tiny: it does the
 * board-specific bring-up that can't be a straddle, calls spangapInit() for
 * the platform core, then calls the build-generated spangapInitStraddles()
 * dispatcher — which runs every staged straddle's init in (order, dependency)
 * order: the net foundation (tls/net/ntp/mdns), the services (acme/ota/web/
 * storage/webrtc/cron/sshd/wg/upnp/duckdns), the LCD launcher, then the RNS
 * stack (rnsd → transports → lxmf/nomad). Each straddle that has on-device UI
 * self-registers its LCD tile / Settings pane from inside its own init, so
 * none of them need a call site here. Adding or `--exclude`ing a straddle
 * never touches this file.
 *
 * What's left in app_main is only what isn't a staged straddle:
 *   - tdeckPreInit()/tdeckPostInit() — the board HAL (power rail, shared-SPI
 *     CS park, display/touch/pointer/keyboard).
 *   - gpsInit() — the T-Deck Plus GNSS receiver task, board-specific until a
 *     GPS service abstraction earns its own straddle.
 *
 * Transports (tcp/auto/lora/espnow) self-register with rnsd via ITS on init —
 * rnsd has no compile-time knowledge of which transports exist.
 *
 * See docs/component-plan.md for the full architecture.
 */
#include "spangap.h"
#include "tdeck.h"
#include "gps.h"

extern "C" void app_main(void)
{
    /* Board prerequisites BEFORE spangapInit(): its fs_mount_sd() is the first
     * shared-bus access and needs the peripheral power rail up + idle CS parked;
     * its lcdInit() needs the display/touch HAL already registered. */
    tdeckPreInit();

    spangapInit();

    /* Keyboard owns its I2C/indev/ISR and needs the lcd task (lcdRun), so it
     * comes up after spangapInit() — not via the board HAL like touch/trackball. */
    tdeckPostInit();

    /* Bring up every staged straddle that declares an `init:` hook, in
     * (order, dependency-topo) order: net foundation → services → LCD → RNS
     * stack. This single call replaces the long hand-written init sequence
     * app_main used to carry; adding/removing a straddle needs no edit here. */
    spangapInitStraddles();

    /* GNSS receiver task — board hardware, not a staged straddle (yet). */
    gpsInit();

    spangapPostAppInit();
}
