/*
 * Copyright 2016-2017 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <string.h>
#include "../ssl_locl.h"
#include "statem_locl.h"

static int final_renegotiate(SSL *s, unsigned int context, int sent,
                                     int *al);
static int init_server_name(SSL *s, unsigned int context);
static int final_server_name(SSL *s, unsigned int context, int sent,
                                     int *al);
#ifndef OPENSSL_NO_EC
static int final_ec_pt_formats(SSL *s, unsigned int context, int sent,
                                       int *al);
#endif
static int init_session_ticket(SSL *s, unsigned int context);
#ifndef OPENSSL_NO_OCSP
static int init_status_request(SSL *s, unsigned int context);
#endif
#ifndef OPENSSL_NO_NEXTPROTONEG
static int init_npn(SSL *s, unsigned int context);
#endif
static int init_alpn(SSL *s, unsigned int context);
static int init_sig_algs(SSL *s, unsigned int context);
static int init_certificate_authorities(SSL *s, unsigned int context);
static EXT_RETURN tls_construct_certificate_authorities(SSL *s, WPACKET *pkt,
                                                        unsigned int context,
                                                        X509 *x,
                                                        size_t chainidx,
                                                        int *al);
static int tls_parse_certificate_authorities(SSL *s, PACKET *pkt,
                                             unsigned int context, X509 *x,
                                             size_t chainidx, int *al);
#ifndef OPENSSL_NO_SRP
static int init_srp(SSL *s, unsigned int context);
#endif
static int init_etm(SSL *s, unsigned int context);
static int init_ems(SSL *s, unsigned int context);
static int final_ems(SSL *s, unsigned int context, int sent, int *al);
static int init_psk_kex_modes(SSL *s, unsigned int context);
#ifndef OPENSSL_NO_EC
static int final_key_share(SSL *s, unsigned int context, int sent, int *al);
#endif
#ifndef OPENSSL_NO_SRTP
static int init_srtp(SSL *s, unsigned int context);
#endif
static int final_sig_algs(SSL *s, unsigned int context, int sent, int *al);
static int final_early_data(SSL *s, unsigned int context, int sent, int *al);

/* Structure to define a built-in extension */
typedef struct extensions_definition_st {
    /* The defined type for the extension */
    unsigned int type;
    /*
     * The context that this extension applies to, e.g. what messages and
     * protocol versions
     */
    unsigned int context;
    /*
     * Initialise extension before parsing. Always called for relevant contexts
     * even if extension not present
     */
    int (*init)(SSL *s, unsigned int context);
    /* Parse extension sent from client to server */
    int (*parse_ctos)(SSL *s, PACKET *pkt, unsigned int context, X509 *x,
                      size_t chainidx, int *al);
    /* Parse extension send from server to client */
    int (*parse_stoc)(SSL *s, PACKET *pkt, unsigned int context, X509 *x,
                      size_t chainidx, int *al);
    /* Construct extension sent from server to client */
    EXT_RETURN (*construct_stoc)(SSL *s, WPACKET *pkt, unsigned int context,
                                 X509 *x, size_t chainidx, int *al);
    /* Construct extension sent from client to server */
    EXT_RETURN (*construct_ctos)(SSL *s, WPACKET *pkt, unsigned int context,
                                 X509 *x, size_t chainidx, int *al);
    /*
     * Finalise extension after parsing. Always called where an extensions was
     * initialised even if the extension was not present. |sent| is set to 1 if
     * the extension was seen, or 0 otherwise.
     */
    int (*final)(SSL *s, unsigned int context, int sent, int *al);
} EXTENSION_DEFINITION;

/*
 * Definitions of all built-in extensions. NOTE: Changes in the number or order
 * of these extensions should be mirrored with equivalent changes to the
 * indexes ( TLSEXT_IDX_* ) defined in ssl_locl.h.
 * Each extension has an initialiser, a client and
 * server side parser and a finaliser. The initialiser is called (if the
 * extension is relevant to the given context) even if we did not see the
 * extension in the message that we received. The parser functions are only
 * called if we see the extension in the message. The finalisers are always
 * called if the initialiser was called.
 * There are also server and client side constructor functions which are always
 * called during message construction if the extension is relevant for the
 * given context.
 * The initialisation, parsing, finalisation and construction functions are
 * always called in the order defined in this list. Some extensions may depend
 * on others having been processed first, so the order of this list is
 * significant.
 * The extension context is defined by a series of flags which specify which
 * messages the extension is relevant to. These flags also specify whether the
 * extension is relevant to a particular protocol or protocol version.
 *
 * TODO(TLS1.3): Make sure we have a test to check the consistency of these
 */
