#ifndef NTB_TLS_H
#define NTB_TLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntb-buffer.h"

/* PyBitmessage's link encryption. Peers that both set NODE_SSL in their
 * version switch the socket to TLS right after the verack exchange; the
 * side that accepted the connection is the TLS server. There are no
 * certificates: the cipher is anonymous ECDH (AECDH-AES256-SHA) on
 * secp256k1, which hides the traffic from a passive observer but does not
 * authenticate the peer.
 *
 * The session runs on memory BIOs so the connection keeps its own socket
 * code: received bytes go in through ntb_tls_receive, bytes to send come
 * out of ntb_tls_send. */

struct ntb_tls;

/* False when the linked OpenSSL lacks the cipher or the curve; the node
 * then does not advertise NODE_SSL. */
bool
ntb_tls_available(void);

struct ntb_tls *
ntb_tls_new(bool server);

void
ntb_tls_free(struct ntb_tls *tls);

/* Feeds ciphertext read from the socket, advances the handshake and
 * appends any decrypted data to plain. False on a TLS failure, which
 * ends the connection. */
bool
ntb_tls_receive(struct ntb_tls *tls,
                const uint8_t *data,
                size_t length,
                struct ntb_buffer *plain);

/* Encrypts and consumes plain once the handshake is done (until then it
 * stays queued) and appends everything the session has to send, the
 * handshake included, to cipher. False on a TLS failure. */
bool
ntb_tls_send(struct ntb_tls *tls,
             struct ntb_buffer *plain,
             struct ntb_buffer *cipher);

bool
ntb_tls_handshake_done(struct ntb_tls *tls);

/* Negotiated protocol and cipher, for the log. */
const char *
ntb_tls_description(struct ntb_tls *tls);

#endif /* NTB_TLS_H */
