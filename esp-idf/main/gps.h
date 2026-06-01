/**
 * gps — GNSS receiver task (T-Deck Plus).
 *
 * Reads NMEA off the on-board receiver (Quectel L76K @ 9600 or u-blox
 * MIA-M10Q @ 38400, batch-dependent — autobauded), parses every fix it can,
 * and republishes a full snapshot into ephemeral gps.* every s.gps.interval
 * seconds. Gated by s.gps.enable. See gps.cpp for the wire/parse details.
 */
#pragma once
void gpsInit(void);
