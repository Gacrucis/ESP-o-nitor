import math
import random
import string
from io import BytesIO

from PIL import Image, ImageDraw, ImageFont

# Salvapantallas a pantalla completa (anti burn-in cuando el servicio está caído).
# Aquí solo se generan los PREVIEWS animados de la web; el ESP los renderiza de
# forma procedural en su firmware con un aspecto equivalente.
WIDTH = 128
HEIGHT = 64
PREVIEW_SCALE = 2
PREVIEW_FRAMES = 48
PREVIEW_INTERVAL_MS = 80


def _new_frame() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("1", (WIDTH, HEIGHT), 0)
    return (image, ImageDraw.Draw(image))


def _black_frames() -> list[Image.Image]:
    # Pantalla apagada: lo más seguro contra burn-in.
    return [_new_frame()[0] for _ in range(2)]


def _snake_frames() -> list[Image.Image]:
    # Culebra que persigue comida en una grilla y crece.
    cell = 4
    cols = WIDTH // cell
    rows = HEIGHT // cell
    rng = random.Random(7)
    snake = [(cols // 2, rows // 2)]
    length = 5
    food = (rng.randrange(cols), rng.randrange(rows))
    frames: list[Image.Image] = []

    for _ in range(PREVIEW_FRAMES):
        head_x, head_y = snake[-1]
        step_x = (food[0] > head_x) - (food[0] < head_x)
        step_y = (food[1] > head_y) - (food[1] < head_y)
        if step_x != 0 and step_y != 0:
            # Un eje por paso para que se vea como movimiento de culebra.
            if rng.random() < 0.5:
                step_y = 0
            else:
                step_x = 0
        head = ((head_x + step_x) % cols, (head_y + step_y) % rows)
        snake.append(head)
        if head == food:
            length += 2
            food = (rng.randrange(cols), rng.randrange(rows))
        while len(snake) > length:
            snake.pop(0)

        image, draw = _new_frame()
        fx, fy = food
        draw.rectangle((fx * cell + 1, fy * cell + 1, fx * cell + cell - 2, fy * cell + cell - 2), fill=1)
        for segment_x, segment_y in snake:
            draw.rectangle((segment_x * cell, segment_y * cell, segment_x * cell + cell - 2, segment_y * cell + cell - 2), fill=1)
        frames.append(image)
    return frames


def _pipes_frames() -> list[Image.Image]:
    # Tubería estilo Win95: crece en línea recta con giros aleatorios; al llenarse, limpia.
    cell = 8
    cols = WIDTH // cell
    rows = HEIGHT // cell
    rng = random.Random(3)
    directions = [(1, 0), (0, 1), (-1, 0), (0, -1)]
    position = (cols // 2, rows // 2)
    direction = 0
    drawn: list[tuple[int, int]] = [position]
    frames: list[Image.Image] = []

    def render(points: list[tuple[int, int]]) -> Image.Image:
        image, draw = _new_frame()
        for px, py in points:
            cx = px * cell + cell // 2
            cy = py * cell + cell // 2
            draw.ellipse((cx - 2, cy - 2, cx + 2, cy + 2), outline=1)
        return image

    for _ in range(PREVIEW_FRAMES):
        if rng.random() < 0.3:
            direction = (direction + rng.choice((1, 3))) % 4
        dx, dy = directions[direction]
        nx, ny = position[0] + dx, position[1] + dy
        if nx < 0 or nx >= cols or ny < 0 or ny >= rows:
            direction = (direction + 2) % 4
            nx, ny = position[0] - dx, position[1] - dy
        position = (max(0, min(cols - 1, nx)), max(0, min(rows - 1, ny)))
        drawn.append(position)
        if len(drawn) > cols * rows:
            drawn = [position]
        frames.append(render(drawn))
    return frames


def _matrix_frames() -> list[Image.Image]:
    # Lluvia de código: columnas de caracteres alfanuméricos que caen.
    font = ImageFont.load_default()
    column_width = 10
    row_height = 11
    columns = WIDTH // column_width
    rows = HEIGHT // row_height + 1
    rng = random.Random(11)
    charset = string.ascii_uppercase + string.digits
    heads = [rng.uniform(-rows, 0.0) for _ in range(columns)]
    speeds = [rng.uniform(0.5, 1.3) for _ in range(columns)]
    glyphs = [[rng.choice(charset) for _ in range(rows)] for _ in range(columns)]
    tail = 6
    frames: list[Image.Image] = []

    for _ in range(PREVIEW_FRAMES):
        image, draw = _new_frame()
        for index in range(columns):
            head = heads[index]
            x = index * column_width
            for step in range(tail):
                row = int(head) - step
                if 0 <= row < rows:
                    draw.text((x, row * row_height), glyphs[index][row], font=font, fill=1)
            heads[index] += speeds[index]
            if head - tail > rows:
                heads[index] = rng.uniform(-rows, -1.0)
                speeds[index] = rng.uniform(0.5, 1.3)
            # Mutar un glifo al azar da el parpadeo característico de la lluvia.
            glyphs[index][rng.randrange(rows)] = rng.choice(charset)
        frames.append(image)
    return frames


def _dvd_frames() -> list[Image.Image]:
    # Mini logo DVD (oval + "DVD" + banda "VIDEO") que rebota en los bordes.
    font = ImageFont.load_default()
    box_w = 40
    box_h = 22
    x = 8.0
    y = 8.0
    # Velocidad constante 2.0/1.0 (suave, mas lento que el clasico 3.0/2.0).
    vx = 2.0
    vy = 1.0
    frames: list[Image.Image] = []

    for _ in range(PREVIEW_FRAMES):
        x += vx
        y += vy
        if x <= 0 or x + box_w >= WIDTH:
            vx = -vx
            x = max(0.0, min(float(WIDTH - box_w), x))
        if y <= 0 or y + box_h >= HEIGHT:
            vy = -vy
            y = max(0.0, min(float(HEIGHT - box_h), y))
        image, draw = _new_frame()
        left = int(x)
        top = int(y)
        center_x = left + box_w // 2
        # Oval caracteristico + "DVD" dentro + banda "VIDEO" debajo.
        draw.ellipse((center_x - 18, top + 1, center_x + 18, top + 13), outline=1)
        draw.text((center_x - 9, top + 3), "DVD", font=font, fill=1)
        draw.rounded_rectangle((left + 4, top + 14, left + 36, top + 22), radius=2, fill=1)
        draw.text((left + 6, top + 14), "VIDEO", font=font, fill=0)
        frames.append(image)
    return frames


def _maze_frames() -> list[Image.Image]:
    # Laberinto perfecto (recursive backtracker) con un punto recorriendo los pasillos.
    cell = 8
    cols = WIDTH // cell
    rows = HEIGHT // cell
    rng = random.Random(5)

    # Genera el laberinto quitando paredes entre celdas conectadas.
    open_edges: set[tuple[tuple[int, int], tuple[int, int]]] = set()
    visited = [[False] * cols for _ in range(rows)]
    stack = [(0, 0)]
    visited[0][0] = True
    while stack:
        cx, cy = stack[-1]
        neighbors = [
            (nx, ny)
            for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1))
            if 0 <= nx < cols and 0 <= ny < rows and not visited[ny][nx]
        ]
        if neighbors:
            nx, ny = rng.choice(neighbors)
            open_edges.add(((cx, cy), (nx, ny)))
            open_edges.add(((nx, ny), (cx, cy)))
            visited[ny][nx] = True
            stack.append((nx, ny))
        else:
            stack.pop()

    def connected(a: tuple[int, int], b: tuple[int, int]) -> bool:
        return (a, b) in open_edges

    def draw_maze(draw: ImageDraw.ImageDraw) -> None:
        for cy in range(rows):
            for cx in range(cols):
                x0, y0 = cx * cell, cy * cell
                x1, y1 = x0 + cell - 1, y0 + cell - 1
                if cx == 0:
                    draw.line((x0, y0, x0, y1), fill=1)
                if cy == 0:
                    draw.line((x0, y0, x1, y0), fill=1)
                if cx + 1 >= cols or not connected((cx, cy), (cx + 1, cy)):
                    draw.line((x1, y0, x1, y1), fill=1)
                if cy + 1 >= rows or not connected((cx, cy), (cx, cy + 1)):
                    draw.line((x0, y1, x1, y1), fill=1)

    position = (0, 0)
    walked = {position}
    frames: list[Image.Image] = []
    for _ in range(PREVIEW_FRAMES):
        cx, cy = position
        moves = [
            (nx, ny)
            for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1))
            if 0 <= nx < cols and 0 <= ny < rows and connected((cx, cy), (nx, ny))
        ]
        fresh = [move for move in moves if move not in walked]
        if fresh:
            position = rng.choice(fresh)
        elif moves:
            position = rng.choice(moves)
        walked.add(position)

        image, draw = _new_frame()
        draw_maze(draw)
        dot_x, dot_y = position
        draw.ellipse((dot_x * cell + 2, dot_y * cell + 2, dot_x * cell + cell - 3, dot_y * cell + cell - 3), fill=1)
        frames.append(image)
    return frames


