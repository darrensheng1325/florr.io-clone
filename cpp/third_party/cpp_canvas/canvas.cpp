#include "canvas.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <utility>
#ifndef __EMSCRIPTEN__
#include "font.h"
#include <SDL.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
static const Font* uiFont(unsigned char family);
#endif

namespace {
Path2D::Segment segment(Path2D::Command command, std::initializer_list<float> values, bool ccw = false) {
    Path2D::Segment result{}; result.command = command; result.counterClockwise = ccw;
    std::copy(values.begin(), values.end(), result.v); return result;
}
}

// Lazily allocated so that the paths a frame builds and throws away never
// consume one. Never reused: a key names one geometry for the life of the
// process, which is what lets the page keep a cache entry that outlives the
// path that filled it.
std::uint32_t Path2D::cacheKey() const {
    static std::uint32_t next = 1;
    if (key_ == 0) key_ = next++;
    return key_;
}

void Path2D::moveTo(float x, float y) { touch(); segments_.push_back(segment(Command::Move, {x, y})); }
void Path2D::lineTo(float x, float y) { touch(); segments_.push_back(segment(Command::Line, {x, y})); }
void Path2D::closePath() { touch(); segments_.push_back(segment(Command::Close, {})); }
void Path2D::quadraticCurveTo(float a,float b,float c,float d) { touch(); segments_.push_back(segment(Command::Quadratic,{a,b,c,d})); }
void Path2D::bezierCurveTo(float a,float b,float c,float d,float e,float f) { touch(); segments_.push_back(segment(Command::Bezier,{a,b,c,d,e,f})); }
void Path2D::arc(float a,float b,float c,float d,float e,bool f) { touch(); segments_.push_back(segment(Command::Arc,{a,b,c,d,e},f)); }
void Path2D::arcTo(float a,float b,float c,float d,float e) { touch(); segments_.push_back(segment(Command::ArcTo,{a,b,c,d,e})); }
void Path2D::ellipse(float a,float b,float c,float d,float e,float f,float g,bool h) { touch(); segments_.push_back(segment(Command::Ellipse,{a,b,c,d,e,f,g},h)); }
void Path2D::rect(float a,float b,float c,float d) { touch(); segments_.push_back(segment(Command::Rect,{a,b,c,d})); }
void Path2D::roundRect(float a,float b,float c,float d,float e) { touch(); segments_.push_back(segment(Command::RoundRect,{a,b,c,d,e})); }
void Path2D::addPath(const Path2D& other) {
    touch();
    segments_.insert(segments_.end(), other.segments_.begin(), other.segments_.end());
    // A path that has absorbed glyphs is a glyph path: losing the mark here
    // would silently drop the text gamma on any composed run.
    glyphOutlines_ = glyphOutlines_ || other.glyphOutlines_;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// One registry makes each C++ Canvas an independent CanvasRenderingContext2D.
EM_JS(int, c2d_create, (const char* element, int width, int height, int virtualCanvas), {
  Module.cppCanvasContexts ||= [];
  let surface;
  if (virtualCanvas) surface = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(width, height) : document.createElement('canvas');
  else { const id = UTF8ToString(element); surface = document.getElementById(id) || Object.assign(document.createElement('canvas'), {id}); if (!surface.parentNode) document.body.appendChild(surface); }
  surface.width = width; surface.height = height;
  Module.cppCanvasContexts.push({surface, ctx: surface.getContext('2d')});
  return Module.cppCanvasContexts.length - 1;
});
EM_JS(void, c2d_destroy, (int id), { if (Module.cppCanvasContexts) Module.cppCanvasContexts[id] = null; });
EM_JS(void, c2d_present, (int id, const char* target), {
  const item = Module.cppCanvasContexts[id], name = UTF8ToString(target);
  let canvas = document.getElementById(name);
  if (!canvas) { canvas = document.createElement('canvas'); canvas.id = name; document.body.appendChild(canvas); }
  canvas.width = item.surface.width; canvas.height = item.surface.height; canvas.getContext('2d').drawImage(item.surface, 0, 0);
});
EM_JS(void, c2d_op, (int id, int op, double a,double b,double c,double d,double e,double f,double g,double h, const char* text), {
  const x = Module.cppCanvasContexts[id].ctx, s = text ? UTF8ToString(text) : "";
  switch(op) {
    case 0:x.save();break; case 1:x.restore();break; case 2: x.reset ? x.reset() : (x.setTransform(1,0,0,1,0,0),x.clearRect(0,0,x.canvas.width,x.canvas.height));break;
    case 3:x.scale(a,b);break; case 4:x.rotate(a);break; case 5:x.translate(a,b);break; case 6:x.transform(a,b,c,d,e,f);break; case 7:x.setTransform(a,b,c,d,e,f);break; case 8:x.resetTransform();break;
    case 9:x.fillStyle=s;break; case 10:x.strokeStyle=s;break; case 11:x.globalAlpha=a;break; case 12:x.globalCompositeOperation=s;break; case 13:x.filter=s;break;
    case 14:x.lineWidth=a;break; case 15:x.lineCap=s;break; case 16:x.lineJoin=s;break; case 17:x.miterLimit=a;break; case 18:x.lineDashOffset=a;break;
    case 19:x.shadowColor=s;x.shadowBlur=a;x.shadowOffsetX=b;x.shadowOffsetY=c;break; case 20:x.font=s;break; case 21:x.textAlign=s;break; case 22:x.textBaseline=s;break; case 23:x.direction=s;break; case 24:x.imageSmoothingEnabled=!!a;break; case 25:x.imageSmoothingQuality=s;break;
    case 30:x.clearRect(a,b,c,d);break; case 31:x.fillRect(a,b,c,d);break; case 32:x.strokeRect(a,b,c,d);break;
    case 33:x.beginPath();break; case 34:x.closePath();break; case 35:x.moveTo(a,b);break; case 36:x.lineTo(a,b);break; case 37:x.quadraticCurveTo(a,b,c,d);break; case 38:x.bezierCurveTo(a,b,c,d,e,f);break; case 39:x.arc(a,b,c,d,e,!!f);break; case 40:x.arcTo(a,b,c,d,e);break; case 41:x.ellipse(a,b,c,d,e,f,g,!!h);break; case 42:x.rect(a,b,c,d);break; case 43:x.roundRect(a,b,c,d,e);break;
    case 44:x.fill(s || 'nonzero');break; case 45:x.stroke();break; case 46:x.clip(s || 'nonzero');break;
    case 47:a < 0 ? x.fillText(s,b,c) : x.fillText(s,b,c,a);break; case 48:a < 0 ? x.strokeText(s,b,c) : x.strokeText(s,b,c,a);break;
  }
});
EM_JS(void, c2d_dash, (int id, const float* data, int length), { Module.cppCanvasContexts[id].ctx.setLineDash(Array.from(HEAPF32.subarray(data>>2,(data>>2)+length))); });
// `data` null means "the cache already holds `key`" -- the C++ side owns the
// bookkeeping, so a null here is never a guess. `evenOdd` replaces the fill
// rule string: this runs a few hundred times a frame and a decoded JS string
// per call is pure waste when there are two possible values.
EM_JS(void, c2d_path, (int id, int key, const float* data, int count, int action, int evenOdd), {
  const cache = Module.cppCanvasPaths || (Module.cppCanvasPaths = new Map());
  let p = data ? null : cache.get(key);
  if (!p) {
    p = new Path2D();
    const v=HEAPF32.subarray(data>>2,(data>>2)+count*10);
    for(let i=0;i<count;i++){const q=i*10,n=v[q],cc=!!v[q+1],a=v.subarray(q+2,q+10); switch(n){case 0:p.moveTo(a[0],a[1]);break;case 1:p.lineTo(a[0],a[1]);break;case 2:p.quadraticCurveTo(a[0],a[1],a[2],a[3]);break;case 3:p.bezierCurveTo(a[0],a[1],a[2],a[3],a[4],a[5]);break;case 4:p.arc(a[0],a[1],a[2],a[3],a[4],cc);break;case 5:p.arcTo(a[0],a[1],a[2],a[3],a[4]);break;case 6:p.ellipse(a[0],a[1],a[2],a[3],a[4],a[5],a[6],cc);break;case 7:p.rect(a[0],a[1],a[2],a[3]);break;case 8:p.roundRect(a[0],a[1],a[2],a[3],a[4]);break;case 9:p.closePath();break;}}
    cache.set(key, p);
  }
  const x=Module.cppCanvasContexts[id].ctx, r=evenOdd?'evenodd':'nonzero';
  if(action===0)x.fill(p,r); else if(action===1)x.stroke(p); else x.clip(p,r);
});
EM_JS(void, c2d_path_drop, (int key), { if (Module.cppCanvasPaths) Module.cppCanvasPaths.delete(key); });
EM_JS(int, c2d_hit, (int id,double a,double b,int stroke,const char* rule), { const x=Module.cppCanvasContexts[id].ctx; return stroke ? x.isPointInStroke(a,b) : x.isPointInPath(a,b,UTF8ToString(rule)); });
EM_JS(double, c2d_measure, (int id,const char* text), { return Module.cppCanvasContexts[id].ctx.measureText(UTF8ToString(text)).width; });
EM_JS(void, c2d_draw, (int dst,int src,double a,double b,double c,double d,int sized), { const x=Module.cppCanvasContexts[dst].ctx, image=Module.cppCanvasContexts[src].surface; sized ? x.drawImage(image,a,b,c,d) : x.drawImage(image,a,b); });
EM_JS(int, c2d_get_pixels, (int id,int x,int y,int w,int h,std::uint8_t* out), { const d=Module.cppCanvasContexts[id].ctx.getImageData(x,y,w,h).data; HEAPU8.set(d,out); return d.length; });
EM_JS(void, c2d_put_pixels, (int id,const std::uint8_t* data,int sw,int sh,int dx,int dy), { const d=new ImageData(new Uint8ClampedArray(HEAPU8.slice(data,data+sw*sh*4)),sw,sh); Module.cppCanvasContexts[id].ctx.putImageData(d,dx,dy); });
EM_JS(void, c2d_image, (int id,const std::uint8_t* data,int iw,int ih,double dx,double dy,double dw,double dh,double alpha), {
  const pixels=new ImageData(new Uint8ClampedArray(HEAPU8.slice(data,data+iw*ih*4)),iw,ih);
  // putImageData ignores the transform, so the pixels go to a scratch surface
  // first and reach the destination through drawImage, which does not.
  const scratch = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(iw,ih) : document.createElement('canvas');
  scratch.width=iw; scratch.height=ih; scratch.getContext('2d').putImageData(pixels,0,0);
  const ctx=Module.cppCanvasContexts[id].ctx, was=ctx.globalAlpha;
  ctx.globalAlpha=was*alpha; ctx.drawImage(scratch,dx,dy,dw,dh); ctx.globalAlpha=was;
});
#define OP(code,a,b,c,d,e,f,g,h,s) c2d_op(contextId_,code,a,b,c,d,e,f,g,h,s)
#else
#define OP(code,a,b,c,d,e,f,g,h,s) ((void)0)
#endif

#ifndef __EMSCRIPTEN__
// ---------------------------------------------------------------------------
// Software backend: flatten -> scanline coverage -> blend. Curves are really
// flattened (not endpoint-approximated), fills honour both winding rules,
// strokes are expanded to outlines, and clips are coverage masks.
// ---------------------------------------------------------------------------
namespace {
constexpr int kSub = 5;
constexpr float kTau = 6.28318530718f;
using Matrix = std::array<float,6>;

struct Poly {
    std::vector<float> pts; std::vector<int> ends; std::vector<char> closed;
    void reset() { pts.clear(); ends.clear(); closed.clear(); }
    int begin() const { return ends.empty() ? 0 : ends.back(); }
    void finish(bool isClosed) {
        const int n = static_cast<int>(pts.size()/2);
        if (n - begin() < 1) { pts.resize(static_cast<size_t>(begin())*2); return; }
        ends.push_back(n); closed.push_back(isClosed ? 1 : 0);
    }
};

Poly gFlat, gDash, gOutline, gDevice;

float matrixScale(const Matrix& m) { const float d = std::abs(m[0]*m[3]-m[1]*m[2]); return d > 0 ? std::sqrt(d) : 1e-3f; }

void flatten(const Path2D& path, const Matrix& m, float scale, Poly& out) {
    out.reset();
    float cx=0, cy=0, sx=0, sy=0; bool open=false;
    const auto emit=[&](float x,float y){ out.pts.push_back(m[0]*x+m[2]*y+m[4]); out.pts.push_back(m[1]*x+m[3]*y+m[5]); };
    const auto start=[&](float x,float y){ if(open) out.finish(false); emit(x,y); open=true; cx=sx=x; cy=sy=y; };
    const auto steps=[&](float length){ return std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(0.f,length)*scale)*1.8f)),2,192); };
    const auto point=[&](float x,float y){ if(!open) start(x,y); else emit(x,y); cx=x; cy=y; };
    for (const Path2D::Segment& s : path.segments()) {
        const float* v = s.v;
        switch (s.command) {
        case Path2D::Command::Move: start(v[0],v[1]); break;
        case Path2D::Command::Line: point(v[0],v[1]); break;
        case Path2D::Command::Quadratic: {
            if (!open) start(cx,cy);
            const float x0=cx,y0=cy; const int n=steps(std::hypot(v[0]-x0,v[1]-y0)+std::hypot(v[2]-v[0],v[3]-v[1]));
            for(int i=1;i<=n;++i){ const float t=float(i)/n,u=1-t;
                emit(u*u*x0+2*u*t*v[0]+t*t*v[2], u*u*y0+2*u*t*v[1]+t*t*v[3]); }
            cx=v[2]; cy=v[3]; break; }
        case Path2D::Command::Bezier: {
            if (!open) start(cx,cy);
            const float x0=cx,y0=cy;
            const int n=steps(std::hypot(v[0]-x0,v[1]-y0)+std::hypot(v[2]-v[0],v[3]-v[1])+std::hypot(v[4]-v[2],v[5]-v[3]));
            for(int i=1;i<=n;++i){ const float t=float(i)/n,u=1-t, a=u*u*u,b=3*u*u*t,c=3*u*t*t,d=t*t*t;
                emit(a*x0+b*v[0]+c*v[2]+d*v[4], a*y0+b*v[1]+c*v[3]+d*v[5]); }
            cx=v[4]; cy=v[5]; break; }
        case Path2D::Command::Arc:
        case Path2D::Command::Ellipse: {
            const bool circle = s.command==Path2D::Command::Arc;
            const float ex=v[0], ey=v[1], rx=std::abs(v[2]), ry=circle?std::abs(v[2]):std::abs(v[3]);
            const float rot=circle?0.f:v[4], a0=circle?v[3]:v[5], a1=circle?v[4]:v[6];
            float sweep=a1-a0;
            if (!s.counterClockwise) { if (sweep>=kTau) sweep=kTau; else { sweep=std::fmod(sweep,kTau); if(sweep<0) sweep+=kTau; } }
            else { if (sweep<=-kTau) sweep=-kTau; else { sweep=std::fmod(sweep,kTau); if(sweep>0) sweep-=kTau; } }
            const float cs=std::cos(rot), sn=std::sin(rot);
            const auto at=[&](float t,float& ox,float& oy){ const float px=rx*std::cos(t), py=ry*std::sin(t); ox=ex+px*cs-py*sn; oy=ey+px*sn+py*cs; };
            const int n=std::clamp(static_cast<int>(std::ceil(std::abs(sweep)*(1.5f+std::sqrt(std::max(rx,ry)*scale)))),4,512);
            float px,py; at(a0,px,py); point(px,py);
            for(int i=1;i<=n;++i){ at(a0+sweep*i/n,px,py); emit(px,py); }
            cx=px; cy=py; break; }
        case Path2D::Command::ArcTo: {
            const float x0=cx,y0=cy, x1=v[0],y1=v[1], x2=v[2],y2=v[3], r=std::abs(v[4]);
            const float ux=x0-x1, uy=y0-y1, wx=x2-x1, wy=y2-y1;
            const float lu=std::hypot(ux,uy), lw=std::hypot(wx,wy);
            if (lu<1e-6f || lw<1e-6f || r<1e-6f) { point(x1,y1); break; }
            const float a0x=ux/lu, a0y=uy/lu, a1x=wx/lw, a1y=wy/lw;
            const float cosA=std::clamp(a0x*a1x+a0y*a1y,-1.f,1.f), half=std::acos(cosA)/2;
            if (std::abs(std::sin(half))<1e-6f) { point(x1,y1); break; }
            const float dist=r/std::tan(half);
            point(x1+a0x*dist, y1+a0y*dist); point(x1+a1x*dist, y1+a1y*dist); break; }
        case Path2D::Command::Rect: {
            if(open){ out.finish(false); open=false; }
            emit(v[0],v[1]); emit(v[0]+v[2],v[1]); emit(v[0]+v[2],v[1]+v[3]); emit(v[0],v[1]+v[3]);
            out.finish(true); cx=sx=v[0]; cy=sy=v[1]; break; }
        case Path2D::Command::RoundRect: {
            if(open){ out.finish(false); open=false; }
            const float x=std::min(v[0],v[0]+v[2]), y=std::min(v[1],v[1]+v[3]);
            const float w=std::abs(v[2]), h=std::abs(v[3]), r=std::clamp(v[4],0.f,std::min(w,h)/2);
            const int n=std::clamp(static_cast<int>(std::ceil(std::sqrt(r*scale)*2.2f)),2,48);
            const float cxs[4]={x+w-r,x+w-r,x+r,x+r}, cys[4]={y+r,y+h-r,y+h-r,y+r};
            for(int corner=0;corner<4;++corner){ const float a=-1.5707963f+corner*1.5707963f;
                for(int i=0;i<=n;++i){ const float t=a+i*1.5707963f/n; emit(cxs[corner]+std::cos(t)*r, cys[corner]+std::sin(t)*r); } }
            out.finish(true); cx=sx=x; cy=sy=y; break; }
        case Path2D::Command::Close:
            if(open){ out.finish(true); open=false; } cx=sx; cy=sy; break;
        }
    }
    if (open) out.finish(false);
}

