"""Hold-right-click context menu using GestureDrag.

Usage:
    menu = HoldMenu(parent_overlay)
    # In your widget setup:
    drag = menu.create_gesture(widget)
    drag.connect("drag-begin", on_begin)
    # In on_begin:
    menu.show(items=[(label, callback), ...], gesture=drag, start_x=x, start_y=y)
    # drag-update and drag-end are handled automatically.
"""
import gi
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, GLib


CTX_MENU_CSS = """
.hold-ctx-menu {
    background: #2b2b2b;
    border: 1px solid #555;
    border-radius: 6px;
    padding: 4px 0;
}
.hold-ctx-item {
    padding: 4px 14px;
    color: #ddd;
    font-size: 10px;
}
.hold-ctx-item-active {
    background: #3d6ea5;
    color: #fff;
    border-radius: 4px;
}
"""

_css_loaded = False


def _ensure_css():
    global _css_loaded
    if _css_loaded:
        return
    provider = Gtk.CssProvider()
    provider.load_from_data(CTX_MENU_CSS.encode())
    Gtk.StyleContext.add_provider_for_display(
        Gdk.Display.get_default(), provider,
        Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    _css_loaded = True


class HoldMenu:
    """Hold-right-click context menu rendered as a Fixed overlay."""

    def __init__(self, overlay: Gtk.Overlay):
        _ensure_css()
        self._overlay = overlay
        self._items: list[tuple[Gtk.Label, callable]] = []
        self._active_index = -1
        self._drag_start_x = 0.0
        self._drag_start_y = 0.0
        self._origin_x = 0.0
        self._origin_y = 0.0

        # Callbacks
        self.on_menu_open: callable = None   # called when menu opens
        self.on_menu_close: callable = None  # called when menu closes

        # Fixed container as overlay child
        self._fixed = Gtk.Fixed()
        self._fixed.set_visible(False)
        self._fixed.set_can_target(False)
        overlay.add_overlay(self._fixed)

        self._box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self._box.add_css_class("hold-ctx-menu")
        self._fixed.put(self._box, 0, 0)

    @property
    def is_active(self) -> bool:
        return self._fixed.get_visible()

    def create_gesture(self, widget: Gtk.Widget) -> Gtk.GestureDrag:
        """Create and attach a right-click GestureDrag to the widget."""
        drag = Gtk.GestureDrag()
        drag.set_button(3)
        drag.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        drag.set_propagation_limit(Gtk.PropagationLimit.NONE)
        drag.connect("drag-update", self._on_drag_update)
        drag.connect("drag-end", self._on_drag_end)
        widget.add_controller(drag)
        return drag

    def show(self, items: list[tuple[str, callable]],
             gesture: Gtk.GestureDrag, start_x: float, start_y: float):
        """Show the menu. items = [(label_text, callback), ...]."""
        # Clear old
        for lbl, _ in self._items:
            self._box.remove(lbl)
        self._items.clear()
        self._active_index = -1

        # Build menu items
        for text, cb in items:
            lbl = Gtk.Label(label=text)
            lbl.set_halign(Gtk.Align.START)
            lbl.add_css_class("hold-ctx-item")
            self._box.append(lbl)
            self._items.append((lbl, cb))

        # Position in overlay coordinates
        widget = gesture.get_widget()
        try:
            ox, oy = widget.translate_coordinates(self._overlay, start_x, start_y)
        except Exception:
            ox, oy = start_x, start_y

        # Offset upward so menu doesn't clip at window bottom
        menu_h = len(items) * 34  # estimated item height
        overlay_h = self._overlay.get_height()
        if overlay_h > 0 and oy + menu_h > overlay_h:
            oy = max(0, overlay_h - menu_h - 4)
        self._fixed.move(self._box, ox, oy)
        self._origin_x = ox
        self._origin_y = oy
        self._drag_start_x = start_x
        self._drag_start_y = start_y
        self._fixed.set_visible(True)

        if self.on_menu_open:
            self.on_menu_open()

    def _on_drag_update(self, gesture, offset_x, offset_y):
        if not self._fixed.get_visible():
            return
        n = len(self._items)
        if n == 0:
            return

        total_h = self._box.get_height()
        if total_h <= 0:
            total_h = n * 34
        item_h = total_h / n

        new_idx = -1
        if 0 <= offset_y < total_h:
            new_idx = min(int(offset_y / item_h), n - 1)

        if new_idx != self._active_index:
            if 0 <= self._active_index < len(self._items):
                self._items[self._active_index][0].remove_css_class("hold-ctx-item-active")
            self._active_index = new_idx
            if 0 <= new_idx < len(self._items):
                self._items[new_idx][0].add_css_class("hold-ctx-item-active")

    def _on_drag_end(self, gesture, offset_x, offset_y):
        if not self._fixed.get_visible():
            return
        idx = self._active_index
        self._dismiss()
        if 0 <= idx < len(self._items):
            _, cb = self._items[idx]
            cb()

    def _dismiss(self):
        self._fixed.set_visible(False)
        self._active_index = -1
        for lbl, _ in self._items:
            lbl.remove_css_class("hold-ctx-item-active")
        if self.on_menu_close:
            self.on_menu_close()
