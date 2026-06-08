"""Shared pytest fixtures.

Adds the package's parent directory to sys.path so tests can run as
``pytest`` from inside ``cm5-bridge/`` without installing the package.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
