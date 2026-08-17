"""Visual regression check for live blur over an animated desktop wallpaper.

The test captures two desktop frames, measures temporal pixel changes outside
DesktopOrganizer and inside its visible containers, and fails when the desktop
is moving while the container background is effectively frozen.
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageDraw, ImageGrab, ImageStat


USER32 = ctypes.windll.user32
DWMAPI = ctypes.windll.dwmapi


class Rect(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    ]


def organizer_container_rects(process_id: int | None = None) -> list[tuple[int, int, int, int]]:
    rects: list[tuple[int, int, int, int]] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def visit(hwnd: int, _lparam: int) -> bool:
        if not USER32.IsWindowVisible(hwnd):
            return True
        if process_id is not None:
            owner_process = wintypes.DWORD()
            USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_process))
            if owner_process.value != process_id:
                return True
        class_name = ctypes.create_unicode_buffer(256)
        USER32.GetClassNameW(hwnd, class_name, len(class_name))
        if class_name.value != "DesktopOrganizer.Container":
            return True
        rect = Rect()
        if USER32.GetWindowRect(hwnd, ctypes.byref(rect)):
            if rect.right > rect.left and rect.bottom > rect.top:
                rects.append((rect.left, rect.top, rect.right, rect.bottom))
        return True

    USER32.EnumWindows(visit, 0)
    return rects


def top_level_windows() -> list[tuple[str, tuple[int, int, int, int]]]:
    windows: list[tuple[str, tuple[int, int, int, int]]] = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    @callback_type
    def visit(hwnd: int, _lparam: int) -> bool:
        if not USER32.IsWindowVisible(hwnd):
            return True
        cloaked = wintypes.DWORD()
        if DWMAPI.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(cloaked), ctypes.sizeof(cloaked)) == 0:
            if cloaked.value:
                return True
        class_name = ctypes.create_unicode_buffer(256)
        USER32.GetClassNameW(hwnd, class_name, len(class_name))
        rect = Rect()
        if USER32.GetWindowRect(hwnd, ctypes.byref(rect)) and rect.right > rect.left and rect.bottom > rect.top:
            windows.append((class_name.value, (rect.left, rect.top, rect.right, rect.bottom)))
        return True

    USER32.EnumWindows(visit, 0)
    return windows


def virtual_screen_bounds() -> tuple[int, int, int, int]:
    x = USER32.GetSystemMetrics(76)  # SM_XVIRTUALSCREEN
    y = USER32.GetSystemMetrics(77)  # SM_YVIRTUALSCREEN
    width = USER32.GetSystemMetrics(78)  # SM_CXVIRTUALSCREEN
    height = USER32.GetSystemMetrics(79)  # SM_CYVIRTUALSCREEN
    return x, y, x + width, y + height


def temporal_energy(diff: Image.Image, mask: Image.Image) -> float:
    gray = diff.convert("L")
    return float(ImageStat.Stat(gray, mask=mask).mean[0])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interval", type=float, default=3.0)
    parser.add_argument("--output", type=Path, default=Path("build-visual/artifacts/dynamic-blur"))
    parser.add_argument(
        "--source-probe",
        type=Path,
        default=Path("build-local/Release/LiveWallpaperCaptureProbe.exe"),
    )
    parser.add_argument("--minimum-outside-energy", type=float, default=0.001)
    parser.add_argument("--minimum-source-energy", type=float, default=0.001)
    parser.add_argument("--minimum-live-ratio", type=float, default=0.002)
    parser.add_argument("--process-id", type=int)
    args = parser.parse_args()

    rects = organizer_container_rects(args.process_id)
    if not rects:
        print("INCONCLUSIVE: no visible DesktopOrganizer container windows")
        return 2

    bounds = virtual_screen_bounds()
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.source_probe.exists():
        print(f"INCONCLUSIVE: live-wallpaper source probe not found: {args.source_probe}")
        return 2
    source_output = args.output / f"source-{time.time_ns()}"
    source_output.mkdir(parents=True, exist_ok=True)
    probe = subprocess.Popen(
        [str(args.source_probe), str(source_output), str(round(args.interval * 1000))],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    ready = source_output / "ready.flag"
    deadline = time.monotonic() + 7.0
    while not ready.exists() and time.monotonic() < deadline and probe.poll() is None:
        time.sleep(0.02)
    if not ready.exists():
        output, _ = probe.communicate(timeout=5)
        print(f"INCONCLUSIVE: source probe did not produce its first frame\n{output}")
        return 2
    frame_a = ImageGrab.grab(bbox=bounds, all_screens=True).convert("RGB")
    time.sleep(args.interval)
    frame_b = ImageGrab.grab(bbox=bounds, all_screens=True).convert("RGB")
    try:
        probe_output, _ = probe.communicate(timeout=7)
    except subprocess.TimeoutExpired:
        probe.kill()
        probe_output, _ = probe.communicate()
        print(f"INCONCLUSIVE: source probe timed out\n{probe_output}")
        return 2
    if probe.returncode != 0:
        print(f"INCONCLUSIVE: source probe failed with {probe.returncode}\n{probe_output}")
        return 2
    source_a = Image.open(source_output / "desktop-a.bmp").convert("RGB")
    source_b = Image.open(source_output / "desktop-b.bmp").convert("RGB")
    frame_a.save(args.output / "frame-a.png")
    frame_b.save(args.output / "frame-b.png")

    diff = ImageChops.difference(frame_a, frame_b)
    source_diff = ImageChops.difference(source_a, source_b)
    width, height = frame_a.size
    if source_a.size != (width, height) or source_b.size != (width, height):
        print(f"INCONCLUSIVE: source size {source_a.size}/{source_b.size} != screen {(width, height)}")
        return 2
    inside_mask = Image.new("L", (width, height), 0)
    outside_mask = Image.new("L", (width, height), 255)
    inside_draw = ImageDraw.Draw(inside_mask)
    outside_draw = ImageDraw.Draw(outside_mask)
    origin_x, origin_y = bounds[0], bounds[1]

    usable_rects: list[tuple[int, int, int, int]] = []
    for left, top, right, bottom in rects:
        # Stay away from antialiased window edges while retaining title and item
        # areas; static foreground content only makes this assertion stricter.
        local = (
            max(0, left - origin_x + 8),
            max(0, top - origin_y + 8),
            min(width, right - origin_x - 8),
            min(height, bottom - origin_y - 8),
        )
        if local[2] > local[0] and local[3] > local[1]:
            usable_rects.append(local)
            inside_draw.rectangle(local, fill=255)
        full_local = (
            max(0, left - origin_x),
            max(0, top - origin_y),
            min(width, right - origin_x),
            min(height, bottom - origin_y),
        )
        outside_draw.rectangle(full_local, fill=0)

    # Exclude the taskbar and a small perimeter where clocks/cursor-edge effects
    # can create changes unrelated to the wallpaper.
    outside_draw.rectangle((0, max(0, height - 64), width, height), fill=0)
    outside_draw.rectangle((0, 0, width, 4), fill=0)
    outside_draw.rectangle((0, 0, 4, height), fill=0)
    outside_draw.rectangle((max(0, width - 4), 0, width, height), fill=0)

    desktop_classes = {"Progman", "WorkerW"}
    organizer_classes = {
        "DesktopOrganizer.Container",
        "DesktopOrganizer.Tint",
        "DesktopOrganizer.Content",
    }
    for class_name, (left, top, right, bottom) in top_level_windows():
        local = (
            max(0, left - origin_x),
            max(0, top - origin_y),
            min(width, right - origin_x),
            min(height, bottom - origin_y),
        )
        if local[2] <= local[0] or local[3] <= local[1]:
            continue
        # Foreground applications can cover desktop-level organizer windows.
        # Remove them from both samples so a playing video/game cannot create a
        # false pass. Organizer surfaces remain in the inside sample and are
        # already removed from the outside sample above.
        if class_name in desktop_classes:
            continue
        if class_name in organizer_classes:
            outside_draw.rectangle(local, fill=0)
            continue
        inside_draw.rectangle(local, fill=0)
        outside_draw.rectangle(local, fill=0)

    inside_pixels = inside_mask.histogram()[255]
    outside_pixels = outside_mask.histogram()[255]
    if inside_pixels < 1000 or outside_pixels < 1000:
        print(f"INCONCLUSIVE: insufficient visible sample area inside={inside_pixels} outside={outside_pixels}")
        return 2

    inside = temporal_energy(diff, inside_mask)
    outside = temporal_energy(diff, outside_mask)
    source_inside = temporal_energy(source_diff, inside_mask)
    source_outside = temporal_energy(source_diff, outside_mask)
    ratio = inside / source_inside if source_inside else 0.0

    visualization = frame_b.copy()
    overlay = ImageDraw.Draw(visualization)
    for local in usable_rects:
        overlay.rectangle(local, outline=(255, 40, 40), width=3)
    visualization.save(args.output / "measured-regions.png")
    diff.save(args.output / "temporal-diff.png")
    source_diff.save(args.output / "source-temporal-diff.png")
    inside_mask.save(args.output / "inside-mask.png")
    visible_bounds = inside_mask.getbbox()
    if visible_bounds:
        gray = Image.new("RGB", frame_a.size, (235, 235, 235))
        visible_a = Image.composite(frame_a, gray, inside_mask).crop(visible_bounds)
        visible_b = Image.composite(frame_b, gray, inside_mask).crop(visible_bounds)
        comparison = Image.new(
            "RGB", (visible_a.width * 2, visible_a.height + 28), (248, 248, 248)
        )
        comparison.paste(visible_a, (0, 28))
        comparison.paste(visible_b, (visible_a.width, 28))
        labels = ImageDraw.Draw(comparison)
        labels.text((8, 7), "Frame A", fill=(25, 25, 25))
        labels.text((visible_a.width + 8, 7), f"Frame B (+{args.interval:g}s)", fill=(25, 25, 25))
        comparison.save(args.output / "container-live-comparison.png")

    print(f"containers={len(usable_rects)}")
    print(f"inside_pixels={inside_pixels}")
    print(f"outside_pixels={outside_pixels}")
    print(f"outside_temporal_energy={outside:.4f}")
    print(f"inside_temporal_energy={inside:.4f}")
    print(f"source_outside_temporal_energy={source_outside:.4f}")
    print(f"source_inside_temporal_energy={source_inside:.4f}")
    print(f"inside_to_source_ratio={ratio:.4f}")

    if outside < args.minimum_outside_energy:
        print("INCONCLUSIVE: desktop wallpaper did not visibly change during capture")
        return 2
    if source_inside < args.minimum_source_energy:
        print("INCONCLUSIVE: the live wallpaper did not change under the visible container regions")
        return 2
    if ratio < args.minimum_live_ratio:
        print("FAIL: organizer blur is frozen while the desktop wallpaper is moving")
        return 1
    print("PASS: organizer blur changes with the animated desktop wallpaper")
    return 0


if __name__ == "__main__":
    sys.exit(main())
