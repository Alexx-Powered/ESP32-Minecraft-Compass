from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


INK = "#17231F"
MUTED = "#63716B"
PAPER = "#F4F7F4"
CARD = "#FFFFFF"
GREEN = "#123D32"
GREEN_2 = "#1D5748"
LIME = "#B9ED67"
BLUE = "#2E74B5"
BLUE_LIGHT = "#E8F1F8"
LINE = "#CDD8D1"
AMBER = "#B87916"
AMBER_LIGHT = "#FFF4D9"
RED = "#A7463D"
RED_LIGHT = "#FBECE9"


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = [
        Path(r"C:\Windows\Fonts\msyhbd.ttc" if bold else r"C:\Windows\Fonts\msyh.ttc"),
        Path(r"C:\Windows\Fonts\simhei.ttf"),
        Path(r"C:\Windows\Fonts\arialbd.ttf" if bold else r"C:\Windows\Fonts\arial.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size=size)
    return ImageFont.load_default()


def wrap_text(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont,
              max_width: int) -> list[str]:
    lines: list[str] = []
    for paragraph in text.split("\n"):
        if not paragraph:
            lines.append("")
            continue
        line = ""
        for char in paragraph:
            trial = line + char
            if line and draw.textbbox((0, 0), trial, font=font)[2] > max_width:
                lines.append(line)
                line = char
            else:
                line = trial
        if line:
            lines.append(line)
    return lines


def draw_text_block(draw: ImageDraw.ImageDraw, xy: tuple[int, int], text: str,
                    font: ImageFont.ImageFont, fill: str, max_width: int,
                    line_gap: int = 7, anchor: str | None = None) -> int:
    x, y = xy
    lines = wrap_text(draw, text, font, max_width)
    bbox = draw.textbbox((0, 0), "国Ag", font=font)
    line_height = bbox[3] - bbox[1]
    for line in lines:
        draw.text((x, y), line, font=font, fill=fill, anchor=anchor)
        y += line_height + line_gap
    return y


def rounded_box(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int],
                fill: str = CARD, outline: str = LINE, radius: int = 24,
                width: int = 2) -> None:
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline,
                           width=width)


def arrow(draw: ImageDraw.ImageDraw, start: tuple[int, int], end: tuple[int, int],
          fill: str = GREEN_2, width: int = 5) -> None:
    draw.line([start, end], fill=fill, width=width)
    angle = math.atan2(end[1] - start[1], end[0] - start[0])
    length = 18
    spread = math.radians(28)
    points = [
        end,
        (int(end[0] - length * math.cos(angle - spread)),
         int(end[1] - length * math.sin(angle - spread))),
        (int(end[0] - length * math.cos(angle + spread)),
         int(end[1] - length * math.sin(angle + spread))),
    ]
    draw.polygon(points, fill=fill)


def routed_arrow(draw: ImageDraw.ImageDraw, points: list[tuple[int, int]],
                 fill: str = GREEN_2, width: int = 5) -> None:
    if len(points) < 2:
        return
    if len(points) > 2:
        draw.line(points[:-1], fill=fill, width=width, joint="curve")
        draw.line([points[-2], points[-1]], fill=fill, width=width)
    arrow(draw, points[-2], points[-1], fill, width)


def title_band(draw: ImageDraw.ImageDraw, width: int, title: str,
               subtitle: str) -> None:
    draw.rectangle((0, 0, width, 142), fill=GREEN)
    draw.text((70, 35), title, font=load_font(40, True), fill="white")
    draw.text((72, 92), subtitle, font=load_font(22), fill="#C9DBD3")


