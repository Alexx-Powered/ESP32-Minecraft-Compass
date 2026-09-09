# MinecraftCompass（ESP-IDF）

固件基于 ESP-IDF 5.5.x / FreeRTOS，实现 HMC5883L 指南针、GPS 目标坐标指向、最多两个电子围栏、WPA2 本地配置页、反向编号的 56 像素 WS2812 指南针、GPIO5 模式按键、SSD1306 OLED 状态，以及持续后台运行的二维椭圆校准。

## 构建

在已激活的 ESP-IDF 5.5.5 环境中进入项目目录，运行：

```sh
idf.py build
```

`sdkconfig.defaults` 保存当前 ESP32-S3 项目配置，`dependencies.lock` 锁定组件版本。首次构建会生成本机的 `sdkconfig` 并下载 `managed_components/`；这些文件和 `build/`、`build-*/` 测试输出不纳入版本控制。本地修正版 `components/hmc5883l` 必须保留。

## 接线

| 功能 | ESP32-S3 GPIO |
|---|---:|
| I2C SDA（HMC5883L + OLED） | 1 |
| I2C SCL（HMC5883L + OLED） | 2 |
| GPS RX（MCU TX） | 3 |
| GPS TX（MCU RX） | 4 |
| 模式按键（另一端接 GND） | 5 |
| WS2812 数据 | 6 |

OLED 默认按 128×64、SSD1306、地址 `0x3C` 初始化。GPS 使用 UART2、9600 8N1，并校验 `$GPRMC` / `$GNRMC` checksum。

## 运行方式

- 上电先校验并应用 NVS 命名空间 `mc_compass` 中的 `cal2d` 校准块；没有有效记录时使用单位矩阵，不阻塞进入主界面。
- GPIO5 稳定按下 40 ms 后在 `NORTH` 与 `TARGET` 两种模式间切换。
- `NORTH` 模式显示相对磁北方向；`TARGET` 模式根据 GPS 当前坐标与 NVS 目标计算大圆初始方位。无有效目标、新鲜定位或磁力计时，逻辑状态为 `status:0`，WS2812 播放连续平滑乱转动画。
- OLED 常驻显示 `MODE`、逻辑 `STATUS`、航向/GPS 状态。已有校准时底行显示 `calibrated(Xs)`，其中 X 是这组数据本次上电后被应用的时长。
- `navigation` 任务持续判断两个围栏。边界视为围栏内；状态只在变化时写入串口日志，并通过配置页状态接口显示，不擅自改变 LED 指南针行为。

## Wi-Fi 配置接口（WGS84 GeoJSON）

设备每次启动一个本地 WPA2 SoftAP，SSID 形如 `MCCompass-A1B2`。SSID 前缀和密码在 `main/app_config.h` 的 `APP_CONFIG_AP_SSID_PREFIX` / `APP_CONFIG_AP_PASSWORD` 修改，当前密码为 `compass123`；启动时会用代码中的密码替换旧 NVS 密码，不清除其他设置。OLED 会显示 SSID、密码及 `192.168.4.1` 约 5 秒，串口日志也会输出凭据。连接热点后，在手机浏览器打开 `http://192.168.4.1` 即可配置。

- Target、Fence 1、Fence 2 是三个独立槽位，保存一个不会覆盖另外两个；
- Target 使用 GeoJSON `Feature + Point`，Fence 使用 `Feature + Polygon`，坐标顺序固定为 `[经度, 纬度]`，实际坐标必须已经是 WGS84；
- 围栏支持 3–16 个有效顶点，外加一个重复首点形成闭合环；支持凹/凸简单多边形，不支持内洞或 MultiPolygon；
- ESP32 端检查结构、坐标范围、UTF-8 名称、闭合、重复点、零面积、自交和最大 2° 包围框；
- 每次成功保存先提交到 NVS，再原子替换运行时快照，重启后自动恢复。

Target GeoJSON 示例：

```json
{"type":"Feature","properties":{"name":"示例目标","coordinateSystem":"WGS84"},"geometry":{"type":"Point","coordinates":[116.34,39.98]}}
```

Fence GeoJSON 示例：

```json
{"type":"Feature","properties":{"name":"示例区域","coordinateSystem":"WGS84"},"geometry":{"type":"Polygon","coordinates":[[[116.33,39.97],[116.34,39.97],[116.34,39.98],[116.33,39.98],[116.33,39.97]]]}}
```

