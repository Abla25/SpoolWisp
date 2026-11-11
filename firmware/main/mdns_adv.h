#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the Bonjour/mDNS advertisement for the print server.
 *
 * Must be called once an IP address is available (i.e. after the Wi-Fi
 * station got its lease, or after the setup portal brought up its AP).
 * Advertises:
 *   - _pdl-datastream._tcp on the raw (JetDirect) port, so macOS/Linux
 *     discover it as a socket printer;
 *   - _printer._tcp (LPD) on the LPD port;
 *   - the host name "spoolwisp.local".
 */
void mdns_adv_start(void);

/**
 * Rename the advertised printer to the model actually connected over USB.
 *
 * Called with the MFG:/MDL: fields of the printer's IEEE-1284 device ID.
 * Works when the printer was already attached at boot and also when it is
 * hot-plugged later: the mDNS records are updated and re-announced.
 */
void mdns_adv_set_printer(const char *manufacturer, const char *model);

/**
 * Revert to the generic "SpoolWisp" name when the USB printer is unplugged,
 * so the advertisement always describes what is really connected.
 */
void mdns_adv_clear_printer(void);

#ifdef __cplusplus
}
#endif
