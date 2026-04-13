#!/usr/bin/env python3
import json
import os
import stat
import sys
import xml.etree.ElementTree as ET


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    sys.exit(1)


def check(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def main() -> int:
    if len(sys.argv) != 2:
        fail("usage: validate_scaffold.py <project-root>")

    root = os.path.abspath(sys.argv[1])

    metadata_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "metadata.json")
    extension_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "extension.js")
    prefs_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "prefs.js")
    constants_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "constants.js")
    control_client_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "controlClient.js")
    pause_policy_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "pausePolicy.js")
    mpris_watcher_path = os.path.join(root, "extension", "milkdrop@mauriciobc.github.io", "mprisWatcher.js")
    schema_path = os.path.join(root, "data", "org.gnome.shell.extensions.milkdrop.gschema.xml")

    for path in [metadata_path, extension_path, prefs_path, constants_path,
                 control_client_path, pause_policy_path, mpris_watcher_path, schema_path]:
        check(os.path.exists(path), f"required file is missing: {path}")

    metadata = json.loads(read_text(metadata_path))
    check(metadata.get("uuid") == "milkdrop@mauriciobc.github.io", "metadata uuid mismatch")
    check(metadata.get("settings-schema") == "org.gnome.shell.extensions.milkdrop", "metadata settings-schema mismatch")

    shell_versions = metadata.get("shell-version", [])
    for version in ["47", "48", "49", "50"]:
        check(version in shell_versions, f"metadata missing shell-version {version}")

    schema_tree = ET.parse(schema_path)
    schema_root = schema_tree.getroot()

    schema_elem = schema_root.find("schema")
    check(schema_elem is not None, "schema element missing")
    check(schema_elem.attrib.get("id") == "org.gnome.shell.extensions.milkdrop", "schema id mismatch")

    key_names = {key.attrib.get("name"): key.attrib.get("type") for key in schema_elem.findall("key")}
    expected = {
        "enabled": "b",
        "monitor": "i",
        "opacity": "d",
        "preset-dir": "s",
        "shuffle": "b",
        "preset-rotation-interval": "i",
        "overlay": "b",
        "pause-on-fullscreen": "b",
        "pause-on-maximized": "b",
        "media-aware": "b",
        "all-monitors": "b",
        "fps": "i",
        "last-preset": "s",
        "was-paused": "b",
    }
    check(key_names == expected, f"schema keys mismatch: got {key_names}")

    opacity_key = None
    for key in schema_elem.findall("key"):
        if key.attrib.get("name") == "opacity":
            opacity_key = key
            break
    check(opacity_key is not None, "opacity key missing")
    range_elem = opacity_key.find("range")
    check(range_elem is not None, "opacity key range missing")
    check(range_elem.attrib.get("min") == "0.0", "opacity range min mismatch")
    check(range_elem.attrib.get("max") == "1.0", "opacity range max mismatch")

    extension_js = read_text(extension_path)
    check("from './constants.js'" in extension_js, "extension.js must import ./constants.js")
    check("from './controlClient.js'" in extension_js, "extension.js must import ./controlClient.js")
    check("from './pausePolicy.js'" in extension_js, "extension.js must import ./pausePolicy.js")
    check("from './mprisWatcher.js'" in extension_js, "extension.js must import ./mprisWatcher.js")
    check("export default class MilkdropExtension extends Extension" in extension_js, "extension class declaration missing")
    check("enable()" in extension_js, "enable() missing in extension")
    check("disable()" in extension_js, "disable() missing in extension")
    check("_spawnProcess(" in extension_js, "_spawnProcess() missing in extension")
    check("monitors-changed" in extension_js, "monitors-changed handling missing in extension")

    constants_js = read_text(constants_path)
    check("FADE_DURATION_MS" in constants_js, "constants.js must export FADE_DURATION_MS")

    control_client_js = read_text(control_client_path)
    check("queryMilkdropStatus" in control_client_js, "controlClient.js must export queryMilkdropStatus")
    check("queryMilkdropSaveState" in control_client_js, "controlClient.js must export queryMilkdropSaveState")
    check("sendMilkdropRestoreState" in control_client_js, "controlClient.js must export sendMilkdropRestoreState")
    check("queryAllMilkdropSaveState" in control_client_js, "controlClient.js must export queryAllMilkdropSaveState")

    prefs_js = read_text(prefs_path)
    check("ExtensionPreferences" in prefs_js, "prefs does not use ExtensionPreferences")
    check("fillPreferencesWindow(window)" in prefs_js, "fillPreferencesWindow missing")

    script_paths = [
        os.path.join(root, "tools", "install.sh"),
        os.path.join(root, "tools", "uninstall.sh"),
        os.path.join(root, "tools", "reload.sh"),
        os.path.join(root, "tools", "nested_devkit.sh"),
    ]
    for script_path in script_paths:
        check(os.path.exists(script_path), f"missing script: {script_path}")
        mode = os.stat(script_path).st_mode
        check(mode & stat.S_IXUSR, f"script is not executable by owner: {script_path}")
        first_line = read_text(script_path).splitlines()[0]
        check(first_line.startswith("#!/usr/bin/env bash"), f"script shebang mismatch in {script_path}")

    print("PASS: scaffold validation checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())