/*
 * LAN save sync — crypto for the direct vault route (VPS only mode):
 * HMAC-SHA256 and AES-256-GCM, compatible with the PC client's Vault
 * (pc/shallot_sync.py, Python "cryptography"). Included from main.c after
 * lansync_core.inc.c. Plain portable C: byte-oriented AES, bitwise GHASH;
 * fast enough for a few new 4 KiB blocks per save version.
 */

static void ls_sha256_final_bin(ls_sha256_t *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, zero = 0, lenbe[8];
    ls_sha256_update(c, &pad, 1);
    while (c->used != 56) ls_sha256_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) lenbe[i] = (uint8_t)(bits >> (56 - 8 * i));
    ls_sha256_update(c, lenbe, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

static void ls_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
}

static int ls_unhex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (!isxdigit((unsigned char)s[2 * i]) || !isxdigit((unsigned char)s[2 * i + 1]) || sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* HMAC-SHA256 over the concatenation of up to two pieces. */
static void ls_hmac_sha256_2(const uint8_t *key, size_t klen, const void *m1, size_t l1, const void *m2, size_t l2, uint8_t out[32]) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[32];
    if (klen > 64) { ls_sha256_t c; ls_sha256_init(&c); ls_sha256_update(&c, key, klen); ls_sha256_final_bin(&c, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    ls_sha256_t c;
    ls_sha256_init(&c);
    ls_sha256_update(&c, ipad, 64);
    if (l1) ls_sha256_update(&c, m1, l1);
    if (l2) ls_sha256_update(&c, m2, l2);
    ls_sha256_final_bin(&c, inner);
    ls_sha256_init(&c);
    ls_sha256_update(&c, opad, 64);
    ls_sha256_update(&c, inner, 32);
    ls_sha256_final_bin(&c, out);
}
static void ls_hmac_sha256(const uint8_t *key, size_t klen, const void *m, size_t l, uint8_t out[32]) {
    ls_hmac_sha256_2(key, klen, m, l, NULL, 0, out);
}

/* ── AES-256 (encryption direction only; GCM never decrypts blocks) ── */
static const uint8_t ls_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

typedef struct { uint8_t rk[240]; } ls_aes256_t;

static uint8_t ls_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void ls_aes256_init(ls_aes256_t *a, const uint8_t key[32]) {
    memcpy(a->rk, key, 32);
    uint8_t rcon = 1, t[4];
    for (int i = 32; i < 240; i += 4) {
        memcpy(t, a->rk + i - 4, 4);
        if (i % 32 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(ls_aes_sbox[t[1]] ^ rcon); t[1] = ls_aes_sbox[t[2]]; t[2] = ls_aes_sbox[t[3]]; t[3] = ls_aes_sbox[u];
            rcon = ls_xtime(rcon);
        } else if (i % 32 == 16) {
            for (int k = 0; k < 4; k++) t[k] = ls_aes_sbox[t[k]];
        }
        for (int k = 0; k < 4; k++) a->rk[i + k] = (uint8_t)(a->rk[i - 32 + k] ^ t[k]);
    }
}

static void ls_aes256_encrypt(const ls_aes256_t *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ a->rk[i]);
    for (int r = 1; r <= 14; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = ls_aes_sbox[s[i]];
        /* ShiftRows (column-major state) */
        s[0] = t[0]; s[4] = t[4]; s[8] = t[8]; s[12] = t[12];
        s[1] = t[5]; s[5] = t[9]; s[9] = t[13]; s[13] = t[1];
        s[2] = t[10]; s[6] = t[14]; s[10] = t[2]; s[14] = t[6];
        s[3] = t[15]; s[7] = t[3]; s[11] = t[7]; s[15] = t[11];
        if (r != 14) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ ls_xtime((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ ls_xtime((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ ls_xtime((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ ls_xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* ── GCM ──────────────────────────────────────────────────────────── */
static void ls_gf_mul(uint8_t x[16], const uint8_t y[16]) {
    uint8_t z[16] = {0}, v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7))) for (int k = 0; k < 16; k++) z[k] ^= v[k];
        int lsb = v[15] & 1;
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}

static void ls_ghash_update(uint8_t s[16], const uint8_t h[16], const uint8_t *d, size_t n) {
    while (n) {
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++) s[i] ^= d[i];
        ls_gf_mul(s, h);
        d += k; n -= k;
    }
}

/* GCM with a 12-byte nonce. dir 1: encrypt in -> out, writes tag; dir 0:
 * checks tag, then decrypts. Returns 0, or -1 on a bad tag. */
static int ls_gcm(const ls_aes256_t *a, const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                  const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[16], int dir) {
    uint8_t h[16] = {0}, j0[16], ctr[16], ks[16], s[16] = {0}, lens[16];
    ls_aes256_encrypt(a, h, h);
    memcpy(j0, nonce, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    ls_ghash_update(s, h, aad, alen);
    if (!dir) ls_ghash_update(s, h, in, n);
    memcpy(ctr, j0, 16);
    for (size_t off = 0; off < n; off += 16) {
        for (int i = 15; i >= 12; i--) if (++ctr[i]) break;
        ls_aes256_encrypt(a, ctr, ks);
        size_t k = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < k; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
    }
    if (dir) ls_ghash_update(s, h, out, n);
    uint64_t abits = (uint64_t)alen * 8, cbits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)(abits >> (56 - 8 * i)); lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i)); }
    ls_ghash_update(s, h, lens, 16);
    uint8_t ej0[16], t[16];
    ls_aes256_encrypt(a, j0, ej0);
    for (int i = 0; i < 16; i++) t[i] = (uint8_t)(s[i] ^ ej0[i]);
    if (dir) { memcpy(tag, t, 16); return 0; }
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(t[i] ^ tag[i]);
    if (diff) { memset(out, 0, n); return -1; }
    return 0;
}

static void ls_random(uint8_t *out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) { ssize_t r = read(fd, out + got, n - got); if (r <= 0) break; got += (size_t)r; }
        close(fd);
    }
    if (got < n) {                    /* last resort: hash of clocks and addresses */
        static uint64_t ctr = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        for (size_t i = got; i < n; i += 32) {
            uint8_t d[32];
            uint64_t m[4] = { (uint64_t)ts.tv_nsec, (uint64_t)time(NULL), (uint64_t)(uintptr_t)&ts, ++ctr };
            ls_sha256_t c; ls_sha256_init(&c); ls_sha256_update(&c, m, sizeof(m)); ls_sha256_final_bin(&c, d);
            memcpy(out + i, d, n - i < 32 ? n - i : 32);
        }
    }
}