#define INVALID_EXTENSION { 0x10000, 0, NULL, NULL, NULL, NULL, NULL, NULL }
static const EXTENSION_DEFINITION ext_defs[] = {
    {
        TLSEXT_TYPE_renegotiate,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_SSL3_ALLOWED | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        NULL, tls_parse_ctos_renegotiate, tls_parse_stoc_renegotiate,
        tls_construct_stoc_renegotiate, tls_construct_ctos_renegotiate,
        final_renegotiate
    },
    {
        TLSEXT_TYPE_server_name,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
        init_server_name,
        tls_parse_ctos_server_name, tls_parse_stoc_server_name,
        tls_construct_stoc_server_name, tls_construct_ctos_server_name,
        final_server_name
    },
#ifndef OPENSSL_NO_SRP
    {
        TLSEXT_TYPE_srp,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        init_srp, tls_parse_ctos_srp, NULL, NULL, tls_construct_ctos_srp, NULL
    },
#else
    INVALID_EXTENSION,
#endif
#ifndef OPENSSL_NO_EC
    {
        TLSEXT_TYPE_ec_point_formats,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        NULL, tls_parse_ctos_ec_pt_formats, tls_parse_stoc_ec_pt_formats,
        tls_construct_stoc_ec_pt_formats, tls_construct_ctos_ec_pt_formats,
        final_ec_pt_formats
    },
    {
        TLSEXT_TYPE_supported_groups,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
        NULL, tls_parse_ctos_supported_groups, NULL,
        tls_construct_stoc_supported_groups,
        tls_construct_ctos_supported_groups, NULL
    },
#else
    INVALID_EXTENSION,
    INVALID_EXTENSION,
#endif
    {
        TLSEXT_TYPE_session_ticket,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        init_session_ticket, tls_parse_ctos_session_ticket,
        tls_parse_stoc_session_ticket, tls_construct_stoc_session_ticket,
        tls_construct_ctos_session_ticket, NULL
    },
    {
        TLSEXT_TYPE_signature_algorithms,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_CERTIFICATE_REQUEST,
        init_sig_algs, tls_parse_ctos_sig_algs,
        tls_parse_ctos_sig_algs, tls_construct_ctos_sig_algs,
        tls_construct_ctos_sig_algs, final_sig_algs
    },
#ifndef OPENSSL_NO_OCSP
    {
        TLSEXT_TYPE_status_request,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_3_CERTIFICATE,
        init_status_request, tls_parse_ctos_status_request,
        tls_parse_stoc_status_request, tls_construct_stoc_status_request,
        tls_construct_ctos_status_request, NULL
    },
#else
    INVALID_EXTENSION,
#endif
#ifndef OPENSSL_NO_NEXTPROTONEG
    {
        TLSEXT_TYPE_next_proto_neg,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        init_npn, tls_parse_ctos_npn, tls_parse_stoc_npn,
        tls_construct_stoc_next_proto_neg, tls_construct_ctos_npn, NULL
    },
#else
    INVALID_EXTENSION,
#endif
    {
        /*
         * Must appear in this list after server_name so that finalisation
         * happens after server_name callbacks
         */
        TLSEXT_TYPE_application_layer_protocol_negotiation,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
        init_alpn, tls_parse_ctos_alpn, tls_parse_stoc_alpn,
        tls_construct_stoc_alpn, tls_construct_ctos_alpn, NULL
    },
#ifndef OPENSSL_NO_SRTP
    {
        TLSEXT_TYPE_use_srtp,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS | SSL_EXT_DTLS_ONLY,
        init_srtp, tls_parse_ctos_use_srtp, tls_parse_stoc_use_srtp,
        tls_construct_stoc_use_srtp, tls_construct_ctos_use_srtp, NULL
    },
#else
    INVALID_EXTENSION,
#endif
    {
        TLSEXT_TYPE_encrypt_then_mac,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        init_etm, tls_parse_ctos_etm, tls_parse_stoc_etm,
        tls_construct_stoc_etm, tls_construct_ctos_etm, NULL
    },
#ifndef OPENSSL_NO_CT
    {
        TLSEXT_TYPE_signed_certificate_timestamp,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_3_CERTIFICATE,
        NULL,
        /*
         * No server side support for this, but can be provided by a custom
         * extension. This is an exception to the rule that custom extensions
         * cannot override built in ones.
         */
        NULL, tls_parse_stoc_sct, NULL, tls_construct_ctos_sct,  NULL
    },
#else
    INVALID_EXTENSION,
#endif
    {
        TLSEXT_TYPE_extended_master_secret,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO
        | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        init_ems, tls_parse_ctos_ems, tls_parse_stoc_ems,
        tls_construct_stoc_ems, tls_construct_ctos_ems, final_ems
    },
    {
        TLSEXT_TYPE_supported_versions,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS_IMPLEMENTATION_ONLY
        | SSL_EXT_TLS1_3_ONLY,
        NULL,
        /* Processed inline as part of version selection */
        NULL, NULL, NULL, tls_construct_ctos_supported_versions, NULL
    },
    {
        TLSEXT_TYPE_psk_kex_modes,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS_IMPLEMENTATION_ONLY
        | SSL_EXT_TLS1_3_ONLY,
        init_psk_kex_modes, tls_parse_ctos_psk_kex_modes, NULL, NULL,
        tls_construct_ctos_psk_kex_modes, NULL
    },
#ifndef OPENSSL_NO_EC
    {
        /*
         * Must be in this list after supported_groups. We need that to have
         * been parsed before we do this one.
         */
        TLSEXT_TYPE_key_share,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_SERVER_HELLO
        | SSL_EXT_TLS1_3_HELLO_RETRY_REQUEST | SSL_EXT_TLS_IMPLEMENTATION_ONLY
        | SSL_EXT_TLS1_3_ONLY,
        NULL, tls_parse_ctos_key_share, tls_parse_stoc_key_share,
        tls_construct_stoc_key_share, tls_construct_ctos_key_share,
        final_key_share
    },
#endif
    {
        TLSEXT_TYPE_cookie,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_HELLO_RETRY_REQUEST
        | SSL_EXT_TLS_IMPLEMENTATION_ONLY | SSL_EXT_TLS1_3_ONLY,
        NULL, NULL, tls_parse_stoc_cookie, NULL, tls_construct_ctos_cookie,
        NULL
    },
    {
        /*
         * Special unsolicited ServerHello extension only used when
         * SSL_OP_CRYPTOPRO_TLSEXT_BUG is set
         */
        TLSEXT_TYPE_cryptopro_bug,
        SSL_EXT_TLS1_2_SERVER_HELLO | SSL_EXT_TLS1_2_AND_BELOW_ONLY,
        NULL, NULL, NULL, tls_construct_stoc_cryptopro_bug, NULL, NULL
    },
    {
        TLSEXT_TYPE_early_data,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS
        | SSL_EXT_TLS1_3_NEW_SESSION_TICKET,
        NULL, tls_parse_ctos_early_data, tls_parse_stoc_early_data,
        tls_construct_stoc_early_data, tls_construct_ctos_early_data,
        final_early_data
    },
    {
        TLSEXT_TYPE_certificate_authorities,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_CERTIFICATE_REQUEST
        | SSL_EXT_TLS1_3_ONLY,
        init_certificate_authorities,
        tls_parse_certificate_authorities, tls_parse_certificate_authorities,
        tls_construct_certificate_authorities,
        tls_construct_certificate_authorities, NULL,
    },
    {
        /* Must be immediately before pre_shared_key */
        TLSEXT_TYPE_padding,
        SSL_EXT_CLIENT_HELLO,
        NULL,
        /* We send this, but don't read it */
        NULL, NULL, NULL, tls_construct_ctos_padding, NULL
    },
    {
        /* Required by the TLSv1.3 spec to always be the last extension */
        TLSEXT_TYPE_psk,
        SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_SERVER_HELLO
        | SSL_EXT_TLS_IMPLEMENTATION_ONLY | SSL_EXT_TLS1_3_ONLY,
        NULL, tls_parse_ctos_psk, tls_parse_stoc_psk, tls_construct_stoc_psk,
        tls_construct_ctos_psk, NULL
    }
};

