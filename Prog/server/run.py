#!/usr/bin/env python3
"""Entry point: python run.py [--simulate] [--serial-port /dev/tty...]"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from standserver.main import main  # noqa: E402

if __name__ == "__main__":
    main()
