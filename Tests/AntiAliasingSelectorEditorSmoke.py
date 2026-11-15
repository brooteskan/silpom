"""Compatibility entry point for the renderer-wide VueLiveLink AA selector test."""
from pathlib import Path
script=Path(__file__).with_name('RenderingSettingsEditorSmoke.py')
exec(compile(script.read_text(),str(script),'exec'))
