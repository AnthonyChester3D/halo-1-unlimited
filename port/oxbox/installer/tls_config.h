/* tls_config.h: Mbed TLS (port/third_party/mbedtls) as the installer builds it (nxdk.mk: MBEDTLS_CONFIG_FILE), for
download.c: a TLS 1.2 and 1.3 client only, with the ciphers and curves servers use today, and nothing that needs a
file system, sockets of its own, threads or the system's randomness (download.c gives it its randomness:
mbedtls_hardware_poll). The rest of Mbed TLS's options are off. */
#ifndef OXBOX_TLS_CONFIG_H
#define OXBOX_TLS_CONFIG_H

/* the platform: nxdk (time() from its C library, milliseconds from download.c) */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
/* (nxdk's clang passes for Microsoft's compiler, whose <sal.h> it hasn't) */
#define MBEDTLS_CHECK_RETURN __attribute__((__warn_unused_result__))

/* symmetric ciphers: AES-GCM and ChaCha20-Poly1305 (and AES-CBC, which some TLS 1.2 servers still choose) */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC

/* hashes */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_HKDF_C

/* public keys: the server's RSA or ECDSA key signs the handshake (TLS 1.3's RSA signatures are PSS) */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* randomness (download.c's mbedtls_hardware_poll feeds it) and PSA, which TLS 1.3 works through */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_PSA_CRYPTO_C

/* TLS: a client, 1.2 and 1.3, naming the server it wants (SNI: GitHub's file host needs it) */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

#endif
