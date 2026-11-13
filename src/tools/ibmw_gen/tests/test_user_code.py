#!/usr/bin/env python3
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# HYG-031: optional per-module `user_code:` snippets replace the default
# TODO body of a user-code region; without `user_code` nothing changes.

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
REPO = ROOT.parents[2]
IBMW_GEN = ROOT / "ibmw_gen.py"
SAMPLES = REPO / "src" / "samples" / "cpp"
TEMPLATES = REPO / "skills" / "migration" / "templates"


def _load(path: pathlib.Path) -> dict:
    data = yaml.safe_load(path.read_text())
    for mod in data["modules"]:
        mod.pop("user_code", None)
    return data


def _generate(data: dict):
    with tempfile.TemporaryDirectory() as tmp:
        yaml_path = pathlib.Path(tmp) / "case.ibmw.yaml"
        yaml_path.write_text(yaml.safe_dump(data, sort_keys=False))
        out = pathlib.Path(tmp) / "out"
        result = subprocess.run([sys.executable, str(IBMW_GEN), str(yaml_path), "-o", str(out)],
                                capture_output=True, text=True, timeout=120)
        files = {}
        if out.exists():
            for path in out.rglob("*"):
                if path.is_file() and path.name != yaml_path.name:
                    files[str(path.relative_to(out))] = path.read_text()
        return result, files


def _with_user_code(data: dict, module: str, user_code: dict) -> dict:
    for mod in data["modules"]:
        if mod["name"] == module:
            mod["user_code"] = user_code
            return data
    raise AssertionError(f"module {module} not in yaml")


# (yaml, module, region, begin-anchor substring, end-anchor substring, snippet)
CASES = [
    (SAMPLES / "dds_native_demo" / "dds_native_demo.ibmw.yaml", "PublisherModule",
     "publish_body", "user code begin (publish_body)", "user code end (publish_body)",
     'msg.id(msg_count_);\nmsg.message("hello");\nmsg_count_++;'),
    (SAMPLES / "dds_native_demo" / "dds_native_demo.ibmw.yaml", "SubscriberModule",
     "on_message_body", "user code begin (on_message_body)", "user code end (on_message_body)",
     'MW_INFO("Received id={}", msg->id());'),
    (SAMPLES / "ros2_chn" / "ros2_chn.ibmw.yaml", "NormalPublisherModule",
     "publish_body", "user code begin (publish_body)", "user code end (publish_body)",
     "msg.data = \"x\";\nmsg_count_++;"),
    (SAMPLES / "ros2_chn" / "ros2_chn.ibmw.yaml", "NormalSubscriberModule",
     "on_message_body", "user code begin (on_message_body)", "user code end (on_message_body)",
     'MW_INFO("got {}", msg->data);'),
    (TEMPLATES / "json_local_pubsub.ibmw.yaml", "PublisherModule",
     "publish_body", "user code begin (publish_body)", "user code end (publish_body)",
     'msg.data = "json";\nmsg_count_++;'),
    (TEMPLATES / "json_local_pubsub.ibmw.yaml", "SubscriberModule",
     "on_message_body", "user code begin (on_message_body)", "user code end (on_message_body)",
     'MW_INFO("got {}", msg->data);'),
    (SAMPLES / "dds_rpc_demo" / "dds_rpc_demo.ibmw.yaml", "RpcServerModule",
     "rpc_server_handler_body.AddTwoInts", "IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_BEGIN",
     "IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_END",
     "rsp.sum(req.a() + req.b());\nreturn ibmw::rpc::Status();"),
    (SAMPLES / "dds_rpc_demo" / "dds_rpc_demo.ibmw.yaml", "RpcClientModule",
     "rpc_client_request_parameter_build.AddTwoInts",
     "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ADD_TWO_INTS_BEGIN",
     "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ADD_TWO_INTS_END",
     "req.a(counter);\nreq.b(2);"),
    (SAMPLES / "dds_rpc_demo" / "dds_rpc_demo.ibmw.yaml", "RpcClientModule",
     "rpc_client_response_parameter_consume.AddTwoInts",
     "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_ADD_TWO_INTS_BEGIN",
     "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_ADD_TWO_INTS_END",
     'MW_INFO("sum={}", rsp.sum());'),
    (SAMPLES / "ros2_rpc" / "ros2_rpc.ibmw.yaml", "RpcServerModule",
     "rpc_server_handler_body.RosTestRpc", "IBMW_RPC_SERVER_HANDLER_BODY_ROS_TEST_RPC_BEGIN",
     "IBMW_RPC_SERVER_HANDLER_BODY_ROS_TEST_RPC_END",
     "rsp.code = 0;\nreturn ibmw::rpc::Status();"),
    (SAMPLES / "ros2_rpc" / "ros2_rpc.ibmw.yaml", "RpcClientModule",
     "rpc_client_request_parameter_build.RosTestRpc",
     "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ROS_TEST_RPC_BEGIN",
     "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ROS_TEST_RPC_END",
     "req.num = count;"),
    (TEMPLATES / "json_local_service.ibmw.yaml", "RpcServerModule",
     "rpc_server_handler_body.AddTwoInts", "IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_BEGIN",
     "IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_END",
     "rsp.sum = req.a + req.b;\nreturn ibmw::rpc::Status();"),
    (TEMPLATES / "json_local_service.ibmw.yaml", "RpcClientModule",
     "rpc_client_request_parameter_build.AddTwoInts",
     "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ADD_TWO_INTS_BEGIN",
     "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ADD_TWO_INTS_END",
     "req.a = 1;\nreq.b = 2;"),
    (SAMPLES / "loaned_message_demo" / "loaned_message_demo.ibmw.yaml", "PointLoanedPublisher",
     "populate_loaned_message", "user code begin (populate_loaned_message)",
     "user code end (populate_loaned_message)",
     "/* fill the loaned sample */\nloop_var->x = 1.0;\n\nloop_var->y = 2.0;"),
    (SAMPLES / "loaned_message_demo" / "loaned_message_demo.ibmw.yaml", "PointLoanedSubscriber",
     "on_message_body", "user code begin (on_message_body)", "user code end (on_message_body)",
     'MW_INFO("x={}", msg->x);'),
    (SAMPLES / "loaned_message_demo" / "loaned_message_demo_iceoryx.ibmw.yaml",
     "PointIceoryxLoanedPublisher",
     "populate_loaned_message", "user code begin (populate_loaned_message)",
     "user code end (populate_loaned_message)",
     "loop_var->x = static_cast<double>(publish_count_);"),
    (SAMPLES / "ros2_point_demo" / "ros2_point_demo.ibmw.yaml", "PointPublisherModule",
     "publish_body", "user code begin (publish_body)", "user code end (publish_body)",
     "msg.x = 1.0;"),
    (SAMPLES / "ros2_point_demo" / "ros2_point_demo.ibmw.yaml", "PointSubscriberModule",
     "on_message_body", "user code begin (on_message_body)", "user code end (on_message_body)",
     'MW_INFO("x={}", msg->x);'),
]


