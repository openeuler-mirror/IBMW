// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Probes the kernel features selfdev shm depends on. Prints one line per feature.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/magic.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
static void report(const char* name, int ok) { int e = errno; printf("%-34s %s errno=%d(%s)\n", name, ok ? "OK  " : "FAIL", ok ? 0 : e, ok ? "" : strerror(e)); }
#define R(name, ok) report(name, (ok))
static long fx(atomic_uint* a, int op, unsigned v, const struct timespec* t) { return syscall(SYS_futex, a, op, v, t, NULL, 0); }
static void send_fd(int s, int fd) {
  char b = 'x'; struct iovec io = {&b, 1}; char c[CMSG_SPACE(sizeof(int))]; memset(c, 0, sizeof c);
  struct msghdr m = {0}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = c; m.msg_controllen = sizeof c;
  struct cmsghdr* h = CMSG_FIRSTHDR(&m); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(h), &fd, sizeof fd); sendmsg(s, &m, 0);
}
static int recv_fd(int s) {
  char b; struct iovec io = {&b, 1}; char c[CMSG_SPACE(sizeof(int))]; struct msghdr m = {0};
  m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = c; m.msg_controllen = sizeof c;
  if (recvmsg(s, &m, 0) < 1) return -1;
  struct cmsghdr* h = CMSG_FIRSTHDR(&m); if (!h || h->cmsg_type != SCM_RIGHTS) return -1;
  int fd; memcpy(&fd, CMSG_DATA(h), sizeof fd); return fd;
}
int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : ".";
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("uid=%d euid=%d pagesize=%ld\n", getuid(), geteuid(), sysconf(_SC_PAGESIZE));
  int mfd = syscall(SYS_memfd_create, "probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  R("memfd_create(ALLOW_SEALING)", mfd >= 0);
  if (mfd < 0) return 1;
  R("ftruncate", ftruncate(mfd, 1 << 20) == 0);
  struct statfs sf; int r = fstatfs(mfd, &sf);
  printf("%-34s f_type=0x%lx tmpfs=%d rc=%d\n", "fstatfs(memfd)", (unsigned long)sf.f_type, sf.f_type == TMPFS_MAGIC, r);
  struct stat st; fstat(mfd, &st); printf("%-34s mode=%o nlink=%ld st_dev=%ld\n", "fstat(memfd)", st.st_mode, (long)st.st_nlink, (long)st.st_dev);
  void* w = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
  R("mmap(RW,SHARED) before seal", w != MAP_FAILED);
  R("F_SEAL_SHRINK|GROW|SEAL", fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0);
  R("F_SEAL_FUTURE_WRITE", fcntl(mfd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
  printf("%-34s 0x%x\n", "F_GET_SEALS", fcntl(mfd, F_GET_SEALS));
  errno = 0; void* w2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
  printf("%-34s %s errno=%d\n", "RW mmap after FUTURE_WRITE refused", w2 == MAP_FAILED ? "OK  " : "NOT-REFUSED", errno);
  void* ro = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0);
  R("RO mmap after seal", ro != MAP_FAILED);
  if (ro != MAP_FAILED) { errno = 0; int m = mprotect(ro, 4096, PROT_READ | PROT_WRITE); printf("%-34s %s errno=%d\n", "mprotect RO->RW refused", m != 0 ? "OK  " : "NOT-REFUSED", errno); }
  if (w != MAP_FAILED) { memcpy(w, "seal", 5); printf("%-34s %s\n", "old RW mapping still writes", memcmp(ro, "seal", 5) == 0 ? "OK  " : "FAIL"); }

  // Cross-process shared memory + SCM_RIGHTS + shared futex, over a named unix socket.
  char path[256]; snprintf(path, sizeof path, "%s/probe.%d.sock", dir, getpid()); unlink(path);
  int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); R("socket(AF_UNIX,SEQPACKET,NONBLOCK)", ls >= 0);
  struct sockaddr_un ad = {0}; ad.sun_family = AF_UNIX; snprintf(ad.sun_path, sizeof ad.sun_path, "%s", path);
  R("bind(path)", bind(ls, (struct sockaddr*)&ad, sizeof ad) == 0); R("listen", listen(ls, 128) == 0);
  atomic_uint* shared = (atomic_uint*)w; atomic_store(shared + 16, 0); atomic_store(shared + 32, 0);
  pid_t kid = fork(); R("fork", kid >= 0);
  if (kid == 0) {
    int c = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (connect(c, (struct sockaddr*)&ad, sizeof ad) != 0) _exit(10);
    int fd = recv_fd(c); if (fd < 0) _exit(11);
    atomic_uint* m = mmap(NULL, 1 << 20, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) _exit(12);
    // Writable mapping must be refused for the child as well.
    if (mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED) write(c, "W", 1);
    else write(c, "R", 1);
    // Wait on a read-only mapping (non-PRIVATE futex), parent wakes it.
    while (atomic_load(m + 16) == 0) fx(m + 16, FUTEX_WAIT, 0, NULL);
    write(c, "F", 1);
    pause();
    _exit(0);
  }
  int ep = epoll_create1(EPOLL_CLOEXEC); R("epoll_create1", ep >= 0);
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = ls}; R("epoll_ctl", epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev) == 0);
  struct epoll_event out; int n = epoll_wait(ep, &out, 1, 3000); R("epoll_wait(listen ready)", n == 1);
  int a = accept4(ls, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC); R("accept4", a >= 0);
