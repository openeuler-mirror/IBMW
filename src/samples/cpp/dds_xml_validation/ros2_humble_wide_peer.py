#!/usr/bin/env python3
"""Stock ROS2 Humble peer for the OHOS IBMW wide DDS validation pair."""

import argparse
import json
import time

import rclpy
from rclpy.serialization import serialize_message
from diagnostic_msgs.msg import DiagnosticArray, DiagnosticStatus, KeyValue
from geometry_msgs.msg import PoseStamped, TransformStamped
from nav_msgs.msg import MapMetaData, OccupancyGrid, Odometry, Path
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import CameraInfo, CompressedImage, Image, Imu, LaserScan
from sensor_msgs.msg import PointCloud2, PointField
from slam_toolbox.srv import SerializePoseGraph
from tf2_msgs.msg import TFMessage
from visualization_msgs.msg import Marker, MarkerArray


DOMAIN = 71
SERVICE = "/slam_toolbox/srv/SerializePoseGraph"
FILENAME = "ibmw_xml_pose_graph"


def header(message, frame):
    message.header.frame_id = frame
    message.header.stamp.sec = 71
    message.header.stamp.nanosec = 123456789


def fixtures():
    result = {}

    message = Image()
    header(message, "camera")
    message.height = 2
    message.width = 3
    message.encoding = "rgb8"
    message.is_bigendian = 0
    message.step = 9
    message.data = list(range(1, 19))
    result["/ibmw/xml/wide/image"] = message

    message = CompressedImage()
    header(message, "camera")
    message.format = "jpeg"
    message.data = [9, 8, 7, 6, 5, 4]
    result["/ibmw/xml/wide/compressed_image"] = message

    message = Imu()
    header(message, "imu")
    message.orientation.x = 0.1
    message.orientation.y = 0.2
    message.orientation.z = 0.3
    message.orientation.w = 0.9
    message.angular_velocity.x = 1.1
    message.angular_velocity.y = 1.2
    message.angular_velocity.z = 1.3
    message.linear_acceleration.x = 2.1
    message.linear_acceleration.y = 2.2
    message.linear_acceleration.z = 2.3
    message.orientation_covariance[0] = 0.01
    message.angular_velocity_covariance[4] = 0.02
    message.linear_acceleration_covariance[8] = 0.03
    result["/ibmw/xml/wide/imu"] = message

    message = LaserScan()
    header(message, "laser")
    message.angle_min = -0.5
    message.angle_max = 0.5
    message.angle_increment = 0.25
    message.time_increment = 0.01
    message.scan_time = 0.1
    message.range_min = 0.2
    message.range_max = 30.0
    message.ranges = [1.0, 2.0, 3.0, 4.0, 5.0]
    message.intensities = [10.0, 20.0, 30.0, 40.0, 50.0]
    result["/ibmw/xml/wide/laser_scan"] = message

    message = PointCloud2()
    header(message, "lidar")
    message.height = 1
    message.width = 2
    message.fields = [PointField(name="x", offset=0, datatype=7, count=1)]
    message.is_bigendian = False
    message.point_step = 4
    message.row_step = 8
    message.data = [0, 0, 128, 63, 0, 0, 0, 64]
    message.is_dense = True
    result["/ibmw/xml/wide/point_cloud"] = message

    message = CameraInfo()
    header(message, "camera")
    message.height = 480
    message.width = 640
    message.distortion_model = "plumb_bob"
    message.d = [0.1, 0.2, 0.3, 0.4, 0.5]
    message.k = [500.0, 0.0, 320.0, 0.0, 500.0, 240.0, 0.0, 0.0, 1.0]
    message.r = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    message.p[0] = 500.0
    message.p[2] = 320.0
    message.p[5] = 500.0
    message.p[6] = 240.0
    message.binning_x = 1
    message.binning_y = 1
    message.roi.x_offset = 2
    message.roi.y_offset = 3
    message.roi.height = 10
    message.roi.width = 11
    result["/ibmw/xml/wide/camera_info"] = message

    message = Odometry()
    header(message, "odom")
    message.child_frame_id = "base_link"
    message.pose.pose.position.x = 1.0
    message.pose.pose.position.y = 2.0
    message.pose.pose.orientation.w = 1.0
    message.twist.twist.linear.x = 0.5
    message.twist.twist.angular.z = 0.25
    message.pose.covariance[0] = 0.1
    message.twist.covariance[0] = 0.2
    result["/ibmw/xml/wide/odometry"] = message

    message = Path()
    header(message, "map")
    pose = PoseStamped()
    header(pose, "map")
    pose.pose.position.x = 3.0
    pose.pose.position.y = 4.0
    pose.pose.orientation.w = 1.0
    message.poses = [pose]
    result["/ibmw/xml/wide/path"] = message

    metadata = MapMetaData()
    metadata.map_load_time.sec = 71
    metadata.map_load_time.nanosec = 9
    metadata.resolution = 0.05
    metadata.width = 4
    metadata.height = 3
    metadata.origin.position.x = -1.0
    metadata.origin.position.y = -2.0
    metadata.origin.orientation.w = 1.0

    message = OccupancyGrid()
    header(message, "map")
    message.info = metadata
    message.data = [0, 100, -1, 50, 25, 75, 1, 2, 3, 4, 5, 6]
    result["/ibmw/xml/wide/occupancy_grid"] = message
    result["/ibmw/xml/wide/map_metadata"] = metadata

    message = TFMessage()
    transform = TransformStamped()
    header(transform, "map")
    transform.child_frame_id = "base_link"
    transform.transform.translation.x = 1.0
    transform.transform.rotation.w = 1.0
    message.transforms = [transform]
    result["/ibmw/xml/wide/tf"] = message

    message = MarkerArray()
    marker = Marker()
    header(marker, "map")
    marker.ns = "ibmw"
    marker.id = 71
    marker.type = Marker.CUBE
    marker.action = Marker.ADD
    marker.pose.orientation.w = 1.0
    marker.scale.x = 1.0
    marker.scale.y = 2.0
    marker.scale.z = 3.0
    marker.color.r = 1.0
    marker.color.g = 0.5
    marker.color.a = 1.0
    marker.text = "wide"
    message.markers = [marker]
    result["/ibmw/xml/wide/markers"] = message

    message = DiagnosticArray()
    header(message, "base_link")
    status = DiagnosticStatus()
    status.level = DiagnosticStatus.WARN
    status.name = "ibmw"
    status.message = "wide fixture"
    status.hardware_id = "xml-71"
    status.values = [KeyValue(key="fixture", value="13")]
    message.status = [status]
    result["/ibmw/xml/wide/diagnostics"] = message

    if len(result) != 13:
        raise RuntimeError("fixture count is not 13")
    return result


