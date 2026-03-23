#!/usr/bin/env python3
"""Standalone test: thumbnail hover zoom + hold-right-click context menu.

Uses GestureDrag for hold-drag-release right-click menu behavior.
"""
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Gdk, GLib, GdkPixbuf
import time as _time


CSS = b"""
.ctx-menu {
    background: #2b2b2b;
    border: 1px solid #555;
    border-radius: 6px;
    padding: 4px 0;
}
.ctx-menu-item {
    padding: 8px 20px;
    color: #ddd;
    font-size: 13px;
}
.ctx-menu-item-active {
    background: #3d6ea5;
    color: #fff;
    border-radius: 4px;
}
"""


class TestWindow(Gtk.ApplicationWindow):
    def __init__(self, app):
        super().__init__(application=app, title="Hold-Right-Click Test (GestureDrag)")
        self.set_default_size(600, 400)

        css_provider = Gtk.CssProvider()
        css_provider.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css_provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        self._ctx_menu_active = False
        self._hover_anim_phase = "idle"
        self._hover_anim_timer = None
        self._hover_anim_start = 0.0
        self._hover_src_pixbuf = None

        # Context menu state
        self._menu_items = []       # [(label_widget, callback), ...]
        self._menu_active_index = -1
        self._menu_origin_x = 0     # overlay coords where menu was placed
        self._menu_origin_y = 0
        self._drag_start_x = 0      # widget-local coords of initial press
        self._drag_start_y = 0

        # --- Overlay root ---
        self._overlay = Gtk.Overlay()
        self.set_child(self._overlay)

        # --- Context menu (Fixed overlay, hidden initially) ---
        self._ctx_fixed = Gtk.Fixed()
        self._ctx_fixed.set_visible(False)
        self._ctx_fixed.set_can_target(False)
        self._overlay.add_overlay(self._ctx_fixed)

        self._ctx_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self._ctx_box.add_css_class("ctx-menu")
        self._ctx_fixed.put(self._ctx_box, 0, 0)

        # --- Hover popup window ---
        self._hover_popup = Gtk.Window()
        self._hover_popup.set_decorated(False)
        self._hover_popup.set_default_size(240, 270)
        self._hover_popup.set_resizable(False)
        self._hover_popup.set_transient_for(self)
        self._hover_popup.set_visible(False)

        self._hover_popup_image = Gtk.Picture()
        self._hover_popup_image.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._hover_popup_image.set_size_request(200, 200)
        popup_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        popup_box.append(self._hover_popup_image)
        self._hover_popup_label = Gtk.Label(label="")
        popup_box.append(self._hover_popup_label)
        self._hover_popup.set_child(popup_box)

        # --- Main content ---
        main_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        main_box.set_margin_top(20)
        main_box.set_margin_start(20)
        self._overlay.set_child(main_box)

        info = Gtk.Label()
        info.set_markup(
            "<b>HOLD</b> right-click → menu appears\n"
            "Drag to item → highlights\n"
            "Release → activates")
        main_box.append(info)

        self._status = Gtk.Label(label="Status: idle")
        main_box.append(self._status)

        grid = Gtk.FlowBox()
        grid.set_max_children_per_line(6)
        grid.set_selection_mode(Gtk.SelectionMode.NONE)
        main_box.append(grid)

        colors = ["#e63946", "#457b9d", "#2a9d8f", "#e9c46a", "#f4a261", "#264653"]
        for i, color in enumerate(colors):
            frame = self._make_thumbnail(f"Item {i+1}", color)
            grid.append(frame)

    def _make_thumbnail(self, name, color):
        pixbuf = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, False, 8, 48, 48)
        r, g, b = int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16)
        pixbuf.fill((r << 24) | (g << 16) | (b << 8) | 0xFF)
        big_pixbuf = pixbuf.scale_simple(200, 200, GdkPixbuf.InterpType.NEAREST)

        texture = Gdk.Texture.new_for_pixbuf(pixbuf)
        img = Gtk.Picture.new_for_paintable(texture)
        img.set_size_request(48, 48)

        frame = Gtk.Frame()
        frame.set_child(img)
        frame.set_size_request(52, 52)

        # Hover for zoom
        hover = Gtk.EventControllerMotion()
        hover.connect("enter", self._on_thumb_enter, name, big_pixbuf)
        hover.connect("leave", self._on_thumb_leave)
        frame.add_controller(hover)

        # Right-click drag: hold → drag → release
        drag = Gtk.GestureDrag()
        drag.set_button(3)  # right mouse button
        drag.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        drag.connect("drag-begin", self._on_right_drag_begin, name)
        drag.connect("drag-update", self._on_right_drag_update)
        drag.connect("drag-end", self._on_right_drag_end)
        frame.add_controller(drag)

        return frame

    # ---- Context menu ----

    def _show_ctx_menu(self, items, overlay_x, overlay_y):
        """Show the context menu at overlay coords."""
        for lbl, _ in self._menu_items:
            self._ctx_box.remove(lbl)
        self._menu_items.clear()
        self._menu_active_index = -1

        for text, cb in items:
            lbl = Gtk.Label(label=text)
            lbl.set_halign(Gtk.Align.START)
            lbl.add_css_class("ctx-menu-item")
            self._ctx_box.append(lbl)
            self._menu_items.append((lbl, cb))

        self._ctx_fixed.move(self._ctx_box, overlay_x, overlay_y)
        self._menu_origin_x = overlay_x
        self._menu_origin_y = overlay_y
        self._ctx_fixed.set_visible(True)
        self._ctx_menu_active = True

    def _dismiss_ctx_menu(self):
        self._ctx_fixed.set_visible(False)
        self._ctx_menu_active = False
        self._menu_active_index = -1
        for lbl, _ in self._menu_items:
            lbl.remove_css_class("ctx-menu-item-active")
        # Resume zoom-out
        self._hover_anim_phase = "zoom_out"
        self._hover_anim_start = _time.monotonic()
        if self._hover_anim_timer is None:
            self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)

    def _update_menu_highlight_rel(self, rel_x, rel_y):
        """Highlight menu item under cursor. rel_x/rel_y are offset from menu origin."""
        n = len(self._menu_items)
        if n == 0:
            return

        # Use total menu box height divided evenly
        total_h = self._ctx_box.get_height()
        if total_h <= 0:
            total_h = n * 34  # fallback
        item_h = total_h / n

        new_idx = -1
        if rel_y >= 0 and rel_y < total_h:
            new_idx = int(rel_y / item_h)
            new_idx = max(0, min(new_idx, n - 1))

        if new_idx != self._menu_active_index:
            if 0 <= self._menu_active_index < len(self._menu_items):
                self._menu_items[self._menu_active_index][0].remove_css_class("ctx-menu-item-active")
            self._menu_active_index = new_idx
            if 0 <= new_idx < len(self._menu_items):
                self._menu_items[new_idx][0].add_css_class("ctx-menu-item-active")

        scale = self.get_scale_factor()
        self._status.set_text(
            f"rel=({rel_x:.0f},{rel_y:.0f}) h={total_h} item_h={item_h:.0f} idx={new_idx} scale={scale}")

    def _activate_menu_item(self):
        idx = self._menu_active_index
        if 0 <= idx < len(self._menu_items):
            _, cb = self._menu_items[idx]
            self._dismiss_ctx_menu()
            cb()
        else:
            self._dismiss_ctx_menu()

    # ---- Right-click drag handlers ----

    def _on_right_drag_begin(self, gesture, start_x, start_y, name):
        """Right button pressed — show context menu."""
        self._status.set_text(f"Status: DRAG-BEGIN {name} at ({start_x:.0f},{start_y:.0f})")

        # Remember start position in widget-local coords
        self._drag_start_x = start_x
        self._drag_start_y = start_y

        # Freeze animation (keep popup visible)
        if self._hover_anim_timer is not None:
            GLib.source_remove(self._hover_anim_timer)
            self._hover_anim_timer = None
        self._hover_anim_phase = "menu_frozen"

        # Convert widget-local start coords to overlay coords
        widget = gesture.get_widget()
        try:
            ox, oy = widget.translate_coordinates(self._overlay, start_x, start_y)
        except Exception:
            ox, oy = start_x, start_y

        self._show_ctx_menu([
            (f"  Look at {name}  ",
             lambda: self._status.set_text(f"ACTION: Look at {name}")),
            (f"  Attack {name}  ",
             lambda: self._status.set_text(f"ACTION: Attack {name}")),
            (f"  Regenerate Image  ",
             lambda: self._status.set_text(f"ACTION: Regen {name}")),
        ], ox, oy)

    def _on_right_drag_update(self, gesture, offset_x, offset_y):
        """Mouse moved while right button held — update highlight."""
        # offset_x/y are relative to drag start, menu is placed at drag start
        # so offset IS the relative position within the menu
        self._update_menu_highlight_rel(offset_x, offset_y)

    def _on_right_drag_end(self, gesture, offset_x, offset_y):
        """Right button released — activate highlighted item."""
        self._status.set_text(f"Status: DRAG-END offset=({offset_x:.0f},{offset_y:.0f})")
        self._activate_menu_item()

    # ---- Thumbnail hover ----

    def _on_thumb_enter(self, controller, x, y, name, pixbuf):
        if self._ctx_menu_active:
            return
        self._status.set_text(f"Status: hover-enter {name}")
        self._hover_src_pixbuf = pixbuf
        self._hover_popup_label.set_text(name)
        texture = Gdk.Texture.new_for_pixbuf(pixbuf)
        self._hover_popup_image.set_paintable(texture)
        self._hover_popup.set_visible(True)
        self._hover_popup.set_opacity(0.0)
        self._hover_anim_phase = "zoom_in"
        self._hover_anim_start = _time.monotonic()
        if self._hover_anim_timer is None:
            self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)

    def _on_thumb_leave(self, controller, *args):
        if self._ctx_menu_active or self._hover_anim_phase == "menu_frozen":
            return
        self._status.set_text("Status: hover-leave → zoom-out")
        if self._hover_anim_phase in ("zoom_in", "float"):
            self._hover_anim_phase = "zoom_out"
            self._hover_anim_start = _time.monotonic()
            if self._hover_anim_timer is None:
                self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)
        else:
            self._hover_popup.set_visible(False)
            if self._hover_anim_timer is not None:
                GLib.source_remove(self._hover_anim_timer)
                self._hover_anim_timer = None

    # ---- Animation ----

    def _hover_anim_tick(self):
        now = _time.monotonic()
        t = now - self._hover_anim_start

        if self._hover_anim_phase == "zoom_in":
            progress = min(t / 0.25, 1.0)
            ease = 1.0 - (1.0 - progress) ** 3
            self._hover_popup.set_opacity(ease)
            sz = int(24 + (200 - 24) * ease)
            self._hover_popup_image.set_size_request(sz, sz)
            if progress >= 1.0:
                self._hover_anim_phase = "float"
                self._hover_anim_start = now
            return True

        elif self._hover_anim_phase == "float":
            # No ripple in test (numpy may not be available)
            return True

        elif self._hover_anim_phase == "zoom_out":
            progress = min(t / 0.18, 1.0)
            ease = progress ** 3
            self._hover_popup.set_opacity(1.0 - ease)
            sz = int(200 + (24 - 200) * ease)
            self._hover_popup_image.set_size_request(max(sz, 1), max(sz, 1))
            if progress >= 1.0:
                self._hover_popup.set_visible(False)
                self._hover_anim_phase = "idle"
                self._hover_anim_timer = None
                return False
            return True

        elif self._hover_anim_phase == "menu_frozen":
            return True

        self._hover_anim_timer = None
        return False


def on_activate(app):
    win = TestWindow(app)
    win.present()

app = Gtk.Application(application_id="com.test.hovermenu")
app.connect("activate", on_activate)
app.run()
