import math
from datetime import datetime, timedelta
from typing import Callable

from PIL import Image, ImageDraw

from monitor_service.types import ToolSnapshot, UsageWindowSnapshot

# Dimensiones del OLED SSD1306 del ESP32.
WIDTH = 128
HEIGHT = 64

# Pantalla de arranque/espera (debe coincidir con la del firmware en main.cpp):
# mascot a la izquierda dentro de un marco de pixeles blancos. Se reutiliza para el
# frame de "esperando cache" del servicio para que se vea igual que al arrancar.
BOOT_MASCOT_CX = 22
BOOT_MASCOT_CY = 24
BOOT_DIVIDER_X = 43
BOOT_LOG_X = 47

# Recuadro reservado en la esquina superior derecha: el firmware del ESP32 dibuja ahí
# la animación de actividad y aquí solo se evita poner contenido. El tamaño llega desde
# la config (activity_animation.frame_width/height) vía set_activity_box, para que el
# borde derecho del texto de cabecera se ajuste a un recuadro de tamaño dinámico.
# Por defecto banda baja y ancha (48x5): cabe sobre la divisoria del tema.
ACTIVITY_BOX_W = 48
ACTIVITY_BOX_H = 5
ACTIVITY_BOX_X = WIDTH - ACTIVITY_BOX_W
ACTIVITY_BOX_Y = 0
# Borde derecho disponible para el texto de cabecera que comparte la franja del recuadro.
CONTENT_RIGHT_X = ACTIVITY_BOX_X - 2


def set_activity_box(width: int, height: int) -> None:
    # Ajusta el recuadro reservado al tamaño elegido en la config (anclado al borde
    # derecho). Sigue el mismo patrón de estado de módulo que set_device_ip.
    global ACTIVITY_BOX_W, ACTIVITY_BOX_H, ACTIVITY_BOX_X, CONTENT_RIGHT_X
    ACTIVITY_BOX_W = max(1, min(WIDTH, width))
    ACTIVITY_BOX_H = max(1, min(HEIGHT, height))
    ACTIVITY_BOX_X = WIDTH - ACTIVITY_BOX_W
    CONTENT_RIGHT_X = ACTIVITY_BOX_X - 2


DEFAULT_THEME = "delta"

# IP del ESP32 (la fija main.py al recibir los frames).
_device_ip = ""


def set_device_ip(ip: str) -> None:
    global _device_ip
    _device_ip = ip

# Catálogo de temas para la página de configuración (id + etiqueta visible).
THEME_DEFINITIONS = [
    {"id": "delta", "label": "Delta"},
]


# ---------------------------------------------------------------------------
# Fuente de píxeles 3x5 hecha a mano (números, letras y símbolos) usada por el
# render estilo terminal.
# ---------------------------------------------------------------------------

GLYPHS_3X5: dict[str, tuple[str, str, str, str, str]] = {
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "010", "010", "010"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "A": ("010", "101", "111", "101", "101"),
    "B": ("110", "101", "110", "101", "110"),
    "C": ("011", "100", "100", "100", "011"),
    "D": ("110", "101", "101", "101", "110"),
    "E": ("111", "100", "110", "100", "111"),
    "F": ("111", "100", "110", "100", "100"),
    "G": ("011", "100", "101", "101", "011"),
    "H": ("101", "101", "111", "101", "101"),
    "I": ("111", "010", "010", "010", "111"),
    "J": ("001", "001", "001", "101", "010"),
    "K": ("101", "110", "100", "110", "101"),
    "L": ("100", "100", "100", "100", "111"),
    "M": ("101", "111", "111", "101", "101"),
    "N": ("101", "111", "111", "111", "101"),
    "O": ("111", "101", "101", "101", "111"),
    "P": ("111", "101", "111", "100", "100"),
    "Q": ("111", "101", "101", "111", "001"),
    "R": ("111", "101", "111", "110", "101"),
    "S": ("011", "100", "010", "001", "110"),
    "T": ("111", "010", "010", "010", "010"),
    "U": ("101", "101", "101", "101", "111"),
    "V": ("101", "101", "101", "101", "010"),
    "W": ("101", "101", "111", "111", "101"),
    "X": ("101", "101", "010", "101", "101"),
    "Y": ("101", "101", "010", "010", "010"),
    "Z": ("111", "001", "010", "100", "111"),
    "%": ("101", "001", "010", "100", "101"),
    ":": ("000", "010", "000", "010", "000"),
    "-": ("000", "000", "111", "000", "000"),
    "+": ("000", "010", "111", "010", "000"),
    ">": ("100", "010", "001", "010", "100"),
    "@": ("111", "101", "111", "100", "011"),
    "_": ("000", "000", "000", "000", "111"),
    "o": ("000", "000", "111", "101", "111"),
    "?": ("111", "001", "011", "000", "010"),
    "!": ("010", "010", "010", "000", "010"),
    " ": ("000", "000", "000", "000", "000"),
}


