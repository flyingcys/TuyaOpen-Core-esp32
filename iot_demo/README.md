# iot_demo (ESP32 / esp-idf)

An ESP32 example that connects to the Tuya cloud and sends/receives DPs; it is
structurally parallel to `linux/examples/iot_demo`. The only difference is
network access: core has no WiFi abstraction, so the WiFi connection is done by
this demo (`main/wifi_sta.c`), and once connected the network is treated as
wired (the port's `port_netif` listens for IP events and reports the link;
sockets go through lwIP).

WiFi credentials are **not compiled in**: an unbound device is provisioned by
the Tuya APP over BLE, and the credentials are persisted in KV so later boots
join the network without provisioning again.

## Structure

```
iot_demo/
  CMakeLists.txt          # esp-idf project entry (EXTRA_COMPONENT_DIRS: core + the esp-idf port component)
  main/
    main.c                # app_main: nvs_flash_init -> tuya_app_main()
    tuya_main.c           # Application main flow + BLE provisioning (token-get port)
    wifi_sta.c/.h         # WiFi station (no built-in credentials; KV-persisted)
    reset_netcfg.c/.h     # Clears network config after 3 consecutive resets
    tuya_config.h         # PID/license code (secrets header can override)
    idf_component.yml     # idf dependency only (core is a local component, not downloaded)
  partitions.csv          # 4MB flash: 64KB nvs + dual OTA slots
  sdkconfig.defaults      # Default configuration (core switches, BLE stack, target chip, partitions)
```

The ESP32 port lives in the core repository, not in this project:
`tuyaopen-core/port/esp-idf` holds the implementation and is itself the
esp-idf component `esp-idf` (esp-idf names components after their directories).
The project adds both the core repository and that directory with
`EXTRA_COMPONENT_DIRS`; `main` depends on `tuyaopen-core` and `esp-idf`.

## Provisioning (BLE only)

There is no compiled-in SSID/password and no menuconfig WiFi entry. The flow is:

1. **Boot**: the WiFi station (event loop, station netif, driver) is
   initialized without credentials. If KV holds credentials from an earlier
   provisioning, the station joins that AP directly.
2. **Unbound device**: when core asks for the binding token
   (`STATE_TOKEN_PENDING`), the demo calls `tuya_ble_init()` +
   `ble_netcfg_start()` and advertises as `TYBLE`.
3. **APP**: the Tuya APP connects over BLE and sends the WiFi SSID/password
   plus the binding token.
4. **Finish**: the callback saves the credentials to KV, removes the
   provisioning channel, joins the WiFi network and publishes
   `EVENT_LINK_ACTIVATE` with the token (region | token | regist_key); core
   then activates the device.
5. **After activation**: the BLE stack stays up for BLE DP control; the
   provisioning channel is removed on `TUYA_EVENT_MQTT_CONNECTED`.
6. **Factory reset** (three quick resets): the stored WiFi credentials are
   cleared together with the activation data; the next boot provisions over
   BLE again.

`CONFIG_ENABLE_BLUETOOTH` is mandatory: `main/tuya_main.c` fails the build with
an `#error` when it is disabled, because there is no other provisioning path.

## Build and flash

**Requires esp-idf v5.x** (mbedtls 3.x; in v6.2+ mbedtls 4.x/TF-PSA-Crypto
moves the low-level crypto API into private headers, which is incompatible with
core). Using the esp-idf bundled with upstream tuyaopen is recommended
(`platform/ESP32/esp-idf` v5.4).

```bash
source <esp-idf>/export.sh           # v5.x
cd esp32/examples/iot_demo
idf.py set-target esp32s3            # or another target chip (esp32/esp32c3/...)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Configuration

- **BLE**: enabled by `sdkconfig.defaults` (`CONFIG_ENABLE_BLUETOOTH`,
  `CONFIG_BT_ENABLED`, `CONFIG_BT_NIMBLE_ENABLED`, VHCI mode, peripheral +
  broadcaster roles, preferred MTU 256). Bluedroid is off; the port talks to
  the esp-idf bundled NimBLE host directly.
- **Credentials** (Tuya PID/license): `tuya_config.h` already ships the same
  PID/license code as linux iot_demo; when switching products, change
  `tuya_config.h` or use `tuya_config_secrets.h` (copy
  `tuya_config_secrets.h.example`, which is ignored by .gitignore).
- **core switches**: the `tuyaopen-core` menu in `menuconfig` (P2P/AI are off
  by default; BLE is required, see above).

## Behavior

1. Power-on -> NVS initialization -> WiFi station init; credentials are loaded
   from KV when a previous BLE provisioning stored them;
2. Not activated: the token-get port starts BLE provisioning; the APP sends
   WiFi credentials + the binding token, the device joins the network and
   activates;
3. Activated: connects to the cloud MQTT and sends/receives DPs (received DPs
   are reported back as-is);
4. Three consecutive resets (each interval <5s) trigger `tuya_iot_reset` to
   clear the network configuration and activation data, then reboot; the next
   boot provisions over BLE again.

## Known limitations

- The BLE port is **peripheral (slave) only** (no GATT client/central), which
  is sufficient for provisioning and BLE DP control.
- The BLE provisioning path is verified at build/link level; APP provisioning
  over BLE has not been validated on real hardware yet.
- The OTA path is wired up (`main/ota_app.c` callback injection + dual-slot
  partition table), but has not been validated on real hardware;
- `port_fs`/`port_ai_demo` (media) are not adapted -- add them when the
  p2p/agent demos need them.
