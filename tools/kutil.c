/* Copyright (C) 2008 Vincent Penquerc'h.
   This file is part of the Kate codec library.
   Written by Vincent Penquerc'h.

   Use, distribution and reproduction of this library is governed
   by a BSD style source license included with this source in the
   file 'COPYING'. Please read these terms before distributing. */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined WIN32 || defined _WIN32 || defined MSDOS || defined __CYGWIN__ || defined __EMX__ || defined OS2
#include <io.h>
#include <fcntl.h>
#endif
#if defined WIN32 || defined _WIN32
#include <process.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <errno.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <ogg/ogg.h>
#include "kate/oggkate.h"
#include "kate_internal.h"
#include "kutil.h"

int time_hours(kate_float t)
{
  return (int)(t/3600);
}

int time_minutes(kate_float t)
{
  int h=(int)(t/3600);
  t-=h*3600;
  return t/60;
}

kate_float time_float_seconds(kate_float t)
{
  int h,m;

  h=(int)(t/3600);
  t-=h*3600;
  m=(int)(t/60);
  t-=m*60;
  return t;
}

int time_seconds(kate_float t)
{
  int h,m;

  h=(int)(t/3600);
  t-=h*3600;
  m=(int)(t/60);
  t-=m*60;
  return t;
}

int time_milliseconds(kate_float t)
{
  return (int)(1000*(t-(float)(int)t));
}

const char *eat_arg(int argc,char **argv,int *n)
{
  if (*n==argc-1) {
    fprintf(stderr,"%s needs an argument\n",argv[*n]);
    exit(-1);
  }
  return argv[++*n];
}

