#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_red_lines.sh — IBMW transport-decoupling R1-R12 red-line auto checker.
#
# 来源/依据：
#   - docs/contributing/architecture-rules.md
#   - docs/contributing/testing.md
#
# 在完整验证流程中作为 Step 0 必跑：
#   Step 0: bash ibmw/scripts/check_red_lines.sh        ← 本脚本
#   Step 1: rm -rf ibmw/build
#   Step 2: cmake --preset full ...
#   Step 3: cmake --build ...
#   Step 4: ctest ...
#   Step 5: smoke
#   Step 6: bench
#
# 模式：
#   (default / --strict) 任何 R 违规 → exit 1
#   --report             列出全部违规但 exit 0（看清现状不阻塞）
#   --baseline <file>    与基线对比，仅"新增违规"才 exit 1（渐进收敛）
#
# 退出码：
#   0  — 全绿（或 --report 模式）
#   1  — strict 模式下检测到违规
#   2  — baseline 模式下新增违规
#   64 — 参数错误 / 环境错误（含扫描步骤本身失败，任何模式）
#
# 用例：
#   bash ibmw/scripts/check_red_lines.sh                 # strict
#   bash ibmw/scripts/check_red_lines.sh --report        # 仅汇报
#   bash ibmw/scripts/check_red_lines.sh --report > ibmw/scripts/baseline_red_lines.txt
#   bash ibmw/scripts/check_red_lines.sh --baseline ibmw/scripts/baseline_red_lines.txt
#
# 注意：
#   - 本脚本只做静态 grep；不依赖 build artifact（R7 nm 检查在 build 后单独跑）
#   - R6 / R8 / R10 主要靠 schema + 设计约束执行，本脚本仅做辅助 grep
#   - R11 和 R12 已转为 strict 检查

set -u
# 注意：不 set -e，否则 grep 未匹配（exit 1）会终止脚本。
# 扫描步骤本身的失败（find/awk 出错、grep 返回 >1）由 scan_failed 计数，
# 最后以 exit 64 结束，避免违规数为 0 的"误绿"。
SCAN_ERRORS=0
scan_failed() {
  SCAN_ERRORS=$((SCAN_ERRORS + 1))
  echo "ERROR: scan step failed: $*" >&2
}

#─────────────────────────────────────────────
# 0. 参数解析
#─────────────────────────────────────────────
MODE="strict"      # strict | report | baseline
BASELINE_FILE=""
SCAN_PATH_OVERRIDE=""   # testing-only override of default scan paths
EXCLUDE_FILES=()

# An option missing its value would make `shift 2` fail and loop forever.
require_value() {
  if [[ $# -lt 2 ]]; then
    echo "Missing value for $1" >&2
    exit 64
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict)    MODE="strict";   shift ;;
    --report)    MODE="report";   shift ;;
    --baseline)  require_value "$@"; MODE="baseline"; BASELINE_FILE="$2"; shift 2 ;;
    --scan-path) require_value "$@"; SCAN_PATH_OVERRIDE="$2"; shift 2 ;;
    --exclude)   require_value "$@"; EXCLUDE_FILES+=("$2"); shift 2 ;;
    -h|--help)
      sed -n '2,40p' "$0"
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Run with -h for help." >&2
      exit 64
      ;;
  esac
done

if [[ "$MODE" == "baseline" ]]; then
  if [[ -z "$BASELINE_FILE" || ! -f "$BASELINE_FILE" ]]; then
    echo "ERROR: --baseline requires an existing file (got: '$BASELINE_FILE')" >&2
    exit 64
  fi
fi

#─────────────────────────────────────────────
# 1. 路径锚定（脚本所在仓库根定位）
#─────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IBMW_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"   # → ibmw/
SAMPLES_DIR="$IBMW_ROOT/src/samples/cpp"
API_DIR="$IBMW_ROOT/src/api"

