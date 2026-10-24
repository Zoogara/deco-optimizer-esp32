// First-run / recovery setup: the board's own Wi-Fi access point with a captive portal that collects the
// Wi-Fi network and the web admin password. Everything else is configured on the normal web page afterwards.
#pragma once
void provision_start(void);        // returns once the AP, DNS responder and HTTP server are running
