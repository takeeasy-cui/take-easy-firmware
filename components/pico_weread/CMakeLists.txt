idf_component_register(
    SRCS "platform/HalStorage.cpp" "platform/WeReadHttpClient.cpp" "weread_service.cpp"
         "vendor/StreamingJsonParser.cpp" "vendor/WeReadProtocol.cpp" "vendor/WeReadStore.cpp"
         "vendor/WeReadBrowse.cpp" "vendor/WeReadXhtmlCodec.cpp" "vendor/WeReadClient.cpp"
    INCLUDE_DIRS "include" "platform" "vendor"
    REQUIRES esp_http_client mbedtls nvs_flash esp_timer esp_wifi read_pico_transfer
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ENABLE_CHINESE_VERSION=1 ENABLE_SERIAL_LOG=1 BOARD_HAS_PSRAM=1)
# 微读协议栈用尺寸优先编译，把应用压回 ota 槽 4MB 上限内；显示热路径保持全局 -O2。
# Compile the WeRead stack for size so the app fits the 4 MB OTA slot; display hot paths keep the global -O2.
target_compile_options(${COMPONENT_LIB} PRIVATE -Oz)
