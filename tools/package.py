"""Builds ../builds/EDF6Spectator.zip from the compiled DLL and the files in package/.

Refuses to package when a source file is newer than the DLL, so a stale build never ships.
"""

import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "package")
DLL = os.path.join(ROOT, "build", "EDF6Spectator.dll")
OUTPUT = os.path.join(os.path.dirname(ROOT), "builds")
ZIP = os.path.join(OUTPUT, "EDF6Spectator.zip")

CONTENTS = [
    (DLL, "Mods/Plugins/EDF6Spectator.dll"),
    (os.path.join(SOURCE, "config.ini"), "Mods/Spectator/config.ini"),
    (os.path.join(SOURCE, "README.txt"), "README.txt"),
]


def newer_than_dll():
    cutoff = os.path.getmtime(DLL)
    stale = []
    for base, _, files in os.walk(os.path.join(ROOT, "src")):
        for name in files:
            path = os.path.join(base, name)
            if os.path.getmtime(path) > cutoff:
                stale.append(os.path.relpath(path, ROOT))
    return sorted(stale)


def main():
    missing = [src for src, _ in CONTENTS if not os.path.exists(src)]
    if missing:
        raise SystemExit("missing files for the package:\n  " + "\n  ".join(missing))
    stale = newer_than_dll()
    if stale:
        raise SystemExit("the DLL is older than the code; run build.bat first:\n  " + "\n  ".join(stale))

    os.makedirs(OUTPUT, exist_ok=True)
    with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for src, dest in CONTENTS:
            z.write(src, dest)

    print(ZIP)
    for src, dest in CONTENTS:
        print("  %-36s %8d bytes" % (dest, os.path.getsize(src)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
