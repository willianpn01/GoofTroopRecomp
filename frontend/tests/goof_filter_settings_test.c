#include "host/goof_config.h"
#include "host/ui/goof_menu.h"
#include <stdio.h>
#include <string.h>

static int failures, warnings;
#define CHECK(id, cond) do { bool ok=(cond); printf("F%d %s\n",id,ok?"PASS":"FAIL"); if(!ok) failures++; } while(0)
static void warning(void *u,const char *s) { (void)u;(void)s;warnings++; }
static GoofConfig stored;
static GoofVideoSettings live, shown;
static GoofMenu menu;
static char file[65536];
static bool preview(void *u,const GoofVideoSettings *v) { (void)u;shown=*v;return true; }
static GoofSaveResult save(void *u,const GoofVideoSettings *v,char *msg,size_t cap) {
  (void)u;live=stored.video=*v;goof_config_serialize(&stored,file,sizeof file);
  snprintf(msg,cap,"SAVED");return GOOF_SAVE_PERSISTED;
}
static void key(int code) {
  GoofUiEvent down={GOOF_UI_KEY_DOWN,(uint16_t)code,false,-1};
  GoofUiEvent up={GOOF_UI_KEY_UP,(uint16_t)code,false,-1};
  goof_menu_handle(&menu,&down);goof_menu_handle(&menu,&up);
}
static void select(int item) {
  for(int i=0;i<20 && menu.sel[GOOF_PAGE_VIDEO]!=item;i++) key(GOOF_UI_SC_DOWN);
}
int main(void) {
  GoofConfig c,d;
  goof_config_defaults(&stored);live=shown=stored.video;
  CHECK(1,live.filter==GOOF_FILTER_NEAREST);
  const char *old="[meta]\nschema=1\n[video]\nvsync=on\nwindow_scale=2\n";
  goof_config_parse(old,strlen(old),&c,warning,NULL);
  CHECK(2,c.video.filter==GOOF_FILTER_NEAREST && c.video.vsync && c.video.window_scale==2 && !warnings);
  bool roundtrip=true;
  for(int i=0;i<GOOF_FILTER_COUNT;i++) {
    c.video.filter=(GoofFilterMode)i;
    goof_config_serialize(&c,file,sizeof file);
    goof_config_parse(file,strlen(file),&d,warning,NULL);
    roundtrip &= goof_video_equal(&c.video,&d.video);
  }
  CHECK(3,roundtrip && !warnings);
  const char *invalid="[meta]\nschema=1\n[video]\nfilter=xbr\n";
  goof_config_parse(invalid,strlen(invalid),&c,warning,NULL);
  CHECK(4,c.video.filter==GOOF_FILTER_NEAREST && warnings==1);
  goof_menu_init(&menu,&stored.bindings,(GoofSettingsHost){0});
  goof_menu_attach_host_pages(&menu,(GoofHostPages){.video=&live,.audio=&stored.audio,
    .preview_video=preview,.save_video=save});
  key(GOOF_UI_SC_F2);key(GOOF_UI_SC_DOWN);key(GOOF_UI_SC_RETURN);
  select(5);key(GOOF_UI_SC_RIGHT);
  CHECK(5,shown.filter==GOOF_FILTER_BILINEAR && live.filter==GOOF_FILTER_NEAREST);
  select(8);key(GOOF_UI_SC_RETURN);
  CHECK(6,shown.filter==GOOF_FILTER_NEAREST && live.filter==GOOF_FILTER_NEAREST);
  select(5);key(GOOF_UI_SC_LEFT);select(7);key(GOOF_UI_SC_RETURN);
  goof_config_parse(file,strlen(file),&d,NULL,NULL);
  CHECK(7,d.video.filter==GOOF_FILTER_SCANLINES && live.filter==GOOF_FILTER_SCANLINES);
  GoofBindings bindings=stored.bindings;
  GoofAudioSettings audio=stored.audio;
  select(6);key(GOOF_UI_SC_RETURN);key(GOOF_UI_SC_LEFT);key(GOOF_UI_SC_RETURN);
  CHECK(8,shown.filter==GOOF_FILTER_NEAREST && live.filter==GOOF_FILTER_SCANLINES);
  select(7);key(GOOF_UI_SC_RETURN);
  CHECK(9,live.filter==GOOF_FILTER_NEAREST && !memcmp(&stored.bindings,&bindings,sizeof bindings)
    && goof_audio_equal(&stored.audio,&audio));
  select(5);key(GOOF_UI_SC_RIGHT);key(GOOF_UI_SC_F2);
  CHECK(10,shown.filter==live.filter && !menu.open);
  c=stored;c.video.filter=(GoofFilterMode)999;goof_video_sanitize(&c.video);
  CHECK(11,c.video.filter==GOOF_FILTER_NEAREST);
  for(int i=0;i<GOOF_FILTER_COUNT;i++) {
    GoofFilterMode mode=GOOF_FILTER_NEAREST;
    CHECK(12+i,goof_filter_from_token(goof_filter_token((GoofFilterMode)i),&mode) && mode==(GoofFilterMode)i);
  }
  printf("GOOF_FILTER_SETTINGS_TEST %s\n",failures?"FAIL":"PASS");
  return failures?1:0;
}
