# Rebuilding the ESP32 core with the lwIP NAPT patch

The NTP forwarder needs lwIP rebuilt with NAPT enabled, then the resulting
`liblwip.a` dropped into the Arduino ESP32 core. This is the exact recipe used
for this project.

## 1. ESP-IDF v4.4 + toolchain

```sh
git clone --recursive --branch v4.4 https://github.com/espressif/esp-idf ~/esp/esp-idf
# checkout the exact commit the patch is based on:
cd ~/esp/esp-idf && git checkout 8153bfe && git submodule update --init --recursive
```

You also need the ESP32-S2 GCC toolchain (xtensa-esp32s2-elf, ~esp-2021r2-8.4.0)
installed via `~/esp/esp-idf/install.sh`. The rest assumes an activated IDF
environment (e.g. a conda env `idf` with the IDF + toolchain dirs on `PATH`):

```sh
export IDF_PATH=~/esp/esp-idf
export IDF_SKIP_SUBMODULE_CHECK=1
export PATH="$HOME/esp/esp-idf/tools:$PATH"
export PATH="$HOME/esp/.idf_tools/tools/xtensa-esp32s2-elf/esp-2021r2-8.4.0/xtensa-esp32s2-elf/bin:$PATH"
```

## 2. Apply the patch

```sh
cd ~/esp/esp-idf
git apply /path/to/ps4-900u-host/docs/lwip-napt.patch
```

The patch is two hunks; `git apply` from the ESP-IDF root covers both
(`components/lwip/Kconfig`, `components/lwip/port/esp32/include/lwipopts.h` and
the `components/lwip/lwip` submodule sources).

## 3. Build `liblwip.a`

Use a small IDF project (this repo built lwIP standalone). Its
`sdkconfig.defaults` must contain:

```
CONFIG_IDF_TARGET="esp32s2"
CONFIG_LWIP_IPV4_NAPT=y
CONFIG_LWIP_IP_FORWARD=n
CONFIG_LWIP_L2_TO_L3_COPY=y
```

```sh
cd <your-build-dir>
rm -f sdkconfig
idf.py build
```

The result is at `build/esp-idf/lwip/liblwip.a`.

## 4. Install into the Arduino core

Replace the core's prebuilt lwIP and mirror the flags in its SDK config:

```sh
CORE=~/.arduino15/packages/esp32/hardware/esp32/2.0.5/tools/sdk/esp32s2
cp build/esp-idf/lwip/liblwip.a "$CORE/lib/liblwip.a"
```

and set, in every `sdkconfig.h` variant under
`$CORE/` (e.g. `dio/`, `dout/`, `qio/`, `qout_qspi/` subdirs):

```
#define CONFIG_LWIP_IPV4_NAPT 1
#define CONFIG_LWIP_IP_FORWARD 0
#define CONFIG_LWIP_L2_TO_L3_COPY 1
```

Recompile the sketch; the NTP interception is now active. A core reinstall wipes
`liblwip.a` and the flags, so redo this step after upgrading the board package.
