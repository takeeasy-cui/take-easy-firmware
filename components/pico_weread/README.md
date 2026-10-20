# Pico WeRead port

This experimental component ports CrossMux's Web API engine into ESP-IDF. The UI and POSIX/HTTP/clock adapters are native Pico code. The first build exposes login, shelf sync, whole-book EPUB download with optional JPEG/PNG inline images, local opening and logout. It never exposes cloud progress upload or annotation operations.

Upstream: `https://github.com/0x1abin/crossmux`, commit `d6a1727bb27a858ba2ee9a44529ba8e458defbad`, MIT; the notice is retained in `vendor/LICENSE-CrossMux.txt`. StreamingJsonParser comes from its FreeInk SDK gitlink `96de1be6ce08eb732909e6e8149af8f892b9a2c5`; its original license accompanies the vendor files. New adapter/service files carry Apache-2.0 headers. Original upstream comments are retained in the vendor directory.

Port differences: relative utility includes point to bounded native helpers; MD5/SHA-256 use the public PSA API; invalid operation enum states fail explicitly. Account sessions use the `pico_weread` NVS namespace rather than a removable-card file; cache paths are confined to `/sdcard/.readpico/weread`; `/WeRead` maps to the selected SD books root. Pico's existing EPUB cover reader replaces CrossMux thumbnail conversion/cache. HTTPS always verifies certificates; no Arduino or wolfSSL runtime is included. HTTP automatic redirects are disabled, and cookies remain scoped by the upstream engine. All protocol/file operations run on one worker. Cancel/exit/lock/media-loss joins that worker before returning.

Cached shelf pages are loaded off the UI thread. Only completed ZIP-validated EPUBs are atomically published by the upstream writer. Already downloaded files remain after logout. NVS sessions are local sensitive data, and the current board config does not enable NVS encryption. The device sends requests only for actions started on this page; leaving or locking stops the active operation and any network it started. Files and private session values are never uploaded to a development server.

The built-in transfer WiFi configuration is reused. A mounted TF card and 2.4 GHz WiFi with internet access are required. Downloads use unofficial Web interfaces and can fail or become unavailable with server changes or book/account restrictions. A successful build and image validation establish a candidate for device testing, not a guarantee of login/download or display behavior on a physical device.

Host verification: `python3 tools/test_weread_port.py` covers protocol vectors, NVS session validation, confined storage, shelf sorting, cancelled writes and ZIP publication. It also opens a ZIP-writer fixture through Pico's existing EPUB reader. `tools/run_wifi_qr_host_test.sh` and `tools/test_wifi_qr_decode.py` cover QR generation, address guards and independent decoding. These tests use temporary public text and fake account values, never real credentials.

Page/service shelf snapshots and page scratch data use PSRAM. The 16 KiB worker stack stays in internal RAM because session persistence accesses NVS. A STA session retained by WiFi provisioning is reused and kept connected after the operation; only a network started by this worker is stopped; stage/heap logs omit account parameters.

The file-browser list is explicitly placed in external BSS (40,704 bytes). ESP-IDF also relocates supported network BSS under this option, adding internal headroom during concurrent display, SD and WiFi activity. Flash/PSRAM timing remains unchanged.

IndexWriter creates the cache root before the first shelf write. The first-login regression starts with no cache directory; this matches the physical-device failure after authentication rather than presupposing initialized storage.

Directory creation checks stat first: FAT rejects mkdir on an already-mounted root with EINVAL, so existing roots must not be recreated. A mount-root simulation enforces this in the first-write regression.

HTTP streaming always drains the SDK read buffer, including complete responses prefetched during header parsing. Native adapter fixtures cover cached bodies, streaming, EAGAIN, cancellation and truncated responses.

rc76 native integration: File management opens this page from “微读传书”. Downloads embed inline JPEG/PNG resources by default; a per-download switch can omit images. Missing image resources are counted and reported at completion. Progress is shown by chapter, illustration and package stage. QR payloads are restricted to the exact WeRead HTTPS confirmation endpoint; transfer QR validation stays restricted to local IPv4 addresses. Shelf and download screens use the shared status bar, secondary header, rounded cards and file-manager navigation. Exit, lock and SD removal cancel and join the worker; incomplete ZIPs are never published.
