"""Package the 'micro' plugin as a portable registry source archive or a native binary."""

import argparse
import gzip
import tarfile
from io import BytesIO
from pathlib import Path


def add(tar, name, data, executable=False):
    info = tarfile.TarInfo("micro/" + name)
    info.size = len(data)
    info.mode = 0o755 if executable else 0o644
    tar.addfile(info, BytesIO(data))


def package(output, library):
    root = Path(__file__).resolve().parent
    plugin = root / "plugin"
    output.parent.mkdir(parents=True, exist_ok=True)
    with (
        output.open("wb") as file,
        gzip.GzipFile(filename="", fileobj=file, mode="wb", mtime=0) as gz,
        tarfile.open(fileobj=gz, mode="w") as tar,
    ):
        add(tar, "LICENSE", (root.parent / "LICENSE").read_bytes())
        for name in ("plugin.toml", "README.md"):
            add(tar, name, (plugin / name).read_bytes())
        if library:
            add(tar, library.name, library.read_bytes(), executable=True)
            return
        # The plugin must build after mrsm extracts it outside this repository.
        manifest = (plugin / "Cargo.toml").read_text()
        local_crate = 'micromeerschaum = { path = ".." }'
        if manifest.count(local_crate) != 1:
            raise ValueError("Expected a repository-local receiver crate")
        add(
            tar,
            "Cargo.toml",
            manifest.replace(
                local_crate, 'micromeerschaum = { path = "receiver" }'
            ).encode(),
        )
        add(tar, "Cargo.lock", (plugin / "Cargo.lock").read_bytes())
        add(tar, "receiver/Cargo.toml", (root / "Cargo.toml").read_bytes())
        for base, prefix in ((plugin, ""), (root, "receiver/")):
            for source in sorted((base / "src").rglob("*")):
                if source.is_file() and source.suffix in (".rs", ".py"):
                    add(
                        tar,
                        prefix + source.relative_to(base).as_posix(),
                        source.read_bytes(),
                    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--library", type=Path, help="prebuilt library for a platform-specific archive"
    )
    args = parser.parse_args()
    package(args.output, args.library)
