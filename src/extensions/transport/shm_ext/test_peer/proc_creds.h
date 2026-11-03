// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// PrintProcCreds(out, bin) prints one line with the ids and capability sets of
// the calling process, read from /proc/self/status:
//   PROC_CREDS bin=<bin> uid=<r,e,s,fs> gid=<r,e,s,fs> groups=<g,...> CapInh=.. CapPrm=.. CapEff=.. CapAmb=..
// The OHOS device scripts run the test binaries under a lowered uid and check
// this line, i.e. what the process itself holds after exec, not what the
// launcher set up before it.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

namespace ibmw::extensions::shm_extension::test {

inline void PrintProcCreds(std::FILE* out, const char* bin) {
  const char* keys[] = {"Uid", "Gid", "Groups", "CapInh", "CapPrm", "CapEff", "CapAmb"};
  const char* names[] = {"uid", "gid", "groups", "CapInh", "CapPrm", "CapEff", "CapAmb"};
  std::string values[7];
  if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
      for (int i = 0; i < 7; ++i) {
        const size_t k = std::strlen(keys[i]);
        if (std::strncmp(line, keys[i], k) != 0 || line[k] != ':') continue;
        // Fields are separated by tabs or spaces: join them with commas.
        for (const char* p = line + k + 1; *p != '\0' && *p != '\n';) {
          const size_t gap = std::strspn(p, " \t");
          p += gap;
          const size_t len = std::strcspn(p, " \t\n");
          if (len == 0) break;
          if (!values[i].empty()) values[i] += ',';
          values[i].append(p, len);
          p += len;
        }
      }
    }
    std::fclose(f);
  }
  std::string text = std::string("PROC_CREDS bin=") + bin;
  for (int i = 0; i < 7; ++i) text += std::string(" ") + names[i] + "=" + values[i];
  // One write(): stderr is unbuffered and shared with other output.
  text += '\n';
  std::fflush(out);
  (void)!::write(fileno(out), text.data(), text.size());
}

}  // namespace ibmw::extensions::shm_extension::test
