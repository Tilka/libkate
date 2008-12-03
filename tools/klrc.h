/* Copyright (C) 2008 Vincent Penquerc'h.
   This file is part of the Kate codec library.
   Written by Vincent Penquerc'h.

   Use, distribution and reproduction of this library is governed
   by a BSD style source license included with this source in the
   file 'COPYING'. Please read these terms before distributing. */


#ifndef KATE_klrc_h_GUARD
#define KATE_klrc_h_GUARD

struct lrc_data {
  kate_float last_event_end_time;
};

extern void write_lrc_event(FILE *fout,void *data,const kate_event *ev,ogg_int64_t granpos,int event_index);
extern void write_lrc_end(FILE *fout);

#endif
