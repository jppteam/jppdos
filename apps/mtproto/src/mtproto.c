/*
 * mtproto.c — minimal MTProto client SKELETON (envelope measurement).
 *
 * Structural implementation of a send/receive MTProto client:
 *   - abridged TCP transport over the SDK v2 net_connect surface
 *   - the PQ + Diffie-Hellman auth-key handshake, using jpp_crypto_rsa_encrypt,
 *     jpp_crypto_sha1, jpp_crypto_aes256_ige_*, and jpp_crypto_dh_compute
 *   - MTProto 2.0 encrypted message framing (SHA-256 msg_key + AES-256-IGE)
 *
 * Response parsing is reduced to the fields the flow consumes; a production
 * client would validate nonce echoes / DH correctness / server salt / seq_no.
 * The point of this file is to make mtproto.bin's code + BSS sizes realistic.
 */

#include "mtproto.h"
#include "jpp_crypto_core.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* Symbols resolved from the firmware symbol table (no libc/libsodium headers
   are on the app include path, so declare the prototypes we use directly). */
extern void randombytes_buf(void *buf, size_t size);

/* ---- Config ------------------------------------------------------------- */

#define MTP_DC_HOST     "149.154.167.51"   /* Telegram DC2 (production) */
#define MTP_DC_PORT     443u
#define MTP_TIMEOUT_MS  8000u

#define MTP_KEY_BYTES       256u           /* 2048-bit RSA / DH operands */
#define MTP_AUTH_KEY_BYTES  256u
#define MTP_MSG_BUF         2048u

/*
 * Telegram server RSA public key modulus — PLACEHOLDER.  A real client ships
 * the DC's actual 2048-bit modulus (public, distributed with the app) and
 * selects it by fingerprint from resPQ.  A 256-byte constant here so the flash
 * footprint of carrying a server key is counted.
 */
static const uint8_t s_rsa_modulus[MTP_KEY_BYTES] = { 0xC1, 0x50, 0x02, 0x3E };
static const uint8_t s_rsa_exponent[3] = { 0x01, 0x00, 0x01 };

/* ---- Static state (BSS footprint) --------------------------------------- */

static int     s_sock = -1;
static uint8_t s_auth_key[MTP_AUTH_KEY_BYTES];
static uint8_t s_auth_key_id[8];
static uint8_t s_nonce[16];
static uint8_t s_server_nonce[16];
static uint8_t s_new_nonce[32];
static uint8_t s_dh_prime[MTP_KEY_BYTES];
static uint8_t s_g_a[MTP_KEY_BYTES];
static uint8_t s_g_b[MTP_KEY_BYTES];
static uint8_t s_gab[MTP_KEY_BYTES];
static uint8_t s_tmp_key[32];
static uint8_t s_tmp_iv[32];
static uint8_t s_tx[MTP_MSG_BUF];
static uint8_t s_rx[MTP_MSG_BUF];
static uint64_t s_msg_seq;

/* ---- TL little-endian writer -------------------------------------------- */

typedef struct { uint8_t *buf; size_t cap; size_t len; } tl_w_t;

static void tl_init(tl_w_t *w, uint8_t *buf, size_t cap) { w->buf = buf; w->cap = cap; w->len = 0u; }

static void w_u32(tl_w_t *w, uint32_t v)
{
    if (w->len + 4u > w->cap) { return; }
    w->buf[w->len++] = (uint8_t)v;
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)(v >> 16);
    w->buf[w->len++] = (uint8_t)(v >> 24);
}

static void w_u64(tl_w_t *w, uint64_t v) { w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)(v >> 32)); }

static void w_raw(tl_w_t *w, const uint8_t *d, size_t n)
{
    if (w->len + n > w->cap) { return; }
    memcpy(w->buf + w->len, d, n);
    w->len += n;
}

/* TL "bytes": 1- or 4-byte length prefix, then data, then 0-padding to 4. */
static void w_bytes(tl_w_t *w, const uint8_t *d, size_t n)
{
    size_t start = w->len;
    if (n < 254u) {
        if (w->len < w->cap) { w->buf[w->len++] = (uint8_t)n; }
    } else {
        w_u32(w, (uint32_t)((n << 8) | 0xFEu));
    }
    w_raw(w, d, n);
    while (((w->len - start) & 3u) != 0u && w->len < w->cap) { w->buf[w->len++] = 0u; }
}

/* ---- Abridged transport ------------------------------------------------- */

