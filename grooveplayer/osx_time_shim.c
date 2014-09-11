/*
 * Copyright (c) 2014 K. Ernest "iFire" Lee
 * Copyright (c) 2014 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "osx_time_shim.h"
#include <mach/mach_time.h>

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
    clock_serv_t cclock;
    mach_timespec_t mts;

    host_get_clock_service(mach_host_self(), clk_id, &cclock);
    kern_return_t retval = clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);

    tp->tv_sec = mts.tv_sec;
    tp->tv_nsec = mts.tv_nsec;

    return retval;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int foo) {
  return 0;
}
