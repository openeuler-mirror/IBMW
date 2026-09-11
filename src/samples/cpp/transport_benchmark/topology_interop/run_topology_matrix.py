#!/usr/bin/env python3
"""Run the four-process ROS2 or DDS topology locally with bounded cleanup."""
import argparse, json, os, pathlib, signal, subprocess, time


def ticks(pid):
    fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(')', 1)[1].split()
    return fields[19]


def proc_cpu(pid):
    fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(')', 1)[1].split()
    return int(fields[11]) + int(fields[12])


def proc_memory(pid):
    values = {}
    for name in ('status', 'smaps_rollup'):
        try:
            for line in pathlib.Path(f"/proc/{pid}/{name}").read_text().splitlines():
                key, _, value = line.partition(':')
                if key in ('VmRSS', 'Pss'):
                    values[key] = int(value.strip().split()[0])
        except (OSError, ValueError):
            pass
    return values


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--endpoint', required=True)
    p.add_argument('--config-dir')
    p.add_argument('--work', default='topology-artifacts')
    p.add_argument('--timeout', type=float, default=45)
    p.add_argument('--hz', type=int, default=100)
    p.add_argument('--warmup-messages', type=int, default=50)
    p.add_argument('--message-count', type=int, default=3000)
    p.add_argument('--cpu-start-seconds', type=float, default=5)
    p.add_argument('--cpu-end-seconds', type=float, default=20)
    a = p.parse_args()
    if (a.timeout <= 0 or a.hz <= 0 or a.warmup_messages < 0 or
            a.message_count <= 0 or a.cpu_start_seconds < 0 or
            a.cpu_end_seconds <= a.cpu_start_seconds):
        p.error('timeout and hz must be positive; message counts must be valid')

    root = pathlib.Path(a.work).resolve()
    config_dir = pathlib.Path(a.config_dir).resolve() if a.config_dir else None
    if config_dir and not config_dir.is_dir():
        p.error(f'config directory does not exist: {config_dir}')
    run = f"topology-{os.getpid()}-{time.monotonic_ns()}"
    w = root / run
    w.mkdir(parents=True, exist_ok=False)
    base_env = os.environ.copy()
    base_env.update(MW_PERF_RUN_ID=run,
                    MW_PERF_ARTIFACT_SCHEMA_VERSION='mw_perf.topology.v1')
    procs = []
    src = None
    snapshots = {}

    def snapshot(label):
        data = {'label': label, 'monotonic_ns': time.monotonic_ns(), 'roles': {}}
        for role, q in zip(('source', 'relay', 'sink_a', 'sink_b'),
                           [x[0] for x in procs]):
            try:
                data['roles'][role] = {'pid': q.pid, 'cpu_ticks': proc_cpu(q.pid),
                                       **proc_memory(q.pid)}
            except (OSError, ValueError):
                data['roles'][role] = {'pid': q.pid}
        snapshots[label] = data
        (w / f'metrics-{label}.json').write_text(json.dumps(data, sort_keys=True) + '\n')

    def start(role, sink='', config_name=''):
        out = w / (role.lower() + (sink or '') + '.log')
        status = w / (role.lower() + (sink or '') + '.status.json')
        cmd = [str(pathlib.Path(a.endpoint).resolve()), '--role', role,
               '--hz', str(a.hz), '--warmup-messages', str(a.warmup_messages),
               '--message-count', str(a.message_count), '--status', str(status)]
        if sink:
            cmd += ['--sink-id', sink, '--csv', str(w / (sink + '.csv')),
                    '--jsonl', str(w / (sink + '.jsonl'))]
        if config_name:
            if config_dir is not None:
                cmd += ['--config', str(config_dir / config_name)]
        f = out.open('w')
        q = subprocess.Popen(cmd, env=base_env, stdout=f, stderr=subprocess.STDOUT,
                             cwd=pathlib.Path(a.endpoint).resolve().parent)
        procs.append((q, f))
        return q

    def stop_all(deadline):
        if len(procs) > 1 and procs[1][0].poll() is None:
            procs[1][0].terminate()
        for q, _ in procs:
            remaining = max(0, deadline - time.monotonic())
            try:
                q.wait(remaining)
            except subprocess.TimeoutExpired:
                q.terminate()
        for q, _ in procs:
            if q.poll() is None:
                try:
                    q.wait(max(0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    q.kill()
                    q.wait()
        for q, f in procs:
            f.close()

    try:
        # Stop source after fork so its identity can be handed to sinks before
        # source resumes and publishes. This avoids fabricating producer IDs.
        src = start('source', config_name='publisher.yaml')
        source_env = base_env.copy()
        source_env.update(MW_PERF_PRODUCER_PID=str(src.pid),
                          MW_PERF_PRODUCER_START_TIME_TICKS=ticks(src.pid))
        # Popen receives an environment snapshot, so the source identity must
        # be included when launching every consumer rather than mutating a
        # dictionary after the source has already started.
        base_env = source_env
        os.kill(src.pid, signal.SIGSTOP)
        start('relay', config_name='relay.yaml')
        start('sink', 'A', 'sink_a.yaml')
        start('sink', 'B', 'sink_b.yaml')
        os.kill(src.pid, signal.SIGCONT)
        started = time.monotonic()
        while time.monotonic() - started < a.cpu_end_seconds:
            elapsed = time.monotonic() - started
            if elapsed >= a.cpu_start_seconds and 'start' not in snapshots:
                snapshot('start')
            time.sleep(min(0.2, a.cpu_end_seconds - elapsed))
        if 'end' not in snapshots:
            snapshot('end')
        while any(q.poll() is None for q, _ in procs[0:1] + procs[2:]):
            if time.monotonic() - started >= a.timeout:
                break
            time.sleep(0.2)
    finally:
        if src is not None and src.poll() is None:
            try:
                os.kill(src.pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
        stop_all(time.monotonic() + a.timeout)
    statuses = []
    for s in ('A', 'B'):
        try:
            d = json.loads((w / ('sink' + s + '.status.json')).read_text())
            lines = [json.loads(x) for x in (w / (s + '.jsonl')).read_text().splitlines() if x]
        except (OSError, json.JSONDecodeError):
            statuses.append(False)
            continue
        complete = (lines and lines[-1].get('kind') == 'completion' and
                    lines[-1].get('complete') is True)
        statuses.append(all(line.get('run_id') == run for line in lines) and
                        d.get('valid') is True and
                        d.get('unique_measured') == a.message_count and
                        complete and len(lines) - 1 == a.message_count)
    # The relay has no finite workload and is intentionally terminated after
    # source and sinks finish; source and both sinks must exit cleanly.
    if 'start' in snapshots and 'end' in snapshots:
        hz = os.sysconf(os.sysconf_names['SC_CLK_TCK'])
        elapsed = (snapshots['end']['monotonic_ns'] - snapshots['start']['monotonic_ns']) / 1e9
        metrics = {'elapsed_seconds': elapsed, 'clk_tck': hz, 'roles': {}}
        for role in snapshots['start']['roles']:
            before, after = snapshots['start']['roles'][role], snapshots['end']['roles'][role]
            metrics['roles'][role] = {
                'cpu_pct': ((after.get('cpu_ticks', 0) - before.get('cpu_ticks', 0)) / hz / elapsed * 100),
                'rss_kb': before.get('VmRSS'), 'pss_kb': before.get('Pss')}
        metrics['total_cpu_pct'] = sum(x['cpu_pct'] for x in metrics['roles'].values())
        (w / 'metrics.json').write_text(json.dumps(metrics, sort_keys=True) + '\n')
    exit_codes = dict(zip(('source', 'relay', 'sink_a', 'sink_b'),
                          (q.returncode for q, _ in procs)))
    (w / 'exit-codes.json').write_text(json.dumps(exit_codes, sort_keys=True) + '\n')
    exit_ok = (len(procs) == 4 and exit_codes['source'] == 0 and
               exit_codes['sink_a'] == 0 and exit_codes['sink_b'] == 0)
    return 0 if len(statuses) == 2 and all(statuses) and exit_ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