static bool tcp_send_all(jpp_sdk_context_t *ctx, const uint8_t *data, size_t len)
{
    jpp_broker_result_t r;
    return jpp_sdk_net_send(ctx, s_sock, data, len, &r) == JPP_SDK_STATUS_OK && r.ok;
}

static bool tp_connect(jpp_sdk_context_t *ctx)
{
    jpp_broker_result_t r;
    if (jpp_sdk_net_connect(ctx, MTP_DC_HOST, (uint16_t)MTP_DC_PORT,
                            MTP_TIMEOUT_MS, &s_sock, &r) != JPP_SDK_STATUS_OK ||
        !r.ok || s_sock < 0) {
        return false;
    }
    const uint8_t abridged = 0xEFu;   /* select the abridged transport */
    return tcp_send_all(ctx, &abridged, 1u);
}

static bool tp_send(jpp_sdk_context_t *ctx, const uint8_t *payload, size_t len)
{
    uint8_t hdr[4];
    size_t hlen;
    uint32_t words = (uint32_t)(len / 4u);
    if (words < 0x7Fu) {
        hdr[0] = (uint8_t)words;
        hlen = 1u;
    } else {
        hdr[0] = 0x7Fu;
        hdr[1] = (uint8_t)words;
        hdr[2] = (uint8_t)(words >> 8);
        hdr[3] = (uint8_t)(words >> 16);
        hlen = 4u;
    }
    return tcp_send_all(ctx, hdr, hlen) && tcp_send_all(ctx, payload, len);
}

static size_t tp_recv(jpp_sdk_context_t *ctx, uint8_t *buf, size_t cap)
{
    jpp_broker_result_t r;
    uint8_t lenbuf[4];
    size_t got = 0u;
    if (jpp_sdk_net_recv(ctx, s_sock, lenbuf, 1u, &got, MTP_TIMEOUT_MS, &r) != JPP_SDK_STATUS_OK ||
        !r.ok || got == 0u) {
        return 0u;
    }
    uint32_t words = lenbuf[0];
    if (words == 0x7Fu) {
        if (jpp_sdk_net_recv(ctx, s_sock, lenbuf, 3u, &got, MTP_TIMEOUT_MS, &r) != JPP_SDK_STATUS_OK ||
            !r.ok || got < 3u) {
            return 0u;
        }
        words = (uint32_t)lenbuf[0] | ((uint32_t)lenbuf[1] << 8) | ((uint32_t)lenbuf[2] << 16);
    }
    size_t need = (size_t)words * 4u;
    if (need > cap) { need = cap; }
    size_t total = 0u;
    while (total < need) {
        if (jpp_sdk_net_recv(ctx, s_sock, buf + total, need - total, &got, MTP_TIMEOUT_MS, &r) != JPP_SDK_STATUS_OK ||
            !r.ok || got == 0u) {
            break;
        }
        total += got;
    }
    return total;
}

/* ---- Unencrypted (auth_key_id = 0) messages ----------------------------- */

static uint64_t next_msg_id(void)
{
    /* Real clients derive msg_id from unixtime<<32; a monotonic counter keeps
       the skeleton self-contained. */
    s_msg_seq += 4u;
    return (s_msg_seq << 2);
}

static bool mtp_send_plain(jpp_sdk_context_t *ctx, const uint8_t *payload, size_t len)
{
    tl_w_t w;
    tl_init(&w, s_tx, sizeof(s_tx));
    w_u64(&w, 0u);                 /* auth_key_id = 0 */
    w_u64(&w, next_msg_id());
    w_u32(&w, (uint32_t)len);
    w_raw(&w, payload, len);
    return tp_send(ctx, w.buf, w.len);
}

/* ---- Handshake (skeleton) ----------------------------------------------- */

/* Derive tmp AES key/iv for the DH-params step from new_nonce+server_nonce, per
   the MTProto spec's SHA-1 construction. */
