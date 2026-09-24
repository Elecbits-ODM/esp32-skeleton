idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bl0937 wifi mqtt nvs littlefs gpio
)
