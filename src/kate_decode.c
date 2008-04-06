/* Copyright (C) 2008 Vincent Penquerc'h.
   This file is part of the Kate codec library.
   Written by Vincent Penquerc'h.

   Use, distribution and reproduction of this library is governed
   by a BSD style source license included with this source in the
   file 'COPYING'. Please read these terms before distributing. */


#define KATE_INTERNAL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include <kate/kate.h>
#include "kate_internal.h"
#include "kate_decode_state.h"
#include "kate_fp.h"

typedef struct kate_memory_guard {
  size_t size;
  void **pointers;
} kate_memory_guard;

static void *kate_memory_guard_malloc(kate_memory_guard *kmg,size_t size)
{
  void **new_pointers;
  void *ptr=kate_malloc(size);
  if (!ptr) return NULL;
  new_pointers=(void**)kate_realloc(kmg->pointers,(kmg->size+1)*sizeof(void*));
  if (!new_pointers) {
    kate_free(ptr);
    return NULL;
  }
  kmg->pointers=new_pointers;
  kmg->pointers[kmg->size++]=ptr;
  return ptr;
}

static void kate_memory_guard_destroy(kate_memory_guard *kmg,int free_pointers)
{
  size_t n;
  if (free_pointers) {
    for (n=0;n<kmg->size;++n) kate_free(kmg->pointers[n]);
  }
  if (kmg->pointers) kate_free(kmg->pointers);
}

#define KMG_GUARD() kate_memory_guard kmg={0,NULL}
#define KMG_MALLOC(size) kate_memory_guard_malloc(&kmg,size)
#define KMG_ERROR(ret) (kate_memory_guard_destroy(&kmg,1),ret)
#define KMG_OK() (kate_memory_guard_destroy(&kmg,0),0)



static void kate_readbuf(oggpack_buffer *opb,char *s,int len)
{
  while (len--) *s++=oggpack_read(opb,8);
}

static kate_int32_t kate_read32(oggpack_buffer *opb)
{
  kate_int32_t v=0;
  v|=oggpack_read(opb,8);
  v|=(oggpack_read(opb,8)<<8);
  v|=(oggpack_read(opb,8)<<16);
  v|=(oggpack_read(opb,8)<<24);
  return v;
}

static kate_int32_t kate_read32v(oggpack_buffer *opb)
{
  int smallv=oggpack_read(opb,4);
  if (smallv==15) {
    int sign=oggpack_read(opb,1);
    int bits=oggpack_read(opb,5)+1;
    kate_int32_t v=oggpack_read(opb,bits);
    if (sign) v=-v;
    return v;
  }
  else return smallv;
}

static kate_int64_t kate_read64(oggpack_buffer *opb)
{
  kate_int32_t vl=kate_read32(opb);
  kate_int32_t vh=kate_read32(opb);
  return (0xffffffff&(kate_int64_t)vl)|(((kate_int64_t)vh)<<32);
}

static void kate_warp(oggpack_buffer *opb)
{
  while (1) {
    kate_int32_t bits=kate_read32v(opb);
    if (!bits) break;
    oggpack_adv(opb,bits);
  }
}

static int kate_decode_check_magic(oggpack_buffer *opb)
{
  char magic[8];

  if (!opb) return KATE_E_INVALID_PARAMETER;

  kate_readbuf(opb,magic,8);
  if (memcmp(magic,"kate\0\0\0\0",8)) return KATE_E_NOT_KATE;

  return 0;
}

/**
  \ingroup decoding
  Checks if a packet is a Kate ID header
  \param op the packet to inspect
  \returns 0 the packet is not an ID header
  \returns !=0 the packet is an ID header
  */
int kate_decode_is_idheader(const kate_packet *op)
{
  oggpack_buffer opb;
  unsigned char headerid;

  if (!op) return 0;

  oggpack_readinit(&opb,op->data,op->nbytes);
  headerid=oggpack_read(&opb,8);
  if (headerid!=0x80) return 0;

  return !kate_decode_check_magic(&opb);
}

static int kate_check_eop(oggpack_buffer *opb)
{
  int bits;

  if (!opb) return KATE_E_INVALID_PARAMETER;

  /* ensure any remaining bits in the current byte are zero (reading 0 bytes yields zero) */
  bits=7&(8-(oggpack_bits(opb)&7));
  if (bits>0) {
    if (oggpack_read(opb,bits)) return KATE_E_BAD_PACKET;
  }
  if (oggpack_look1(opb)>=0) return KATE_E_BAD_PACKET; /* no more data expected */

  return 0;
}

