from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image


def load(path: str) -> Image.Image:
    return Image.open(Path(path)).convert("RGB")


def mean_corner_delta(clear: Image.Image, blurred: Image.Image, radius: int = 36) -> float:
    width, height = clear.size
    samples: list[int] = []
    corners = (
        (0, 0, 1, 1),
        (width - 1, 0, -1, 1),
        (0, height - 1, 1, -1),
        (width - 1, height - 1, -1, -1),
    )
    for origin_x, origin_y, direction_x, direction_y in corners:
        center_x = origin_x + direction_x * radius
        center_y = origin_y + direction_y * radius
        for local_y in range(radius):
            for local_x in range(radius):
                x = origin_x + direction_x * local_x
                y = origin_y + direction_y * local_y
                if (x - center_x) ** 2 + (y - center_y) ** 2 <= radius**2:
                    continue
                before = clear.getpixel((x, y))
                after = blurred.getpixel((x, y))
                samples.extend(abs(a - b) for a, b in zip(before, after))
    return sum(samples) / max(1, len(samples))


def edge_energy(image: Image.Image) -> float:
    gray = image.convert("L")
    # Ignore rounded corners, the title, and the one-pixel border. This region
    # contains only the wallpaper filtered through the glass.
    crop = gray.crop((24, 44, gray.width - 24, gray.height - 24))
    width, height = crop.size
    get_pixels = getattr(crop, "get_flattened_data", crop.getdata)
    pixels = list(get_pixels())
    total = 0
    count = 0
    for y in range(1, height):
        row = y * width
        previous_row = (y - 1) * width
        for x in range(1, width):
            value = pixels[row + x]
            total += abs(value - pixels[row + x - 1])
            total += abs(value - pixels[previous_row + x])
            count += 2
    return total / max(1, count)


if len(sys.argv) != 4:
    raise SystemExit("usage: blur_visual_assert.py blur-0.bmp blur-1.bmp blur-100.bmp")

clear, light, strong = map(load, sys.argv[1:])
if not (clear.size == light.size == strong.size):
    raise AssertionError("snapshot sizes differ")

corner_delta = mean_corner_delta(clear, light)
clear_edges = edge_energy(clear)
light_edges = edge_energy(light)
strong_edges = edge_energy(strong)

print(f"corner_delta_0_to_1={corner_delta:.3f}")
print(f"edge_energy_0={clear_edges:.3f}")
print(f"edge_energy_1={light_edges:.3f}")
print(f"edge_energy_100={strong_edges:.3f}")

# The one-pixel antialiased border legitimately changes by a couple of RGB
# levels when the backing surface becomes opaque. A rectangular blur leak was
# more than 30 levels in this metric, so 3 still sharply catches the bug while
# allowing correct subpixel border blending.
assert corner_delta < 3.0, "blur leaks into the transparent rounded corners"
assert light_edges >= clear_edges * 0.65, "1% blur is already much too strong"
assert strong_edges <= light_edges * 0.75, "1% and 100% blur are not visually distinct"
