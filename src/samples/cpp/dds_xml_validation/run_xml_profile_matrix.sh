#!/usr/bin/env bash
set -eu

if [ "$#" -ne 3 ]; then
  printf 'usage: %s <root-dir> <role> <shm|udp|mixed|physical>\n' "$0" >&2
  exit 2
fi

root_dir=$1
role=$2
profile=$3
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

case "$role" in
  publisher)
    template=$script_dir/publisher.yaml
    app=ibmw_dds_xml_wide_publisher_app
    ;;
  subscriber)
    template=$script_dir/subscriber.yaml
    app=ibmw_dds_xml_wide_subscriber_app
    ;;
  *)
    printf 'invalid role: %s\n' "$role" >&2
    exit 2
    ;;
esac

case "$profile" in
  shm) profile_name=ibmw_xml_shm ;;
  udp) profile_name=ibmw_xml_udp ;;
  mixed) profile_name=ibmw_xml_mixed ;;
  physical) profile_name=ibmw_xml_udp_physical ;;
  *)
    printf 'invalid profile: %s\n' "$profile" >&2
    exit 2
    ;;
esac

xml_profile_file=$root_dir/profiles.xml
app_path=$root_dir/bin/$app

if [ ! -d "$root_dir" ] || [ ! -f "$xml_profile_file" ] ||
   [ ! -f "$template" ] || [ ! -x "$app_path" ]; then
  printf 'invalid root or staged validation files under: %s\n' "$root_dir" >&2
  exit 1
fi

config_file=$(mktemp "${TMPDIR:-/tmp}/ibmw-xml-profile.XXXXXX")
cleanup() {
  rm -f "$config_file"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

while IFS= read -r line || [ -n "$line" ]; do
  line=${line//@XML_PROFILE_FILE@/$xml_profile_file}
  line=${line//@XML_PROFILE_NAME@/$profile_name}
  line=${line//@ROOT_DIR@/$root_dir}
  printf '%s\n' "$line"
done < "$template" > "$config_file"

export LD_LIBRARY_PATH=$root_dir/lib:$root_dir/bin
# Pass the real config path: the engine canonicalizes it and exposes it to
# extensions, so an unlinked /proc/self/fd/N target cannot be resolved.
"$app_path" "$config_file" &
app_pid=$!
trap 'kill -TERM "$app_pid" 2>/dev/null || true' HUP INT TERM
status=0
wait "$app_pid" || status=$?
while kill -0 "$app_pid" 2>/dev/null; do
  status=0
  wait "$app_pid" || status=$?
done
exit "$status"
