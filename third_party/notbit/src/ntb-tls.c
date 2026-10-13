#include "config.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdio.h>
#include <string.h>

#include "ntb-tls.h"
#include "ntb-util.h"

struct ntb_tls {
        SSL *ssl;
        BIO *in; /* ciphertext from the peer, read by the session */
        BIO *out; /* ciphertext for the peer, written by the session */
        bool done;
        char description[96];
};

static SSL_CTX *context;
static bool context_tried;

static SSL_CTX *
get_context(void)
{
        SSL_CTX *ctx;

        if (context_tried)
                return context;
        context_tried = true;

        ctx = SSL_CTX_new(TLS_method());
        if (ctx == NULL)
                return NULL;

        /* Anonymous ciphers only exist up to TLS 1.2, and OpenSSL 1.1+
         * accepts them only at security level 0. PyBitmessage asks for
         * exactly this suite and the secp256k1 curve; the other groups
         * are for peers that offer something else. */
        if (!SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) ||
            !SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION) ||
            !SSL_CTX_set_cipher_list(ctx, "AECDH-AES256-SHA@SECLEVEL=0") ||
            !SSL_CTX_set1_groups_list(ctx, "secp256k1:X25519:P-256:P-384")) {
                ERR_clear_error();
                SSL_CTX_free(ctx);
                return NULL;
        }

        SSL_CTX_set_options(ctx,
                            SSL_OP_ALL |
                            SSL_OP_SINGLE_ECDH_USE |
                            SSL_OP_CIPHER_SERVER_PREFERENCE);
#ifdef SSL_OP_NO_RENEGOTIATION
        SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
#endif
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

        context = ctx;
        return context;
}

bool
ntb_tls_available(void)
{
        return get_context() != NULL;
}

struct ntb_tls *
ntb_tls_new(bool server)
{
        SSL_CTX *ctx = get_context();
        struct ntb_tls *tls;

        if (ctx == NULL)
                return NULL;

        tls = ntb_alloc(sizeof *tls);
        tls->ssl = SSL_new(ctx);
        tls->in = BIO_new(BIO_s_mem());
        tls->out = BIO_new(BIO_s_mem());
        tls->done = false;
        tls->description[0] = '\0';

        if (tls->ssl == NULL || tls->in == NULL || tls->out == NULL) {
                if (tls->ssl)
                        SSL_free(tls->ssl);
                if (tls->in)
                        BIO_free(tls->in);
                if (tls->out)
                        BIO_free(tls->out);
                ntb_free(tls);
                ERR_clear_error();
                return NULL;
        }

        /* An empty memory BIO reports "retry" rather than end of file. */
        BIO_set_mem_eof_return(tls->in, -1);
        SSL_set_bio(tls->ssl, tls->in, tls->out);

        if (server)
                SSL_set_accept_state(tls->ssl);
        else
                SSL_set_connect_state(tls->ssl);

        return tls;
}

void
ntb_tls_free(struct ntb_tls *tls)
{
        /* SSL_free frees both BIOs */
        SSL_free(tls->ssl);
        ntb_free(tls);
}

static bool
retryable(struct ntb_tls *tls, int ret)
{
        int error = SSL_get_error(tls->ssl, ret);

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                return true;

        ERR_clear_error();
        return false;
}

static bool
advance_handshake(struct ntb_tls *tls)
{
        int ret;

        if (tls->done)
                return true;

        ret = SSL_do_handshake(tls->ssl);
        if (ret == 1) {
                tls->done = true;
                snprintf(tls->description, sizeof tls->description,
                         "%s %s",
                         SSL_get_version(tls->ssl),
                         SSL_get_cipher_name(tls->ssl));
                return true;
        }

        return retryable(tls, ret);
}

bool
ntb_tls_receive(struct ntb_tls *tls,
                const uint8_t *data,
                size_t length,
                struct ntb_buffer *plain)
{
        int got;

        if (length > 0 && BIO_write(tls->in, data, (int) length) != (int) length)
                return false;

        if (!advance_handshake(tls))
                return false;

        if (!tls->done)
                return true;

        while (true) {
                ntb_buffer_ensure_size(plain, plain->length + 16384);
                got = SSL_read(tls->ssl,
                               plain->data + plain->length,
                               (int) (plain->size - plain->length));
                if (got <= 0)
                        /* A close_notify ends the stream; the socket
                         * closing reports the end of the connection. */
                        return retryable(tls, got) ||
                                SSL_get_error(tls->ssl, got) ==
                                SSL_ERROR_ZERO_RETURN;
                plain->length += got;
        }
}

bool
ntb_tls_send(struct ntb_tls *tls,
             struct ntb_buffer *plain,
             struct ntb_buffer *cipher)
{
        size_t pending;
        int wrote, got;

        if (!advance_handshake(tls))
                return false;

        if (tls->done && plain->length > 0) {
                /* A memory BIO never fills up, so the write takes all
                 * of it at once. */
                wrote = SSL_write(tls->ssl, plain->data, (int) plain->length);
                if (wrote <= 0) {
                        if (!retryable(tls, wrote))
                                return false;
                } else {
                        memmove(plain->data,
                                plain->data + wrote,
                                plain->length - wrote);
                        plain->length -= wrote;
                }
        }

        while ((pending = BIO_ctrl_pending(tls->out)) > 0) {
                ntb_buffer_ensure_size(cipher, cipher->length + pending);
                got = BIO_read(tls->out,
                               cipher->data + cipher->length,
                               (int) pending);
                if (got <= 0)
                        break;
                cipher->length += got;
        }

        return true;
}

bool
ntb_tls_handshake_done(struct ntb_tls *tls)
{
        return tls->done;
}

const char *
ntb_tls_description(struct ntb_tls *tls)
{
        return tls->description;
}
