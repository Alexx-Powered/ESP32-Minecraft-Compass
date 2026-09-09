# WGS84 GeoJSON 配置接口

本说明对应当前固件接口与手机配置页，取代旧方案报告中的自定义 JSON 合同；旧报告及 DOCX 尚未重写。设备内置页面已适配 WGS84 GeoJSON；本次页面调整不改变接收端、NVS 格式、热点安全策略、指南针和 LED 模式。

## 1. 接口不变，配置内容改为 GeoJSON

| 槽位 | GET / PUT URL | 几何类型 |
|---|---|---|
| Target | `/api/v1/target` | `Feature` + `Point` |
| 围栏 1 | `/api/v1/fence/1` | `Feature` + `Polygon` |
| 围栏 2 | `/api/v1/fence/2` | `Feature` + `Polygon` |

请求支持 `Content-Type: application/geo+json` 或 `application/json`；成功响应为 `application/geo+json`。状态接口 `/api/v1/status` 与错误响应仍是普通 JSON。每次 PUT 独立保存一个槽位，提交 NVS 成功后才替换运行时数据。失败保持原有配置，通常返回 `422`（校验）或 `503`（NVS 保存）。

遵循 [RFC 7946 GeoJSON](https://www.rfc-editor.org/rfc/rfc7946)：WGS84 十进制度，位置顺序为 **`[longitude, latitude]`，即 `[经度, 纬度]`**。`properties.coordinateSystem` 可省略，若提供只能是 `"WGS84"`；禁止旧的顶层 `crs`。固件信任声明，但无法仅凭数值检测坐标是否真的经过正确转换，不能只改标签。

## 2. 支持的内容和边界

- 每次一个 `Feature`；必须含 `properties`（对象或 `null`）和 `geometry`。不接受裸 Geometry、FeatureCollection、MultiPolygon、GeometryCollection。
- Target 的 `geometry.type` 必须为 `Point`，例如 [target.geojson](examples/target.geojson)。
- 围栏的 `geometry.type` 必须为 `Polygon`，`coordinates` 必须只有一个闭合外环。至少 3、最多 16 个有效顶点，加上重复首点后为 4–17 个位置。首尾全部坐标值必须一致；顺/逆时针输入都可接受，导出统一逆时针。
- 不支持内洞、多外环、自交、重复有效顶点和零面积；经纬度包围框各不超过 2°，不支持跨日期变更线的大区域。此限制是嵌入式实现约束，不是 GeoJSON 标准限制。
- 经纬度必须为有限数字，范围分别是 `[-180,180]` 和 `[-90,90]`。可附加一个有限数字海拔作为第三项，仅校验后忽略，围栏判断仍是二维。
- 内部继续用整数 E6（保留小数点后 6 位并四舍五入）；更高精度输入可读取，读回是 E6 精度，最大量化误差每轴 0.0000005°。量化后重合的顶点也会拒绝。边界按量化后的几何计算且视为围栏内。
- 请求最多 3072 UTF-8 字节，包含缩进与元数据；JSON 嵌套最多 16 层；拒绝重复键、非有限数值、非法 UTF-8 和字符串中的 U+0000。

`properties` 中处理以下可选字段：

| 字段 | 缺省行为 / 要求 |
|---|---|
| `name` | 缺省或 null 使用 `Target` / `Fence 1` / `Fence 2`；启用时不可为空，最多 128 UTF-8 字节 |
| `enabled` | 缺省 `true`，提供时必须为布尔值 |
| `coordinateSystem` | 缺省 WGS84，若提供必须为字符串 `WGS84` |

`source`、`mapProvider`、`sourceCoordinateSystem`、`coordinateConversion`、`areaSquareMeters` 等附加元数据允许存在，但不参与导航、不存入 NVS，也不会在 GET 时原样返回。`sourceCoordinateSystem: "GCJ-02"` 只是来源说明，不会触发再次转换。Feature 可选 `id` 为字符串或数字，但不决定保存槽位，仍以 URL 为准。

提供的 [北航围栏完整示例](examples/beihang-fence.geojson) 可以直接作为 PUT 请求体：8 个位置中最后一个是闭合点，实际保存 7 个有效顶点；中文名称保留。

## 3. 禁用与旧数据

禁用并清空某槽位几何，可向该槽位 PUT：

```json
{"type":"Feature","properties":{"enabled":false},"geometry":null}
```

也可保留合法 Point/Polygon 并设 `enabled:false`，下次启用时无需重绘。`geometry:null` 不允许启用，`Polygon.coordinates:[]` 不是本接口的清空形式。

NVS 命名空间与键保持 `mc_geo/target`、`mc_geo/fence1`、`mc_geo/fence2`。升级启动只读历史记录，不批量擦除、不主动迁移写入：

- 新 GeoJSON：直接恢复；成功保存后断电重启可用。
- 旧自定义 WGS84：仍可恢复，GET 导出新 GeoJSON，下次 PUT 才按新格式存储。
- 旧自定义 GCJ02：继续按旧方式用于导航；GET 返回 `409`、错误码 `LEGACY_CRS`，避免把 GCJ02 错当 WGS84 导出。请重新生成 WGS84 文件并 PUT 覆盖对应槽位。
- HTTP 不再接受旧 `schema/type=target|polygon/vertices` 格式。

## 4. 手机配置页与安全限制

连接热点后打开 `http://192.168.4.1`，使用内置 `config_page.html`：

- **Target**：选择“输入坐标”，填写经度、纬度后应用；或选择“粘贴 GeoJSON”，粘贴完整 Point Feature 后应用。手动输入自动生成 WGS84 Point GeoJSON，空值、非数字和越界坐标不能提交。
- **围栏 1 / 围栏 2**：仅粘贴完整 Polygon Feature 并应用，不再提供手动矩形或顶点坐标输入。两个槽位独立保存。
- 页面单独显示设备当前配置；初次加载时粘贴框留空，方便直接长按粘贴。导入时显示名称、顶点数及北朝上的轮廓预览（不是地图）。完整几何合法性仍由固件判定。
- 点击应用后显示保存进度、成功或具体错误；NVS 保存成功才提示断电重启有效。失败保留输入，后台状态刷新不覆盖正在编辑的内容。
- 页面无外部字体、脚本或地图依赖；无需互联网。用户自行开发的 GeoJSON 生成网站未修改，生成后复制到设备页面即可。

手机页面按 UTF-8 统计 3072 字节上限，并原样提交粘贴文本，让固件仍能检查重复 JSON 键。小屏输入框使用 16px 字号，主要操作按钮高度至少 48px，三个配置页签可独立切换。

热点和原来的同源写入限制保持不变：浏览器 Origin 只允许 `http://192.168.4.1`，无 Origin 的本地命令行请求仍可使用；未开放 CORS，不接受来自外部网站或 `file://` 页面的跨域写入。如果自有网站只生成文件，不受影响；生成文件与直接向设备上传是两个环节。

连接设备热点后，在 PowerShell 项目根目录执行（需要新固件已烧录）：

```powershell
curl.exe --request PUT --header "Content-Type: application/geo+json" --data-binary "@docs/examples/beihang-fence.geojson" "http://192.168.4.1/api/v1/fence/1"
curl.exe "http://192.168.4.1/api/v1/fence/1"
curl.exe "http://192.168.4.1/api/v1/status"
```

对围栏 2 使用同一个 GeoJSON，改 URL 为 `/api/v1/fence/2` 即可，不必修改 Feature id。

## 5. 实现位置与验证

- `main/geo_config.c`：`geo_config_parse_target_json()` / `geo_config_parse_fence_json()` 为公共 GeoJSON 校验入口；`parse_feature()` 检查公共字段，`parse_position()` 校验位置，`validate_fence_geometry()` 检查几何。旧解析器仅供 NVS 回读。
- `geo_config_target_to_json()` / `geo_config_fence_to_json()`：规范化导出；仅存导航必需字段，Polygon 补回闭合点。
- `main/config_portal.c`：HTTP 媒体类型、原有 GET/PUT 路由、旧 CRS 冲突响应；同源写入策略保持不变。
- `main/config_page.html`：手机页签、Target 手动输入、GeoJSON 粘贴校验与轮廓预览；串行发送请求，避免并发挤占设备的少量 HTTP 连接。
- `tools/test_config_page.mjs`：通过 Playwright 和模拟 HTTP 接口测试页面，不写入设备 NVS。执行 `node tools/test_config_page.mjs main/config_page.html <包含 playwright 的 node_modules 目录>`；需安装 Chrome。
- `tests/host/test_geojson.c`：直接编译生产 `geo_config.c` 与 ESP-IDF 的 cJSON，模拟 NVS 和 FreeRTOS，不用另一套几何算法替代被测代码。

Windows 本机测试：`./tests/host/run_tests.ps1`。默认使用本机 ESP-IDF 5.5.5 与 Visual Studio 2017 C 编译器，可通过 `-IdfPath` 和 `-VcVars` 改路径。Linux 可使用 `bash tests/host/run_tests.sh /path/to/esp-idf`，该脚本另外启用 AddressSanitizer/UBSan；Windows 脚本不含这些 sanitizer。

测试覆盖北航原始示例、经纬顺序与范围、16/17 点边界、闭环、重复点、自交、凹多边形内外及边界、禁用与空几何、元数据、旧格式拒绝、导出回读、分配失败清理、NVS 提交失败不更新运行时、重启恢复与历史格式兼容。主机模拟测试不能代替实际断电、GPS 和无线网络联调。

此前 GeoJSON 接收端验证：Windows/MSVC 本机测试通过；Linux sanitizer 脚本仅提供，未在本机执行成功。

本次手机页面验证：Chrome 模拟浏览器测试通过，覆盖 320–1440px 布局、手动/GeoJSON Target、两个围栏独立提交、用户原始 Target 与北航围栏示例、非法输入、重复键原样传输、保存失败、断连、旧 CRS 与延迟加载时的草稿保护。ESP-IDF 5.5.5 / ESP32-S3 下 `idf.py build` 通过，固件大小 985312 字节，应用分区剩余 63264 字节（约 6%）。COM3 烧录尝试因串口拒绝访问而失败；尚未执行新版页面的真机 HTTP、手机浏览器或断电恢复测试，浏览器测试使用模拟接口。
