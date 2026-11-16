"""The command line of the examples, with argparse. options.ts is the TypeScript twin.

Not an example of its own: the examples import it.
"""

import argparse
import logging

from motion_master_client import AlState


class _HelpFormatter(argparse.ArgumentDefaultsHelpFormatter):
    """Show the default of an option that has one, as commander does, and no "(default: None)"."""

    def _get_help_string(self, action):
        if action.default is None or action.default is False:
            return action.help
        return super()._get_help_string(action)


class _Parser(argparse.ArgumentParser):
    """An argument parser that acts on -v/--verbose once it has parsed the command line."""

    def parse_args(self, args=None, namespace=None):
        options = super().parse_args(args, namespace)
        if options.verbose:
            # The http log of the client: one line for each request, with its times.
            http_log = logging.getLogger("motion_master_client.http")
            handler = logging.StreamHandler()
            handler.setFormatter(logging.Formatter("%(name)s %(message)s"))
            http_log.addHandler(handler)
            http_log.setLevel(logging.DEBUG)
            http_log.propagate = False
        return options


def parser(doc):
    """Return an argument parser whose --help starts with the first line of `doc`.

    Every example has -v/--verbose, which writes one line for each HTTP request with its times. The
    TypeScript examples take DEBUG=motion-master-client:http instead, the convention of Node.js.
    """
    ap = _Parser(description=doc.splitlines()[0], formatter_class=_HelpFormatter)
    ap.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="write one line for each HTTP request, with its round trip and the server times",
    )
    return ap


def integer(text):
    """Parse an integer: decimal, or with a 0x, 0o or 0b prefix, with an optional sign."""
    try:
        return int(text, 0)
    except ValueError:
        raise ValueError("Not an integer.") from None


def byte_list(text):
    """Parse a list of bytes, such as 0x10,0x27 or 16,39: integers from 0 to 255, comma-separated."""
    values = []
    for part in text.split(","):
        value = integer(part)
        if not 0 <= value <= 255:
            raise ValueError(f"{part.strip()} is not a byte, 0 to 255.")
        values.append(value)
    return values


# The AL states that a device can be moved to, by their names in lower case: init to op.
AL_STATES = {s.name.lower(): s for s in AlState if s}
