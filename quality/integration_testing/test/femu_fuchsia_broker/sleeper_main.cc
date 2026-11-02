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
#include <zircon/syscalls.h>

// Stand-in process for a skeleton or proxy holder: it only exists so that the test has a real process to kill. A handle
// the test passed as PA_USER0 (one end of the liveness eventpair) stays in its handle table and closes with the process.
int main()
{
    zx_nanosleep(ZX_TIME_INFINITE);
    return 0;
}