/* Check whether an extension's context matches the current context */
static int validate_context(SSL *s, unsigned int extctx, unsigned int thisctx)
{
    /* Check we're allowed to use this extension in this context */
    if ((thisctx & extctx) == 0)
        return 0;

    if (SSL_IS_DTLS(s)) {
        if ((extctx & SSL_EXT_TLS_ONLY) != 0)
            return 0;
    } else if ((extctx & SSL_EXT_DTLS_ONLY) != 0) {
        return 0;
    }

    return 1;
}

/*
 * Verify whether we are allowed to use the extension |type| in the current
 * |context|. Returns 1 to indicate the extension is allowed or unknown or 0 to
 * indicate the extension is not allowed. If returning 1 then |*found| is set to
 * the definition for the extension we found.
 */
static int verify_extension(SSL *s, unsigned int context, unsigned int type,
                            custom_ext_methods *meths, RAW_EXTENSION *rawexlist,
                            RAW_EXTENSION **found)
{
    size_t i;
    size_t builtin_num = OSSL_NELEM(ext_defs);
    const EXTENSION_DEFINITION *thisext;

    for (i = 0, thisext = ext_defs; i < builtin_num; i++, thisext++) {
        if (type == thisext->type) {
            if (!validate_context(s, thisext->context, context))
                return 0;

            *found = &rawexlist[i];
            return 1;
        }
    }

    /* Check the custom extensions */
    if (meths != NULL) {
        size_t offset = 0;
        ENDPOINT role = ENDPOINT_BOTH;
        custom_ext_method *meth = NULL;

        if ((context & SSL_EXT_CLIENT_HELLO) != 0)
            role = ENDPOINT_SERVER;
        else if ((context & SSL_EXT_TLS1_2_SERVER_HELLO) != 0)
            role = ENDPOINT_CLIENT;

        meth = custom_ext_find(meths, role, type, &offset);
        if (meth != NULL) {
            if (!validate_context(s, meth->context, context))
                return 0;
            *found = &rawexlist[offset + builtin_num];
            return 1;
        }
    }

    /* Unknown extension. We allow it */
    *found = NULL;
    return 1;
}

/*
 * Check whether the context defined for an extension |extctx| means whether
 * the extension is relevant for the current context |thisctx| or not. Returns
 * 1 if the extension is relevant for this context, and 0 otherwise
 */
int extension_is_relevant(SSL *s, unsigned int extctx, unsigned int thisctx)
{
    if ((SSL_IS_DTLS(s)
                && (extctx & SSL_EXT_TLS_IMPLEMENTATION_ONLY) != 0)
            || (s->version == SSL3_VERSION
                    && (extctx & SSL_EXT_SSL3_ALLOWED) == 0)
            || (SSL_IS_TLS13(s)
                && (extctx & SSL_EXT_TLS1_2_AND_BELOW_ONLY) != 0)
            || (!SSL_IS_TLS13(s) && (extctx & SSL_EXT_TLS1_3_ONLY) != 0)
            || (s->hit && (extctx & SSL_EXT_IGNORE_ON_RESUMPTION) != 0))
        return 0;

    return 1;
}

/*
 * Gather a list of all the extensions from the data in |packet]. |context|
 * tells us which message this extension is for. The raw extension data is
 * stored in |*res| on success. In the event of an error the alert type to use
 * is stored in |*al|. We don't actually process the content of the extensions
 * yet, except to check their types. This function also runs the initialiser
 * functions for all known extensions if |init| is nonzero (whether we have
 * collected them or not). If successful the caller is responsible for freeing
 * the contents of |*res|.
 *
 * Per http://tools.ietf.org/html/rfc5246#section-7.4.1.4, there may not be
 * more than one extension of the same type in a ClientHello or ServerHello.
 * This function returns 1 if all extensions are unique and we have parsed their
 * types, and 0 if the extensions contain duplicates, could not be successfully
 * found, or an internal error occurred. We only check duplicates for
 * extensions that we know about. We ignore others.
 */
