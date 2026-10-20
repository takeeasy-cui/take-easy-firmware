@echo off
rem ============================================================
rem take-easy firmware build (ESP-IDF v6.1, manual PATH mode)
rem Purpose: verification build after the 2026-10-18 avg_engine
rem          capacity changes (CMDS 3072 / LABELS 192 / FLAGS 32
rem          + dynamic PSRAM script buffer sized by file length).
rem Usage:   run this file from cmd, output = build\Read_Pico.bin
rem Why manual PATH instead of D:\esp-idf\export.bat:
rem          activate_venv.py fails because D:\esp-tools\tools.json
rem          (the installed-tool manifest) is missing, so it flags
rem          riscv32-esp-elf / openocd / ccache / dfu-util / etc as
rem          "no installed versions". Those are NOT needed for an
rem          esp32s3 Xtensa build. The esp32s3 toolchain IS fully
rem          installed under D:\esp-tools\tools. So we set up the
rem          environment by hand (this matches the proven earlier
rem          workflow) and call idf.py directly.
rem Toolchain versions (single install each, see D:\esp-tools\tools):
rem          xtensa-esp-elf esp-15.2.0_20251204 / cmake 3.22.1 /
rem          ninja 1.12.1 / esp-rom-elfs 20241011 /
rem          xtensa-esp-elf-gdb 17.1_20260402
rem Note:    keep this file pure ASCII (lesson 11: cmd parses with
rem          ANSI codepage, UTF-8 Chinese breaks line offsets).
rem ============================================================
setlocal
set "IDF_PATH=D:\esp-idf"
set "IDF_TOOLS_PATH=D:\esp-tools"
set "IDF_PYTHON_ENV_PATH=D:\esp-tools\python_env\idf6.1_py3.12_env"
set "IDF_TARGET=esp32s3"
rem ESP_IDF_VERSION is normally set by export activation; the
rem idf_component_manager extension reads it unguarded and
rem crashes on None, so provide it manually here.
set "ESP_IDF_VERSION=6.1"
set "PATH=D:\tools\MinGit\cmd;%IDF_PYTHON_ENV_PATH%\Scripts;D:\esp-tools\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;D:\esp-tools\tools\cmake\3.22.1\bin;D:\esp-tools\tools\ninja\1.12.1;D:\esp-tools\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;%PATH%"
cd /d D:\readpico-rc95
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" -X utf8 "%IDF_PATH%\tools\idf.py" build
if errorlevel 1 (
  echo [ERR] build failed
  exit /b 1
)
echo.
echo [OK] output: D:\readpico-rc95\build\Read_Pico.bin
dir /b build\Read_Pico.bin 2>nul || echo (check build output path)
endlocal
