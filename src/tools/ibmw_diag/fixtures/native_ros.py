"""Independent native ROS 2 peer: real message and RPC effects, not a mock."""
import argparse
import json
from pathlib import Path
import signal
import time

import rclpy
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, LivelinessPolicy
from rclpy.duration import Duration
from rclpy.signals import SignalHandlerOptions
from rclpy.serialization import serialize_message
from std_msgs.msg import Int32
from example_interfaces.srv import AddTwoInts
from rcl_interfaces.msg import Log, ParameterEvent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--policy", choices=["normal", "reliability", "durability", "depth", "deadline",
                                             "liveliness", "liveliness_lease_duration"], default="normal")
    args = parser.parse_args()
    stopped = False

    def stop(*_):
        nonlocal stopped
        stopped = True

    def record(name, value):
        with (args.directory / (name + ".jsonl")).open("a") as output:
            output.write(json.dumps(value) + "\n")

    rclpy.init(signal_handler_options=SignalHandlerOptions.NO)
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    node = rclpy.create_node("diag_native_peer")
    qos = QoSProfile(depth=10)
    if args.policy == "reliability":
        qos.reliability = ReliabilityPolicy.BEST_EFFORT
    if args.policy == "deadline":
        qos.deadline = Duration(seconds=2)
    if args.policy == "liveliness_lease_duration":
        qos.liveliness_lease_duration = Duration(seconds=2)
    publisher = node.create_publisher(Int32, "/diag/data", qos)
    node.create_subscription(Int32, "/diag/input", lambda msg: record("received", {"data": msg.data}), 10)
    if args.policy != "normal":
        # Keep infrastructure topics paired so root doctor exit codes measure
        # the application QoS case, independently of idle ROS logging/events.
        infrastructure_qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.BEST_EFFORT,
                                         durability=DurabilityPolicy.VOLATILE)
        node.create_subscription(Log, "/rosout", lambda _: None, infrastructure_qos)
        node.create_subscription(ParameterEvent, "/parameter_events", lambda _: None, infrastructure_qos)
        good = QoSProfile(depth=10, reliability=qos.reliability, deadline=qos.deadline,
                          liveliness=qos.liveliness, liveliness_lease_duration=qos.liveliness_lease_duration)
        bad = QoSProfile(depth=3 if args.policy == "depth" else 10)
        if args.policy == "durability":
            bad.durability = DurabilityPolicy.TRANSIENT_LOCAL
        if args.policy == "deadline":
            bad.deadline = Duration(seconds=.5)
        if args.policy == "liveliness":
            bad.liveliness = LivelinessPolicy.MANUAL_BY_TOPIC
        if args.policy == "liveliness_lease_duration":
            bad.liveliness_lease_duration = Duration(seconds=1)
        node.create_subscription(Int32, "/diag/data", lambda msg: record("good", {"data": msg.data}), good)
        node.create_subscription(Int32, "/diag/data", lambda msg: record("bad", {"data": msg.data}), bad)

    def add(request, response):
        record("rpc", {"a": request.a, "b": request.b})
        if request.a == -999:
            time.sleep(.8)
        response.sum = request.a + request.b
        record("rpc-done", {"a": request.a, "b": request.b})
        return response

    node.create_service(AddTwoInts, "/diag_add", add)
    message = Int32(data=42)

    def publish():
        publisher.publish(message)
        record("sent", {"time_ns": time.monotonic_ns(), "bytes": len(serialize_message(message))})

    node.create_timer(.04, publish)
    print("READY", flush=True)
    try:
        while not stopped:
            rclpy.spin_once(node, timeout_sec=.05)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