// The pixel box a polygon can touch, intersected with the box the caller is
// allowed to write. That second box is the CLIP's, not just the surface's:
// scan converting a shape and then discarding it a pixel at a time against the
// clip is work with no output, and a full-screen fill under a small clip is
// exactly the shape this loop takes. SkDraw narrows the same way before it
// hands anything to SkScan.
bool polyBounds(const Poly& p, int cx0, int cy0, int cx1, int cy1, int& x0, int& y0, int& x1, int& y1) {
    if (p.pts.empty()) return false;
    float lo=1e30f, hi=-1e30f, top=1e30f, bottom=-1e30f;
    for (size_t i=0;i<p.pts.size();i+=2) {
        const float x=p.pts[i], y=p.pts[i+1];
        if (!(std::isfinite(x)&&std::isfinite(y))) continue;
        lo=std::min(lo,x); hi=std::max(hi,x); top=std::min(top,y); bottom=std::max(bottom,y);
    }
    if (hi<lo) return false;
    x0=std::max(cx0,static_cast<int>(std::floor(lo))); x1=std::min(cx1,static_cast<int>(std::ceil(hi))+1);
    y0=std::max(cy0,static_cast<int>(std::floor(top))); y1=std::min(cy1,static_cast<int>(std::ceil(bottom))+1);
    return x0<x1 && y0<y1;
}

// One edge, in the form the scanline walk reads it: the y span it covers, a
// point on it, and dx/dy so a crossing is a multiply. The slope is divided out
// once here rather than once per crossing -- an edge is crossed five times per
// scanline it touches -- and ylo/yhi are hoisted out of the inner loop, where
// they used to be a min and a max per subsample.
struct Edge { float ylo,yhi,x0,y0,slope,dir; };
struct Cross { float x,dir; };

// Ordering the crossings of one subsample line.
//
// Ordering among EQUAL x is not preserved, and does not need to be: the spans
// between equal crossings are zero-width, and both the winding total and the
// crossing parity after such a group are the same whichever way the group is
// walked. Sorting on x alone is therefore exact, and half the comparison.
inline void insertionSortByX(Cross* xs, size_t n) {
    for (size_t i=1;i<n;++i) {
        const Cross v=xs[i]; size_t j=i;
        while (j>0 && xs[j-1].x>v.x) { xs[j]=xs[j-1]; --j; }
        xs[j]=v;
    }
}

// A long run is bucketed by whole-pixel column rather than compared. The
// column a crossing falls in is an index into the same coverage buffer the
// scanline is already sized for, so the histogram costs nothing to address,
// and one insertion pass afterwards settles the order inside each column --
// which is cheap because a column holds one or two crossings. This is the
// case that matters: a stroked outline is thousands of one-pixel-tall edges,
// so a single scanline can carry a hundred crossings across a span of pixels
// no wider than that, and paying n log n unpredictable branches for it was
// most of what a text label cost.
//
// `hist` is all zeroes on entry and left that way on exit. When the crossings
// turn out to be spread far wider than they are numerous the histogram would
// cost more than it saves, so the run falls back to a comparison sort.
inline void sortCrossings(Cross* xs, size_t n, int bx0, int width,
                          std::vector<int>& hist, std::vector<Cross>& tmp) {
    const auto byX=[](const Cross& a,const Cross& b){ return a.x<b.x; };
    if (n<=40) { insertionSortByX(xs,n); return; }
    // Clamped in float space, before the cast: a crossing can legitimately
    // land far outside the box (the winding walk needs it, even though the
    // span it opens is clipped away), and converting 1e20f to int is not
    // something the standard defines. The first test also catches NaN.
    const float leftEdge=static_cast<float>(bx0), rightEdge=static_cast<float>(bx0+width);
    const auto column=[&](float x)->int {
        if (!(x>leftEdge)) return 0;
        if (x>=rightEdge) return width-1;
        return static_cast<int>(std::floor(x))-bx0;
    };
    int lo=width, hi=-1;
    for (size_t i=0;i<n;++i) {
        const int b=column(xs[i].x);
        ++hist[static_cast<size_t>(b)];
        if (b<lo) lo=b;
        if (b>hi) hi=b;
    }
    if (static_cast<size_t>(hi-lo) > 8*n) {
        for (size_t i=0;i<n;++i) hist[static_cast<size_t>(column(xs[i].x))]=0;
        std::sort(xs,xs+n,byX);
        return;
    }
    int running=0;
    for (int b=lo;b<=hi;++b) { const int c=hist[static_cast<size_t>(b)]; hist[static_cast<size_t>(b)]=running; running+=c; }
    if (tmp.size()<n) tmp.resize(n);
    for (size_t i=0;i<n;++i) tmp[static_cast<size_t>(hist[static_cast<size_t>(column(xs[i].x))]++)]=xs[i];
    for (int b=lo;b<=hi;++b) hist[static_cast<size_t>(b)]=0;
    std::copy(tmp.begin(),tmp.begin()+static_cast<std::ptrdiff_t>(n),xs);
    insertionSortByX(xs,n);
}

// --- scanline bands ---------------------------------------------------------
// A large fill is split across threads by SCANLINE BAND. Every emit callback in
// this file writes to storage indexed by [y*stride + x] and reads nothing that
// another band writes, so two bands can never touch the same pixel: the split
// is exact rather than an approximation, and the result is bit-identical to the
// sequential walk. Coverage for a row depends only on that row's crossings, so
// a band needs no state from the band above it beyond the edges still open at
// its first scanline, which it rebuilds itself.
//
// Only BIG paths are split. Measured over a frame of the game, ~740 fills a
// frame carry 14M bbox pixels, but 83% of that area is in the ~36 fills bigger
// than 256x256 -- and the thousands of small ones (glyphs, HUD boxes) are a
// twelfth of the pixels, where waking a thread costs more than the fill.
struct FillScratch {
    std::vector<Edge> active;
    std::vector<float> acc, run;
    std::vector<int> hist;
    std::vector<Cross> xs, sorted;
};

// Threads are created once and kept. A fill this size happens dozens of times a
// frame, and std::thread's construction alone costs more than one band's work.
class BandPool {
public:
    static BandPool& instance() { static BandPool pool; return pool; }
    int maxBands() const { return bands_; }
    FillScratch& scratch(int band) { return scratch_[static_cast<std::size_t>(band)]; }

