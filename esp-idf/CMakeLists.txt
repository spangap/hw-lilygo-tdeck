# hw-lilygo-tdeck board HAL — an ordinary spangap component (no longer a buildable).
#
# Compiles the LilyGo T-Deck Plus board code: power/CS/reset + the display/
# touch/trackball/button input HAL + QWERTY keyboard (tdeck.cpp). The GNSS
# receiver is the generic spangap/gps straddle (this board only stages
# it and supplies its CONFIG_GPS_* pins). The on-device-UI input HAL lives in
# conditional/spangap-lcd/ and is compiled ONLY when spangap-lcd is staged.
#
# app_main is NOT here — it's generated into the reticulous/reticulous buildable
# that adds this board with `--with`. This component just exports the board's
# Service classes that the generated dispatcher constructs and walks.
include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
idf_component_register(
    SRCS
        "src/tdeck.cpp"
        # This board's one self-assertion (detect_hw), called by
        # spangapInit() before any bus is claimed.
        "src/detect.cpp"
        # conditional/<name>/ board code, compiled only when <name> is staged
        # (conditional/spangap-lcd/src/tdeck_lcd.cpp).
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    REQUIRES
        ${SPANGAP_REQUIRES}
        driver
        esp_adc          # battery voltage ADC on GPIO 4 (tdeck task)
        esp_timer
        esp_event
        esp_netif
        esp_wifi
        lwip
    PRIV_REQUIRES
        # GT911 capacitive touch for the conditional/spangap-lcd/ input HAL.
        # Fetched via reticulous/reticulous's main/idf_component.yml (the manager
        # doesn't recurse into staged components); declared here for locality.
        esp_lcd_touch_gt911
)