if [[ -n "$SCAN_PATH_OVERRIDE" ]]; then
  if [[ ! -d "$SCAN_PATH_OVERRIDE" ]]; then
    echo "ERROR: --scan-path dir not found: $SCAN_PATH_OVERRIDE" >&2
    exit 64
  fi
  SAMPLES_DIR="$SCAN_PATH_OVERRIDE"
fi

if [[ ! -d "$SAMPLES_DIR" ]]; then
  echo "ERROR: samples dir not found: $SAMPLES_DIR" >&2
  exit 64
fi

#─────────────────────────────────────────────
# 2. user-facing 文件集合（R1/R2/R3/R4/R9 共用）
#─────────────────────────────────────────────
# user-facing 定义（skill §R1）：
#   - samples/cpp/**/managed/**/*.{h,cc,hpp,cpp}
#   - samples/cpp/**/<sample>_gen/src/**/*_module.{h,cc}
#   - samples/cpp/**/standalone/**/main.cc
#
# 用临时文件存路径列表，避免 xargs 在文件名含空格时崩
USER_FACING_LIST="$(mktemp)"
VIOLATIONS_RAW=""
VIOLATIONS_NEW=""
cleanup() {
  [[ -n "${USER_FACING_LIST:-}" && -f "$USER_FACING_LIST" ]] && rm -f "$USER_FACING_LIST"
  [[ -n "${VIOLATIONS_RAW:-}"   && -f "$VIOLATIONS_RAW"   ]] && rm -f "$VIOLATIONS_RAW"
  [[ -n "${VIOLATIONS_NEW:-}"   && -f "$VIOLATIONS_NEW"   ]] && rm -f "$VIOLATIONS_NEW"
  [[ -n "${R8_HITS:-}"          && -f "$R8_HITS"          ]] && rm -f "$R8_HITS"
  [[ -n "${VIOLATION_LINES_FILE:-}" && -f "$VIOLATION_LINES_FILE" ]] && rm -f "$VIOLATION_LINES_FILE"
  [[ -n "${BASELINE_LINES_FILE:-}"  && -f "$BASELINE_LINES_FILE"  ]] && rm -f "$BASELINE_LINES_FILE"
  [[ -n "${CURRENT_LINES_NORM:-}"   && -f "$CURRENT_LINES_NORM"   ]] && rm -f "$CURRENT_LINES_NORM"
  return 0
}
trap cleanup EXIT

# managed 目录下所有 .h/.cc/.hpp/.cpp
find "$SAMPLES_DIR" -type d -name 'managed' -print0 \
  | xargs -0 -I{} find {} -type f \( -name '*.h' -o -name '*.cc' -o -name '*.hpp' -o -name '*.cpp' \) \
  >> "$USER_FACING_LIST"
st=("${PIPESTATUS[@]}"); [[ ${st[0]} -eq 0 && ${st[1]} -eq 0 ]] || scan_failed "managed file list (${st[*]})"

# *_gen/src 下的 *_module.{h,cc}
find "$SAMPLES_DIR" -type d -name '*_gen' -print0 \
  | xargs -0 -I{} find {}/src -type f \( -name '*_module.h' -o -name '*_module.cc' \) 2>/dev/null \
  >> "$USER_FACING_LIST"
# 不是每个 *_gen 都有 src/，内层 find 的失败是预期的，只检查外层。
[[ ${PIPESTATUS[0]} -eq 0 ]] || scan_failed "*_gen directory list"

# standalone main.cc
find "$SAMPLES_DIR" -type d -name 'standalone' -print0 \
  | xargs -0 -I{} find {} -type f -name 'main.cc' \
  >> "$USER_FACING_LIST"
st=("${PIPESTATUS[@]}"); [[ ${st[0]} -eq 0 && ${st[1]} -eq 0 ]] || scan_failed "standalone file list (${st[*]})"

