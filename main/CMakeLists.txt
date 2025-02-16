file(GLOB_RECURSE srcs "main.c" "src/*.c")

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_wifi esp_http_server
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)