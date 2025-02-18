file(GLOB_RECURSE srcs "main.c" "src/*.c")

idf_component_register(
    SRCS "main.c" "wav_player.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_wifi esp_http_server driver spiffs
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)