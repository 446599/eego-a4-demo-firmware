# EegoRead A4 Developer Demo

EegoRead A4 的独立开发板演示固件，基于 ESP-IDF 6.0.1，目标芯片为
ESP32-S3。固件上电后直接进入开发者菜单，用于检查屏幕、触摸、实体键、前光、
电池、RTC、microSD、Wi-Fi 和 BLE。

固件版本为 `V1.1.0-DEMO`。详细硬件、驱动和验收说明见
[开发者演示固件说明](docs/DEVELOPER_DEMO.md)。

## 快速开始

### 构建

```sh
source scripts/idf_env.sh
idf.py -B build-final set-target esp32s3
idf.py -B build-final build
```

首次建立 `build-final` 后，后续增量构建只需：

```sh
source scripts/idf_env.sh
idf.py -B build-final build
```

产物：

```text
build-final/eegoread_firmware.bin
```

### 烧录

```sh
bash scripts/flash_app_verified.sh /dev/cu.usbmodem1101
```

macOS 下请将 `/dev/cu.usbmodem1101` 替换为实际串口。脚本会：

- 检查镜像版本和 app 分区容量；
- 使用 esptool stub 将镜像写入 app0 `0x10000`；
- 写入后执行 hash 校验；
- 重启设备并检查 `V1.1.0-DEMO`、demo 入口和启动标记。

### 如果仍启动生产版

当前分区布局同时存在 app0 和 app1。若 OTA 元数据指向 app1，即使 demo 已正确写入
app0，设备仍会启动旧生产固件。串口日志会出现：

```text
Loaded app from partition at offset 0x510000
```

此时只清空 OTA 选择区，不要整片擦除：

```sh
source scripts/idf_env.sh
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 921600 \
  --before default-reset --after hard-reset \
  erase-region 0xe000 0x2000
```

该操作不会删除 NVS、分区表、app1 或 SD 卡内容。清空后 bootloader 会选择 app0。

## 页面

菜单共 10 项，分两页显示。

| 页面 | 功能 |
| --- | --- |
| Overview | 型号、电源板配置、固件、MAC、电量、RTC 和 SD 摘要 |
| Display / Refresh | GC 全刷、DU 快刷、耗时统计和测试图案 |
| Touch | GSL1680 原始/校准坐标、触点和 24 点触摸轨迹 |
| Hardware Keys | UP/DOWN/POWER 电平、按下/抬起和累计次数 |
| Frontlight | LM3630A 冷光、暖光两个独立滑块 |
| Power / Battery | CW2015/旧板 ADC、充电状态和 GPIO6 电源配置 |
| RTC | PCF8563 只读日期和时间 |
| microSD | 挂载、重挂载和根目录只读列表 |
| Wi-Fi Scan | 主动扫描 SSID、RSSI 和网络数量 |
| BLE Observer | NimBLE observer 扫描设备数量和最强 RSSI |

## 操作

### 触摸

- 点击屏幕右下角箭头打开/关闭菜单；
- 菜单内点击条目切换页面；
- `Touch` 页按住并拖动可观察触点轨迹；
- `Frontlight` 页拖动 COLD/WARM 滑块实时调整双通道亮度。

### 实体键

| 场景 | UP | DOWN | POWER |
| --- | --- | --- | --- |
| 普通页面 | 执行页内主操作 | 强制整页 GC | 打开菜单 |
| 菜单 | 上一项 | 下一项 | 进入选中项 |
| Hardware Keys | 捕获按键状态 | 捕获按键状态 | 捕获按键状态 |

`Hardware Keys` 页独占三个按键，以便按下和抬起都能局部刷新状态表。该页仍可通过
右下角触摸箭头进入菜单。

## 硬件版本

### A4 与 A4 Lite

两者通过 LM3630A 前光控制器是否在线区分：

- LM3630A `0x36` 有应答：`A4`；
- LM3630A `0x36` 无应答：`A4 Lite`。

### CW2015 与旧电源板

电源板配置与 A4/A4 Lite 是独立维度：

- CW2015 `0x62` 在线：新电源板，GPIO6 低电平启用 SD 电源；
- CW2015 不存在：旧电源板，GPIO10 使用校准 ADC，GPIO6 保持高电平。

页面会同时显示这两类信息，不能互相替代。

## 主要硬件

| 项目 | 配置 |
| --- | --- |
| MCU | ESP32-S3，16 MiB Flash，8 MiB Octal PSRAM |
| 屏幕 | UC8279C 兼容，768 × 552，SPI3，20 MHz |
| 触摸 | GSL1680，I2C0 `0x40` |
| 前光 | LM3630A，I2C `0x36`，HWEN GPIO12 |
| 电池 | CW2015 `0x62` 或旧板 GPIO10 ADC |
| 充电检测 | GPIO11，高电平有效 |
| RTC | PCF8563，I2C `0x51` |
| microSD | SPI2：SCK39 / MOSI38 / MISO40 / CS47 |

