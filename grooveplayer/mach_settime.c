/*
 * Copyright (c) 2014 K. Ernest "iFire" Lee
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "mach_settime.h"
int 
pthread_condattr_setclock(pthread_condattr_t *attr, int foo) 
{ 
  (void)attr; 
  (void)foo; 
  return (0); 
} 

