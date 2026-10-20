set(main_requires
    read_pico
    read_pico_transfer
    read_pico_search
    espressif__qrcode
    espressif__esp_jpeg
    # 渐进式 JPEG；基线仍走 esp_jpeg。/ Progressive JPEG; baseline stays on esp_jpeg.
    jpegdec
    espressif__libpng
    espressif__zlib
    espressif__esp_tinyusb
    # 微信传书（rc88 的 pico_weread 组件：扫码登录/云端书架/批量下载）。
    # WeChat transfer (rc88 pico_weread: QR login, cloud shelf, batch download).
    pico_weread
    # BLE HID 主机（蓝牙翻页器）。本仓库开了 MINIMAL_BUILD，组件不进依赖图的话连它的
    # Kconfig 都不会加载，sdkconfig.defaults 里的 CONFIG_BT_* 会被判为未知符号而丢弃。
    # BLE HID host (Bluetooth page-turner). This tree sets MINIMAL_BUILD, so a component
    # outside the dependency graph does not even load its Kconfig and the CONFIG_BT_*
    # entries in sdkconfig.defaults are dropped as unknown symbols.
    bt
    # 蓝牙翻页器本体。/ The page-turner itself.
    ble_page_turner
    continuous_du
    e0470_epaper_waveform
    e0470_page_turn
    avg_engine
    read_pico_pmu
    fatfs
    wear_levelling
    bootloader_support
    spi_flash
    heap
    nvs_flash
    esp_hw_support
    esp_rom
    esp_driver_gpio
    esp_driver_usb_serial_jtag
    esp_app_format
    app_update
    esp_http_client
    mbedtls
    espressif__cjson
)
# 开机图 / 锁屏图以 zlib(9) 压缩嵌入（大面积白纸位图约省 97% ≈ 800KB 闪存），
# 绘制时通过 ui_draw_full_image_z 解压进临时 PSRAM。main/assets/ 保留 4bpp 母版，
# .bin.z 为提交的压缩产物；改图后手动运行 tools/assets_compress.py 重新生成。
# Splash/lock frames embed zlib(9)-compressed (~97% smaller for flat frames,
# ~800KB flash saved) and inflate into scratch PSRAM at draw time. Masters stay
# in main/assets/ as raw 4bpp; the .bin.z blobs are committed artifacts.
# Regenerate with tools/assets_compress.py after editing a master.

idf_component_register(
    SRCS
        "app_main.c"
        "boot_state.c"
        "apps/app_boot_recovery.c"
        "ota_update.c"
        "display.c"
        "display_main_waveform.c"
        "display_reader_waveform.c"
        "sleep.c"
        "time_service.c"
        "calc_core.c"
        "lock_calc.c"
        "weather_service.c"
        "lock_pin.c"
        "lock_pin_flow.c"
        "lock_screen.c"
        "settings.c"
        # YanReader IME 引擎移植（2026-10-19 方案 A）：拼音+两分+词组+用户词典，
        # 码表驻自定义 data 分区 "ime"（mmap 直读），引擎 API 见 ime/IME.h。
        "ime/IME.cpp"
        "ime/yong_dict.cpp"
        "ime/yong_pinyin.cpp"
        "ime/ime_stub.cpp"
        "ime/ime_glue.cpp"
        "app/app_loop.c"
        "app/crash_note.c"
        "app/app_registry.c"
        "ui/ui_kit.c"
        "ui/ui_text_edit.c"
        "ui/ui_text_input.c"
        "ui/ui_ime.c"
        "ui/ui_keyboard.c"
        "ui/ui_gesture.c"
        "ui/ui_pinpad.c"
        "ui/ui_power_dialog.c"
        "ui/ui_quick_menu.c"
        "ui/ui_lock_feedback.c"
        "ui/ui_quick_menu_input.c"
        "ui/ui_click_feedback.c"
        "ui/ui_wifi_qr.c"
        "ui/datamatrix.c"
        "ui/ui_menu.c"
        "ui/ui_nav.c"
        "ui/ui_nothing.c"
        "ui/ui_home_widget.c"
        "ui/ui_home_clock.c"
        "ui/ui_image_dither.c"
        "ui/ui_wallpaper.c"
        "ui/ui_avatar.c"
        "ui/ui_reader_background.c"
        "apps/app_book.c"
        "apps/book_toc.c"
        "apps/app_dashboard.c"
        "apps/app_apps.c"
        "apps/app_calc.c"
        "apps/app_notes.c"
        "apps/app_pomodoro.c"
        "apps/app_muyu.c"
        "alarm.c"
        "apps/app_weread.c"
        "apps/app_files.c"
        "apps/file_tree.c"
        "apps/app_device_settings.c"
        "apps/app_image.c"
        "apps/app_transfer.c"
        "apps/usb_storage.c"
        # GameCore 统一游戏内核（2026-10-18）：主引擎在 game/，插件在 apps/plugins/。
        "game/game_core.c"
        "game/game_pkg.c"
        "game/game_host_ui.c"
        "apps/plugins/avgr.c"
        "apps/plugins/gtext.c"
        "apps/plugins/gbbk.c"
        "apps/app_avgr.c"
        "apps/app_gtext.c"
        "apps/app_gbbk.c"
        "book/book_store.c"
        "book/book_progress.c"
        "book/book_history_backup.c"
        "book/book_title.c"
        "book/book_ticket.c"
        "book/book_excerpt.c"
        "book/book_lock_collage.c"
        "book/book_source.c"
        "book/book_index_cache.c"
        "book/book_txt.c"
        "book/book_epub.c"
        "book/book_cover.c"
        "book/book_cover_jobs.c"
        "book/book_image_header.c"
        "book/book_cover_auto.c"
        "book/zip_reader.c"
        "book/html_text.c"
        "book/book_layout.c"
        "book/gbk.c"
        "font/ttf_font.c"
        "font/app_font_context.c"
        "font/ui_font.c"
        "font/ui_hanzi.c"
        "factory/pmu_selftest.c"
        "factory/vcom_setup.c"
    REQUIRES ${main_requires}
    INCLUDE_DIRS "." "app" "apps" "font" "ui" "factory" "book" "game" "apps/plugins" "ime"
    EMBED_FILES
        "assets/lock_4bpp.bin.z"
        "assets/loading_4bpp.bin.z"
        # v2（2026-10-17）：默认透明书封（灰8+alpha8 双平面，未选书封时的回退）。
        "assets/cover_default.bin.z"
        "assets/fox_avatar.bin"
        "assets/builtin.ttf"
        "assets/ui-hanzi.bin"
        "assets/reading.md"
        "assets/display-test.png"
        # IME 英文词表（77KB，embed；拼音/两分大码表在 ime 分区）。
        "ime/english_words.txt"
)

# 包住 IDF 的 panic 处理器：崩溃现场在重启前落进 RTC，没有串口时那是唯一的现场。
# 链接选项挂在 main 上，INTERFACE 会带到最终链接；写在 project() 之后的全局属性则太晚。
# Wrap the IDF panic handler so the crash scene reaches RTC before a restart; without a console
# that is the only place it survives. The option hangs off main, whose INTERFACE carries it into
# the final link; a global property set after project() is too late for the linker to see it.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# 本地 USB 测时入口，普通构建不包含。/ Local USB timing probe, omitted from ordinary product builds.
if("$ENV{KIIKO_LOCAL_LATENCY}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_LOCAL_LATENCY=1)
endif()
