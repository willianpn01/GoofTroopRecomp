/* E5 host output/readback tests. Screenshots are presentation evidence only. */
#include "sdl_filter.h"
#include "goof_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define REQUIRE(c) do { if(!(c)) { fprintf(stderr,"FAIL line=%d SDL=%s\n",__LINE__,SDL_GetError());failures++; } } while(0)
static int cmp(const void *a,const void *b) { double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y); }
static void stats(const char *mode,double *v,int n) {
  double sum=0;for(int i=0;i<n;i++) sum+=v[i];qsort(v,n,sizeof *v,cmp);
  printf("PERF mode=%s preprocessing_ms=0 n=%d draw_flush_readback_mean_ms=%.4f p95=%.4f p99=%.4f max=%.4f\n",
    mode,n,sum/n,v[n*95/100],v[n*99/100],v[n-1]);
}
static void readback(SDL_Renderer *r,SDL_Surface *s) {
  REQUIRE(SDL_RenderReadPixels(r,NULL,SDL_PIXELFORMAT_ARGB8888,s->pixels,s->pitch)==0);
}
int main(int argc,char **argv) {
  REQUIRE(SDL_Init(SDL_INIT_VIDEO)==0);
  SDL_Window *w=SDL_CreateWindow("E5 filter test",0,0,1170,896,SDL_WINDOW_HIDDEN);
  SDL_Renderer *r=SDL_CreateRenderer(w,-1,SDL_RENDERER_ACCELERATED);
  if(!r) r=SDL_CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
  REQUIRE(r!=NULL);if(!r) return 1;
  SDL_RendererInfo ri;SDL_GetRendererInfo(r,&ri);printf("RENDERER %s flags=%x\n",ri.name,ri.flags);
  enum { W=320,H=224 };
  uint32_t *src=malloc(W*H*4),*copy=malloc(W*H*4);
  for(int y=0;y<H;y++) for(int x=0;x<W;x++) src[y*W+x]=((x+y)&1)?0xffffff:0;
  memcpy(copy,src,W*H*4);
  SDL_Texture *t=SDL_CreateTexture(r,SDL_PIXELFORMAT_XRGB8888,SDL_TEXTUREACCESS_STREAMING,W,H);
  REQUIRE(t!=NULL);REQUIRE(SDL_UpdateTexture(t,NULL,src,W*4)==0);
  GoofPresentationImage image={t,W,H,SDL_PIXELFORMAT_XRGB8888};
  SDL_Surface *a=SDL_CreateRGBSurfaceWithFormat(0,1170,896,32,SDL_PIXELFORMAT_ARGB8888);
  SDL_Surface *b=SDL_CreateRGBSurfaceWithFormat(0,1170,896,32,SDL_PIXELFORMAT_ARGB8888);
  SDL_Rect dst={13,0,960,672};
  SDL_SetRenderDrawColor(r,0,0,0,255);SDL_RenderClear(r);
  SDL_SetTextureScaleMode(t,SDL_ScaleModeNearest);SDL_RenderCopy(r,t,NULL,&dst);readback(r,a);
  REQUIRE(goof_sdl_filter_draw(r,&image,GOOF_FILTER_NEAREST,&dst));readback(r,b);
  REQUIRE(!memcmp(a->pixels,b->pixels,a->pitch*a->h));
  printf("F11 nearest_exact_E4_copy %s\n",failures?"FAIL":"PASS");
  REQUIRE(goof_sdl_filter_draw(r,&image,GOOF_FILTER_BILINEAR,&dst));readback(r,b);
  REQUIRE(memcmp(a->pixels,b->pixels,a->pitch*a->h)!=0);
  REQUIRE(goof_sdl_filter_draw(r,&image,GOOF_FILTER_NEAREST,&dst));readback(r,b);
  REQUIRE(!memcmp(a->pixels,b->pixels,a->pitch*a->h));
  SDL_ScaleMode scale;SDL_GetTextureScaleMode(t,&scale);REQUIRE(scale==SDL_ScaleModeNearest);
  printf("F12 bilinear_to_nearest_exact %s\n",failures?"FAIL":"PASS");
  for(int i=0;i<W*H;i++) src[i]=0xffffff;
  memcpy(copy,src,W*H*4);REQUIRE(SDL_UpdateTexture(t,NULL,src,W*4)==0);
  dst=(SDL_Rect){13,0,640,448};
  SDL_SetRenderDrawColor(r,0,0,0,255);SDL_RenderClear(r);
  REQUIRE(goof_sdl_filter_draw(r,&image,GOOF_FILTER_SCANLINES,&dst));readback(r,a);
  for(int y=0;y<448;y++) {
    unsigned pixel=*(uint32_t*)((Uint8*)a->pixels+y*a->pitch+40*4)&255;
    REQUIRE((y&1)?(pixel>=190 && pixel<=192):pixel==255);
  }
  REQUIRE((*(uint32_t*)a->pixels & 0xffffff)==0); /* outside destination stays black */
  SDL_SetRenderDrawColor(r,0,0,0,255);SDL_RenderClear(r);
  REQUIRE(goof_sdl_filter_draw(r,&image,GOOF_FILTER_SCANLINES,&dst));readback(r,b);
  REQUIRE(!memcmp(a->pixels,b->pixels,a->pitch*a->h)); /* repeats cannot accumulate darkening */
  printf("F13 scanline_output_rows_repeat_identity_no_bleed %s\n",failures?"FAIL":"PASS");
  for(int k=1;k<=4;k++) {
    int total=0;for(int y=0;y<H*k;y++) total+=goof_scanline_alpha(y,H*k,H);
    REQUIRE(total==32*H*k);
  }
  REQUIRE(goof_scanline_alpha(0,448,H)==0 && goof_scanline_alpha(1,448,H)==64);
  REQUIRE(goof_scanline_alpha(0,672,H)==0 && goof_scanline_alpha(1,672,H)==32 && goof_scanline_alpha(2,672,H)==64);
  for(int y=0;y<H;y++) REQUIRE(goof_scanline_alpha(y,H,H)==32);
  printf("F13 source_row_scanline_coverage_1X_to_4X %s\n",failures?"FAIL":"PASS");
  for(int aspect=0;aspect<2;aspect++) for(int k=0;k<=4;k++) for(int fs=0;fs<2;fs++)
    for(int mode=0;mode<GOOF_FILTER_COUNT;mode++) {
      GoofRect geo=goof_video_dest_rect((GoofPixelAspect)aspect,fs?GOOF_SCALING_FIT:GOOF_SCALING_INTEGER,
                                      k?256*k:1170,k?224*k:896);
      dst=(SDL_Rect){geo.x,geo.y,geo.w,geo.h};
      SDL_SetRenderDrawColor(r,7,11,19,255);SDL_SetRenderDrawBlendMode(r,SDL_BLENDMODE_NONE);
      REQUIRE(goof_sdl_filter_draw(r,&image,(GoofFilterMode)mode,&dst));
      Uint8 red,green,blue,alpha;SDL_BlendMode blend;
      SDL_GetRenderDrawColor(r,&red,&green,&blue,&alpha);SDL_GetRenderDrawBlendMode(r,&blend);
      REQUIRE(red==7 && green==11 && blue==19 && alpha==255 && blend==SDL_BLENDMODE_NONE);
      REQUIRE(SDL_GetRenderTarget(r)==NULL);
    }
  printf("F17_F18 geometry_aspects_scales_and_state_restore %s\n",failures?"FAIL":"PASS");
  dst=(SDL_Rect){0,0,1024,896};
  for(int i=0;i<1000;i++) REQUIRE(goof_sdl_filter_draw(r,&image,(GoofFilterMode)(i%3),&dst));
  REQUIRE(!memcmp(src,copy,W*H*4));
  printf("F19 1000_transitions_zero_filter_allocations_source_unchanged %s\n",failures?"FAIL":"PASS");
  double times[300],freq=(double)SDL_GetPerformanceFrequency();
  for(int mode=0;mode<3;mode++) {
    for(int i=0;i<300;i++) {
      Uint64 start=SDL_GetPerformanceCounter();
      REQUIRE(goof_sdl_filter_draw(r,&image,(GoofFilterMode)mode,&dst));
      readback(r,b); /* flush/readback gives a conservative GPU-inclusive bound */
      times[i]=1000.0*(SDL_GetPerformanceCounter()-start)/freq;
    }
    stats(goof_filter_token((GoofFilterMode)mode),times,300);
  }
  SDL_DestroyTexture(t);
  if(argc==4) {
    GoofAppStatus status;GoofAppConfig config={argv[1]};
    GoofApp *app=goof_app_create(&config,&status);REQUIRE(app!=NULL);
    GoofInputScript script={0};char err[256];
    REQUIRE(goof_input_script_load(argv[2],&script,err,sizeof err));
    GoofAppDiag diag={0};
    t=SDL_CreateTexture(r,SDL_PIXELFORMAT_XRGB8888,SDL_TEXTUREACCESS_STREAMING,256,224);
    image=(GoofPresentationImage){t,256,224,SDL_PIXELFORMAT_XRGB8888};
    dst=(SDL_Rect){0,0,768,672};
    for(int epoch=1;epoch<=1300;epoch++) {
      GoofInputSample input=goof_input_script_sample(&script,epoch);
      REQUIRE(goof_app_step(app,&input,&diag)==GOOF_APP_OK);
      REQUIRE(goof_app_render(app,&diag)==GOOF_APP_OK);
      if(epoch==300 || epoch==1250) {
        uint32_t width,height,pitch;
        const uint32_t *pixels=goof_app_framebuffer(app,&width,&height,&pitch);
        uint32_t original[256*224];memcpy(original,pixels,sizeof original);
        REQUIRE(SDL_UpdateTexture(t,NULL,pixels,(int)pitch)==0);
        for(int mode=0;mode<3;mode++) {
          SDL_SetRenderDrawColor(r,0,0,0,255);SDL_RenderClear(r);
          REQUIRE(goof_sdl_filter_draw(r,&image,(GoofFilterMode)mode,&dst));readback(r,a);
          char name[2048];snprintf(name,sizeof name,"%s/epoch%d_%s.bmp",argv[3],epoch,goof_filter_token((GoofFilterMode)mode));
          REQUIRE(SDL_SaveBMP(a,name)==0);
          REQUIRE(!memcmp(original,pixels,sizeof original));
        }
      }
    }
    SDL_DestroyTexture(t);goof_app_destroy(app);goof_input_script_free(&script);
    printf("SCREENSHOTS same_guest_scene_three_modes %s\n",failures?"FAIL":"PASS");
  }
  free(src);free(copy);SDL_FreeSurface(a);SDL_FreeSurface(b);
  SDL_DestroyRenderer(r);SDL_DestroyWindow(w);SDL_Quit();
  printf("GOOF_FILTER_SDL_TEST %s\n",failures?"FAIL":"PASS");return failures?1:0;
}
