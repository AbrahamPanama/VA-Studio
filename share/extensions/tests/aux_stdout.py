# coding=utf-8
"""
Helper script to check that inkex can be imported with stdout closed
"""

import sys
import importlib
from pathlib import Path

sys.path.append(str(Path(sys.path[0]).parent))

sys.stdout.close()
sys.stdout = None

import inkex
