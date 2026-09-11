import json
import hashlib
import os
import signal
import shutil
import subprocess
import sys
from types import SimpleNamespace
from pathlib import Path
from typing import Any

import pytest

from mw_perf_collect import rollback_evaluation as rollback
from mw_perf_collect import core
from mw_perf_collect.campaign import validate_companion_object


def test_parse_cmake_cache_preserves_type_and_value():
    parsed = rollback.parse_cmake_cache("# comment\nA:BOOL=ON\nB:PATH=/x=y\nBROKEN\n")
    assert parsed == {"A": {"type": "BOOL", "value": "ON"},
                      "B": {"type": "PATH", "value": "/x=y"}}


def test_selected_options_includes_all_registered_product_controls():
    cache = {"IBMW_BUILD_TESTS": {"type": "BOOL", "value": "ON"},
             "IBMW_ENABLE_COMM_TRACE": {"type": "BOOL", "value": "ON"},
             "IBMW_UNRECOGNIZED": {"type": "STRING", "value": "value"},
             "IBMW_INTERNAL": {"type": "INTERNAL", "value": "ignored"}}
    assert rollback.selected_options(cache) == {
        "IBMW_BUILD_TESTS": "ON", "IBMW_ENABLE_COMM_TRACE": "ON",
        "IBMW_UNRECOGNIZED": "value"}


def test_option_delta_reports_added_removed_and_changed():
    assert rollback.option_delta({"A": "ON", "B": "OFF"}, {"A": "OFF", "C": "ON"}) == {
        "A": {"anchor": "ON", "fresh": "OFF"},
        "B": {"anchor": "OFF", "fresh": None},
        "C": {"anchor": None, "fresh": "ON"},
    }


def test_toolchain_options_include_compiler_binutils_and_target_controls():
    names = ("CMAKE_C_COMPILER_AR", "CMAKE_CXX_COMPILER_AR",
             "CMAKE_C_COMPILER_RANLIB", "CMAKE_CXX_COMPILER_RANLIB",
             "CMAKE_C_COMPILER_TARGET", "CMAKE_CXX_COMPILER_TARGET",
             "CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN", "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN")
    cache = {name: {"type": "FILEPATH", "value": f"/{name}"} for name in names}
    assert set(rollback.toolchain_options(cache)) == set(names)


def test_compare_controls_rejects_mismatch():
    with pytest.raises(RuntimeError, match="controls differ"):
        rollback._compare_controls({"CMAKE_AR": "/one/ar"}, {"CMAKE_AR": "/two/ar"}, "toolchain")


def test_source_identity_drift_is_rejected():
    with pytest.raises(RuntimeError, match="source identity changed"):
        rollback._require_same_source_identity({"sha256": "before"}, {"sha256": "after"})


def _dependency_fixture(tmp_path):
    base = (tmp_path / "anchor" / "_deps").resolve(); base.mkdir(parents=True)
    for name in ("backward", "googletest"):
        source = base / f"{name}-src"; source.mkdir()
        (source / "CMakeLists.txt").write_text(f"project({name})\n")
        (source / f"{name}.cc").write_text(f"// {name}\n")
    cache = {"FETCHCONTENT_BASE_DIR": {"value": str(base)}}
    return base, cache


def _staged_dependency_control(tmp_path):
    base, cache = _dependency_fixture(tmp_path)
    _base, dependencies = rollback._anchor_dependency_sources(cache)
    work = (tmp_path / "work").resolve(); work.mkdir()
    source_cache, fresh_base = rollback._stage_dependency_sources(work, dependencies)
    aggregate = {}
    for checkpoint in rollback._DEPENDENCY_CHECKPOINTS:
        field = "anchor_source" if checkpoint.startswith("anchor_") else "staged_source"
        aggregate[checkpoint] = rollback._record_dependency_checkpoint(
            dependencies, checkpoint, field)
    return {"provider": "anchor_fetchcontent_source_cache", "reuse_scope": "source_only",
            "cache_verified": True, "anchor_base_dir": str(base),
            "fresh_base_dir": str(fresh_base), "staged_source_cache": str(source_cache),
            "dependencies": dependencies, "aggregate_identity": aggregate}


def test_configure_argv_uses_fresh_disconnected_cache_and_all_overrides(tmp_path):
    control = _staged_dependency_control(tmp_path)
    source = (tmp_path / "product-source").resolve(); source.mkdir()
    build = (tmp_path / "fresh" / "build").resolve()
    argv = rollback._configure_argv(
        "/usr/bin/cmake", source, build, {"CMAKE_GENERATOR": {"value": "Ninja"}},
        {"CMAKE_BUILD_TYPE": "Release"}, {"IBMW_ENABLE_COMM_TRACE": "ON"},
        Path(control["fresh_base_dir"]), control["dependencies"])
    assert f'-DFETCHCONTENT_BASE_DIR={control["fresh_base_dir"]}' in argv
    assert "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" in argv
    overrides = [item for item in argv if item.startswith("-DFETCHCONTENT_SOURCE_DIR_")]
    assert overrides == [
        f'-DFETCHCONTENT_SOURCE_DIR_BACKWARD={control["dependencies"][0]["staged_source"]}',
        f'-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={control["dependencies"][1]["staged_source"]}']
    assert all("-build" not in item and "-subbuild" not in item for item in argv)
    assert sorted(path.name for path in Path(control["staged_source_cache"]).iterdir()) == [
        "backward-src", "googletest-src"]
    assert "-DIBMW_ENABLE_COMM_TRACE=OFF" in argv
    assert "-DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=OFF" in argv


def test_dependency_override_preserves_cmake_fetchcontent_name_spelling():
    assert rollback._dependency_override("yaml-cpp") == "FETCHCONTENT_SOURCE_DIR_YAML-CPP"
    assert rollback._dependency_override("tbb") == "FETCHCONTENT_SOURCE_DIR_TBB"


@pytest.mark.parametrize("condition", ["missing_base", "source_symlink", "nested_symlink"])
def test_dependency_inventory_rejects_missing_or_symlinked_authority(tmp_path, condition):
    base, cache = _dependency_fixture(tmp_path)
    if condition == "missing_base":
        cache = {}
    elif condition == "source_symlink":
        source = base / "backward-src"; shutil.rmtree(source)
        source.symlink_to(base / "googletest-src", target_is_directory=True)
    else:
        (base / "backward-src" / "escape").symlink_to(base / "googletest-src")
    with pytest.raises(RuntimeError, match="missing or invalid|symlink"):
        rollback._anchor_dependency_sources(cache)


def test_dependency_fd_traversal_rejects_entry_replaced_after_lstat(tmp_path, monkeypatch):
    base, _cache = _dependency_fixture(tmp_path)
    source = base / "backward-src"
    victim = source / "backward.cc"
    outside = tmp_path / "outside.cc"; outside.write_text("outside\n")
    real_open = rollback.os.open
    replaced = False

    def racing_open(path, flags, mode=0o777, *, dir_fd=None):
        nonlocal replaced
        if path == "backward.cc" and dir_fd is not None and not replaced:
            replaced = True
            victim.rename(source / "original.cc")
            victim.symlink_to(outside)
        return real_open(path, flags, mode, dir_fd=dir_fd)

    monkeypatch.setattr(rollback.os, "open", racing_open)
    with pytest.raises(RuntimeError, match="symlink|changed|unavailable"):
        rollback._external_source_identity(source)


def test_dependency_copy_rejects_growth_before_writing_excess_bytes(tmp_path, monkeypatch):
    source = (tmp_path / "source").resolve(); source.mkdir()
    (source / "data.bin").write_bytes(b"declared")
    destination = (tmp_path / "destination").resolve()
    real_read = rollback.os.read
    injected = False

    def growing_read(fd, count):
        nonlocal injected
        chunk = real_read(fd, count)
        if not chunk and not injected:
            injected = True
            return b"growth"
        return chunk

    monkeypatch.setattr(rollback.os, "read", growing_read)
    with pytest.raises(RuntimeError, match="grew beyond declared byte budget"):
        rollback._dependency_tree(source, destination, max_bytes=1024)
    assert (destination / "data.bin").read_bytes() == b"declared"


@pytest.mark.parametrize("operation", ["discovery", "checkpoint"])
def test_dependency_aggregate_exhaustion_stops_second_tree_scan(
        tmp_path, monkeypatch, operation):
    base, cache = _dependency_fixture(tmp_path)
    dependencies = None
    if operation == "checkpoint":
        _base, dependencies = rollback._anchor_dependency_sources(cache)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_FILES", 2)
    second_tree_stats = 0
    first_tree_complete = False
    first_file_stats = 0
    real_stat = rollback.os.stat

    def instrumented_stat(path, *args, **kwargs):
        nonlocal first_file_stats, first_tree_complete, second_tree_stats
        if first_tree_complete:
            second_tree_stats += 1
        if path == "backward.cc":
            first_file_stats += 1
            first_tree_complete = first_file_stats == 2
        return real_stat(path, *args, **kwargs)

    monkeypatch.setattr(rollback.os, "stat", instrumented_stat)
    with pytest.raises(RuntimeError, match="file count limit"):
        if operation == "discovery":
            rollback._anchor_dependency_sources(cache)
        else:
            assert dependencies is not None
            rollback._record_dependency_checkpoint(
                dependencies, "anchor_after_staging", "anchor_source")
    assert second_tree_stats == 0


@pytest.mark.parametrize("omission", ["base", "disconnected", "backward_override"])
def test_dependency_cache_requires_exact_base_disconnected_and_every_override(tmp_path, omission):
    control = _staged_dependency_control(tmp_path)
    cache = {"FETCHCONTENT_BASE_DIR": {"value": control["fresh_base_dir"]},
             "FETCHCONTENT_FULLY_DISCONNECTED": {"value": "ON"}}
    for dependency in control["dependencies"]:
        cache[dependency["override_variable"]] = {"value": dependency["staged_source"]}
    key = {"base": "FETCHCONTENT_BASE_DIR", "disconnected": "FETCHCONTENT_FULLY_DISCONNECTED",
           "backward_override": "FETCHCONTENT_SOURCE_DIR_BACKWARD"}[omission]
    broken = dict(cache); broken.pop(key)
    with pytest.raises(RuntimeError, match="FETCHCONTENT"):
        rollback._require_dependency_cache(
            broken, Path(control["fresh_base_dir"]), Path(control["staged_source_cache"]),
            control["dependencies"])


def test_dependency_cache_rejects_generated_local_source_binding_mismatch(tmp_path):
    control = _staged_dependency_control(tmp_path)
    dependency = control["dependencies"][0]
    dependency["cache_bindings"] = [{"variable": "backward_SOURCE_DIR", "relative": "."}]
    cache = {"FETCHCONTENT_BASE_DIR": {"value": control["fresh_base_dir"]},
             "FETCHCONTENT_FULLY_DISCONNECTED": {"value": "ON"},
             "backward_SOURCE_DIR": {"value": dependency["anchor_source"]}}
    for item in control["dependencies"]:
        cache[item["override_variable"]] = {"value": item["staged_source"]}
    with pytest.raises(RuntimeError, match="backward_SOURCE_DIR cache mismatch"):
        rollback._require_dependency_cache(
            cache, Path(control["fresh_base_dir"]), Path(control["staged_source_cache"]),
            control["dependencies"])


def test_dependency_cache_rejects_forged_alternate_staged_copy(tmp_path):
    control = _staged_dependency_control(tmp_path)
    canonical_cache = Path(control["staged_source_cache"])
    alternate_cache = (tmp_path / "alternate-cache").resolve()
    shutil.copytree(canonical_cache, alternate_cache)
    cache = {
        "FETCHCONTENT_BASE_DIR": {"value": control["fresh_base_dir"]},
        "FETCHCONTENT_FULLY_DISCONNECTED": {"value": "ON"},
    }
    for dependency in control["dependencies"]:
        forged = alternate_cache / (dependency["name"] + "-src")
        dependency["staged_source"] = str(forged)
        cache[dependency["override_variable"]] = {"value": str(forged)}
    with pytest.raises(RuntimeError, match="not canonical"):
        rollback._require_dependency_cache(
            cache, Path(control["fresh_base_dir"]), canonical_cache,
            control["dependencies"])


def test_dependency_source_mutation_is_rejected_by_aggregate_control(tmp_path):
    control = _staged_dependency_control(tmp_path)
    Path(control["dependencies"][0]["staged_source"], "backward.cc").write_text("mutated\n")
    control["dependencies"][0]["identity"]["staged_after_build"] = \
        rollback._external_source_identity(Path(control["dependencies"][0]["staged_source"]))
    control["aggregate_identity"]["staged_after_build"] = rollback._aggregate_dependency_identity(
        control["dependencies"], "staged_after_build")
    with pytest.raises(ValueError, match="identity changed"):
        rollback._portable_dependency_source(control)


@pytest.mark.parametrize("kind", ["file", "directory"])
def test_dependency_permission_mutation_is_rejected(tmp_path, kind):
    control = _staged_dependency_control(tmp_path)
    source = Path(control["dependencies"][0]["staged_source"])
    target = source / ("backward.cc" if kind == "file" else ".")
    target.chmod(0o600 if kind == "file" else 0o700)
    changed = rollback._external_source_identity(source)
    assert changed["tree_sha256"] != control["dependencies"][0]["identity"][
        "anchor_before_staging"]["tree_sha256"]
    control["dependencies"][0]["identity"]["staged_after_build"] = changed
    control["aggregate_identity"]["staged_after_build"] = rollback._aggregate_dependency_identity(
        control["dependencies"], "staged_after_build")
    with pytest.raises(ValueError, match="identity changed"):
        rollback._portable_dependency_source(control)


