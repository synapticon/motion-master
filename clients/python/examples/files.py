"""Where the examples save their files, and how they name them. files.ts is the TypeScript twin.

Not an example of its own: the examples import it.
"""

from datetime import datetime
from pathlib import Path

# The git-ignored folder next to the client that the examples save into.
FILES_DIR = Path(__file__).resolve().parent.parent / "files"


def file_stamp(date=None):
    """Return the local time as YYYYMMDD-HHMMSS, for the name of a file."""
    return f"{date or datetime.now():%Y%m%d-%H%M%S}"
