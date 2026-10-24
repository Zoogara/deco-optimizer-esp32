# Screenshot renderer

Renders the **real** device UI (`main/ui.c`: the same LVGL widgets, fonts and layout the firmware runs) and the **real** web pages
(`main/web/index.html`, and the setup page's HTML taken straight from `main/provision.c`) with **invented** demo data, and writes PNGs.
It is how the images in `docs/screenshots` are made. It is host-only; nothing here is part of the firmware.

    idf.py build                       # once: fetches LVGL and writes build/config/sdkconfig.h
    make -C tools/screenshots shots    # device screens -> tools/screenshots/out/*.png
    make -C tools/screenshots web      # web pages      -> tools/screenshots/out/web_*.png

Needs a C compiler, Python 3 with Pillow (`pip install pillow`), and Google Chrome for the web pages (it runs headless with a throwaway profile).

## How it works
* `Makefile` compiles LVGL 8.4 for the host with the same settings as the device (the `CONFIG_LV_*` lines of the firmware's `sdkconfig.h`), except the
  colour depth: the device uses 16-bit RGB565, the host renders 32-bit, which looks the same.
* `host_ui.c` compiles `main/ui.c` in place, so the renderer exercises the shipped widget code, and adds one helper to reach its tileview.
* `shim/esp_lvgl_port.h` replaces ESP-IDF's LVGL port (a lock that does nothing on one thread); `demo_data.c` supplies a made-up run history.
* `render.c` drives each scene and writes a PPM; `topng.py` masks it to the round display, scales it 2x and adds a bezel.
* `web_shots.py` runs a mock backend with invented data, serves the real `index.html`, captures it with headless Chrome in dark and light, and crops the tall page.

## The demo data
All invented: the network `HomeMesh`, addresses in `192.168.1.x`, a reserved placeholder MAC, `user@example.com`, a fake host-key fingerprint and run dates in
March 2026. Nothing comes from a real network or person. Look at every image before copying it to `docs/screenshots`.
