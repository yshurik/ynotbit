/* Run by cmake/CheckNodeTls.cmake while configuring: can the OpenSSL found do what the node's TLS
 * needs? It prints OpenSSL's version and exits 0 if so, prints why and exits 2 if not. */
#include <stdio.h>
#include "ntb-tls-setup.h"

int
main(void)
{
        SSL_CTX *ctx = SSL_CTX_new(TLS_method());
        const char *refused = ctx ? ntb_tls_setup_context(ctx, NTB_TLS_GROUPS) : "SSL_CTX_new";
        char text[512];

        if (refused != NULL) {
                ntb_tls_describe_failure(refused, text, sizeof text);
                puts(text);
                return 2;
        }

        puts(OpenSSL_version(OPENSSL_VERSION));
        return 0;
}
