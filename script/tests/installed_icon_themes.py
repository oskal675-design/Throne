#!/usr/bin/env python3
"""Run installed-theme chooser regressions using real Throne application objects."""
from pathlib import Path
from toolbar_theme_icons import main

if __name__ == "__main__":
    main(source=Path(__file__).with_suffix(".cpp"), cases=[
        ("old-settings", []),
        ("system", ["system"]),
        ("selected", ["selected"]),
        ("partial", ["partial"]),
        ("inherited", ["inherited"]),
        ("cycle", ["cycle"]),
        ("missing", ["missing"]),
        ("hidden", ["hidden"]),
        ("disabled", ["disabled"]),
        ("selected-2x", ["selected", "--scale-2"]),
    ])