完整 GPIO、驱动调用和波形说明见
[开发者演示固件说明](docs/DEVELOPER_DEMO.md)。

## 软件结构

```text
main/
  developer_demo.c             开机、屏幕、启动页和事件循环入口
  devdemo/
    devdemo_app.h              页面回调契约
    devdemo_app_loop.c         触摸、按键、菜单、切页和刷新
    devdemo_app_registry.c     页面注册表和菜单顺序
    devdemo_ui.*               公共 1bpp 绘制原语
    devdemo_ui_menu.*          页面菜单
    demo_fonts.*               精简 demo 字体子集
    apps/                      每个外设一个独立页面

components/
  a4_board/                    GPIO、I2C、触摸、前光、电池、RTC、SD 电源
  a4_sd/                       microSD 挂载和目录列表
  display_manager/             显示刷新互斥和统计
  epd_uc8279c/                 EPD SPI、GC/DU 和局部刷新
  fatfs/                       ESP-IDF FatFs exFAT 覆盖层
```

新增页面时：

1. 在 `main/devdemo/apps/` 新建 `app_*.c`；
2. 实现需要的 `devdemo_app_t` 回调；
3. 将页面加入 `devdemo_app_registry.c`；
4. 不需要修改通用事件循环。

## 刷新策略

- 画布为竖屏 `552 × 768` 1bpp，`1=白、0=黑`；
- 提交前旋转为面板 `768 × 552`；
- 首次、强制刷新和每 8 次软刷新附近使用 GC；
- 普通页面更新使用 DU；
- `Touch` 页拖动使用面板局部 DU，只刷新触摸图区域；
- `Hardware Keys`、`Frontlight` 滑块等交互页也使用局部区域刷新。

## 无线演示

Wi-Fi 和 BLE 都只做扫描：

- Wi-Fi 使用 `WIFI_STORAGE_RAM`，不读取或保存 AP 凭据，扫描结束关闭射频；
- BLE 只启动 NimBLE observer，不创建 GATT、不连接、不配对、不绑定；
- 离开无线页面会停止并释放当前射频资源。

## 测试

运行主机静态测试：

```sh
uv run pytest -q
```

测试会检查：

- 仓库不存在阅读器、OTA、云服务和传输组件；
- 不存在正式入口 `main/app_main.c`；
- Wi-Fi/BLE 保持扫描模式；
- 页面注册表包含 10 个独立 demo；
- README 和详细文档存在必要说明。

构建产物信息：

```sh
python -m esptool image-info build-final/eegoread_firmware.bin
shasum -a 256 build-final/eegoread_firmware.bin
```

## 常见问题

### 串口不存在或刷写前掉线

重新插拔 USB，然后检查：

```sh
find /dev -maxdepth 1 -name 'cu.usbmodem*' -print
```

存在多个串口时，可逐个读取 MAC：

```sh
source scripts/idf_env.sh
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem2101 read-mac
```

### 烧录成功但仍是生产版

检查启动日志是否从 `0x510000` 加载，并按上文清空 `otadata` 选择区。

### 前光页面显示 NOT FITTED

该设备按前光控制器判断为 A4 Lite。固件不会对不存在的 `0x36` 设备写寄存器。

### 电量来源与预期不同

确认设备使用的是 CW2015 新电源板还是 GPIO10 ADC 旧板。已经绑定 CW2015 的设备不会
因一次读取失败自动切换为 ADC。

## 证据边界

- 构建通过不等于烧录成功；
- 烧录 hash 通过不等于应用启动；
- 启动日志通过不等于屏幕、触摸或前光在光学上正常；
- Wi-Fi/BLE 扫描成功不等于连接、配对或传输能力可用。

设备验收时应分别记录构建、镜像、启动、功能、光学和无线证据。

## 限制

- 不用于量产或正式发布；
- 不提供 OTA 功能；
- 不连接 Wi-Fi AP；
- 不连接或绑定 BLE 设备；
- 不修改、格式化 SD 卡；
- 不写 RTC；
- 不读取书籍，也不实现阅读 UI。

## 字体与许可

`main/devdemo/demo_fonts.c` 是仅包含 UI 所需字形的生成子集。相关字体许可和声明位于
[`LICENSES/fonts`](LICENSES/fonts)。

## 维护规则

仓库维护边界见 [AGENTS.md](AGENTS.md)。提交前至少执行：

```sh
git diff --check
uv run pytest -q
source scripts/idf_env.sh
idf.py -B build-final build
```
