#ifndef NTB_TLS_SETUP_H
#define NTB_TLS_SETUP_H
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>

/* What the node asks of OpenSSL for PyBitmessage's link encryption, in one place: ntb-tls.c builds
 * its context from it, and the configure-time check (cmake/check_node_tls.c) and the tests run the
 * same calls. Header only, so the check links nothing but OpenSSL. */

/* Anonymous ciphers only exist up to TLS 1.2, and OpenSSL 1.1+ accepts them only at security
 * level 0. PyBitmessage asks for exactly this suite and the secp256k1 curve; the other groups
 * are for peers that offer something else. Homebrew's OpenSSL 4.1.0 refuses secp256k1 as a TLS
 * group, so the last call of the setup fails there and the node has no TLS. */
#define NTB_TLS_CIPHERS "AECDH-AES256-SHA@SECLEVEL=0"
#define NTB_TLS_GROUPS "secp256k1:X25519:P-256:P-384"

/* Sets ctx up the way the node needs it. Returns NULL when OpenSSL took every step, else the name
 * of the call it refused; the reason is then in OpenSSL's error queue, which this empties first so
 * that nothing older is taken for it. The node passes NTB_TLS_GROUPS as groups. */
static const char *
ntb_tls_setup_context(SSL_CTX *ctx,
                      const char *groups)
{
        ERR_clear_error();

        if (!SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION))
                return "SSL_CTX_set_max_proto_version";
        if (!SSL_CTX_set_min_proto_version(ctx, TLS1_VERSION))
                return "SSL_CTX_set_min_proto_version";
        if (!SSL_CTX_set_cipher_list(ctx, NTB_TLS_CIPHERS))
                return "SSL_CTX_set_cipher_list";
        if (!SSL_CTX_set1_groups_list(ctx, groups))
                return "SSL_CTX_set1_groups_list";

        return NULL;
}

/* Why a setup stopped, for a log line or a build error: the call, OpenSSL's own words for it (the
 * first error in the queue, which is the cause) and the library's version. Empties the queue. */
static void
ntb_tls_describe_failure(const char *call,
                         char *text,
                         size_t size)
{
        char reason[256] = "OpenSSL gave no reason";
        unsigned long error = ERR_get_error();

        if (error != 0)
                ERR_error_string_n(error, reason, sizeof reason);
        ERR_clear_error();

        snprintf(text, size, "%s failed (%s), %s", call, reason,
                 OpenSSL_version(OPENSSL_VERSION));
}

#endif /* NTB_TLS_SETUP_H */