def _flower_frames() -> list[Image.Image]:
    # "Flower Box": curva rosa r=cos(k*theta) que rota y rebota en las paredes.
    radius = 14.0
    x = WIDTH / 2.0
    y = HEIGHT / 2.0
    vx = 2.3
    vy = 1.7
    petals = 5
    frames: list[Image.Image] = []

    for frame_index in range(PREVIEW_FRAMES):
        x += vx
        y += vy
        if x - radius <= 0 or x + radius >= WIDTH:
            vx = -vx
            x = max(radius, min(WIDTH - radius, x))
        if y - radius <= 0 or y + radius >= HEIGHT:
            vy = -vy
            y = max(radius, min(HEIGHT - radius, y))
        rotation = frame_index * 0.2
        image, draw = _new_frame()
        previous: tuple[int, int] | None = None
        steps = 120
        for step in range(steps + 1):
            theta = (step / steps) * math.tau
            r = radius * abs(math.cos(petals * theta))
            point = (int(x + r * math.cos(theta + rotation)), int(y + r * math.sin(theta + rotation)))
            if previous is not None:
                draw.line((previous[0], previous[1], point[0], point[1]), fill=1)
            previous = point
        frames.append(image)
    return frames


_SAVER_BUILDERS = {
    "black": _black_frames,
    "snake": _snake_frames,
    "pipes": _pipes_frames,
    "matrix": _matrix_frames,
    "dvd": _dvd_frames,
    "maze": _maze_frames,
    "flower": _flower_frames,
}


def render_screensaver_preview_gif(saver: str) -> bytes:
    builder = _SAVER_BUILDERS.get(saver, _black_frames)
    frames = builder()
    scaled = [
        frame.convert("L").resize((WIDTH * PREVIEW_SCALE, HEIGHT * PREVIEW_SCALE), Image.NEAREST)
        for frame in frames
    ]

    buffer = BytesIO()
    scaled[0].save(
        buffer,
        format="GIF",
        save_all=True,
        append_images=scaled[1:],
        duration=PREVIEW_INTERVAL_MS,
        loop=0,
    )
    return buffer.getvalue()
