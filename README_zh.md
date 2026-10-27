# TuyaOpen-Core ESP32 工程

[English](README.md) | **简体中文**

这是一个独立的 ESP-IDF 工程，通过 ESP-IDF 组件管理器从官方仓库
[TuyaOpen-Core](https://github.com/tuya/TuyaOpen-Core) 拉取 core 及其 ESP32
port。首次执行 configure 时，组件会被下载到 `iot_demo/managed_components/`，
本仓库不存放任何 core 源码。

`iot_demo/` 连接涂鸦云，通过 BLE 配网（WiFi 账号密码和绑定 token 由涂鸦 APP
下发，不写死在固件里），并收发 DP。它与 core 仓库里的 Linux demo 结构平行。

## 工程结构

```
TuyaOpen-Core-esp32/
└── iot_demo/
    ├── CMakeLists.txt          # esp-idf 工程入口（不再使用 EXTRA_COMPONENT_DIRS）
    ├── main/
    │   ├── idf_component.yml   # 组件管理器依赖（core + port）
    │   ├── main.c              # app_main：nvs_flash_init -> tuya_app_main()
    │   ├── tuya_main.c         # 应用主流程 + BLE 配网（token-get port）
    │   ├── wifi_sta.c/.h       # WiFi station（凭据来自 BLE，持久化到 KV）
    │   ├── reset_netcfg.c/.h   # 连续 3 次复位后清除网络配置
    │   ├── ota_app.c/.h        # OTA 回调注入 core
    │   └── tuya_config.h       # PID/授权码（可用 tuya_config_secrets.h 覆盖）
    ├── partitions.csv          # 4MB flash：NVS + 双 OTA 分区
    └── sdkconfig.defaults      # core 开关、BLE 协议栈、目标芯片、分区
```

## Core 依赖（组件管理器）

`iot_demo/main/idf_component.yml`：

```yaml
dependencies:
  idf: ">=5.0"
  tuya/tuyaopen-core:
    git: https://github.com/tuya/TuyaOpen-Core.git
    version: "84b2cbc6d3c2ab13b3261b21fa784b2fdacca807"
  tuya/esp-idf:
    git: https://github.com/tuya/TuyaOpen-Core.git
    path: port/esp-idf
    version: "84b2cbc6d3c2ab13b3261b21fa784b2fdacca807"
```

两条依赖分别对应：

- 仓库根目录 -> `managed_components/tuya__tuyaopen-core`；
- `port/esp-idf`（用 `path` 指定）-> `managed_components/tuya__esp-idf`。
  该目录名 `esp-idf` 同时就是组件名，所以 `main/CMakeLists.txt` 里写
  `REQUIRES esp-idf`；组件管理器会把短名 `tuyaopen-core` / `esp-idf` 映射到
  带命名空间的 managed 组件。

两条依赖的 `version` 必须一致，推荐用 commit SHA 或 tag（分支也可以）。组件管理器
会递归克隆 git 依赖，因此 core 的 `third_party/` 子模块（mbedtls、cJSON、
coreMQTT、coreHTTP、backoffAlgorithm，以及 coreHTTP 的 llhttp）会自动拉全。

git 克隆缓存在 `~/.cache/Espressif/ComponentManager`，两条依赖共享同一份；
真正重复的只有 `port/esp-idf` 目录那份约 340 KB 的拷贝。

### 升级 core 版本

1. 修改两处 `version`；
2. 清掉生成状态后重新解析：

```bash
rm -rf managed_components dependencies.lock
idf.py reconfigure
idf.py build
```

### 使用本地 core 源码

把 `git` 字段换成 `override_path`（两者不要同时写，同时存在时 `git` 优先）。
相对路径以 `main/idf_component.yml` 所在目录为基准：

```yaml
dependencies:
  idf: ">=5.0"
  tuya/tuyaopen-core:
    override_path: "../../../tuyaopen-core"
  tuya/esp-idf:
    override_path: "../../../tuyaopen-core/port/esp-idf"
```

## 网络模型：已连接的 WiFi 按有线处理

core 没有 WiFi 抽象：

- **demo 层**（`main/wifi_sta.c`）用 esp-idf 原生 API 连接 AP；账号密码来自 BLE
  配网（涂鸦 APP），并持久化在 KV 中，固件里没有写死的 SSID/密码；
- **demo 层**（`main/tuya_main.c`）负责 BLE 配网：未绑定设备以 `TYBLE` 广播，
  APP 下发 SSID/密码和绑定 token，finish 回调入网并把 token 发布给 core；
- **port 层**（`port/esp-idf/src/port_netif.c`）监听 `IP_EVENT_STA_GOT_IP` /
  `WIFI_EVENT_STA_DISCONNECTED`，把链路状态上报 core
  （`core_netif_set_link_cb`；链路 up = 已拿到 IP）；
- 其余流程与 Linux 侧一致：`core_network.c` 直接调用 socket API（通过 port 的
  `interface/core_network_platform.h` 走 lwIP），连云/激活/DP 复用同一套 core。

## 构建与烧录

**需要 ESP-IDF v5.x**（mbedtls 3.x）。v6.2+ 把 mbedtls 重构为 TF-PSA-Crypto
4.x（sha256/aes/rsa/md/ctr_drbg 等底层接口移入私有头），与 core 的 TLS/加密层
不兼容。建议使用上游 tuyaopen 自带的 esp-idf（`platform/ESP32/esp-idf`，v5.4）：

```bash
# esp-idf v5.4（mbedtls 3.x）；Python 3.12 用 tuyaopen 自带的那份
export PATH=/path/to/tuyaopen/.tools/python/3.12.13/cpython-3.12.13-linux-x86_64-gnu/bin:$PATH
export IDF_TOOLS_PATH=/path/to/tuyaopen/platform/ESP32/.espressif
source /path/to/tuyaopen/platform/ESP32/esp-idf/export.sh

cd iot_demo
idf.py set-target esp32s3       # 或其他目标芯片
idf.py menuconfig               # 可选：配置 core 开关（BLE 必须保持开启）
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

首次 configure 会从 GitHub 下载 core，需要网络（或改用本地 `override_path`）。

### Flash 大小与分区布局

当前固件已开启 OTA：使用双 OTA 槽、`otadata` 分区，并通过 `main/ota_app.c`
把 OTA 回调注入 core。因此 flash **至少需要 4 MB**（默认 `sdkconfig.defaults`
选择 `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` 并使用自定义分区表）。
`iot_demo/partitions.csv` 的布局正好用满 4 MB：

| 分区 | 类型 | 偏移 | 大小 | 用途 |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 64 KB | core KV（证书、激活数据、WiFi 凭据） |
| otadata | data/ota | 0x19000 | 8 KB | OTA 启动槽选择 |
| phy_init | data/phy | 0x1B000 | 4 KB | RF 校准数据 |
| ota_0 | app/ota_0 | 0x20000 | 1984 KB | 应用槽 A |
| ota_1 | app/ota_1 | 0x210000 | 1984 KB | 应用槽 B（OTA 目标） |

- 当前固件约 1.17 MiB（`0x12C120`），每个 OTA 槽剩余约 0.77 MiB（40%）。
- 两个 OTA 槽大小相同，保证升级镜像总能放进非当前槽。
- 原布局中未使用的 `storage`（spiffs）分区已删除；core 的 KV 使用 NVS。后续
  demo 如果需要文件系统，再加回 `storage` 分区（并考虑改用 8 MB flash）。
- 如果应用超过单槽约 1.9 MiB，请改用 8 MB flash 并扩大两个 OTA 槽。
- 修改 `partitions.csv` 后需要通过 USB 重新烧录；OTA 升级不会更新分区表。

### BLE 开关是必需的

`sdkconfig.defaults` 只在生成 `sdkconfig` 时生效。已存在的 `sdkconfig`
（git 忽略、跨构建保留）优先级更高，所以只改 `sdkconfig.defaults` 不会切换任何开关。

`iot_demo` 只支持 BLE 配网，因此 `CONFIG_ENABLE_BLUETOOTH` 必须开启：
关闭时 `main/tuya_main.c` 会用 `#error` 中断构建（没有其他配网路径）。仓库里的
默认配置已开启：

```bash
grep CONFIG_ENABLE_BLUETOOTH sdkconfig                  # 期望：=y
grep CONFIG_ENABLE_BLUETOOTH build/config/sdkconfig.h   # 期望：#define ... 1
```

关闭 BT 的配置只适合做 **port 层** 的编译检查，本 demo 本身无法在 BLE 关闭时构建。
这类实验请使用独立的 sdkconfig 和构建目录：

```bash
idf.py -B build-bt-off \
       -DSDKCONFIG=$PWD/sdkconfig.bt-off \
       -DSDKCONFIG_DEFAULTS=/path/to/defaults-without-bt build
```

## 配网与运行流程

1. 上电：NVS 初始化，WiFi station 启动；如果之前配过网，从 KV 读取凭据直接入网。
2. 未绑定设备：core 请求绑定 token（`STATE_TOKEN_PENDING`）时，demo 调用
   `tuya_ble_init()` + `ble_netcfg_start()`，以 `TYBLE` 广播。
3. APP 配网：涂鸦 APP 通过 BLE 下发 SSID/密码和绑定 token；finish 回调保存凭据、
   停止配网通道、连接 WiFi，并发布 `EVENT_LINK_ACTIVATE`。
4. 激活后：连接云端 MQTT，收发 DP；BLE 协议栈保持运行以支持 BLE DP 控制，
   配网通道在 `TUYA_EVENT_MQTT_CONNECTED` 时关闭。
5. 连续 3 次复位会清除网络配置和激活数据，下次启动重新 BLE 配网。

PID/授权码在 `main/tuya_config.h`；把 `tuya_config_secrets.h.example` 复制为
`tuya_config_secrets.h` 可在本地覆盖（该文件已被 git 忽略）。

## 版本与 OTA

上报给涂鸦云的版本号是 `PROJECT_VERSION`，在 menuconfig 的
**`IoT Demo Application -> Project version`** 中配置，并作为 `software_ver`
传给 `tuya_iot_init()`。**做 OTA 升级时，这个版本号必须比设备当前运行的版本高**，
否则云端不会下发升级。

发布一次 OTA 的流程：

1. 运行 `idf.py menuconfig`，把 `IoT Demo Application -> Project version`
   改成比当前运行版本更高的值（例如 `1.0.0` -> `1.0.1`）；
2. 重新执行 `idf.py build`，升级镜像为 `build/iot_demo.bin`；
3. 把该镜像以相同的版本号上传到涂鸦 IoT 平台，配置为对应产品的固件升级；
4. 云端向设备下发升级通知，`main/ota_app.c` 中的回调把镜像写入非当前 OTA 槽，
   完成后切换启动分区并重启进入新固件；
5. 保持双 OTA 槽分区表（`partitions.csv` 已满足），旧镜像可保留用于回滚。

注意：

- OTA 比较的是 `PROJECT_VERSION`；ESP-IDF 的 app 描述版本
  （`Application manager -> Project version`，`APP_PROJECT_VER`）是另一个值，
  不参与云端升级判断；
- 设备必须已激活并连接云端，才能收到 OTA 通知；
- OTA 流程已实现，但本工程尚未在真机上验证完整升级。

## 与 Linux 侧的 Port 对照

路径相对 core 仓库（下载后的副本在 `managed_components/tuya__tuyaopen-core/`）。

| 能力 | Linux port | ESP32 port |
|---|---|---|
| 主卡（OS 原语） | `port/linux/src/port_api.c`（pthread/POSIX） | `port/esp-idf/src/port_api.c`（FreeRTOS/esp-idf） |
| KV | `port/linux/src/port_kv.c`（littlefs+AES） | `port/esp-idf/src/port_kv.c`（NVS + 长 key 映射） |
| netif | `port/linux/src/port_netif.c`（每 3s 轮询 ioctl） | `port/esp-idf/src/port_netif.c`（esp_netif 事件） |
| 网络 | `core_network.c` + `port/linux/interface/core_network_platform.h`（POSIX） | `core_network.c` + `port/esp-idf/interface/core_network_platform.h`（lwIP） |
| OTA | 无（未注册） | `main/ota_app.c`（esp_ota，双分区；回调注入） |
| BLE | `port/linux/src/port_bt/`（bluez/GDBus）+ `port_bluetooth.c` | `port/esp-idf/src/port_bluetooth.c`（esp-idf 自带 NimBLE；`CONFIG_ENABLE_BLUETOOTH`） |
| fs/媒体 | `port_fs` / `port_ai_demo` | 尚未适配 |

## 验证状态与限制

- 已用 ESP-IDF v5.4 构建验证（0 warning）；固件约 1.2 MB。
- BLE 配网（APP -> WiFi -> 激活 -> MQTT）已用该 demo 代码在真机验证通过。
- BLE port 仅支持 **peripheral（从机）**（无 GATT client/central），足以满足
  配网和 BLE DP 控制。
- OTA 已开启（`main/ota_app.c` 回调注入 + 双分区表），但升级流程尚未真机验证。
- `port_fs` / `port_ai_demo`（媒体）尚未适配，等 p2p/agent demo 需要时再加。

---

TuyaOpen-Core 官方仓库：<https://github.com/tuya/TuyaOpen-Core>
