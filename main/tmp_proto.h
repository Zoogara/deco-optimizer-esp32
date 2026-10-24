// TMP App-V2 framing for the TP-Link Deco (see PROTOCOL.md).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define TMP_CHK_SEED 0xE63EB2DEu  // 0x19C14D21 ^ 0xFFFFFFFF, passed as the "previous crc"

// Builds a transport frame around payload into out; returns total length.
size_t tmp_build_frame(uint8_t *out, uint32_t seq, const uint8_t *payload, size_t plen);
// Builds an App-V2 request payload (20-byte header + json); returns length.
size_t tmp_build_app(uint8_t *out, uint16_t opcode, uint16_t app_seq, const char *json);