def qos_profile():
    return QoSProfile(
        history=HistoryPolicy.KEEP_LAST,
        depth=10,
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.VOLATILE,
    )


def report(role, passed, details):
    status = "PASS" if passed else "FAIL"
    service = "PASS" if details.get("service") else "FAIL"
    print(json.dumps(details, sort_keys=True, separators=(",", ":")), flush=True)
    print(
        "ROS2_HUMBLE_WIDE_RESULT "
        f"role={role} status={status} topics={details.get('topics', 0)} "
        f"service={service}",
        flush=True,
    )


def run_publisher(node, messages, timeout):
    qos = qos_profile()
    publishers = {
        topic: node.create_publisher(type(message), topic, qos)
        for topic, message in messages.items()
    }
    client = node.create_client(SerializePoseGraph, SERVICE)
    deadline = time.monotonic() + timeout
    future = None
    response_ok = False
    matched = []
    ever_matched = set()
    while time.monotonic() < deadline and not response_ok:
        rclpy.spin_once(node, timeout_sec=0.05)
        matched = [topic for topic, pub in publishers.items()
                   if pub.get_subscription_count() > 0]
        ever_matched.update(matched)
        if len(matched) != len(messages) or not client.service_is_ready():
            continue
        for topic, message in messages.items():
            publishers[topic].publish(message)
        if future is None:
            request = SerializePoseGraph.Request()
            request.filename = FILENAME
            future = client.call_async(request)
        elif future.done():
            error = future.exception()
            if error is not None:
                raise error
            response_ok = future.result().result == 0
            future = None
        time.sleep(0.1)
    details = {
        "domain": DOMAIN,
        "matched_topics": sorted(ever_matched),
        "rmw": "rmw_fastrtps_cpp",
        "service": response_ok,
        "topics": len(ever_matched),
    }
    return len(ever_matched) == len(messages) and response_ok, details


def run_subscriber(node, messages, timeout):
    qos = qos_profile()
    received = set()
    mismatches = {}
    subscriptions = []
    ever_matched = set()

    def callback(topic, expected, actual):
        if serialize_message(actual) == serialize_message(expected):
            received.add(topic)
        else:
            mismatches[topic] = "payload differs from wide_fixtures.h"

    for topic, expected in messages.items():
        subscriptions.append(node.create_subscription(
            type(expected), topic,
            lambda actual, t=topic, e=expected: callback(t, e, actual), qos))

    service_seen = False
    filename_ok = False

    def service_callback(request, response):
        nonlocal service_seen, filename_ok
        service_seen = True
        filename_ok = request.filename == FILENAME
        response.result = 0 if filename_ok and len(received) == len(messages) else 255
        return response

    service = node.create_service(SerializePoseGraph, SERVICE, service_callback)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=0.1)
        ever_matched.update(
            topic for topic in messages if node.count_publishers(topic) > 0)
        if len(received) == len(messages) and service_seen and filename_ok:
            break
    passed = (len(received) == len(messages) and service_seen and filename_ok
              and not mismatches)
    details = {
        "domain": DOMAIN,
        "matched_topics": sorted(ever_matched),
        "mismatches": mismatches,
        "missing_topics": sorted(set(messages) - received),
        "rmw": "rmw_fastrtps_cpp",
        "service": service_seen and filename_ok,
        "topics": len(received),
    }
    return passed, details


def main():
    global DOMAIN
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("publisher", "subscriber"))
    parser.add_argument("--domain", type=int, default=DOMAIN)
    parser.add_argument("--timeout", type=float, default=35.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    messages = fixtures()
    if args.self_test:
        report("self-test", True, {"service": True, "topics": len(messages)})
        return 0
    if args.role is None:
        parser.error("--role is required unless --self-test is used")

    DOMAIN = args.domain
    rclpy.init(args=None, domain_id=args.domain)
    node = rclpy.create_node(f"ibmw_wide_{args.role}_peer")
    try:
        if args.role == "publisher":
            passed, details = run_publisher(node, messages, args.timeout)
        else:
            passed, details = run_subscriber(node, messages, args.timeout)
        report(args.role, passed, details)
        return 0 if passed else 1
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