int tls_collect_extensions(SSL *s, PACKET *packet, unsigned int context,
                           RAW_EXTENSION **res, int *al, size_t *len,
                           int init)
{
    PACKET extensions = *packet;
    size_t i = 0;
    size_t num_exts;
    custom_ext_methods *exts = &s->cert->custext;
    RAW_EXTENSION *raw_extensions = NULL;
    const EXTENSION_DEFINITION *thisexd;

    *res = NULL;

    /*
     * Initialise server side custom extensions. Client side is done during
     * construction of extensions for the ClientHello.
     */
    if ((context & SSL_EXT_CLIENT_HELLO) != 0)
        custom_ext_init(&s->cert->custext);

    num_exts = OSSL_NELEM(ext_defs) + (exts != NULL ? exts->meths_count : 0);
    raw_extensions = OPENSSL_zalloc(num_exts * sizeof(*raw_extensions));
    if (raw_extensions == NULL) {
        *al = SSL_AD_INTERNAL_ERROR;
        SSLerr(SSL_F_TLS_COLLECT_EXTENSIONS, ERR_R_MALLOC_FAILURE);
        return 0;
    }

    i = 0;
    while (PACKET_remaining(&extensions) > 0) {
        unsigned int type, idx;
        PACKET extension;
        RAW_EXTENSION *thisex;

        if (!PACKET_get_net_2(&extensions, &type) ||
            !PACKET_get_length_prefixed_2(&extensions, &extension)) {
            SSLerr(SSL_F_TLS_COLLECT_EXTENSIONS, SSL_R_BAD_EXTENSION);
            *al = SSL_AD_DECODE_ERROR;
            goto err;
        }
        /*
         * Verify this extension is allowed. We only check duplicates for
         * extensions that we recognise. We also have a special case for the
         * PSK extension, which must be the last one in the ClientHello.
         */
        if (!verify_extension(s, context, type, exts, raw_extensions, &thisex)
                || (thisex != NULL && thisex->present == 1)
                || (type == TLSEXT_TYPE_psk
                    && (context & SSL_EXT_CLIENT_HELLO) != 0
                    && PACKET_remaining(&extensions) != 0)) {
            SSLerr(SSL_F_TLS_COLLECT_EXTENSIONS, SSL_R_BAD_EXTENSION);
            *al = SSL_AD_ILLEGAL_PARAMETER;
            goto err;
        }
        idx = thisex - raw_extensions;
        /*-
         * Check that we requested this extension (if appropriate). Requests can
         * be sent in the ClientHello and CertificateRequest. Unsolicited
         * extensions can be sent in the NewSessionTicket. We only do this for
         * the built-in extensions. Custom extensions have a different but
         * similar check elsewhere.
         * Special cases:
         * - The HRR cookie extension is unsolicited
         * - The renegotiate extension is unsolicited (the client signals
         *   support via an SCSV)
         * - The signed_certificate_timestamp extension can be provided by a
         * custom extension or by the built-in version. We let the extension
         * itself handle unsolicited response checks.
         */
        if (idx < OSSL_NELEM(ext_defs)
                && (context & (SSL_EXT_CLIENT_HELLO
                               | SSL_EXT_TLS1_3_CERTIFICATE_REQUEST
                               | SSL_EXT_TLS1_3_NEW_SESSION_TICKET)) == 0
                && type != TLSEXT_TYPE_cookie
                && type != TLSEXT_TYPE_renegotiate
                && type != TLSEXT_TYPE_signed_certificate_timestamp
                && (s->ext.extflags[idx] & SSL_EXT_FLAG_SENT) == 0) {
            SSLerr(SSL_F_TLS_COLLECT_EXTENSIONS, SSL_R_UNSOLICITED_EXTENSION);
            *al = SSL_AD_UNSUPPORTED_EXTENSION;
            goto err;
        }
        if (thisex != NULL) {
            thisex->data = extension;
            thisex->present = 1;
            thisex->type = type;
            thisex->received_order = i++;
            if (s->ext.debug_cb)
                s->ext.debug_cb(s, !s->server, thisex->type,
                                PACKET_data(&thisex->data),
                                PACKET_remaining(&thisex->data),
                                s->ext.debug_arg);
        }
    }

    if (init) {
        /*
         * Initialise all known extensions relevant to this context,
         * whether we have found them or not
         */
        for (thisexd = ext_defs, i = 0; i < OSSL_NELEM(ext_defs);
             i++, thisexd++) {
            if (thisexd->init != NULL && (thisexd->context & context) != 0
                && extension_is_relevant(s, thisexd->context, context)
                && !thisexd->init(s, context)) {
                *al = SSL_AD_INTERNAL_ERROR;
                goto err;
            }
        }
    }

    *res = raw_extensions;
    if (len != NULL)
        *len = num_exts;
    return 1;

 err:
    OPENSSL_free(raw_extensions);
    return 0;
}

/*
 * Runs the parser for a given extension with index |idx|. |exts| contains the
 * list of all parsed extensions previously collected by
 * tls_collect_extensions(). The parser is only run if it is applicable for the
 * given |context| and the parser has not already been run. If this is for a
 * Certificate message, then we also provide the parser with the relevant
 * Certificate |x| and its position in the |chainidx| with 0 being the first
 * Certificate. Returns 1 on success or 0 on failure. In the event of a failure
 * |*al| is populated with a suitable alert code. If an extension is not present
 * this counted as success.
 */
int tls_parse_extension(SSL *s, TLSEXT_INDEX idx, int context,
                        RAW_EXTENSION *exts, X509 *x, size_t chainidx, int *al)
{
    RAW_EXTENSION *currext = &exts[idx];
    int (*parser)(SSL *s, PACKET *pkt, unsigned int context, X509 *x,
                  size_t chainidx, int *al) = NULL;

    /* Skip if the extension is not present */
    if (!currext->present)
        return 1;

    /* Skip if we've already parsed this extension */
    if (currext->parsed)
        return 1;

    currext->parsed = 1;

    if (idx < OSSL_NELEM(ext_defs)) {
        /* We are handling a built-in extension */
        const EXTENSION_DEFINITION *extdef = &ext_defs[idx];

        /* Check if extension is defined for our protocol. If not, skip */
        if (!extension_is_relevant(s, extdef->context, context))
            return 1;

        parser = s->server ? extdef->parse_ctos : extdef->parse_stoc;

        if (parser != NULL)
            return parser(s, &currext->data, context, x, chainidx, al);

        /*
         * If the parser is NULL we fall through to the custom extension
         * processing
         */
    }

    /* Parse custom extensions */
    if (custom_ext_parse(s, context, currext->type,
                         PACKET_data(&currext->data),
                         PACKET_remaining(&currext->data),
                         x, chainidx, al) <= 0)
        return 0;

    return 1;
}

/*
 * Parse all remaining extensions that have not yet been parsed. Also calls the
 * finalisation for all extensions at the end if |fin| is nonzero, whether we
 * collected them or not. Returns 1 for success or 0 for failure. If we are
 * working on a Certificate message then we also pass the Certificate |x| and
 * its position in the |chainidx|, with 0 being the first certificate. On
 * failure, |*al| is populated with a suitable alert code.
 */
int tls_parse_all_extensions(SSL *s, int context, RAW_EXTENSION *exts, X509 *x,
                             size_t chainidx, int *al, int fin)
{
    size_t i, numexts = OSSL_NELEM(ext_defs);
    const EXTENSION_DEFINITION *thisexd;

    /* Calculate the number of extensions in the extensions list */
    numexts += s->cert->custext.meths_count;

    /* Parse each extension in turn */
    for (i = 0; i < numexts; i++) {
        if (!tls_parse_extension(s, i, context, exts, x, chainidx, al))
            return 0;
    }

    if (fin) {
        /*
         * Finalise all known extensions relevant to this context,
         * whether we have found them or not
         */
        for (i = 0, thisexd = ext_defs; i < OSSL_NELEM(ext_defs);
             i++, thisexd++) {
            if (thisexd->final != NULL && (thisexd->context & context) != 0
                && !thisexd->final(s, context, exts[i].present, al))
                return 0;
        }
    }

    return 1;
}