def _region(text: str, begin: str, end: str):
    lines = text.splitlines(keepends=True)
    b = next(i for i, line in enumerate(lines) if begin in line)
    e = next(i for i in range(b + 1, len(lines)) if end in lines[i])
    return lines, b, e


class UserCodeRegionTest(unittest.TestCase):
    _baseline: dict = {}

    def _baseline_files(self, path: pathlib.Path) -> dict:
        if path not in self._baseline:
            result, files = _generate(_load(path))
            self.assertEqual(result.returncode, 0, result.stderr)
            self._baseline[path] = files
        return self._baseline[path]

    def test_snippet_replaces_region_body_and_nothing_else(self):
        for path, module, region, begin, end, snippet in CASES:
            with self.subTest(yaml=path.name, module=module, region=region):
                base = self._baseline_files(path)
                result, files = _generate(_with_user_code(_load(path), module, {region: snippet}))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(set(files), set(base))
                changed = [p for p in files if files[p] != base[p]]
                self.assertEqual(len(changed), 1, changed)
                rel = changed[0]
                new_lines, nb, ne = _region(files[rel], begin, end)
                body = "".join(new_lines[nb + 1:ne])
                self.assertNotIn("TODO", body)
                for line in snippet.splitlines():
                    if line.strip():
                        self.assertIn(line, body)
                indent = new_lines[nb][:len(new_lines[nb]) - len(new_lines[nb].lstrip())]
                for line in new_lines[nb + 1:ne]:
                    if line.strip() and line.strip() != "\\":
                        self.assertTrue(line.startswith(indent), line)
                if new_lines[nb].rstrip().endswith("\\"):
                    col = len(new_lines[nb].rstrip()) - 1
                    for line in new_lines[nb + 1:ne]:
                        self.assertEqual(line.rstrip("\n")[col:], "\\", line)
                if begin in base[rel]:
                    # Everything outside the region is byte-identical.
                    old_lines, ob, oe = _region(base[rel], begin, end)
                    self.assertNotEqual(old_lines[ob + 1:oe], new_lines[nb + 1:ne])
                    self.assertEqual(old_lines[:ob + 1], new_lines[:nb + 1])
                    self.assertEqual(old_lines[oe:], new_lines[ne:])
                else:
                    # direct_* template: the single placeholder line is wrapped.
                    old_lines = base[rel].splitlines(keepends=True)
                    self.assertEqual(old_lines[:nb], new_lines[:nb])
                    self.assertIn("TODO", old_lines[nb])
                    self.assertEqual(old_lines[nb + 1:], new_lines[ne + 1:])

    def test_snippet_is_dedented_and_reindented(self):
        path = SAMPLES / "dds_native_demo" / "dds_native_demo.ibmw.yaml"
        snippet = "        if (true) {\n          msg_count_++;\n        }\n"
        _, files = _generate(_with_user_code(_load(path), "PublisherModule",
                                             {"publish_body": snippet}))
        text = files["src/publisher_module/publisher_module.cc"]
        expected = textwrap.dedent("""\
            // IBMW: user code begin (publish_body)
            if (true) {
              msg_count_++;
            }
            // IBMW: user code end (publish_body)
        """)
        self.assertIn(textwrap.indent(expected, "    "), text)

    def test_absent_user_code_keeps_default_regions(self):
        base = self._baseline_files(SAMPLES / "dds_native_demo" / "dds_native_demo.ibmw.yaml")
        text = base["src/publisher_module/publisher_module.cc"]
        self.assertIn("    // IBMW: user code begin (publish_body)\n"
                      "    // TODO: populate msg fields here\n"
                      "    msg_count_++;\n"
                      "    // IBMW: user code end (publish_body)\n", text)


