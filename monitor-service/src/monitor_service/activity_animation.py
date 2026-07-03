import hashlib
import math
from io import BytesIO
from typing import Callable

from PIL import Image, ImageDraw

from monitor_service.types import ActivityAnimationConfig

# Binary header of the package the ESP32 downloads and stores (little-endian format).
ANIM_PACK_MAGIC = 0xA1
ANIM_PACK_VERSION = 1
ANIM_PACK_HEADER_BYTES = 10

# Content revision of the generators: included in the ETag to invalidate the
# ESP cache when the drawing of a style changes without changing its parameters. Bump
# this number every time the way an existing style is rendered is modified.
ANIM_CONTENT_REVISION = 2

# Scale of the preview GIF on the web.
PREVIEW_SCALE = 8


def _new_frame(width: int, height: int) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("1", (width, height), 0)
    return (image, ImageDraw.Draw(image))


def _dot_radius(height: int) -> float:
    # Dot radius bounded to the available height so it neither overflows nor disappears.
    return max(0.5, min(1.5, (height - 1) / 2.0))


def _spinner_frames(width: int, height: int) -> list[Image.Image]:
    # A dot orbiting around the center: 8 evenly spaced positions.
    frames: list[Image.Image] = []
    center_x = width / 2.0 - 0.5
    center_y = height / 2.0 - 0.5
    dot_radius = _dot_radius(height)
    orbit_radius = max(0.0, min(width, height) / 2.0 - dot_radius)
    for step in range(8):
        image, draw = _new_frame(width, height)
        angle = (math.pi * 2.0 * step) / 8.0
        dot_x = center_x + orbit_radius * math.cos(angle)
        dot_y = center_y + orbit_radius * math.sin(angle)
        draw.ellipse((dot_x - dot_radius, dot_y - dot_radius, dot_x + dot_radius, dot_y + dot_radius), fill=1)
        frames.append(image)
    return frames


