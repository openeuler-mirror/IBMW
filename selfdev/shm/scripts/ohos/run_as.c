// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Runs a command as an ordinary uid: run_as <uid> <gid> <deny_dir> <cmd> [args...]
// run_as --caps prints the ids and capabilities of the calling process and
// exits: run as <cmd> it shows what an exec'ed file from the same directory gets.
//
// Must start as root. Takes uid/gid with no supplementary groups, then clears
// the effective, permitted, inheritable and ambient capabilities: the HongMeng
// kernel (OHOS) keeps them across setresuid, where Linux would drop them, and
// CAP_DAC_OVERRIDE alone would let the command into any directory. Before exec
// it checks the result and prints one RUN_AS line; <deny_dir> must be a
// root-owned 0700 directory in which root can create a file (positive control)
// and the lowered process cannot (EACCES).
// Exit 125 on any failure (nothing is run). The SELinux domain is not changed
// (stays that of the caller, e.g. su).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Parses a nonzero decimal id below 2^31 (no sign, no trailing characters).
static int parse_id(const char* text, unsigned long* out) {
  char* end = NULL;
  errno = 0;
  if (text[0] < '1' || text[0] > '9') return -1;
  *out = strtoul(text, &end, 10);
  return (errno != 0 || *end != '\0' || *out > 0x7fffffffUL) ? -1 : 0;
}

static int die(const char* what) {
  fprintf(stderr, "RUN_AS result=FAIL step=%s errno=%d(%s)\n", what, errno, strerror(errno));
  return 125;
}

// Copies the value of "<key>:\t..." from /proc/self/status into out.
static void status_field(const char* key, char* out, size_t n) {
  snprintf(out, n, "?");
  FILE* f = fopen("/proc/self/status", "r");
  if (f == NULL) return;
  char line[256];
  size_t k = strlen(key);
  while (fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, key, k) == 0 && line[k] == ':') {
      char* v = line + k + 1;
      v += strspn(v, " \t");
      v[strcspn(v, "\n")] = '\0';
      snprintf(out, n, "%s", v);
      break;
    }
  }
  fclose(f);
}

// One line with the ids, capability sets and SELinux context of this process.
static void print_state(const char* tag, const char* extra) {
  char cap_inh[64], cap_eff[64], cap_prm[64], cap_bnd[64], cap_amb[64], context[128] = "?";
  status_field("CapInh", cap_inh, sizeof cap_inh);
  status_field("CapEff", cap_eff, sizeof cap_eff);
  status_field("CapPrm", cap_prm, sizeof cap_prm);
  status_field("CapBnd", cap_bnd, sizeof cap_bnd);
  status_field("CapAmb", cap_amb, sizeof cap_amb);
  FILE* f = fopen("/proc/self/attr/current", "r");
  if (f != NULL) {
    if (fgets(context, sizeof context, f) == NULL) snprintf(context, sizeof context, "?");
    context[strcspn(context, "\n")] = '\0';
    fclose(f);
  }
  printf("%s uid=%u euid=%u gid=%u egid=%u groups=%d CapInh=%s CapEff=%s CapPrm=%s CapBnd=%s CapAmb=%s %scontext=%s\n",
         tag, (unsigned)getuid(), (unsigned)geteuid(), (unsigned)getgid(), (unsigned)getegid(), getgroups(0, NULL),
         cap_inh, cap_eff, cap_prm, cap_bnd, cap_amb, extra, context);
  fflush(stdout);
}

int main(int argc, char** argv) {
  if (argc == 2 && strcmp(argv[1], "--caps") == 0) {
    print_state("RUN_AS_CAPS", "");
    return 0;
  }
  if (argc < 5) {
    fprintf(stderr, "usage: run_as <uid> <gid> <deny_dir> <cmd> [args...]\n");
    return 125;
  }
  unsigned long uid_value, gid_value;
  if (parse_id(argv[1], &uid_value) != 0 || parse_id(argv[2], &gid_value) != 0) {
    errno = EINVAL;
    return die("ids-not-nonzero-decimal");
  }
  const uid_t uid = (uid_t)uid_value;
  const gid_t gid = (gid_t)gid_value;
  const char* deny = argv[3];
  struct stat st;
  if (stat(deny, &st) != 0) return die("stat-deny-dir");
  if (st.st_uid != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 077) != 0) {
    errno = EINVAL;
    return die("deny-dir-not-root-0700");
  }
  // Positive control: still root, the probe can be created, so EACCES later
  // comes from the dropped privileges and not from the directory or the path.
  // One name per process: the runner starts two run_as at the same time.
  char probe[4096];
  const int probe_len = snprintf(probe, sizeof probe, "%s/run_as_probe.%d", deny, (int)getpid());
  if (probe_len < 0 || (size_t)probe_len >= sizeof probe) {
    errno = ENAMETOOLONG;
    return die("deny-path-too-long");
  }
  const int root_fd = open(probe, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (root_fd < 0) return die("deny-dir-root-create");
  close(root_fd);
  if (unlink(probe) != 0) return die("deny-dir-root-unlink");

  // The bounding set and PR_SET_NO_NEW_PRIVS are left as for any process: on the
  // OHOS device an empty bounding set makes exec of toybox fail with EPERM (cause
  // not confirmed; toybox gains capabilities on exec, see the test document).
  // Ambient is cleared first; it is checked again below with the other sets.
  if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0 && errno != EINVAL) return die("ambient-clear");
  if (setgroups(0, NULL) != 0) return die("setgroups");
  if (setresgid(gid, gid, gid) != 0) return die("setresgid");
  if (setresuid(uid, uid, uid) != 0) return die("setresuid");

  struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct data[2];
  memset(data, 0, sizeof data);
  if (syscall(SYS_capset, &header, data) != 0) return die("capset");
  struct __user_cap_data_struct now[2];
  memset(now, 0, sizeof now);
  if (syscall(SYS_capget, &header, now) != 0) return die("capget");

  uid_t r, e, s;
  gid_t rg, eg, sg;
  if (getresuid(&r, &e, &s) != 0 || getresgid(&rg, &eg, &sg) != 0) return die("getres");
  const int groups = getgroups(0, NULL);
  const unsigned caps = now[0].effective | now[1].effective | now[0].permitted | now[1].permitted |
                        now[0].inheritable | now[1].inheritable;
  char cap_amb[64];
  status_field("CapAmb", cap_amb, sizeof cap_amb);
  if (r != uid || e != uid || s != uid || rg != gid || eg != gid || sg != gid || groups != 0 || caps != 0 ||
      strcmp(cap_amb, "0000000000000000") != 0) {
    errno = EPERM;
    return die("verify-ids-caps");
  }

  // The DAC check the dropped capabilities would bypass.
  const int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  const int deny_errno = errno;
  if (fd >= 0) {
    close(fd);
    unlink(probe);
    errno = EPERM;
    return die("deny-dir-writable");
  }
  if (deny_errno != EACCES) {
    errno = deny_errno;
    return die("deny-dir-unexpected-errno");
  }

  print_state("RUN_AS", "deny=EACCES result=PASS ");
  execvp(argv[4], argv + 4);
  return die("exec");
}
