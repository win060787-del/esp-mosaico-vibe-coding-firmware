// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_pet.h"
#include "gsp_sim_bridge.h"

static struct { app_pet_handle_t controller; int last; } backend;
/* This is a simulator data source, not a replacement UI. A state-file edit drives
 * the shared C controller's real transitions, timers and touch handlers. */
static bool sample(void *context, app_pet_snapshot_t *out)
{
    (void)context;
    int state = PET_WORKING;
    const char *path = getenv("PET_SIM_STATE_FILE");
    if (path) {
        FILE *f = fopen(path, "r");
        if (!f) return false;
        int n = fscanf(f, "%d", &state); fclose(f);
        if (n != 1 || state < 0 || state > 7) return false;
    }
    if (state == backend.last) return false;
    backend.last = state;
    memset(out, 0, sizeof(*out));
    out->state = state==7?PET_IDLE:state==6?PET_WORKING:(app_pet_state_t)state; out->source_ok = state != PET_OFFLINE;
    out->generated_at = 1793547730; out->reset_at = out->generated_at + 226398;
    out->server_time = out->generated_at;
    out->task_states[0]=PET_WORKING;out->task_states[1]=state==6?PET_IDLE:PET_COMPLETED;
    out->task_unread[1]=state!=6;
    out->task_states[2]=PET_WORKING;out->task_states[3]=PET_WORKING;
    out->remaining = 42; out->active = state == PET_WORKING || state == PET_WAITING || state==6 ? 3 : 0;
    snprintf(out->quota_label, sizeof(out->quota_label), "本周剩余");
    snprintf(out->tasks[0], PET_TITLE_BYTES, "开发 Codex 桌宠");
    snprintf(out->tasks[1], PET_TITLE_BYTES, "开发应用接口");
    snprintf(out->tasks[2], PET_TITLE_BYTES, "手表转录");
    snprintf(out->tasks[3], PET_TITLE_BYTES, "公司官网");
    if(state==6)out->tasks[1][0]=0;
    out->done_at = state==7?0:out->generated_at - 90;
    snprintf(out->recent_title,PET_TITLE_BYTES,"执行中 · 更新桌宠界面");
    snprintf(out->done_title, PET_TITLE_BYTES, "官网更新");
    return true;
}
static int battery_sample(void *ctx) {
    (void)ctx;
    const char *path=getenv("PET_SIM_BATTERY_FILE");
    if(!path)return 82;
    FILE *f=fopen(path,"r");
    if(!f)return -1;
    int value=-1;int count=fscanf(f,"%d",&value);fclose(f);
    return count==1?value:-1;
}
static app_pet_preferences_t prefs={.volume=50,.brightness=100,.vibration=true,.sound=true,.wireless_ready=true};
static bool preferences(void *ctx,app_pet_preferences_t *out) { (void)ctx;*out=prefs;return true; }
static bool action(void *ctx,app_pet_action_t action,unsigned value) {
    (void)ctx;
    static unsigned previews;
    if(action==PET_SET_VOLUME)prefs.volume=value;
    else if(action==PET_SET_BRIGHTNESS)prefs.brightness=value;
    else if(action==PET_TOGGLE_VIBRATION)prefs.vibration=!prefs.vibration;
    else if(action==PET_PREVIEW)++previews;
    else if(action==PET_SET_SOUND)prefs.sound=value!=0;
    else if(action==PET_SET_COLOR)prefs.color=value;
    const char *path=getenv("PET_SIM_PREFS_FILE");
    if(path){FILE *f=fopen(path,"w");if(f){fprintf(f,"%u %u %u %u %u %u",prefs.volume,prefs.brightness,prefs.vibration,previews,prefs.sound,prefs.color);fclose(f);}}
    return true;
}
static app_pet_management_t netview={.generation=1,.terminal_total=2,.wifi_total=2,.terminal_ids={"11111111111111111111111111111111","22222222222222222222222222222222"},.terminal_names={"Mac mini","MacBook Pro"},.terminal_online={true,true},.terminal_active={2,1},.selected_terminal="11111111111111111111111111111111",.wifi_ssids={"Home","Office"},.connected_ssid="Home"};
static bool management(void *ctx,app_pet_management_t *out){(void)ctx;*out=netview;return true;}
static bool manage_action(void *ctx,app_pet_management_action_t action,unsigned value){
    (void)ctx;
    if(action==PET_MANAGE_SCAN){netview.scan_count=1;strcpy(netview.scan_ssids[0],"Test Wi-Fi");}
    if(action==PET_MANAGE_MODE)netview.mode=value;
    if(action==PET_MANAGE_TERMINAL && value<2){netview.mode=1;snprintf(netview.selected_terminal,sizeof(netview.selected_terminal),"%s",netview.terminal_ids[value]);}
    if(action==PET_MANAGE_ENROLL)snprintf(netview.enrollment_code,sizeof(netview.enrollment_code),"ABCDE-12345");
    if(action==PET_MANAGE_WIFI_ADD){netview.portal_active=true;strcpy(netview.portal_ssid,"Codex-Pet-demo");strcpy(netview.portal_password,"DEMO12345678");}
    if(action==PET_MANAGE_PORTAL_STOP)netview.portal_active=false;
    if(action==PET_MANAGE_WIFI_DELETE && value<netview.wifi_total){netview.wifi_ssids[value][0]=0;--netview.wifi_total;}
    ++netview.generation;
    const char *path=getenv("PET_SIM_NET_FILE");if(path){FILE *f=fopen(path,"w");if(f){fprintf(f,"%u %u %u %u",action,value,netview.mode,netview.wifi_total);fclose(f);}}
    return true;
}
static bool join_wifi(void *ctx,const char *ssid,const char *password){
    (void)ctx;
    bool success=!strcmp(ssid,"Test Wi-Fi")&&!strcmp(password,"aA!12345");
    netview.join_result=success?2:3;
    ++netview.join_revision;
    ++netview.generation;
    const char *path=getenv("PET_SIM_JOIN_FILE");
    if(path){
        FILE *f=fopen(path,"w");
        if(f){fprintf(f,"join %s",success?"success":"failure");fclose(f);}
    }
    return true;
}
static app_pet_voice_t voice={.connected=true,.name="Mac mini",.state="idle"};
static bool voice_poll(void *ctx,app_pet_voice_t *out){(void)ctx;*out=voice;return true;}
static bool voice_action(void *ctx,const char *action){
    (void)ctx;
    if(!strcmp(action,"start")){voice.recording=true;voice.level=60;strcpy(voice.state,"recording");strcpy(voice.text,"帮我检查当前项目的任务进展，并整理今天完成的工作。文字正在同步到 Mac 输入框。");}
    if(!strcmp(action,"stop")){voice.recording=false;strcpy(voice.state,"ready");}
    if(!strcmp(action,"clear")){voice.recording=false;voice.text[0]=0;}
    if(!strcmp(action,"delete")){voice.recording=false;size_t n=strlen(voice.text);if(n){--n;while(n && ((unsigned char)voice.text[n]&0xc0)==0x80)--n;voice.text[n]=0;}}
    if(!strcmp(action,"send")){voice.recording=false;voice.text[0]=0;strcpy(voice.state,"sent");}
    const char *path=getenv("PET_SIM_VOICE_FILE");if(path){FILE *f=fopen(path,"a");if(f){fprintf(f,"%s\n",action);fclose(f);}}
    ++voice.revision;return true;
}
esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    backend.last = -1;
    esp_gsp_err_t err=app_pet_create(ui, sample, NULL, &backend.controller);
    if(err==ESP_GSP_OK){app_pet_set_battery_source(backend.controller,battery_sample,NULL);app_pet_set_controls(backend.controller,preferences,action,NULL);app_pet_set_management(backend.controller,management,manage_action,NULL);app_pet_set_join(backend.controller,join_wifi);app_pet_set_voice(backend.controller,voice_poll,voice_action,NULL);}
    return err;
}
void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    (void)ui;
    esp_gsp_err_t e = app_pet_destroy(&backend.controller);
    if (e != ESP_GSP_OK) fprintf(stderr, "pet cleanup failed: %d\n", (int)e);
}