# ibmw-template/ user-facing tree (non-overridden, production scan only).
# The starter project is a peer of src/ for refactor + red-line purposes
# (see docs/contributing/architecture-rules.md). Sweep all .h/.cc/.hpp/.cpp under:
#   ibmw-template/src/**                            # primary template variants
#   ibmw-template/my_ibmw_project/src/**            # nested example project
# Excluding: snapshots/ (build artifacts), any */build/ trees, *.md, cfg/.
# Skipped automatically when --scan-path overrides SAMPLES_DIR (test harness).
if [[ -z "$SCAN_PATH_OVERRIDE" ]]; then
  for tmpl_root in "$IBMW_ROOT/ibmw-template/src" \
                   "$IBMW_ROOT/ibmw-template/my_ibmw_project/src"; do
    if [[ -d "$tmpl_root" ]]; then
      find "$tmpl_root" -type f \
          \( -name '*.h' -o -name '*.cc' -o -name '*.hpp' -o -name '*.cpp' \) \
          -not -path '*/build/*' -not -path '*/snapshots/*' \
          >> "$USER_FACING_LIST" || scan_failed "template file list: $tmpl_root"
    fi
  done
fi

# --scan-path test mode: synthetic dirs lack the managed/_gen/standalone
# layout, so fall back to scanning all .h/.cc in the override directory directly.
# This branch is taken only by the synthetic R8/R12 harness; production runs
# always populate via the three structural finds above.
if [[ -n "$SCAN_PATH_OVERRIDE" && ! -s "$USER_FACING_LIST" ]]; then
  find "$SAMPLES_DIR" -type f \( -name '*.h' -o -name '*.cc' -o -name '*.hpp' -o -name '*.cpp' \) \
    >> "$USER_FACING_LIST" || scan_failed "scan-path file list"
fi

USER_FACING_COUNT=$(wc -l < "$USER_FACING_LIST")

#─────────────────────────────────────────────
# 3. 通用违规收集
#─────────────────────────────────────────────
VIOLATIONS_RAW="$(mktemp)"

# emit <RULE_ID> <severity> <description> 后追加 grep 结果（每行：<file>:<line>:<content>）
emit_rule() {
  local rule="$1"; local sev="$2"; local desc="$3"
  echo "── [$rule] ($sev) $desc ──" >> "$VIOLATIONS_RAW"
}