    // Runs job(0) on the calling thread and job(1..bands-1) on the workers,
    // returning once every band is done. Not reentrant, and does not need to
    // be: nothing inside a fill starts another one.
    void run(int bands, const std::function<void(int)>& job) {
        if (bands <= 1) { job(0); return; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            bandCount_ = bands;
            next_.store(1, std::memory_order_relaxed);
            remaining_.store(bands - 1, std::memory_order_relaxed);
            ++generation_;
        }
        wake_.notify_all();
        job(0);
        std::unique_lock<std::mutex> lock(mutex_);
        // BOTH conditions, and the second is not redundant: a worker decrements
        // remaining_ when its band is done but is still inside the claim loop
        // for a moment afterwards. Returning on remaining_ alone let the NEXT
        // fill reset next_ under that worker, which then claimed a band from a
        // job whose std::function had already been destroyed with the frame
        // that made it -- a dangling call, and a crash a few seconds into a
        // scene busy enough to thread two fills back to back.
        idle_.wait(lock, [this] {
            return remaining_.load(std::memory_order_acquire) == 0 && inLoop_ == 0;
        });
        // Nothing may call through this again until run() publishes a new one.
        job_ = nullptr;
    }

private:
    BandPool() {
        const unsigned hardware = std::thread::hardware_concurrency();
        // Capped: past a handful of bands the fill is memory-bound and the
        // extra threads only add wake-up latency to every call.
        bands_ = static_cast<int>(std::min(hardware ? hardware : 1u, 8u));
        if (bands_ < 1) bands_ = 1;
        scratch_.resize(static_cast<std::size_t>(bands_));
        for (int i = 1; i < bands_; ++i) workers_.emplace_back([this] { loop(); });
    }
    ~BandPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            ++generation_;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
    }

    void loop() {
        std::uint64_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this, seen] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            const std::function<void(int)>* const job = job_;
            const int bands = bandCount_;
            // Woke after the fill it was meant for had already finished. There
            // is nothing to run and, more to the point, nothing valid to run.
            if (!job) continue;

            // Counted while the lock is held, so run() cannot observe zero
            // bands outstanding and start reusing next_ while this worker is
            // still claiming from it.
            ++inLoop_;
            lock.unlock();
            // Claimed rather than assigned, so a worker that woke late does not
            // leave its band unrun -- whoever is free takes the next one.
            for (;;) {
                const int band = next_.fetch_add(1, std::memory_order_relaxed);
                if (band >= bands) break;
                (*job)(band);
                remaining_.fetch_sub(1, std::memory_order_acq_rel);
            }
            lock.lock();
            --inLoop_;
            // Taking the lock above is also what publishes this band's pixels
            // to the thread waiting in run().
            idle_.notify_all();
        }
    }

    std::vector<std::thread> workers_;
    std::vector<FillScratch> scratch_;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    const std::function<void(int)>* job_ = nullptr;
    std::atomic<int> next_{0}, remaining_{0};
    std::uint64_t generation_ = 0;
    int bands_ = 1, bandCount_ = 0;
    // Workers currently inside the claim loop, which is not the same as bands
    // outstanding -- see run().
    int inLoop_ = 0;
    bool stop_ = false;
};

// Below these a fill stays on the calling thread. Rows, because a band thinner
// than this rebuilds more open edges than it rasterises; area, because a tall
// thin sliver is not worth waking anyone for.
constexpr int kMinBandRows = 32;
constexpr long long kMinThreadedArea = 1 << 15;

// Coverage comes out of the walk below in two shapes, and they are worth
// telling apart. A shape's EDGE gives partial coverage, one pixel at a time --
// that is `emit`. Its INTERIOR gives coverage of exactly 1 over a run of
// pixels, and feeding that through a per-pixel callback throws the run away:
// the caller ends up re-deriving a constant colour and re-blending it for
// every pixel of a flat fill, which is most of the pixels in a frame of this
// game (ground tiles, walls, panels). `emitSpan(x0, x1, y)` hands the run over
// whole instead, so the caller can memset it -- which is the split Skia draws
// between SkBlitter::blitH and blitAntiH, and the reason its solid fills cost
// what a memcpy costs.
//
// emitSpan(x0, x1, y) MUST be exactly equivalent to emit(x, y, 1.0f) for every
// x in [x0, x1).
template <class Emit, class EmitSpan>
void scanFill(const Poly& poly, bool evenOdd, int cx0, int cy0, int cx1, int cy1, Emit emit,
              EmitSpan emitSpan) {
    // Shared by every band and built once, on the calling thread.
    static std::vector<Edge> edges, byRow; static std::vector<int> rowStart, cursor;
    int bx0,by0,bx1,by1; if (!polyBounds(poly,cx0,cy0,cx1,cy1,bx0,by0,bx1,by1)) return;
    edges.clear();
    int first=0;
    for (size_t c=0;c<poly.ends.size();++c) {
        const int last=poly.ends[c];
        for (int i=first;i<last;++i) {
            const int j=(i+1==last)?first:i+1;
            const float ax=poly.pts[2*i], ay=poly.pts[2*i+1], bx=poly.pts[2*j], by=poly.pts[2*j+1];
            if (ay==by || !(std::isfinite(ax)&&std::isfinite(ay)&&std::isfinite(bx)&&std::isfinite(by))) continue;
            edges.push_back({std::min(ay,by),std::max(ay,by),ax,ay,(bx-ax)/(by-ay), by>ay?1.f:-1.f});
        }
        first=last;
    }
    if (edges.empty()) return;

    // Edges bucketed by the first scanline that can see them. This used to be
    // a sort of every edge by its top y, which is O(n log n) over a polygon
    // that -- once a stroke has been expanded to an outline -- runs to
    // thousands of edges. The bucket is an integer, so counting them lands
    // them in the same order for one pass each way.
    const int rows=by1-by0;
    const float boxTop=static_cast<float>(by0), boxBottom=static_cast<float>(by1);
    // The two range tests come before the cast, not after: an edge's y can be
    // any finite float, and converting one of those to int is not something
    // the standard defines. Past them, floor(ylo) is provably a row index.
    const auto firstRow=[&](const Edge& e)->int {
        if (e.yhi<=boxTop) return -1;              // wholly above the box
        if (!(e.ylo<boxBottom)) return -1;         // starts below it, or is NaN
        if (e.ylo<=boxTop) return 0;
        return static_cast<int>(std::floor(e.ylo))-by0;
    };
    rowStart.assign(static_cast<size_t>(rows)+1,0);
    for (const Edge& e : edges) { const int r=firstRow(e); if (r>=0) ++rowStart[static_cast<size_t>(r)]; }
    int total=0;
    for (int r=0;r<=rows;++r) { const int n=rowStart[static_cast<size_t>(r)]; rowStart[static_cast<size_t>(r)]=total; total+=n; }
    if (total==0) return;
    byRow.resize(static_cast<size_t>(total));
    cursor.assign(rowStart.begin(),rowStart.end());
    for (const Edge& e : edges) { const int r=firstRow(e); if (r>=0) byRow[static_cast<size_t>(cursor[static_cast<size_t>(r)]++)]=e; }

    const size_t span_count=static_cast<size_t>(bx1-bx0);
    const float weight=1.f/kSub;

    // One band unless the path is big enough to pay for the hand-off.
    BandPool& pool=BandPool::instance();
    const long long area=static_cast<long long>(rows)*static_cast<long long>(bx1-bx0);
    int bandCount=1;
    if (area>=kMinThreadedArea && rows>=2*kMinBandRows)
        bandCount=std::max(1,std::min(pool.maxBands(),rows/kMinBandRows));

    const auto runBand=[&](int band){
        FillScratch& sc=pool.scratch(band);
        std::vector<Edge>& active=sc.active;
        std::vector<float>& acc=sc.acc; std::vector<float>& run=sc.run;
        std::vector<int>& hist=sc.hist;
        std::vector<Cross>& xs=sc.xs; std::vector<Cross>& sorted=sc.sorted;
        const int ys=by0+static_cast<int>((static_cast<long long>(rows)*band)/bandCount);
        const int ye=by0+static_cast<int>((static_cast<long long>(rows)*(band+1))/bandCount);
        if (ys>=ye) return;
        acc.assign(span_count,0.f);
        run.assign(span_count,0.f);
        hist.assign(span_count,0);
        active.clear();
        // What this band inherits: edges opened on a row above it and not yet
        // retired at its first scanline. Walked in bucket order, which is the
        // order the sequential fill holds them in -- it appends in that order
        // and retires in place -- so the active list is identical either way.
        for (int r=0;r<ys-by0;++r)
            for (int k=rowStart[static_cast<size_t>(r)];k<rowStart[static_cast<size_t>(r)+1];++k)
                if (byRow[static_cast<size_t>(k)].yhi>static_cast<float>(ys))
                    active.push_back(byRow[static_cast<size_t>(k)]);
        for (int y=ys;y<ye;++y) {
            const float top=static_cast<float>(y);
            // Retire the edges this scanline has passed, in place and in order:
            // what survives is what remove_if used to leave behind.
            size_t keep=0;
            for (size_t i=0;i<active.size();++i) if (active[i].yhi>top) active[keep++]=active[i];
            active.resize(keep);
            for (int k=rowStart[static_cast<size_t>(y-by0)];k<rowStart[static_cast<size_t>(y-by0)+1];++k)
                active.push_back(byRow[static_cast<size_t>(k)]);
            if (active.empty()) continue;
            int lo=bx1, hi=bx0-1;      // the x range any span touched, so the pass below walks only that
            const auto span=[&](float a,float b){
                a=std::max(a,static_cast<float>(bx0)); b=std::min(b,static_cast<float>(bx1));
                if (b<=a) return;
                int ia=static_cast<int>(std::floor(a)), ib=static_cast<int>(std::floor(b));
                ia=std::clamp(ia,bx0,bx1-1); ib=std::clamp(ib,bx0,bx1-1);
                if (ia==ib) acc[static_cast<size_t>(ia-bx0)]+=(b-a)*weight;
                else {
                    // The whole-pixel interior used to be one write per pixel,
                    // which is what made a full-screen rect five passes over the
                    // width. Two difference-array entries carry it instead, summed
                    // by the running total in the emit pass below.
                    acc[static_cast<size_t>(ia-bx0)]+=(ia+1-a)*weight;
                    run[static_cast<size_t>(ia+1-bx0)]+=weight;
                    run[static_cast<size_t>(ib-bx0)]-=weight;
                    acc[static_cast<size_t>(ib-bx0)]+=(b-ib)*weight;
                }
                if (ia<lo) lo=ia;
                if (ib>hi) hi=ib;
            };
            if (xs.size()<active.size()) xs.resize(active.size());
            Cross* const cross=xs.data();
            for (int s=0;s<kSub;++s) {
                const float sy=y+(s+0.5f)/kSub;
                // Branchless on purpose: whether a given edge reaches this
                // subsample line is data the predictor cannot learn, and a
                // mispredict costs more than the crossing does. Every slot is
                // written and the cursor only advances for the ones that count,
                // which is safe because `xs` is sized for the whole active list.
                size_t nx=0;
                for (const Edge& ed : active) {
                    cross[nx].x=ed.x0+(sy-ed.y0)*ed.slope;
                    cross[nx].dir=ed.dir;
                    nx += (sy>=ed.ylo && sy<ed.yhi) ? 1u : 0u;
                }
                if (nx<2) continue;
                sortCrossings(cross,nx,bx0,bx1-bx0,hist,sorted);
                float winding=0; int crossings=0;
                for (size_t i=0;i+1<nx;++i) {
                    winding+=cross[i].dir; ++crossings;
                    if (evenOdd ? (crossings&1) : (winding!=0)) span(cross[i].x, cross[i+1].x);
                }
            }
            if (lo>hi) continue;
            float carry=0;
            // The interior is saturated rather than merely large: the run
            // array takes one `weight` per subsample line and there are kSub
            // of them, so a wholly covered pixel accumulates to 1 (a hair over
            // it, in float, which is what the old min() was trimming).
            int runStart=-1;
            for (int x=lo;x<=hi;++x) {
                const size_t k=static_cast<size_t>(x-bx0);
                carry+=run[k]; run[k]=0;
                const float a=acc[k]+carry; acc[k]=0;
                if (a>=1.f) { if (runStart<0) runStart=x; continue; }
                if (runStart>=0) { emitSpan(runStart,x,y); runStart=-1; }
                if (a>0.002f) emit(x,y,a);
            }
            if (runStart>=0) emitSpan(runStart,hi+1,y);
        }

    };
    if (bandCount>1) pool.run(bandCount,runBand); else runBand(0);
}

