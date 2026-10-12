#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Show a bounded animation on one Wayland output and report frame feedback."""
import argparse
import json
import os
import tempfile
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--connector", default="DP-1")
parser.add_argument("--seconds", type=int, default=8, choices=range(3, 31))
args = parser.parse_args()
Gtk.init()
display = Gdk.Display.get_default()
if display is None:
    raise SystemExit("No display connection")
monitors = display.get_monitors()
matches = [monitors.get_item(i) for i in range(monitors.get_n_items())
           if monitors.get_item(i).get_connector() == args.connector]
if len(matches) != 1:
    raise SystemExit("Requested output is absent or ambiguous")

loop = GLib.MainLoop()
window = Gtk.Window(title="Aurora external display frame test")
area = Gtk.DrawingArea()
window.set_child(area)
started = time.monotonic()
stats = {"connector": args.connector, "ticks": 0, "draws": 0}
feedback = set()
complete_frames = set()


def draw(_area, context, width, height):
    phase = (time.monotonic() - started) % 2 / 2
    context.set_source_rgb(0.06, 0.10, 0.17)
    context.paint()
    context.set_source_rgb(0.15, 0.80, 0.55)
    context.rectangle(phase * width, height * 0.25, width * 0.15, height * 0.5)
    context.fill()
    stats["draws"] += 1


def tick(widget, clock):
    stats["ticks"] += 1
    counter = clock.get_frame_counter()
    for frame in range(max(0, counter - 16), counter):
        previous = clock.get_timings(frame)
        if previous is not None and previous.get_complete():
            complete_frames.add(frame)
            presented = previous.get_presentation_time()
            if presented:
                feedback.add(presented)
    widget.queue_draw()
    return GLib.SOURCE_CONTINUE


def finish():
    window.destroy()
    loop.quit()
    return GLib.SOURCE_REMOVE


def save_widget_pixels():
    try:
        if stats["actual_connector"] != args.connector or not stats["draws"]:
            raise RuntimeError("test widget not ready on requested connector")
        width, height = area.get_width(), area.get_height()
        if width <= 0 or height <= 0 or width > 8192 or height > 8192:
            raise RuntimeError("widget dimensions out of diagnostic bounds")
        paintable = Gtk.WidgetPaintable.new(area)
        snapshot = Gtk.Snapshot.new()
        paintable.snapshot(snapshot, width, height)
        node = snapshot.to_node()
        if node is None:
            raise RuntimeError("widget snapshot empty")
        renderer = window.get_renderer()
        texture = renderer.render_texture(node, None)
        directory = tempfile.mkdtemp(prefix="aurora-widget-pixels-", dir="/var/tmp")
        path = os.path.join(directory, "widget.png")
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(texture.save_to_png_bytes().get_data())
            output.flush()
            os.fsync(output.fileno())
        stats["widget_png"] = path
        stats["widget_dimensions"] = [width, height]
        stats["renderer_type"] = renderer.__gtype__.name
        stats["widget_pixels_pass"] = True
        stats["widget_scope"] = "owned test widget only; before compositor and physical scanout"
    except Exception as error:
        stats["widget_pixels_pass"] = False
        stats["widget_error_type"] = type(error).__name__
    return GLib.SOURCE_REMOVE


area.set_draw_func(draw)
area.add_tick_callback(tick)
window.connect("close-request", lambda _window: finish())
window.fullscreen_on_monitor(matches[0])
window.present()
if args.widget_pixels:
    GLib.timeout_add_seconds(2, save_widget_pixels)
GLib.timeout_add_seconds(args.seconds, finish)
loop.run()
stats["elapsed_s"] = round(time.monotonic() - started, 3)
stats["presentation_feedback_frames"] = len(feedback)
stats["complete_timings"] = len(complete_frames)
stats["callback_pass"] = stats["ticks"] >= args.seconds * 15
stats["draw_pass"] = stats["draws"] >= args.seconds * 15
stats["presentation_pass"] = len(feedback) >= args.seconds * 10
print(json.dumps(stats, sort_keys=True))
raise SystemExit(0 if all(stats[k] for k in
                         ("callback_pass", "draw_pass", "presentation_pass")) else 1)
