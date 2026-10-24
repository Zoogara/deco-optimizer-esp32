#include "tmp_proto.h"
#include <string.h>
#include "esp_rom_crc.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }

size_t tmp_build_frame(uint8_t *out, uint32_t seq, const uint8_t *payload, size_t plen)
{
    const uint8_t h12[8] = {0x01, 0x01, 0x05, 0x00, plen >> 8, plen, 0x00, 0x00};
    memcpy(out, h12, 8);
    put32(out + 8, seq);
    // checksum field counts as zeros while computing
    uint8_t zero[4] = {0};
    uint32_t crc = esp_rom_crc32_le(TMP_CHK_SEED, out, 12);
    crc = esp_rom_crc32_le(crc, zero, 4);
    crc = esp_rom_crc32_le(crc, payload, plen);
    put32(out + 12, crc);
    memcpy(out + 16, payload, plen);
    return 16 + plen;
}

size_t tmp_build_app(uint8_t *out, uint16_t opcode, uint16_t app_seq, const char *json)
{
    size_t jlen = strlen(json);
    put16(out, 0x0102);
    put16(out + 2, opcode);
    out[4] = 0x02;  // request
    out[5] = 0x00;
    put16(out + 6, app_seq);
    put32(out + 8, esp_rom_crc32_le(0, (const uint8_t *)json, jlen));  // standard CRC-32
    put32(out + 12, jlen);
    put32(out + 16, 0);
    memcpy(out + 20, json, jlen);
    return 20 + jlen;
}