// Source-over onto one pixel, split so the two cases that carry a frame stay
// inline at the call site. Out of line the whole thing was the single most
// expensive symbol in a frame: it runs once per covered pixel and every caller
// is in this file.
//
// Both fast paths are the general form with the divide by the output alpha
// cancelling exactly, not an approximation of it -- an opaque source at full
// coverage (the interior of every rect, wall and sprite body) and a pixel that
// is already opaque (which, once a frame has laid its background down, is
// every pixel on screen). What is left is a partly transparent source landing
// on a partly transparent pixel, which is rare enough to pay for a call.
void blendOntoTranslucent(Color& d, Color c, float sa) {
    if (d.a==0) { d=Color{c.r,c.g,c.b,static_cast<std::uint8_t>(std::lround(sa*255))}; return; }
    const float da=d.a/255.f, oa=sa+da*(1-sa);
    if (oa<=0) { d=Color{0,0,0,0}; return; }
    d=Color{static_cast<std::uint8_t>(std::lround((c.r*sa+d.r*da*(1-sa))/oa)),
            static_cast<std::uint8_t>(std::lround((c.g*sa+d.g*da*(1-sa))/oa)),
            static_cast<std::uint8_t>(std::lround((c.b*sa+d.b*da*(1-sa))/oa)),
            static_cast<std::uint8_t>(std::lround(oa*255))};
}

#if defined(__GNUC__) || defined(__clang__)
// Not a hint: at -O2 the inliner decides the emit lambdas are already big
// enough and leaves this out of line, which puts a call on every covered pixel
// and makes it the top symbol in a frame.
__attribute__((always_inline)) inline
#else
inline
#endif
void blend(Color& d, Color c, float coverage) {
    const float sa=std::clamp(coverage,0.f,1.f)*(c.a/255.f);
    if (sa<=0.0005f) return;
    if (sa>=1.f) { d=Color{c.r,c.g,c.b,255}; return; }
    if (d.a==255) {
        const float keep=1.f-sa;
        d=Color{static_cast<std::uint8_t>(std::lround(c.r*sa+d.r*keep)),
                static_cast<std::uint8_t>(std::lround(c.g*sa+d.g*keep)),
                static_cast<std::uint8_t>(std::lround(c.b*sa+d.b*keep)),
                255};
        return;
    }
    blendOntoTranslucent(d,c,sa);
}

void pushContour(Poly& out, const std::vector<float>& pts) {
    const size_t n=pts.size()/2;
    if (n<3) return;
    double area=0;
    for (size_t i=0,j=n-1;i<n;j=i++) area += static_cast<double>(pts[2*j])*pts[2*i+1] - static_cast<double>(pts[2*i])*pts[2*j+1];
    if (area>0) for (size_t i=n;i-->0;) { out.pts.push_back(pts[2*i]); out.pts.push_back(pts[2*i+1]); }
    else for (size_t i=0;i<n;++i) { out.pts.push_back(pts[2*i]); out.pts.push_back(pts[2*i+1]); }
    out.finish(true);
}

void pushDisc(Poly& out, float x, float y, float r, float scale) {
    const int n=std::clamp(static_cast<int>(std::ceil(std::sqrt(r*scale)*4.f)),8,72);
    static std::vector<float> pts; pts.clear(); pts.reserve(n*2);
    for (int i=0;i<n;++i) { const float t=kTau*i/n; pts.push_back(x+std::cos(t)*r); pts.push_back(y+std::sin(t)*r); }
    pushContour(out,pts);
}

// Segment quads, join wedges and caps are emitted as separately closed, equally
// wound contours; a nonzero fill of the lot is exactly their union.
void strokeOutline(const Poly& src, float hw, int cap, int join, float miterLimit, float scale, Poly& out) {
    out.reset();
    if (hw<=0) return;
    // Scratch kept between calls: stroking runs once per path per frame and a
    // fresh pair of vectors here was the library's largest per-frame allocation.
    static std::vector<float> pts, quad;
    int first=0;
    for (size_t c=0;c<src.ends.size();++c) {
        const int last=src.ends[c]; const bool closed=src.closed[c]!=0;
        pts.clear();
        for (int i=first;i<last;++i) {
            const float x=src.pts[2*i], y=src.pts[2*i+1];
            if (!(std::isfinite(x)&&std::isfinite(y))) continue;
            if (pts.size()>=2 && std::abs(pts[pts.size()-2]-x)<1e-6f && std::abs(pts.back()-y)<1e-6f) continue;
            pts.push_back(x); pts.push_back(y);
        }
        first=last;
        if (closed && pts.size()>=4 && std::abs(pts[0]-pts[pts.size()-2])<1e-6f && std::abs(pts[1]-pts.back())<1e-6f) pts.resize(pts.size()-2);
        const int n=static_cast<int>(pts.size()/2);
        if (n==0) continue;
        if (n==1) {
            if (cap==1) pushDisc(out,pts[0],pts[1],hw,scale);
            else if (cap==2) { quad={pts[0]-hw,pts[1]-hw,pts[0]+hw,pts[1]-hw,pts[0]+hw,pts[1]+hw,pts[0]-hw,pts[1]+hw}; pushContour(out,quad); }
            continue;
        }
        const int segments = closed ? n : n-1;
        for (int i=0;i<segments;++i) {
            const int j=(i+1)%n;
            const float ax=pts[2*i], ay=pts[2*i+1], bx=pts[2*j], by=pts[2*j+1];
            const float dx=bx-ax, dy=by-ay, len=std::hypot(dx,dy);
            if (len<1e-9f) continue;
            const float nx=-dy/len*hw, ny=dx/len*hw;
            quad={ax+nx,ay+ny,bx+nx,by+ny,bx-nx,by-ny,ax-nx,ay-ny};
            pushContour(out,quad);
        }
        const int joins = closed ? n : n-1;
        for (int k = closed?0:1; k<joins; ++k) {
            const int prev=(k-1+n)%n, cur=k, nxt=(k+1)%n;
            const float px=pts[2*cur], py=pts[2*cur+1];
            float d0x=px-pts[2*prev], d0y=py-pts[2*prev+1], d1x=pts[2*nxt]-px, d1y=pts[2*nxt+1]-py;
            const float l0=std::hypot(d0x,d0y), l1=std::hypot(d1x,d1y);
            if (l0<1e-9f||l1<1e-9f) continue;
            d0x/=l0; d0y/=l0; d1x/=l1; d1y/=l1;
            const float cross=d0x*d1y-d0y*d1x;
            if (std::abs(cross)<1e-6f && d0x*d1x+d0y*d1y>0) continue;
            if (join==1) { pushDisc(out,px,py,hw,scale); continue; }
            const float side = cross>0 ? -1.f : 1.f;
            const float o0x=-d0y*hw*side, o0y=d0x*hw*side, o1x=-d1y*hw*side, o1y=d1x*hw*side;
            // Only the OUTER wedge is emitted, and only when no miter quad
            // takes its place. The inner one -- {p, p-o0, p-o1} -- lies inside
            // the overlap of the two segment quads that meet here, and the
            // outer one lies inside the miter quad, so both are contours whose
            // region the union already contains: dropping them leaves the
            // filled shape identical and takes 40% of the edges out of a
            // stroke. That matters because a stroked glyph run is thousands of
            // these, and scanFill's cost is per edge per scanline.
            bool mitered=false;
            if (join==0) {
                const float denom=-cross;
                if (std::abs(denom)>=1e-9f) {
                    const float rx=o1x-o0x, ry=o1y-o0y;
                    const float t=(rx*(-d1y)-ry*(-d1x))/denom;
                    const float qx=px+o0x+t*d0x, qy=py+o0y+t*d0y;
                    if (std::hypot(qx-px,qy-py) <= miterLimit*hw) {
                        quad={px,py,px+o0x,py+o0y,qx,qy,px+o1x,py+o1y};
                        pushContour(out,quad);
                        mitered=true;
                    }
                }
            }
            if (!mitered) { quad={px,py,px+o0x,py+o0y,px+o1x,py+o1y}; pushContour(out,quad); }
        }
        if (closed || cap==0) continue;
        for (int e=0;e<2;++e) {
            const int at = e==0 ? 0 : n-1;
            const int other = e==0 ? 1 : n-2;
            float dx=pts[2*at]-pts[2*other], dy=pts[2*at+1]-pts[2*other+1];
            const float len=std::hypot(dx,dy); if (len<1e-9f) continue;
            dx/=len; dy/=len;
            const float ex=pts[2*at], ey=pts[2*at+1];
            if (cap==1) { pushDisc(out,ex,ey,hw,scale); continue; }
            const float nx=-dy*hw, ny=dx*hw;
            quad={ex+nx,ey+ny,ex+nx+dx*hw,ey+ny+dy*hw,ex-nx+dx*hw,ey-ny+dy*hw,ex-nx,ey-ny};
            pushContour(out,quad);
        }
    }
}

void applyDash(const Poly& src, const std::vector<float>& dash, float offset, Poly& out) {
    out.reset();
    float total=0; for (float d : dash) total += std::max(0.f,d);
    if (total<=1e-6f) { out=src; return; }
    const size_t count=dash.size();
    int first=0;
    for (size_t c=0;c<src.ends.size();++c) {
        const int last=src.ends[c]; const bool closed=src.closed[c]!=0;
        size_t index=0; float remaining=0; bool on=true;
        float carry=std::fmod(offset,total*((count&1)?2:1)); if (carry<0) carry+=total*((count&1)?2:1);
        while (carry>0) { const float d=std::max(0.f,dash[index%count]); if (carry<d) { remaining=d-carry; carry=0; } else { carry-=d; ++index; on=!on; } }
        if (remaining<=0) { remaining=std::max(0.f,dash[index%count]); }
        bool started=false;
        const int total_points = closed ? (last-first+1) : (last-first);
        for (int i=1;i<total_points;++i) {
            const int a=first+((i-1)%(last-first)), b=first+(i%(last-first));
            float ax=src.pts[2*a], ay=src.pts[2*a+1];
            const float bx=src.pts[2*b], by=src.pts[2*b+1];
            float segment=std::hypot(bx-ax,by-ay);
            while (segment>1e-9f) {
                if (on && !started) { out.pts.push_back(ax); out.pts.push_back(ay); started=true; }
                if (remaining>=segment) {
                    remaining-=segment;
                    if (on) { out.pts.push_back(bx); out.pts.push_back(by); }
                    segment=0;
                } else {
                    const float t=remaining/segment;
                    const float mx=ax+(bx-ax)*t, my=ay+(by-ay)*t;
                    if (on) { out.pts.push_back(mx); out.pts.push_back(my); out.finish(false); started=false; }
                    ax=mx; ay=my; segment-=remaining;
                    ++index; on=!on; remaining=std::max(1e-6f,dash[index%count]);
                }
            }
        }
        if (started) out.finish(false);
        first=last;
    }
}

void transformPoly(const Poly& in, const Matrix& m, Poly& out) {
    out.reset();
    out.pts.resize(in.pts.size());
    for (size_t i=0;i<in.pts.size();i+=2) {
        const float x=in.pts[i], y=in.pts[i+1];
        out.pts[i]=m[0]*x+m[2]*y+m[4]; out.pts[i+1]=m[1]*x+m[3]*y+m[5];
    }
    out.ends=in.ends; out.closed=in.closed;
}

