/* The node's TLS setup (ntb-tls-setup.h). On this OpenSSL it goes through; when OpenSSL refuses a
 * step, the description names the call, gives OpenSSL's reason and the library's version, which is
 * what the node's log and the configure-time check (cmake/check_node_tls.c) print. */
#include <stdio.h>
#include <string.h>
#include "ntb-tls-setup.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    char text[512];
    const char *refused;
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());

    CHECK(ctx != NULL);
    /* If this fails, a node built against this OpenSSL offers no TLS to PyBitmessage peers. */
    refused = ntb_tls_setup_context(ctx, NTB_TLS_GROUPS);
    if (refused) {
        ntb_tls_describe_failure(refused, text, sizeof text);
        fprintf(stderr, "the node's TLS setup is refused: %s\n", text);
    }
    CHECK(refused == NULL);
    SSL_CTX_free(ctx);

    /* A group no OpenSSL knows stops the setup at the groups call. */
    ctx = SSL_CTX_new(TLS_method());
    CHECK(ctx != NULL);
    refused = ntb_tls_setup_context(ctx, "no-such-group");
    CHECK(refused != NULL);
    CHECK(strcmp(refused, "SSL_CTX_set1_groups_list") == 0);
    SSL_CTX_free(ctx);

    /* The description says which call and which OpenSSL, and leaves no error for the next caller. */
    ntb_tls_describe_failure(refused, text, sizeof text);
    CHECK(strstr(text, "SSL_CTX_set1_groups_list failed") != NULL);
    CHECK(strstr(text, OpenSSL_version(OPENSSL_VERSION)) != NULL);
    CHECK(ERR_peek_error() == 0);
    return 0;
}
