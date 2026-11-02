/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
void Report(const char* what, const long result)
{
    const int saved_errno = errno;
    std::printf("PROBE %s result=%ld errno=%d (%s)\n",
                what,
                result,
                result < 0 ? saved_errno : 0,
                result < 0 ? std::strerror(saved_errno) : "ok");
}

void ProbeMapping(const char* what, const int fd)
{
    Report(what, fd);
    if (fd < 0)
    {
        return;
    }
    Report("ftruncate", ::ftruncate(fd, 4096));
    Report("fchmod", ::fchmod(fd, 0600));
    Report("flock(LOCK_EX|LOCK_NB)", ::flock(fd, LOCK_EX | LOCK_NB));
    Report("fcntl(F_GETFL)", ::fcntl(fd, F_GETFL));
    void* const address = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Report("mmap(MAP_SHARED)", address == MAP_FAILED ? -1 : 0);
    if (address != MAP_FAILED)
    {
        std::memset(address, 0x5a, 4096);
        Report("munmap", ::munmap(address, 4096));
    }
    ::close(fd);
}
}  // namespace

int main()
{
    ProbeMapping("shm_open(/probe)", ::shm_open("/probe", O_CREAT | O_EXCL | O_RDWR, 0600));
    Report("shm_unlink(/probe)", ::shm_unlink("/probe"));
    ProbeMapping("memfd_create", ::memfd_create("probe", 0));
    ProbeMapping("open(/tmp/probe_file)", ::open("/tmp/probe_file", O_CREAT | O_RDWR, 0600));
    Report("unlink(/tmp/probe_file)", ::unlink("/tmp/probe_file"));
    ProbeMapping("open(/dev/shm/probe_file)", ::open("/dev/shm/probe_file", O_CREAT | O_RDWR, 0600));
    std::printf("PROBE done\n");
    return 0;
}