def test_portable_dependency_control_is_deterministic_and_path_free(tmp_path):
    portable = rollback._portable_dependency_source(_staged_dependency_control(tmp_path))
    assert [item["name"] for item in portable["dependencies"]] == ["backward", "googletest"]
    assert portable["dependency_count"] == 2
    assert len(portable["aggregate_sha256"]) == 64
    _assert_no_paths(portable)


@pytest.mark.parametrize("location", ["anchor_build", "anchor_subbuild", "anchor_other",
                                       "staged_sibling"])
def test_dependency_cache_rejects_all_unauthorized_path_entries(tmp_path, location):
    control = _staged_dependency_control(tmp_path)
    anchor_base = Path(control["anchor_base_dir"])
    staged_root = Path(control["staged_source_cache"])
    cache = {
        "FETCHCONTENT_BASE_DIR": {"type": "PATH", "value": control["fresh_base_dir"]},
        "FETCHCONTENT_FULLY_DISCONNECTED": {"type": "BOOL", "value": "ON"},
    }
    for dependency in control["dependencies"]:
        cache[dependency["override_variable"]] = {
            "type": "PATH", "value": dependency["staged_source"]}
    bad = {
        "anchor_build": anchor_base / "backward-build",
        "anchor_subbuild": anchor_base / "backward-subbuild",
        "anchor_other": anchor_base / "unrelated",
        "staged_sibling": staged_root / "unregistered-src" / "CMakeFiles",
    }[location]
    cache["UNAUTHORIZED_PATH"] = {"type": "PATH", "value": str(bad)}
    with pytest.raises(RuntimeError, match="anchor dependency base|authorized staged source"):
        rollback._require_dependency_cache(cache, Path(control["fresh_base_dir"]), staged_root,
                                           control["dependencies"], anchor_base)


def test_dependency_cache_rejects_symlink_alias_into_anchor_base(tmp_path):
    control = _staged_dependency_control(tmp_path)
    anchor_base = Path(control["anchor_base_dir"])
    alias = tmp_path / "alias"
    alias.symlink_to(anchor_base, target_is_directory=True)
    cache = {
        "FETCHCONTENT_BASE_DIR": {"type": "PATH", "value": control["fresh_base_dir"]},
        "FETCHCONTENT_FULLY_DISCONNECTED": {"type": "BOOL", "value": "ON"},
    }
    for dependency in control["dependencies"]:
        cache[dependency["override_variable"]] = {
            "type": "PATH", "value": dependency["staged_source"]}
    cache["ALIAS_PATH"] = {"type": "PATH", "value": str(alias / "dep-build")}
    with pytest.raises(RuntimeError, match="anchor dependency base"):
        rollback._require_dependency_cache(cache, Path(control["fresh_base_dir"]),
                                           Path(control["staged_source_cache"]),
                                           control["dependencies"], anchor_base)


def test_dependency_cache_allows_generated_descendant_of_exact_staged_source(tmp_path):
    control = _staged_dependency_control(tmp_path)
    staged_root = Path(control["staged_source_cache"])
    iceoryx_source = Path(control["dependencies"][0]["staged_source"])
    platform = iceoryx_source / "iceoryx_platform" / "linux"
    platform.mkdir(parents=True)
    cache = {
        "FETCHCONTENT_BASE_DIR": {"type": "PATH", "value": control["fresh_base_dir"]},
        "FETCHCONTENT_FULLY_DISCONNECTED": {"type": "BOOL", "value": "ON"},
        "ICEORYX_PLATFORM": {"type": "PATH", "value": str(platform)},
    }
    for dependency in control["dependencies"]:
        cache[dependency["override_variable"]] = {
            "type": "PATH", "value": dependency["staged_source"]}

    rollback._require_dependency_cache(
        cache, Path(control["fresh_base_dir"]), staged_root, control["dependencies"],
        anchor_base=Path(control["anchor_base_dir"]))


def test_dependency_cache_rejects_symlink_alias_into_exact_staged_source(tmp_path):
    control = _staged_dependency_control(tmp_path)
    staged_root = Path(control["staged_source_cache"])
    platform = Path(control["dependencies"][0]["staged_source"]) / "iceoryx_platform" / "linux"
    platform.mkdir(parents=True)
    alias = tmp_path / "iceoryx-platform-alias"
    alias.symlink_to(platform, target_is_directory=True)
    cache = {
        "FETCHCONTENT_BASE_DIR": {"type": "PATH", "value": control["fresh_base_dir"]},
        "FETCHCONTENT_FULLY_DISCONNECTED": {"type": "BOOL", "value": "ON"},
        "ICEORYX_PLATFORM": {"type": "PATH", "value": str(alias)},
    }
    for dependency in control["dependencies"]:
        cache[dependency["override_variable"]] = {
            "type": "PATH", "value": dependency["staged_source"]}

    with pytest.raises(RuntimeError, match="aliased path"):
        rollback._require_dependency_cache(
            cache, Path(control["fresh_base_dir"]), staged_root, control["dependencies"],
            anchor_base=Path(control["anchor_base_dir"]))


def test_dependency_cache_allows_canonical_staged_and_fresh_paths(tmp_path):
    control = _staged_dependency_control(tmp_path)
    fresh_base = Path(control["fresh_base_dir"])
    cache = {
        "FETCHCONTENT_BASE_DIR": {"type": "PATH", "value": str(fresh_base)},
        "FETCHCONTENT_FULLY_DISCONNECTED": {"type": "BOOL", "value": "ON"},
        "GENERATED_DEP_BUILD": {"type": "PATH", "value": str(fresh_base / "dep-build")},
    }
    for dependency in control["dependencies"]:
        cache[dependency["override_variable"]] = {
            "type": "PATH", "value": dependency["staged_source"]}
    rollback._require_dependency_cache(
        cache, fresh_base, Path(control["staged_source_cache"]), control["dependencies"],
        anchor_base=Path(control["anchor_base_dir"]))


def test_replayed_cmake_cxx_flags_rejects_anchor_dependency_build(tmp_path):
    control = _staged_dependency_control(tmp_path)
    anchor_base = Path(control["anchor_base_dir"])
    value = f"-O2 -I{anchor_base}/backward-build/include"
    with pytest.raises(RuntimeError, match="replayed control CMAKE_CXX_FLAGS.*anchor dependency base"):
        rollback._audit_replayed_controls(
            {"CMAKE_CXX_FLAGS": value}, directory=tmp_path,
            anchor_base=anchor_base)


def test_replayed_control_rejects_symlink_alias_into_anchor_dependency_base(tmp_path):
    control = _staged_dependency_control(tmp_path)
    anchor_base = Path(control["anchor_base_dir"])
    alias = tmp_path / "anchor-deps-alias"
    alias.symlink_to(anchor_base, target_is_directory=True)
    with pytest.raises(RuntimeError, match="anchor dependency base"):
        rollback._audit_replayed_controls(
            {"CMAKE_CXX_FLAGS": f"-isystem {alias}/backward-subbuild/include"},
            directory=tmp_path, anchor_base=anchor_base)


@pytest.mark.parametrize("arguments", [
    lambda path: [f"-I{path}"],
    lambda path: ["-I", str(path)],
    lambda path: [f"-isystem{path}"],
    lambda path: ["-isystem", str(path)],
    lambda path: ["-include", str(path)],
    lambda path: [f"--sysroot={path}"],
    lambda path: ["--sysroot", str(path)],
    lambda path: [f"-fplugin={path}"],
    lambda path: [f"-specs={path}"],
    lambda path: [f"-L{path}"],
    lambda path: ["-L", str(path)],
    lambda path: [f"-Wl,-rpath,{path}"],
    lambda path: [str(path)],
])
def test_compile_argument_audit_rejects_joined_separated_and_plain_anchor_paths(
        tmp_path, arguments):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    anchor_base.mkdir(parents=True)
    bad = anchor_base / "backward-build" / "include"
    records[0]["arguments"][1:1] = arguments(bad)
    with pytest.raises(RuntimeError, match="fresh compile command references anchor dependency base"):
        rollback.parse_compile_inventory(
            _compile(records), source, build, anchor_base)


_ADDITIONAL_COMPILER_PATH_OPTIONS = (
    "-iquote", "-idirafter", "-imacros", "-isysroot", "-iprefix",
    "-iwithprefix", "-iwithprefixbefore", "-include-pch", "-F", "-iframework", "-B",
)


@pytest.mark.parametrize("option", _ADDITIONAL_COMPILER_PATH_OPTIONS)
@pytest.mark.parametrize("joined", [True, False], ids=["joined", "separated"])
@pytest.mark.parametrize("alias", [False, True], ids=["direct", "symlink-alias"])
def test_compile_argument_audit_rejects_additional_anchor_path_options(
        tmp_path, option, joined, alias):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    bad = anchor_base / "backward-src" / "include"
    bad.mkdir(parents=True)
    operand = bad
    if alias:
        operand = tmp_path / "dependency-alias"
        operand.symlink_to(bad, target_is_directory=True)
    arguments = [f"{option}{operand}"] if joined else [option, str(operand)]
    records[0]["arguments"][1:1] = arguments
    with pytest.raises(RuntimeError, match="fresh compile command references anchor dependency base"):
        rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)


@pytest.mark.parametrize("arguments", [
    lambda path: ["--include-directory", str(path)],
    lambda path: [f"--include-directory={path}"],
    lambda path: ["--include-directory-after", str(path)],
    lambda path: [f"--include-directory-after={path}"],
    lambda path: ["--include", str(path)],
    lambda path: [f"--include={path}"],
    lambda path: ["--imacros", str(path)],
    lambda path: [f"--imacros={path}"],
    lambda path: ["--include-prefix", str(path)],
    lambda path: [f"--include-prefix={path}"],
    lambda path: ["--include-with-prefix", str(path)],
    lambda path: [f"--include-with-prefix={path}"],
    lambda path: ["--include-with-prefix-before", str(path)],
    lambda path: [f"--include-with-prefix-before={path}"],
    lambda path: ["--prefix", str(path)],
    lambda path: [f"--prefix={path}"],
    lambda path: [f"--gcc-toolchain={path}"],
])
def test_compile_argument_audit_rejects_supported_long_anchor_path_options(
        tmp_path, arguments):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    bad = anchor_base / "backward-src"
    bad.mkdir(parents=True)
    records[0]["arguments"][1:1] = arguments(bad)
    with pytest.raises(RuntimeError, match="fresh compile command references anchor dependency base"):
        rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)


@pytest.mark.parametrize("option", _ADDITIONAL_COMPILER_PATH_OPTIONS)
@pytest.mark.parametrize("joined", [True, False], ids=["joined", "separated"])
@pytest.mark.parametrize("location", ["fresh", "staged"])
def test_compile_argument_audit_allows_additional_fresh_and_staged_paths(
        tmp_path, option, joined, location):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    anchor_base.mkdir(parents=True)
    root = build if location == "fresh" else tmp_path / "work" / "source-cache"
    operand = (root / "backward-src" / "include").resolve()
    operand.mkdir(parents=True)
    arguments = [f"{option}{operand}"] if joined else [option, str(operand)]
    records[0]["arguments"][1:1] = arguments
    result = rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)
    assert result["record_count"] == 2


def test_command_path_joined_forms_are_longest_first_and_do_not_match_arbitrary_i_tokens(
        tmp_path):
    assert rollback._PATH_ARGUMENT_JOINED_FORMS.index("-include-pch") < (
        rollback._PATH_ARGUMENT_JOINED_FORMS.index("-include"))
    assert rollback._PATH_ARGUMENT_JOINED_FORMS.index("-iwithprefixbefore") < (
        rollback._PATH_ARGUMENT_JOINED_FORMS.index("-iwithprefix"))
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    anchor_base.mkdir(parents=True)
    arbitrary = "-Winvalid-command-line-argument"
    assert not any(arbitrary.startswith(form) for form in rollback._PATH_ARGUMENT_JOINED_FORMS)
    rollback._audit_command_tokens(
        [arbitrary], directory=tmp_path,
        anchor_base=anchor_base, label="fresh compile command")


@pytest.mark.parametrize("arguments", [
    lambda path: [f"-Wl,-L,{path}"],
    lambda path: [f"-Wl,-L{path}"],
    lambda path: [f"-Wp,-I,{path}"],
    lambda path: [f"-Wp,-I{path}"],
])
def test_compile_argument_audit_rejects_latest_comma_forwarding_escapes(
        tmp_path, arguments):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    bad = anchor_base / "backward-src" / "include"
    bad.mkdir(parents=True)
    records[0]["arguments"][1:1] = arguments(bad)
    with pytest.raises(RuntimeError, match="anchor dependency base"):
        rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)


@pytest.mark.parametrize("arguments", [
    lambda path: ["-Xclang", "-include-pch", "-Xclang", str(path)],
    lambda path: ["-Xpreprocessor", "-iframework", "-Xpreprocessor", str(path)],
    lambda path: ["-Xlinker", "--library-path", "-Xlinker", str(path)],
    lambda path: [f"-Wl,--sysroot={path}"],
    lambda path: [f"-Wp,-isysroot,{path}"],
])
def test_compile_argument_audit_rejects_forwarded_path_option_streams(
        tmp_path, arguments):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    bad = anchor_base / "backward-src" / "frameworks"
    bad.mkdir(parents=True)
    records[0]["arguments"][1:1] = arguments(bad)
    with pytest.raises(RuntimeError, match="anchor dependency base"):
        rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)


def test_compile_argument_audit_include_pch_does_not_collide_with_include(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    bad = anchor_base / "backward-src" / "prefix.pch"
    bad.parent.mkdir(parents=True)
    bad.write_bytes(b"pch")
    records[0]["arguments"][1:1] = [f"-include-pch{bad}", "-Winvalid-pch"]
    with pytest.raises(RuntimeError, match="anchor dependency base"):
        rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)


