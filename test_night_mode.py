"""Static regression checks for night grading of gameplay overlays."""
from pathlib import Path


ROOT = Path(__file__).resolve().parent / "mods" / "night-mode" / "patched"
FILES = (
    ROOT / "Materials" / "Shaders" / "debug-modify-color.slh",
    ROOT / "Materials" / "Shaders" / "debug-modify-color-half.slh",
)
required_guard = (
    "#if !DRAW_DEPTH_ONLY && !VIEW_MODE_OVERDRAW_HEAT && !DEBUG_UNLIT "
    "&& !HIGHLIGHT_COLOR && !HIGHLIGHT_WAVE_ANIM"
)
required_defaults = (
    "#ensuredefined HIGHLIGHT_COLOR 0",
    "#ensuredefined HIGHLIGHT_WAVE_ANIM 0",
)

for path in FILES:
    text = path.read_text(encoding="utf-8")
    assert required_guard in text, f"night grade still affects gameplay overlay: {path}"
    guard_at = text.index(required_guard)
    for default in required_defaults:
        assert default in text[:guard_at], (
            f"global shader footer reads an undefined highlight switch: {path}")
        assert text.index(default) < guard_at
    assert "#if !defined(HIGHLIGHT_" not in text, (
        f"C-style defined guard is not supported by DAVA preprocessing: {path}")
    assert "output.color.rgb" in text

print("night mode overlay guard: all passing")
