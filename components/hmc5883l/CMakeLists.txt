idf_component_register(
    SRCS "hmc5883l.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
        log
        esp-idf-lib__i2cdev
        esp-idf-lib__esp_idf_lib_helpers
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
)