@pytest.mark.parametrize("wrapper", ["-Xclang", "-Xpreprocessor", "-Xlinker"])
def test_compile_argument_audit_rejects_forwarded_plain_symlink_alias(tmp_path, wrapper):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    bad = anchor_base / "backward-src"
    bad.mkdir(parents=True)
    alias = build / "dependency-alias"
    alias.symlink_to(bad, target_is_directory=True)
    records[0]["arguments"][1:1] = [wrapper, alias.name]
    with pytest.raises(RuntimeError, match="anchor dependency base"):
        rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)


def test_compile_argument_audit_allows_flags_and_forwarded_fresh_staged_paths(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    anchor_base.mkdir(parents=True)
    fresh = build / "frameworks"
    staged = tmp_path / "work" / "source-cache" / "backward-src" / "include"
    fresh.mkdir(); staged.mkdir(parents=True)
    records[0]["arguments"][1:1] = [
        "-Winvalid-command-line-argument",
        "-Wl,-z,now", f"-Wl,-F,{fresh}",
        "-Xpreprocessor", "-I", "-Xpreprocessor", str(staged),
        "-Xlinker", "--sysroot", "-Xlinker", str(build),
    ]
    result = rollback.parse_compile_inventory(_compile(records), source, build, anchor_base)
    assert result["record_count"] == 2


def test_compile_argument_audit_rejects_anchor_path_in_response_file(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    anchor_base.mkdir(parents=True)
    response = build / "flags.rsp"
    response.write_text(f"-O2 -I {anchor_base}/backward-build/include\n")
    records[0]["arguments"].insert(1, "@flags.rsp")
    with pytest.raises(RuntimeError, match="fresh compile command references anchor dependency base"):
        rollback.parse_compile_inventory(
            _compile(records), source, build, anchor_base)


def test_compile_argument_audit_allows_private_staged_include(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    anchor_base = (tmp_path / "anchor" / "_deps").resolve()
    anchor_base.mkdir(parents=True)
    staged = (tmp_path / "work" / "source-cache" / "backward-src" / "include").resolve()
    staged.mkdir(parents=True)
    records[0]["arguments"][1:1] = ["-isystem", str(staged)]
    result = rollback.parse_compile_inventory(
        _compile(records), source, build, anchor_base)
    assert result["record_count"] == 2


def test_dependency_aggregate_limits_allow_boundary_and_reject_exceed(tmp_path, monkeypatch):
    base, cache = _dependency_fixture(tmp_path)
    _anchor, dependencies = rollback._anchor_dependency_sources(cache)
    total_files = sum(item["identity"]["anchor_before_staging"]["file_count"]
                      for item in dependencies)
    total_directories = sum(item["identity"]["anchor_before_staging"]["directory_count"]
                            for item in dependencies)
    total_bytes = sum(item["identity"]["anchor_before_staging"]["byte_count"]
                      for item in dependencies)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_FILES", total_files)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_DIRECTORIES", total_directories)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_ENTRIES", total_files + total_directories)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_TOTAL_BYTES", total_bytes)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_BYTES", total_bytes)
    work = tmp_path / "work"
    work.mkdir()
    rollback._stage_dependency_sources(work, dependencies)
    aggregate = rollback._aggregate_dependency_identity(dependencies, "anchor_before_staging")
    assert aggregate["file_count"] == total_files
    assert aggregate["directory_count"] == total_directories
    assert aggregate["byte_count"] == total_bytes

    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_FILES", total_files - 1)
    with pytest.raises(RuntimeError, match="aggregate file count"):
        rollback._aggregate_dependency_identity(dependencies, "anchor_before_staging")
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_FILES", total_files)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_DIRECTORIES", total_directories - 1)
    with pytest.raises(RuntimeError, match="aggregate directory count"):
        rollback._aggregate_dependency_identity(dependencies, "anchor_before_staging")
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_DIRECTORIES", total_directories)
    monkeypatch.setattr(rollback, "_MAX_DEPENDENCY_TOTAL_BYTES", total_bytes - 1)
    with pytest.raises(RuntimeError, match="aggregate byte limit"):
        rollback._aggregate_dependency_identity(dependencies, "anchor_before_staging")


def test_tool_identity_replacement_is_rejected(tmp_path):
    tool = tmp_path / "tool"
    tool.write_text("#!/bin/sh\nprintf 'one\\n'\n")
    tool.chmod(0o755)
    before = rollback._tool_identity(str(tool))
    tool.write_text("#!/bin/sh\nprintf 'two\\n'\n")
    after = rollback._tool_identity(str(tool))
    assert {"path", "device", "inode", "size", "sha256", "version"} <= set(after)
    with pytest.raises(RuntimeError, match="identity differs"):
        rollback._require_same_tool_identity(before, after, "CMAKE_AR")


def test_tool_identity_canonical_path_mismatch_is_rejected(tmp_path):
    first = tmp_path / "first"; second = tmp_path / "second"
    first.write_text("same executable bytes\n"); second.write_bytes(first.read_bytes())
    with pytest.raises(RuntimeError, match="identity differs"):
        rollback._require_same_tool_identity(rollback._file_identity(first),
                                             rollback._file_identity(second), "ambient cmake")


def _compile(records):
    return json.dumps(records)


def _strict_compile_fixture(tmp_path):
    source = (tmp_path / "source").resolve()
    build = (tmp_path / "build").resolve()
    source.mkdir(); build.mkdir()
    records = []
    for source_name, target in rollback._REQUIRED_SOURCES.items():
        source_file = source / source_name
        source_file.parent.mkdir(parents=True, exist_ok=True)
        source_file.write_text("// source\n")
        output = build / "src" / "CMakeFiles" / target / (source_file.stem + ".o")
        output.parent.mkdir(parents=True, exist_ok=True)
        records.append({"directory": str(build), "file": str(source_file),
                        "arguments": ["c++", "-c", str(source_file), "-o", str(output)]})
    return source, build, records


def test_compile_inventory_accepts_required_sources():
    inventory = rollback.parse_compile_inventory(_compile([
        {"file": "/src/dds_transport_driver.cc", "arguments": ["c++", "-c"]},
        {"file": "/src/dds_managed_exact_candidate.cc", "command": "c++ -c candidate"},
    ]))
    assert inventory["record_count"] == 2
    assert len(inventory["content_sha256"]) == 64


@pytest.mark.parametrize("record,error", [
    ({"file": "/repo/src/extensions/transport/dds_ext/dds_comm_trace.cc", "command": "c++ -c"}, "forbidden trace source"),
    ({"file": "/repo/src/core/observability/comm_trace.cc", "command": "c++ -c"}, "forbidden trace source"),
    ({"file": "/src/dds_transport_driver.cc", "command": "c++ -DIBMW_ENABLE_COMM_TRACE=1"}, "trace enable token"),
    ({"file": "/src/dds_transport_driver.cc", "arguments": ["c++", "-DIBMW_ENABLE_COMM_TRACE"]}, "trace enable token"),
])
def test_compile_inventory_rejects_trace_material(record, error):
    records = [record, {"file": "/src/dds_managed_exact_candidate.cc", "command": "c++ -c"}]
    with pytest.raises(RuntimeError, match=error):
        rollback.parse_compile_inventory(_compile(records))


def test_compile_inventory_rejects_missing_required_source():
    with pytest.raises(RuntimeError, match="omit required"):
        rollback.parse_compile_inventory(_compile([
            {"file": "/src/dds_transport_driver.cc", "command": "c++ -c"}]))


def test_nm_parser_extracts_demangled_symbol_portion():
    output = ("0000000000001234 T ibmw::Thing::Run(int)\n"
              "                 U malloc@GLIBC_2.2.5\n"
              "file.o:0000 t local name\n"
              "archive.a(member.o):0001 W archive symbol\n")
    assert rollback.parse_nm_symbols(output) == [
        "ibmw::Thing::Run(int)", "malloc@GLIBC_2.2.5", "local name", "archive symbol"]


def test_compile_inventory_resolves_relative_files_and_checks_target_dirs(tmp_path):
    source = tmp_path / "source"
    build = tmp_path / "build"
    extension = source / "src/extensions/transport/dds_ext"
    extension.mkdir(parents=True)
    build.mkdir()
    for name in ("dds_transport_driver.cc", "dds_managed_exact_candidate.cc"):
        (extension / name).write_text("// source\n")
    driver_output = build / "src/CMakeFiles/ibmw_dds_extension.dir/driver.o"
    candidate_output = build / "src/CMakeFiles/ibmw_dds_trace_off_business_roundtrip.dir/main.o"
    driver_output.parent.mkdir(parents=True)
    candidate_output.parent.mkdir(parents=True)
    driver_output.write_bytes(b"object")
    candidate_output.write_bytes(b"object")
    records = [
        {"directory": str(extension), "file": "dds_transport_driver.cc",
         "arguments": ["c++", "-c", "dds_transport_driver.cc", "-o",
                       str(driver_output)]},
        {"directory": str(extension), "file": "dds_managed_exact_candidate.cc",
         "command": f"c++ -c dds_managed_exact_candidate.cc -o {candidate_output}"},
    ]
    result = rollback.parse_compile_inventory(_compile(records), source, build)
    assert set(result["sources"]) == {
        "src/extensions/transport/dds_ext/dds_transport_driver.cc",
        "src/extensions/transport/dds_ext/dds_managed_exact_candidate.cc"}
    assert all(len(item["command_sha256"]) == 64 for item in result["records"])
    assert {item["target"] for item in result["records"]} == rollback._REGISTERED_TARGETS
    assert result["registered_records"]["record_count"] == 2
    assert result["other_records"]["record_count"] == 0
    assert "Registered-target source validation excludes" in result["claim"]
    assert "whole compile configuration" in result["claim"]


