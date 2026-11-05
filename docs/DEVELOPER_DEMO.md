# EegoRead 独立开发演示固件

本文对应独立的私有精简固件工程，版本标记为 `V1.1.0-DEMO`。源码树只保留
板级、显示、存储、无线扫描和 demo 页面所需的代码，不包含正式阅读器实现。

## 1. 定位与边界

该镜像是独立的开发者演示系统，不是正式阅读器中的一个页面。上电后先显示演示
启动页，然后进入 `Overview`；通过底栏菜单把手或实体 POWER 键打开页面菜单。
启动路径不会初始化以下模块：

- 阅读会话、书库、EPUB/TXT 解析和文件传输；
- OTA、云端账户、绑定、同步及发布服务；
- USB MSC、BLE 文件传输和任何配对流程；
- 自动锁屏、运输模式和正式阅读器的休眠状态机。

这样做的目的，是让开发者在排查屏幕、触摸、电源、存储和无线问题时，不受阅读器
业务状态干扰。该工程不用于发布 OTA，也不应把演示镜像当作正式固件。

页面集合覆盖：

- 屏幕全刷、快刷和刷新计数；
- GSL1680 触摸坐标与三个实体键；
- LM3630A 前光和 A4/A4 Lite 型号识别；
- CW2015 或旧板 ADC 电量，以及充电检测；
- PCF8563 RTC 只读时间；
- SD 卡挂载和根目录统计；
- Wi-Fi 主动扫描；
- BLE NimBLE observer 扫描。

## 页面与交互

演示固件采用独立硬件 demo 的分层方式：

- `main/developer_demo.c` 只负责开机、屏幕初始化、启动页和进入事件循环；
- `main/devdemo/devdemo_app.h` 定义页面回调契约；
- `main/devdemo/devdemo_app_loop.c` 统一处理触摸、三键、菜单、页面切换和刷新；
- `main/devdemo/devdemo_app_registry.c` 是唯一页面顺序表；
- `main/devdemo/apps/` 中每个外设使用一个独立页面文件；
- 页面只依赖公共 `devdemo_ui`，不直接引用其他页面。

| 页面 | 文件 | 内容 |
| --- | --- | --- |
| Overview | `app_overview.c` | 型号、电源配置、固件、MAC、电量、RTC 和 SD 摘要 |
| Display / Refresh | `app_display.c` | GC、DU 快刷、耗时和测试图案 |
| Touch | `app_touch.c` | 原始/校准坐标、触点和触摸次数 |
| Hardware Keys | `app_keys.c` | UP/DOWN/POWER 状态与次数 |
| Frontlight | `app_frontlight.c` | A4/A4 Lite 判断和冷/暖双滑块 |
| Power / Battery | `app_power.c` | CW2015/ADC、充电、GPIO6 电源板型 |
| RTC | `app_rtc.c` | PCF8563 只读日期时间 |
| microSD | `app_sd.c` | 挂载和根目录只读列表 |
| Wi-Fi Scan | `app_wifi.c` | 主动扫描、SSID 数量和最强 RSSI |
| BLE Observer | `app_ble.c` | NimBLE observer、设备数量和最强 RSSI |

菜单每页显示 8 项。触摸底栏右侧箭头打开菜单；触摸菜单行选择页面。实体键映射：

- UP：执行当前页面的主操作；Wi-Fi/BLE 页用于开始扫描；
- DOWN：强制整页 GC 刷新；
- POWER：打开/关闭页面菜单；`Hardware Keys` 诊断页例外，POWER 用于采样本页状态；
- 菜单内 UP/DOWN：移动高亮项，POWER：进入选中页面。

## 2. 两个不同维度的硬件版本

不要把“A4 / A4 Lite”与“新电源板 / 旧电源板”混为一谈。

### A4 与 A4 Lite

判断依据是 LM3630A 前光控制器是否实际应答：

- I2C 地址 `0x36` 可访问：`A4`；
- I2C 地址 `0x36` 不存在：`A4 Lite`。

该判断只描述前光硬件。A4 也可能使用旧电源板，A4 Lite 也可能使用新电源板。
演示页会在标题下同时显示前光型号和电源配置。

### CW2015 电源板与 ADC 旧板

判断依据是 `a4_board_init()` 对 `0x62` CW2015 的探测和已绑定板型：

- 检测到 CW2015：新电源配置，GPIO6 低电平启用 SD 电源，电量来自 CW2015；
- 未检测到 CW2015：旧电源配置，GPIO10 使用校准 ADC 电压回退，GPIO6
  保持高电平。

已经成功检测到 CW2015 后，单次读数失败不会自动降级为 ADC。演示页使用
`a4_board_sd_power_can_suspend()` 显示当前绑定结果。

## 3. 硬件连接

### 屏幕 UC8279C