#ifdef SO_PEERCRED
  struct ucred uc; socklen_t ul = sizeof uc; int pc = getsockopt(a, SOL_SOCKET, SO_PEERCRED, &uc, &ul);
  printf("%-34s rc=%d pid=%d(want %d) uid=%d\n", "SO_PEERCRED", pc, uc.pid, kid, uc.uid);
#endif
  send_fd(a, mfd);
  int fl = fcntl(a, F_GETFL); fcntl(a, F_SETFL, fl & ~O_NONBLOCK);
  struct timeval tv = {5, 0}; setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char rc = 0; ssize_t g = read(a, &rc, 1); printf("%-34s %c (R=refused,W=NOT refused) n=%zd\n", "SCM_RIGHTS peer RW mmap", rc, g);
  usleep(100000); atomic_store(shared + 16, 1); long woke = fx(shared + 16, FUTEX_WAKE, 1, NULL);
  g = read(a, &rc, 1); printf("%-34s %s woke=%ld got=%c\n", "shared futex cross-process wake", (g == 1 && rc == 'F') ? "OK  " : "FAIL", woke, g == 1 ? rc : '-');
  errno = 0; struct timespec to = {0, 20000000}; atomic_store(shared + 32, 0); long tw = fx(shared + 32, FUTEX_WAIT, 0, &to);
  printf("%-34s %s errno=%d\n", "futex timed wait ETIMEDOUT", (tw < 0 && errno == ETIMEDOUT) ? "OK  " : "FAIL", errno);
  // Peer liveness primitives used for quarantine decisions.
  errno = 0; int k0 = kill(kid, 0); printf("%-34s rc=%d errno=%d\n", "kill(pid,0) live peer", k0, errno);
#ifdef SYS_pidfd_open
  errno = 0; long pf = syscall(SYS_pidfd_open, kid, 0); printf("%-34s rc=%ld errno=%d\n", "pidfd_open", pf, errno);
#endif
  // Peer crash: hangup must arrive.
  struct epoll_event ev2 = {.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP, .data.fd = a}; epoll_ctl(ep, EPOLL_CTL_ADD, a, &ev2);
  kill(kid, SIGKILL); int status; waitpid(kid, &status, 0);
  n = epoll_wait(ep, &out, 1, 3000); printf("%-34s n=%d events=0x%x\n", "EPOLLHUP after SIGKILL", n, n > 0 ? out.events : 0);
  errno = 0; k0 = kill(kid, 0); printf("%-34s rc=%d errno=%d\n", "kill(pid,0) dead peer", k0, errno);
  // Socket backpressure: MSG_DONTWAIT must give EAGAIN, not block.
  int sp[2]; socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp); char blob[1024] = {0}; int sent = 0; ssize_t sr;
  while ((sr = send(sp[0], blob, sizeof blob, MSG_DONTWAIT | MSG_NOSIGNAL)) > 0 && sent < 1000000) sent++;
  printf("%-34s after %d msgs errno=%d(%s)\n", "SEQPACKET backpressure", sent, errno, strerror(errno));
  // inotify on the discovery directory.
  int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); R("inotify_init1", in >= 0);
  if (in >= 0) {
    int wd = inotify_add_watch(in, dir, IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_DELETE_SELF); R("inotify_add_watch", wd >= 0);
    char f[300]; snprintf(f, sizeof f, "%s/probe.%d.new", dir, getpid()); int t = open(f, O_CREAT | O_WRONLY, 0600); close(t); unlink(f);
    usleep(100000); char ib[4096]; ssize_t il = read(in, ib, sizeof ib); printf("%-34s bytes=%zd errno=%d\n", "inotify events delivered", il, il < 0 ? errno : 0);
    // A socket bound in the directory must fire the same events as a file.
    char s2[300]; snprintf(s2, sizeof s2, "%s/p%d.s", dir, getpid()); int ss = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a2 = {0}; a2.sun_family = AF_UNIX; snprintf(a2.sun_path, sizeof a2.sun_path, "%s", s2); bind(ss, (struct sockaddr*)&a2, sizeof a2);
    usleep(100000); il = read(in, ib, sizeof ib); printf("%-34s bytes=%zd errno=%d\n", "inotify for socket bind", il, il < 0 ? errno : 0); close(ss); unlink(s2);
  }
  unlink(path);
  // Atomics in shared mapping via cmpxchg (LSE/LL-SC).
  atomic_uint v = 0; unsigned e = 0; printf("%-34s %d\n", "CAS", atomic_compare_exchange_strong(&v, &e, 1));
  puts("done");
  return 0;
}