def build_config_flow(path: Path) -> None:
    image = Image.new("RGB", (1800, 980), PAPER)
    draw = ImageDraw.Draw(image)
    title_band(draw, image.width, "配置数据流：联网生成与离线应用解耦",
               "高德只负责搜索与绘制；ESP32 只负责接收、校验、持久化和运行")

    columns = [(70, 205, 540, 765), (665, 205, 1135, 765),
               (1260, 205, 1730, 765)]
    fills = [BLUE_LIGHT, CARD, "#EAF4EE"]
    outlines = [BLUE, LINE, GREEN_2]
    headings = ["1  高德 JSON 生成器", "2  独立 JSON 合同", "3  ESP32 本地配置页"]
    badges = ["需要互联网", "可复制 / 可审阅", "仅本地 WPA2"]
    bodies = [
        "AutoComplete / PlaceSearch\n搜索并定位地点\n\n点击或拖动 Marker\n确定 Target\n\nPolygonEditor\n绘制 Fence 1 / Fence 2",
        "target.json\n\nfence1.json\n\nfence2.json\n\nschema = 1\ncrs = WGS84 / GCJ02\n3–16 个用户顶点",
        "192.168.4.1\n手动输入或粘贴 JSON\n\n严格字段与几何校验\n独立 NVS 槽位\n\n保存后原子替换运行快照",
    ]
    for box, fill, outline, heading, badge, body in zip(
            columns, fills, outlines, headings, badges, bodies):
        rounded_box(draw, box, fill=fill, outline=outline, radius=28, width=3)
        draw.text((box[0] + 34, box[1] + 32), heading,
                  font=load_font(28, True), fill=INK)
        badge_box = (box[0] + 34, box[1] + 86, box[0] + 235, box[1] + 126)
        draw.rounded_rectangle(badge_box, 18, fill=outline)
        draw.text(((badge_box[0] + badge_box[2]) // 2,
                   (badge_box[1] + badge_box[3]) // 2), badge,
                  font=load_font(17, True), fill="white", anchor="mm")
        draw_text_block(draw, (box[0] + 38, box[1] + 164), body,
                        load_font(23), INK, box[2] - box[0] - 76, 11)

    arrow(draw, (540, 485), (655, 485), BLUE, 6)
    arrow(draw, (1135, 485), (1250, 485), GREEN_2, 6)

    rounded_box(draw, (130, 825, 1670, 930), fill=GREEN, outline=GREEN,
                radius=24, width=1)
    draw.text((185, 852), "运行时：GPS WGS84 → 按对象 CRS 对齐 → Target 方位 / 两个围栏状态 → WS2812、OLED、日志与状态接口",
              font=load_font(25, True), fill="white")
    draw.text((185, 892), "公网或托管平台不可用时，已经保存的 NVS 配置和罗盘功能仍可独立运行。",
              font=load_font(20), fill="#C9DBD3")
    image.save(path, quality=95)


def build_crs_flow(path: Path) -> None:
    image = Image.new("RGB", (1800, 940), PAPER)
    draw = ImageDraw.Draw(image)
    title_band(draw, image.width, "坐标系处理：先对齐，再计算",
               "GPS 原始数据始终保留为 WGS84；配置对象声明自己使用的坐标系")

    rounded_box(draw, (80, 220, 470, 420), fill=BLUE_LIGHT, outline=BLUE, width=3)
    draw.text((275, 268), "GPS RMC", font=load_font(32, True), fill=BLUE, anchor="mm")
    draw.text((275, 330), "WGS84 原始 fix", font=load_font(27), fill=INK, anchor="mm")
    draw.text((275, 374), "不覆盖、不伪装转换", font=load_font(19), fill=MUTED, anchor="mm")

    rounded_box(draw, (680, 205, 1120, 435), fill=CARD, outline=GREEN_2, width=3)
    draw.text((900, 265), "读取对象 crs", font=load_font(31, True), fill=GREEN, anchor="mm")
    draw.text((900, 330), "WGS84 还是 GCJ02？", font=load_font(27), fill=INK, anchor="mm")
    draw.text((900, 382), "每个 Target / Fence 独立声明", font=load_font(19), fill=MUTED, anchor="mm")
    arrow(draw, (470, 320), (670, 320), GREEN_2, 6)

    rounded_box(draw, (1325, 175, 1715, 375), fill="#EAF4EE", outline=GREEN_2, width=3)
    draw.text((1520, 230), "对象 = WGS84", font=load_font(29, True), fill=GREEN, anchor="mm")
    draw.text((1520, 300), "GPS 原值直接比较", font=load_font(24), fill=INK, anchor="mm")
    arrow(draw, (1120, 285), (1315, 285), GREEN_2, 6)
    draw.text((1215, 242), "WGS84", font=load_font(18, True), fill=GREEN, anchor="mm")

    rounded_box(draw, (1325, 500, 1715, 720), fill=AMBER_LIGHT, outline=AMBER, width=3)
    draw.text((1520, 555), "对象 = GCJ02", font=load_font(29, True), fill=AMBER, anchor="mm")
    draw.text((1520, 620), "GPS WGS84 → GCJ02", font=load_font(24), fill=INK, anchor="mm")
    draw.text((1520, 670), "再与高德坐标比较", font=load_font(20), fill=MUTED, anchor="mm")
    arrow(draw, (1030, 435), (1320, 580), AMBER, 6)
    draw.text((1180, 485), "GCJ02", font=load_font(18, True), fill=AMBER, anchor="mm")

    rounded_box(draw, (250, 735, 1280, 875), fill=GREEN, outline=GREEN, width=1)
    draw.text((765, 780), "同一坐标系中的运算", font=load_font(29, True), fill="white", anchor="mm")
    draw.text((765, 830), "Target：大圆初始方位    ·    Fence：边界包含的点在多边形内判断",
              font=load_font(23), fill="#D4E4DD", anchor="mm")
    draw.text((765, 706), "两条分支完成坐标对齐后进入同一套导航逻辑",
              font=load_font(19, True), fill=GREEN, anchor="mm")
    image.save(path, quality=95)


def build_task_priority(path: Path) -> None:
    image = Image.new("RGB", (1800, 1120), PAPER)
    draw = ImageDraw.Draw(image)
    title_band(draw, image.width, "FreeRTOS 任务优先级与阻塞边界",
               "高优先级保证传感器与导航新鲜度；慢操作被隔离在 UI、HTTP 或校准任务")

    headers = ["优先级", "任务 / 服务", "主要等待", "不会阻塞的下游"]
    x = [75, 265, 635, 1135, 1725]
    draw.rounded_rectangle((75, 185, 1725, 250), 18, fill=GREEN)
    for index, header in enumerate(headers):
        draw.text(((x[index] + x[index + 1]) // 2, 218), header,
                  font=load_font(22, True), fill="white", anchor="mm")

    rows = [
        ("6", "mag_acquire", "20 ms 周期；I2C 最多 100/250 ms", "队列满时丢校准样本，导航仍继续"),
        ("5", "navigation", "磁样本队列最多 50 ms", "覆盖最新 UI；不等待 OLED/WS2812"),
        ("5", "gps", "UART 最多 20 ms", "覆盖最新 fix；无数据不忙等"),
        ("5", "HTTP Server", "请求体 + JSON + NVS commit", "只卡 HTTP 任务；导航继续旧快照"),
        ("4", "button", "10 ms 周期 / 40 ms 去抖", "不依赖 GPS、网页或显示"),
        ("3", "ui", "OLED I2C 最多 100 ms / 5 Hz", "不向 navigation 施加反压"),
        ("2", "calibration", "样本等待 + 椭圆拟合 + NVS", "最低应用优先级；传感器非阻塞发送"),
    ]
    row_h = 103
    y0 = 268
    fills = [BLUE_LIGHT, "#EAF4EE", "#EAF4EE", AMBER_LIGHT,
             CARD, CARD, RED_LIGHT]
    accents = [BLUE, GREEN_2, GREEN_2, AMBER, MUTED, MUTED, RED]
    for row_index, row in enumerate(rows):
        top = y0 + row_index * row_h
        bottom = top + 86
        draw.rounded_rectangle((75, top, 1725, bottom), 16,
                               fill=fills[row_index], outline=LINE, width=2)
        draw.rounded_rectangle((91, top + 13, 166, bottom - 13), 18,
                               fill=accents[row_index])
        draw.text((128, (top + bottom) // 2), row[0],
                  font=load_font(28, True), fill="white", anchor="mm")
        draw.text((300, (top + bottom) // 2), row[1],
                  font=load_font(24, True), fill=INK, anchor="lm")
        draw.text((665, (top + bottom) // 2), row[2],
                  font=load_font(20), fill=INK, anchor="lm")
        draw.text((1165, (top + bottom) // 2), row[3],
                  font=load_font(20), fill=INK, anchor="lm")

    note_y = 1000
    draw.rounded_rectangle((75, note_y, 1725, 1080), 18, fill=GREEN)
    draw.text((105, note_y + 24), "共享边界：HMC5883L 与 OLED 共用有超时的 I2C 互斥锁；地理配置只在完整快照替换时短暂加锁。",
              font=load_font(21, True), fill="white")
    draw.text((105, note_y + 55), "启动时另有一次 5 秒凭据展示；它发生在任务创建前，不是运行期阻塞。",
              font=load_font(19), fill="#C9DBD3")
    image.save(path, quality=95)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    build_config_flow(args.output_dir / "config_flow.png")
    build_crs_flow(args.output_dir / "crs_flow.png")
    build_task_priority(args.output_dir / "task_priority.png")


if __name__ == "__main__":
    main()
