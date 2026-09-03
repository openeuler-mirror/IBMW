# DDS Managed Exact Candidate

`IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=ON` exposes the production
`ibmw_dds_managed_exact_candidate` executable without enabling tests. It
requires the IBMW runtime, communication trace, DDS extension, and protocols.
The candidate uses the generated native `DdsGenericMsg` protocol and does not
require ROS 2 runtime libraries or `IBMW_BUILD_ROS2_TYPES`.

For an install build, install component `dds_managed_exact_candidate` places:

- `bin/ibmw_dds_managed_exact_candidate`
- `lib/libibmw_dds_transport.so`

Deploy those files together with every shared object listed by the target
platform's `readelf -d` `NEEDED` entries. Keep the executable in `bin` and the
extension in `lib`; the executable's install RPATH is
`$ORIGIN/../lib:$ORIGIN`. Provider and platform libraries are not copied by
this component because they come from the selected DDS SDK and OHOS system
image.

Local managed collector tests invoke the candidate without arguments. In that
mode it prints `managed exact candidate ready` after all 1000 callbacks pass
validation, then waits for SIGTERM from the collector.

Remote capture agents must invoke the production binary with the explicit
completion mode:

```text
mw_perf_ohos_capture_agent ... -- \
  /absolute/path/bin/ibmw_dds_managed_exact_candidate --exit-after-complete
```

`--exit-after-complete` is accepted only by the production candidate. After all
1000 callbacks and validations complete, it runs the normal dispatcher, DDS
bus, and communication-trace shutdown sequence and exits zero. The fault
candidate CLI remains restricted to its existing `--fault=...` arguments.
