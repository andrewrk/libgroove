/*
 * Copyright (c) 2014 K. Ernest "iFire" Lee
 * Copyright (c) 2014 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_MACH_TIME_H_INCLUDED
#define GROOVE_MACH_TIME_H_INCLUDED
#ifdef __APPLE__

#include <sys/types.h>
#include <sys/_types/_timespec.h>
#include <mach/mach.h>
#include <mach/clock.h>
#include <pthread.h>

#define CLOCK_MONOTONIC SYSTEM_CLOCK

typedef int clockid_t;

/* the mach kernel uses struct mach_timespec, so struct timespec
    is loaded from <sys/_types/_timespec.h> for compatability */

int clock_gettime(clockid_t clk_id, struct timespec *tp);
int pthread_condattr_setclock(pthread_condattr_t *attr, int foo);

#endif
#endif