# 在 user-facing 文件集中 grep 模式；命中行追加到 VIOLATIONS_RAW，每行加 "RULE: " 前缀
#
# Per-rule EXEMPT 锚点：用形如
#     // IBMW_R<N>_EXEMPT_BEGIN[: optional-justification]
#     <exempt source lines>
#     // IBMW_R<N>_EXEMPT_END
# 的注释对，可以临时豁免特定 user-facing 文件中针对规则 R<N>（N=1..9）的检查。
# 与 R7 EXEMPT 机制（grep_pkg_with_exempt 内部、仅 src/api/pkg & src/api/c 适用）
# 区分：本机制是用户面（samples/cpp + ibmw-template）使用的、按规则号匹配的通用
# 锚点。匹配是文本级（grep -E "IBMW_R${rule#R}_EXEMPT_BEGIN"），由 awk 维护
# in-block 状态，输出时保留原始行号以保证 baseline diff 路径稳定。
#
# 注意：
#   - 锚点本身（包含 "EXEMPT_BEGIN/END" 字面量的行）仍会出现在文件中；为防止
#     注释行本身命中其他规则的正则，本函数同时跳过任何含 "IBMW_R*_EXEMPT_*"
#     字样的行（视为元数据行）。
#   - 锚点是 *规则范围豁免*，不是全局豁免。R1 锚点不影响 R2/R3 的检测。
grep_user_facing() {
  local rule="$1"; local pattern="$2"
  if [[ "$USER_FACING_COUNT" -eq 0 ]]; then return; fi
  # 提取 rule 数字，用于构造 EXEMPT_BEGIN/END 锚点字面量（R1 → IBMW_R1_EXEMPT_）
  local rule_num="${rule#R}"
  local exempt_begin="IBMW_R${rule_num}_EXEMPT_BEGIN"
  local exempt_end="IBMW_R${rule_num}_EXEMPT_END"
  # -E 正则；-n 行号；-H 强制带文件名；--no-messages 静默不存在的文件
  while IFS= read -r f; do
    [[ -z "$f" || ! -f "$f" ]] && continue
    
    # K-M: repeatable --exclude flag
    local excluded=0
    for ex in "${EXCLUDE_FILES[@]+"${EXCLUDE_FILES[@]}"}"; do
      if [[ "$f" == *"$ex"* ]]; then
        excluded=1
        break
      fi
    done
    [[ $excluded -eq 1 ]] && continue

    # awk 预过滤：用空行替换 EXEMPT 区间内行 + 锚点元数据行，保留原始 NR 行号
    # 与文件其余内容；空行不会命中任何 R1-R9 的正则（所有规则模式都需要非空字面）。
    # 之后 grep -nE 直接给出正确的原始行号 + 命中内容。
    awk -v begin_marker="$exempt_begin" -v end_marker="$exempt_end" '
      {
        is_meta = ($0 ~ /IBMW_R[0-9]+_EXEMPT_(BEGIN|END)/)
      }
      $0 ~ begin_marker { in_exempt = 1; print ""; next }
      $0 ~ end_marker   { in_exempt = 0; print ""; next }
      in_exempt || is_meta { print ""; next }
      { print }
    ' "$f" \
      | grep -nE --no-messages "$pattern" 2>/dev/null \
      | sed "s|^|$rule: $f:|" \
      >> "$VIOLATIONS_RAW"
    local st=("${PIPESTATUS[@]}")
    [[ ${st[0]} -eq 0 && ${st[1]} -le 1 ]] || scan_failed "$rule: $f (${st[*]})"
  done < "$USER_FACING_LIST"
}

# 在指定单一文件 grep
grep_single_file() {
  local rule="$1"; local file="$2"; local pattern="$3"
  [[ ! -f "$file" ]] && return
  grep -nHE --no-messages "$pattern" "$file" 2>/dev/null \
    | sed "s|^|$rule: |" \
    >> "$VIOLATIONS_RAW"
  [[ ${PIPESTATUS[0]} -le 1 ]] || scan_failed "$rule: $file"
}

# 在 yaml 集合 grep
grep_yaml() {
  local rule="$1"; local pattern="$2"
  # grep -r 保留 0/1/2 退出码（xargs 会把"未匹配"和"出错"都折叠成 123）。
  grep -rnHE --no-messages --include='*.ibmw.yaml' --include='*.ibmw.yml' "$pattern" "$SAMPLES_DIR" 2>/dev/null \
    | sed "s|^|$rule: |" \
    >> "$VIOLATIONS_RAW"
  [[ ${PIPESTATUS[0]} -le 1 ]] || scan_failed "$rule: yaml under $SAMPLES_DIR"
}

#─────────────────────────────────────────────
# 4. R1 — user-facing include 红线
#─────────────────────────────────────────────
emit_rule "R1" "HIGH" "user-facing 不得 include transport-private header"
# skill §R1 grep 模式
R1_PAT='^[[:space:]]*#include[[:space:]]+["<](dds_ext|iceoryx_ext|shm_ext|ros2_ext|mqtt_ext|net_ext|zenoh_ext|extensions/transport|dds_types|ros2_types|fastdds|iox/|json_transport\.h)'
grep_user_facing "R1" "$R1_PAT"