static int kate_decode_info_header(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int len;
  int ret;
  char *language,*category;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  ki->bitstream_version_major=oggpack_read(opb,8);
  ki->bitstream_version_minor=oggpack_read(opb,8);
  if (ki->bitstream_version_major>KATE_BITSTREAM_VERSION_MAJOR) return KMG_ERROR(KATE_E_VERSION);

  ki->num_headers=oggpack_read(opb,8);
  ki->text_encoding=oggpack_read(opb,8);
  ki->text_directionality=oggpack_read(opb,8);
  if (oggpack_read(opb,8)!=0) return KMG_ERROR(KATE_E_BAD_PACKET); /* reserved - 0 */
  ki->granule_shift=oggpack_read(opb,8);

  if (kate_read32(opb)!=0) return KMG_ERROR(KATE_E_BAD_PACKET); /* reserved - 0 */
  if (kate_read32(opb)!=0) return KMG_ERROR(KATE_E_BAD_PACKET); /* reserved - 0 */
  ki->gps_numerator=kate_read32(opb);
  ki->gps_denominator=kate_read32(opb);

  if (ki->granule_shift>=64) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (ki->gps_numerator==0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (ki->gps_denominator==0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if ((ki->gps_numerator^ki->gps_denominator)<0) return KMG_ERROR(KATE_E_BAD_PACKET);

  len=16;
  language=(char*)KMG_MALLOC(len);
  if (!language) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
  kate_readbuf(opb,language,len); /* a terminating null is included */
  if (language[len-1]) return KMG_ERROR(KATE_E_BAD_PACKET); /* but do check it to be sure */

  len=16;
  category=(char*)KMG_MALLOC(len);
  if (!category) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
  kate_readbuf(opb,category,len); /* a terminating null is included */
  if (category[len-1]) return KMG_ERROR(KATE_E_BAD_PACKET); /* but do check it to be sure */

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->language=language;
  ki->category=category;

  return KMG_OK();
}

static int kate_decode_comment_packet(const kate_info *ki,kate_comment *kc,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,len,nc;
  char **user_comments,*vendor;
  int comments,*comment_lengths;

  if (!ki || !kc || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  len=kate_read32(opb);
  if (len<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && len>KATE_LIMIT_COMMENT_LENGTH) return KMG_ERROR(KATE_E_LIMIT);
  vendor=(char*)KMG_MALLOC(len+1);
  if (!vendor) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
  kate_readbuf(opb,vendor,len);
  vendor[len]=0;

  comments=kate_read32(opb);
  if (comments<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && len>KATE_LIMIT_COMMENTS) return KMG_ERROR(KATE_E_LIMIT);
  user_comments=(char**)KMG_MALLOC(comments*sizeof(char*));
  comment_lengths=(int*)KMG_MALLOC(comments*sizeof(int));
  if (!user_comments || !comment_lengths) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);

  for (nc=0;nc<comments;++nc) user_comments[nc]=NULL;
  for (nc=0;nc<comments;++nc) {
    len=kate_read32(opb);
    if (len<0) return KMG_ERROR(KATE_E_BAD_PACKET);
    if (!ki->no_limits && len>KATE_LIMIT_COMMENT_LENGTH) return KMG_ERROR(KATE_E_LIMIT);
    user_comments[nc]=(char*)KMG_MALLOC(len+1);
    if (!user_comments[nc]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    if (len) {
      kate_readbuf(opb,user_comments[nc],len);
    }
    user_comments[nc][len]=0;
    comment_lengths[nc]=len;
  }

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  kc->user_comments=user_comments;
  kc->comment_lengths=comment_lengths;
  kc->comments=comments;
  kc->vendor=vendor;

  return KMG_OK();
}

static int kate_decode_region(kate_region *kr,oggpack_buffer *opb)
{
  if (!kr || !opb) return KATE_E_INVALID_PARAMETER;
  kr->metric=oggpack_read(opb,8);
  kr->x=kate_read32v(opb);
  kr->y=kate_read32v(opb);
  kr->w=kate_read32v(opb);
  kr->h=kate_read32v(opb);
  kr->style=kate_read32v(opb);
  kate_warp(opb);
  return 0;
}

static int kate_decode_regions_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n,nregions;
  kate_region **regions;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  nregions=kate_read32v(opb);
  if (nregions<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && nregions>KATE_LIMIT_REGIONS) return KMG_ERROR(KATE_E_LIMIT);
  regions=(kate_region**)KMG_MALLOC(nregions*sizeof(kate_region*));
  if (!regions) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
  for (n=0;n<nregions;++n) {
    regions[n]=(kate_region*)KMG_MALLOC(sizeof(kate_region));
    if (!regions[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    ret=kate_decode_region(regions[n],opb);
    if (ret<0) return KMG_ERROR(ret);
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->nregions=nregions;
  ki->regions=regions;

  return KMG_OK();
}

static int kate_decode_color(kate_color *kc,oggpack_buffer *opb)
{
  if (!kc || !opb) return KATE_E_INVALID_PARAMETER;
  kc->r=oggpack_read(opb,8);
  kc->g=oggpack_read(opb,8);
  kc->b=oggpack_read(opb,8);
  kc->a=oggpack_read(opb,8);
  return 0;
}

static int kate_decode_style(kate_style *ks,oggpack_buffer *opb)
{
  int ret;
  kate_float d[8];
  size_t idx;

  if (!ks || !opb) return KATE_E_INVALID_PARAMETER;

  ret=kate_fp_decode_kate_float(sizeof(d)/sizeof(d[0]),d,1,opb);
  if (ret<0) return ret;

  idx=0;
  ks->halign=d[idx++];
  ks->valign=d[idx++];
  ks->font_width=d[idx++];
  ks->font_height=d[idx++];
  ks->left_margin=d[idx++];
  ks->top_margin=d[idx++];
  ks->right_margin=d[idx++];
  ks->bottom_margin=d[idx++];
  ret=kate_decode_color(&ks->text_color,opb);
  if (ret<0) return ret;
  ret=kate_decode_color(&ks->background_color,opb);
  if (ret<0) return ret;
  ret=kate_decode_color(&ks->draw_color,opb);
  if (ret<0) return ret;
  ks->font_metric=oggpack_read(opb,8);
  ks->margin_metric=oggpack_read(opb,8);
  ks->bold=oggpack_read(opb,1);
  ks->italics=oggpack_read(opb,1);
  ks->underline=oggpack_read(opb,1);
  ks->strike=oggpack_read(opb,1);
  kate_warp(opb);

  return 0;
}

static int kate_decode_styles_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n,nstyles;
  kate_style **styles;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  nstyles=kate_read32v(opb);
  if (nstyles<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && nstyles>KATE_LIMIT_STYLES) return KMG_ERROR(KATE_E_LIMIT);
  styles=(kate_style**)KMG_MALLOC(nstyles*sizeof(kate_style*));
  if (!styles) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
  for (n=0;n<nstyles;++n) {
    styles[n]=(kate_style*)KMG_MALLOC(sizeof(kate_style));
    if (!styles[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    ret=kate_decode_style(styles[n],opb);
    if (ret<0) return KMG_ERROR(ret);
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->nstyles=nstyles;
  ki->styles=styles;

  return KMG_OK();
}

static int kate_decode_curve(const kate_info *ki,kate_curve *kc,oggpack_buffer *opb)
{
  int ret;

  if (!ki || !kc || !opb) return KATE_E_INVALID_PARAMETER;

  kc->type=oggpack_read(opb,8);
  kc->npts=kate_read32v(opb);
  kate_warp(opb);
  if (!ki->no_limits && kc->npts>KATE_LIMIT_CURVE_POINTS) return KATE_E_LIMIT;
  kc->pts=(kate_float*)kate_malloc(kc->npts*2*sizeof(kate_float));
  if (!kc->pts) return KATE_E_OUT_OF_MEMORY;

  ret=kate_fp_decode_kate_float(kc->npts,kc->pts,2,opb);
  if (ret<0) {
    kate_free(kc->pts);
    kc->pts=NULL;
    return ret;
  }

  return 0;
}

static int kate_decode_curves_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n,ncurves;
  kate_curve **curves=NULL;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  ncurves=kate_read32v(opb);
  if (ncurves<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && ncurves>KATE_LIMIT_CURVES) return KMG_ERROR(KATE_E_LIMIT);

  if (ncurves>0) {
    curves=(kate_curve**)KMG_MALLOC(ncurves*sizeof(kate_curve*));
    if (!curves) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    for (n=0;n<ncurves;++n) {
      curves[n]=(kate_curve*)KMG_MALLOC(sizeof(kate_curve));
      if (!curves[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
      ret=kate_decode_curve(ki,curves[n],opb);
      if (ret<0) return KMG_ERROR(ret);
    }
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->ncurves=ncurves;
  ki->curves=curves;

  return KMG_OK();
}

static int kate_decode_motion(const kate_info *ki,kate_motion *km,oggpack_buffer *opb)
{
  KMG_GUARD();
  size_t n;
  int ret;

  if (!ki || !km || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  km->ncurves=kate_read32v(opb);
  if (!ki->no_limits && km->ncurves>KATE_LIMIT_MOTION_CURVES) return KMG_ERROR(KATE_E_LIMIT);
  km->curves=(kate_curve**)KMG_MALLOC(km->ncurves*sizeof(kate_curve*));
  if (!km->curves) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
  km->durations=(kate_float*)KMG_MALLOC(km->ncurves*sizeof(kate_float));
  if (!km->durations) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);

  for (n=0;n<km->ncurves;++n) {
    if (oggpack_read(opb,1)) {
      size_t idx=kate_read32v(opb);
      if (idx>=ki->ncurves) return KMG_ERROR(KATE_E_BAD_PACKET);
      km->curves[n]=ki->curves[idx];
    }
    else {
      km->curves[n]=(kate_curve*)KMG_MALLOC(sizeof(kate_curve));
      if (!km->curves[n]) return KATE_E_OUT_OF_MEMORY;
      ret=kate_decode_curve(ki,km->curves[n],opb);
      if (ret<0) return KMG_ERROR(ret);
    }
  }
  ret=kate_fp_decode_kate_float(km->ncurves,km->durations,1,opb);
  if (ret<0) return KMG_ERROR(ret);
  km->x_mapping=oggpack_read(opb,8);
  km->y_mapping=oggpack_read(opb,8);
  km->semantics=oggpack_read(opb,8);
  km->periodic=oggpack_read(opb,1);
  kate_warp(opb);

  return KMG_OK();
}

static int kate_decode_motions_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n,nmotions;
  kate_motion **motions=NULL;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  nmotions=kate_read32v(opb);
  if (nmotions<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && nmotions>KATE_LIMIT_MOTIONS) return KMG_ERROR(KATE_E_LIMIT);

  if (nmotions>0) {
    motions=(kate_motion**)KMG_MALLOC(nmotions*sizeof(kate_motion*));
    if (!motions) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    for (n=0;n<nmotions;++n) {
      motions[n]=(kate_motion*)KMG_MALLOC(sizeof(kate_motion));
      if (!motions[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
      ret=kate_decode_motion(ki,motions[n],opb);
      if (ret<0) return KMG_ERROR(ret);
    }
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->nmotions=nmotions;
  ki->motions=motions;

  return KMG_OK();
}

static int kate_decode_palette(const kate_info *ki,kate_palette *kp,oggpack_buffer *opb)
{
  kate_color *colors;
  size_t n;

  if (!ki || !kp || !opb) return KATE_E_INVALID_PARAMETER;

  kp->ncolors=oggpack_read(opb,8)+1;

  colors=(kate_color*)kate_malloc(kp->ncolors*sizeof(kate_color));
  if (!colors) return KATE_E_OUT_OF_MEMORY;

  for (n=0;n<kp->ncolors;++n) {
    int ret=kate_decode_color(colors+n,opb);
    if (ret<0) {
      kate_free(colors);
      return ret;
    }
  }
  kate_warp(opb);

  kp->colors=colors;

  return 0;
}

static int kate_decode_palettes_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n,npalettes;
  kate_palette **palettes=NULL;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  npalettes=kate_read32v(opb);
  if (npalettes<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && npalettes>KATE_LIMIT_PALETTES) return KMG_ERROR(KATE_E_LIMIT);

  if (npalettes>0) {
    palettes=(kate_palette**)KMG_MALLOC(npalettes*sizeof(kate_palette*));
    if (!palettes) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    for (n=0;n<npalettes;++n) {
      palettes[n]=(kate_palette*)KMG_MALLOC(sizeof(kate_palette));
      if (!palettes[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
      ret=kate_decode_palette(ki,palettes[n],opb);
      if (ret<0) return KMG_ERROR(ret);
    }
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->npalettes=npalettes;
  ki->palettes=palettes;

  return KMG_OK();
}

static int kate_decode_bitmap(const kate_info *ki,kate_bitmap *kb,oggpack_buffer *opb)
{
  size_t n,npixels;
  unsigned char *pixels;

  if (!ki || !kb || !opb) return KATE_E_INVALID_PARAMETER;

  kb->width=kate_read32v(opb);
  kb->height=kate_read32v(opb);
  kb->bpp=oggpack_read(opb,8);
  kb->palette=kate_read32v(opb);
  if (kb->width<=0 || kb->height<=0 || kb->bpp<1 || kb->bpp>8) return KATE_E_BAD_PACKET;
  if (!ki->no_limits && (kb->width>KATE_LIMIT_BITMAP_SIZE || kb->height>KATE_LIMIT_BITMAP_SIZE)) return KATE_E_LIMIT;

  npixels=kb->width*kb->height;
  pixels=(unsigned char*)kate_malloc(npixels);
  if (!pixels) return KATE_E_OUT_OF_MEMORY;

  for (n=0;n<npixels;++n) {
    pixels[n]=oggpack_read(opb,kb->bpp);
  }
  kate_warp(opb);

  kb->pixels=pixels;

  return 0;
}

static int kate_decode_bitmaps_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n,nbitmaps;
  kate_bitmap **bitmaps=NULL;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  nbitmaps=kate_read32v(opb);
  if (nbitmaps<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && nbitmaps>KATE_LIMIT_BITMAPS) return KMG_ERROR(KATE_E_LIMIT);

  if (nbitmaps>0) {
    bitmaps=(kate_bitmap**)KMG_MALLOC(nbitmaps*sizeof(kate_bitmap*));
    if (!bitmaps) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    for (n=0;n<nbitmaps;++n) {
      bitmaps[n]=(kate_bitmap*)KMG_MALLOC(sizeof(kate_bitmap));
      if (!bitmaps[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
      ret=kate_decode_bitmap(ki,bitmaps[n],opb);
      if (ret<0) return KMG_ERROR(ret);
    }
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->nbitmaps=nbitmaps;
  ki->bitmaps=bitmaps;

  return KMG_OK();
}

static int kate_decode_font_range(const kate_info *ki,kate_font_range *kfr,oggpack_buffer *opb)
{
  if (!ki || !kfr || !opb) return KATE_E_INVALID_PARAMETER;

  kfr->first_code_point=kate_read32v(opb);
  kfr->last_code_point=kate_read32v(opb);
  kfr->first_bitmap=kate_read32v(opb);
  kate_warp(opb);

  return 0;
}

static int kate_decode_font_ranges_packet(kate_info *ki,oggpack_buffer *opb)
{
  KMG_GUARD();
  int l,n,ret;
  int nfont_ranges,nfont_mappings;
  kate_font_range **font_ranges=NULL;
  kate_font_mapping **font_mappings=NULL;

  if (!ki || !opb) return KMG_ERROR(KATE_E_INVALID_PARAMETER);

  nfont_ranges=kate_read32v(opb);
  if (nfont_ranges<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && nfont_ranges>KATE_LIMIT_FONT_RANGES) return KMG_ERROR(KATE_E_LIMIT);

  if (nfont_ranges>0) {
    font_ranges=(kate_font_range**)KMG_MALLOC(nfont_ranges*sizeof(kate_font_range*));
    if (!font_ranges) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    for (n=0;n<nfont_ranges;++n) {
      font_ranges[n]=(kate_font_range*)KMG_MALLOC(sizeof(kate_font_range));
      if (!font_ranges[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
      ret=kate_decode_font_range(ki,font_ranges[n],opb);
      if (ret<0) return KMG_ERROR(ret);
    }
  }

  /* these may now be used by the mappings */
  ki->nfont_ranges=nfont_ranges;
  ki->font_ranges=font_ranges;

  /* now, the mappings */
  nfont_mappings=kate_read32v(opb);
  if (nfont_mappings<0) return KMG_ERROR(KATE_E_BAD_PACKET);
  if (!ki->no_limits && nfont_mappings>KATE_LIMIT_FONT_MAPPINGS) return KMG_ERROR(KATE_E_LIMIT);

  if (nfont_mappings>0) {
    font_mappings=(kate_font_mapping**)KMG_MALLOC(nfont_mappings*sizeof(kate_font_mapping*));
    if (!font_mappings) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
    for (n=0;n<nfont_mappings;++n) {
      font_mappings[n]=(kate_font_mapping*)KMG_MALLOC(sizeof(kate_font_mapping));
      if (!font_mappings[n]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);

      nfont_ranges=kate_read32v(opb);
      if (nfont_ranges<0) return KMG_ERROR(KATE_E_BAD_PACKET);
      if (!ki->no_limits && nfont_ranges>KATE_LIMIT_FONT_MAPPING_RANGES) return KMG_ERROR(KATE_E_LIMIT);

      if (nfont_ranges>0) {
        font_ranges=(kate_font_range**)KMG_MALLOC(nfont_ranges*sizeof(kate_font_range*));
        if (!font_ranges) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);

        for (l=0;l<nfont_ranges;++l) {
          if (oggpack_read(opb,1)) {
            size_t idx=kate_read32v(opb);
            if (idx>=ki->nfont_ranges) return KMG_ERROR(KATE_E_BAD_PACKET);
            font_ranges[l]=ki->font_ranges[idx];
          }
          else {
            font_ranges[l]=(kate_font_range*)KMG_MALLOC(sizeof(kate_font_range));
            if (!font_ranges[l]) return KMG_ERROR(KATE_E_OUT_OF_MEMORY);
            ret=kate_decode_font_range(ki,font_ranges[l],opb);
            if (ret<0) return KMG_ERROR(ret);
          }
        }
        font_mappings[n]->nranges=nfont_ranges;
        font_mappings[n]->ranges=font_ranges;
      }
      else {
        font_mappings[n]->nranges=0;
        font_mappings[n]->ranges=NULL;
      }
    }
  }

  kate_warp(opb);

  ret=kate_check_eop(opb);
  if (ret<0) return KMG_ERROR(ret);

  ki->nfont_mappings=nfont_mappings;
  ki->font_mappings=font_mappings;

  return KMG_OK();
}

/**
  \ingroup decoding
  Decodes a header packet, and updates the kate_info and kate_comment structures
  from the decoded data.
  \param ki the kate_info structure to update
  \param kc the kate_comment structure to update
  \param op the packet to decode
  \returns 0 success
  \returns KATE_E_* error
  */
int kate_decode_headerin(kate_info *ki,kate_comment *kc,kate_packet *op)
{
  oggpack_buffer opb;
  unsigned char headerid;
  int ret;
  int packetno;

  if (!ki || !kc || !op) return KATE_E_INVALID_PARAMETER;

  oggpack_readinit(&opb,op->data,op->nbytes);
  headerid=oggpack_read(&opb,8);

  ret=kate_decode_check_magic(&opb);
  if (ret<0) return ret;

  if (!(headerid&0x80)) return KATE_E_BAD_PACKET;
  packetno=headerid&~0x80;
  if (packetno<ki->num_headers) {
    if (ki->probe!=packetno) return KATE_E_BAD_PACKET;
  }

  switch (packetno) {
    case 0: /* this is the info packet */
      ret=kate_decode_info_header(ki,&opb);
      break;

    case 1: /* this is the comments packet */
      ret=kate_decode_comment_packet(ki,kc,&opb);
      break;

    case 2: /* this is the region list packet */
      ret=kate_decode_regions_packet(ki,&opb);
      break;

    case 3: /* this is the style list packet */
      ret=kate_decode_styles_packet(ki,&opb);
      break;

    case 4: /* this is the curve list packet */
      ret=kate_decode_curves_packet(ki,&opb);
      break;

    case 5: /* this is the motion list packet */
      ret=kate_decode_motions_packet(ki,&opb);
      break;

    case 6: /* this is the palette list packet */
      ret=kate_decode_palettes_packet(ki,&opb);
      break;

    case 7: /* this is the bitmap list packet */
      ret=kate_decode_bitmaps_packet(ki,&opb);
      break;

    case 8: /* this is the font ranges list packet */
      ret=kate_decode_font_ranges_packet(ki,&opb);
      if (ret==0) ret=1; /* we're done, we know of no more headers to come */
      break;

    default:
      /* we ignore extra header packets for future proofing */
      ret=0;
      break;
  }

  if (ret>=0) {
    /* if the header was parsed successfully, we're ready for the next one */
    ki->probe++;
  }

  return ret;
}

/**
  \ingroup decoding
  Initializes a kate_state structure for decoding using the supplied kate_info structure.
  When done, the kate_state should be cleared using kate_clear.
  \param k the kate_state to initialize for decoding
  \param ki the kate_info structure initialized from the decoded headers
  \returns 0 success
  \returns KATE_E_* error
  */
int kate_decode_init(kate_state *k,kate_info *ki)
{
  if (!k || !ki) return KATE_E_INVALID_PARAMETER;

  k->ki=ki;
  k->kes=NULL;
  k->kds=kate_decode_state_create();
  if (!k->kds) return KATE_E_OUT_OF_MEMORY;

  return 0;
}

#define READ_OVERRIDE(read) \
  do { \
    if (oggpack_read(opb,1)) { read; } \
  } while(0)

static int kate_decode_text_packet(kate_state *k,oggpack_buffer *opb)
{
  KMG_GUARD();
  int ret,n;
  int len;
  char *text;
  kate_decode_state *kds;
  kate_event *ev;

  if (!k || !opb) return KATE_E_INVALID_PARAMETER;
  if (!k->kds) return KATE_E_INIT;

  ret=kate_decode_state_clear(k->kds,k->ki,1);
  if (ret<0) return ret;

  kds=k->kds;
  ev=kds->event;

  ev->start=kate_read64(opb);
  ev->duration=kate_read64(opb);
  ev->backlink=kate_read64(opb);
  if (ev->start<0 || ev->duration<0) goto error_bad_packet;
  if (ev->backlink<0 || ev->backlink>ev->start) goto error_bad_packet;

  ev->start_time=kate_granule_duration(k->ki,ev->start);
  ev->end_time=ev->start_time+kate_granule_duration(k->ki,ev->duration);

  len=kate_read32(opb);
  if (len<0) goto error_bad_packet;
  if (!k->ki->no_limits && len>KATE_LIMIT_TEXT_LENGTH) goto error_limit;

  /* read the text, and null terminate it - 4 characters for UTF-32 */
  text=(char*)KMG_MALLOC(len+4);
  if (!text) goto error_out_of_memory;
  kate_readbuf(opb,text,len);
  text[len]=0;
  text[len+1]=0;
  text[len+2]=0;
  text[len+3]=0;

  ev->text=text;
  ev->len=len;
  ev->len0=len+4;

  if (k->ki->remove_markup) {
    ret=kate_text_remove_markup(ev->text_encoding,ev->text,&ev->len);
    if (ret<0) goto error;
  }

  if (oggpack_read(opb,1)) {
    ev->id=kate_read32v(opb);
  }

  if (oggpack_read(opb,1)) {
    kate_motion **motions=NULL;
    size_t nmotions=0;

    len=kate_read32v(opb);
    if (len<=0) goto error_bad_packet; /* if the flag was set, there's at least one */
    if (!k->ki->no_limits && len>KATE_LIMIT_TEXT_MOTIONS) goto error_limit;
    motions=(kate_motion**)KMG_MALLOC(len*sizeof(kate_motion*));
    if (!motions) goto error_out_of_memory;
    nmotions=0;
    for (n=0;n<len;++n) {
      if (oggpack_read(opb,1)) {
        size_t idx=kate_read32v(opb);
        if (idx>=k->ki->nmotions) goto error_bad_packet;
        motions[n]=k->ki->motions[idx];
      }
      else {
        motions[n]=KMG_MALLOC(sizeof(kate_motion));
        if (!motions[n]) goto error_out_of_memory;
        ret=kate_decode_motion(k->ki,motions[n],opb);
        if (ret<0) goto error;
      }
      ++nmotions;
    }

    ev->motions=motions;
    ev->nmotions=nmotions;
  }

  if (oggpack_read(opb,1)) {
    READ_OVERRIDE(ev->text_encoding=oggpack_read(opb,8));
    READ_OVERRIDE(ev->text_directionality=oggpack_read(opb,8));
    READ_OVERRIDE(
      do {
        len=kate_read32v(opb);
        if (len<0) goto error_bad_packet;
        if (!k->ki->no_limits && len>KATE_LIMIT_LANGUAGE_LENGTH) goto error_limit;
        if (len>0) {
          ev->language=(char*)KMG_MALLOC(len+1);
          if (!ev->language) goto error_out_of_memory;
          kate_readbuf(opb,ev->language,len);
          ev->language[len]=0;
        }
      } while(0)
    );
    READ_OVERRIDE(
      size_t idx=kate_read32v(opb);
      if (idx>=k->ki->nregions) goto error_bad_packet;
      ev->region=k->ki->regions[idx];
    );
    READ_OVERRIDE(
      ev->region=KMG_MALLOC(sizeof(kate_region));
      if (!ev->region) goto error_out_of_memory;
      ret=kate_decode_region(ev->region,opb);
      if (ret<0) goto error;
    );
    READ_OVERRIDE(
      size_t idx=kate_read32v(opb);
      if (idx>=k->ki->nstyles) goto error_bad_packet;
      ev->style=k->ki->styles[idx]
    );
    READ_OVERRIDE(
      ev->style=kate_malloc(sizeof(kate_style));
      if (!ev->style) goto error_out_of_memory;
      ret=kate_decode_style(ev->style,opb);
      if (ret<0) goto error;
    );
    READ_OVERRIDE(
      size_t idx=kate_read32v(opb);
      if (idx>=k->ki->nstyles) goto error_bad_packet;
      ev->secondary_style=k->ki->styles[idx]
    );
    READ_OVERRIDE(
      ev->secondary_style=kate_malloc(sizeof(kate_style));
      if (!ev->secondary_style) goto error_out_of_memory;
      ret=kate_decode_style(ev->secondary_style,opb);
      if (ret<0) goto error;
    );
    READ_OVERRIDE(
      size_t idx=kate_read32v(opb);
      if (idx>=k->ki->nfont_mappings) goto error_bad_packet;
      ev->font_mapping=k->ki->font_mappings[idx]
    );
  }

  if (((k->ki->bitstream_version_major<<8)|k->ki->bitstream_version_minor)>=0x0002) {
    /* 0.2 adds a warp for palette and bitmap */
    kate_read32v(opb); /* the size of the warp */
    if (oggpack_read(opb,1)) {
      READ_OVERRIDE(
        size_t idx=kate_read32v(opb);
        if (idx>=k->ki->npalettes) goto error_bad_packet;
        ev->palette=k->ki->palettes[idx];
      );
      READ_OVERRIDE(
        ev->palette=KMG_MALLOC(sizeof(kate_palette));
        if (!ev->palette) goto error_out_of_memory;
        ret=kate_decode_palette(k->ki,ev->palette,opb);
        if (ret<0) goto error;
      );
      READ_OVERRIDE(
        size_t idx=kate_read32v(opb);
        if (idx>=k->ki->nbitmaps) goto error_bad_packet;
        ev->bitmap=k->ki->bitmaps[idx];
      );
      READ_OVERRIDE(
        ev->bitmap=KMG_MALLOC(sizeof(kate_bitmap));
        if (!ev->bitmap) goto error_out_of_memory;
        ret=kate_decode_bitmap(k->ki,ev->bitmap,opb);
        if (ret<0) goto error;
      );
    }
  }

  kate_warp(opb);

  /* if no style, use the region (if any) default style */
  if (!ev->style && ev->region) {
    if (ev->region->style>=0) {
      size_t idx=ev->region->style;
      if (idx<k->ki->nstyles) {
        ev->style=k->ki->styles[idx];
      }
    }
  }

  return KMG_OK();

error_limit:
  ret=KATE_E_LIMIT;
  goto error;

error_bad_packet:
  ret=KATE_E_BAD_PACKET;
  goto error;

error_out_of_memory:
  ret=KATE_E_OUT_OF_MEMORY;
  goto error;

error:
  /* TODO: should not be necessary now that kate_memory_guard is there
     we may leak a bit, but calling this will double free stuff */
  /* kate_event_release(ev); */
  kds->event=NULL;
  return KMG_ERROR(ret);
}

static int kate_decode_keepalive_packet(kate_state *k,oggpack_buffer *opb)
{
  if (!k || !opb) return KATE_E_INVALID_PARAMETER;
  if (!k->kds) return KATE_E_INIT;

  return 0;
}

static int kate_decode_end_packet(kate_state *k,oggpack_buffer *opb)
{
  if (!k || !opb) return KATE_E_INVALID_PARAMETER;
  if (!k->kds) return KATE_E_INIT;

  return 1;
}

/**
  \ingroup decoding
  Decodes a data packet.
  \param k the kate_state to decode to
  \param op the packet to decode
  \returns 0 success
  \returns 1 success, and we're at end of stream
  \returns KATE_E_* error
  */
int kate_decode_packetin(kate_state *k,kate_packet *op)
{
  oggpack_buffer opb;
  int ret,id;

  if (!k || !op) return KATE_E_INVALID_PARAMETER;
  if (!k->ki) return KATE_E_INIT;
  if (!k->kds) return KATE_E_INIT;

  ret=kate_decode_state_clear(k->kds,k->ki,0);
  if (ret<0) return ret;

  oggpack_readinit(&opb,op->data,op->nbytes);
  id=oggpack_read(&opb,8);
  if (id&0x80) {
    /* we have a header - we'll ignore it
       it may have happened either because we seeked to the beginning of the stream
       or because we're parsing a stream that has more headers than we know about */
    return 0;
  }

  switch (id) {
    case 0x00: return kate_decode_text_packet(k,&opb);
    case 0x01: return kate_decode_keepalive_packet(k,&opb);
    case 0x7f: return kate_decode_end_packet(k,&opb);
    default: return 0; /* unknown data packets are ignored, for future proofing */
  }

  return 0;
}

/**
  \ingroup decoding
  Requests a pointer to the event decoded by the last packet, if there is one.
  \param k the kate_state to get the event from
  \param event a pointer to where to place the pointer to the event, if any
  \returns 0 success, an event was returned
  \returns 1 success, but there was no event to return
  \returns KATE_E_* error
  */
int kate_decode_eventout(kate_state *k,kate_const kate_event **event)
{
  if (!k) return KATE_E_INVALID_PARAMETER;
  if (!k->kds) return KATE_E_INIT;

  if (!k->kds->event) return 1;

  if (event) *event=k->kds->event;

  return 0;
}