class UserCodeValidationTest(unittest.TestCase):
    PUBSUB = TEMPLATES / "json_local_pubsub.ibmw.yaml"

    def _rejected(self, module: str, user_code, needle: str):
        result, files = _generate(_with_user_code(_load(self.PUBSUB), module, user_code))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(needle, result.stderr)
        self.assertEqual(files, {})

    def test_unknown_region_is_rejected(self):
        self._rejected("PublisherModule", {"on_message_body": "x();"},
                       "unknown user_code region 'on_message_body'. Allowed regions: publish_body")

    def test_method_suffix_rules(self):
        self._rejected("PublisherModule", {"publish_body.Foo": "x();"},
                       "unknown user_code region 'publish_body.Foo'")

    def test_non_string_and_non_mapping_are_rejected(self):
        self._rejected("PublisherModule", {"publish_body": 42}, "must be a non-empty string")
        self._rejected("PublisherModule", ["publish_body"], "'user_code' must be a mapping")

    def test_anchor_line_injection_is_rejected(self):
        for line in ("// IBMW: user code end (publish_body)",
                     "  // IBMW_PUBSUB_PUBLISHER_BODY_END",
                     "// IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_BEGIN",
                     "/* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_END */"):
            with self.subTest(line=line):
                self._rejected("PublisherModule", {"publish_body": f"x();\n{line}\n"},
                               "must not contain IBMW marker anchor lines")

    def test_line_comment_in_macro_region_is_rejected(self):
        path = SAMPLES / "loaned_message_demo" / "loaned_message_demo.ibmw.yaml"
        data = _with_user_code(_load(path), "PointLoanedPublisher",
                               {"populate_loaned_message": "loop_var->x = 1;  // bad"})
        result, files = _generate(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("emitted inside a macro body", result.stderr)
        self.assertEqual(files, {})

    def test_unsupported_module_type_is_rejected(self):
        data = _load(self.PUBSUB)
        data["modules"].append({"name": "BareModule", "type": "bare",
                                "user_code": {"publish_body": "x();"}})
        result, files = _generate(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'user_code' is not supported for module type 'bare'", result.stderr)
        self.assertEqual(files, {})

    def test_unknown_rpc_method_fails_generation(self):
        path = TEMPLATES / "json_local_service.ibmw.yaml"
        data = _with_user_code(_load(path), "RpcServerModule",
                               {"rpc_server_handler_body.NoSuchMethod": "return {};"})
        result, _ = _generate(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("user_code region 'rpc_server_handler_body.NoSuchMethod' was not found",
                      result.stderr)

    def test_quotes_allowed_in_user_code_but_not_in_topic(self):
        snippet = 'msg.data = "say \\"hi\\"\\n";\nmsg_count_++;'
        result, files = _generate(_with_user_code(_load(self.PUBSUB), "PublisherModule",
                                                  {"publish_body": snippet}))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('    msg.data = "say \\"hi\\"\\n";\n',
                      files["src/publisher_module/publisher_module.cc"])

        data = _with_user_code(_load(self.PUBSUB), "PublisherModule", {"publish_body": snippet})
        data["modules"][0]["topic"] = 'chat"ter'
        result, files = _generate(data)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Module 'PublisherModule': 'topic' must not contain", result.stderr)
        self.assertEqual(files, {})


if __name__ == "__main__":
    unittest.main()