#─────────────────────────────────────────────
# 5. R2 — 命名空间引用红线
#─────────────────────────────────────────────
emit_rule "R2" "HIGH" "user-facing 不得引用 transport-specific 命名空间"
R2_PAT='ibmw::(extensions|ros2::transport|dds|iceoryx|json::transport)::|fastdds::|iox::|eprosima::|zenoh::|mqtt::'
grep_user_facing "R2" "$R2_PAT"

#─────────────────────────────────────────────
# 6. R3 — 用户类成员/方法红线
#─────────────────────────────────────────────
emit_rule "R3" "HIGH" "user-facing 类不得持 *TransportDriver* 成员"
R3_PAT='(IceoryxTransportDriver|ShmTransportDriver|DdsTransportDriver|FastDdsDriver|Ros2TransportDriver|JsonTransportDriver)[[:space:]]*\*'
grep_user_facing "R3" "$R3_PAT"

#─────────────────────────────────────────────
# 7. R4 — Module::SetDriver 字符串匹配红线
#─────────────────────────────────────────────
emit_rule "R4" "HIGH" "user-facing 不得 override SetDriver(driver_name) 做字符串匹配"
# 启发式：在 user-facing 文件里出现 SetDriver 形参带 driver_name + 字符串字面量比较
R4_PAT='SetDriver[[:space:]]*\([^)]*driver_name|driver_name[[:space:]]*==[[:space:]]*"(iceoryx|shm|dds|ros2|json|mqtt|net|zenoh)"'
grep_user_facing "R4" "$R4_PAT"

#─────────────────────────────────────────────
# 8. R5 — YAML schema 红线
#─────────────────────────────────────────────
emit_rule "R5" "MEDIUM" "用户 yaml 不得含 transport-specific 字段"
# 禁字段（skill §R5）：iceoryx_msg_type / shm_msg_type / dds_type_name / dds_namespace_cpp / ros2_type_name /
#                     ros2_msg_include / ros2_package / zero_copy_config / enable_drivers
# 注意：prefer_transport: shm/network/local 是允许的（v0.3 决议）
R5_PAT='^[[:space:]]*(iceoryx_msg_type|shm_msg_type|dds_type_name|dds_namespace_cpp|ros2_type_name|ros2_msg_include|ros2_package|zero_copy_config|enable_drivers)[[:space:]]*:'
grep_yaml "R5" "$R5_PAT"

#─────────────────────────────────────────────
# 9. R7 — pkg ABI 红线（typed-slot 单一 ABI）
#─────────────────────────────────────────────
emit_rule "R7" "CRITICAL" "user-facing + pkg_main.h 不得 export 含 driver_name 字符串的 set_driver"
# 规则：
#   (a) api/pkg/pkg_main.h —— pkg ABI 入口不得含 driver_name 字段；
#       若新增等价字段，应红
#   (b) api/pkg/pkg_macro.h —— 不得出现 driver_name；
#       豁免锚点 IBMW_R7_EXEMPT_BEGIN..END 保留供未来扩展
#   (c) api/c/*.h —— ABI 头不应再出现 driver_name
#   (d) user-facing —— 不应出现 driver_name 字面 dispatch
PKG_MAIN="$API_DIR/pkg/pkg_main.h"
R7_PAT='set_driver[^;]*driver_name'
grep_single_file "R7" "$PKG_MAIN" "$R7_PAT"

# (b) pkg_macro.h 等：用 awk 过滤 EXEMPT 块；其余行再 grep driver_name
grep_pkg_with_exempt() {
  local rule="$1"; local file="$2"; local pattern="$3"
  [[ ! -f "$file" ]] && return
  awk '
    /IBMW_R7_EXEMPT_BEGIN/ { in_exempt=1 }
    !in_exempt { print NR ":" $0 }
    /IBMW_R7_EXEMPT_END/   { in_exempt=0 }
  ' "$file" \
    | grep -E --no-messages "$pattern" 2>/dev/null \
    | sed "s|^|$rule: $file:|" \
    >> "$VIOLATIONS_RAW"
  local st=("${PIPESTATUS[@]}")
  [[ ${st[0]} -eq 0 && ${st[1]} -le 1 ]] || scan_failed "$rule: $file (${st[*]})"
}
for f in "$API_DIR/pkg/"*.h "$API_DIR/pkg/"*.cc "$API_DIR/c/"*.h; do
  [[ "$f" == "$PKG_MAIN" ]] && continue
  grep_pkg_with_exempt "R7" "$f" 'driver_name'