// 5x7 cell font, column-major bits, ASCII 32..126.
const unsigned char kFont[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5f,0x00,0x00}, {0x00,0x03,0x00,0x03,0x00}, {0x14,0x7f,0x14,0x7f,0x14}, {0x24,0x2a,0x7f,0x2a,0x12},
    {0x63,0x13,0x08,0x64,0x63}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x00,0x03,0x00,0x00}, {0x00,0x1c,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1c,0x00},
    {0x2a,0x1c,0x3e,0x1c,0x2a}, {0x08,0x08,0x3e,0x08,0x08}, {0x00,0x40,0x30,0x10,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x60,0x10,0x08,0x04,0x03}, {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00}, {0x62,0x51,0x49,0x49,0x46}, {0x41,0x49,0x49,0x49,0x36},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1e}, {0x00,0x36,0x36,0x00,0x00}, {0x40,0x36,0x16,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x59,0x09,0x06}, {0x3e,0x41,0x5d,0x55,0x1e}, {0x7c,0x12,0x11,0x12,0x7c}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01}, {0x3e,0x41,0x49,0x49,0x3a},
    {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00}, {0x30,0x40,0x40,0x41,0x3f}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x06,0x08,0x30,0x7f}, {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e},
    {0x7f,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x7f,0x20,0x18,0x20,0x7f}, {0x63,0x14,0x08,0x14,0x63}, {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7f,0x41,0x41},
    {0x03,0x04,0x08,0x10,0x60}, {0x41,0x41,0x7f,0x00,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x00,0x00},
    {0x20,0x54,0x54,0x54,0x78}, {0x7f,0x44,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x44,0x7f}, {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7e,0x09,0x09,0x02}, {0x0c,0x52,0x52,0x52,0x3e}, {0x7f,0x04,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x38,0x04,0x78}, {0x7c,0x04,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7e,0x12,0x12,0x12,0x0c}, {0x0c,0x12,0x12,0x12,0x7e}, {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x4c,0x54,0x74,0x24}, {0x04,0x3f,0x44,0x44,0x20},
    {0x3c,0x40,0x40,0x40,0x7c}, {0x1c,0x20,0x40,0x20,0x1c}, {0x3c,0x40,0x38,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44}, {0x0e,0x50,0x50,0x50,0x3e},
    {0x44,0x64,0x54,0x4c,0x44}, {0x00,0x08,0x36,0x41,0x41}, {0x00,0x00,0x7f,0x00,0x00}, {0x41,0x41,0x36,0x08,0x00}, {0x18,0x04,0x08,0x10,0x0c},
};
} // namespace
#endif

Canvas::Canvas(int w,int h,std::string id) : width_(std::max(1,w)),height_(std::max(1,h)),elementId_(std::move(id)),logicalWidth_(width_),logicalHeight_(height_) {
#ifdef __EMSCRIPTEN__
  contextId_=c2d_create(elementId_.c_str(),width_,height_,0);
#else
  pixels_.assign(static_cast<size_t>(width_)*height_,Color{255,255,255});
#endif
}
Canvas::Canvas(int w,int h,bool virtualCanvas) : width_(std::max(1,w)),height_(std::max(1,h)),virtual_(virtualCanvas),logicalWidth_(width_),logicalHeight_(height_) {
#ifdef __EMSCRIPTEN__
  contextId_=c2d_create("",width_,height_,1);
#else
  pixels_.assign(static_cast<size_t>(width_)*height_,Color{0,0,0,0});
#endif
}
Canvas Canvas::createVirtual(int w,int h) { return Canvas(w,h,true); }
void Canvas::setLogicalSize(int w,int h) {
  logicalWidth_ = w > 0 ? w : width_;
  logicalHeight_ = h > 0 ? h : height_;
}
Canvas::Canvas(Canvas&& other) noexcept
    : width_(other.width_), height_(other.height_), contextId_(other.contextId_), virtual_(other.virtual_),
      elementId_(std::move(other.elementId_)),
      logicalWidth_(other.logicalWidth_), logicalHeight_(other.logicalHeight_),
      fill_(other.fill_), stroke_(other.stroke_),
      lineWidth_(other.lineWidth_), currentPath_(std::move(other.currentPath_)) {
  other.contextId_ = -1;
#ifdef __EMSCRIPTEN__
  web_ = std::move(other.web_); webStack_ = std::move(other.webStack_);
  pendingSaves_ = other.pendingSaves_; other.pendingSaves_ = 0;
#else
  pixels_ = std::move(other.pixels_);
  state_ = std::move(other.state_); stack_ = std::move(other.stack_);
#endif
}
Canvas& Canvas::operator=(Canvas&& other) noexcept {
  if (this == &other) return *this;
#ifdef __EMSCRIPTEN__
  if (contextId_ >= 0) c2d_destroy(contextId_);
#endif
  width_=other.width_; height_=other.height_; contextId_=other.contextId_; virtual_=other.virtual_;
  elementId_=std::move(other.elementId_); fill_=other.fill_; stroke_=other.stroke_; lineWidth_=other.lineWidth_;
  currentPath_=std::move(other.currentPath_); other.contextId_=-1;
  logicalWidth_=other.logicalWidth_; logicalHeight_=other.logicalHeight_;
#ifdef __EMSCRIPTEN__
  web_=std::move(other.web_); webStack_=std::move(other.webStack_);
  pendingSaves_=other.pendingSaves_; other.pendingSaves_=0;
#else
  pixels_=std::move(other.pixels_);
  state_=std::move(other.state_); stack_=std::move(other.stack_);
#endif
  return *this;
}
Canvas::~Canvas() {
#ifdef __EMSCRIPTEN__
  if (contextId_ >= 0) c2d_destroy(contextId_);
#endif
}
void Canvas::present(const std::string& id) {
#ifdef __EMSCRIPTEN__
  c2d_present(contextId_, id.c_str());
#else
  (void)id;
#endif
}
// Hex, not rgba(): the alpha channel is already an 8-bit integer, so
// `#rrggbbaa` names exactly the same colour, and it costs one fixed-size
// buffer instead of four std::to_string allocations -- one of which formats a
// float -- on every colour change of every frame. The browser parses it faster
// too, which is what the count of these actually pays for.
static std::string css(Color c) {
  static constexpr char kHex[] = "0123456789abcdef";
  char buf[9] = {'#',
                 kHex[c.r >> 4], kHex[c.r & 15],
                 kHex[c.g >> 4], kHex[c.g & 15],
                 kHex[c.b >> 4], kHex[c.b & 15],
                 kHex[c.a >> 4], kHex[c.a & 15]};
  return std::string(buf, c.a == 255 ? 7 : 9);
}
#ifdef __EMSCRIPTEN__
// Materialises the saves that were deferred. The snapshot pushed here is the
// state as it was when save() was called: flushing happens BEFORE the change
// that forced it, and nothing between the save and that change altered a thing
// -- that is exactly the condition under which the save was safe to defer.
void Canvas::flushSaves() {
  while (pendingSaves_ > 0) { --pendingSaves_; webStack_.push_back(web_); OP(0,0,0,0,0,0,0,0,0,""); }
}
// Setting a value the context already holds is a no-op in the browser, so it
// is a no-op here too. `MIRROR` is the whole pattern: compare, and on a real
// change unwind any deferred save before the write that needs unwinding.
#define MIRROR(field, value) \
  do { if (web_.field == (value)) return; flushSaves(); web_.field = (value); } while (0)
#else
#define MIRROR(field, value) do { } while (0)
#endif