def _glyph_image(text: str, scale: int, gap: int) -> Image.Image:
    glyphs = [GLYPHS_3X5.get(char, GLYPHS_3X5[" "]) for char in text]
    base_width = max(1, 3 * len(glyphs) + gap * (len(glyphs) - 1))
    base = Image.new("1", (base_width, 5), 0)
    pixels = base.load()

    cursor_x = 0
    for glyph in glyphs:
        for row_index, row in enumerate(glyph):
            for column_index, cell in enumerate(row):
                if cell == "1":
                    pixels[cursor_x + column_index, row_index] = 1
        cursor_x += 3 + gap

    if scale > 1:
        return base.resize((base.width * scale, base.height * scale), Image.NEAREST)
    return base


def _paste(image: Image.Image, x: int, y: int, sprite: Image.Image) -> None:
    mask_white = Image.new("1", sprite.size, 1)
    image.paste(mask_white, (x, y), sprite)


def _draw_glyphs(image: Image.Image, x: int, y: int, text: str, scale: int, gap: int) -> int:
    sprite = _glyph_image(text, scale, gap)
    _paste(image, x, y, sprite)
    return sprite.width


def _draw_glyphs_right(image: Image.Image, right_x: int, y: int, text: str, scale: int, gap: int) -> int:
    sprite = _glyph_image(text, scale, gap)
    _paste(image, right_x - sprite.width, y, sprite)
    return sprite.width


# ---------------------------------------------------------------------------
# Tema "delta": estilo terminal con columnas restante / delta / reset, barra
# sólida con marca de lo esperado por la regresión lineal.
# ---------------------------------------------------------------------------


def _fmt_reset_clock(reset_in_seconds: int) -> str:
    # Para >= 24h se muestra la duración (XdYh); para menos, la HORA LOCAL real
    # a la que se resetea (hora actual + lo que falta), no una cuenta regresiva.
    seconds = max(0, reset_in_seconds)
    if seconds >= 24 * 3600:
        hours = seconds // 3600
        return f"{hours // 24}D{hours % 24}H"
    reset_at = datetime.now() + timedelta(seconds=seconds)
    return reset_at.strftime("%H:%M")


def _fmt_reset_remaining(reset_in_seconds: int) -> str:
    # Tiempo restante para el reset en horas y minutos (p.ej. 2H05M; 45M si <1h).
    seconds = max(0, reset_in_seconds)
    total_minutes = seconds // 60
    hours = total_minutes // 60
    minutes = total_minutes % 60
    if hours > 0:
        return f"{hours}H{minutes:02d}M"
    return f"{minutes}M"


