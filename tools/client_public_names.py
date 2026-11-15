"""Print the public names of the Python client as JSON, for tools/check-client-parity.mjs.

The names come from the syntax tree of motion_master_client.py, so this needs no package that the
client imports. The output is {"module": [...], "classes": {"ClassName": [...]}}.
"""

import ast
import json
import re
import sys
from pathlib import Path

CLIENT = Path(__file__).resolve().parent.parent / "clients" / "python" / "motion_master_client.py"
CONSTANT = re.compile(r"^[A-Z][A-Z0-9_]*$")


def public(name):
    return not name.startswith("_")


def class_members(node):
    """Return the public methods and properties of a class, without the dunder methods."""
    return sorted(
        item.name
        for item in node.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef)) and public(item.name)
    )


def main():
    tree = ast.parse(CLIENT.read_text(encoding="utf-8"), filename=str(CLIENT))
    module = set()
    classes = {}
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and public(node.name):
            module.add(node.name)
            classes[node.name] = class_members(node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and public(node.name):
            module.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and CONSTANT.match(target.id):
                    module.add(target.id)
        elif isinstance(node, ast.ImportFrom) and node.module == "motion_master_objects":
            # A name imported under a private alias is the client's own building block.
            for alias in node.names:
                name = alias.asname or alias.name
                if public(name):
                    module.add(name)
    json.dump({"module": sorted(module), "classes": classes}, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
