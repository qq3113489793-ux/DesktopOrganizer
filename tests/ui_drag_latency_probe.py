"""Measure DesktopOrganizer slider drag responsiveness without changing saved values.

Run this while DesktopOrganizer is open.  The probe sends the same mouse messages
as a drag, samples the already-rendered console surface, and restores every thumb
to its starting position before moving to the next slider.
"""

from __future__ import annotations

import argparse
import statistics
import time

import win32con
import win32gui
import win32ui


SLIDERS = ("opacity", "blur", "corner")


def capture_scanline(hwnd: int, y: int) -> list[tuple[int, int, int]]:
    left, top, right, bottom = win32gui.GetWindowRect(hwnd)
    width = right - left
    window_dc = win32gui.GetWindowDC(hwnd)
    source = win32ui.CreateDCFromHandle(window_dc)
    memory = source.CreateCompatibleDC()
    bitmap = win32ui.CreateBitmap()
    bitmap.CreateCompatibleBitmap(source, width, 1)
    memory.SelectObject(bitmap)
    memory.BitBlt((0, 0), (width, 1), source, (0, y), win32con.SRCCOPY)
    raw = bitmap.GetBitmapBits(True)
    pixels = [(raw[i + 2], raw[i + 1], raw[i]) for i in range(0, width * 4, 4)]
    win32gui.DeleteObject(bitmap.GetHandle())
    memory.DeleteDC()
    source.DeleteDC()
    win32gui.ReleaseDC(hwnd, window_dc)
    return pixels


def find_thumb_center(hwnd: int, y: int, track_left: int, track_right: int) -> int:
    pixels = capture_scanline(hwnd, y)
    white = [
        x
        for x in range(track_left, min(track_right + 1, len(pixels)))
        if all(channel >= 248 for channel in pixels[x])
    ]
    runs: list[list[int]] = []
    for x in white:
        if not runs or x != runs[-1][-1] + 1:
            runs.append([x])
        else:
            runs[-1].append(x)
    candidates = [run for run in runs if 8 <= len(run) <= 20]
    if not candidates:
        raise RuntimeError(f"cannot locate slider thumb at y={y}")
    run = max(candidates, key=len)
    return round((run[0] + run[-1]) / 2)


def lparam(x: int, y: int) -> int:
    return (y << 16) | (x & 0xFFFF)


def probe_slider(hwnd: int, name: str, duration: float, hz: int) -> dict[str, float]:
    left, top, right, bottom = win32gui.GetClientRect(hwnd)
    y = (bottom - top) // 2
    track_left = 10
    track_right = max(track_left + 1, right - 10)
    start_x = find_thumb_center(hwnd, y, track_left, track_right)
    travel = min(150, track_right - 12 - start_x)
    if travel < 60:
        travel = -min(150, start_x - track_left - 12)
    target_x = start_x + travel
    steps = max(2, round(duration * hz))
    samples: list[tuple[float, int, int]] = []
    handler_ms: list[float] = []

    win32gui.SendMessage(hwnd, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, lparam(start_x, y))
    started = time.perf_counter()
    try:
        for index in range(steps + 1):
            requested = round(start_x + travel * index / steps)
            before = time.perf_counter()
            win32gui.SendMessage(
                hwnd,
                win32con.WM_MOUSEMOVE,
                win32con.MK_LBUTTON,
                lparam(requested, y),
            )
            handler_ms.append((time.perf_counter() - before) * 1000)
            samples.append(
                (
                    time.perf_counter() - started,
                    requested,
                    find_thumb_center(hwnd, y, track_left, track_right),
                )
            )
            deadline = started + (index + 1) / hz
            delay = deadline - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
    finally:
        win32gui.SendMessage(hwnd, win32con.WM_MOUSEMOVE, win32con.MK_LBUTTON, lparam(start_x, y))
        win32gui.SendMessage(hwnd, win32con.WM_LBUTTONUP, 0, lparam(start_x, y))

    frozen_ms = 0.0
    frozen_start = samples[0][0]
    previous = samples[0][2]
    jumps: list[int] = []
    for timestamp, _, rendered in samples[1:]:
        jumps.append(abs(rendered - previous))
        if rendered != previous:
            frozen_ms = max(frozen_ms, (timestamp - frozen_start) * 1000)
            frozen_start = timestamp
        previous = rendered
    frozen_ms = max(frozen_ms, (samples[-1][0] - frozen_start) * 1000)
    ordered = sorted(handler_ms)
    return {
        "start_x": start_x,
        "target_x": target_x,
        "rendered_positions": len({sample[2] for sample in samples}),
        "longest_frozen_ms": frozen_ms,
        "max_jump_px": max(jumps, default=0),
        "handler_p95_ms": ordered[max(0, round(len(ordered) * 0.95) - 1)],
        "handler_max_ms": max(handler_ms, default=0.0),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=0.8)
    parser.add_argument("--hz", type=int, default=120)
    args = parser.parse_args()
    hwnd = win32gui.FindWindow("DesktopOrganizer.Console", "总控制台")
    if not hwnd:
        raise SystemExit("DesktopOrganizer console is not open")
    slider_windows: list[int] = []

    def collect_slider(child: int, _: object) -> bool:
        if win32gui.GetClassName(child) == "DesktopOrganizer.Slider":
            slider_windows.append(child)
        return True

    win32gui.EnumChildWindows(hwnd, collect_slider, None)
    slider_windows.sort(key=lambda child: win32gui.GetWindowRect(child)[1])
    if len(slider_windows) < len(SLIDERS):
        raise SystemExit("cannot find all three console sliders")
    for name, slider in zip(SLIDERS, slider_windows):
        metrics = probe_slider(slider, name, args.duration, args.hz)
        print(
            name,
            " ".join(
                f"{key}={value:.2f}" if isinstance(value, float) else f"{key}={value}"
                for key, value in metrics.items()
            ),
        )
        time.sleep(0.15)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
