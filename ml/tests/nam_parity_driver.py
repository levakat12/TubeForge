from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

from test_nam_reader import build_document

from nts_ml.export.packed import pack_wavenet
from nts_ml.nam import load_capture

# Every capture in `amp_learning/` and `pedal_learning/`, if either is present. These are the real
# files, and they are what the converter actually has to survive: synthetic fixtures agree on the
# easy parts. Absent corpora are not a failure -- they are third-party captures that are
# deliberately not in the repository -- so the synthetic cases below always run as well.
CORPORA = ("amp_learning", "pedal_learning")
TIERS = ("lite", "standard")


def convert_with_cpp(converter: Path, capture: Path, tier: str, destination: Path) -> bytes:
    """Run the plug-in's converter over one capture and return the bytes it wrote."""
    result = subprocess.run([str(converter), str(capture), tier, str(destination)],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{capture.name} [{tier}]: {result.stderr.strip()}")
    return destination.read_bytes()


def compare(converter: Path, capture: Path, tier: str, root: Path) -> str | None:
    """Return a failure description, or None when the two converters agree exactly."""
    parsed = load_capture(capture)
    if tier not in parsed.tiers:
        return None
    expected = pack_wavenet(parsed.spec(tier))
    actual = convert_with_cpp(converter, capture, tier, root / "cpp.bin")
    if actual == expected:
        return None
    if len(actual) != len(expected):
        return (f"{capture.name} [{tier}]: {len(actual)} bytes from the plug-in converter, "
                f"{len(expected)} from nts-nam-import")
    first = next(index for index, (left, right) in enumerate(zip(actual, expected)) if left != right)
    return f"{capture.name} [{tier}]: first difference at byte {first}"


def synthetic_cases(converter: Path, root: Path) -> list[str]:
    """Geometries the corpus does not happen to contain.

    The real captures are uniform -- 23 layers, one of two widths -- so on their own they would
    leave the layer table, the head-bias flag and the odd-width cases unexercised.
    """
    failures: list[str] = []
    shapes = (
        dict(channels=2, kernels=(2, 3), dilations=(1, 2), seed=7),
        dict(channels=3, kernels=(3, 3, 2), dilations=(1, 2, 4), seed=41),
        dict(channels=5, kernels=(6, 6, 15), dilations=(1, 3, 7), seed=99),
    )
    for index, shape in enumerate(shapes):
        capture = root / f"synthetic-{index}.nam"
        capture.write_text(json.dumps(build_document(**shape)), encoding="utf-8")
        for tier in TIERS:
            failure = compare(converter, capture, tier, root)
            if failure:
                failures.append(failure)
    return failures


def corpus_cases(converter: Path, repository: Path, root: Path) -> tuple[list[str], int]:
    import zipfile

    failures: list[str] = []
    compared = 0
    staged = root / "capture.nam"
    for folder in CORPORA:
        directory = repository / folder
        if not directory.is_dir():
            continue
        for archive in sorted(directory.glob("*.zip")):
            with zipfile.ZipFile(archive) as opened:
                for name in opened.namelist():
                    if not name.lower().endswith(".nam"):
                        continue
                    staged.write_bytes(opened.read(name))
                    for tier in TIERS:
                        try:
                            failure = compare(converter, staged, tier, root)
                        except (RuntimeError, ValueError) as error:
                            failure = f"{name} [{tier}]: {error}"
                        compared += 1
                        if failure:
                            failures.append(failure)
    return failures, compared


def main() -> int:
    """Require the plug-in's converter to write byte-identical models to the Python one.

    The plug-in carries its own `.nam` reader and NTSM v3 packer because it has to be able to
    open a capture without an interpreter on the machine. Byte identity is what stops that
    duplication from drifting, and it is a stronger gate than comparing rendered audio: if the
    bytes match, then the whole existing chain -- Python renderer checked against upstream
    `neural-amp-modeler`, C++ runtime checked against the Python renderer -- transfers to the
    in-plug-in path unchanged.
    """
    if len(sys.argv) < 2:
        print("usage: nam_parity_driver.py <nts_nam_cli>", file=sys.stderr)
        return 2
    converter = Path(sys.argv[1])
    repository = Path(__file__).resolve().parents[2]

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        failures = synthetic_cases(converter, root)
        synthetic = len(TIERS) * 3
        corpus_failures, compared = corpus_cases(converter, repository, root)
        failures.extend(corpus_failures)

    total = synthetic + compared
    if failures:
        print(f"NAM converter parity FAILED on {len(failures)} of {total} comparisons",
              file=sys.stderr)
        for line in failures[:20]:
            print("  " + line, file=sys.stderr)
        return 1
    where = "synthetic only" if compared == 0 else f"{compared} corpus, {synthetic} synthetic"
    print(f"NAM converter parity: {total} comparisons byte-identical ({where})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
