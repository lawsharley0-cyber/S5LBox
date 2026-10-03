/*
 * S5LBox — IMG3 firmware container.
 *
 * Every 3.x-era Apple firmware image (LLB, iBoot, kernelcache, device tree,
 * logo) ships wrapped in IMG3: a tagged container holding the payload, its
 * key/IV bag, and an RSA signature. Parsing it is the first step of M3 —
 * before we can execute Apple's boot chain we have to unwrap it.
 *
 * Layout (all little-endian):
 *   header   '3gmI' magic, full size, data size, signed size, ident (e.g. 'ibot')
 *   tags     repeated: magic, total size, data size, data[], padding[]
 *
 * Tags we care about:
 *   TYPE  the image type, repeating ident
 *   DATA  the payload, AES-CBC encrypted when a KBAG says so
 *   KBAG  encryption key bag (crypt state, key bits, IV + key, RSA-wrapped)
 *   SHSH  RSA signature over the image
 *   CERT  certificate chain
 *   VERS  a version string
 *
 * This parser is strictly bounds-checked: it is the first component to touch
 * untrusted user-supplied files, so every offset is validated against the
 * buffer before use.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_IMG3_H
#define S5LBOX_IMG3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMG3_MAGIC 0x496d6733u  /* 'Img3' as stored ('3gmI' on disk) */

#define IMG3_TAG_TYPE 0x54595045u
#define IMG3_TAG_DATA 0x44415441u
#define IMG3_TAG_KBAG 0x4b424147u
#define IMG3_TAG_SHSH 0x53485348u
#define IMG3_TAG_CERT 0x43455254u
#define IMG3_TAG_VERS 0x56455253u
#define IMG3_TAG_SEPO 0x5345504fu
#define IMG3_TAG_BORD 0x424f5244u

typedef enum {
    IMG3_OK = 0,
    IMG3_ERR_TOO_SMALL,     /* buffer smaller than a header                */
    IMG3_ERR_BAD_MAGIC,     /* not an IMG3 container                       */
    IMG3_ERR_BAD_SIZE,      /* declared size inconsistent with the buffer  */
    IMG3_ERR_BAD_TAG,       /* a tag runs past the end of the container    */
    IMG3_ERR_INVALID_ARGUMENT
} img3_status_t;

/* A KBAG describes how DATA is encrypted. cryptState 1 = production keys,
 * 2 = development. The key/IV here are RSA-wrapped on real firmware; users
 * supply the unwrapped values (published for 3.x devices) separately. */
typedef struct {
    uint32_t crypt_state;
    uint32_t key_bits;      /* 128 / 192 / 256 */
    uint8_t  iv[16];
    uint8_t  key[32];
    bool     present;    /* a KBAG was found AND understood        */
    bool     malformed;  /* a KBAG was found but could NOT be parsed */
} img3_kbag_t;

typedef struct {
    uint32_t ident;         /* e.g. 'ibot', 'krnl', 'dtre' */
    uint32_t full_size;
    uint32_t data_size;
    uint32_t signed_size;

    const uint8_t *data;    /* DATA tag payload (points into the input) */
    uint32_t       data_len;
    uint32_t       data_room; /* bytes the DATA tag holds, padding included */

    img3_kbag_t kbag;
    bool        has_shsh;
    char        vers[64];   /* NUL-terminated if a VERS tag was present */
} img3_t;

/* Parse `buf` in place. Nothing is copied; `out` points into `buf`, which must
 * outlive it. Returns IMG3_OK or a specific error. */
img3_status_t img3_parse(const uint8_t *buf, size_t len, img3_t *out);

/*
 * Decrypt DATA with an explicit, user-supplied IV. This is the only safe form
 * for real firmware: the
 * IV stored in the KBAG is RSA/GID-wrapped, whereas the IV published alongside
 * the key by the reverse-engineering community is the unwrapped one. They are
 * different values, so decrypting with the KBAG's IV would silently produce
 * garbage in the first block. There is deliberately no API that implicitly
 * uses img.kbag.iv.
 *
 * `key_bits` must match the parsed KBAG and `out_cap` must cover img.data_len
 * bytes. When the payload ends inside a block, the tag's padding normally
 * holds the rest of that block's ciphertext (the 3GS's iPhone OS 3.1.3
 * device tree: 42840 bytes in a 42848-byte tag), and then the whole block is
 * decrypted and only `data_len` bytes kept, as iBoot does. A tag with no such
 * padding has its unaligned tail copied through verbatim. `out_len` receives
 * the number of bytes written. `out` may be img->data (decrypting in place).
 */
bool img3_decrypt_data_iv(const img3_t *img, const uint8_t *key, unsigned key_bits,
                          const uint8_t iv[16], uint8_t *out, size_t out_cap,
                          uint32_t *out_len);

/*
 * The same, choosing what happens to an unaligned tail: `decrypt_tail` false
 * always copies it through verbatim, which is what the tools that produced
 * the iPhone 3G's accepted kernel.macho did (docs/BOOT_CHAIN.md: its
 * kernelcache's last block stays ciphertext, and unlzss reports the 42-byte
 * zero-fill that is part of the accepted hash). A kernelcache's tail is
 * compressed padding past the end of the kernel, so either choice boots; the
 * accepted hash needs this one. A device tree's tail is its last property,
 * so it must be decrypted.
 */
bool img3_decrypt_data_iv_tail(const img3_t *img, const uint8_t *key, unsigned key_bits,
                               const uint8_t iv[16], uint8_t *out, size_t out_cap,
                               uint32_t *out_len, bool decrypt_tail);

/* Human-readable status, for logs and the app UI. */
const char *img3_strerror(img3_status_t st);

/* Render a four-character tag/ident into `out` (needs 5 bytes). */
void img3_ident_str(uint32_t ident, char *out);

#endif /* S5LBOX_IMG3_H */
