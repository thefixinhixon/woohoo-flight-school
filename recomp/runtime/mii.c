/* Default Mii for the system Mii database.
 * A real console always has at least the owner's Mii; the game formats an empty database when none exists, and its
 * Mii selection screen then has nothing to pick. When the game opens shared extdata 0xF000000B/CFL_DB.dat and all
 * 100 slots are empty, slot 0 gets one standard Mii. PWR_NO_DEFAULT_MII=1 turns this off.
 * CFL_DB.dat (seen in the file the game formats): "CFOG" + u32 at 0, 100 Mii slots of 0x5C bytes at 8, "CFHE" at
 * 0x23F8; CRC16-CCITT (poly 0x1021, init 0) of 0..0xC81E stored big-endian at 0xC81E (the face library reformats the
 * database when it does not match); "CFRA" at 0xC820 with its own CRC at 0xE4BE (untouched here).
 * Mii slot layout (0x5C bytes, 3DS Mii format as documented on 3dbrew; inferred, field bit positions not checked
 * against the game's reader): version, flags, position, origin, system ID, Mii ID (big endian), MAC, then gender/
 * birthday/colour, UTF-16 name, height/build, face/hair/eyes/eyebrows/nose/mouth/beard/glasses/mole, UTF-16 author. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_SIZE 310560
#define SLOT0 8
#define SLOT_SIZE 0x5C
#define CRC_AT 0xC81E

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= (uint32_t)p[i] << 8; for (int k = 0; k < 8; k++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; }
    return (uint16_t)c;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void utf16(uint8_t *p, const char *s) { for (int i = 0; s[i] && i < 10; i++) put16(p + 2 * i, (uint8_t)s[i]); }

static void default_mii(uint8_t *m) {
    static const uint8_t sysid[8] = { 0x50, 0x57, 0x52, 0x00, 0x00, 0x00, 0x00, 0x01 };
    static const uint8_t mac[6] = { 0x00, 0x1F, 0x32, 0x50, 0x57, 0x52 };
    memset(m, 0, SLOT_SIZE);
    m[0x00] = 3;                      /* version */
    m[0x01] = 0x01;                   /* copying allowed */
    m[0x02] = 0x00;                   /* page 0, slot 0 */
    m[0x03] = 3 << 4;                 /* made on a 3DS */
    memcpy(m + 0x04, sysid, 8);
    m[0x0C] = 0x94; m[0x0D] = 0xAB; m[0x0E] = 0x12; m[0x0F] = 0xCD;   /* Mii ID, big endian: normal Mii + creation time */
    memcpy(m + 0x10, mac, 6);
    put16(m + 0x18, 0u | 1u << 1 | 1u << 5 | 4u << 10);              /* male, born 1 January, favourite colour blue */
    utf16(m + 0x1A, "Pilot");
    m[0x2E] = 64; m[0x2F] = 64;       /* height, build */
    m[0x30] = 0;                      /* sharing on, face shape 0, skin 0 */
    m[0x31] = 0;                      /* no wrinkles / makeup */
    m[0x32] = 33;                     /* hair style */
    m[0x33] = 1;                      /* hair colour */
    put32(m + 0x34, 2u | 0u << 6 | 4u << 9 | 3u << 13 | 4u << 16 | 2u << 21 | 12u << 25);   /* eyes */
    put32(m + 0x38, 6u | 1u << 5 | 4u << 8 | 3u << 12 | 6u << 16 | 2u << 21 | 10u << 25);   /* eyebrows */
    put16(m + 0x3C, 1u | 4u << 5 | 9u << 9);                         /* nose */
    put16(m + 0x3E, 23u | 0u << 6 | 4u << 9 | 3u << 13);             /* mouth */
    put16(m + 0x40, 13u);                                            /* mouth position, no mustache */
    put16(m + 0x42, 0u | 4u << 6 | 10u << 10);                       /* no beard */
    put16(m + 0x44, 0u | 4u << 7 | 10u << 11);                       /* no glasses */
    put16(m + 0x46, 0u | 4u << 1 | 2u << 5 | 20u << 10);             /* no mole */
    utf16(m + 0x48, "PWR");
}

void mii_seed_db(const char *path) {
    if (getenv("PWR_NO_DEFAULT_MII")) return;
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    static uint8_t db[DB_SIZE];
    size_t n = fread(db, 1, sizeof db, f);
    if (n == DB_SIZE && !memcmp(db, "CFOG", 4)) {
        int empty = 1;
        for (int i = 0; i < 100 * SLOT_SIZE && empty; i++) empty = db[SLOT0 + i] == 0;
        if (empty) {
            default_mii(db + SLOT0);
            uint16_t c = crc16(db, CRC_AT);
            db[CRC_AT] = (uint8_t)(c >> 8); db[CRC_AT + 1] = (uint8_t)c;
            fseek(f, 0, SEEK_SET); fwrite(db, 1, CRC_AT + 2, f);
            fprintf(stderr, "pwr: Mii database was empty; added a default Mii\n");
        }
    }
    fclose(f);
}
