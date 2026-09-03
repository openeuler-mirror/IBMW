import argparse
import subprocess
import tempfile
from pathlib import Path


TYPEINFO = (
    "_ZTIN4ibmw7runtime4core9transport15TransportDriverE",
    "_ZTIN4ibmw7runtime4core9transport8internal22CommTraceSendOverlayV1E",
)


def _run_case(root: Path, cmake: str, verifier: str, name: str, lines: list[str], expected: str) -> None:
    artifact = root / f"{name}.so"
    artifact.touch()
    reader = root / f"readelf-{name}"
    reader.write_text(
        "#!/bin/sh\n"
        "test \"$1\" = --dyn-syms && test \"$2\" = --wide || exit 97\n"
        "printf '%s\\n' \"Symbol table '.dynsym' contains entries:\"\n"
        + "".join(f"printf '%s\\n' '{line}'\n" for line in lines)
    )
    reader.chmod(0o755)
    result = subprocess.run(
        [cmake, f"-DREADELF={reader}", f"-DARTIFACTS={artifact}", "-P", verifier],
        text=True, capture_output=True, check=False,
    )
    assert result.returncode != 0, result.stdout + result.stderr
    assert expected in result.stdout + result.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--verifier", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ibmw-rtti-verifier-") as directory:
        root = Path(directory)
        visible = [f"{index}: 0000000000000000 16 OBJECT WEAK DEFAULT 12 {symbol}"
                   for index, symbol in enumerate(TYPEINFO, 1)]
        _run_case(root, args.cmake, args.verifier, "hidden",
                  [visible[0].replace("DEFAULT", "HIDDEN"), visible[1]],
                  "not GLOBAL/WEAK DEFAULT")
        _run_case(root, args.cmake, args.verifier, "static-only", [],
                  "missing from")
        _run_case(root, args.cmake, args.verifier, "undefined",
                  [visible[0].replace("12 ", "UND ", 1), visible[1]],
                  "not GLOBAL/WEAK DEFAULT")
        _run_case(root, args.cmake, args.verifier, "ambiguous",
                  visible + [visible[0].replace("1:", "3:", 1)], "ambiguous")


if __name__ == "__main__":
    main()
