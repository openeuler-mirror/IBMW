"""Install, relocate, then repeat all functional scenarios with the installed CLI."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="run-", dir=args.artifacts))
    original, installed = root / "original-prefix", root / "relocated-prefix"
    command = ["cmake", "--install", str(args.build), "--config", args.config, "--prefix", str(original)]
    with (root / "install.log").open("w") as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        print((root / "install.log").read_text()[-8000:])
        return result.returncode
    original.rename(installed)
    metadata = subprocess.run(["ctest", "--test-dir", str(args.build), "-C", args.config,
                               "--show-only=json-v1"], capture_output=True, text=True, check=True)
    cases = [test for test in json.loads(metadata.stdout)["tests"]
             if test["name"].startswith("ibmw_diag_")
             and (test["name"].endswith("_functional_test") or test["name"].endswith("_network_test"))]
    if not cases:
        raise AssertionError("No functional scenarios found; installation cannot pass vacuously")
    peers = installed / "test-peers"
    peers.mkdir()
    replace_options = {"--cli", "--net", "--dds", "--ros2", "--grpc", "--mqtt", "--zenoh", "--iceoryx",
                       "--parameter", "--log", "--dds-types", "--roudi"}
    fixture_options = {"--server", "--typed-server", "--lifecycle-server", "--types", "--ros-types", "--qos-peer"}
    results = []
    checked_libraries = set()
    env = dict(os.environ)
    env.pop("IBMW_CLI_CONFIG", None)
    # System/ROS dependency paths are valid; a production build-tree fallback is not.
    dependency_paths = [p for p in env.get("LD_LIBRARY_PATH", "").split(os.pathsep)
                        if p and Path(p) != args.build and args.build not in Path(p).parents
                        and not any(Path(p).glob("libibmw_*.so"))]
    # Follow the documented installed runtime environment, pointing exclusively
    # at the relocated prefix for IBMW libraries and ROS-generated interfaces.
    env["LD_LIBRARY_PATH"] = os.pathsep.join([str(installed / "lib"), str(installed / "bin"), *dependency_paths])
    env["AMENT_PREFIX_PATH"] = str(installed) + os.pathsep + env.get("AMENT_PREFIX_PATH", "")

    def check_linkage(path):
        if path.name in checked_libraries:
            return
        linkage = subprocess.run(["ldd", str(path)], capture_output=True, text=True, env=env, check=True)
        (root / (path.name + ".ldd.txt")).write_text(linkage.stdout)
        # CTest stores evidence below the build directory. The relocated SDK
        # itself is allowed there; every other build-tree reference is not.
        outside_installation = linkage.stdout.replace(str(installed), "<relocated-prefix>")
        if "not found" in linkage.stdout or str(args.build) in outside_installation:
            raise AssertionError("Installed artifact has unresolved or build-tree dependencies:\n" + linkage.stdout)
        checked_libraries.add(path.name)

    # CLI providers/discovery plugins are loaded dynamically, so ldd(ibmw)
    # alone does not establish that the relocated installation is independent.
    manifest = installed / "share/ibmw/cli.yaml"
    providers = re.findall(r'library:\s*"([^"]+)"', manifest.read_text())
    expected = {f"libibmw_{name}_cli.so" for name in ["topic", "service", "module", "param", "log", "doctor"]}
    if not expected.issubset({Path(path).name for path in providers}):
        raise AssertionError("Installed CLI manifest is missing a required feature provider")
    for library in providers:
        path = (manifest.parent / library).resolve()
        if not path.is_file() or installed.resolve() not in path.parents:
            raise AssertionError(f"CLI provider escapes the relocated installation: {path}")
    for library in sorted((installed / "bin").glob("libibmw*.so")):
        check_linkage(library)

    for test in cases:
        command = list(test["command"])
        for index, option in enumerate(command[:-1]):
            if option in replace_options:
                source = Path(command[index + 1])
                matches = [p for p in installed.rglob(source.name) if p.is_file() and peers not in p.parents]
                if not matches:
                    raise AssertionError(f"Installed artifact is missing: {source.name}")
                command[index + 1] = str(sorted(matches, key=lambda p: len(str(p)))[0])
                check_linkage(Path(command[index + 1]))
            elif option in fixture_options:
                source = Path(command[index + 1])
                shutil.copy2(source, peers / source.name)
                command[index + 1] = str(peers / source.name)
            elif option == "--artifacts":
                command[index + 1] = str(root / "evidence" / test["name"])
        check_linkage(Path(command[command.index("--cli") + 1]))
        with (root / (test["name"] + ".log")).open("w") as output:
            try:
                result = subprocess.run(command, cwd=installed, env=env, stdout=output,
                                        stderr=subprocess.STDOUT, timeout=250)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
        results.append({"name": test["name"], "command": command, "code": code})
        (root / "results.json").write_text(json.dumps(results, indent=2))
        print(f"{'PASS' if code == 0 else 'FAIL'} installed {test['name']}", flush=True)
    failures = [item["name"] for item in results if item["code"]]
    print(f"Installed and relocated: {len(results) - len(failures)}/{len(results)} targets passed; evidence: {root}")
    if failures:
        print("Failures: " + ", ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
