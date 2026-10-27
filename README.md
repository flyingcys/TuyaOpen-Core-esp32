# TuyaOpen-Core ESP32 project

**English** | [简体中文](README_zh.md)

A standalone ESP-IDF project that consumes the official
[TuyaOpen-Core](https://github.com/tuya/TuyaOpen-Core) repository through the
ESP-IDF component manager. Core and its ESP32 port are downloaded into
`iot_demo/managed_components/` during the first configure; no core source is
vendored in this repository.

`iot_demo/` connects to the Tuya cloud, is provisioned over BLE (WiFi
credentials plus the binding token are sent by the Tuya APP; nothing is
compiled in), and sends/receives DPs. It is structurally parallel to the Linux
demo in the core repository.

## Structure

```
TuyaOpen-Core-esp32/
└── iot_demo/
    ├── CMakeLists.txt          # esp-idf project entry (no EXTRA_COMPONENT_DIRS)
    ├── main/
    │   ├── idf_component.yml   # component-manager dependencies (core + port)
    │   ├── main.c              # app_main: nvs_flash_init -> tuya_app_main()
    │   ├── tuya_main.c         # application flow + BLE provisioning (token-get port)
    │   ├── wifi_sta.c/.h       # WiFi station (credentials from BLE, persisted in KV)
    │   ├── reset_netcfg.c/.h   # clear network config after 3 consecutive resets
    │   ├── ota_app.c/.h        # OTA callbacks registered into core
    │   └── tuya_config.h       # PID/license (overridable via tuya_config_secrets.h)
    ├── partitions.csv          # 4 MB flash: NVS + dual OTA slots
    └── sdkconfig.defaults      # core switches, BLE stack, target, partitions
```

## Core dependency (component manager)

`iot_demo/main/idf_component.yml`:

```yaml
dependencies:
  idf: ">=5.0"
  tuya/tuyaopen-core:
    git: https://github.com/tuya/TuyaOpen-Core.git
    version: "9e47ac3baf297fe0826a2e5963baa621336dca02"
  tuya/esp-idf:
    git: https://github.com/tuya/TuyaOpen-Core.git
    path: port/esp-idf
    version: "9e47ac3baf297fe0826a2e5963baa621336dca02"
```

The two entries correspond to:

- the repository root -> `managed_components/tuya__tuyaopen-core`;
- `port/esp-idf` (`path`) -> `managed_components/tuya__esp-idf`. The directory
  name `esp-idf` is also the component name, which is why
  `main/CMakeLists.txt` requires `esp-idf`; the component manager maps the
  short names `tuyaopen-core` / `esp-idf` to their namespaced managed
  components.

Both entries must pin the same `version`; a commit SHA or tag is recommended
(a branch also works). The manager clones the git dependency recursively, so
core's `third_party/` submodules (mbedtls, cJSON, coreMQTT, coreHTTP,
backoffAlgorithm, plus coreHTTP's llhttp) are fetched automatically.

The git clone is cached under `~/.cache/Espressif/ComponentManager` and shared
by both dependencies; the only duplicated content is the small `port/esp-idf`
directory copy (~340 KB).

### Updating the core version

1. Change both `version` fields.
2. Drop the generated state and re-resolve:

```bash
rm -rf managed_components dependencies.lock
idf.py reconfigure
idf.py build
```

### Using a local core checkout

Replace the `git` field with `override_path` (remove `git`; when both are
present, `git` wins). Relative paths are resolved from
`main/idf_component.yml`:

```yaml
dependencies:
  idf: ">=5.0"
  tuya/tuyaopen-core:
    override_path: "../../../tuyaopen-core"
  tuya/esp-idf:
    override_path: "../../../tuyaopen-core/port/esp-idf"
```

## Network model: a connected WiFi is treated as wired

Core has no WiFi abstraction:

- the **demo layer** (`main/wifi_sta.c`) connects to the AP with the esp-idf
  native API; credentials come from BLE provisioning (Tuya APP) and are
  persisted in KV, so no SSID/password is compiled in;
- the **demo layer** (`main/tuya_main.c`) runs BLE provisioning: an unbound
  device advertises as `TYBLE`, the APP sends SSID/password + the binding
  token, and the finish callback joins the network and publishes the token to
  core;
- the **port layer** (`port/esp-idf/src/port_netif.c`) listens for
  `IP_EVENT_STA_GOT_IP` / `WIFI_EVENT_STA_DISCONNECTED` and reports the link
  status to core (`core_netif_set_link_cb`; link up = IP obtained);
- the rest matches Linux: `core_network.c` calls the socket API directly (lwIP
  via the port's `interface/core_network_platform.h`), and cloud
  connection/activation/DP reuse the same core.

## Build and flash

**ESP-IDF v5.x is required** (mbedtls 3.x). In v6.2+ mbedtls was refactored
into TF-PSA-Crypto 4.x (the low-level crypto APIs such as sha256/aes/rsa/md/
ctr_drbg moved into private headers), which is incompatible with core's
TLS/crypto layer. Using the esp-idf bundled with upstream tuyaopen is
recommended (`platform/ESP32/esp-idf`, v5.4):

```bash
# esp-idf v5.4 from upstream ESP32 (mbedtls 3.x); for python 3.12 use tuyaopen's bundled copy
export PATH=/path/to/tuyaopen/.tools/python/3.12.13/cpython-3.12.13-linux-x86_64-gnu/bin:$PATH
export IDF_TOOLS_PATH=/path/to/tuyaopen/platform/ESP32/.espressif
source /path/to/tuyaopen/platform/ESP32/esp-idf/export.sh

cd iot_demo
idf.py set-target esp32s3       # or another target chip
idf.py menuconfig               # optional: core switches (BLE stays required)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The first configure downloads core from GitHub; network access (or a local
`override_path`) is required.

### Flash size and partition layout

The firmware has OTA enabled: it uses dual OTA slots, an `otadata` partition
and the `main/ota_app.c` callback injection into core. That is why the project
requires **at least 4 MB of flash** (the default `sdkconfig.defaults` selects
`CONFIG_ESPTOOLPY_FLASHSIZE_4MB` and a custom partition table). The layout in
`iot_demo/partitions.csv` uses the whole 4 MB:

| Partition | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 64 KB | core KV (certificates, activation data, WiFi credentials) |
| otadata | data/ota | 0x19000 | 8 KB | OTA slot selection |
| phy_init | data/phy | 0x1B000 | 4 KB | RF calibration data |
| ota_0 | app/ota_0 | 0x20000 | 1984 KB | application slot A |
| ota_1 | app/ota_1 | 0x210000 | 1984 KB | application slot B (OTA target) |

- Current firmware: ~1.17 MiB (`0x12C120`); each slot has ~0.77 MiB (40%) free.
- The two OTA slots are equal so an update image always fits the inactive slot.
- The old unused `storage` (spiffs) partition was removed; core KV uses NVS. If
  a future demo needs a filesystem, add a `storage` partition (and consider an
  8 MB flash).
- If the application grows beyond ~1.9 MiB per slot, use an 8 MB flash and
  enlarge both OTA slots.
- Changing `partitions.csv` requires a USB re-flash; OTA updates do not rewrite
  the partition table.

### The BLE switch is mandatory

`sdkconfig.defaults` is only consumed when `sdkconfig` is generated. An
existing `sdkconfig` (git-ignored, kept between builds) always wins, so editing
`sdkconfig.defaults` alone does not flip any switch.

`iot_demo` provisions WiFi over BLE only, so `CONFIG_ENABLE_BLUETOOTH` must be
on: `main/tuya_main.c` stops the build with an `#error` when it is disabled
(there is no other provisioning path). The committed defaults enable it:

```bash
grep CONFIG_ENABLE_BLUETOOTH sdkconfig                  # expect: =y
grep CONFIG_ENABLE_BLUETOOTH build/config/sdkconfig.h   # expect: #define ... 1
```

The BT-off configuration is only useful as a **port-level** build check; this
demo itself cannot be built with BLE off. Use a separate sdkconfig and build
directory for such experiments:

```bash
idf.py -B build-bt-off \
       -DSDKCONFIG=$PWD/sdkconfig.bt-off \
       -DSDKCONFIG_DEFAULTS=/path/to/defaults-without-bt build
```

## Provisioning and behavior

1. Power-on: NVS initializes, the WiFi station starts; credentials saved by a
   previous BLE provisioning are loaded from KV.
2. Unbound device: when core asks for the binding token
   (`STATE_TOKEN_PENDING`), the demo calls `tuya_ble_init()` +
   `ble_netcfg_start()` and advertises as `TYBLE`.
3. APP: the Tuya APP connects over BLE and sends SSID/password + the binding
   token; the finish callback saves them, stops the provisioning channel,
   joins the network and publishes `EVENT_LINK_ACTIVATE`.
4. Activated: the device connects to the cloud MQTT and sends/receives DPs;
   the BLE stack stays up for BLE DP control while the provisioning channel is
   removed on `TUYA_EVENT_MQTT_CONNECTED`.
5. Three consecutive resets clear the network configuration and activation
   data; the next boot provisions over BLE again.

Credentials (PID/license) live in `main/tuya_config.h`; copy
`tuya_config_secrets.h.example` to `tuya_config_secrets.h` to override them
locally (the file is git-ignored).

## Version and OTA

The version reported to the Tuya cloud is `PROJECT_VERSION`, configured in
menuconfig under **`IoT Demo Application -> Project version`** and passed to
`tuya_iot_init()` as `software_ver`. **For an OTA upgrade, the version must be
higher than the version currently running on the device**, otherwise the cloud
will not offer the update.

To publish an OTA update:

1. Run `idf.py menuconfig` and set `IoT Demo Application -> Project version`
   to a value higher than the running firmware (for example `1.0.0` ->
   `1.0.1`).
2. Rebuild with `idf.py build`; the OTA image is `build/iot_demo.bin`.
3. Upload that image to the Tuya IoT platform as a firmware update for the
   product, using the same version number.
4. The cloud notifies the device; the callbacks in `main/ota_app.c` write the
   image into the inactive OTA slot, switch the boot partition and reboot into
   the new firmware.
5. Keep the dual-slot partition table (already in `partitions.csv`) so the
   previous image stays available for rollback.

Notes:

- OTA compares `PROJECT_VERSION`; the ESP-IDF app descriptor version
  (`Application manager -> Project version`, `APP_PROJECT_VER`) is a separate
  value and does not drive the cloud upgrade decision.
- The device must be activated and connected to the cloud to receive the OTA
  notification.
- The OTA flow is implemented but has not been validated on real hardware in
  this project yet.

## Port mapping vs. the Linux side

Paths are relative to the core repository (the downloaded copy lives under
`managed_components/tuya__tuyaopen-core/`).

| Capability | Linux port | ESP32 port |
|---|---|---|
| Main card (OS primitives) | `port/linux/src/port_api.c` (pthread/POSIX) | `port/esp-idf/src/port_api.c` (FreeRTOS/esp-idf) |
| KV | `port/linux/src/port_kv.c` (littlefs+AES) | `port/esp-idf/src/port_kv.c` (NVS + long-key mapping) |
| netif | `port/linux/src/port_netif.c` (ioctl polled every 3s) | `port/esp-idf/src/port_netif.c` (esp_netif events) |
| network | `core_network.c` + `port/linux/interface/core_network_platform.h` (POSIX) | `core_network.c` + `port/esp-idf/interface/core_network_platform.h` (lwIP) |
| OTA | none (not registered) | `main/ota_app.c` (esp_ota, dual-slot partitions; callback injection) |
| BLE | `port/linux/src/port_bt/` (bluez/GDBus) + `port_bluetooth.c` | `port/esp-idf/src/port_bluetooth.c` (esp-idf bundled NimBLE; `CONFIG_ENABLE_BLUETOOTH`) |
| fs/media | `port_fs` / `port_ai_demo` | not adapted yet |

## Verified status and limitations

- Build verified with ESP-IDF v5.4 (0 warnings); firmware ~1.2 MB.
- BLE provisioning (APP -> WiFi -> activation -> MQTT) has been validated on
  real hardware with this demo code.
- The BLE port is **peripheral (slave) only** (no GATT client/central), which
  is sufficient for provisioning and BLE DP control.
- OTA is enabled (`main/ota_app.c` callback injection + dual-slot partition
  table) but the upgrade flow has not been validated on real hardware.
- `port_fs` / `port_ai_demo` (media) are not adapted; add them when the
  p2p/agent demos need them.
