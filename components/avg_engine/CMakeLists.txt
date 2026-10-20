# avg_engine: PyWright 风格文字推理 AVG 引擎核心（与主固件解耦）。
# 依赖最小化：LEDC 蜂鸣走 read_pico 组件（read_pico_buzzer_tone），
# 文件走 STDIO，PSRAM 走 heap_caps —— 不 include 任何显示头文件。
# 增加新游戏 = SD 卡新增脚本+素材，不改本组件。
idf_component_register(
    SRCS "src/avg_script.c" "src/avg_core.c" "src/avg_asset.c" "src/avg_beep.c"
    INCLUDE_DIRS "include"
    REQUIRES read_pico
)
