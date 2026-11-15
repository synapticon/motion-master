"""Run a procedure to its end for an example, and print it as the examples do.

procedures.ts is the TypeScript twin. Not an example of its own: the examples import it.
"""

import sys

from motion_master_client import format_step, step_changes


def run_to_end(client, position, name, params=None):
    """Run a procedure to its end, and print each step when its status changes.

    Then print the error and the final status, and return the last snapshot when the run
    succeeded. Ctrl-C cancels the run and ends the program with "cancelled" on stderr. A run that
    does not succeed ends the program with 1.
    """
    snapshot = None
    changes = step_changes(client.run_procedure(position, name, params or {}))
    try:
        for snapshot, changed in changes:
            for step in changed:
                print(format_step(step))
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        sys.exit(1)
    finally:
        # Closing the generator cancels a run that is still in progress.
        changes.close()

    if snapshot.get("error"):
        print(f"error: {snapshot['error']}")
    print(f"{name} {snapshot['status']}")
    if snapshot["status"] != "succeeded":
        sys.exit(1)
    return snapshot