def _dots_frames(width: int, height: int) -> list[Image.Image]:
    # Three dots that light up progressively (1, 2, 3).
    frames: list[Image.Image] = []
    positions = (
        max(1, width // 6),
        width // 2,
        min(width - 2, (width * 5) // 6),
    )
    center_y = (height - 1) / 2.0
    dot_radius = _dot_radius(height)
    for visible in (1, 2, 3):
        image, draw = _new_frame(width, height)
        for index in range(visible):
            dot_x = positions[index]
            draw.ellipse((dot_x - dot_radius, center_y - dot_radius, dot_x + dot_radius, center_y + dot_radius), fill=1)
        frames.append(image)
    return frames


def _march_right_frames(
    width: int,
    height: int,
    shape_w: int,
    shape_h: int,
    draw_shape: Callable[[ImageDraw.ImageDraw, int, int, int, int], None],
) -> list[Image.Image]:
    # Evenly spaced elements that move to the right and reappear at the start.
    # draw_shape paints a given element (left, top, width, height); PIL clips whatever
    # overflows the canvas, so the march is robust to any size.
    frames: list[Image.Image] = []
    top_y = max(0, (height - shape_h) // 2)
    spacing = max(shape_w + 1, width // 4)
    shape_count = max(1, width // spacing + 1)
    offsets = list(range(0, spacing, max(1, shape_w))) or [0]
    for offset in offsets:
        image, draw = _new_frame(width, height)
        for index in range(shape_count):
            left = (offset + index * spacing) % width
            draw_shape(draw, left, top_y, shape_w, shape_h)
        frames.append(image)
    return frames


def _draw_square(draw: ImageDraw.ImageDraw, left: int, top: int, shape_w: int, shape_h: int) -> None:
    draw.rectangle((left, top, left + shape_w - 1, top + shape_h - 1), fill=1)


def _draw_plus(draw: ImageDraw.ImageDraw, left: int, top: int, shape_w: int, shape_h: int) -> None:
    # Star shaped like "+": horizontal and vertical arms crossed at the center.
    center_x = left + (shape_w - 1) // 2
    center_y = top + (shape_h - 1) // 2
    draw.rectangle((left, center_y, left + shape_w - 1, center_y), fill=1)
    draw.rectangle((center_x, top, center_x, top + shape_h - 1), fill=1)


def _dots_right_frames(width: int, height: int) -> list[Image.Image]:
    # Square dots of 3x3 px (bounded to the canvas) that move to the right.
    shape_w = max(1, min(3, width))
    shape_h = max(1, min(3, height))
    return _march_right_frames(width, height, shape_w, shape_h, _draw_square)


def _stars_right_frames(width: int, height: int) -> list[Image.Image]:
    # Stars shaped like "+" that move to the right. Odd sides so that
    # the crossing stays centered; 3x3 by default and reduced if the canvas is smaller.
    shape_w = max(1, min(3, width))
    shape_h = max(1, min(3, height))
    return _march_right_frames(width, height, shape_w, shape_h, _draw_plus)


def _pulse_frames(width: int, height: int) -> list[Image.Image]:
    # Filled circle that beats: grows and shrinks.
    frames: list[Image.Image] = []
    center_x = width / 2.0 - 0.5
    center_y = height / 2.0 - 0.5
    max_radius = max(1, min(width, height) // 2)
    growing = list(range(1, max_radius + 1))
    shrinking = list(range(max_radius - 1, 0, -1))
    for radius in growing + shrinking:
        image, draw = _new_frame(width, height)
        draw.ellipse((center_x - radius, center_y - radius, center_x + radius, center_y + radius), fill=1)
        frames.append(image)
    return frames


def _bars_frames(width: int, height: int) -> list[Image.Image]:
    # Three-bar equalizer with heights that change per frame.
    frames: list[Image.Image] = []
    bar_width = max(1, width // 16)
    gap = max(1, (width - bar_width * 3) // 4)
    bar_x_positions = (
        gap,
        gap * 2 + bar_width,
        gap * 3 + bar_width * 2,
    )
    tall = height
    mid = max(1, (height * 2) // 3)
    low = max(1, height // 3)
    height_patterns = ((mid, tall, low), (tall, low, mid), (low, mid, tall), (mid, mid, mid))
    for pattern in height_patterns:
        image, draw = _new_frame(width, height)
        for bar_x, bar_height in zip(bar_x_positions, pattern):
            top_y = height - bar_height
            draw.rectangle((bar_x, top_y, min(bar_x + bar_width - 1, width - 1), height - 1), fill=1)
        frames.append(image)
    return frames


def _wave_frames(width: int, height: int) -> list[Image.Image]:
    # Traveling sine of 2 px per column so it reads as a continuous wave.
    frames: list[Image.Image] = []
    center_y = (height - 1) / 2.0
    amplitude = (height - 1) / 2.0
    wavelength = max(6.0, width / 2.0)
    frame_count = 12
    for index in range(frame_count):
        phase = index * wavelength / frame_count
        image, draw = _new_frame(width, height)
        for x in range(0, width, 2):
            angle = ((x + phase) / wavelength) * math.pi * 2.0
            y = int(round(center_y + amplitude * math.sin(angle)))
            draw.rectangle((x, y, min(x + 1, width - 1), y), fill=1)
        frames.append(image)
    return frames


def _worm_frames(width: int, height: int) -> list[Image.Image]:
    # Port of the Gusano_SSD1306.ino sketch: horizontal bounce with triangular wave,
    # sinusoidal vertical wiggle and body as the head's history.
    frames: list[Image.Image] = []
    worm_length = max(3, min(9, width // 5))
    center_y = (height - 1) / 2.0
    amplitude = (height - 1) / 2.0
    phase = 0.0
    worm_time = 0.0
    phase_step = 2.0 / 32.0
    wiggle_step = math.tau / 32.0
    history: list[tuple[int, int]] = []

    for _ in range(32):
        phase += phase_step
        if phase > 2.0:
            phase -= 2.0
        horizontal_ratio = phase if phase < 1.0 else 2.0 - phase
        head_x = round(horizontal_ratio * (width - 1))

        worm_time += wiggle_step
        if worm_time > math.tau:
            worm_time -= math.tau
        head_y = max(0, min(height - 1, round(center_y + amplitude * math.sin(worm_time))))

        history.insert(0, (head_x, head_y))
        history = history[:worm_length]

        image, draw = _new_frame(width, height)
        for index, point in enumerate(history):
            if index == 0:
                draw.point(point, fill=1)
                continue
            # Join the history points so the worm does not look "broken" when
            # the head changes row between frames.
            previous = history[index - 1]
            draw.line((previous[0], previous[1], point[0], point[1]), fill=1)
        frames.append(image)

    return frames


def _ball_frames(width: int, height: int) -> list[Image.Image]:
    # Ball that bounces from one end to the other horizontally (back and forth).
    frames: list[Image.Image] = []
    radius = _dot_radius(height)
    center_y = height / 2.0 - 0.5
    left = radius
    right = max(left, width - 1 - radius)
    steps = 7
    forward = [left + (right - left) * step / steps for step in range(steps + 1)]
    # Bounce: returns along the reverse path without repeating the ends.
    sweep = forward + forward[-2:0:-1]
    for center_x in sweep:
        image, draw = _new_frame(width, height)
        draw.ellipse((center_x - radius, center_y - radius, center_x + radius, center_y + radius), fill=1)
        frames.append(image)
    return frames


_STYLE_BUILDERS = {
    "spinner": _spinner_frames,
    "dots": _dots_frames,
    "dots-right": _dots_right_frames,
    "stars-right": _stars_right_frames,
    "pulse": _pulse_frames,
    "bars": _bars_frames,
    "ball": _ball_frames,
    "wave": _wave_frames,
    "worm": _worm_frames,
}


def render_activity_frames(style: str, width: int, height: int) -> list[Image.Image]:
    builder = _STYLE_BUILDERS.get(style, _spinner_frames)
    return builder(width, height)


def _flags_byte(config: ActivityAnimationConfig) -> int:
    return 0x01 if config["invert_on_waiting"] else 0x00


def pack_activity_animation(config: ActivityAnimationConfig) -> bytes:
    # Packs header + frames (Adafruit drawBitmap format per frame) for the ESP32.
    width = config["frame_width"]
    height = config["frame_height"]
    frames = render_activity_frames(config["style"], width, height)
    interval_ms = min(65535, max(20, config["interval_ms"]))
    invert_blink_ms = min(65535, max(100, config["invert_blink_ms"]))

    header = bytes(
        [
            ANIM_PACK_MAGIC,
            ANIM_PACK_VERSION,
            width,
            height,
            len(frames),
            _flags_byte(config),
            interval_ms & 0xFF,
            (interval_ms >> 8) & 0xFF,
            invert_blink_ms & 0xFF,
            (invert_blink_ms >> 8) & 0xFF,
        ]
    )

    body = b"".join(frame.convert("1").tobytes() for frame in frames)
    return header + body


def activity_animation_etag(config: ActivityAnimationConfig) -> str:
    # Content-stable ETag: changes only if what the ESP must re-download changes.
    fingerprint = "|".join(
        [
            str(ANIM_PACK_VERSION),
            str(ANIM_CONTENT_REVISION),
            config["style"],
            str(config["interval_ms"]),
            str(int(config["invert_on_waiting"])),
            str(config["invert_blink_ms"]),
            f"{config['frame_width']}x{config['frame_height']}",
        ]
    )
    return '"' + hashlib.sha1(fingerprint.encode("utf-8")).hexdigest() + '"'


def render_activity_preview_gif(config: ActivityAnimationConfig) -> bytes:
    # Animated (enlarged) GIF to preview the animation on the web.
    width = config["frame_width"]
    height = config["frame_height"]
    frames = render_activity_frames(config["style"], width, height)
    scaled_frames = [
        frame.convert("L").resize((width * PREVIEW_SCALE, height * PREVIEW_SCALE), Image.NEAREST)
        for frame in frames
    ]

    buffer = BytesIO()
    scaled_frames[0].save(
        buffer,
        format="GIF",
        save_all=True,
        append_images=scaled_frames[1:],
        duration=max(20, config["interval_ms"]),
        loop=0,
    )
    return buffer.getvalue()