done

# (d) user-facing 不应出现 driver_name 字面 dispatch
R7_USER_PAT='driver_name[[:space:]]*==[[:space:]]*"(iceoryx|shm|dds|ros2|json|mqtt|net|zenoh|local|tcp|http|udp)"'
grep_user_facing "R7" "$R7_USER_PAT"

#─────────────────────────────────────────────
# 10. R9 — Transport 初始化样板红线
#─────────────────────────────────────────────
emit_rule "R9" "HIGH" "user-facing 不得直接调 EnsurePodTopic / RegisterType / typesupport_register"
R9_PAT='EnsurePodTopic|RegisterType[[:space:]]*\(|register_type[[:space:]]*\(|typesupport_register'
grep_user_facing "R9" "$R9_PAT"

#─────────────────────────────────────────────
# 11. R11 — Publisher / Subscriber 模板单一化（strict）
#─────────────────────────────────────────────
emit_rule "R11" "HIGH" "yaml 不得按 transport 分裂 module_type（strict）"
# 禁字段：module_type: dds_publisher / iceoryx_publisher / shm_publisher / ros2_publisher / json_publisher
# 允许：publisher / subscriber / loaned_publisher / loaned_subscriber / direct_<transport>_*
R11_PAT='^[[:space:]]*module_type[[:space:]]*:[[:space:]]*(dds|iceoryx|shm|ros2|json)_(publisher|subscriber|loaned_publisher|loaned_subscriber)[[:space:]]*$'
# R11 strict：命中直接计入 $VIOLATIONS_RAW，与 R1-R9 共用 strict / baseline 判定
grep_yaml "R11" "$R11_PAT"

#─────────────────────────────────────────────
# 11.5. R8 — LoanedSample<...> + SenderAccess<...>::Send( 必须在 IBMW_HAS_* 守卫内
#─────────────────────────────────────────────
# File-level portability rule:
#   File-level rule. A user-facing file that uses BOTH the loaned-sample API and
#   the SenderAccess::Send escape hatch outside any compile-time transport-feature
#   guard is mixing the SHM-required path with the always-on path — a portability
#   bug. Awk strips #if IBMW_HAS_* / #ifdef IBMW_HAS_* / defined(IBMW_HAS_*) blocks
#   (including matching #else / #elif / #endif) before grepping the residual.
#   Severity HIGH; counted in strict total.
emit_rule "R8" "HIGH" "user-facing 不得在无 IBMW_HAS_* 守卫的代码区同时使用 LoanedSample<> + SenderAccess<>::Send("

# 提取去掉 IBMW_HAS_* 守卫块后的"裸代码"，输出 "<file>:<line>:<content>"
strip_ibmw_has_guards() {
  awk '
    BEGIN { depth = 0 }
    /^[[:space:]]*#[[:space:]]*(if|ifdef)[[:space:]]+IBMW_HAS_[A-Z_]+/   { depth++; next }
    /^[[:space:]]*#[[:space:]]*if[[:space:]]+defined[[:space:]]*\([[:space:]]*IBMW_HAS_[A-Z_]+/ { depth++; next }
    /^[[:space:]]*#[[:space:]]*if[[:space:]]+!defined[[:space:]]*\([[:space:]]*IBMW_HAS_[A-Z_]+/ { depth++; next }
    /^[[:space:]]*#[[:space:]]*endif/ { if (depth > 0) { depth--; next } else { print NR ":" $0 } ; next }
    /^[[:space:]]*#[[:space:]]*(else|elif)/ { if (depth > 0) next }
    { if (depth == 0) print NR ":" $0 }
  ' "$1"
}

