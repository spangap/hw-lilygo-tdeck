/**
 * reticulous — ESP-IDF entry point.
 *
 * spangap-core does the foundational platform startup (fs/storage/log/cli/
 * cron/pm). This file then brings up the net foundation (tls/net/ntp/mdns)
 * and a few central straddles (acme/ota/web/lcd) explicitly, and calls
 * spangapInitStraddles() — the build-generated dispatcher that runs the
 * `init:` hook of every staged straddle that declares one (wg/upnp/duckdns/
 * maps by default, plus anything pulled in with --include). Reticulous task
 * inits + LCD program registration follow, then spangapPostAppInit()
 * finalises boot.
 *
 * Board hardware bring-up is owned by tdeck.cpp and straddles spangapInit():
 * tdeckPreInit() (power rail, shared-SPI CS park, reset-on-off hook, display/
 * touch/pointer HAL) before it; tdeckPostInit() (keyboard) after it.
 *
 * Transports (tcp, auto, lora, espnow) self-register with rnsd via ITS
 * on init — rnsd has no compile-time knowledge of which transports exist.
 *
 * See docs/component-plan.md for the full architecture.
 */
#include "spangap.h"
#include "tdeck.h"

/* Net foundation + the few central straddles this file still drives by hand. */
#include "net.h"
#include "tls.h"
#include "ntp.h"
#include "spangap_mdns.h"
#if CONFIG_SPANGAP_WEB
#include "web.h"
#include "webrtc_task.h"
#endif
#if CONFIG_SPANGAP_OTA
#include "ota.h"
#endif
#if CONFIG_SPANGAP_ACME
#include "acme.h"
#endif
#if CONFIG_SPANGAP_LCD
#include "lcd.h"
#endif

/* wg / upnp / duckdns / maps are not referenced in this file at all. Each
 * declares an `init:` hook in its straddle.yaml and is brought up by the
 * generated spangapInitStraddles() dispatcher; each self-registers its own LCD
 * UI from inside that init (maps its launcher tile, the others their settings
 * panes). So they can be added or `--exclude`d with no edit here. */

#include "rnsd.h"
#include "lxmf.h"
#include "nomad.h"
#include "auto.h"
#include "tcp.h"
#include "lora.h"
#include "espnow.h"
#include "gps.h"

namespace {

/* /state/net_up runs on NET_EV_UPSTREAM_UP. Used to live inside
 * spangap-core's spangapInit(), moved here when core stopped knowing about
 * net. spawnTask + cliRunFile + fsStatePath are core APIs (compat/cli/fs). */
void netUpTask(void*) {
    cliRunFile(fsStatePath("/net_up").c_str());
    killSelf();
}

void onNetUp(const char*) {
    spawnTask(netUpTask, "net_up", 4096, nullptr, 1, 1);
}

}  // namespace

extern "C" void app_main(void)
{
    /* Board prerequisites BEFORE spangapInit(): its fs_mount_sd() is the first
     * shared-bus access and needs the peripheral power rail up + idle CS parked;
     * its lcdInit() needs the display/touch HAL already registered. tdeckPreInit()
     * also installs the reset-on-off peripheral power-off hook. */
    tdeckPreInit();

    spangapInit();

    /* Keyboard owns its I2C/indev/ISR and needs the lcd task (lcdRun), so it
     * comes up after spangapInit() — not via the board HAL like touch/trackball. */
    tdeckPostInit();

    /* ── spangap sibling straddles ─────────────────────────────────────
     * Order matches what spangap-core's old spangapInit() used to do
     * before the straddle split. */
    tlsInit();
    netInit();
    ntpInit();
    ntpApplyTimezone();                   /* logs now in persisted local TZ */
    mdnsInit();
#if CONFIG_SPANGAP_ACME
    acmeInit();
#endif
#if CONFIG_SPANGAP_OTA
    otaInit();
#endif
#if CONFIG_SPANGAP_WEB
    webInit();
    storageInit();                        /* after webInit — registers with web */
    webrtcInit();
#else
    storageInit();                        /* no web → just bring storage up directly */
#endif
    cronInit();

    /* Auto-init every staged straddle that declares an `init:` hook in its
     * straddle.yaml — wg, upnp, duckdns, maps by default; anything pulled in
     * with `--include` (e.g. spangap/sshd) too. Generated dispatcher; adding
     * such a straddle needs no edit here. Runs after the net+storage+cron
     * foundation, before the reticulous task graph. */
    spangapInitStraddles();

#if CONFIG_SPANGAP_LCD
    lcdInit();                            /* after storage+web — status bar reads them */
#endif

    /* Boot-script runner on first STA-internet event. Register here (not in
     * the consumer task graph below) so a fast upstream-up doesn't miss it. */
    netRegister(NET_EV_UPSTREAM_UP, onNetUp);

    /* ── reticulous task graph ─────────────────────────────────────────*/
    rnsdInit();
    lxmfInit();
    nomadInit();                          // Nomad Network page client (browser half)

    // Reticulum transports (each self-registering with rnsd via ITS):
    tcpInit();
    autoInit();
    loraInit();
    espnowInit();

    gpsInit();                            // GNSS receiver task (T-Deck Plus)

#if CONFIG_SPANGAP_LCD
    lxmfLcdRegister();                    /* LXMF */
    nomadLcdRegister();                   /* Nomad Network browser */
    /* maps registers its own launcher program inside mapsInit() (run above by
     * spangapInitStraddles) — nothing to wire here. */
#endif

    spangapPostAppInit();
}
