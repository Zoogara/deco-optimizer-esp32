// Which Deco node is this device connected to? Reads the mesh node list from the Deco's local web API
// (plain HTTP, RSA-signed and AES-encrypted), once per boot, and keeps a few fields per node.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DECO_MAX_NODES 12

typedef struct {
    char name[32];          // the name given in the Deco app (custom_nickname, base64 in the reply) or the plain nickname
    char model[16];         // e.g. M5, E4R
    char role[8];           // master or slave
    uint8_t mac[6];
    uint8_t bssid_2g[6];    // the node's radios: these are what a client's BSSID is matched against
    uint8_t bssid_5g[6];
} deco_node_t;

// Best effort; logs what happened and changes nothing else. Blocking (a few seconds). Call from the worker task only.
void deco_nodes_fetch(void);
int deco_nodes_count(void);
// Finds the node whose radio (or MAC) is the given BSSID. False if the list is not loaded or nothing matches.
bool deco_node_for_bssid(const uint8_t bssid[6], deco_node_t *out);
// Loads the node table saved by the last successful read, so names are known from boot. Call after nvs_flash_init().
void deco_nodes_load_cache(void);
// "Name (Model)" for the node with this BSSID, or "" if unknown.
void deco_node_label(const uint8_t bssid[6], char *out, size_t n);
