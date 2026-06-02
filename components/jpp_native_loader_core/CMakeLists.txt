idf_component_register(
    SRCS
        "src/jpp_native_loader_core.c"
        "src/jpp_native_symtab.c"
    INCLUDE_DIRS "include"
    REQUIRES jpp_core jpp_crypto_core jpp_app_pool freertos log heap
)
