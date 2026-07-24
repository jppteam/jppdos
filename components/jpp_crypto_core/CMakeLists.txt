idf_component_register(
    SRCS "src/jpp_crypto_core.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__libsodium mbedtls
)