| 信号 | GPIO/参数 |
| --- | --- |
| SPI | `SPI3_HOST` |
| SCLK | GPIO42 |
| MOSI | GPIO45 |
| CS | GPIO21 |
| DC | GPIO14 |
| RST | GPIO13 |
| BUSY | GPIO41 |
| SPI 时钟 | 20 MHz |

逻辑画布为 `552 x 768` 的 1bpp 位图，`1=白、0=黑`。提交到面板前通过
`a4_rotate_to_panel(..., false)` 转换为横向 `768 x 552` 面板帧。

首次显示和定期维护使用 `epd_uc8279c_show_frame()` 的 GC 波形；普通状态更新使用
`epd_uc8279c_show_frame_fast()` 的 DU 快刷。演示固件每 8 次软刷新安排一次 GC；
`Display / Refresh` 页可单独执行 GC、FAST 和全屏测试图案。

### 触摸 GSL1680

| 信号 | GPIO/参数 |
| --- | --- |
| I2C | `I2C_NUM_0` |
| SDA/SCL | GPIO2 / GPIO1 |
| 地址 | `0x40` |
| INT | GPIO9 |
| RESET | GPIO3 |

演示固件轮询 `a4_board_read_touch()`。控制器原生的 `raw_x >= 920` 读数视为无效，
其余坐标通过 `touch_calibration_map()` 五点分段校准到 `552 x 768` 画布。
`Touch` 页在按下后保留 24 点轨迹；拖动期间约每 80 ms 生成一次区域刷新，通过
`a4_rotate_portrait_dirty_to_panel()` 转换坐标后调用
`epd_uc8279c_show_frame_fast_partial()`，只提交触摸图区域。抬手后整页刷新以更新
坐标统计。

### 实体键

| 按键 | GPIO | 有效电平 |
| --- | --- | --- |
| UP | GPIO5 | 低 |
| DOWN | GPIO7 | 低 |
| POWER | GPIO8 | 高 |

演示固件每 20 ms 采样一次输入，并使用 `a4_board_button_pressed()` 做 5 次、约
100 ms 的去抖。页面外
UP 执行页内操作、DOWN 强制 GC、POWER 打开菜单；菜单内 UP/DOWN 移动、POWER 选择。
`Hardware Keys` 页接管三个按键：按下和抬起都会局部刷新 GPIO 电平与计数，菜单仍可
通过底栏右下角箭头打开。
演示模式不会把 POWER 映射为正式阅读器的关机或休眠。

### 前光 LM3630A

| 信号 | GPIO/参数 |
| --- | --- |
| I2C 地址 | `0x36` |
| HWEN | GPIO12 |

调用 `a4_board_set_frontlight(cold, warm)`。演示页提供两个独立的 0–100%
水平滑块，分别对应冷光和暖光通道；拖动中约每 100 ms 写入一次并局部刷新当前滑块，
抬手后整页刷新。未检测到 `0x36` 时显示 `NOT FITTED`，不会写总线。

### 电池、充电和电源锁存

| 功能 | 连接 |
| --- | --- |
| CW2015 | I2C `0x62` |
| 旧板电池采样 | GPIO10 校准 ADC |
| 充电检测 | GPIO11，高电平有效 |
| MCU 电源锁存 | GPIO4 |
| SD 电源控制 | GPIO6 |

初始化后调用：

- `a4_board_read_battery_percent()`
- `a4_board_battery_present()`
- `a4_board_is_charging()`
- `a4_board_sd_power_can_suspend()`

### RTC PCF8563

| 信号 | GPIO/参数 |
| --- | --- |
| I2C 地址 | `0x51` |
| 总线 | `I2C_NUM_0` |

演示固件只调用 `a4_board_read_datetime()`，不会写 RTC，也不会把系统时间回写设备。

### microSD

| 信号 | GPIO/参数 |
| --- | --- |
| SPI | `SPI2_HOST` |
| SCLK | GPIO39 |
| MOSI | GPIO38 |
| MISO | GPIO40 |
| CS | GPIO47 |

演示页调用 `a4_sd_mount()`，再用 `a4_sd_list("", ...)` 读取根目录统计。示例不会
创建、删除或修改 SD 文件。

## 4. Wi-Fi 扫描（scan-only）

Wi-Fi 演示使用 ESP-IDF Wi-Fi 驱动，而不是正式阅读器的持久化 Wi-Fi 配置流程：

1. 在 `Wi-Fi Scan` 页点击 SCAN 或按 UP 时创建默认 STA netif 和 Wi-Fi 驱动；
2. `WIFI_STORAGE_RAM`，不读取或写入站点 SSID/密码；
3. 设置中国区 `CN`，扫描信道 1-13；
4. 启动一次异步主动扫描；
5. 扫描结束后读取 SSID、RSSI，按 SSID 去重；
6. 无论成功、失败还是超时，都调用 `esp_wifi_stop()` 关闭射频。

离开 Wi-Fi 页会停止扫描并关闭射频。演示模式不调用
`esp_wifi_set_config()`、`wifi_manager_connect()` 或
`wifi_manager_set_enabled()`，不会修改已保存的 Wi-Fi 策略。

