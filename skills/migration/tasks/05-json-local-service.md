Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 05 — JSON/Local Application Service

Goal: create JSON RPC practice target.

Steps:

1. Copy `templates/json_local_service.ibmw.yaml` to
   `json_local_application/service/<app>_json_local_service.ibmw.yaml`.
2. Fill request/response JSON schema and RPC names.
3. Generate server/client modules.
4. User-region server maps JSON request to adapter input, calls adapter, maps
   output to JSON response.
5. Verify local/RPC smoke only.

Evidence:

- YAML: `json_local_application/service/*_json_local_service.ibmw.yaml`
- Runtime pass marker: `JSON_LOCAL_SERVICE_PASS` inside
  `reports/runtime_logs/json_local_service.log` or equivalent.
- Final report explicitly says JSON RPC passed locally while backend
  global status remains degraded.
