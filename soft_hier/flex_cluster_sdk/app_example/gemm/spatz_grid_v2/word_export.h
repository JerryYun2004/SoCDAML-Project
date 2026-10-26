/* Lossless, printable transport for untimed validation words.
 * The caller supplies _putchar(), using the SDK's existing character port.
 * Serialize explicit little-endian bytes, independent of host endianness.
 * No floating-point conversion, allocation, checksum, or libc formatter.
 */
#ifndef SOCDAML_WORD_EXPORT_H
#define SOCDAML_WORD_EXPORT_H
#include <stdint.h>

#define SOCDAML_EXPORT_BLOCK_WORDS 192u

static inline void export_three_bytes(uint8_t a, uint8_t b, uint8_t c)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    _putchar(alphabet[a >> 2]);
    _putchar(alphabet[((a & 3u) << 4) | (b >> 4)]);
    _putchar(alphabet[((b & 15u) << 2) | (c >> 6)]);
    _putchar(alphabet[c & 63u]);
}

static inline void export_word_payload(const volatile uint32_t *p, uint32_t count)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t i = 0;
    /* Read each volatile word exactly once; three words form four triples. */
    for (; i + 3u <= count; i += 3u) {
        uint32_t a = p[i], b = p[i+1u], c = p[i+2u];
        export_three_bytes((uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16));
        export_three_bytes((uint8_t)(a >> 24), (uint8_t)b, (uint8_t)(b >> 8));
        export_three_bytes((uint8_t)(b >> 16), (uint8_t)(b >> 24), (uint8_t)c);
        export_three_bytes((uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24));
    }
    if (i < count) {
        uint32_t a = p[i];
        export_three_bytes((uint8_t)a, (uint8_t)(a >> 8), (uint8_t)(a >> 16));
        uint8_t last = (uint8_t)(a >> 24);
        if (i + 1u < count) {
            uint32_t b = p[i+1u];
            export_three_bytes(last, (uint8_t)b, (uint8_t)(b >> 8));
            uint8_t x = (uint8_t)(b >> 16), y = (uint8_t)(b >> 24);
            _putchar(alphabet[x >> 2]);
            _putchar(alphabet[((x & 3u) << 4) | (y >> 4)]);
            _putchar(alphabet[(y & 15u) << 2]);
            _putchar('=');
        } else {
            _putchar(alphabet[last >> 2]);
            _putchar(alphabet[(last & 3u) << 4]);
            _putchar('=');
            _putchar('=');
        }
    }
}
#endif