static void derive_tmp_aes(void)
{
    uint8_t h1[20], h2[20], h3[20];
    uint8_t cat[64];
    /* SHA1(new_nonce + server_nonce) */
    memcpy(cat, s_new_nonce, 32u); memcpy(cat + 32u, s_server_nonce, 16u);
    (void)jpp_crypto_sha1(cat, 48u, h1);
    /* SHA1(server_nonce + new_nonce) */
    memcpy(cat, s_server_nonce, 16u); memcpy(cat + 16u, s_new_nonce, 32u);
    (void)jpp_crypto_sha1(cat, 48u, h2);
    /* SHA1(new_nonce + new_nonce) */
    memcpy(cat, s_new_nonce, 32u); memcpy(cat + 32u, s_new_nonce, 32u);
    (void)jpp_crypto_sha1(cat, 64u, h3);

    memcpy(s_tmp_key, h1, 20u);
    memcpy(s_tmp_key + 20u, h2, 12u);          /* key = h1 || h2[0:12] */
    memcpy(s_tmp_iv, h2 + 12u, 8u);
    memcpy(s_tmp_iv + 8u, h3, 20u);
    memcpy(s_tmp_iv + 28u, s_new_nonce, 4u);   /* iv = h2[12:20] || h3 || nn[0:4] */
}

static bool mtp_handshake(jpp_sdk_context_t *ctx)
{
    tl_w_t w;

    /* Step 1: req_pq_multi#be7e8ef1 nonce:int128 */
    randombytes_buf(s_nonce, sizeof(s_nonce));
    tl_init(&w, s_rx, sizeof(s_rx));
    w_u32(&w, 0xbe7e8ef1u);
    w_raw(&w, s_nonce, 16u);
    if (!mtp_send_plain(ctx, w.buf, w.len)) { return false; }

    /* Recv resPQ; a full client parses server_nonce + pq + key fingerprints.
       Skeleton: read the frame, synthesize the fields the flow needs. */
    size_t n = tp_recv(ctx, s_rx, sizeof(s_rx));
    if (n < 40u) { return false; }
    memcpy(s_server_nonce, s_rx + 24u, 16u);   /* offset past hdr+resPQ+nonce */
    randombytes_buf(s_new_nonce, sizeof(s_new_nonce));

    /* Step 2: build p_q_inner_data, SHA1, RSA-encrypt to the server key. */
    uint8_t inner[192];
    tl_w_t iw; tl_init(&iw, inner, sizeof(inner));
    w_u32(&iw, 0x83c95aecu);                   /* p_q_inner_data */
    uint8_t pq[8] = { 0x17, 0xED, 0x48, 0x94, 0x1A, 0x08, 0xF9, 0x81 };
    w_bytes(&iw, pq, sizeof(pq));
    uint8_t p[4] = { 0x49, 0x4C, 0x55, 0x3B }, q[4] = { 0x53, 0x91, 0x10, 0x73 };
    w_bytes(&iw, p, sizeof(p));
    w_bytes(&iw, q, sizeof(q));
    w_raw(&iw, s_nonce, 16u);
    w_raw(&iw, s_server_nonce, 16u);
    w_raw(&iw, s_new_nonce, 32u);

    uint8_t data_with_hash[MTP_KEY_BYTES];
    memset(data_with_hash, 0, sizeof(data_with_hash));
    (void)jpp_crypto_sha1(inner, iw.len, data_with_hash);
    memcpy(data_with_hash + 20u, inner, iw.len);       /* SHA1 || data || pad */

    uint8_t encrypted_pq[MTP_KEY_BYTES];
    size_t enc_len = 0u;
    if (jpp_crypto_rsa_encrypt(data_with_hash, sizeof(data_with_hash),
                               s_rsa_modulus, sizeof(s_rsa_modulus),
                               s_rsa_exponent, sizeof(s_rsa_exponent),
                               encrypted_pq, &enc_len) != JPP_CRYPTO_OK) {
        return false;
    }

    /* Step 3: req_DH_params (carries encrypted_data). */
    tl_init(&w, s_tx + 512u, 1024u);
    w_u32(&w, 0xd712e4beu);                    /* req_DH_params */
    w_raw(&w, s_nonce, 16u);
    w_raw(&w, s_server_nonce, 16u);
    w_bytes(&w, p, sizeof(p));
    w_bytes(&w, q, sizeof(q));
    uint8_t fp[8] = { 0 };
    w_raw(&w, fp, 8u);                          /* public key fingerprint */
    w_bytes(&w, encrypted_pq, enc_len);
    if (!mtp_send_plain(ctx, w.buf, w.len)) { return false; }

    /* Recv server_DH_params_ok; decrypt the answer with tmp AES-IGE. */
    n = tp_recv(ctx, s_rx, sizeof(s_rx));
    if (n < 60u) { return false; }
    derive_tmp_aes();
    /* The encrypted answer starts after auth_key_id(8)+msg_id(8)+len(4)+
       server_DH_params_ok header(1+16+16+string hdr). Skeleton uses a fixed
       offset; a real client parses the TL string length. */
    size_t ans_off = 56u;
    size_t ans_len = (n - ans_off) & ~15u;
    if (ans_len >= 16u) {
        (void)jpp_crypto_aes256_ige_decrypt(s_rx + ans_off, ans_len,
                                            s_tmp_key, s_tmp_iv, s_rx + ans_off);
        /* Inside: SHA1(20) + server_DH_inner_data(dh_prime, g, g_a, ...). */
        memcpy(s_dh_prime, s_rx + ans_off + 40u, MTP_KEY_BYTES > (ans_len - 40u)
               ? (ans_len - 40u) : MTP_KEY_BYTES);
        memcpy(s_g_a, s_rx + ans_off + 40u, sizeof(s_g_a) < (ans_len - 40u)
               ? sizeof(s_g_a) : 4u);
    }

    /* Step 4: pick b, compute g_b = g^b mod p and auth_key = g_a^b mod p. */
    uint8_t b[MTP_KEY_BYTES];
    randombytes_buf(b, sizeof(b));
    uint8_t g[4] = { 0x00, 0x00, 0x00, 0x03 };
    size_t out_len = 0u;
    (void)jpp_crypto_dh_compute(g, sizeof(g), b, sizeof(b),
                                s_dh_prime, sizeof(s_dh_prime), s_g_b, &out_len);
    (void)jpp_crypto_dh_compute(s_g_a, sizeof(s_g_a), b, sizeof(b),
                                s_dh_prime, sizeof(s_dh_prime), s_gab, &out_len);
    memcpy(s_auth_key, s_gab, sizeof(s_auth_key));

    /* auth_key_id = low 64 bits of SHA1(auth_key). */
    uint8_t akh[20];
    (void)jpp_crypto_sha1(s_auth_key, sizeof(s_auth_key), akh);
    memcpy(s_auth_key_id, akh + 12u, 8u);

    /* Step 5: client_DH_inner_data, AES-IGE encrypt, set_client_DH_params. */
    tl_w_t cw; tl_init(&cw, s_tx, sizeof(s_tx));
    w_u32(&cw, 0x6643b654u);                    /* client_DH_inner_data */
    w_raw(&cw, s_nonce, 16u);
    w_raw(&cw, s_server_nonce, 16u);
    w_u64(&cw, 0u);                             /* retry_id */
    w_bytes(&cw, s_g_b, sizeof(s_g_b));
    uint8_t cdh[512];
    uint8_t chash[20];
    (void)jpp_crypto_sha1(cw.buf, cw.len, chash);
    memcpy(cdh, chash, 20u);
    memcpy(cdh + 20u, cw.buf, cw.len);
    size_t cdh_len = (20u + cw.len + 15u) & ~15u;
    (void)jpp_crypto_aes256_ige_encrypt(cdh, cdh_len, s_tmp_key, s_tmp_iv, cdh);

    tl_init(&w, s_rx, sizeof(s_rx));
    w_u32(&w, 0xf5045f1fu);                     /* set_client_DH_params */
    w_raw(&w, s_nonce, 16u);
    w_raw(&w, s_server_nonce, 16u);
    w_bytes(&w, cdh, cdh_len);
    if (!mtp_send_plain(ctx, w.buf, w.len)) { return false; }

    /* Recv dh_gen_ok — handshake complete (auth_key established). */
    n = tp_recv(ctx, s_rx, sizeof(s_rx));
    return n >= 40u;
}