def _delta_bar(
    draw: ImageDraw.ImageDraw,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    remaining_ratio: float,
    expected_ratio: float,
) -> None:
    # Barra sólida hasta el % restante real. El tramo entre lo actual y lo
    # esperado (regresión lineal) se marca: superávit (delta positivo) lleva 1px
    # negro arriba y abajo dentro del sólido; déficit (delta negativo) lleva
    # puntos dispersos desde lo actual hasta lo esperado.
    draw.rectangle((x0, y0, x1, y1), outline=1)
    inner_x0 = x0 + 2
    inner_y0 = y0 + 2
    inner_y1 = y1 - 2
    available = (x1 - 2) - inner_x0
    actual_x = inner_x0 + int(available * max(0.0, min(1.0, remaining_ratio)))
    expected_x = inner_x0 + int(available * max(0.0, min(1.0, expected_ratio)))

    # Relleno sólido hasta lo actual.
    if actual_x > inner_x0:
        draw.rectangle((inner_x0, inner_y0, actual_x, inner_y1), fill=1)

    if actual_x >= expected_x:
        # Superávit: tramo extra (de lo esperado a lo actual) con 1px negro arriba
        # y abajo dentro del sólido, dejando una banda blanca al centro.
        draw.line((expected_x, inner_y0, actual_x, inner_y0), fill=0)
        draw.line((expected_x, inner_y1, actual_x, inner_y1), fill=0)
    else:
        # Déficit: puntos dispersos desde lo actual hasta lo esperado. El offset de 2px
        # separa los puntos del relleno sólido; pero con 0% restante no hay sólido y ese
        # offset dejaba un hueco negro pegado al borde interior izquierdo. En ese caso se
        # arranca justo en el borde para que el patrón disperso llegue hasta la izquierda.
        start_px = actual_x + 2 if actual_x > inner_x0 else inner_x0
        for px in range(start_px, expected_x + 1, 2):
            for py in range(inner_y0, inner_y1 + 1, 2):
                draw.point((px, py), fill=1)


def _draw_delta_window_bar(
    draw: ImageDraw.ImageDraw, x0: int, y0: int, x1: int, y1: int, window: UsageWindowSnapshot
) -> None:
    remaining_ratio = max(0.0, min(100.0, window["remaining_percent"])) / 100.0
    expected_ratio = max(0.0, min(100.0, window["expected_remaining_percent"])) / 100.0
    _delta_bar(draw, x0, y0, x1, y1, remaining_ratio, expected_ratio)


def _pace_delta_text(window: UsageWindowSnapshot) -> str:
    # Delta entre lo restante y lo esperado: positivo = margen extra disponible
    # (vas por debajo del ritmo), negativo = vas pasado del ritmo.
    delta = int(round(window["remaining_percent"] - window["expected_remaining_percent"]))
    if delta > 0:
        return f"+{delta}%"
    return f"{delta}%"


def _split_reset_units(reset_text: str) -> tuple[str, str]:
    # Separa el reset en (unidad mayor, unidad menor) tras la primera letra de
    # unidad: "2H05M" -> ("2H", "05M"), "5D12H" -> ("5D", "12H"). Si no hay
    # segunda unidad ("45M", "14:30") el resto queda vacío.
    for index, char in enumerate(reset_text):
        if char in ("H", "D") and index + 1 < len(reset_text):
            return (reset_text[: index + 1], reset_text[index + 1 :])
    return (reset_text, "")


def _draw_reset_right(
    image: Image.Image,
    right_x: int,
    top_y: int,
    reset_text: str,
    scale: int,
    gap: int,
    unit_gap_px: int,
) -> int:
    # Reset alineado a la derecha; con unit_gap_px > 0 AGREGA ese hueco negro sobre
    # la separacion normal entre la unidad mayor y la menor (horas|minutos, dias|horas).
    head, tail = _split_reset_units(reset_text)
    if unit_gap_px <= 0 or tail == "":
        return _draw_glyphs_right(image, right_x, top_y, reset_text, scale, gap)

    # Hueco del borde = separacion normal entre glifos (gap * scale) + el extra pedido.
    boundary_gap = gap * scale + unit_gap_px
    tail_sprite = _glyph_image(tail, scale, gap)
    head_sprite = _glyph_image(head, scale, gap)
    _paste(image, right_x - tail_sprite.width, top_y, tail_sprite)
    head_right = right_x - tail_sprite.width - boundary_gap
    _paste(image, head_right - head_sprite.width, top_y, head_sprite)
    return tail_sprite.width + boundary_gap + head_sprite.width


