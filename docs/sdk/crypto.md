# Crypto primitives

!!! info "Requires SDK level 2 · C only."
    Declare `"sdk_min": 2`. These are plain C functions from
    `jpp_crypto_core.h`, not `jpp_sdk_*` calls, and there are no MicroPython
    bindings. See the [SDK changelog](../sdk-changelog.md).

!!! success "No capability required."
    Pure computation — no I/O and no security boundary to gate. Nothing to
    declare in the manifest, and the user is never prompted.

Stateless, mbedTLS-backed primitives (AES / SHA / bignum are hardware-accelerated
on the ESP32-C6). The heavy crypto code lives in the firmware, so an app can
implement transport crypto such as MTProto without carrying its own AES/bignum
in the app pool.

### `jpp_crypto_sha256`
### `jpp_crypto_sha1`

One-shot digests.

```c
jpp_crypto_status_t jpp_crypto_sha256(const uint8_t *msg, size_t len, uint8_t out[32]);
jpp_crypto_status_t jpp_crypto_sha1(const uint8_t *msg, size_t len, uint8_t out[20]);
```

### `jpp_crypto_aes256_ige_encrypt` / `jpp_crypto_aes256_ige_decrypt` { #jpp_crypto_aes256_ige_encrypt }

AES-256 in IGE mode (the mode MTProto uses). `length` must be a non-zero
multiple of 16. `iv` is 32 bytes (two blocks) and is read-only. `out` may alias
`in` for in-place operation.

```c
jpp_crypto_status_t jpp_crypto_aes256_ige_encrypt(
    const uint8_t *in, size_t length,
    const uint8_t key[32], const uint8_t iv[32], uint8_t *out);
jpp_crypto_status_t jpp_crypto_aes256_ige_decrypt(
    const uint8_t *in, size_t length,
    const uint8_t key[32], const uint8_t iv[32], uint8_t *out);
```

### `jpp_crypto_modexp`

Big-integer modular exponentiation `out = base^exp mod modulus`. All operands
are unsigned big-endian byte strings. `out` receives `modulus_len` bytes,
big-endian, left-padded with zeros.

```c
jpp_crypto_status_t jpp_crypto_modexp(
    const uint8_t *base, size_t base_len,
    const uint8_t *exp, size_t exp_len,
    const uint8_t *modulus, size_t modulus_len,
    uint8_t *out, size_t *out_len);
```

### `jpp_crypto_rsa_encrypt`
### `jpp_crypto_dh_compute`

Thin, clarity-only wrappers over `modexp`: `rsa_encrypt` computes
`data^exponent mod modulus` (the RSA public-key operation); `dh_compute`
computes `base^exp mod prime` (a Diffie-Hellman step). The math is identical to
`modexp`.

```c
jpp_crypto_status_t jpp_crypto_rsa_encrypt(
    const uint8_t *data, size_t data_len,
    const uint8_t *modulus, size_t modulus_len,
    const uint8_t *exponent, size_t exponent_len,
    uint8_t *out, size_t *out_len);
jpp_crypto_status_t jpp_crypto_dh_compute(
    const uint8_t *base, size_t base_len,
    const uint8_t *exp, size_t exp_len,
    const uint8_t *prime, size_t prime_len,
    uint8_t *out, size_t *out_len);
```

**Returns (all):** `JPP_CRYPTO_OK`, `JPP_CRYPTO_ERR_INVALID_ARG` (NULL/zero-length
operand, non-block-multiple AES length, or zero modulus), or
`JPP_CRYPTO_ERR_INTERNAL`.

---