int should_add_extension(SSL *s, unsigned int extctx, unsigned int thisctx,
                         int max_version)
{
    /* Skip if not relevant for our context */
    if ((extctx & thisctx) == 0)
        return 0;

    /* Check if this extension is defined for our protocol. If not, skip */
    if ((SSL_IS_DTLS(s) && (extctx & SSL_EXT_TLS_IMPLEMENTATION_ONLY) != 0)
            || (s->version == SSL3_VERSION
                    && (extctx & SSL_EXT_SSL3_ALLOWED) == 0)
            || (SSL_IS_TLS13(s)
                && (extctx & SSL_EXT_TLS1_2_AND_BELOW_ONLY) != 0)
            || (!SSL_IS_TLS13(s)
                && (extctx & SSL_EXT_TLS1_3_ONLY) != 0
                && (thisctx & SSL_EXT_CLIENT_HELLO) == 0)
            || ((extctx & SSL_EXT_TLS1_3_ONLY) != 0
                && (thisctx & SSL_EXT_CLIENT_HELLO) != 0
                && (SSL_IS_DTLS(s) || max_version < TLS1_3_VERSION)))
        return 0;

    return 1;
}

/*
 * Construct all the extensions relevant to the current |context| and write
 * them to |pkt|. If this is an extension for a Certificate in a Certificate
 * message, then |x| will be set to the Certificate we are handling, and
 * |chainidx| will indicate the position in the chainidx we are processing (with
 * 0 being the first in the chain). Returns 1 on success or 0 on failure. If a
 * failure occurs then |al| is populated with a suitable alert code. On a
 * failure construction stops at the first extension to fail to construct.
 */