/* Vault keys from the shared 32-byte secret (same derivation as the PC client). */
typedef struct { uint8_t enc[32], id[32]; ls_aes256_t aes; } ls_vkeys_t;

static void ls_vkeys_init(ls_vkeys_t *k, const uint8_t secret[32]) {
    static const char enc_label[] = "shallot-vault/enc/v1", id_label[] = "shallot-vault/id/v1";
    ls_hmac_sha256(secret, 32, enc_label, sizeof(enc_label) - 1, k->enc);
    ls_hmac_sha256(secret, 32, id_label, sizeof(id_label) - 1, k->id);
    ls_aes256_init(&k->aes, k->enc);
}
/* Block id: hex HMAC(id_key, data). */
static void ls_vault_block_id(const ls_vkeys_t *k, const void *d, size_t n, char out[65]) {
    uint8_t m[32];
    ls_hmac_sha256(k->id, 32, d, n, m);
    ls_hex(m, 32, out);
}
/* Name: hex HMAC(id_key, "name:" + text). */
static void ls_vault_name(const ls_vkeys_t *k, const char *text, char out[65]) {
    uint8_t m[32];
    ls_hmac_sha256_2(k->id, 32, "name:", 5, text, strlen(text), m);
    ls_hex(m, 32, out);
}
/* out = nonce(12) | ciphertext | tag(16); returns its length. */
static size_t ls_vault_seal(const ls_vkeys_t *k, const char *aad, const uint8_t *pt, size_t n, uint8_t *out) {
    ls_random(out, 12);
    ls_gcm(&k->aes, out, (const uint8_t *)aad, strlen(aad), pt, n, out + 12, out + 12 + n, 1);
    return n + 28;
}
/* Returns the plaintext length, or -1 if the blob is not authentic. */
static int64_t ls_vault_open(const ls_vkeys_t *k, const char *aad, const uint8_t *blob, size_t n, uint8_t *out) {
    if (n < 28) return -1;
    size_t m = n - 28;
    uint8_t tag[16];
    memcpy(tag, blob + 12 + m, 16);
    if (ls_gcm(&k->aes, blob, (const uint8_t *)aad, strlen(aad), blob + 12, m, out, tag, 0) < 0) return -1;
    return (int64_t)m;
}
