/*
 * Copyright (c) 2014 K. Ernest "iFire" Lee
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef mach_settime_h
#define mach_settime_h

#include <pthread.h>

int 
pthread_condattr_setclock(pthread_condattr_t *attr, int foo);
#endif

