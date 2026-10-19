# MinecraftCompass（ESP-IDF）

把 Minecraft 风格的指南针带到现实中。项目基于 **ESP32-S3、ESP-IDF 5.5.5 和 FreeRTOS**，结合磁力计、GPS 与像素灯光，实现指北、目标方向指示和电子围栏功能，方便大家复刻与扩展。

## 主要功能

- **双模式指南针**：支持磁北指向和 GPS 目标方向指示，通过按键切换 `NORTH` / `TARGET` 模式。
- **Minecraft 风格灯效**：使用 56 颗 WS2812 像素灯显示方向；目标模式下定位、目标或传感器数据无效时，播放旋转动画。
- **手机配置页面**：连接设备 Wi-Fi 热点即可配置。目标位置支持手动输入经纬度或导入 GeoJSON，围栏支持导入 GeoJSON。
- **双电子围栏**：可独立设置最多两个多边形围栏，判断设备位于围栏内或围栏外，并在配置页面查看状态。
- **后台自动校准**：持续采集磁力计数据，在满足质量要求后更新校准结果；校准和目标、围栏配置均支持断电保存。
- **OLED 状态显示**：显示当前模式、航向、GPS 和校准状态，便于查看设备运行情况。

## 开发进度

**OneNET 云端 OTA 升级正在建设中，当前版本暂未提供此功能。**

## 硬件与接线

主要硬件：ESP32-S3、HMC5883L 磁力计、UART GPS 模块、56 颗 WS2812、128×64 SSD1306 OLED 和一个模式按键。

| 功能 | ESP32-S3 GPIO |
|---|---:|
| I2C SDA（HMC5883L + OLED） | 1 |
| I2C SCL（HMC5883L + OLED） | 2 |
| GPS RX（MCU TX） | 3 |
| GPS TX（MCU RX） | 4 |
| 模式按键（另一端接 GND） | 5 |
| WS2812 数据 | 6 |

OLED 默认 I2C 地址为 `0x3C`，GPS 默认串口参数为 `9600 8N1`。接线、传感器安装角度、磁偏角、灯光亮度和默认目标可在 [main/app_config.h](main/app_config.h) 中调整。

## 构建与使用

在已激活的 ESP-IDF 5.5.5 环境中进入项目目录，运行：

```sh
idf.py build
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口，例如 Windows 下的 `COM3`。`sdkconfig.defaults` 保存 ESP32-S3 默认配置，`dependencies.lock` 锁定组件版本；首次构建会自动获取托管依赖。请保留 `components/hmc5883l` 中的本地修正版驱动。

上电后：

1. 用手机连接 `MCCompass-XXXX` 热点，默认密码为 `compass123`。启动时 OLED 会短暂显示连接信息。
2. 在浏览器打开 [设备配置页](http://192.168.4.1)。
3. 设置目标位置，按需导入围栏 GeoJSON；坐标使用 **WGS84**，顺序为 **[经度, 纬度]**。
4. 按下 GPIO5 模式按键，切换指北与目标指向模式。保存成功的配置会在重启后自动恢复。

## 更多资料

- [GeoJSON 接口说明](docs/GeoJSON接口说明.md)
- [目标坐标示例](docs/examples/target.geojson)
- [北航校园围栏示例](docs/examples/beihang-fence.geojson)
- [Target 与电子围栏配置方案](docs/Target与电子围栏配置方案.md)