void Canvas::save() {
#ifdef __EMSCRIPTEN__
  ++pendingSaves_;
#else
  OP(0,0,0,0,0,0,0,0,0,"");
  stack_.push_back(state_);
#endif
}
void Canvas::restore() {
#ifdef __EMSCRIPTEN__
  // A save that never had to be materialised has nothing to restore.
  if (pendingSaves_ > 0) { --pendingSaves_; return; }
  if (!webStack_.empty()) { web_ = std::move(webStack_.back()); webStack_.pop_back(); }
  OP(1,0,0,0,0,0,0,0,0,"");
#else
  OP(1,0,0,0,0,0,0,0,0,"");
  if (!stack_.empty()) { state_=std::move(stack_.back()); stack_.pop_back(); fill_=state_.fill; stroke_=state_.stroke; lineWidth_=state_.lineWidth; }
#endif
}
void Canvas::reset() {
#ifdef __EMSCRIPTEN__
  // reset() empties the context's own save stack, so the mirror drops both its
  // stack and its deferred saves rather than trying to reconcile them, and
  // returns to the defaults reset() leaves behind.
  pendingSaves_ = 0; webStack_.clear(); web_ = WebState{};
#endif
  OP(2,0,0,0,0,0,0,0,0,""); resetTransform();
#ifndef __EMSCRIPTEN__
  state_=State{}; stack_.clear();
#endif
}
// The transform is part of what restore() unwinds, so a deferred save has to be
// materialised before one is applied. There is no redundancy to eliminate here
// -- a transform is a composition, not an assignment.
void Canvas::scale(float a,float b) {
#ifdef __EMSCRIPTEN__
  flushSaves();
#endif
  OP(3,a,b,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  auto& m=state_.matrix; m[0]*=a; m[1]*=a; m[2]*=b; m[3]*=b;
#endif
}
void Canvas::rotate(float a) {
#ifdef __EMSCRIPTEN__
  flushSaves();
#endif
  OP(4,a,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  auto& m=state_.matrix; const float c=std::cos(a), s=std::sin(a), m0=m[0],m1=m[1],m2=m[2],m3=m[3];
  m[0]=m0*c+m2*s; m[1]=m1*c+m3*s; m[2]=m2*c-m0*s; m[3]=m3*c-m1*s;
#endif
}
void Canvas::translate(float a,float b) {
#ifdef __EMSCRIPTEN__
  flushSaves();
#endif
  OP(5,a,b,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  auto& m=state_.matrix; m[4]+=m[0]*a+m[2]*b; m[5]+=m[1]*a+m[3]*b;
#endif
}
void Canvas::transform(float a,float b,float c,float d,float e,float f) {
#ifdef __EMSCRIPTEN__
  flushSaves();
#endif
  OP(6,a,b,c,d,e,f,0,0,"");
#ifndef __EMSCRIPTEN__
  const auto m=state_.matrix; auto& o=state_.matrix;
  o[0]=m[0]*a+m[2]*b; o[1]=m[1]*a+m[3]*b; o[2]=m[0]*c+m[2]*d; o[3]=m[1]*c+m[3]*d;
  o[4]=m[0]*e+m[2]*f+m[4]; o[5]=m[1]*e+m[3]*f+m[5];
#endif
}
void Canvas::setTransform(float a,float b,float c,float d,float e,float f) {
#ifdef __EMSCRIPTEN__
  flushSaves();
#endif
  OP(7,a,b,c,d,e,f,0,0,"");
#ifndef __EMSCRIPTEN__
  state_.matrix={a,b,c,d,e,f};
#endif
}
void Canvas::resetTransform() {
#ifdef __EMSCRIPTEN__
  flushSaves();
#endif
  OP(8,0,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  state_.matrix={1,0,0,1,0,0};
#endif
}
#ifndef __EMSCRIPTEN__
std::array<float,6> Canvas::currentTransform() const { return state_.matrix; }
void Canvas::blitDevice(const std::uint8_t* rgba,int iw,int ih,int dx,int dy) {
  if (!rgba || iw<=0 || ih<=0 || state_.alpha<=0) return;
  const int x0=std::max(0,dx), x1=std::min(width_,dx+iw);
  const int y0=std::max(0,dy), y1=std::min(height_,dy+ih);
  if (x0>=x1||y0>=y1) return;
  const bool clipped=state_.clip!=nullptr;
  const float alpha=state_.alpha;
  for (int y=y0;y<y1;++y) {
    const std::uint8_t* src=rgba+(static_cast<std::size_t>(y-dy)*iw+(x0-dx))*4;
    Color* dst=pixels_.data()+static_cast<std::size_t>(y)*width_+x0;
    for (int x=x0;x<x1;++x,src+=4,++dst) {
      // Fully transparent is the common case over a run's bounding box -- a
      // glyph covers a fraction of the box it is baked into -- so it is the
      // first thing tested and the cheapest thing to skip.
      if (src[3]==0) continue;
      float a=src[3]*(1.f/255.f)*alpha;
      if (clipped) a*=clipAt(x,y);
      if (a<=0.f) continue;
      blend(*dst,Color{src[0],src[1],src[2],255},a);
    }
  }
}
#endif
// The css() string is only ever read by the browser context, so building it on
// the native path was one heap allocation per colour change per frame.
void Canvas::setFillStyle(Color c){fill_=c;
#ifdef __EMSCRIPTEN__
  setFillStyle(css(c));
#else
  state_.fill=c;
#endif
}
void Canvas::setStrokeStyle(Color c){stroke_=c;
#ifdef __EMSCRIPTEN__
  setStrokeStyle(css(c));
#else
  state_.stroke=c;
#endif
}
void Canvas::setFillStyle(const std::string&s){MIRROR(fill,s);OP(9,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::setStrokeStyle(const std::string&s){MIRROR(stroke,s);OP(10,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::setGlobalAlpha(float a){MIRROR(alpha,a);OP(11,a,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  state_.alpha=std::clamp(a,0.f,1.f);
#endif
}
void Canvas::setGlobalCompositeOperation(const std::string&s){MIRROR(composite,s);OP(12,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::setFilter(const std::string&s){MIRROR(filter,s);OP(13,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::setLineWidth(float a){lineWidth_=std::max(0.f,a);MIRROR(lineWidth,lineWidth_);OP(14,lineWidth_,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  state_.lineWidth=lineWidth_;
#endif
}
void Canvas::setLineCap(const std::string&s){MIRROR(lineCap,s);OP(15,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  state_.lineCap = s=="round"?1 : s=="square"?2 : 0;
#endif
}
void Canvas::setLineJoin(const std::string&s){MIRROR(lineJoin,s);OP(16,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  state_.lineJoin = s=="round"?1 : s=="bevel"?2 : 0;
#endif
}
void Canvas::setMiterLimit(float a){MIRROR(miterLimit,a);OP(17,a,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  state_.miterLimit=std::max(1.f,a);
#endif
}
void Canvas::setLineDash(const std::vector<float>&v) {
#ifdef __EMSCRIPTEN__
  MIRROR(dash,v);
  c2d_dash(contextId_, v.data(), static_cast<int>(v.size()));
#else
  state_.dash=v;
#endif
}
void Canvas::setLineDashOffset(float a){MIRROR(dashOffset,a);OP(18,a,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  state_.dashOffset=a;
#endif
}
void Canvas::setShadow(Color c,float a,float b,float d){
#ifdef __EMSCRIPTEN__
  const std::string s=css(c);
  if (web_.shadowColour==s && web_.shadowBlur==a && web_.shadowOffsetX==b && web_.shadowOffsetY==d) return;
  flushSaves();
  web_.shadowColour=s; web_.shadowBlur=a; web_.shadowOffsetX=b; web_.shadowOffsetY=d;
  OP(19,a,b,d,0,0,0,0,0,s.c_str());
#else
  (void)c;(void)a;(void)b;(void)d;
#endif
}
void Canvas::setFont(const std::string&s){MIRROR(font,s);OP(20,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  for (size_t i=0;i<s.size();++i) if (std::isdigit(static_cast<unsigned char>(s[i]))) { state_.fontSize=std::max(1.f,std::strtof(s.c_str()+i,nullptr)); break; }
  std::string f=s; for (char& c : f) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  state_.fontFamily = f.find("mono")!=std::string::npos||f.find("courier")!=std::string::npos ? 2
                    : f.find("sans")!=std::string::npos ? 0
                    : f.find("serif")!=std::string::npos||f.find("times")!=std::string::npos||f.find("georgia")!=std::string::npos ? 1 : 0;
#endif
}
void Canvas::setTextAlign(const std::string&s){MIRROR(textAlign,s);OP(21,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  state_.textAlign = (s=="center")?1 : (s=="right"||s=="end")?2 : 0;
#endif
}
void Canvas::setTextBaseline(const std::string&s){MIRROR(textBaseline,s);OP(22,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  state_.textBaseline = (s=="top"||s=="hanging")?0 : (s=="middle")?1 : (s=="bottom"||s=="ideographic")?2 : 3;
#endif
}
void Canvas::setDirection(const std::string&s){MIRROR(direction,s);OP(23,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::setImageSmoothingEnabled(bool a){MIRROR(smoothing,a);OP(24,a,0,0,0,0,0,0,0,"");}
void Canvas::setImageSmoothingQuality(const std::string&s){MIRROR(smoothingQuality,s);OP(25,0,0,0,0,0,0,0,0,s.c_str());}
void Canvas::clear(Color c) {
#ifdef __EMSCRIPTEN__
  save(); resetTransform(); setFillStyle(c); fillRect(0,0,width_,height_); restore();
#else
  std::fill(pixels_.begin(), pixels_.end(), c);
#endif
}
void Canvas::clearRect(float a,float b,float c,float d) {
  OP(30,a,b,c,d,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  Path2D box; box.rect(a,b,c,d);
  flatten(box, state_.matrix, matrixScale(state_.matrix), gFlat);
  int cx0,cy0,cx1,cy1; drawBounds(cx0,cy0,cx1,cy1);
  const bool clippedClear=state_.clip!=nullptr;
  scanFill(gFlat,false,cx0,cy0,cx1,cy1,[&](int x,int y,float cov){
    const float keep=1.f-cov*clipAt(x,y);
    Color& p=pixels_[static_cast<size_t>(y)*width_+x];
    p.a=static_cast<std::uint8_t>(std::lround(p.a*keep));
    if (p.a==0) p=Color{0,0,0,0};
  }, [&](int x0,int x1,int y){
    Color* const row=pixels_.data()+static_cast<size_t>(y)*width_;
    // keep == 0 for the whole run, and a pixel whose alpha reaches zero is
    // cleared outright -- the same two lines the per-pixel form ends on.
    if (!clippedClear) { std::fill(row+x0,row+x1,Color{0,0,0,0}); return; }
    for (int x=x0;x<x1;++x) {
      const float keep=1.f-clipAt(x,y);
      Color& p=row[x];
      p.a=static_cast<std::uint8_t>(std::lround(p.a*keep));
      if (p.a==0) p=Color{0,0,0,0};
    }
  });
#endif
}
void Canvas::fillRect(float a,float b,float c,float d) {
  OP(31,a,b,c,d,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  Path2D box; box.rect(a,b,c,d); fillDevice(box,false,state_.fill);
#endif
}
void Canvas::strokeRect(float a,float b,float c,float d) {
  OP(32,a,b,c,d,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  Path2D box; box.rect(a,b,c,d); strokeDevice(box);
#endif
}
void Canvas::beginPath(){currentPath_=Path2D{};OP(33,0,0,0,0,0,0,0,0,"");} void Canvas::closePath(){currentPath_.closePath();OP(34,0,0,0,0,0,0,0,0,"");} void Canvas::moveTo(float a,float b){currentPath_.moveTo(a,b);OP(35,a,b,0,0,0,0,0,0,"");} void Canvas::lineTo(float a,float b){currentPath_.lineTo(a,b);OP(36,a,b,0,0,0,0,0,0,"");}
void Canvas::quadraticCurveTo(float a,float b,float c,float d){currentPath_.quadraticCurveTo(a,b,c,d);OP(37,a,b,c,d,0,0,0,0,"");} void Canvas::bezierCurveTo(float a,float b,float c,float d,float e,float f){currentPath_.bezierCurveTo(a,b,c,d,e,f);OP(38,a,b,c,d,e,f,0,0,"");} void Canvas::arc(float a,float b,float c,float d,float e,bool f){currentPath_.arc(a,b,c,d,e,f);OP(39,a,b,c,d,e,f,0,0,"");} void Canvas::arcTo(float a,float b,float c,float d,float e){currentPath_.arcTo(a,b,c,d,e);OP(40,a,b,c,d,e,0,0,0,"");} void Canvas::ellipse(float a,float b,float c,float d,float e,float f,float g,bool h){currentPath_.ellipse(a,b,c,d,e,f,g,h);OP(41,a,b,c,d,e,f,g,h,"");} void Canvas::rect(float a,float b,float c,float d){currentPath_.rect(a,b,c,d);OP(42,a,b,c,d,0,0,0,0,"");} void Canvas::roundRect(float a,float b,float c,float d,float e){currentPath_.roundRect(a,b,c,d,e);OP(43,a,b,c,d,e,0,0,0,"");}
void Canvas::fill(const std::string&s){
  OP(44,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  fillDevice(currentPath_, s=="evenodd", state_.fill);
#endif
}
void Canvas::stroke() {
  OP(45,0,0,0,0,0,0,0,0,"");
#ifndef __EMSCRIPTEN__
  strokeDevice(currentPath_);
#endif
}
void Canvas::clip(const std::string&s){
#ifdef __EMSCRIPTEN__
  // The clip region is save/restore state, so a deferred save has to exist
  // before one is narrowed.
  flushSaves();
#endif
  OP(46,0,0,0,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  clip(currentPath_, s);
#endif
}
#ifdef __EMSCRIPTEN__
// One scratch buffer: a fresh vector per path was an allocation on every fill
// of every frame, and the packed copy is consumed before the next call needs
// the space.
static const std::vector<float>& pack(const Path2D&p){static std::vector<float>r;r.clear();r.reserve(p.segments().size()*10);for(auto&q:p.segments()){r.push_back((float)q.command);r.push_back(q.counterClockwise);for(float x:q.v)r.push_back(x);}return r;}

namespace {
// The C++ half of the retained-path cache: what the page is known to hold, and
// in what order it was told. Bounded, and evicted oldest-first, so a client
// that draws thousands of distinct paths cannot grow the page's map without
// limit. Sized well above the ~350 paths a busy frame touches, so a path drawn
// every frame is never the one evicted.
constexpr std::size_t kMaxCachedPaths = 2048;
std::unordered_map<std::uint32_t,std::uint32_t> gCachedRevision;
std::deque<std::uint32_t> gCacheOrder;

// True when the page already holds this exact geometry under this key, in
// which case the caller sends the key alone. Records the geometry as sent
// either way, because the caller sends it when this returns false.
bool pathAlreadySent(const Path2D& path) {
    const std::uint32_t key = path.cacheKey();
    const auto it = gCachedRevision.find(key);
    if (it != gCachedRevision.end()) {
        if (it->second == path.revision()) return true;
        it->second = path.revision();
        return false;
    }
    gCacheOrder.push_back(key);
    if (gCacheOrder.size() > kMaxCachedPaths) {
        const std::uint32_t evicted = gCacheOrder.front();
        gCacheOrder.pop_front();
        gCachedRevision.erase(evicted);
        c2d_path_drop(evicted);
    }
    gCachedRevision.emplace(key, path.revision());
    return false;
}
} // namespace

// action: 0 fill, 1 stroke, 2 clip.
static void emitPath(int contextId, const Path2D& path, int action, bool evenOdd) {
    const std::uint32_t key = path.cacheKey();
    if (pathAlreadySent(path)) { c2d_path(contextId, key, nullptr, 0, action, evenOdd); return; }
    const std::vector<float>& v = pack(path);
    c2d_path(contextId, key, v.data(), static_cast<int>(path.segments().size()), action, evenOdd);
}
#endif
void Canvas::fill(const Path2D&p,const std::string&s) {
#ifdef __EMSCRIPTEN__
  emitPath(contextId_, p, 0, s=="evenodd");
#else
  fillDevice(p, s=="evenodd", state_.fill);
#endif
}
void Canvas::stroke(const Path2D&p) {
#ifdef __EMSCRIPTEN__
  emitPath(contextId_, p, 1, false);
#else
  strokeDevice(p);
#endif
}
void Canvas::clip(const Path2D&p,const std::string&s) {
#ifdef __EMSCRIPTEN__
  flushSaves();
  emitPath(contextId_, p, 2, s=="evenodd");
#else
  flatten(p, state_.matrix, matrixScale(state_.matrix), gFlat);
  auto mask=std::make_shared<ClipMask>();
  int x0,y0,x1,y1;
  // Narrowed by the clip already in force: a nested clip can only ever shrink
  // the region, so the outer one bounds the rasterisation of the inner.
  int cx0,cy0,cx1,cy1; drawBounds(cx0,cy0,cx1,cy1);
  if (!polyBounds(gFlat,cx0,cy0,cx1,cy1,x0,y0,x1,y1)) { mask->x0=mask->y0=mask->x1=mask->y1=0; state_.clip=mask; return; }
  if (x1<=x0||y1<=y0) { mask->x0=mask->y0=mask->x1=mask->y1=0; state_.clip=mask; return; }
  mask->x0=x0; mask->y0=y0; mask->x1=x1; mask->y1=y1;
  mask->alpha.assign(static_cast<size_t>(x1-x0)*(y1-y0),0);
  const bool evenOdd = s=="evenodd";
  std::uint8_t* const cells=mask->alpha.data();
  const int maskStride=x1-x0;
  const bool nested=state_.clip!=nullptr;
  scanFill(gFlat,evenOdd,x0,y0,x1,y1,[&](int x,int y,float cov){
    // scanFill only emits coverage in (0,1], so the clamp is only needed
    // once an outer mask has been multiplied in.
    const float v=nested ? std::clamp(cov*clipAt(x,y),0.f,1.f) : cov;
    cells[static_cast<size_t>(y-y0)*maskStride+(x-x0)]=static_cast<std::uint8_t>(std::lround(v*255));
  }, [&](int sx0,int sx1,int y){
    std::uint8_t* const row=cells+static_cast<size_t>(y-y0)*maskStride-x0;
    // Unnested, the interior of a clip is simply opaque -- which is what most
    // clips in this client are, being one rectangle over nothing.
    if (!nested) { std::memset(row+sx0,255,static_cast<size_t>(sx1-sx0)); return; }
    for (int x=sx0;x<sx1;++x)
      row[x]=static_cast<std::uint8_t>(std::lround(std::clamp(clipAt(x,y),0.f,1.f)*255));
  });
  state_.clip=mask;
#endif
}
bool Canvas::isPointInPath(float a,float b,const std::string&s)const {
#ifdef __EMSCRIPTEN__
  return c2d_hit(contextId_,a,b,0,s.c_str());
#else
  flatten(currentPath_, state_.matrix, matrixScale(state_.matrix), gFlat);
  const bool evenOdd = s=="evenodd";
  int winding=0, crossings=0, first=0;
  for (size_t c=0;c<gFlat.ends.size();++c) {
    const int last=gFlat.ends[c];
    for (int i=first;i<last;++i) {
      const int j=(i+1==last)?first:i+1;
      const float ax=gFlat.pts[2*i],ay=gFlat.pts[2*i+1],bx=gFlat.pts[2*j],by=gFlat.pts[2*j+1];
      if ((ay>b)==(by>b)) continue;
      if (a < ax+(b-ay)*(bx-ax)/(by-ay)) { ++crossings; winding += by>ay ? 1 : -1; }
    }
    first=last;
  }
  return evenOdd ? (crossings&1)!=0 : winding!=0;
#endif
}
bool Canvas::isPointInStroke(float a,float b)const {
#ifdef __EMSCRIPTEN__
  return c2d_hit(contextId_,a,b,1,"");
#else
  (void)a;(void)b; return false;
#endif
}
void Canvas::fillText(const std::string&s,float a,float b,float c){OP(47,c,a,b,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  glyphs(s,a,b,c,state_.fill);
#endif
}
void Canvas::strokeText(const std::string&s,float a,float b,float c){OP(48,c,a,b,0,0,0,0,0,s.c_str());
#ifndef __EMSCRIPTEN__
  glyphs(s,a,b,c,state_.stroke);
#endif
}
float Canvas::measureText(const std::string&s)const {
#ifdef __EMSCRIPTEN__
  return c2d_measure(contextId_,s.c_str());
#else
  if (const Font* font=uiFont(state_.fontFamily)) return font->measure(s,state_.fontSize);
  return static_cast<float>(s.size())*6.f*(state_.fontSize/8.f);
#endif
}
void Canvas::drawCanvas(const Canvas&s,float a,float b) {
#ifdef __EMSCRIPTEN__
  c2d_draw(contextId_,s.contextId_,a,b,0,0,0);
#else
  drawCanvas(s, a, b, static_cast<float>(s.width_), static_cast<float>(s.height_));
#endif
}
void Canvas::drawCanvas(const Canvas&s,float a,float b,float c,float d) {
#ifdef __EMSCRIPTEN__
  c2d_draw(contextId_,s.contextId_,a,b,c,d,1);
#else
  if (c <= 0 || d <= 0) return;
  const auto topLeft=mapPoint(a,b), bottomRight=mapPoint(a+c,b+d);
  const float dx0=std::min(topLeft.first,bottomRight.first), dx1=std::max(topLeft.first,bottomRight.first);
  const float dy0=std::min(topLeft.second,bottomRight.second), dy1=std::max(topLeft.second,bottomRight.second);
  for (int y = std::max(0, static_cast<int>(std::floor(dy0))); y < std::min(height_, static_cast<int>(std::ceil(dy1))); ++y)
    for (int x = std::max(0, static_cast<int>(std::floor(dx0))); x < std::min(width_, static_cast<int>(std::ceil(dx1))); ++x) {
      const int sx = std::clamp(static_cast<int>((x + 0.5f - dx0) * s.width_ / std::max(1e-3f, dx1-dx0)), 0, s.width_ - 1);
      const int sy = std::clamp(static_cast<int>((y + 0.5f - dy0) * s.height_ / std::max(1e-3f, dy1-dy0)), 0, s.height_ - 1);
      paint(x, y, s.pixels_[static_cast<size_t>(sy) * s.width_ + sx], state_.alpha * clipAt(x,y));
    }
#endif
}
void Canvas::drawImage(const std::uint8_t* rgba,int iw,int ih,float dx,float dy,float dw,float dh,float alpha) {
#ifdef __EMSCRIPTEN__
  c2d_image(contextId_,rgba,iw,ih,dx,dy,dw,dh,alpha);
#else
  if (!rgba || iw<=0 || ih<=0 || dw==0 || dh==0 || state_.alpha<=0 || alpha<=0) return;
  // One matrix from image pixels straight to device pixels: the image->box
  // scale folded into the current transform. Inverting THAT (rather than
  // mapping the box's corners forward) is what keeps a rotated image sampled
  // along its own axes.
  const auto& t=state_.matrix;
  const float ux=dw/iw, uy=dh/ih;
  const float ma=t[0]*ux, mb=t[1]*ux, mc=t[2]*uy, md=t[3]*uy;
  const float me=t[0]*dx+t[2]*dy+t[4], mf=t[1]*dx+t[3]*dy+t[5];
  const float det=ma*md-mb*mc;
  if (!(std::abs(det)>1e-12f)) return;
  const float ia=md/det, ib=-mb/det, ic=-mc/det, id=ma/det;
  const float ie=(mc*mf-md*me)/det, iff=(mb*me-ma*mf)/det;
  float lo=1e30f,hi=-1e30f,top=1e30f,bottom=-1e30f;
  const float cw=static_cast<float>(iw), ch=static_cast<float>(ih);
  const float corners[4][2]={{0,0},{cw,0},{cw,ch},{0,ch}};
  for (const auto& c : corners) {
    const float px=ma*c[0]+mc*c[1]+me, py=mb*c[0]+md*c[1]+mf;
    if (!(std::isfinite(px)&&std::isfinite(py))) return;
    lo=std::min(lo,px); hi=std::max(hi,px); top=std::min(top,py); bottom=std::max(bottom,py);
  }
  const int x0=std::max(0,static_cast<int>(std::floor(lo))), x1=std::min(width_,static_cast<int>(std::ceil(hi))+1);
  const int y0=std::max(0,static_cast<int>(std::floor(top))), y1=std::min(height_,static_cast<int>(std::ceil(bottom))+1);
  if (x0>=x1||y0>=y1) return;
  // Minification is where a point sample turns detailed artwork into noise, so
  // the subsample grid tracks how many image pixels land under one device
  // pixel; magnification needs none, and the bilinear fetch below carries it.
  const float stepX=std::hypot(ma,mb), stepY=std::hypot(mc,md);
  const float shrink=std::max(stepX>1e-6f?1.f/stepX:1.f, stepY>1e-6f?1.f/stepY:1.f);
  const int grid=std::clamp(static_cast<int>(std::ceil(shrink)),1,4);
  const float slice=1.f/grid;
  const int samples=grid*grid;
  for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
    const float clip=clipAt(x,y);
    if (clip<=0.f) continue;
    float sumR=0,sumG=0,sumB=0,sumA=0;
    for (int sy=0;sy<grid;++sy) for (int sx=0;sx<grid;++sx) {
      const float px=x+(sx+0.5f)*slice, py=y+(sy+0.5f)*slice;
      const float u=ia*px+ic*py+ie, v=ib*px+id*py+iff;
      if (u<0||v<0||u>=cw||v>=ch) continue;
      // Texel centres sit at +0.5, so the bilinear weights are measured from
      // there; sampling at u,v directly shifts the whole image half a pixel.
      const float fu=u-0.5f, fv=v-0.5f;
      const int bx=static_cast<int>(std::floor(fu)), by=static_cast<int>(std::floor(fv));
      const float tx=fu-bx, ty=fv-by;
      const int lx=std::clamp(bx,0,iw-1), rx=std::clamp(bx+1,0,iw-1);
      const int ty0=std::clamp(by,0,ih-1), ty1=std::clamp(by+1,0,ih-1);
      const std::uint8_t* p00=rgba+(static_cast<std::size_t>(ty0)*iw+lx)*4;
      const std::uint8_t* p10=rgba+(static_cast<std::size_t>(ty0)*iw+rx)*4;
      const std::uint8_t* p01=rgba+(static_cast<std::size_t>(ty1)*iw+lx)*4;
      const std::uint8_t* p11=rgba+(static_cast<std::size_t>(ty1)*iw+rx)*4;
      const float w00=(1-tx)*(1-ty), w10=tx*(1-ty), w01=(1-tx)*ty, w11=tx*ty;
      // Interpolated PREMULTIPLIED: blending straight colour across a
      // transparent texel drags its (undefined) rgb into the visible edge.
      const float a00=p00[3]*(1.f/255.f), a10=p10[3]*(1.f/255.f), a01=p01[3]*(1.f/255.f), a11=p11[3]*(1.f/255.f);
      sumR+=w00*p00[0]*a00+w10*p10[0]*a10+w01*p01[0]*a01+w11*p11[0]*a11;
      sumG+=w00*p00[1]*a00+w10*p10[1]*a10+w01*p01[1]*a01+w11*p11[1]*a11;
      sumB+=w00*p00[2]*a00+w10*p10[2]*a10+w01*p01[2]*a01+w11*p11[2]*a11;
      sumA+=w00*a00+w10*a10+w01*a01+w11*a11;
    }
    if (sumA<=1e-4f) continue;
    const Color color{static_cast<std::uint8_t>(std::clamp(std::lround(sumR/sumA),0L,255L)),
                      static_cast<std::uint8_t>(std::clamp(std::lround(sumG/sumA),0L,255L)),
                      static_cast<std::uint8_t>(std::clamp(std::lround(sumB/sumA),0L,255L)), 255};
    paint(x,y,color,(sumA/samples)*alpha*state_.alpha*clip);
  }
#endif
}
std::vector<std::uint8_t> Canvas::getImageData(int a,int b,int c,int d)const {
  std::vector<std::uint8_t>r(std::max(0,c)*std::max(0,d)*4);
#ifdef __EMSCRIPTEN__
  c2d_get_pixels(contextId_,a,b,c,d,r.data());
#else
  // Row at a time: Color is exactly the RGBA byte quartet this returns, so
  // the in-range part of a row is a straight copy. Per pixel, with the bounds
  // test inside the loop, this was a measurable slice of every frame -- the
  // window reads the whole surface back once to present it.
  const int sx0=std::max(0,-a), sx1=std::min(c,width_-a);
  for(int y=0;y<d;y++){
    const int sy=b+y;
    if(sy<0||sy>=height_||sx1<=sx0) continue;
    std::memcpy(&r[(size_t(y)*c+sx0)*4], &pixels_[size_t(sy)*width_+a+sx0], size_t(sx1-sx0)*4);
  }
#endif
  return r;
}
void Canvas::putImageData(const std::vector<std::uint8_t>&r,int a,int b,int c,int d) {
  if(r.size() < size_t(a)*b*4) return;
#ifdef __EMSCRIPTEN__
  c2d_put_pixels(contextId_,r.data(),a,b,c,d);
#else
  for(int y=0;y<b;y++)for(int x=0;x<a;x++){auto i=(size_t(y)*a+x)*4;blendPixel(c+x,d+y,Color{r[i],r[i+1],r[i+2],r[i+3]});}
#endif
}
void Canvas::fillCircle(float a,float b,float c){beginPath();arc(a,b,c,0,6.283185307f);fill();}void Canvas::strokeCircle(float a,float b,float c){beginPath();arc(a,b,c,0,6.283185307f);stroke();}
bool Canvas::savePPM(const std::string& path)const {
#ifdef __EMSCRIPTEN__
  (void)path; return true;
#else
  std::ofstream f(path,std::ios::binary); if(!f)return false;
  f<<"P6\n"<<width_<<" "<<height_<<"\n255\n";
  for(auto&p:pixels_){ const float a=p.a/255.f;
    f.put(static_cast<char>(std::lround(p.r*a+255*(1-a)))); f.put(static_cast<char>(std::lround(p.g*a+255*(1-a)))); f.put(static_cast<char>(std::lround(p.b*a+255*(1-a)))); }
  return !!f;
#endif
}

bool Canvas::showWindow(const std::string& title, const std::function<void(Canvas&, float)>& drawFrame) {
#ifdef __EMSCRIPTEN__
  (void)title; (void)drawFrame;
  return true;
#else
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
  SDL_Window* window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        width_, height_, SDL_WINDOW_SHOWN);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
  SDL_Texture* texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width_, height_) : nullptr;
  if (!texture) { if(renderer) SDL_DestroyRenderer(renderer); if(window) SDL_DestroyWindow(window); SDL_Quit(); return false; }
  std::vector<std::uint8_t> rgba(static_cast<size_t>(width_) * height_ * 4);
  const Uint64 start=SDL_GetPerformanceCounter(), frequency=SDL_GetPerformanceFrequency();
  bool running=true; while(running) { const Uint64 frameStart=SDL_GetPerformanceCounter(); SDL_Event event; while(SDL_PollEvent(&event)) if(event.type==SDL_QUIT) running=false;
    if (drawFrame) drawFrame(*this, static_cast<float>(SDL_GetPerformanceCounter()-start)/frequency);
    for (size_t i=0;i<pixels_.size();++i) {
      const Color& p=pixels_[i]; const float a=p.a/255.f;
      rgba[i*4]=static_cast<std::uint8_t>(p.r*a+255*(1-a)); rgba[i*4+1]=static_cast<std::uint8_t>(p.g*a+255*(1-a));
      rgba[i*4+2]=static_cast<std::uint8_t>(p.b*a+255*(1-a)); rgba[i*4+3]=255;
    }
    SDL_UpdateTexture(texture, nullptr, rgba.data(), width_ * 4);
    SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, nullptr, nullptr); SDL_RenderPresent(renderer);
    const Uint32 elapsed=static_cast<Uint32>((SDL_GetPerformanceCounter()-frameStart)*1000/frequency);
    if (elapsed < 16) SDL_Delay(16-elapsed);
  }
  SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return true;
#endif
}
#ifndef __EMSCRIPTEN__
std::pair<float,float> Canvas::mapPoint(float x,float y) const { const auto& m=state_.matrix; return {m[0]*x+m[2]*y+m[4], m[1]*x+m[3]*y+m[5]}; }
// The box any draw on this canvas may write: the surface, narrowed by the clip
// if there is one. Empty when the clip has already excluded everything.
void Canvas::drawBounds(int& x0,int& y0,int& x1,int& y1) const {
  x0=0; y0=0; x1=width_; y1=height_;
  if (const ClipMask* c=state_.clip.get()) {
    x0=std::max(x0,c->x0); y0=std::max(y0,c->y0);
    x1=std::min(x1,c->x1); y1=std::min(y1,c->y1);
  }
}
float Canvas::clipAt(int x,int y) const {
  const ClipMask* c=state_.clip.get(); if (!c) return 1.f;
  if (x<c->x0||x>=c->x1||y<c->y0||y>=c->y1) return 0.f;
  return c->alpha[static_cast<size_t>(y-c->y0)*(c->x1-c->x0)+(x-c->x0)]*(1.f/255.f);
}
void Canvas::paint(int x,int y,Color c,float coverage) {
  if(x<0||y<0||x>=width_||y>=height_) return;
  blend(pixels_[size_t(y)*width_+x], c, coverage);
}
void Canvas::blendPixel(int x,int y,Color c) { paint(x,y,c,1.f); }
namespace {
// The browser blends a SHAPE's coverage linearly -- a rect straddling a pixel
// at 0.7 coverage lands on 179, dead on the ladder -- but it does not blend a
// FILLED GLYPH's that way: its mask rasterizer pushes partial coverage up a
// ramp (a stroked glyph is a path, and stays linear -- see strokeDevice), so
// the edge of a white stem reaches full brightness instead of stopping at
// mid-grey. Rasterizing the bundled Ubuntu faces in Chrome and here, then
// pairing the two bitmaps pixel for pixel, puts that ramp at coverage^(1/3.2)
// (linear rasterizing is 21.8 rms off Chrome across four glyph sizes, 1/2.2 is
// 11.8 off, 1/3.2 is 10.1). Applying it to shapes too would fatten every
// petal, wall and button edge in the client away from the reference.
constexpr float kTextGamma = 3.2f;
inline float textCoverage(float cov, bool glyph) {
  if (!(glyph && cov > 0.f && cov < 1.f)) return cov;
  // cov^(1/3.2) is cov^(5/16), which is three multiplies and four square
  // roots -- all single instructions -- instead of a call into powf. The two
  // agree to a few ULP, orders below the 1/255 the result is quantised to,
  // and this runs once per partially-covered pixel of every glyph on screen.
  const float square=cov*cov, fifth=square*square*cov;
  return std::sqrt(std::sqrt(std::sqrt(std::sqrt(fifth))));
}
} // namespace
void Canvas::fillDevice(const Path2D& path, bool evenOdd, Color color) {
  if (color.a==0 || state_.alpha<=0 || path.segments().empty()) return;
  const bool glyph=path.glyphOutlines();
  flatten(path, state_.matrix, matrixScale(state_.matrix), gFlat);
  // Hoisted: scanFill guarantees x and y are inside the surface, so the
  // per-pixel path is a blend and nothing else -- no bounds test, no reload of
  // the alpha, and no shared_ptr dereference for a clip that usually is not set.
  Color* const surface=pixels_.data();
  const int stride=width_;
  const float alpha=state_.alpha;
  const bool clipped=state_.clip!=nullptr;
  int cx0,cy0,cx1,cy1; drawBounds(cx0,cy0,cx1,cy1);
  // textCoverage(1,..) is 1 whichever ramp applies, so an interior run's source
  // alpha is just the global alpha -- hoisted out of the run entirely.
  const float solid=alpha;
  const bool memsettable = !clipped && solid>=1.f && color.a==255;
  const Color opaque{color.r,color.g,color.b,255};
  scanFill(gFlat, evenOdd, cx0,cy0,cx1,cy1, [&](int x,int y,float cov){
    const float a=textCoverage(cov,glyph)*alpha;
    blend(surface[static_cast<size_t>(y)*stride+x], color, clipped ? a*clipAt(x,y) : a);
  }, [&](int x0,int x1,int y){
    Color* const row=surface+static_cast<size_t>(y)*stride;
    // An opaque colour at full coverage IS the destination -- blend's own
    // sa>=1 case writes exactly this -- so the run is a store, not a blend.
    if (memsettable) { std::fill(row+x0,row+x1,opaque); return; }
    if (!clipped) { for (int x=x0;x<x1;++x) blend(row[x],color,solid); return; }
    for (int x=x0;x<x1;++x) blend(row[x],color,solid*clipAt(x,y));
  });
}
void Canvas::strokeDevice(const Path2D& path) {
  if (state_.stroke.a==0 || state_.alpha<=0 || state_.lineWidth<=0 || path.segments().empty()) return;
  const float scale=matrixScale(state_.matrix);
  // No text gamma here, even for glyph outlines. The browser rasterizes a
  // FILLED glyph through its mask pipeline, where coverage goes up the ramp
  // kTextGamma models -- but a STROKED one is converted to a path and stroked
  // like any other shape, with plain linear coverage. Ramping it here made a
  // 12px outline rasterize as if it were several times wider: every
  // partial-coverage pixel along both edges of the outline jumped most of the
  // way to opaque, so the outline both spread outward and ate into the white
  // core the fill puts back on top. Measured against Chrome on the bundled
  // Ubuntu Bold -- 'o'/'H'/'S'/'e'/'A'/'n' at 12..40px and lineWidth 2..8 --
  // the ramp overshot total outline ink by 5% at lineWidth 8 and 28% at
  // lineWidth 2 (the excess tracks edge length, not area, which is what makes
  // it worse the thinner the line); dropping it lands every case within 1%
  // and halves per-pixel rms, 17.4 -> 7.0, level with a plain shape stroke's
  // 8.5 against the same reference.
  flatten(path, Matrix{1,0,0,1,0,0}, scale, gFlat);
  const Poly* source=&gFlat;
  if (!state_.dash.empty()) { applyDash(gFlat, state_.dash, state_.dashOffset, gDash); source=&gDash; }
  strokeOutline(*source, state_.lineWidth/2, state_.lineCap, state_.lineJoin, state_.miterLimit, scale, gOutline);
  transformPoly(gOutline, state_.matrix, gDevice);
  const Color color=state_.stroke;
  Color* const surface=pixels_.data();
  const int stride=width_;
  const float alpha=state_.alpha;
  const bool clipped=state_.clip!=nullptr;
  int cx0,cy0,cx1,cy1; drawBounds(cx0,cy0,cx1,cy1);
  const bool memsettable = !clipped && alpha>=1.f && color.a==255;
  const Color opaque{color.r,color.g,color.b,255};
  scanFill(gDevice, false, cx0,cy0,cx1,cy1, [&](int x,int y,float cov){
    const float a=cov*alpha;
    blend(surface[static_cast<size_t>(y)*stride+x], color, clipped ? a*clipAt(x,y) : a);
  }, [&](int x0,int x1,int y){
    Color* const row=surface+static_cast<size_t>(y)*stride;
    if (memsettable) { std::fill(row+x0,row+x1,opaque); return; }
    if (!clipped) { for (int x=x0;x<x1;++x) blend(row[x],color,alpha); return; }
    for (int x=x0;x<x1;++x) blend(row[x],color,alpha*clipAt(x,y));
  });
}
// The system face for each generic family, loaded once on first use. A missing
// face falls back to the 5x7 bitmap below, which has no descenders.
static const Font* uiFont(unsigned char family) {
  struct Face { Font font; bool tried=false; };
  static Face faces[3];
  if (family>2) family=0;
  Face& face=faces[family];
  if (!face.tried) {
    face.tried=true;
    static const std::vector<std::string> kPreferred[3]={
      {"Helvetica","Arial","Verdana","DejaVuSans","LiberationSans-Regular"},
      {"Georgia","Times New Roman","Times","DejaVuSerif","LiberationSerif-Regular"},
      {"Menlo","Courier New","DejaVuSansMono","LiberationMono-Regular"},
    };
    std::string path;
    if (Font::findSystemFont(kPreferred[family],path)) face.font.loadFromFile(path);
  }
  return face.font.valid() ? &face.font : nullptr;
}

void Canvas::glyphs(const std::string& text, float x, float y, float maxWidth, Color color) {
  if (text.empty() || color.a==0) return;
  if (const Font* font=uiFont(state_.fontFamily)) {
    const float size=state_.fontSize, width=font->measure(text,size);
    const float squeeze=(maxWidth>0 && width>maxWidth && width>0) ? maxWidth/width : 1.f;
    const float ascent=font->ascent(size), descent=font->descent(size);
    const float penX=x-(state_.textAlign==1 ? width*squeeze/2 : state_.textAlign==2 ? width*squeeze : 0.f);
    const float baseline=y+(state_.textBaseline==0 ? ascent : state_.textBaseline==1 ? (ascent+descent)/2
                          : state_.textBaseline==2 ? descent : 0.f);
    Path2D shaped;
    font->appendText(shaped,text,penX,baseline,size);
    if (squeeze!=1.f)
      for (Path2D::Segment& s : shaped.segments()) {
        const int pairs = s.command==Path2D::Command::Move||s.command==Path2D::Command::Line ? 1
                        : s.command==Path2D::Command::Quadratic ? 2
                        : s.command==Path2D::Command::Bezier ? 3 : 0;
        for (int k=0;k<pairs;++k) s.v[k*2]=penX+(s.v[k*2]-penX)*squeeze;
      }
    fillDevice(shaped,false,color);
    return;
  }
  const float unit=state_.fontSize/8.f;
  float advance=6*unit, height=7*unit;
  const float full=static_cast<float>(text.size())*advance;
  float squeeze=1.f;
  if (maxWidth>0 && full>maxWidth) squeeze=maxWidth/full;
  float penX = x - (state_.textAlign==1 ? full*squeeze/2 : state_.textAlign==2 ? full*squeeze : 0.f);
  const float penY = y - (state_.textBaseline==0 ? 0.f : state_.textBaseline==1 ? height/2 : height);
  Path2D glyph;
  for (unsigned char ch : text) {
    if (ch>=32 && ch<127) {
      const unsigned char* columns=kFont[ch-32];
      for (int c=0;c<5;++c) for (int r=0;r<7;++r)
        if (columns[c]>>r & 1) glyph.rect(penX+c*unit*squeeze, penY+r*unit, unit*squeeze+0.01f, unit+0.01f);
    }
    penX += advance*squeeze;
  }
  fillDevice(glyph, false, color);
}
#endif
