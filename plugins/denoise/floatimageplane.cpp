/*
 * * Copyright (C) 2006-2010 Anders Brander <anders@brander.dk>, 
 * * Anders Kvist <akv@lnxbx.dk> and Klaus Post <klauspost@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "floatimageplane.h"
#include "fftw3.h"
#include <string.h>

namespace RawStudio {
namespace FFTFilter {

FloatImagePlane::FloatImagePlane( int _w, int _h, int _plane_id ) :
w(_w), h(_h), data(0), plane_id(_plane_id), filter(0), window(0), pitch(0), allocated(0)
{
}

FloatImagePlane::FloatImagePlane( const FloatImagePlane& p ) :
w(p.w), h(p.h),data(p.data), plane_id(p.plane_id), filter(0), window(p.window),
pitch(p.pitch), allocated(0)
{
}

FloatImagePlane::~FloatImagePlane(void)
{
  if (allocated)
    fftwf_free(allocated);
  if (filter)
    delete filter;
  filter = 0;
  allocated = 0;
}

void FloatImagePlane::allocateImage()
{
  if (allocated)
    return;
  pitch = ((w+3)/4)*4;
  allocated = (gfloat *) fftwf_malloc(pitch*h*sizeof(gfloat)); 
  g_assert(allocated);
  data = allocated;
}

gfloat* FloatImagePlane::getLine( int y ) {
  return &data[pitch*y];
}

gfloat* FloatImagePlane::getAt( int x, int y ) {
  return &data[pitch*y+x];
}

void FloatImagePlane::mirrorEdges( int mirror_x, int mirror_y ) {
  // Mirror top
  for (int y = 0; y<mirror_y; y++){
    memcpy(getLine(mirror_y-y-1), getLine(mirror_y+y), w*sizeof(gfloat));
  }
  // Mirror bottom
  for (int y = 0; y<mirror_y; y++){
    memcpy(getLine(h-mirror_y+y), getLine(h-mirror_y-y-1), w*sizeof(gfloat));
  }
  // Mirror left and right
  for (int y = 0; y<h; y++){
    gfloat *l = getAt(mirror_x,y);
    gfloat *r = getAt(w-mirror_x-1,y);
    for(int x = 1; x<=mirror_x; x++) {
      l[-x] = l[x+1];
      r[x] = r[-x-1];
    }
  }
}

void FloatImagePlane::addJobs(JobQueue *jobs, int bw, int bh, int ox, int oy, FloatImagePlane *outPlane) {
  int start_y = 0;
  gboolean endy = false;

  while (!endy) {
    int start_x = 0;
    gboolean endx = false;
    while (!endx) {
      PlanarImageSlice *s = new PlanarImageSlice();
      s->in = getSlice(start_x, start_y, bw, bh);
      s->offset_x = start_x;
      s->offset_y = start_y;
      s->overlap_x = ox;
      s->overlap_y = oy;
      s->filter = filter;
      s->window = window;
      FFTJob *j = new FFTJob(s);
      j->outPlane = outPlane;
      jobs->addJob(j);
      if (start_x + bw*2 - ox*2 >= w) {  //Will next block be out of frame?
        if (start_x == w - bw)
          endx = true;
         start_x = w - bw; // Add last possible block
      } else {
        start_x += bw - ox*2;
      }
    } // end while x
    if (start_y + bh*2 - oy*2 >= h) { //Will next block be out of frame?
      if (start_y == h - bh)
        endy = true;
      start_y = h - bh;   // Add last possible block
    } else {
      start_y += bh - oy*2;
    }
  }//end while y
}

FloatImagePlane* FloatImagePlane::getSlice( int x, int y,int new_w, int new_h )
{
  g_assert(x+new_w<=w);
  g_assert(y+new_h<=h);
  g_assert(x>=0);
  g_assert(x>=0);
  FloatImagePlane* s = new FloatImagePlane(new_w, new_h, plane_id);
  s->data = getAt(x,y);
  s->pitch = pitch;
  return s;
}

//TODO: SSE2 me.
void FBitBlt(guchar* dstp, int dst_pitch, const guchar* srcp, int src_pitch, int row_size, int height) {
  if (height == 1 || (dst_pitch == src_pitch && src_pitch == row_size)) {
    memcpy(dstp, srcp, row_size*height);
    return;
  }
  for (int y=height; y>0; --y) {
    memcpy(dstp, srcp, row_size);
    dstp += dst_pitch;
    srcp += src_pitch;
  }
}

void FloatImagePlane::applySlice( PlanarImageSlice *p ) {
  int start_y = p->offset_y + p->overlap_y; 
  int start_x = p->offset_x + p->overlap_x; 
  g_assert(start_y >= 0);
  g_assert(start_x >= 0);
  g_assert(start_y < h);
  g_assert(start_x < w);

  if (p->blockSkipped) {
    FBitBlt((guchar*)getAt(start_x,start_y), pitch*sizeof(float),
            (const guchar*)p->in->getAt(p->overlap_x,p->overlap_y), p->in->pitch*sizeof(float), 
              p->in->w*sizeof(float)-p->overlap_x*2*sizeof(float), p->in->h-p->overlap_y*2);
    return;
  }

  float normalization = 1.0f / (float)(p->out->w * p->out->h);
  
  int end_x = p->offset_x + p->out->w - p->overlap_x;
  int end_y = p->offset_y + p->out->h - p->overlap_y;

  g_assert(end_y >= 0);
  g_assert(end_x >= 0);
  g_assert(end_y < h);
  g_assert(end_x < w);

  for (int y = start_y; y < end_y; y++ ) {
    float* src = p->out->getAt(p->overlap_x,y-start_y+p->overlap_y);
    float* dst = getAt(start_x,y);
    for (int x = start_x; x < end_x; x++) {
      *dst++ = normalization * (*src++);
    }
  }
}


void FloatImagePlane::blitOnto( FloatImagePlane *dst ) 
{
  g_assert(dst->w == w);
  g_assert(dst->h == h);
  FBitBlt((guchar*)dst->data, dst->pitch*sizeof(float),(guchar*)data,pitch*sizeof(float),w*sizeof(float),h);
}

void FloatImagePlane::multiply(float factor) 
{
  for (int y = 0; y < h; y++ ) {
    float* src = getAt(0,y);
    for (int x = 0; x < w; x++) {
      src[x] *= factor;
    }
  }
}

}}// namespace RawStudio::FFTFilter