def _detailed_block(
    image: Image.Image,
    draw: ImageDraw.ImageDraw,
    top_y: int,
    window: UsageWindowSnapshot,
    center_text: str,
    reset_text: str,
    draw_bar: Callable[[ImageDraw.ImageDraw, int, int, int, int, UsageWindowSnapshot], None],
    unit_gap_px: int,
) -> None:
    remaining = int(round(max(0.0, min(100.0, window["remaining_percent"]))))

    # Fila de valores: restante (izq), texto central, reset (der). El reset va pegado
    # al borde derecho (sin hueco) tanto si quedan días como si solo quedan minutos.
    # El texto central se centra dentro del hueco real entre la columna izquierda
    # y la de reset, no sobre el ancho total, para que no parezca pegado a un lado.
    left_width = _draw_glyphs(image, 0, top_y, f"{remaining}%", 2, 1)
    right_width = _draw_reset_right(image, WIDTH, top_y, reset_text, 2, 1, unit_gap_px)
    center_sprite = _glyph_image(center_text, 2, 1)
    gap_start = left_width
    gap_end = WIDTH - right_width
    center_x = gap_start + (gap_end - gap_start - center_sprite.width) // 2
    _paste(image, center_x, top_y, center_sprite)

    # Barra de uso grande (el estilo lo decide cada tema vía draw_bar).
    draw_bar(draw, 0, top_y + 12, WIDTH - 1, top_y + 23, window)


def _last_octet(ip: str) -> str:
    # Devuelve el último octeto de una IPv4 (p.ej. "192.168.1.198" -> "198").
    return ip.rsplit(".", 1)[-1]


def _truncate_glyphs_to(text: str, start_x: int, right_x: int) -> str:
    # Recorta el texto (fuente 3x5 con 1 px de separación) para que no rebase right_x.
    available = right_x - start_x
    if available <= 0:
        return ""
    max_chars = (available + 1) // 4
    return text[:max_chars]


def _render_detailed_columns(
    label: str,
    snapshot: ToolSnapshot,
    center_text: Callable[[UsageWindowSnapshot], str],
    draw_bar: Callable[[ImageDraw.ImageDraw, int, int, int, int, UsageWindowSnapshot], None],
    unit_gap_px: int,
) -> Image.Image:
    # Render base estilo terminal del tema "delta": center_text decide el texto de la
    # columna central y draw_bar el estilo de barra.
    image = Image.new("1", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)

    # Cabecera compacta estilo terminal (letras de 1 px): ">_ ETIQUETA  IP: x.x.x.x" + indicador.
    prompt_width = _draw_glyphs(image, 0, 1, ">_", 1, 1)
    label_width = _draw_glyphs(image, prompt_width + 3, 1, label.upper(), 1, 1)
    if _device_ip != "":
        ip_start_x = prompt_width + label_width + 8
        # Solo el último octeto: deja más espacio horizontal a la animación de la esquina.
        ip_text = _truncate_glyphs_to(f"IP:{_last_octet(_device_ip)}", ip_start_x, CONTENT_RIGHT_X)
        _draw_glyphs(image, ip_start_x, 1, ip_text, 1, 1)
    # La divisoria se mantiene a todo el ancho (no se corta bajo la animación).
    draw.line((0, 8, WIDTH - 1, 8), fill=1)

    # Se deja libre la última fila (y=63): en el OLED real se ve arriba (wrap).
    current = snapshot["current"]
    weekly = snapshot["weekly"]
    # Ventana de 5h: horas y minutos restantes para el reset.
    _detailed_block(
        image, draw, 10, current, center_text(current), _fmt_reset_remaining(current["reset_in_seconds"]), draw_bar, unit_gap_px
    )
    # Ventana semanal: días y horas (o hora local si falta < 24h).
    _detailed_block(
        image, draw, 38, weekly, center_text(weekly), _fmt_reset_clock(weekly["reset_in_seconds"]), draw_bar, unit_gap_px
    )

    return image