R8_HITS="$(mktemp)"
if [[ "$USER_FACING_COUNT" -gt 0 ]]; then
  while IFS= read -r f; do
    [[ -z "$f" || ! -f "$f" ]] && continue

    # Honor repeatable --exclude flag (parity with scan_rule()).
    excluded=0
    for ex in "${EXCLUDE_FILES[@]+"${EXCLUDE_FILES[@]}"}"; do
      if [[ "$f" == *"$ex"* ]]; then
        excluded=1
        break
      fi
    done
    [[ $excluded -eq 1 ]] && continue

    stripped="$(strip_ibmw_has_guards "$f")" || { scan_failed "R8: $f"; continue; }
    has_loaned=$(echo "$stripped" | grep -cE 'LoanedSample[[:space:]]*<' 2>/dev/null || true)
    has_send=$(echo "$stripped"   | grep -cE 'SenderAccess[[:space:]]*<[^>]*>[[:space:]]*::[[:space:]]*Send[[:space:]]*\(' 2>/dev/null || true)
    if [[ "$has_loaned" -gt 0 && "$has_send" -gt 0 ]]; then
      # 同一文件命中两个 pattern 才视为违规；行号取首个 LoanedSample 命中
      first_loaned_line=$(echo "$stripped" | grep -nE 'LoanedSample[[:space:]]*<' 2>/dev/null | head -1 | cut -d: -f1)
      first_loaned_content=$(echo "$stripped" | grep -E 'LoanedSample[[:space:]]*<' 2>/dev/null | head -1)
      echo "R8: $f:${first_loaned_line:-?}: (file mixes LoanedSample<> + SenderAccess<>::Send( outside IBMW_HAS_*)" \
        >> "$VIOLATIONS_RAW"
    fi
  done < "$USER_FACING_LIST"
fi

#─────────────────────────────────────────────
# 11.6. R12 — IBMW_HAS_* 不得用作运行时表达式（STRICT）
#─────────────────────────────────────────────
# Strict compile-time-selector rule:
#   The IBMW_HAS_<TRANSPORT> macros are compile-time selectors. Using them as
#   runtime values (if / ternary / function arg) defeats the link-time elision
#   they are designed to enable. The check excludes lines starting with
#   #if / #ifdef / #elif (those are legitimate compile-time uses) and lines
#   inside defined(...) (also compile-time). Severity HIGH; counted in strict total.
emit_rule "R12" "HIGH" "user-facing 不得把 IBMW_HAS_* 当作运行时表达式（STRICT）"

if [[ "$USER_FACING_COUNT" -gt 0 ]]; then
  while IFS= read -r f; do
    [[ -z "$f" || ! -f "$f" ]] && continue

    # Honor repeatable --exclude flag (parity with scan_rule()).
    excluded=0
    for ex in "${EXCLUDE_FILES[@]+"${EXCLUDE_FILES[@]}"}"; do
      if [[ "$f" == *"$ex"* ]]; then
        excluded=1
        break
      fi
    done
    [[ $excluded -eq 1 ]] && continue

    grep -nHE 'IBMW_HAS_[A-Z_]+' "$f" 2>/dev/null \
      | grep -vE ':[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif)[[:space:]]+' \
      | grep -vE 'defined[[:space:]]*\([[:space:]]*IBMW_HAS_' \
      | sed 's|^|R12: |' \
      >> "$VIOLATIONS_RAW"
    [[ ${PIPESTATUS[0]} -le 1 ]] || scan_failed "R12: $f"
  done < "$USER_FACING_LIST"
fi

