/*
 * PS5 port: the mbedTLS 3.6 build for the frontend's own HTTPS (ps5/frontend/fe_https.cpp), 2026-10-05, AI-assisted.
 *
 * After the jailbreak the console's libSceSsl can't reach its own certificate store (sceSslGetCaList: 0x80020002), so it
 * trusts no site unless it is handed roots, and it can't follow cross-signed chains such as archive.org's (GoDaddy's
 * Root G2 sent cross-signed by Class 2). This build is a TLS 1.2 client that always checks the server's certificate chain
 * against the bundled Mozilla roots, and the server's name. Legacy crypto only (no PSA, no TLS 1.3, no server side).
 * Built with -DMBEDTLS_CONFIG_FILE='"ps5sx2_mbedtls_config.h"' -I<this folder>.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5SX2_MBEDTLS_CONFIG_H
#define PS5SX2_MBEDTLS_CONFIG_H

/* The system. ps5sx2_mbedtls_platform.c has gmtime_r and the zeroing: the PS5's libc exports neither gmtime_r nor
 * explicit_bzero. fe_https.cpp adds the system's random numbers as the only entropy source. */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* Ciphers and hashes: AES-GCM (with AES-NI and PCLMUL, which the PS5's CPU has) and ChaCha20-Poly1305. */
#define MBEDTLS_AES_C
#define MBEDTLS_AESNI_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C /* only so the 4 Mozilla roots that sign themselves with SHA-1 can be read; see below */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* Public keys: RSA (PKCS#1 v1.5 and PSS) and ECDSA/ECDHE on P-256, P-384, P-521 and X25519. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

/* Certificates: the chain, key usage and extended key usage are checked. SHA-1 signatures inside a chain are refused
 * (mbedTLS's default profile); a trusted root's own signature isn't checked, so a root signed with SHA-1 still works as
 * the end of a chain. TLS 1.2's handshake offers no SHA-1 signatures either (mbedTLS's default list). */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE

/* TLS 1.2, client side, forward-secret key exchanges only. */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET

/* mbedtls_strerror() for the log. */
#define MBEDTLS_ERROR_C

#endif /* PS5SX2_MBEDTLS_CONFIG_H */