def _render_delta(label: str, snapshot: ToolSnapshot) -> Image.Image:
    # Columna central con el delta numérico (+10% margen extra / -5% pasado) y
    # barra sólida con marca de lo esperado por la regresión lineal.
    # unit_gap_px=1: 1 px negro entre horas|minutos y dias|horas en el reset.
    return _render_detailed_columns(label, snapshot, _pace_delta_text, _draw_delta_window_bar, 4)


# ---------------------------------------------------------------------------
# Registro de temas.
# ---------------------------------------------------------------------------

_THEMES: dict[str, Callable[[str, ToolSnapshot], Image.Image]] = {
    "delta": _render_delta,
}


def render_tool_image(label: str, snapshot: ToolSnapshot, theme: str) -> Image.Image:
    # Dibuja la pantalla completa 128x64 (1 bit) según el tema elegido. La esquina
    # superior derecha (ACTIVITY_BOX_*) se deja libre de contenido a propósito: ahí
    # el firmware del ESP32 superpone la animación de actividad.
    renderer = _THEMES.get(theme, _THEMES[DEFAULT_THEME])
    return renderer(label, snapshot)


def pack_frame(image: Image.Image) -> bytes:
    # Empaqueta a formato Adafruit drawBitmap: row-major, MSB primero,
    # filas alineadas a byte. Para 128 px de ancho son 16 bytes por fila.
    return image.convert("1").tobytes()


def _draw_mascot_claude(draw: ImageDraw.ImageDraw, cx: int, cy: int) -> None:
    # "Spark" de Claude: 12 rayos radiales desde el centro (igual que el firmware).
    inner_radius = 2.5
    outer_radius = 11.0
    for index in range(12):
        angle = index * (math.pi / 6.0)
        draw.line(
            (
                cx + round(math.cos(angle) * inner_radius),
                cy + round(math.sin(angle) * inner_radius),
                cx + round(math.cos(angle) * outer_radius),
                cy + round(math.sin(angle) * outer_radius),
            ),
            fill=1,
        )
    draw.ellipse((cx - 2, cy - 2, cx + 2, cy + 2), fill=1)


def _draw_mascot_codex(draw: ImageDraw.ImageDraw, cx: int, cy: int) -> None:
    # Codex: ventana de terminal con el prompt ">_" (igual que el firmware).
    width = 28
    height = 22
    x = cx - width // 2
    y = cy - height // 2
    draw.rounded_rectangle((x, y, x + width - 1, y + height - 1), radius=3, outline=1)
    draw.line((x + 1, y + 6, x + width - 2, y + 6), fill=1)
    draw.ellipse((x + 3, y + 2, x + 5, y + 4), fill=1)
    draw.ellipse((x + 7, y + 2, x + 9, y + 4), fill=1)
    # Prompt ">_" dibujado con líneas para no depender de una fuente.
    draw.line((x + 6, y + 10, x + 9, y + 13), fill=1)
    draw.line((x + 9, y + 13, x + 6, y + 16), fill=1)
    draw.line((x + 11, y + 16, x + 15, y + 16), fill=1)


def draw_boot_decor(image: Image.Image, is_claude: bool) -> None:
    # Marco de pixeles blancos + mascot + divisor, en sitio. El texto lo añade quien llama.
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, WIDTH - 1, HEIGHT - 1), outline=1)
    if is_claude:
        _draw_mascot_claude(draw, BOOT_MASCOT_CX, BOOT_MASCOT_CY)
    else:
        _draw_mascot_codex(draw, BOOT_MASCOT_CX, BOOT_MASCOT_CY)
    draw.line((BOOT_DIVIDER_X, 3, BOOT_DIVIDER_X, HEIGHT - 4), fill=1)