#─────────────────────────────────────────────
# 12. 输出与判定
#─────────────────────────────────────────────
# 提取真正的违规行（"R<N>: ..." 开头），排除 emit_rule 写的标题行
VIOLATION_LINES_FILE="$(mktemp)"
grep -E '^R[0-9]+: ' "$VIOLATIONS_RAW" > "$VIOLATION_LINES_FILE" 2>/dev/null || true
VIOLATION_COUNT=$(wc -l < "$VIOLATION_LINES_FILE")

# 报告头
echo "═══════════════════════════════════════════════════════════"
echo " IBMW Red-Line Check (R1-R12)  mode=$MODE"
echo " repo: $IBMW_ROOT"
echo " user-facing files scanned: $USER_FACING_COUNT"
echo " timestamp: $(date '+%Y-%m-%d %H:%M:%S')"
echo "═══════════════════════════════════════════════════════════"

# 逐 rule 块输出
cat "$VIOLATIONS_RAW"

echo ""
echo "═══════════════════════════════════════════════════════════"
echo " Summary"
echo "   R1-R12 violations   : $VIOLATION_COUNT"
echo "═══════════════════════════════════════════════════════════"

if [[ "$SCAN_ERRORS" -gt 0 ]]; then
  echo "[$MODE] ERROR: $SCAN_ERRORS scan step(s) failed; the result above is incomplete." >&2
  exit 64
fi

# 模式判定
case "$MODE" in
  report)
    echo "[--report] exit 0 regardless of violations."
    exit 0
    ;;
  baseline)
    # 比较：当前违规集 vs baseline 违规集
    #
    # 路径无关化（path-agnostic diff）：违规行格式为
    #   "R<N>: <abspath>:<line>:<content>"
    # 不同 checkout 路径会让 diff 把所有项判为 "new"。
    # 会让 diff 把所有项判为 "new"。规范化方式：将 "<任意前缀>/src/" 折叠为
    # "src/"，因为所有受检文件都在 src/ 子树下（samples/api/extensions/tools）。
    # 这样比对就只看 repo-relative 路径 + 行号 + 内容三元组。
    normalize_paths() {
      sed -E -e 's|^(R[0-9]+:[[:space:]]+)[^:]*/src/|\1src/|' \
             -e 's|^(R[0-9]+:[[:space:]]+)[^:]*/ibmw-template/|\1ibmw-template/|'
    }
    BASELINE_LINES_FILE="$(mktemp)"
    grep -E '^R[0-9]+: ' "$BASELINE_FILE" 2>/dev/null | normalize_paths > "$BASELINE_LINES_FILE" || true
    CURRENT_LINES_NORM="$(mktemp)"
    normalize_paths < "$VIOLATION_LINES_FILE" > "$CURRENT_LINES_NORM"
    VIOLATIONS_NEW="$(mktemp)"
    # 排序后 comm，找出当前有、baseline 无的行
    comm -23 <(sort -u "$CURRENT_LINES_NORM") <(sort -u "$BASELINE_LINES_FILE") > "$VIOLATIONS_NEW"
    NEW_COUNT=$(wc -l < "$VIOLATIONS_NEW")
    if [[ "$NEW_COUNT" -gt 0 ]]; then
      echo ""
      echo "── NEW violations vs baseline ($BASELINE_FILE) ──"
      cat "$VIOLATIONS_NEW"
      echo ""
      echo "[--baseline] FAIL: $NEW_COUNT new violation(s) introduced."
      exit 2
    fi
    echo "[--baseline] OK: no new violations vs baseline."
    exit 0
    ;;
  strict|*)
    if [[ "$VIOLATION_COUNT" -gt 0 ]]; then
      echo ""
      echo "[--strict] FAIL: $VIOLATION_COUNT R1-R12 violation(s) detected."
      echo "  Use '--report' to view without blocking."
      echo "  Use '--baseline <file>' to gate only on new regressions."
      exit 1
    fi
    echo "[--strict] OK: all R1-R12 red-lines green."
    exit 0
    ;;
esac