def test_compile_inventory_normalizes_registered_dependency_source(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    dependency = tmp_path / "source-cache" / "fmt-src"
    dependency_source = dependency / "src/format.cc"
    dependency_source.parent.mkdir(parents=True)
    dependency_source.write_text("// dependency\n")
    output = build / "_deps/fmt-build/CMakeFiles/fmt.dir/src/format.cc.o"
    output.parent.mkdir(parents=True)
    output.write_bytes(b"object")
    records.append({"directory": str(build), "file": str(dependency_source),
                    "arguments": ["c++", "-I" + str(dependency / "include"), "-c",
                                  str(dependency_source), "-o", str(output)]})

    result = rollback.parse_compile_inventory(
        _compile(records), source, build, dependency_source_roots={"fmt": dependency})

    item = next(item for item in result["records"] if item["target"] == "fmt")
    assert item["source"] == "dependency:fmt/src/format.cc"
    assert item["command_sha256"] == hashlib.sha256("\0".join([
        "c++", "-Idependency:fmt/include", "-c", "dependency:fmt/src/format.cc",
        "-o", "build/_deps/fmt-build/CMakeFiles/fmt.dir/src/format.cc.o",
    ]).encode()).hexdigest()
    rollback._path_free(result)
    assert str(tmp_path) not in json.dumps(result)


def test_compile_inventory_rejects_required_output_outside_build(tmp_path):
    source = tmp_path / "source"
    build = tmp_path / "build"
    outside = tmp_path / "outside"
    extension = source / "src/extensions/transport/dds_ext"
    extension.mkdir(parents=True); build.mkdir(); outside.mkdir()
    records = []
    for source_name, target in rollback._REQUIRED_SOURCES.items():
        source_file = source / source_name
        source_file.write_text("// source\n")
        output = outside / "CMakeFiles" / target / (source_file.stem + ".o")
        output.parent.mkdir(parents=True, exist_ok=True); output.write_bytes(b"object")
        records.append({"directory": str(build), "file": str(source_file), "output": str(output),
                        "arguments": ["c++", "-c", str(source_file), "-o", str(output)]})
    with pytest.raises(RuntimeError, match="outside fresh build root"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_arbitrary_output_outside_build(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    ordinary = source / "src/ordinary.cc"; ordinary.write_text("// source\n")
    outside = tmp_path / "outside.o"
    records.append({"directory": str(build), "file": str(ordinary),
                    "arguments": ["c++", "-c", str(ordinary), "-o", str(outside)]})
    with pytest.raises(RuntimeError, match="compile output is outside fresh build root"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_duplicate_required_source(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    duplicate = dict(records[0])
    duplicate["arguments"] = list(records[0]["arguments"])
    duplicate_output = Path(duplicate["arguments"][-1]).with_name("duplicate.o")
    duplicate["arguments"][-1] = str(duplicate_output)
    records.append(duplicate)
    with pytest.raises(RuntimeError, match="duplicate required rollback sources"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_allows_required_source_duplicate_in_other_target(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    duplicate = dict(records[0])
    duplicate["arguments"] = list(records[0]["arguments"])
    duplicate["arguments"][-1] = str(build / "src/CMakeFiles/other_test.dir/driver.o")
    records.append(duplicate)
    result = rollback.parse_compile_inventory(_compile(records), source, build)
    assert result["record_count"] == 3
    assert result["registered_records"]["record_count"] == 2
    assert result["other_records"]["record_count"] == 1
    assert {item["target"] for item in result["records"]} == {
        "ibmw_dds_extension", "ibmw_dds_trace_off_business_roundtrip", "other_test"}


def test_compile_inventory_rejects_required_source_in_other_registered_target(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    records[0]["arguments"][-1] = str(
        build / "src/CMakeFiles/ibmw_dds_trace_off_business_roundtrip.dir/driver.o")
    with pytest.raises(RuntimeError, match="appears in the other registered target"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_duplicate_required_source_in_registered_target(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    duplicate = dict(records[0])
    duplicate["arguments"] = list(records[0]["arguments"])
    duplicate["arguments"][-1] = str(
        build / "src/CMakeFiles/ibmw_dds_extension.dir/duplicate.o")
    records.append(duplicate)
    with pytest.raises(RuntimeError, match="duplicate required rollback sources"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_dot_dot_source_alias(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    source_file = Path(records[0]["file"])
    records[0]["file"] = str(source_file.parent / "subdir" / ".." / source_file.name)
    with pytest.raises(RuntimeError, match=r"source uses a '\.\.' alias"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_symlink_source_alias(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    extension = source / "src/extensions/transport/dds_ext"
    alias = source / "source-alias"
    alias.symlink_to(extension, target_is_directory=True)
    records[0]["file"] = str(alias / "dds_transport_driver.cc")
    with pytest.raises(RuntimeError, match="source uses a symlink alias"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_dot_dot_output_alias(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    output = Path(records[0]["arguments"][-1])
    records[0]["arguments"][-1] = str(output.parent / "subdir" / ".." / output.name)
    with pytest.raises(RuntimeError, match=r"output uses a '\.\.' alias"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_rejects_symlink_output_alias(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    real_target = Path(records[0]["arguments"][-1]).parent
    alias = build / "target-alias"
    alias.symlink_to(real_target, target_is_directory=True)
    records[0]["arguments"][-1] = str(alias / "driver.o")
    with pytest.raises(RuntimeError, match="output uses a symlink alias"):
        rollback.parse_compile_inventory(_compile(records), source, build)


def test_compile_inventory_prefers_relative_o_token_to_generated_output_field(tmp_path):
    source = tmp_path / "source"; build = tmp_path / "build"
    source_dir = source / "src/extensions/transport/dds_ext"; source_dir.mkdir(parents=True)
    records = []
    for source_name, target in rollback._REQUIRED_SOURCES.items():
        source_file = source / source_name; source_file.write_text("// source\n")
        directory = build / "src/extensions/transport/dds_ext"; directory.mkdir(parents=True, exist_ok=True)
        relative = Path("CMakeFiles") / target / (source_file.stem + ".o")
        output = directory / relative; output.parent.mkdir(parents=True, exist_ok=True); output.write_bytes(b"object")
        generated_output = Path("src/extensions/transport/dds_ext") / relative
        records.append({"directory": str(directory), "file": str(source_file),
                        "output": str(generated_output),
                        "command": f"c++ -c {source_file} -o {relative}"})
    result = rollback.parse_compile_inventory(_compile(records), source, build)
    assert all(item["output"].startswith("src/extensions/transport/dds_ext/CMakeFiles/")
               for item in result["records"])


def test_compile_inventory_allows_unbuilt_generated_source_in_fresh_build(tmp_path):
    source, build, records = _strict_compile_fixture(tmp_path)
    generated = build / "src/schema/dds_idl/common/gencode/HeaderPubSubTypes.cxx"
    output = build / "src/schema/CMakeFiles/schema_codegen.dir/HeaderPubSubTypes.cxx.o"
    records.append({"directory": str(build), "file": str(generated),
                    "command": f"c++ -c {generated} -o {output}"})
    result = rollback.parse_compile_inventory(_compile(records), source, build)
    assert any(item["source"] ==
               "build/src/schema/dds_idl/common/gencode/HeaderPubSubTypes.cxx"
               for item in result["records"])


@pytest.mark.parametrize("provider_count", [0, 2])
def test_provider_identity_rejects_missing_or_ambiguous_provider(tmp_path, monkeypatch,
                                                                 provider_count):
    prefix = tmp_path / "provider"; library = prefix / "lib"; library.mkdir(parents=True)
    for index in range(provider_count):
        (library / f"libfastdds.so.{index}").write_bytes(b"dds")
    (library / "libfastcdr.so.1").write_bytes(b"cdr")
    cache = {"IBMW_THIRD_PARTY_SOURCE_PREFIX": {"value": str(prefix)}}
    monkeypatch.setattr(rollback, "_elf_identity", lambda path, readelf: {"path": str(path)})
    with pytest.raises(RuntimeError, match="exactly one fresh-derived dds provider"):
        rollback._provider_identity(cache, "readelf")


def _link_plan_fixture(tmp_path, *, response=False):
    build = (tmp_path / "fresh" / "build").resolve(); build.mkdir(parents=True)
    anchor = (tmp_path / "anchor").resolve(); anchor.mkdir()
    provider_root = (tmp_path / "provider" / "lib").resolve(); provider_root.mkdir(parents=True)
    providers = {}
    for family in ("dds", "cdr"):
        path = provider_root / f"libfast{family}.so.1"; path.write_bytes(family.encode())
        providers[family] = {"path": str(path)}
    providers["library_paths"] = [str(provider_root)]
    core_archive = build / "libibmw_core_engine.a"; core_archive.write_bytes(b"archive")
    extension = build / "libibmw_dds_transport.so"; extension.write_bytes(b"extension")
    executable = build / "ibmw_dds_trace_off_business_roundtrip"; executable.write_bytes(b"executable")
    images = {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
              for name, path in (("extension", extension), ("executable", executable))}
    base = build / "src/extensions/transport/dds_ext"
    for target, image, output in (
            ("ibmw_dds_extension", "extension", extension),
            ("ibmw_dds_trace_off_business_roundtrip", "executable", executable)):
        plan_dir = base / "CMakeFiles" / (target + ".dir"); plan_dir.mkdir(parents=True)
        command_dir = base
        obj = plan_dir / ("driver.o" if image == "extension" else "main.o")
        obj.write_bytes((target + " object").encode())
        args = f"{os.path.relpath(obj, command_dir)} {os.path.relpath(core_archive, command_dir)} " \
               f"{providers['dds']['path']} {providers['cdr']['path']}"
        if response:
            (plan_dir / "inputs.rsp").write_text(args)
            args = f"@{os.path.relpath(plan_dir / 'inputs.rsp', command_dir)}"
        (plan_dir / "link.txt").write_text(
            f"/bin/true {args} -o {os.path.relpath(output, command_dir)}\n")
    return build, anchor, images, providers


def test_link_closure_accepts_realistic_generated_plans_and_response_files(tmp_path):
    build, anchor, images, providers = _link_plan_fixture(tmp_path, response=True)
    closure = rollback.capture_link_closure(build, anchor, images, providers)
    assert [item["target"] for item in closure["plans"]] == list(rollback._LINK_TARGET_IMAGES)
    assert all(item["responses"] for item in closure["plans"])
    assert all(item["output"]["sha256"] == images[item["image"]]["sha256"]
               for item in closure["plans"])
    assert all(item["plan"]["identity"].startswith("fresh:") for item in closure["plans"])
    assert str(tmp_path) not in json.dumps(closure)


def _ninja_link_fixture(tmp_path, *, response=False):
    build, anchor, images, providers = _link_plan_fixture(tmp_path)
    commands = {}
    for target, image_name in rollback._LINK_TARGET_IMAGES.items():
        plan = next(build.rglob(f"CMakeFiles/{target}.dir/link.txt"))
        tokens = plan.read_text().split()
        command_dir = plan.parent.parent.parent
        for index, token in enumerate(tokens):
            if index == 0 or token == "-o" or token.startswith("-"):
                continue
            if ((index > 0 and tokens[index - 1] == "-o")
                    or rollback._LINK_FILE_RE.search(token)):
                candidate = Path(token)
                tokens[index] = str(candidate if candidate.is_absolute() else
                                    (command_dir / candidate).resolve())
        command = " ".join(tokens)
        if response:
            output_index = tokens.index("-o")
            response_path = build / f"{target}.rsp"
            response_path.write_text(" ".join(tokens[1:output_index]))
            command = f"/bin/true @{response_path.name} " + " ".join(tokens[output_index:])
        commands[target] = command
        plan.unlink()
    tool = (tmp_path / "ninja-exporter").resolve()
    tool.write_text(
        f"#!{sys.executable}\n"
        "import json, sys\n"
        f"commands = json.loads({json.dumps(json.dumps(commands))})\n"
        "if sys.argv[1:] == ['--version']:\n"
        "    print('test-ninja 1')\n"
        "else:\n"
        "    print(commands[sys.argv[-1]])\n")
    tool.chmod(0o755)
    (build / "build.ninja").write_text("# immutable Ninja authority\n")
    (build / "CMakeCache.txt").write_text(
        f"CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_MAKE_PROGRAM:FILEPATH={tool}\n")
    return build, anchor, images, providers, tool


def test_ninja_link_closure_captures_path_free_authority_and_response_file(tmp_path):
    build, anchor, images, providers, tool = _ninja_link_fixture(tmp_path, response=True)
    closure = rollback.capture_link_closure(build, anchor, images, providers)
    assert all(plan["plan"]["identity"] == "ninja:build.ninja" for plan in closure["plans"])
    assert all(plan["responses"][0]["identity"] == "ninja-tool" for plan in closure["plans"])
    assert all(plan["responses"][1]["identity"] ==
               f"ninja-commands:{plan['target']}" for plan in closure["plans"])
    assert all(any(item["identity"].startswith("response-")
                   for item in plan["responses"][2:]) for plan in closure["plans"])
    assert closure["plans"][0]["responses"][0]["sha256"] == rollback._sha256(tool)
    assert str(tmp_path) not in json.dumps(closure)


@pytest.mark.parametrize("commands", [
    ["/usr/bin/c++ -c input.cc -o CMakeFiles/example.dir/input.o"],
    ["/usr/bin/c++ input.o -o audited", "/usr/bin/c++ input.o -o audited"],
    ["/usr/bin/c++ input.o -o different"],
], ids=["zero", "multiple", "mismatched"])
def test_ninja_final_link_command_rejects_zero_multiple_and_mismatched_outputs(
        tmp_path, commands):
    build = tmp_path.resolve()
    expected = build / "audited"
    with pytest.raises(RuntimeError, match="exactly one Ninja final link command"):
        rollback._ninja_link_command(("\n".join(commands) + "\n").encode(),
                                     build, expected, "example")


def test_ninja_final_link_command_skips_external_dependency_working_directory(tmp_path):
    build = (tmp_path / "fresh" / "build").resolve(); build.mkdir(parents=True)
    dependency = (tmp_path / "anchor" / "_deps" / "backward-build").resolve()
    dependency.mkdir(parents=True)
    expected = build / "audited"
    commands = (
        f"cd {dependency} && /usr/bin/c++ -c input.cc -o CMakeFiles/dependency.dir/input.o\n"
        f"cd {build} && /usr/bin/c++ input.o -o {expected}\n"
    )
    tokens, directory = rollback._ninja_link_command(
        commands.encode(), build, expected, "example")
    assert tokens[-2:] == ["-o", str(expected)]
    assert directory == build


@pytest.mark.parametrize("cwd_kind", ["external", "alias"])
def test_ninja_final_link_command_rejects_matching_output_from_unauthorized_cwd(
        tmp_path, cwd_kind):
    build = (tmp_path / "fresh" / "build").resolve(); build.mkdir(parents=True)
    expected = build / "audited"
    if cwd_kind == "external":
        command_dir = (tmp_path / "anchor" / "_deps" / "backward-build").resolve()
        command_dir.mkdir(parents=True)
        error = "outside fresh build root"
    else:
        real_dir = build / "link"; real_dir.mkdir()
        command_dir = tmp_path / "link-alias"
        command_dir.symlink_to(real_dir, target_is_directory=True)
        error = "aliased working directory"
    command = f"cd {command_dir} && /usr/bin/c++ input.o -o {expected}\n"
    with pytest.raises(RuntimeError, match=error):
        rollback._ninja_link_command(command.encode(), build, expected, "example")


def test_ninja_final_link_command_rejects_multiple_matches_with_dependency_commands(tmp_path):
    build = (tmp_path / "fresh" / "build").resolve(); build.mkdir(parents=True)
    dependency = (tmp_path / "anchor" / "_deps" / "backward-build").resolve()
    dependency.mkdir(parents=True)
    expected = build / "audited"
    commands = (
        f"cd {dependency} && /usr/bin/c++ -c input.cc -o dependency.o\n"
        f"/usr/bin/c++ one.o -o {expected}\n"
        f"/usr/bin/c++ two.o -o {expected}\n"
    )
    with pytest.raises(RuntimeError, match="exactly one Ninja final link command.*found 2"):
        rollback._ninja_link_command(commands.encode(), build, expected, "example")


def test_ninja_link_closure_detects_authority_and_exporter_mutation(tmp_path):
    build, anchor, images, providers, tool = _ninja_link_fixture(tmp_path)
    before = rollback.capture_link_closure(build, anchor, images, providers)
    (build / "build.ninja").write_text("# mutated Ninja authority\n")
    assert rollback.capture_link_closure(build, anchor, images, providers) != before
    (build / "build.ninja").write_text("# immutable Ninja authority\n")
    tool.write_text(tool.read_text() + "# mutated exporter\n")
    assert rollback.capture_link_closure(build, anchor, images, providers) != before


def test_ninja_link_closure_rejects_authority_mutation_during_export(tmp_path, monkeypatch):
    build, anchor, images, providers, _tool = _ninja_link_fixture(tmp_path)
    run = rollback._run_bounded_command
    mutated = False

    def mutate_authority(argv, *, timeout, max_output):
        nonlocal mutated
        output = run(argv, timeout=timeout, max_output=max_output)
        if not mutated:
            (build / "build.ninja").write_text("# changed during export\n")
            mutated = True
        return output

    monkeypatch.setattr(rollback, "_run_bounded_command", mutate_authority)
    with pytest.raises(RuntimeError, match="build.ninja changed during Ninja command export"):
        rollback.capture_link_closure(build, anchor, images, providers)


@pytest.mark.skipif(not all(shutil.which(tool) for tool in ("cmake", "ninja", "c++")),
                    reason="CMake, Ninja, and a C++ compiler are required")
def test_link_closure_accepts_real_cmake_ninja_fixture(tmp_path):
    source = tmp_path / "source"; source.mkdir()
    build = tmp_path / "build"
    (source / "extension.cc").write_text("extern \"C\" int extension() { return 1; }\n")
    (source / "main.cc").write_text("int main() { return 0; }\n")
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(rollback_ninja LANGUAGES CXX)\n"
        "add_library(ibmw_dds_extension SHARED extension.cc)\n"
        "set_target_properties(ibmw_dds_extension PROPERTIES OUTPUT_NAME ibmw_dds_transport)\n"
        "add_executable(ibmw_dds_trace_off_business_roundtrip main.cc)\n")
    subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    subprocess.run(["cmake", "--build", str(build)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    extension = build / "libibmw_dds_transport.so"
    executable = build / "ibmw_dds_trace_off_business_roundtrip"
    images = {name: {"path": str(path), "sha256": rollback._sha256(path)}
              for name, path in (("extension", extension), ("executable", executable))}
    provider_root = tmp_path / "provider"; provider_root.mkdir()
    providers: dict[str, Any] = {
        name: {"path": str(provider_root / f"lib{name}.so")} for name in ("dds", "cdr")}
    for item in providers.values():
        Path(item["path"]).write_bytes(b"provider")
    providers["library_paths"] = [str(provider_root)]
    anchor = tmp_path / "anchor"; anchor.mkdir()
    closure = rollback.capture_link_closure(build, anchor, images, providers)
    assert all(plan["plan"]["identity"] == "ninja:build.ninja" for plan in closure["plans"])
    assert str(tmp_path) not in json.dumps(closure)


def test_link_response_cycle_and_depth_are_rejected(tmp_path):
    plan = tmp_path / "link.txt"; one = tmp_path / "one.rsp"; two = tmp_path / "two.rsp"
    plan.write_text("/bin/true @one.rsp")
    one.write_text("@two.rsp"); two.write_text("@one.rsp")
    with pytest.raises(RuntimeError, match="cycle"):
        rollback._expand_link_tokens(plan)
    two.write_text("input.o")
    with pytest.raises(RuntimeError, match="depth"):
        rollback._expand_link_tokens(plan, max_depth=1)


@pytest.mark.parametrize("kind,error", [
    ("anchor_object", "under anchor"), ("anchor_archive", "under anchor"),
    ("outside_object", "outside fresh"), ("unregistered_shared", "not registered"),
    ("output_mismatch", "output mismatch"),
])
def test_link_closure_rejects_nonfresh_or_unregistered_inputs(tmp_path, kind, error):
    build, anchor, images, providers = _link_plan_fixture(tmp_path)
    plan = next(build.rglob("CMakeFiles/ibmw_dds_extension.dir/link.txt"))
    tokens = plan.read_text().split()
    if kind == "output_mismatch":
        wrong = build / "wrong.so"; wrong.write_bytes(b"wrong")
        tokens[tokens.index("-o") + 1] = os.path.relpath(wrong, plan.parent.parent.parent)
    else:
        suffix = {"anchor_object": "bad.o", "anchor_archive": "libbad.a",
                  "outside_object": "bad.o", "unregistered_shared": "libbad.so"}[kind]
        root = anchor if kind.startswith("anchor") else tmp_path / "outside"
        root.mkdir(exist_ok=True); bad = (root / suffix).resolve(); bad.write_bytes(b"bad")
        tokens.insert(tokens.index("-o"), str(bad))
    plan.write_text(" ".join(tokens) + "\n")
    with pytest.raises(RuntimeError, match=error):
        rollback.capture_link_closure(build, anchor, images, providers)


def test_link_closure_detects_plan_and_input_mutation(tmp_path):
    build, anchor, images, providers = _link_plan_fixture(tmp_path)
    before = rollback.capture_link_closure(build, anchor, images, providers)
    plan = next(build.rglob("CMakeFiles/ibmw_dds_extension.dir/link.txt"))
    plan.write_text(plan.read_text().replace("/bin/true", "/bin/false"))
    assert rollback.capture_link_closure(build, anchor, images, providers) != before
    plan.write_text(plan.read_text().replace("/bin/false", "/bin/true"))
    obj = next(build.rglob("CMakeFiles/ibmw_dds_extension.dir/*.o")); obj.write_bytes(b"mutated")
    assert rollback.capture_link_closure(build, anchor, images, providers) != before


@pytest.mark.parametrize("missing", ["plan", "input"])
def test_link_closure_rejects_missing_plan_or_input(tmp_path, missing):
    build, anchor, images, providers = _link_plan_fixture(tmp_path)
    if missing == "plan":
        next(build.rglob("CMakeFiles/ibmw_dds_extension.dir/link.txt")).unlink()
        error = "expected one generated link plan"
    else:
        next(build.rglob("CMakeFiles/ibmw_dds_extension.dir/*.o")).unlink()
        error = "linked input is missing"
    with pytest.raises(RuntimeError, match=error):
        rollback.capture_link_closure(build, anchor, images, providers)


def test_link_closure_rejects_ibmw_archive_in_registered_external_root(tmp_path):
    build, anchor, images, providers = _link_plan_fixture(tmp_path)
    external = Path(providers["library_paths"][0]) / "libibmw_stale.a"
    external.write_bytes(b"stale")
    plan = next(build.rglob("CMakeFiles/ibmw_dds_extension.dir/link.txt"))
    tokens = plan.read_text().split(); tokens.insert(tokens.index("-o"), str(external))
    plan.write_text(" ".join(tokens) + "\n")
    with pytest.raises(RuntimeError, match="IBMW linked library is outside fresh"):
        rollback.capture_link_closure(build, anchor, images, providers)


@pytest.mark.parametrize("nm_output,error", [
    ("not parseable\n", "no parsed symbols"),
    ("0000 T ibmw::comm_trace::hook()\n", "trace symbols found"),
])
def test_symbol_audit_rejects_zero_or_forbidden_symbols(tmp_path, monkeypatch, nm_output, error):
    image = tmp_path / "image"; image.write_bytes(b"elf")
    logs = tmp_path / "logs"; logs.mkdir()

    def fake_run(argv, log_base, timeout, **kwargs):
        stdout = nm_output if argv[0] == "nm" else "Build ID: 1234\n"
        Path(f"{log_base}.stdout.log").write_text(stdout)
        Path(f"{log_base}.stderr.log").write_text("")
        return SimpleNamespace(stdout=stdout)

    monkeypatch.setattr(rollback, "_run", fake_run)
    with pytest.raises(RuntimeError, match=error):
        rollback._audit_image(image, logs, "nm", "readelf", rollback.re.compile("ordinary"))


def _candidate_fixture(tmp_path, behavior="success"):
    candidate = tmp_path / "candidate.py"
    body = """#!/usr/bin/env python3
import os, sys, time
ready = int(os.environ['MW_PERF_ROLLBACK_READY_FD'])
release = int(os.environ['MW_PERF_ROLLBACK_RELEASE_FD'])
BEHAVIOR
"""
    actions = {
        "success": "os.write(ready, b'R'); os.read(release, 1); sys.exit(0)",
        "nonzero": "os.write(ready, b'R'); os.read(release, 1); sys.exit(7)",
        "timeout": "time.sleep(10)",
    }
    candidate.write_text(body.replace("BEHAVIOR", actions[behavior])); candidate.chmod(0o755)
    extension = tmp_path / "libibmw_dds_transport.so"; extension.write_bytes(b"extension")
    providers = {}
    for family in ("dds", "cdr"):
        path = tmp_path / f"libfast{family}.so"; path.write_bytes(family.encode())
        identity = rollback._file_identity(path); providers[family] = identity
    providers["library_paths"] = [str(tmp_path)]
    logs = tmp_path / "logs"; logs.mkdir()
    return candidate, extension, providers, logs


def _runtime_records(candidate, extension, provider, *, omit_provider=None, alternate=False):
    def record(path, source):
        identity = rollback._file_identity(path)
        return {"source": source, "resolved_path": str(path), **identity}
    records = [record(candidate, "exe"), record(extension, "maps")]
    records.extend(record(Path(provider[family]["path"]), "maps") for family in ("dds", "cdr")
                   if family != omit_provider)
    if alternate:
        records.append({"source": "maps", "resolved_path": "/tmp/libfastdds.so.alternate",
                        "device": 0, "inode": 0, "sha256": "alternate"})
    return records


@pytest.mark.parametrize("failure,error", [
    ("wrong_provider", "provider mapping does not match"),
    ("alternate", "unexpected alternate DDS provider"),
])
def test_candidate_rejects_wrong_provider_or_alternate_map(tmp_path, monkeypatch, failure, error):
    candidate, extension, provider, logs = _candidate_fixture(tmp_path)
    observations = _runtime_records(candidate, extension, provider,
                                    omit_provider="dds" if failure == "wrong_provider" else None,
                                    alternate=failure == "alternate")
    monkeypatch.setattr(rollback, "_runtime_observations", lambda pid, label: observations)
    with pytest.raises(RuntimeError, match=error):
        rollback._execute_candidate(candidate, extension, tmp_path, provider, logs)


def test_candidate_rejects_nonzero_exit(tmp_path, monkeypatch):
    candidate, extension, provider, logs = _candidate_fixture(tmp_path, "nonzero")
    monkeypatch.setattr(rollback, "_runtime_observations",
                        lambda pid, label: _runtime_records(candidate, extension, provider))
    with pytest.raises(RuntimeError, match="exited with status 7"):
        rollback._execute_candidate(candidate, extension, tmp_path, provider, logs)


def test_candidate_handshake_timeout_cleans_process_group(tmp_path, monkeypatch):
    candidate, extension, provider, logs = _candidate_fixture(tmp_path, "timeout")
    monkeypatch.setattr(rollback, "_HANDSHAKE_TIMEOUT", 0.05)
    real_killpg = os.killpg
    killed = []

    def tracking_killpg(pid, sig):
        killed.append((pid, sig)); real_killpg(pid, sig)

    monkeypatch.setattr(rollback.os, "killpg", tracking_killpg)
    with pytest.raises(RuntimeError, match="ready byte R"):
        rollback._execute_candidate(candidate, extension, tmp_path, provider, logs)
    assert killed and killed[0][1] == signal.SIGKILL
    with pytest.raises(ProcessLookupError):
        os.kill(killed[0][0], 0)


def test_existing_work_root_fails_without_deleting_it(tmp_path):
    work = tmp_path / "existing"; work.mkdir(); marker = work / "marker"; marker.write_text("keep")
    with pytest.raises(RuntimeError, match="must not exist"):
        rollback.execute_trace_off_build(tmp_path, tmp_path, work)
    assert marker.read_text() == "keep"


def test_anchor_validation_failure_cleans_work_root(tmp_path):
    source = tmp_path / "source"; source.mkdir()
    anchor = tmp_path / "anchor"; anchor.mkdir()
    (anchor / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\nIBMW_ENABLE_COMM_TRACE:BOOL=OFF\n")
    work = tmp_path / "work"
    with pytest.raises(RuntimeError, match="must be ON"):
        rollback.execute_trace_off_build(source, anchor, work)
    assert not work.exists()


@pytest.mark.skipif(os.environ.get("MW_PERF_RUN_ROLLBACK_INTEGRATION") != "1",
                    reason="set MW_PERF_RUN_ROLLBACK_INTEGRATION=1 to run the real rollback build")
def test_execute_trace_off_build_integration(tmp_path):
    source = Path(__file__).resolve().parents[3]
    anchor = source / ".opencode" / "comm-trace-on"
    result = rollback.execute_trace_off_build(source, anchor, tmp_path / "rollback")
    assert result["selected_options"]["IBMW_ENABLE_COMM_TRACE"] == "OFF"
    assert set(result["delta"]) == {
        "IBMW_ENABLE_COMM_TRACE", "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"}
    assert result["runtime"]["returncode"] == 0
    assert result["runtime"]["executable_matches"]
    assert result["runtime"]["extension_matches"]
    assert result["dependency_source_control"]["reuse_scope"] == "source_only"


def _packaging_result(tmp_path):
    paths = {}
    for name, data in (("extension", b"extension-image"), ("executable", b"executable-image"),
                       ("dds", b"dds-provider"), ("cdr", b"cdr-provider"),
                       ("cmake", b"cmake-tool"), ("compiler", b"compiler-tool")):
        path = tmp_path / name
        path.write_bytes(data)
        paths[name] = path
    digest = lambda name: hashlib.sha256(paths[name].read_bytes()).hexdigest()
    source = {"root": "/source", "commit": "1" * 40, "branch": "main", "dirty": True,
              "diff_sha256": "2" * 64, "untracked_tree_sha256": "3" * 64,
              "untracked_count": 1, "working_tree_sha256": "4" * 64}
    audits = {"nm_all": {"symbol_count": 8, "stdout_sha256": "5" * 64},
              "nm_dynamic": {"symbol_count": 3, "stdout_sha256": "6" * 64}}
    images = {name: {"path": str(paths[name]), "sha256": digest(name), "size": paths[name].stat().st_size,
                     "build_id": "ab" + ("1" if name == "extension" else "2"), "audits": audits,
                     "readelf_stdout_sha256": "7" * 64} for name in ("extension", "executable")}
    providers: dict[str, Any] = {name: {"path": str(paths[name]), "sha256": digest(name),
                        "size": paths[name].stat().st_size, "build_id": "cd" + name,
                        "version": "provider-1", "device": 1, "inode": 2}
                 for name in ("dds", "cdr")}
    providers["library_paths"] = [str(tmp_path)]
    tool = lambda name: {"path": str(paths[name]), "sha256": digest(name),
                         "size": paths[name].stat().st_size, "version": name + "-1",
                         "device": 1, "inode": 2}
    records = sorted(
        ({"source": source, "source_kind": "source",
          "output": f"src/CMakeFiles/{target[:-4]}.dir/main.o",
          "target": target[:-4], "command_sha256": "7" * 64}
         for source, target in rollback._REQUIRED_SOURCES.items()),
        key=lambda item: (item["source"], item["output"], item["target"] or ""))
    registered = [item for item in records if item["target"] in rollback._REGISTERED_TARGETS]
    other = [item for item in records if item["target"] not in rollback._REGISTERED_TARGETS]
    subset = lambda items: {"record_count": len(items), "content_sha256": hashlib.sha256(
        core._canonical_json_bytes(items)).hexdigest()}
    inventory = {"record_count": len(records), "records": records,
                 "sources": sorted({item["source"] for item in records}),
                 "content_sha256": hashlib.sha256(
                     core._canonical_json_bytes(records)).hexdigest(),
                 "registered_records": subset(registered),
                 "other_records": subset(other),
                 "claim": "trace sources and definitions are absent from the compile inventory"}
    def observation(name, source_kind):
        identity = rollback._file_identity(paths[name])
        return {"source": source_kind, "resolved_path": str(paths[name]), **identity}
    runtime_observations = [observation("executable", "exe"), observation("extension", "maps"),
                            observation("dds", "maps"), observation("cdr", "maps")]
    dependency_source_control = _staged_dependency_control(tmp_path / "dependency")
    link_plans = []
    for target, image_name in (("ibmw_dds_extension", "extension"),
                               ("ibmw_dds_trace_off_business_roundtrip", "executable")):
        inputs = [{"class": "fresh_object",
                   "identity": f"fresh:src/CMakeFiles/{target}.dir/main.o",
                   "size": 6, "sha256": hashlib.sha256(b"object").hexdigest()}]
        plan = {"target": target, "image": image_name,
                "plan": {"identity": f"fresh:src/CMakeFiles/{target}.dir/link.txt",
                         "size": 4, "sha256": hashlib.sha256(b"link").hexdigest()},
                "responses": [],
                "tokens": [{"kind": "tool", "value": digest("compiler")},
                           {"kind": "input", "value": 0},
                           {"kind": "flag", "value": "-o"},
                           {"kind": "output", "value": f"image:{image_name}"}],
                "inputs": inputs,
                "output": {"class": "fresh_output", "identity": f"image:{image_name}",
                           "size": paths[image_name].stat().st_size,
                           "sha256": digest(image_name)}}
        plan["closure_sha256"] = hashlib.sha256(core._canonical_json_bytes(plan)).hexdigest()
        link_plans.append(plan)
    link_closure = {"schema": "mw_perf.rollback_link_closure.v1", "plans": link_plans}
    link_closure["content_sha256"] = hashlib.sha256(
        core._canonical_json_bytes(link_closure)).hexdigest()
    return {"work_root": str(tmp_path), "build_dir": str(tmp_path),
            "source_identity": {"before": source, "after": dict(source)},
            "product_options": {
                "anchor": {"IBMW_ENABLE_COMM_TRACE": "ON",
                           "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE": "ON"},
                "fresh": {"IBMW_ENABLE_COMM_TRACE": "OFF",
                          "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE": "OFF"},
                "delta": {
                    "IBMW_ENABLE_COMM_TRACE": {"anchor": "ON", "fresh": "OFF"},
                    "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE": {
                        "anchor": "ON", "fresh": "OFF"}}},
            "delta": {
                "IBMW_ENABLE_COMM_TRACE": {"anchor": "ON", "fresh": "OFF"},
                "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE": {
                    "anchor": "ON", "fresh": "OFF"}},
            "evidence_control": {"name": "CMAKE_EXPORT_COMPILE_COMMANDS", "forced": "ON", "fresh": "ON"},
            "dependency_source_control": dependency_source_control,
            "compile_inventory": inventory, "link_closure": link_closure,
            "images": images, "provider_identity": providers,
            "runtime": {"pid": 123, "returncode": 0, "observations": runtime_observations,
                        "executable_matches": [runtime_observations[0]],
                        "extension_matches": [runtime_observations[1]],
                        "provider_matches": {"dds": [runtime_observations[2]], "cdr": [runtime_observations[3]]},
                        "stdout_sha256": "b" * 64, "stderr_sha256": "c" * 64},
            "logs": {"logs/build.stdout.log": "b" * 64},
            "tools": {"cmake": tool("cmake"), "active_toolchain": {"CMAKE_CXX_COMPILER": tool("compiler")}}}


def _anchor_metadata():
    _contract, trusted_raw = core._trusted_p0a_contract("mw_perf.p0a_contract.v8")
    return {"run_id": "anchor-run", "root_manifest_sha256": "c" * 64,
            "evidence_manifest_sha256": "d" * 64,
            "registration_contract": {"id": "mw_perf.p0a_contract.v8",
                                      "sha256": hashlib.sha256(trusted_raw).hexdigest()},
            "production_build_identity_sha256": "f" * 64}


def _package(tmp_path, monkeypatch, result=None):
    monkeypatch.setattr(rollback, "_anchor_binding", lambda _path: _anchor_metadata())
    value = result or _packaging_result(tmp_path)
    monkeypatch.setattr(rollback, "_revalidate_packaging", lambda claimed, _anchor, _logs: (
        claimed, {name: Path(claimed["images"][name]["path"]).read_bytes()
                  for name in rollback._IMAGE_NAMES}))
    packaged = rollback.build_trace_off_rollback_companion(
        value, tmp_path, tmp_path / "payload", tmp_path / "objects", "campaign", "companion")
    return value, packaged


def _assert_no_paths(value):
    if isinstance(value, dict):
        assert all("path" not in key.lower() and not key.lower().endswith("_dir") for key in value)
        for item in value.values():
            _assert_no_paths(item)
    elif isinstance(value, list):
        for item in value:
            _assert_no_paths(item)
    elif isinstance(value, str):
        assert not value.startswith("/")


@pytest.mark.parametrize("value", [
    {"nested": {"evil": "/abs/path"}},
    {"nested": {"foo_path": "logical"}},
    {"nested": {"evil": r"C:\\escape"}},
    {"nested": {"evil": r"relative\\escape"}},
])
def test_path_free_rejects_nested_unknown_path_authority(value):
    with pytest.raises(ValueError, match="path"):
        rollback._path_free(value)


def test_build_rollback_companion_success_closed_schema_path_free_and_valid_object(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)
    report, descriptor = packaged["report"], packaged["descriptor"]
    schema = json.loads((Path(rollback.__file__).parent / "schemas/rollback-trace-off-result.schema.v2.json").read_text())
    assert not core._schema_errors(report, schema)
    assert report["schema"] == "mw_perf.rollback_trace_off.v2"
    expected_delta = {
        "IBMW_ENABLE_COMM_TRACE": {"anchor": "ON", "fresh": "OFF"},
        "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE": {"anchor": "ON", "fresh": "OFF"},
    }
    assert report["product_delta"] == expected_delta
    assert "managed-candidate build ON-to-OFF" in report["claim"]
    assert "separate target" in report["claim"]
    assert "independent compile and final-link closure" in report["claim"]
    assert report["dependency_source_control"]["reuse_scope"] == "source_only"
    assert [item["name"] for item in report["dependency_source_control"]["dependencies"]] == [
        "backward", "googletest"]
    _assert_no_paths(report)
    payload = tmp_path / "payload"
    expected = {"rollback_trace_off.json", "build_plan.json", "compile_source_inventory.json",
                "build_identity.json", "symbol_audit.json", "runtime_resolution.json",
                "provider_identity.json", "link_closure.json", "images/extension", "images/executable",
                "rollback_evidence_manifest.json"}
    assert {str(path.relative_to(payload)) for path in payload.rglob("*") if path.is_file()} == expected
    evidence_raw = (payload / "rollback_evidence_manifest.json").read_bytes()
    evidence = json.loads(evidence_raw)
    assert set(evidence["expected_files"]) == expected - {"rollback_evidence_manifest.json"}
    report_raw = (payload / "rollback_trace_off.json").read_bytes()
    assert descriptor["root_manifest_sha256"] == hashlib.sha256(report_raw).hexdigest()
    assert descriptor["evidence_manifest_sha256"] == hashlib.sha256(evidence_raw).hexdigest()
    object_path = tmp_path / "objects/sha256" / descriptor["object_sha256"][:2] / (descriptor["object_sha256"] + ".tar")
    assert validate_companion_object(object_path, descriptor)["valid"]
    assert rollback.validate_trace_off_rollback_companion(object_path, descriptor)["valid"]


def test_build_rollback_companion_rejects_malformed_managed_candidate_delta(
        tmp_path, monkeypatch):
    result = _packaging_result(tmp_path)
    malformed = {"anchor": "ON", "fresh": "OFF", "unexpected": "value"}
    result["delta"]["IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"] = malformed
    result["product_options"]["delta"][
        "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"] = malformed
    monkeypatch.setattr(rollback, "_anchor_binding", lambda _path: _anchor_metadata())
    monkeypatch.setattr(rollback, "_revalidate_packaging", lambda claimed, _anchor, _logs: (
        claimed, {name: Path(claimed["images"][name]["path"]).read_bytes()
                  for name in rollback._IMAGE_NAMES}))

    with pytest.raises(ValueError, match="managed exact candidate delta is not ON-to-OFF"):
        rollback.build_trace_off_rollback_companion(
            result, tmp_path, tmp_path / "payload", tmp_path / "objects",
            "campaign", "companion")


def test_build_rollback_companion_accepts_captured_ninja_closure(tmp_path, monkeypatch):
    result = _packaging_result(tmp_path)
    build, ninja_anchor, ninja_images, ninja_providers, ninja_tool = _ninja_link_fixture(
        tmp_path / "ninja", response=True)
    closure = rollback.capture_link_closure(
        build, ninja_anchor, ninja_images, ninja_providers)

    for name in rollback._IMAGE_NAMES:
        path = Path(ninja_images[name]["path"])
        result["images"][name].update(
            {"path": str(path), "sha256": rollback._sha256(path), "size": path.stat().st_size})
    for family in ("dds", "cdr"):
        path = Path(ninja_providers[family]["path"])
        result["provider_identity"][family].update(
            {"path": str(path), "sha256": rollback._sha256(path), "size": path.stat().st_size})
    result["provider_identity"]["library_paths"] = ninja_providers["library_paths"]

    def observation(path, source):
        return {"source": source, "resolved_path": str(path), **rollback._file_identity(path)}

    runtime = result["runtime"]
    runtime["executable_matches"] = [observation(
        Path(ninja_images["executable"]["path"]), "exe")]
    runtime["extension_matches"] = [observation(
        Path(ninja_images["extension"]["path"]), "maps")]
    runtime["provider_matches"] = {
        family: [observation(Path(ninja_providers[family]["path"]), "maps")]
        for family in ("dds", "cdr")}
    runtime["observations"] = [runtime["executable_matches"][0],
                               runtime["extension_matches"][0],
                               *runtime["provider_matches"]["dds"],
                               *runtime["provider_matches"]["cdr"]]

    true_tool = rollback._file_identity(Path("/bin/true").resolve())
    true_tool["version"] = "true-test"
    ninja_identity = rollback._file_identity(ninja_tool)
    ninja_identity["version"] = "test-ninja 1"
    result["tools"]["active_toolchain"] = {
        "CMAKE_CXX_COMPILER": true_tool, "CMAKE_MAKE_PROGRAM": ninja_identity}
    result["link_closure"] = closure

    inventory = result["compile_inventory"]
    outputs = {plan["target"]: next(
        item["identity"].removeprefix("fresh:") for item in plan["inputs"]
        if item["class"] == "fresh_object") for plan in closure["plans"]}
    for record in inventory["records"]:
        if record["target"] in outputs:
            record["output"] = outputs[record["target"]]
    inventory["records"].sort(key=lambda item: (
        item["source"], item["output"], item["target"] or ""))
    inventory["sources"] = sorted({item["source"] for item in inventory["records"]})
    inventory["content_sha256"] = hashlib.sha256(
        core._canonical_json_bytes(inventory["records"])).hexdigest()
    for key, selected in (("registered_records", True), ("other_records", False)):
        records = [item for item in inventory["records"]
                   if (item["target"] in rollback._REGISTERED_TARGETS) is selected]
        inventory[key] = {"record_count": len(records), "content_sha256": hashlib.sha256(
            core._canonical_json_bytes(records)).hexdigest()}

    monkeypatch.setattr(rollback, "_anchor_binding", lambda _path: _anchor_metadata())
    monkeypatch.setattr(rollback, "_revalidate_packaging", lambda claimed, _anchor, _logs: (
        claimed, {name: Path(claimed["images"][name]["path"]).read_bytes()
                  for name in rollback._IMAGE_NAMES}))
    rollback._path_free(result["link_closure"])
    packaged = rollback.build_trace_off_rollback_companion(
        result, tmp_path, tmp_path / "payload", tmp_path / "objects", "campaign", "ninja")

    _assert_no_paths(json.loads((tmp_path / "payload/build_identity.json").read_text()))
    descriptor = packaged["descriptor"]
    object_path = (tmp_path / "objects/sha256" / descriptor["object_sha256"][:2]
                   / (descriptor["object_sha256"] + ".tar"))
    assert rollback.validate_trace_off_rollback_companion(object_path, descriptor)["valid"]


def test_validate_rollback_companion_requires_expected_descriptor(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)
    descriptor = packaged["descriptor"]
    object_path = (tmp_path / "objects/sha256" / descriptor["object_sha256"][:2]
                   / (descriptor["object_sha256"] + ".tar"))

    assert rollback.validate_trace_off_rollback_companion(object_path) == {
        "valid": False,
        "failure_codes": ["ROLLBACK_EXPECTED_DESCRIPTOR"],
        "descriptor": None,
    }


def test_build_rollback_companion_rejects_report_rewrite(tmp_path, monkeypatch):
    result, _ = _package(tmp_path, monkeypatch)
    (tmp_path / "payload/rollback_trace_off.json").write_text("{}\n")
    with pytest.raises(ValueError, match="must not already exist"):
        rollback.build_trace_off_rollback_companion(result, tmp_path, tmp_path / "payload",
                                                    tmp_path / "objects", "campaign", "companion")


@pytest.mark.parametrize("mutation,error", [
    ("bad_delta", "not limited"), ("source_drift", "source identity"),
    ("symbol_zero", "symbol audit"), ("runtime_mismatch", "runtime image identity"),
    ("provider_mismatch", "provider runtime identity"),
])
def test_build_rollback_companion_rejects_bad_runner_semantics(tmp_path, monkeypatch, mutation, error):
    result = _packaging_result(tmp_path)
    if mutation == "bad_delta":
        result["delta"]["OTHER"] = {"anchor": "ON", "fresh": "OFF"}
    elif mutation == "source_drift":
        result["source_identity"]["after"]["commit"] = "0" * 40
    elif mutation == "symbol_zero":
        result["images"]["extension"]["audits"]["nm_all"]["symbol_count"] = 0
    elif mutation == "runtime_mismatch":
        result["runtime"]["extension_matches"][0]["sha256"] = "0" * 64
    else:
        result["runtime"]["provider_matches"]["dds"][0]["sha256"] = "0" * 64
    monkeypatch.setattr(rollback, "_anchor_binding", lambda _path: _anchor_metadata())
    monkeypatch.setattr(rollback, "_revalidate_packaging", lambda claimed, _anchor, _logs: (
        claimed, {name: Path(claimed["images"][name]["path"]).read_bytes()
                  for name in rollback._IMAGE_NAMES}))
    with pytest.raises(ValueError, match=error):
        rollback.build_trace_off_rollback_companion(result, tmp_path, tmp_path / "payload",
                                                    tmp_path / "objects", "campaign", "companion")


def test_build_rollback_companion_rejects_image_tamper(tmp_path, monkeypatch):
    result = _packaging_result(tmp_path)
    Path(result["images"]["extension"]["path"]).write_bytes(b"tampered")
    monkeypatch.setattr(rollback, "_anchor_binding", lambda _path: _anchor_metadata())
    monkeypatch.setattr(rollback, "_revalidate_packaging", lambda claimed, _anchor, _logs: (
        claimed, {name: Path(claimed["images"][name]["path"]).read_bytes()
                  for name in rollback._IMAGE_NAMES}))
    with pytest.raises(ValueError, match="hash does not match"):
        rollback.build_trace_off_rollback_companion(result, tmp_path, tmp_path / "payload",
                                                    tmp_path / "objects", "campaign", "companion")


def test_build_rollback_companion_rejects_bad_anchor(tmp_path):
    result = _packaging_result(tmp_path)
    with pytest.raises(ValueError, match="anchor v8 evidence is unavailable"):
        rollback.build_trace_off_rollback_companion(result, tmp_path, tmp_path / "payload",
                                                    tmp_path / "objects", "campaign", "companion")


def _anchor_fixture(tmp_path, monkeypatch):
    anchor = tmp_path / "anchor"
    (anchor / "capability").mkdir(parents=True)
    (anchor / "registration").mkdir()
    contract_version = rollback._ROLLBACK_CONTRACT
    _contract, contract_raw = core._trusted_p0a_contract(contract_version)
    run_id = "anchor-run"
    manifest = {"run_id": run_id, "run_status": "completed", "candidate_only": True,
                "metric_availability": "unsupported", "measurement_kind": "exact_correlated",
                "dds_comm_trace": {"status": "exact_correlated",
                                   "registration_contract_version": contract_version}}
    evidence = {"run_id": run_id, "registration_contract_version": contract_version,
                "registration_contract_sha256": hashlib.sha256(contract_raw).hexdigest()}
    validation = {"valid_for_collection": True, "valid_for_metric": False,
                  "valid_for_comparison": False,
                  "p0a_evidence": {"valid": False, "failure_codes": ["P0A_ARTIFACT"]}}
    declarations = [
        {"role": "extension", "name": "extension", "source": "maps",
         "expected_path": "/extension", "sha256": "1" * 64,
         "expected_device": 1, "expected_inode": 2, "expected_size": 3},
        {"role": "sut", "name": "candidate", "source": "exe",
         "expected_path": "/candidate", "sha256": "2" * 64,
         "expected_device": 4, "expected_inode": 5, "expected_size": 6},
    ]
    declaration_hash = hashlib.sha256(core._canonical_json_bytes(declarations)).hexdigest()
    resolution = {"schema": "mw_perf.p0a.resolution_declarations.v1", "run_id": run_id,
                  "registration_contract_version": contract_version,
                  "declaration_sha256": declaration_hash, "declarations": declarations}
    trusted_resolution = {"schema": "mw_perf.p0a.resolution.v2", "run_id": run_id,
                          "resolved": True, "conclusion": "pass",
                          "declaration_sha256": declaration_hash, "declarations": declarations}
    values = {"manifest.json": manifest, "capability/p0a_evidence_manifest.json": evidence,
              "validation.json": validation,
              "registration/resolution_declarations.json": resolution,
              "capability/resolution.json": trusted_resolution}
    for relative, value in values.items():
        (anchor / relative).write_bytes(core._canonical_json_bytes(value, newline=True))
    (anchor / "registration/contract.json").write_bytes(contract_raw)
    monkeypatch.setattr(core, "validate_dir", lambda _anchor, profile: validation)
    return anchor


@pytest.mark.parametrize("relative,field,value", [
    ("capability/p0a_evidence_manifest.json", "run_id", "other-run"),
    ("capability/p0a_evidence_manifest.json", "registration_contract_sha256", "0" * 64),
    ("registration/resolution_declarations.json", "registration_contract_version", "wrong"),
    ("registration/resolution_declarations.json", "run_id", "other-run"),
    ("capability/resolution.json", "run_id", "other-run"),
])
def test_anchor_binding_rejects_explicit_cross_file_mismatch(
        tmp_path, monkeypatch, relative, field, value):
    anchor = _anchor_fixture(tmp_path, monkeypatch)
    path = anchor / relative
    document = json.loads(path.read_bytes())
    document[field] = value
    path.write_bytes(core._canonical_json_bytes(document, newline=True))
    with pytest.raises(ValueError):
        rollback._anchor_binding(anchor)


def test_anchor_binding_rejects_root_evidence_contract_version_mismatch(tmp_path, monkeypatch):
    anchor = _anchor_fixture(tmp_path, monkeypatch)
    path = anchor / "manifest.json"
    manifest = json.loads(path.read_bytes())
    manifest["dds_comm_trace"]["registration_contract_version"] = "wrong"
    path.write_bytes(core._canonical_json_bytes(manifest, newline=True))
    with pytest.raises(ValueError):
        rollback._anchor_binding(anchor)


def test_regular_unique_rejects_symlinked_parent_escape(tmp_path):
    root = tmp_path / "build"
    outside = tmp_path / "outside"
    root.mkdir(); outside.mkdir()
    (outside / "ibmw_dds_trace_off_business_roundtrip").write_bytes(b"outside")
    (root / "escaped").symlink_to(outside, target_is_directory=True)
    with pytest.raises(RuntimeError, match="symlink|escapes"):
        rollback._regular_unique(root, "ibmw_dds_trace_off_business_roundtrip")


def test_anchor_binding_uses_immutable_reads_for_racing_input(tmp_path, monkeypatch):
    anchor = _anchor_fixture(tmp_path, monkeypatch)
    manifest = anchor / "manifest.json"
    original_read = rollback.os.read
    replaced = False
    original_inode = manifest.stat().st_ino

    def racing_read(fd, count):
        nonlocal replaced
        if not replaced and rollback.os.fstat(fd).st_ino == original_inode:
            replaced = True
            replacement = manifest.with_name("manifest.replacement")
            manifest.rename(replacement)
            manifest.write_bytes(replacement.read_bytes())
        return original_read(fd, count)

    monkeypatch.setattr(rollback.os, "read", racing_read)
    with pytest.raises(ValueError, match="anchor v8 evidence is unavailable"):
        rollback._anchor_binding(anchor)


def test_anchor_binding_rejects_symlinked_input_parent(tmp_path, monkeypatch):
    anchor = _anchor_fixture(tmp_path, monkeypatch)
    capability = anchor / "capability"
    outside = tmp_path / "outside-capability"
    outside.mkdir()
    for item in capability.iterdir():
        item.rename(outside / item.name)
    capability.rmdir()
    capability.symlink_to(outside, target_is_directory=True)
    with pytest.raises(ValueError, match="anchor v8 evidence is unavailable"):
        rollback._anchor_binding(anchor)


def test_required_runtime_identity_ignores_incidental_observation_drift(tmp_path):
    result = _packaging_result(tmp_path)["runtime"]
    fresh = json.loads(json.dumps(result))
    fresh["pid"] = 999
    for item in fresh["observations"] + fresh["executable_matches"] + fresh["extension_matches"]:
        item["device"] += 10
        item["inode"] += 10
        item["mapping"] = {"permissions": "r-xp"}
    assert rollback._required_runtime_hashes(result) == rollback._required_runtime_hashes(fresh)
    fresh["extension_matches"][0]["sha256"] = "0" * 64
    assert rollback._required_runtime_hashes(result) != rollback._required_runtime_hashes(fresh)


def _rebuild_mutated_package(tmp_path, descriptor, mutate):
    payload = tmp_path / "payload"
    mutate(payload)
    evidence_path = payload / "rollback_evidence_manifest.json"
    evidence = json.loads(evidence_path.read_bytes())
    for relative in rollback._ROLLBACK_PAYLOAD_ROLES:
        data = (payload / relative).read_bytes()
        evidence["expected_files"][relative]["sha256"] = hashlib.sha256(data).hexdigest()
        evidence["expected_files"][relative]["size"] = len(data)
    evidence_path.write_bytes(core._canonical_json_bytes(evidence, newline=True))
    report_raw = (payload / "rollback_trace_off.json").read_bytes()
    metadata = {name: descriptor[name] for name in ("campaign_id", "role", "companion_id", "run_id",
                                                     "evidence_contract", "build_identity_sha256")}
    metadata["root_manifest_sha256"] = hashlib.sha256(report_raw).hexdigest()
    metadata["evidence_manifest_sha256"] = hashlib.sha256(evidence_path.read_bytes()).hexdigest()
    snapshot = rollback.capture_payload_snapshot(payload)
    new_descriptor = rollback.build_companion_object_from_snapshot(
        snapshot, tmp_path / "mutated-objects", metadata)
    object_path = (tmp_path / "mutated-objects/sha256" / new_descriptor["object_sha256"][:2]
                   / (new_descriptor["object_sha256"] + ".tar"))
    return object_path, new_descriptor


@pytest.mark.parametrize("document,operation", [
    ("report", "remove"),
    ("identity", "remove"),
    ("report", "add"),
    ("identity", "add"),
])
def test_tar_rejects_coherently_repackaged_product_delta_mismatch(
        tmp_path, monkeypatch, document, operation):
    result = _packaging_result(tmp_path)
    candidate = "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"
    if operation == "add":
        result["delta"].pop(candidate)
        for controls in result["product_options"].values():
            controls.pop(candidate)
    _, packaged = _package(tmp_path, monkeypatch, result)
    descriptor = dict(packaged["descriptor"])

    def mutate(payload):
        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        delta = (report["product_delta"] if document == "report"
                 else identity["product"]["delta"])
        if operation == "remove":
            delta.pop(candidate)
        else:
            delta[candidate] = {"anchor": "ON", "fresh": "OFF"}

        if document == "identity":
            identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
            build_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()
            descriptor["build_identity_sha256"] = build_hash
            report["rollback_build_identity_sha256"] = build_hash
            plan_path = payload / "build_plan.json"
            plan = json.loads(plan_path.read_bytes())
            plan["build_identity_sha256"] = build_hash
            plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))

    object_path, new_descriptor = _rebuild_mutated_package(
        tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    assert rollback.validate_trace_off_rollback_companion(
        object_path, new_descriptor)["failure_codes"] == ["ROLLBACK_OBJECT_SEMANTICS"]


@pytest.mark.parametrize("mutation", ["extra", "absolute", "dotdot"])
def test_tar_rejects_canonical_build_plan_mutation_with_updated_manifests(
        tmp_path, monkeypatch, mutation):
    _, packaged = _package(tmp_path, monkeypatch)

    def mutate(payload):
        path = payload / "build_plan.json"
        plan = json.loads(path.read_bytes())
        if mutation == "extra":
            plan["arbitrary"] = "canonical rewrite"
        elif mutation == "absolute":
            plan["compile"]["records"][0]["source"] = "/host/source.cc"
        else:
            plan["compile"]["records"][0]["output"] = "obj/../escape.o"
        path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    object_path, descriptor = _rebuild_mutated_package(tmp_path, packaged["descriptor"], mutate)
    assert validate_companion_object(object_path, descriptor)["valid"]
    assert not rollback.validate_trace_off_rollback_companion(object_path, descriptor)["valid"]


def test_tar_rejects_forged_anchor_trusted_hash_with_updated_manifests(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)
    forged = "0" * 64

    def mutate(payload):
        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        report["anchor"]["registration_contract"]["sha256"] = forged
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))
        plan_path = payload / "build_plan.json"
        plan = json.loads(plan_path.read_bytes())
        plan["anchor"]["contract_sha256"] = forged
        plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    descriptor = dict(packaged["descriptor"])
    descriptor["evidence_contract"] = {"id": rollback._ROLLBACK_CONTRACT, "sha256": forged}
    object_path, new_descriptor = _rebuild_mutated_package(tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    assert not rollback.validate_trace_off_rollback_companion(object_path, new_descriptor)["valid"]


def test_tar_rejects_coherent_build_plan_compile_rewrite(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)

    def mutate(payload):
        path = payload / "build_plan.json"
        plan = json.loads(path.read_bytes())
        forged_records = json.loads(json.dumps(plan["compile"]["records"]))
        forged_records[0]["source"] = "forged/driver.cc"
        forged_records[0]["output"] = "forged/driver.o"
        plan["compile"] = {"records": forged_records,
            "record_count": 2, "content_sha256": "0" * 64}
        path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    object_path, descriptor = _rebuild_mutated_package(
        tmp_path, packaged["descriptor"], mutate)
    assert validate_companion_object(object_path, descriptor)["valid"]
    assert not rollback.validate_trace_off_rollback_companion(object_path, descriptor)["valid"]


@pytest.mark.parametrize("digest_mutation", ["stale", "forged"])
def test_tar_rejects_inventory_rewrite_with_incorrect_archive_derivable_digest(
        tmp_path, monkeypatch, digest_mutation):
    _, packaged = _package(tmp_path, monkeypatch)
    descriptor = dict(packaged["descriptor"])

    def mutate(payload):
        inventory_path = payload / "compile_source_inventory.json"
        inventory = json.loads(inventory_path.read_bytes())
        inventory["records"][0]["output"] = "forged/output.o"
        registered = [item for item in inventory["records"]
                      if item["target"] in rollback._REGISTERED_TARGETS]
        other = [item for item in inventory["records"]
                 if item["target"] not in rollback._REGISTERED_TARGETS]
        inventory["registered_records"] = {
            "record_count": len(registered),
            "content_sha256": hashlib.sha256(
                core._canonical_json_bytes(registered)).hexdigest(),
        }
        inventory["other_records"] = {
            "record_count": len(other),
            "content_sha256": hashlib.sha256(
                core._canonical_json_bytes(other)).hexdigest(),
        }
        if digest_mutation == "forged":
            inventory["content_sha256"] = "0" * 64
        inventory_path.write_bytes(core._canonical_json_bytes(inventory, newline=True))

        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        identity["compile"] = inventory
        identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
        build_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()
        descriptor["build_identity_sha256"] = build_hash

        plan_path = payload / "build_plan.json"
        plan = json.loads(plan_path.read_bytes())
        plan["compile"] = {name: inventory[name] for name in (
            "records", "record_count", "content_sha256")}
        plan["build_identity_sha256"] = build_hash
        plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))

        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        report["compile_out"] = {name: inventory[name] for name in (
            "record_count", "content_sha256", "registered_records", "other_records", "claim")}
        report["rollback_build_identity_sha256"] = build_hash
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))

    object_path, new_descriptor = _rebuild_mutated_package(
        tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    result = rollback.validate_trace_off_rollback_companion(object_path, new_descriptor)
    assert result["failure_codes"] == ["ROLLBACK_OBJECT_SEMANTICS"]


def test_tar_rejects_coherently_rehashed_build_identity_rewrite(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)

    def mutate(payload):
        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        identity["source"]["commit"] = "0" * 40
        identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
        forged_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()

        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        report["source"] = identity["source"]
        report["rollback_build_identity_sha256"] = forged_hash
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))

        plan_path = payload / "build_plan.json"
        plan = json.loads(plan_path.read_bytes())
        plan["build_identity_sha256"] = forged_hash
        plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    object_path, descriptor = _rebuild_mutated_package(
        tmp_path, packaged["descriptor"], mutate)
    assert validate_companion_object(object_path, descriptor)["valid"]
    assert not rollback.validate_trace_off_rollback_companion(object_path, descriptor)["valid"]


def test_tar_rejects_independent_runtime_observation_rewrite(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)

    def mutate(payload):
        runtime_path = payload / "runtime_resolution.json"
        runtime = json.loads(runtime_path.read_bytes())
        runtime["observations_count"] -= 1
        runtime["observations_sha256"] = "0" * 64
        runtime_path.write_bytes(core._canonical_json_bytes(runtime, newline=True))

        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        report["runtime_summary"] = runtime
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))

    object_path, descriptor = _rebuild_mutated_package(
        tmp_path, packaged["descriptor"], mutate)
    assert validate_companion_object(object_path, descriptor)["valid"]
    assert not rollback.validate_trace_off_rollback_companion(object_path, descriptor)["valid"]


def test_tar_rejects_coherently_rehashed_invented_link_object(tmp_path, monkeypatch):
    _, packaged = _package(tmp_path, monkeypatch)
    descriptor = dict(packaged["descriptor"])

    def mutate(payload):
        closure_path = payload / "link_closure.json"
        closure = json.loads(closure_path.read_bytes())
        plan = closure["plans"][0]
        object_input = next(item for item in plan["inputs"] if item["class"] == "fresh_object")
        object_input["identity"] = (
            "fresh:src/CMakeFiles/ibmw_dds_extension.dir/invented.o")
        plan_without_hash = {name: item for name, item in plan.items()
                             if name != "closure_sha256"}
        plan["closure_sha256"] = hashlib.sha256(
            core._canonical_json_bytes(plan_without_hash)).hexdigest()
        closure["content_sha256"] = hashlib.sha256(core._canonical_json_bytes({
            "schema": closure["schema"], "plans": closure["plans"]})).hexdigest()
        closure_path.write_bytes(core._canonical_json_bytes(closure, newline=True))
        summary = rollback._link_closure_summary(closure)

        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        identity["link_closure"] = closure
        identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
        build_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()
        descriptor["build_identity_sha256"] = build_hash

        plan_path = payload / "build_plan.json"
        build_plan = json.loads(plan_path.read_bytes())
        build_plan["link_closure"] = summary
        build_plan["build_identity_sha256"] = build_hash
        plan_path.write_bytes(core._canonical_json_bytes(build_plan, newline=True))

        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        report["link_closure"] = summary
        report["rollback_build_identity_sha256"] = build_hash
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))

    object_path, new_descriptor = _rebuild_mutated_package(
        tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    result = rollback.validate_trace_off_rollback_companion(object_path, new_descriptor)
    assert result["failure_codes"] == ["ROLLBACK_OBJECT_SEMANTICS"]


def _rehash_dependency_summary(summary):
    records = [{key: dependency[key] for key in (
        "name", "override_variable", "cache_bindings", "file_count",
        "directory_count", "byte_count", "tree_sha256")}
        for dependency in summary["dependencies"]]
    aggregate = hashlib.sha256(core._canonical_json_bytes(records)).hexdigest()
    summary.update({
        "dependency_count": len(records),
        "file_count": sum(item["file_count"] for item in records),
        "directory_count": sum(item["directory_count"] for item in records),
        "byte_count": sum(item["byte_count"] for item in records),
        "aggregate_sha256": aggregate,
        "checkpoint_digests": {name: aggregate for name in rollback._DEPENDENCY_CHECKPOINTS},
    })


@pytest.mark.parametrize("mutation", ["files", "directories", "bytes", "escape"])
def test_v2_dependency_summary_rejects_coherent_resource_or_path_tamper(tmp_path, mutation):
    summary = rollback._portable_dependency_source(_staged_dependency_control(tmp_path))
    if mutation == "files":
        for dependency in summary["dependencies"]:
            dependency["file_count"] = rollback._MAX_DEPENDENCY_FILES // 2 + 1
    elif mutation == "directories":
        for dependency in summary["dependencies"]:
            dependency["directory_count"] = rollback._MAX_DEPENDENCY_DIRECTORIES // 2 + 1
    elif mutation == "bytes":
        for dependency in summary["dependencies"]:
            dependency["byte_count"] = rollback._MAX_DEPENDENCY_TOTAL_BYTES // 2 + 1
    else:
        summary["dependencies"][0]["cache_bindings"] = [
            {"variable": "forged_SOURCE_DIR", "relative": "../escape"}]
    _rehash_dependency_summary(summary)
    with pytest.raises(ValueError, match="resource limits|bindings"):
        rollback._validate_v2_dependency_summary(summary)


@pytest.mark.parametrize("mutation", ["files", "bytes", "escape"])
def test_v2_tar_rejects_coherently_rehashed_dependency_limit_or_escape(
        tmp_path, monkeypatch, mutation):
    _, packaged = _package(tmp_path, monkeypatch)
    descriptor = dict(packaged["descriptor"])

    def mutate(payload):
        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        summary = report["dependency_source_control"]
        if mutation == "files":
            for dependency in summary["dependencies"]:
                dependency["file_count"] = rollback._MAX_DEPENDENCY_FILES // 2 + 1
        elif mutation == "bytes":
            for dependency in summary["dependencies"]:
                dependency["byte_count"] = rollback._MAX_DEPENDENCY_TOTAL_BYTES // 2 + 1
        else:
            summary["dependencies"][0]["cache_bindings"] = [
                {"variable": "forged_SOURCE_DIR", "relative": "../escape"}]
        _rehash_dependency_summary(summary)

        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        identity["dependency_source_control"] = summary
        identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
        build_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()
        descriptor["build_identity_sha256"] = build_hash
        report["rollback_build_identity_sha256"] = build_hash
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))
        plan_path = payload / "build_plan.json"
        plan = json.loads(plan_path.read_bytes())
        plan["build_identity_sha256"] = build_hash
        plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    object_path, new_descriptor = _rebuild_mutated_package(
        tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    assert not rollback.validate_trace_off_rollback_companion(
        object_path, new_descriptor)["valid"]


@pytest.mark.parametrize("field", ["dependency_count", "file_count", "directory_count",
                                    "byte_count", "aggregate_sha256", "checkpoint"])
def test_v2_tar_rejects_coherently_rehashed_dependency_summary_forgery(
        tmp_path, monkeypatch, field):
    _, packaged = _package(tmp_path, monkeypatch)
    descriptor = dict(packaged["descriptor"])

    def mutate(payload):
        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        dependency = report["dependency_source_control"]
        if field == "checkpoint":
            dependency["checkpoint_digests"]["staged_after_build"] = "0" * 64
        elif field.endswith("sha256"):
            dependency[field] = "0" * 64
        else:
            dependency[field] += 1

        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        identity["dependency_source_control"] = dependency
        identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
        build_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()
        descriptor["build_identity_sha256"] = build_hash

        report["rollback_build_identity_sha256"] = build_hash
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))
        plan_path = payload / "build_plan.json"
        plan = json.loads(plan_path.read_bytes())
        plan["build_identity_sha256"] = build_hash
        plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    object_path, new_descriptor = _rebuild_mutated_package(
        tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    assert rollback.validate_trace_off_rollback_companion(
        object_path, new_descriptor)["failure_codes"] == ["ROLLBACK_OBJECT_SEMANTICS"]


def test_validator_preserves_v1_archive_semantics(tmp_path, monkeypatch):
    result = _packaging_result(tmp_path)
    candidate = "IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"
    result["delta"].pop(candidate)
    for controls in result["product_options"].values():
        controls.pop(candidate)
    _, packaged = _package(tmp_path, monkeypatch, result)
    descriptor = dict(packaged["descriptor"])

    def mutate(payload):
        report_path = payload / "rollback_trace_off.json"
        report = json.loads(report_path.read_bytes())
        current = report["dependency_source_control"]
        legacy = {
            "cache_variable": "googletest_LOCAL_SOURCE",
            "provider": "anchor_external_dependency_source",
            "reuse_scope": "source_only",
            "cache_verified": True,
            "mutation_checkpoints": ["before_configure", "after_configure", "after_build"],
            "source_inventory": {name: current[name] for name in (
                "file_count", "directory_count", "aggregate_sha256")},
        }
        legacy["source_inventory"]["tree_sha256"] = legacy["source_inventory"].pop(
            "aggregate_sha256")
        report["schema"] = "mw_perf.rollback_trace_off.v1"
        report["dependency_source_control"] = legacy

        identity_path = payload / "build_identity.json"
        identity = json.loads(identity_path.read_bytes())
        identity["dependency_source_control"] = legacy
        identity_path.write_bytes(core._canonical_json_bytes(identity, newline=True))
        build_hash = hashlib.sha256(core._canonical_json_bytes(identity)).hexdigest()
        descriptor["build_identity_sha256"] = build_hash
        report["rollback_build_identity_sha256"] = build_hash
        report_path.write_bytes(core._canonical_json_bytes(report, newline=True))

        plan_path = payload / "build_plan.json"
        plan = json.loads(plan_path.read_bytes())
        plan["build_identity_sha256"] = build_hash
        plan_path.write_bytes(core._canonical_json_bytes(plan, newline=True))

    object_path, new_descriptor = _rebuild_mutated_package(
        tmp_path, descriptor, mutate)
    assert validate_companion_object(object_path, new_descriptor)["valid"]
    assert rollback.validate_trace_off_rollback_companion(object_path, new_descriptor)["valid"]