/* ---- Encrypted message (MTProto 2.0) ------------------------------------ */

/* Derive the AES key/iv from auth_key + msg_key (MTProto 2.0, SHA-256). */
static void derive_msg_aes(const uint8_t msg_key[16], int from_client,
                           uint8_t key[32], uint8_t iv[32])
{
    uint8_t buf[16 + 36];
    uint8_t sha_a[32], sha_b[32];
    size_t x = from_client ? 0u : 8u;
    memcpy(buf, msg_key, 16u);
    memcpy(buf + 16u, s_auth_key + x, 36u);
    (void)jpp_crypto_sha256(buf, sizeof(buf), sha_a);
    memcpy(buf, s_auth_key + 40u + x, 36u);
    memcpy(buf + 36u, msg_key, 16u);
    (void)jpp_crypto_sha256(buf, 52u, sha_b);
    memcpy(key, sha_a, 8u); memcpy(key + 8u, sha_b + 8u, 16u); memcpy(key + 24u, sha_a + 24u, 8u);
    memcpy(iv, sha_b, 8u); memcpy(iv + 8u, sha_a + 8u, 16u); memcpy(iv + 24u, sha_b + 24u, 8u);
}

static bool mtp_send_message(jpp_sdk_context_t *ctx, const char *text)
{
    /* Inner: messages.sendMessage skeleton (flags, peer, message, random_id). */
    tl_w_t iw; tl_init(&iw, s_tx + 1024u, 512u);
    w_u32(&iw, 0x1cc20387u);                    /* messages.sendMessage-ish */
    w_u32(&iw, 0u);                             /* flags */
    w_u32(&iw, 0x9db1bc6du);                    /* inputPeerSelf */
    w_bytes(&iw, (const uint8_t *)text, strlen(text));
    uint64_t rnd; randombytes_buf(&rnd, sizeof(rnd));
    w_u64(&iw, rnd);

    /* Wrap: salt(8)+session_id(8)+msg_id(8)+seq_no(4)+len(4)+body, padded. */
    tl_w_t pw; tl_init(&pw, s_tx, 1024u);
    uint64_t salt, session; randombytes_buf(&salt, 8u); randombytes_buf(&session, 8u);
    w_u64(&pw, salt);
    w_u64(&pw, session);
    w_u64(&pw, next_msg_id());
    w_u32(&pw, 0u);
    w_u32(&pw, (uint32_t)iw.len);
    w_raw(&pw, iw.buf, iw.len);
    while ((pw.len & 15u) != 0u && pw.len < pw.cap) { pw.buf[pw.len++] = 0u; }

    /* msg_key = middle 16 bytes of SHA-256(auth_key[88:120] + plaintext). */
    uint8_t mk_src[32 + 1024];
    memcpy(mk_src, s_auth_key + 88u, 32u);
    memcpy(mk_src + 32u, pw.buf, pw.len);
    uint8_t mk_full[32];
    (void)jpp_crypto_sha256(mk_src, 32u + pw.len, mk_full);
    uint8_t msg_key[16];
    memcpy(msg_key, mk_full + 8u, 16u);

    uint8_t key[32], iv[32];
    derive_msg_aes(msg_key, 1, key, iv);
    (void)jpp_crypto_aes256_ige_encrypt(pw.buf, pw.len, key, iv, pw.buf);

    /* Frame: auth_key_id(8) + msg_key(16) + encrypted. */
    tl_w_t fw; tl_init(&fw, s_rx, sizeof(s_rx));
    w_raw(&fw, s_auth_key_id, 8u);
    w_raw(&fw, msg_key, 16u);
    w_raw(&fw, pw.buf, pw.len);
    return tp_send(ctx, fw.buf, fw.len);
}