int tls_construct_extensions(SSL *s, WPACKET *pkt, unsigned int context,
                             X509 *x, size_t chainidx, int *al)
{
    size_t i;
    int min_version, max_version = 0, reason, tmpal;
    const EXTENSION_DEFINITION *thisexd;

    /*
     * Normally if something goes wrong during construction it's an internal
     * error. We can always override this later.
     */
    tmpal = SSL_AD_INTERNAL_ERROR;

    if (!WPACKET_start_sub_packet_u16(pkt)
               /*
                * If extensions are of zero length then we don't even add the
                * extensions length bytes to a ClientHello/ServerHello in SSLv3
                */
            || ((context &
                 (SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_2_SERVER_HELLO)) != 0
                && s->version == SSL3_VERSION
                && !WPACKET_set_flags(pkt,
                                     WPACKET_FLAGS_ABANDON_ON_ZERO_LENGTH))) {
        SSLerr(SSL_F_TLS_CONSTRUCT_EXTENSIONS, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if ((context & SSL_EXT_CLIENT_HELLO) != 0) {
        reason = ssl_get_min_max_version(s, &min_version, &max_version);
        if (reason != 0) {
            SSLerr(SSL_F_TLS_CONSTRUCT_EXTENSIONS, reason);
            goto err;
        }
    }

    /* Add custom extensions first */
    if ((context & SSL_EXT_CLIENT_HELLO) != 0) {
        /* On the server side with initialise during ClientHello parsing */
        custom_ext_init(&s->cert->custext);
    }
    if (!custom_ext_add(s, context, pkt, x, chainidx, max_version, &tmpal)) {
        SSLerr(SSL_F_TLS_CONSTRUCT_EXTENSIONS, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    for (i = 0, thisexd = ext_defs; i < OSSL_NELEM(ext_defs); i++, thisexd++) {
        EXT_RETURN (*construct)(SSL *s, WPACKET *pkt, unsigned int context,
                                X509 *x, size_t chainidx, int *al);
        EXT_RETURN ret;

        /* Skip if not relevant for our context */
        if (!should_add_extension(s, thisexd->context, context, max_version))
            continue;

        construct = s->server ? thisexd->construct_stoc
                              : thisexd->construct_ctos;

        if (construct == NULL)
            continue;

        ret = construct(s, pkt, context, x, chainidx, &tmpal);
        if (ret == EXT_RETURN_FAIL)
            goto err;
        if (ret == EXT_RETURN_SENT
                && (context & (SSL_EXT_CLIENT_HELLO
                               | SSL_EXT_TLS1_3_CERTIFICATE_REQUEST
                               | SSL_EXT_TLS1_3_NEW_SESSION_TICKET)) != 0)
            s->ext.extflags[i] |= SSL_EXT_FLAG_SENT;
    }

    if (!WPACKET_close(pkt)) {
        SSLerr(SSL_F_TLS_CONSTRUCT_EXTENSIONS, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    return 1;

 err:
    *al = tmpal;
    return 0;
}

/*
 * Built in extension finalisation and initialisation functions. All initialise
 * or finalise the associated extension type for the given |context|. For
 * finalisers |sent| is set to 1 if we saw the extension during parsing, and 0
 * otherwise. These functions return 1 on success or 0 on failure. In the event
 * of a failure then |*al| is populated with a suitable error code.
 */

static int final_renegotiate(SSL *s, unsigned int context, int sent,
                                     int *al)
{
    if (!s->server) {
        /*
         * Check if we can connect to a server that doesn't support safe
         * renegotiation
         */
        if (!(s->options & SSL_OP_LEGACY_SERVER_CONNECT)
                && !(s->options & SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION)
                && !sent) {
            *al = SSL_AD_HANDSHAKE_FAILURE;
            SSLerr(SSL_F_FINAL_RENEGOTIATE,
                   SSL_R_UNSAFE_LEGACY_RENEGOTIATION_DISABLED);
            return 0;
        }

        return 1;
    }

    /* Need RI if renegotiating */
    if (s->renegotiate
            && !(s->options & SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION)
            && !sent) {
        *al = SSL_AD_HANDSHAKE_FAILURE;
        SSLerr(SSL_F_FINAL_RENEGOTIATE,
               SSL_R_UNSAFE_LEGACY_RENEGOTIATION_DISABLED);
        return 0;
    }


    return 1;
}

static int init_server_name(SSL *s, unsigned int context)
{
    if (s->server)
        s->servername_done = 0;

    return 1;
}

static int final_server_name(SSL *s, unsigned int context, int sent,
                                     int *al)
{
    int ret = SSL_TLSEXT_ERR_NOACK;
    int altmp = SSL_AD_UNRECOGNIZED_NAME;

    if (s->ctx != NULL && s->ctx->ext.servername_cb != 0)
        ret = s->ctx->ext.servername_cb(s, &altmp,
                                        s->ctx->ext.servername_arg);
    else if (s->session_ctx != NULL
             && s->session_ctx->ext.servername_cb != 0)
        ret = s->session_ctx->ext.servername_cb(s, &altmp,
                                       s->session_ctx->ext.servername_arg);

    switch (ret) {
    case SSL_TLSEXT_ERR_ALERT_FATAL:
        *al = altmp;
        return 0;

    case SSL_TLSEXT_ERR_ALERT_WARNING:
        *al = altmp;
        return 1;

    case SSL_TLSEXT_ERR_NOACK:
        s->servername_done = 0;
        return 1;

    default:
        return 1;
    }
}

#ifndef OPENSSL_NO_EC
static int final_ec_pt_formats(SSL *s, unsigned int context, int sent,
                                       int *al)
{
    unsigned long alg_k, alg_a;

    if (s->server)
        return 1;

    alg_k = s->s3->tmp.new_cipher->algorithm_mkey;
    alg_a = s->s3->tmp.new_cipher->algorithm_auth;

    /*
     * If we are client and using an elliptic curve cryptography cipher
     * suite, then if server returns an EC point formats lists extension it
     * must contain uncompressed.
     */
    if (s->ext.ecpointformats != NULL
            && s->ext.ecpointformats_len > 0
            && s->session->ext.ecpointformats != NULL
            && s->session->ext.ecpointformats_len > 0
            && ((alg_k & SSL_kECDHE) || (alg_a & SSL_aECDSA))) {
        /* we are using an ECC cipher */
        size_t i;
        unsigned char *list = s->session->ext.ecpointformats;

        for (i = 0; i < s->session->ext.ecpointformats_len; i++) {
            if (*list++ == TLSEXT_ECPOINTFORMAT_uncompressed)
                break;
        }
        if (i == s->session->ext.ecpointformats_len) {
            SSLerr(SSL_F_FINAL_EC_PT_FORMATS,
                   SSL_R_TLS_INVALID_ECPOINTFORMAT_LIST);
            return 0;
        }
    }

    return 1;
}
#endif

static int init_session_ticket(SSL *s, unsigned int context)
{
    if (!s->server)
        s->ext.ticket_expected = 0;

    return 1;
}

#ifndef OPENSSL_NO_OCSP
static int init_status_request(SSL *s, unsigned int context)
{
    if (s->server) {
        s->ext.status_type = TLSEXT_STATUSTYPE_nothing;
    } else {
        /*
         * Ensure we get sensible values passed to tlsext_status_cb in the event
         * that we don't receive a status message
         */
        OPENSSL_free(s->ext.ocsp.resp);
        s->ext.ocsp.resp = NULL;
        s->ext.ocsp.resp_len = 0;
    }

    return 1;
}
#endif

#ifndef OPENSSL_NO_NEXTPROTONEG
static int init_npn(SSL *s, unsigned int context)
{
    s->s3->npn_seen = 0;

    return 1;
}
#endif

static int init_alpn(SSL *s, unsigned int context)
{
    OPENSSL_free(s->s3->alpn_selected);
    s->s3->alpn_selected = NULL;
    s->s3->alpn_selected_len = 0;
    if (s->server) {
        OPENSSL_free(s->s3->alpn_proposed);
        s->s3->alpn_proposed = NULL;
        s->s3->alpn_proposed_len = 0;
    }
    return 1;
}

static int init_sig_algs(SSL *s, unsigned int context)
{
    /* Clear any signature algorithms extension received */
    OPENSSL_free(s->s3->tmp.peer_sigalgs);
    s->s3->tmp.peer_sigalgs = NULL;

    return 1;
}

#ifndef OPENSSL_NO_SRP
static int init_srp(SSL *s, unsigned int context)
{
    OPENSSL_free(s->srp_ctx.login);
    s->srp_ctx.login = NULL;

    return 1;
}
#endif

static int init_etm(SSL *s, unsigned int context)
{
    s->ext.use_etm = 0;

    return 1;
}

static int init_ems(SSL *s, unsigned int context)
{
    if (!s->server)
        s->s3->flags &= ~TLS1_FLAGS_RECEIVED_EXTMS;

    return 1;
}

static int final_ems(SSL *s, unsigned int context, int sent, int *al)
{
    if (!s->server && s->hit) {
        /*
         * Check extended master secret extension is consistent with
         * original session.
         */
        if (!(s->s3->flags & TLS1_FLAGS_RECEIVED_EXTMS) !=
            !(s->session->flags & SSL_SESS_FLAG_EXTMS)) {
            *al = SSL_AD_HANDSHAKE_FAILURE;
            SSLerr(SSL_F_FINAL_EMS, SSL_R_INCONSISTENT_EXTMS);
            return 0;
        }
    }

    return 1;
}

static int init_certificate_authorities(SSL *s, unsigned int context)
{
    sk_X509_NAME_pop_free(s->s3->tmp.peer_ca_names, X509_NAME_free);
    s->s3->tmp.peer_ca_names = NULL;
    return 1;
}

static EXT_RETURN tls_construct_certificate_authorities(SSL *s, WPACKET *pkt,
                                                        unsigned int context,
                                                        X509 *x,
                                                        size_t chainidx,
                                                        int *al)
{
    const STACK_OF(X509_NAME) *ca_sk = SSL_get0_CA_list(s);

    if (ca_sk == NULL || sk_X509_NAME_num(ca_sk) == 0)
        return EXT_RETURN_NOT_SENT;

    if (!WPACKET_put_bytes_u16(pkt, TLSEXT_TYPE_certificate_authorities)
        || !WPACKET_start_sub_packet_u16(pkt)
        || !construct_ca_names(s, pkt)
        || !WPACKET_close(pkt)) {
        SSLerr(SSL_F_TLS_CONSTRUCT_CERTIFICATE_AUTHORITIES,
               ERR_R_INTERNAL_ERROR);
        return EXT_RETURN_FAIL;
    }

    return EXT_RETURN_SENT;
}

static int tls_parse_certificate_authorities(SSL *s, PACKET *pkt,
                                             unsigned int context, X509 *x,
                                             size_t chainidx, int *al)
{
    if (!parse_ca_names(s, pkt, al))
        return 0;
    if (PACKET_remaining(pkt) != 0) {
        *al = SSL_AD_DECODE_ERROR;
        return 0;
    }
    return 1;
}

#ifndef OPENSSL_NO_SRTP
static int init_srtp(SSL *s, unsigned int context)
{
    if (s->server)
        s->srtp_profile = NULL;

    return 1;
}
#endif

static int final_sig_algs(SSL *s, unsigned int context, int sent, int *al)
{
    if (!sent && SSL_IS_TLS13(s) && !s->hit) {
        *al = TLS13_AD_MISSING_EXTENSION;
        SSLerr(SSL_F_FINAL_SIG_ALGS, SSL_R_MISSING_SIGALGS_EXTENSION);
        return 0;
    }

    return 1;
}

#ifndef OPENSSL_NO_EC
static int final_key_share(SSL *s, unsigned int context, int sent, int *al)
{
    if (!SSL_IS_TLS13(s))
        return 1;

    /* Nothing to do for key_share in an HRR */
    if ((context & SSL_EXT_TLS1_3_HELLO_RETRY_REQUEST) != 0)
        return 1;

    /*
     * If
     *     we are a client
     *     AND
     *     we have no key_share
     *     AND
     *     (we are not resuming
     *      OR the kex_mode doesn't allow non key_share resumes)
     * THEN
     *     fail;
     */
    if (!s->server
            && !sent
            && (!s->hit
                || (s->ext.psk_kex_mode & TLSEXT_KEX_MODE_FLAG_KE) == 0)) {
        /* Nothing left we can do - just fail */
        *al = SSL_AD_MISSING_EXTENSION;
        SSLerr(SSL_F_FINAL_KEY_SHARE, SSL_R_NO_SUITABLE_KEY_SHARE);
        return 0;
    }
    /*
     * If
     *     we are a server
     *     AND
     *     we have no key_share
     * THEN
     *     If
     *         we didn't already send a HelloRetryRequest
     *         AND
     *         the client sent a key_share extension
     *         AND
     *         (we are not resuming
     *          OR the kex_mode allows key_share resumes)
     *         AND
     *         a shared group exists
     *     THEN
     *         send a HelloRetryRequest
     *     ELSE If
     *         we are not resuming
     *         OR
     *         the kex_mode doesn't allow non key_share resumes
     *     THEN
     *         fail;
     */
    if (s->server && s->s3->peer_tmp == NULL) {
        /* No suitable share */
        if (s->hello_retry_request == 0 && sent
                && (!s->hit
                    || (s->ext.psk_kex_mode & TLSEXT_KEX_MODE_FLAG_KE_DHE)
                       != 0)) {
            const unsigned char *pcurves, *pcurvestmp, *clntcurves;
            size_t num_curves, clnt_num_curves, i;
            unsigned int group_id = 0;

            /* Check if a shared group exists */

            /* Get the clients list of supported groups. */
            if (!tls1_get_curvelist(s, 1, &clntcurves, &clnt_num_curves)) {
                *al = SSL_AD_INTERNAL_ERROR;
                SSLerr(SSL_F_FINAL_KEY_SHARE, ERR_R_INTERNAL_ERROR);
                return 0;
            }

            /* Get our list of available groups */
            if (!tls1_get_curvelist(s, 0, &pcurves, &num_curves)) {
                *al = SSL_AD_INTERNAL_ERROR;
                SSLerr(SSL_F_FINAL_KEY_SHARE, ERR_R_INTERNAL_ERROR);
                return 0;
            }

            /* Find the first group we allow that is also in client's list */
            for (i = 0, pcurvestmp = pcurves; i < num_curves;
                 i++, pcurvestmp += 2) {
                group_id = bytestogroup(pcurvestmp);

                if (check_in_list(s, group_id, clntcurves, clnt_num_curves, 1))
                    break;
            }

            if (i < num_curves) {
                /* A shared group exists so send a HelloRetryRequest */
                s->s3->group_id = group_id;
                s->hello_retry_request = 1;
                return 1;
            }
        }
        if (!s->hit
                || (s->ext.psk_kex_mode & TLSEXT_KEX_MODE_FLAG_KE) == 0) {
            /* Nothing left we can do - just fail */
            if (!sent)
                *al = SSL_AD_MISSING_EXTENSION;
            else
                *al = SSL_AD_HANDSHAKE_FAILURE;
            SSLerr(SSL_F_FINAL_KEY_SHARE, SSL_R_NO_SUITABLE_KEY_SHARE);
            return 0;
        }
    }

    /* We have a key_share so don't send any more HelloRetryRequest messages */
    if (s->server)
        s->hello_retry_request = 0;

    /*
     * For a client side resumption with no key_share we need to generate
     * the handshake secret (otherwise this is done during key_share
     * processing).
     */
    if (!sent && !s->server && !tls13_generate_handshake_secret(s, NULL, 0)) {
        *al = SSL_AD_INTERNAL_ERROR;
        SSLerr(SSL_F_FINAL_KEY_SHARE, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
}
#endif

static int init_psk_kex_modes(SSL *s, unsigned int context)
{
    s->ext.psk_kex_mode = TLSEXT_KEX_MODE_FLAG_NONE;
    return 1;
}

int tls_psk_do_binder(SSL *s, const EVP_MD *md, const unsigned char *msgstart,
                      size_t binderoffset, const unsigned char *binderin,
                      unsigned char *binderout, SSL_SESSION *sess, int sign,
                      int external)
{
    EVP_PKEY *mackey = NULL;
    EVP_MD_CTX *mctx = NULL;
    unsigned char hash[EVP_MAX_MD_SIZE], binderkey[EVP_MAX_MD_SIZE];
    unsigned char finishedkey[EVP_MAX_MD_SIZE], tmpbinder[EVP_MAX_MD_SIZE];
    unsigned char tmppsk[EVP_MAX_MD_SIZE];
    unsigned char *early_secret, *psk;
    const char resumption_label[] = "res binder";
    const char external_label[] = "ext binder";
    const char nonce_label[] = "resumption";
    const char *label;
    size_t bindersize, labelsize, hashsize = EVP_MD_size(md);
    int ret = -1;

    if (external) {
        label = external_label;
        labelsize = sizeof(external_label) - 1;
    } else {
        label = resumption_label;
        labelsize = sizeof(resumption_label) - 1;
    }

    if (sess->master_key_length != hashsize) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, SSL_R_BAD_PSK);
        goto err;
    }

    if (external) {
        psk = sess->master_key;
    } else {
        psk = tmppsk;
        if (!tls13_hkdf_expand(s, md, sess->master_key,
                               (const unsigned char *)nonce_label,
                               sizeof(nonce_label) - 1, sess->ext.tick_nonce,
                               sess->ext.tick_nonce_len, psk, hashsize)) {
            SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
            goto err;
        }
    }

    /*
     * Generate the early_secret. On the server side we've selected a PSK to
     * resume with (internal or external) so we always do this. On the client
     * side we do this for a non-external (i.e. resumption) PSK so that it
     * is in place for sending early data. For client side external PSK we
     * generate it but store it away for later use.
     */
    if (s->server || !external)
        early_secret = (unsigned char *)s->early_secret;
    else
        early_secret = (unsigned char *)sess->early_secret;
    if (!tls13_generate_secret(s, md, NULL, psk, hashsize, early_secret)) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /*
     * Create the handshake hash for the binder key...the messages so far are
     * empty!
     */
    mctx = EVP_MD_CTX_new();
    if (mctx == NULL
            || EVP_DigestInit_ex(mctx, md, NULL) <= 0
            || EVP_DigestFinal_ex(mctx, hash, NULL) <= 0) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* Generate the binder key */
    if (!tls13_hkdf_expand(s, md, early_secret, (unsigned char *)label,
                           labelsize, hash, hashsize, binderkey, hashsize)) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* Generate the finished key */
    if (!tls13_derive_finishedkey(s, md, binderkey, finishedkey, hashsize)) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (EVP_DigestInit_ex(mctx, md, NULL) <= 0) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /*
     * Get a hash of the ClientHello up to the start of the binders. If we are
     * following a HelloRetryRequest then this includes the hash of the first
     * ClientHello and the HelloRetryRequest itself.
     */
    if (s->hello_retry_request) {
        size_t hdatalen;
        void *hdata;

        hdatalen = BIO_get_mem_data(s->s3->handshake_buffer, &hdata);
        if (hdatalen <= 0) {
            SSLerr(SSL_F_TLS_PSK_DO_BINDER, SSL_R_BAD_HANDSHAKE_LENGTH);
            goto err;
        }

        /*
         * For servers the handshake buffer data will include the second
         * ClientHello - which we don't want - so we need to take that bit off.
         */
        if (s->server) {
            PACKET hashprefix, msg;

            /* Find how many bytes are left after the first two messages */
            if (!PACKET_buf_init(&hashprefix, hdata, hdatalen)
                    || !PACKET_forward(&hashprefix, 1)
                    || !PACKET_get_length_prefixed_3(&hashprefix, &msg)
                    || !PACKET_forward(&hashprefix, 1)
                    || !PACKET_get_length_prefixed_3(&hashprefix, &msg)) {
                SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
                goto err;
            }
            hdatalen -= PACKET_remaining(&hashprefix);
        }

        if (EVP_DigestUpdate(mctx, hdata, hdatalen) <= 0) {
            SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
            goto err;
        }
    }

    if (EVP_DigestUpdate(mctx, msgstart, binderoffset) <= 0
            || EVP_DigestFinal_ex(mctx, hash, NULL) <= 0) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    mackey = EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, NULL, finishedkey, hashsize);
    if (mackey == NULL) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (!sign)
        binderout = tmpbinder;

    bindersize = hashsize;
    if (EVP_DigestSignInit(mctx, NULL, md, NULL, mackey) <= 0
            || EVP_DigestSignUpdate(mctx, hash, hashsize) <= 0
            || EVP_DigestSignFinal(mctx, binderout, &bindersize) <= 0
            || bindersize != hashsize) {
        SSLerr(SSL_F_TLS_PSK_DO_BINDER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (sign) {
        ret = 1;
    } else {
        /* HMAC keys can't do EVP_DigestVerify* - use CRYPTO_memcmp instead */
        ret = (CRYPTO_memcmp(binderin, binderout, hashsize) == 0);
    }

 err:
    OPENSSL_cleanse(binderkey, sizeof(binderkey));
    OPENSSL_cleanse(finishedkey, sizeof(finishedkey));
    EVP_PKEY_free(mackey);
    EVP_MD_CTX_free(mctx);

    return ret;
}

static int final_early_data(SSL *s, unsigned int context, int sent, int *al)
{
    if (!s->server || !sent)
        return 1;

    if (s->max_early_data == 0
            || !s->hit
            || s->session->ext.tick_identity != 0
            || s->early_data_state != SSL_EARLY_DATA_ACCEPTING
            || !s->ext.early_data_ok
            || s->hello_retry_request
            || s->s3->alpn_selected_len != s->session->ext.alpn_selected_len
            || (s->s3->alpn_selected_len > 0
                && memcmp(s->s3->alpn_selected, s->session->ext.alpn_selected,
                          s->s3->alpn_selected_len) != 0)) {
        s->ext.early_data = SSL_EARLY_DATA_REJECTED;
    } else {
        s->ext.early_data = SSL_EARLY_DATA_ACCEPTED;

        if (!tls13_change_cipher_state(s,
                    SSL3_CC_EARLY | SSL3_CHANGE_CIPHER_SERVER_READ)) {
            *al = SSL_AD_INTERNAL_ERROR;
            return 0;
        }
    }

    return 1;
}