分别向 `/api/v1/target`、`/api/v1/fence/1`、`/api/v1/fence/2` 发送 PUT；槽位由 URL 决定，不需要自定义 `schema/id/vertices`。省略 `enabled` 默认启用；禁用并清空几何时发送 `{"type":"Feature","properties":{"enabled":false},"geometry":null}`。支持 `application/geo+json` 和 `application/json` 请求。

内置 `main/config_page.html` 已适配手机与 WGS84 GeoJSON，分为 Target、围栏 1、围栏 2 三个页签：Target 可手动输入经纬度并应用，或粘贴 Point GeoJSON 并应用；两个围栏仅保留粘贴 Polygon GeoJSON 并应用，已移除手动矩形/坐标输入。粘贴后可预览名称、坐标或围栏轮廓，并显示格式错误；以设备校验及 NVS 保存成功为准。首次打开时粘贴框留空，当前配置单独展示，避免手机上反复全选旧 JSON。用户自行开发的 GeoJSON 生成网站未修改。原有同源限制保持不变，不提供跨域写入许可。详细约定、curl 测试方法及限制见 [GeoJSON 接口说明](docs/GeoJSON接口说明.md)；完整实测样例见 [北航围栏](docs/examples/beihang-fence.geojson)。

旧自定义格式仅在读取历史 NVS 时兼容，不再接受新上传。旧 WGS84 数据读取时导出为 GeoJSON；旧 GCJ02 数据继续用于导航，但配置 GET 返回 `409 LEGACY_CRS`，需要用户重新导入真实 WGS84 坐标。不会把坐标仅改标签冒充转换。本项目仍假设 GPS 接收机输出 WGS84，需与接收机配置核对。

配置分别保存在 NVS 命名空间 `mc_geo` 的 `target`、`fence1`、`fence2`，热点密码保存在 `mc_net/ap_psk`；校准仍保存在 `mc_compass/cal2d`。初始化失败时程序不会通过擦除整个 NVS 来“恢复”，以免丢失校准和用户配置。

## 后台二维校准的可信门槛

低优先级校准任务只接受有限、未饱和且有实际位移的磁场样本。候选拟合同时检查：

- 至少 240 个有效变化样本，采集跨度至少 12 秒；窗口最长 120 秒；
- 24 个方向箱至少覆盖 20 个，连续空缺不超过 4 个；
- 初次椭圆圆化后的 MAD 异常点剔除，内点率至少 80%；
- X/Y 跨度、椭圆长短轴比、圆化后的半径变异系数；
- 综合置信度至少 0.82，并连续 3 次相邻拟合参数一致。

通过后先生成带版本、长度、序号与校验和的记录，再写入 NVS 并立即应用。两次写入至少间隔 5 分钟，避免用户持续转动时磨损 Flash。

## 首次修改的配置

所有板级与算法参数集中在 `main/app_config.h`：

- 文件最前面的 `APP_SENSOR_ANGLE_OFFSET_DEG`：传感器安装方向修正，正值让最终航向顺时针增加，负值逆时针，默认 `0.0°`；
- `APP_TARGET_LATITUDE_DEG` / `APP_TARGET_LONGITUDE_DEG`：仅在 NVS 中尚无 Target 时使用的首次启动回退值；
- `APP_MAGNETIC_DECLINATION_DEG`：当地磁偏角，东偏为正。当前默认 `0.0°`；目标方位与真北精度需要按部署地设置，后续可由 WMM/Wi-Fi 配置替换；
- `APP_WS2812_BRIGHTNESS_PERCENT`：全局亮度，当前 30%。

LED 帧直接采用 `WS2812 Test` 当前的 32×56 反向物理顺序数据：新编号 `new[i] = old[55-i]`，GPIO6 上使用 RMT、GRB 线序。

角度状态不是均匀 11.25° 切分。`main/compass_angle.h` 保存了 27 组实测中心角和相邻中心的 Voronoi 边界；`compass_status_with_hysteresis()` 在真实边界外再增加迟滞，从而选择对应的 WS2812 status。

HMC5883L 驱动固定为 `components/hmc5883l` 中的本地修正版。该版本修复了上游 1.0.7 将单字节寄存器读入未初始化枚举而造成的越界访问，以及配置寄存器位掩码更新错误；不要再同时添加 `esp-idf-lib/hmc5883l` 依赖。