/* ---- App UI ------------------------------------------------------------- */

void mtproto_run(jpp_sdk_context_t *ctx)
{
    const char *lines[3];
    jpp_broker_result_t r;

    lines[0] = "MTProto (skeleton)";
    lines[1] = "CENTER: connect+auth";
    lines[2] = "LEFT: exit";
    jpp_sdk_set_frame(ctx, lines, 3u);

    for (;;) {
        jpp_sdk_key_event_t ev = JPP_SDK_KEY_NONE;
        if (jpp_sdk_wait_key(ctx, 0u, &ev) != JPP_SDK_STATUS_OK) { continue; }
        if (ev == JPP_SDK_KEY_LEFT || ev == JPP_SDK_KEY_CENTER_LONG) { break; }
        if (ev != JPP_SDK_KEY_CENTER) { continue; }

        lines[1] = "Connecting...";
        lines[2] = "";
        jpp_sdk_set_frame(ctx, lines, 3u);

        bool ok = tp_connect(ctx) && mtp_handshake(ctx);
        if (ok) {
            (void)mtp_send_message(ctx, "hello from J++Device");
            jpp_sdk_log(ctx, "mtproto_auth_ok");
            lines[1] = "Auth OK; msg sent";
        } else {
            jpp_sdk_log(ctx, "mtproto_auth_fail");
            lines[1] = "Connect/auth failed";
        }
        lines[2] = "CENTER retry / LEFT exit";
        jpp_sdk_set_frame(ctx, lines, 3u);

        if (s_sock >= 0) { jpp_sdk_net_close(ctx, s_sock, &r); s_sock = -1; }
    }

    if (s_sock >= 0) { jpp_sdk_net_close(ctx, s_sock, &r); s_sock = -1; }
    jpp_sdk_request_close(ctx);
}