## 5. BLE observer 扫描

BLE 演示只启动 NimBLE host 和 observer：

1. 在 `BLE Observer` 页点击 SCAN 或按 UP 时创建 `ESP_PM_NO_LIGHT_SLEEP` 锁；
2. `nimble_port_init()` 后设置 reset/sync 回调；
3. sync 后推断本机地址类型；
4. `ble_gap_disc()` 进行约 4 秒通用观察扫描；
5. 按地址去重，记录数量和最强 RSSI；
6. 扫描结束后 `nimble_port_stop()`、`nimble_port_deinit()` 释放 host、controller
   和 PM 锁。

不会调用 `ble_transfer_start()`、`ble_hid_enable()`、`ble_gap_connect()`、
`ble_gap_adv_start()`、GATT 服务初始化或 bonding/store 初始化，因此不会建立连接、
配对、绑定或创建 BLE 身份资料。

Wi-Fi 和 BLE 页面互斥：切换到另一无线页面时，前一页的 `on_exit` 会停止扫描并
释放射频。菜单只覆盖当前页面，不会并行启动第二套无线协议栈。

## 6. 编译

仓库唯一标准构建目录是项目根目录下的 `build-final`。在该工作树执行：

```bash
source scripts/idf_env.sh
idf.py -B build-final set-target esp32s3
idf.py -B build-final build
```

`set-target` 只在全新工作树或首次创建 `build-final` 时执行；后续增量构建直接运行
`idf.py -B build-final build`。不要在缺少 `sdkconfig` 时直接构建，否则 ESP-IDF
默认目标可能落到 ESP32。

`main/CMakeLists.txt` 只注册 `developer_demo.c`、公共事件循环、菜单和
`main/devdemo/apps/` 页面。仓库中没有正式入口 `main/app_main.c`、阅读器、
OTA、云服务、传书服务或 USB MSC。版本号由顶层 `CMakeLists.txt` 固定为
`V1.1.0-DEMO`。

构建完成后至少检查：

```bash
python "$IDF_PATH/components/esptool_py/esptool/esptool.py" \
  image-info build-final/eegoread_firmware.bin
shasum -a 256 build-final/eegoread_firmware.bin
```

## 7. 烧录与设备验证

仓库标准 app-only 烧录脚本为：

```bash
scripts/flash_app_verified.sh COM7
```

macOS 下把 `COM7` 替换为实际串口，例如 `/dev/cu.usbmodem...`。脚本只写 app0
`0x10000`，使用 esptool stub 并检查启动日志。若设备要求整片烧录，应显式
确认分区和参数，不应把 app-only 命令改写成未经验证的整片命令。

设备验收建议：

1. 串口确认版本为 `V1.1.0-DEMO`，且登录出现 `developer_demo`；
2. 用底栏箭头或 POWER 打开菜单，确认 10 个页面分两页显示且可切换；
3. `Overview` 的 A4/A4 Lite 与实机前光硬件一致；
4. `Power / Battery` 的 CW2015/ADC 与已绑定电源板型一致；
5. `Touch` 页显示原始和校准坐标，`Hardware Keys` 记录三键次数；
6. `Display / Refresh` 的 GC、FAST、PATTERN 均可执行并显示耗时；
7. A4 上分别拖动冷/暖两个滑块，检查亮度变化和百分比；A4 Lite 应显示未安装；
8. 插拔 USB 后 `Power / Battery` 的充电状态更新；
9. 插入和拔出 microSD 后 remount 并检查根目录条目；
10. `RTC` 页时间应持续更新且没有写 RTC 操作；
11. Wi-Fi 扫描后显示 AP 数量，离开页面后射频关闭；
12. BLE 扫描后显示设备数量，结束后 host/controller 释放。

## 8. 证据分层

以下结论必须分开记录：

- **主机验证**：静态策略测试、Markdown 检查和 ESP-IDF 编译通过；
- **镜像验证**：`image-info` 中的版本、大小和校验和；
- **启动验证**：串口日志确认 demo 入口及外设初始化；
- **功能验证**：触摸、按键、前光、电池、RTC、SD 的实际操作结果；
- **光学验证**：GC/快刷次数与实际屏幕变化、残影和耗时；
- **无线验证**：扫描结果与射频关闭日志，不等于连接、配对或传输成功。

构建成功不能证明设备外设正常；串口初始化成功也不能证明屏幕内容或前光肉眼可见。

## 9. 明确限制

- 不发布 OTA，不生成正式发布资产；
- 不连接 AP，不保存 Wi-Fi 凭据，不启动 AP 模式；
- 不连接、广播、配对或绑定 BLE 设备，不创建 GATT 服务；
- 不读写书籍、传输文件或修改 SD 内容；
- 不修改 RTC；
- 不初始化正式阅读器、云端账户、USB MSC 或自动休眠状态机。
