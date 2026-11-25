"""Build the Arduino Library Manager ZIP from the same sources as the ESP-IDF component."""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def package(output):
    port = Path(__file__).resolve().parent
    root = port.parents[1]
    core = root / "core"
    entries = {"LICENSE": root / "LICENSE", "library.properties": port / "library.properties",
               "src/MicroMeerschaum.h": port / "MicroMeerschaum.h"}
    for path in (port / "examples").rglob("*.ino"):
        entries[path.relative_to(port).as_posix()] = path
    for path in (core / "include").rglob("*"):
        if path.is_file() and path.suffix in (".h", ".hpp"):
            entries["src/" + path.relative_to(core / "include").as_posix()] = path
    for directory in ("src", "storage", "shared", "posix"):
        for path in (core / directory).glob("*"):
            if path.is_file() and path.suffix in (".c", ".h"):
                entries["src/" + path.relative_to(core).as_posix()] = path
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w") as archive:
        for name, source in sorted(entries.items()):
            info = ZipInfo("MicroMeerschaum/" + name)
            info.external_attr = 0o100644 << 16
            archive.writestr(info, source.read_bytes(), compress_type=ZIP_DEFLATED)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    package(parser.parse_args().output)
