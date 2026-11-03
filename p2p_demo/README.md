# p2p_demo (ESP32 scaffold)

ESP-IDF scaffold for the TuyaOpen-Core P2P demo.

- `components/tuyaopen-core` and `components/esp-idf` are relative symlinks to
  the sibling `TuyaOpen-Core` checkout (`../../TuyaOpen-Core`). This lets the
  demo build against local core changes without re-downloading managed
  components.
- `CONFIG_ENABLE_TUYA_P2P=y` and `CONFIG_P2P_ICE_BACKEND_LIBRE=y` select the
  libre (re) ICE/STUN/TURN backend; ESP-IDF's own lwIP and pthread are used.
- The application currently reuses the iot_demo BLE provisioning flow. The
  Linux demo's virtual camera/media chain and P2P callbacks are being ported
  next.

Build and flash:

```sh
idf.py -B build build
idf.py -p /dev/ttyUSB0 flash
```
