// SPDX-License-Identifier: Apache-2.0
#include "app_pet.h"
#include "bundle_gsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS 167U
#define STALE_TICKS (90000U / TICK_MS)
#define DONE_SECONDS 600U
struct app_pet {
    esp_gsp_handle_t ui;
    app_pet_source_t source;
    void *source_ctx;
    app_pet_battery_source_t battery_source;
    void *battery_ctx;
    void *timer;
    app_pet_snapshot_t data;
    unsigned ticks;
    app_pet_voice_source_t voice_source;app_pet_voice_action_t voice_action;void *voice_ctx;
    app_pet_voice_t voice;unsigned voice_offset,voice_revision;bool voice_long;
    unsigned received_tick;
    unsigned last_draw_tick;
    unsigned last_spin_tick;
    int shown_icons[PET_TASKS];
    bool received;
    bool details;
    bool settings;
    app_pet_join_handler_t join;
    unsigned scan_page;
    bool edit_ssid, reveal, joining;
    unsigned join_revision;
    char join_ssid[33], input[65];
    unsigned key_mode;
    unsigned page, wifi_index, management_generation;
    char wifi_name[33];
    app_pet_management_t management;
    app_pet_management_source_t management_source;
    app_pet_management_handler_t management_action;
    void *management_ctx;
    bool settings_dirty;
    app_pet_preferences_source_t preferences_source;
    app_pet_action_handler_t action_handler;
    void *controls_ctx;
    app_pet_preferences_t preferences;
    int shown_eyes;
    int shown_color;
    esp_gsp_err_t last_error;
};
static void check(struct app_pet *p, esp_gsp_err_t e)
{
    if (e != ESP_GSP_OK) p->last_error = e;
}
static void eyes(struct app_pet *p, int which)
{
    if (p->shown_eyes == which) return;
    esp_gsp_err_t (*const setters[])(esp_gsp_handle_t, bool) = {
        gsp_pet_open_set_visible, gsp_pet_blink_set_visible,
        gsp_pet_waiting_set_visible, gsp_pet_idle_set_visible,
        gsp_pet_happy_set_visible, gsp_pet_failed_set_visible,
    };
    for (unsigned i = 0; i < 6; ++i) check(p, setters[i](p->ui, (int)i == which));
    p->shown_eyes = which;
}
static void remaining_time(char *out, size_t size, uint32_t now, uint32_t reset)
{
    uint32_t seconds = reset > now ? reset - now : 0;
    if (!reset) snprintf(out, size, "--");
    else if (!seconds) snprintf(out, size, "刷新中");
    else if (seconds >= 86400) snprintf(out, size, "%u天%u时", (unsigned)(seconds / 86400), (unsigned)((seconds % 86400) / 3600));
    else if (seconds >= 3600) snprintf(out, size, "%u时%u分", (unsigned)(seconds / 3600), (unsigned)((seconds % 3600) / 60));
    else snprintf(out, size, "%u分钟", (unsigned)((seconds + 59) / 60));
}
static bool online(const struct app_pet *p)
{
    return p->received && p->ticks - p->received_tick < STALE_TICKS && p->data.source_ok;
}
static void task_icon(struct app_pet *p, unsigned index, bool visible)
{
    esp_gsp_err_t (*const setters[PET_TASKS][3])(esp_gsp_handle_t,bool) = {
        {gsp_pet_task0_run_set_visible, gsp_pet_task0_unread_set_visible, gsp_pet_task0_unknown_set_visible},
        {gsp_pet_task1_run_set_visible, gsp_pet_task1_unread_set_visible, gsp_pet_task1_unknown_set_visible},
        {gsp_pet_task2_run_set_visible, gsp_pet_task2_unread_set_visible, gsp_pet_task2_unknown_set_visible},
        {gsp_pet_task3_run_set_visible, gsp_pet_task3_unread_set_visible, gsp_pet_task3_unknown_set_visible},
    };
    bool running=p->data.task_states[index]==PET_WORKING || p->data.task_states[index]==PET_WAITING;
    unsigned selected=!visible?3:running?0:p->data.task_states[index]==PET_COMPLETED && p->data.task_unread[index]?1:2;
    if(p->shown_icons[index]==(int)selected)return;
    p->shown_icons[index]=(int)selected;
    for(unsigned i=0;i<3;++i) check(p,setters[index][i](p->ui,visible && i==selected));
}
static void render(struct app_pet *p)
{
    char value[64];
    check(p, gsp_pet_brand_set_text(p->ui, "CODEX"));
    check(p,gsp_pet_mg_source_set_text(p->ui,p->data.source_name));
    const bool linked = online(p);
    const uint32_t elapsed=(uint32_t)((uint64_t)(p->ticks-p->received_tick)*TICK_MS/1000U);
    const uint32_t now=(p->data.server_time?p->data.server_time:p->data.generated_at)+elapsed;
    if(p->data.server_time){
        uint32_t local=(p->data.server_time+elapsed+8U*3600U)%86400U;
        snprintf(value,sizeof(value),"%02u:%02u",(unsigned)(local/3600),(unsigned)((local%3600)/60));
    } else snprintf(value,sizeof(value),"--:--");
    check(p,gsp_pet_clock_set_text(p->ui,value));
    const bool done = linked && p->data.done_at && now >= p->data.done_at && now - p->data.done_at < DONE_SECONDS;
    const app_pet_state_t state = linked ? p->data.state : PET_OFFLINE;
    static const char *const status[] = {"等待连接", "空闲中", "work ...", "等待你确认", "任务已完成", "任务失败"};
    check(p, gsp_pet_status_set_text(p->ui, status[state]));
    check(p,gsp_pet_link_online_set_visible(p->ui,linked));
    check(p,gsp_pet_link_offline_set_visible(p->ui,!linked));
    const int battery=p->battery_source?p->battery_source(p->battery_ctx):-1;
    const bool valid=battery>=0 && battery<=100;
    if(valid)snprintf(value,sizeof(value),"%d%%",battery);
    else snprintf(value,sizeof(value),"--%%");
    check(p,gsp_pet_battery_set_text(p->ui,value));
    check(p,gsp_pet_batt_unknown_set_visible(p->ui,!valid));
    check(p,gsp_pet_batt_green_set_visible(p->ui,valid && battery>30));
    check(p,gsp_pet_batt_yellow_set_visible(p->ui,valid && battery>10 && battery<=30));
    check(p,gsp_pet_batt_red_set_visible(p->ui,valid && battery<=10));
    if (linked && p->data.remaining >= 0) snprintf(value, sizeof(value), "%d%%", p->data.remaining);
    else snprintf(value, sizeof(value), "--%%");
    check(p, gsp_pet_quota_set_text(p->ui, value));
    check(p, gsp_pet_quota_label_set_text(p->ui, p->data.quota_label[0] ? p->data.quota_label : "剩余额度"));
    if (linked) remaining_time(value, sizeof(value), now, p->data.reset_at);
    else snprintf(value, sizeof(value), "--");
    check(p, gsp_pet_reset_set_text(p->ui, value));
    if (linked) snprintf(value, sizeof(value), "%u%s", p->data.active, p->data.partial ? "+" : "");
    else snprintf(value, sizeof(value), "--");
    check(p, gsp_pet_active_set_text(p->ui, value));
    check(p, gsp_pet_blue_set_visible(p->ui, done));
    if(done && p->data.done_count>1)snprintf(value,sizeof(value),"刚完成%u项",p->data.done_count);
    else snprintf(value,sizeof(value),"%s",done?"刚刚完成":"最近交互");
    check(p,gsp_pet_done_set_text(p->ui,value));
    char completed[PET_TITLE_BYTES+4];
    size_t used=0;unsigned cells=0;
    const char *recent=linked && p->data.recent_title[0]?p->data.recent_title:linked?"暂无交互记录":"等待数据同步";
    const unsigned char *title=(const unsigned char *)(done?p->data.done_title:recent);
    while(title[used]){
        unsigned bytes=title[used]<0x80?1:(title[used]&0xe0)==0xc0?2:(title[used]&0xf0)==0xe0?3:4;
        unsigned width=bytes==1?1:2;
        if(cells+width>24)break;
        cells+=width;used+=bytes;
    }
    memcpy(completed,title,used);completed[used]=0;
    if(title[used])strcat(completed,"…");
    check(p,gsp_pet_done_title_set_text(p->ui,completed));
    esp_gsp_err_t (*const labels[])(esp_gsp_handle_t, const char *) = {
        gsp_pet_task0_set_text, gsp_pet_task1_set_text, gsp_pet_task2_set_text, gsp_pet_task3_set_text,
    };
    for (unsigned i = 0; i < PET_TASKS; ++i) {
        const char *title = linked ? p->data.tasks[i] : "";
        if (i == 0 && linked && !p->data.active && !p->data.tasks[i][0]) title = "暂无进行中的任务";
        if (i == 0 && !linked) title = "等待云端数据";
        check(p, labels[i](p->ui, title));
        task_icon(p,i,linked && p->data.tasks[i][0]);
    }
    check(p,gsp_pet_mg_detail_heading_set_text(p->ui,done?"最近完成":"最近交互"));
    check(p, gsp_pet_done_detail_set_text(p->ui, done ? p->data.done_title : recent));
}
static void management_send(struct app_pet *p,app_pet_management_action_t action,unsigned value)
{
    if(p->management_action)p->management_action(p->management_ctx,action,value);
}
static void show_page(struct app_pet *p,unsigned page)
{
    unsigned old=p->page;p->page=page;p->settings=page==2;p->settings_dirty=true;
    esp_gsp_err_t (*const roots[])(esp_gsp_handle_t,bool)={
        gsp_pet_settings_home_set_visible,gsp_pet_settings_set_visible,gsp_pet_terminals_set_visible,
        gsp_pet_wifi_set_visible,gsp_pet_wifi_detail_set_visible,gsp_pet_enroll_set_visible,
        gsp_pet_portal_set_visible,gsp_pet_wifi_confirm_set_visible,
        gsp_pet_wifi_scan_set_visible,gsp_pet_wifi_entry_set_visible,gsp_pet_voice_page_set_visible};
    if(old>0 && old<=11)check(p,roots[old-1](p->ui,false));
    if(page>0 && page<=11)check(p,roots[page-1](p->ui,true));
    if(old==10 && page!=10){
        memset(p->input,0,sizeof(p->input));
        check(p,gsp_pet_mg_entry_display_set_text(p->ui,""));
    }
    management_send(p,PET_MANAGE_SCREEN,page==3?1:(page>=4 && page<=10)?2:0);
    if(page==2){
        check(p,gsp_pet_volume_set_value(p->ui,p->preferences.volume));
        check(p,gsp_pet_brightness_set_value(p->ui,p->preferences.brightness));
    }
}
static void render_management(struct app_pet *p)
{
    app_pet_management_t *m=&p->management;char value[128];
    const char *modes[]={"自动跟随","固定终端","全部汇总"};
    if(p->page==1){
        if(p->preferences.version[0])check(p,gsp_pet_home_version_set_text(p->ui,p->preferences.version));
        check(p,gsp_pet_mg_hub_source_set_text(p->ui,modes[m->mode<3?m->mode:0]));
        check(p,gsp_pet_mg_hub_wifi_set_text(p->ui,m->connected_ssid[0]?m->connected_ssid:"未连接"));
        check(p,gsp_pet_mg_hub_update_set_text(p->ui,p->preferences.wireless_ready?"已就绪":"待配对"));return;
    }
    if(p->page==0 || p->page==2)return;
    esp_gsp_err_t (*const mode[])(esp_gsp_handle_t,bool)={gsp_pet_mg_mode0_set_visible,gsp_pet_mg_mode1_set_visible,gsp_pet_mg_mode2_set_visible};
    esp_gsp_err_t (*const selected[])(esp_gsp_handle_t,bool)={gsp_pet_mg_terminal_selected0_set_visible,gsp_pet_mg_terminal_selected1_set_visible,gsp_pet_mg_terminal_selected2_set_visible,gsp_pet_mg_terminal_selected3_set_visible};
    esp_gsp_err_t (*const terminals[])(esp_gsp_handle_t,const char*)={gsp_pet_mg_terminal0_set_text,gsp_pet_mg_terminal1_set_text,gsp_pet_mg_terminal2_set_text,gsp_pet_mg_terminal3_set_text};
    esp_gsp_err_t (*const wifi[])(esp_gsp_handle_t,const char*)={gsp_pet_mg_wifi0_set_text,gsp_pet_mg_wifi1_set_text,gsp_pet_mg_wifi2_set_text,gsp_pet_mg_wifi3_set_text};
    if(p->page==3)for(unsigned i=0;i<3;++i)check(p,mode[i](p->ui,m->mode==i));
    for(unsigned i=0;i<4;++i){
        if(m->terminal_ids[i][0])snprintf(value,sizeof(value),"%s · %s %u",m->terminal_names[i],m->terminal_online[i]?"在线":"离线",m->terminal_active[i]);
        else value[0]=0;
        if(p->page==3)check(p,terminals[i](p->ui,value));
        if(p->page==3)check(p,selected[i](p->ui,m->terminal_ids[i][0]&&!strcmp(m->terminal_ids[i],m->selected_terminal)));
        if(m->wifi_ssids[i][0])snprintf(value,sizeof(value),"%u  %s%s",m->wifi_page*4+i+1,m->wifi_ssids[i],!strcmp(m->wifi_ssids[i],m->connected_ssid)?" · 已连接":"");
        else value[0]=0;
        if(p->page==4)check(p,wifi[i](p->ui,value));
    }
    snprintf(value,sizeof(value),"%u/%u",m->terminal_page+1,(m->terminal_total+3)/4? (m->terminal_total+3)/4:1);
    if(p->page==3)check(p,gsp_pet_mg_terminal_page_set_text(p->ui,value));
    snprintf(value,sizeof(value),"%u/%u",m->wifi_page+1,(m->wifi_total+3)/4?(m->wifi_total+3)/4:1);
    if(p->page==4)check(p,gsp_pet_mg_wifi_page_set_text(p->ui,value));
    if(p->page==3)check(p,gsp_pet_mg_terminal_status_set_text(p->ui,m->status));
    if(p->page==4)check(p,gsp_pet_mg_wifi_message_set_text(p->ui,m->status));
    if(p->page==4)check(p,gsp_pet_mg_connected_set_text(p->ui,m->connected_ssid[0]?m->connected_ssid:"未连接"));
    if(p->page==5)check(p,gsp_pet_mg_wifi_name_set_text(p->ui,p->wifi_name));
    if(p->page==8)check(p,gsp_pet_mg_delete_name_set_text(p->ui,p->wifi_name));
    snprintf(value,sizeof(value),"连接优先级 %u",p->wifi_index+1);
    if(p->page==5)check(p,gsp_pet_mg_wifi_priority_set_text(p->ui,value));
    if(p->page==6)check(p,gsp_pet_enrollment_code_set_text(p->ui,m->enrollment_code[0]?m->enrollment_code:"..."));
    if(p->page==6)check(p,gsp_pet_mg_enrollment_status_set_text(p->ui,m->status));
    if(p->page==7)check(p,gsp_pet_mg_portal_ssid_set_text(p->ui,m->portal_active?m->portal_ssid:"热点已关闭"));
    if(p->page==7)check(p,gsp_pet_mg_portal_password_set_text(p->ui,m->portal_active?m->portal_password:""));
    if(p->page==7)check(p,gsp_pet_mg_portal_message_set_text(p->ui,m->status));
}
static void start_entry(struct app_pet *p,bool ssid)
{
    p->edit_ssid=ssid;
    p->reveal=false;
    show_page(p,10);
    memset(p->input,0,sizeof(p->input));
    check(p,gsp_pet_mg_entry_title_set_text(p->ui,ssid?"输入网络名称":p->join_ssid));
    check(p,gsp_pet_mg_entry_status_set_text(p->ui,ssid?"输入后按 OK":"输入密码后按 OK，开放网络直接按 OK"));
}
static void render_wifi_entry(struct app_pet *p,bool refresh)
{
    if(p->page==9 && refresh){
        esp_gsp_err_t (*const rows[])(esp_gsp_handle_t,const char *)={
            gsp_pet_mg_scan0_set_text,gsp_pet_mg_scan1_set_text,gsp_pet_mg_scan2_set_text,
            gsp_pet_mg_scan3_set_text,gsp_pet_mg_scan4_set_text};
        for(unsigned i=0;i<5;++i){
            unsigned index=p->scan_page*5+i;
            check(p,rows[i](p->ui,index<p->management.scan_count?p->management.scan_ssids[index]:""));
        }
        check(p,gsp_pet_mg_scan_status_set_text(p->ui,p->management.scan_count?"请选择网络":p->management.status));
    }
    if(p->page!=10)return;
    if(p->joining){
        unsigned result=p->management.join_result;
        if(p->join_revision!=p->management.join_revision && result>=2){
            p->joining=false;
            if(result==2){show_page(p,4);render_management(p);}
            else check(p,gsp_pet_mg_entry_status_set_text(p->ui,"连接或保存失败，请检查密码后重试"));
        }
        return;
    }
    char value[65];
    strcpy(value,p->input);
    if(!p->edit_ssid && !p->reveal)memset(value,'*',strlen(value));
    check(p,gsp_pet_mg_entry_toggle_set_text(p->ui,p->edit_ssid?"":p->reveal?"隐藏":"显示"));
    check(p,gsp_pet_mg_entry_display_set_text(p->ui,value));
    memset(value,0,sizeof(value));
}
static bool wifi_entry_event(struct app_pet *p,const esp_gsp_event_t *e)
{
    if(gsp_pet_event_is_new_wifi(e)||gsp_pet_event_is_scan_refresh(e)){
        p->scan_page=0;
        show_page(p,9);
        management_send(p,PET_MANAGE_SCAN,0);
    }else if(gsp_pet_event_is_scan_back(e))show_page(p,4);
    else if(gsp_pet_event_is_scan_manual(e)){p->join_ssid[0]=0;start_entry(p,true);}
    else if(gsp_pet_event_is_scan_previous(e)){if(p->scan_page)--p->scan_page;}
    else if(gsp_pet_event_is_scan_next(e)){if((p->scan_page+1)*5<p->management.scan_count)++p->scan_page;}
    else if(gsp_pet_event_is_entry_back(e)){if(!p->joining)show_page(p,9);}
    else if(gsp_pet_event_is_entry_show(e))p->reveal=!p->reveal;
    else if(gsp_pet_event_is_entry_lower(e)||gsp_pet_event_is_entry_upper(e)||gsp_pet_event_is_entry_symbols(e)){
        p->key_mode=gsp_pet_event_is_entry_lower(e)?0:gsp_pet_event_is_entry_upper(e)?1:2;
        check(p,gsp_pet_key_layer0_set_visible(p->ui,p->key_mode==0));
        check(p,gsp_pet_key_layer1_set_visible(p->ui,p->key_mode==1));
        check(p,gsp_pet_key_layer2_set_visible(p->ui,p->key_mode==2));
    }else if(gsp_pet_event_is_entry_delete(e)){
        size_t n=strlen(p->input);
        if(!p->joining && n)p->input[n-1]=0;
    }else if(e->type==ESP_GSP_EVENT_CALL && e->action_id>=GSP_PET_ACTION_ENTRY_KEY00 && e->action_id<=GSP_PET_ACTION_ENTRY_KEY42){
        static const char *const keys[]={"0123456789abcdefghijklmnopqrstuvwxyz ","0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ","0123456789!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ "};
        size_t i=e->action_id-GSP_PET_ACTION_ENTRY_KEY00,n=strlen(p->input);
        if(!p->joining && i<strlen(keys[p->key_mode]) && n<(p->edit_ssid?32U:64U)){
            p->input[n]=keys[p->key_mode][i];
            p->input[n+1]=0;
        }
    }else if(gsp_pet_event_is_entry_ok(e)){
        if(p->page!=10 || p->joining)return true;
        if(p->edit_ssid){
            if(p->input[0]){strcpy(p->join_ssid,p->input);start_entry(p,false);}
        }else if(p->join && p->join(p->management_ctx,p->join_ssid,p->input)){
            p->joining=true;p->join_revision=p->management.join_revision;
            check(p,gsp_pet_mg_entry_display_set_text(p->ui,""));
            check(p,gsp_pet_mg_entry_status_set_text(p->ui,"正在连接…"));
        }else check(p,gsp_pet_mg_entry_status_set_text(p->ui,"暂时无法连接，请重试"));
    }else {
        unsigned row;
        if(gsp_pet_event_is_scan_row0(e))row=0;
        else if(gsp_pet_event_is_scan_row1(e))row=1;
        else if(gsp_pet_event_is_scan_row2(e))row=2;
        else if(gsp_pet_event_is_scan_row3(e))row=3;
        else if(gsp_pet_event_is_scan_row4(e))row=4;
        else return false;
        unsigned index=p->scan_page*5+row;
        if(index<p->management.scan_count){
            strcpy(p->join_ssid,p->management.scan_ssids[index]);
            start_entry(p,false);
        }
    }
    render_wifi_entry(p,true);
    return true;
}
static bool management_event(struct app_pet *p,const esp_gsp_event_t *e)
{
    if(gsp_pet_event_is_show_settings(e))show_page(p,1);
    else if(gsp_pet_event_is_hub_back(e))show_page(p,0);
    else if(gsp_pet_event_is_open_preferences(e))show_page(p,2);
    else if(gsp_pet_event_is_settings_back(e)||gsp_pet_event_is_terminals_back(e)||gsp_pet_event_is_wifi_back(e))show_page(p,1);
    else if(gsp_pet_event_is_open_terminals(e))show_page(p,3);
    else if(gsp_pet_event_is_open_wifi(e))show_page(p,4);
    else if(gsp_pet_event_is_wifi_detail_back(e)||gsp_pet_event_is_portal_back(e))show_page(p,4);
    else if(gsp_pet_event_is_enrollment_back(e))show_page(p,3);
    else if(gsp_pet_event_is_mode0(e))management_send(p,PET_MANAGE_MODE,0);
    else if(gsp_pet_event_is_mode1(e))management_send(p,PET_MANAGE_MODE,1);
    else if(gsp_pet_event_is_mode2(e))management_send(p,PET_MANAGE_MODE,2);
    else if(gsp_pet_event_is_terminal_previous(e))management_send(p,PET_MANAGE_TERMINAL_PAGE,p->management.terminal_page?p->management.terminal_page-1:0);
    else if(gsp_pet_event_is_terminal_next(e))management_send(p,PET_MANAGE_TERMINAL_PAGE,p->management.terminal_page+1);
    else if(gsp_pet_event_is_wifi_previous(e))management_send(p,PET_MANAGE_WIFI_PAGE,p->management.wifi_page?p->management.wifi_page-1:0);
    else if(gsp_pet_event_is_wifi_next(e))management_send(p,PET_MANAGE_WIFI_PAGE,p->management.wifi_page+1);
    else if(gsp_pet_event_is_new_terminal(e)||gsp_pet_event_is_regenerate_code(e)){show_page(p,6);management_send(p,PET_MANAGE_ENROLL,0);}
    else if(gsp_pet_event_is_new_wifi(e)){show_page(p,7);management_send(p,PET_MANAGE_WIFI_ADD,0);}
    else if(gsp_pet_event_is_close_portal(e)){management_send(p,PET_MANAGE_PORTAL_STOP,0);show_page(p,4);}
    else if(gsp_pet_event_is_wifi_connect(e)){management_send(p,PET_MANAGE_WIFI_CONNECT,p->wifi_index);show_page(p,4);}
    else if(gsp_pet_event_is_wifi_promote(e)){management_send(p,PET_MANAGE_WIFI_PROMOTE,p->wifi_index);show_page(p,4);}
    else if(gsp_pet_event_is_wifi_delete(e))show_page(p,8);
    else if(gsp_pet_event_is_delete_cancel(e)||gsp_pet_event_is_delete_cancel_button(e))show_page(p,5);
    else if(gsp_pet_event_is_delete_confirm(e)){management_send(p,PET_MANAGE_WIFI_DELETE,p->wifi_index);show_page(p,4);}
    else {
        int row=-1;bool wifi=false;
        if(gsp_pet_event_is_terminal0(e))row=0;
        else if(gsp_pet_event_is_terminal1(e))row=1;
        else if(gsp_pet_event_is_terminal2(e))row=2;
        else if(gsp_pet_event_is_terminal3(e))row=3;
        else if(gsp_pet_event_is_wifi0(e)){row=0;wifi=true;}
        else if(gsp_pet_event_is_wifi1(e)){row=1;wifi=true;}
        else if(gsp_pet_event_is_wifi2(e)){row=2;wifi=true;}
        else if(gsp_pet_event_is_wifi3(e)){row=3;wifi=true;}
        else return false;
        if(wifi && p->management.wifi_ssids[row][0]){
            p->wifi_index=p->management.wifi_page*4+(unsigned)row;
            snprintf(p->wifi_name,sizeof(p->wifi_name),"%s",p->management.wifi_ssids[row]);show_page(p,5);
        }else if(!wifi && p->management.terminal_ids[row][0])management_send(p,PET_MANAGE_TERMINAL,(unsigned)row);
    }
    render_management(p);if(p->page==0)render(p);return true;
}
static void render_voice(struct app_pet *p)
{
    if(p->page!=11)return;
    app_pet_voice_t *v=&p->voice;
    check(p,gsp_pet_mg_voice_target_set_text(p->ui,v->name[0]?v->name:"语音输入"));
    const char *status=v->error[0]?v->error:v->recording?"正在听写":!strcmp(v->state,"warming")?"正在加载语音模型":v->busy?"正在处理":!strcmp(v->state,"sent")?"已发送":v->connected?"点击麦克风继续":"等待 Mac 连接";
    check(p,gsp_pet_mg_voice_status_set_text(p->ui,status));
    check(p,gsp_pet_voice_record_set_enabled(p->ui,!v->busy));
    check(p,gsp_pet_voice_send_set_enabled(p->ui,!v->busy && v->text[0]));
    check(p,gsp_pet_voice_delete_set_enabled(p->ui,!v->busy && v->text[0]));
    esp_gsp_err_t (*const bars[])(esp_gsp_handle_t,bool)={gsp_pet_voice_bar0_set_visible,gsp_pet_voice_bar1_set_visible,gsp_pet_voice_bar2_set_visible,gsp_pet_voice_bar3_set_visible,gsp_pet_voice_bar4_set_visible,gsp_pet_voice_bar5_set_visible,gsp_pet_voice_bar6_set_visible,gsp_pet_voice_bar7_set_visible,gsp_pet_voice_bar8_set_visible};
    for(unsigned i=0;i<9;++i)check(p,bars[i](p->ui,v->recording && (i<2+v->level/13 || i==p->ticks%9)));
}
static bool voice_event(struct app_pet *p,const esp_gsp_event_t *e)
{
    const char *action=NULL;
    if(gsp_pet_event_is_open_voice(e)){p->details=false;check(p,gsp_pet_details_set_visible(p->ui,false));show_page(p,11);action="open";}
    else if(gsp_pet_event_is_voice_back(e)){action="leave";show_page(p,0);render(p);}
    else if(gsp_pet_event_is_voice_record(e))action=p->voice.recording?"stop":"start";
    else if(gsp_pet_event_is_voice_send(e))action="send";
    else if(gsp_pet_event_is_voice_delete_press(e)){p->voice_long=false;return true;}
    else if(gsp_pet_event_is_voice_clear(e)){p->voice_long=true;action="clear";}
    else if(gsp_pet_event_is_voice_delete_release(e)){if(!p->voice_long)action="delete";}
    else return false;
    if(action && p->voice_action && !p->voice_action(p->voice_ctx,action))snprintf(p->voice.error,sizeof(p->voice.error),"操作繁忙，请稍后再试");
    render_voice(p);return true;
}
void app_pet_set_voice(app_pet_handle_t p,app_pet_voice_source_t source,app_pet_voice_action_t action,void *ctx){p->voice_source=source;p->voice_action=action;p->voice_ctx=ctx;}
static void timer_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    struct app_pet *p = ctx;
    ++p->ticks;
    if(p->page==11 && p->voice_source && p->voice_source(p->voice_ctx,&p->voice)){render_voice(p);}
    bool refresh=p->management_source && p->management_source(p->management_ctx,&p->management) && p->management_generation!=p->management.generation;
    if(refresh){render_management(p);p->management_generation=p->management.generation;}
    if(refresh && (p->page==9 || p->joining))render_wifi_entry(p,true);
    bool changed = p->source && p->source(p->source_ctx, &p->data);
    if (changed) { p->received = true; p->received_tick = p->ticks; }
    if (p->page==0 && (changed || (p->ticks-p->last_draw_tick)*TICK_MS >= 1000U)) {
        render(p);
        p->last_draw_tick = p->ticks;
    }
    int face = 3;
    if (online(p)) {
        switch (p->data.state) {
        case PET_WORKING: face = 0; break;
        case PET_WAITING: face = 2; break;
        case PET_COMPLETED: face = 4; break;
        case PET_FAILED: face = 5; break;
        default: face = 3; break;
        }
        if (p->ticks % 19 == 0) face = 1;
    }
    eyes(p, face);
    if(p->details && online(p)){
        esp_err_t (*const phases[PET_TASKS][6])(esp_gsp_handle_t,bool)={
        {gsp_pet_task0_spin0_set_visible,gsp_pet_task0_spin1_set_visible,gsp_pet_task0_spin2_set_visible,gsp_pet_task0_spin3_set_visible,gsp_pet_task0_spin4_set_visible,gsp_pet_task0_spin5_set_visible},
        {gsp_pet_task1_spin0_set_visible,gsp_pet_task1_spin1_set_visible,gsp_pet_task1_spin2_set_visible,gsp_pet_task1_spin3_set_visible,gsp_pet_task1_spin4_set_visible,gsp_pet_task1_spin5_set_visible},
        {gsp_pet_task2_spin0_set_visible,gsp_pet_task2_spin1_set_visible,gsp_pet_task2_spin2_set_visible,gsp_pet_task2_spin3_set_visible,gsp_pet_task2_spin4_set_visible,gsp_pet_task2_spin5_set_visible},
        {gsp_pet_task3_spin0_set_visible,gsp_pet_task3_spin1_set_visible,gsp_pet_task3_spin2_set_visible,gsp_pet_task3_spin3_set_visible,gsp_pet_task3_spin4_set_visible,gsp_pet_task3_spin5_set_visible}};
        for(unsigned i=0;i<PET_TASKS;++i)if(p->shown_icons[i]==0)
            for(unsigned phase=0;phase<6;++phase)check(p,phases[i][phase](p->ui,phase==p->ticks%6));
    }
    if(p->preferences_source){
        app_pet_preferences_t prefs;
        if(p->preferences_source(p->controls_ctx,&prefs)){
            if(prefs.color<6 && p->shown_color!=(int)prefs.color){
                static const uint32_t colors[]={0x7028e8,0x1677ff,0x17b897,0xd97706,0xd94f9d,0x169db9};
                esp_err_t (*const heads[])(esp_gsp_handle_t,uint32_t)={gsp_pet_head_open_set_color_rgb888,gsp_pet_head_blink_set_color_rgb888,gsp_pet_head_waiting_set_color_rgb888,gsp_pet_head_idle_set_color_rgb888,gsp_pet_head_happy_set_color_rgb888,gsp_pet_head_failed_set_color_rgb888};
                for(unsigned i=0;i<6;++i)check(p,heads[i](p->ui,colors[prefs.color]));
                p->shown_color=prefs.color;
            }
            bool changed=p->settings_dirty || prefs.volume!=p->preferences.volume || prefs.brightness!=p->preferences.brightness || prefs.color!=p->preferences.color || prefs.sound!=p->preferences.sound || prefs.vibration!=p->preferences.vibration || prefs.wireless_ready!=p->preferences.wireless_ready || prefs.save_error!=p->preferences.save_error || strcmp(prefs.version,p->preferences.version)!=0;
            if(p->settings && changed){
                if(prefs.volume!=p->preferences.volume)check(p,gsp_pet_volume_set_value(p->ui,prefs.volume));
                if(prefs.brightness!=p->preferences.brightness)check(p,gsp_pet_brightness_set_value(p->ui,prefs.brightness));
                if(prefs.version[0])check(p,gsp_pet_version_set_text(p->ui,prefs.version));
                check(p,gsp_pet_settings_result_set_text(p->ui,prefs.save_error?"未保存":""));
                char value[24];snprintf(value,sizeof(value),"%u%%",prefs.volume);check(p,gsp_pet_volume_text_set_text(p->ui,value));
                snprintf(value,sizeof(value),"%u%%",prefs.brightness);check(p,gsp_pet_brightness_text_set_text(p->ui,value));
                check(p,gsp_pet_vibration_text_set_text(p->ui,prefs.vibration?"已开启":"已关闭"));
                check(p,gsp_pet_preview_set_enabled(p->ui,prefs.sound && prefs.volume>0));
                check(p,gsp_pet_sound_text_set_text(p->ui,prefs.sound?"已开启":"已关闭"));
                check(p,gsp_pet_wireless_text_set_text(p->ui,prefs.wireless_ready?"无线更新已就绪":"无线更新待配对"));
                esp_err_t (*const selected[])(esp_gsp_handle_t,bool)={gsp_pet_selected0_set_visible,gsp_pet_selected1_set_visible,gsp_pet_selected2_set_visible,gsp_pet_selected3_set_visible,gsp_pet_selected4_set_visible,gsp_pet_selected5_set_visible};
                for(unsigned i=0;i<6;++i)check(p,selected[i](p->ui,prefs.color==i));
                p->settings_dirty=false;
            }
            p->preferences=prefs;
        }
    }
}
static void on_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{
    struct app_pet *p = ctx;
    if(voice_event(p,event)||wifi_entry_event(p,event)||management_event(p,event))return;
    if(gsp_pet_event_is_show_tasks(event))p->details=true;
    else if(gsp_pet_event_is_back(event))p->details=false;
    else {
        app_pet_action_t action;int32_t value=0;
        if(gsp_pet_event_is_volume_changed(event)){action=PET_SET_VOLUME;check(p,gsp_pet_volume_get_value(ui,&value));}
        else if(gsp_pet_event_is_brightness_changed(event)){action=PET_SET_BRIGHTNESS;check(p,gsp_pet_brightness_get_value(ui,&value));}
        else if(gsp_pet_event_is_vibration(event))action=PET_TOGGLE_VIBRATION;
        else if(gsp_pet_event_is_preview(event))action=PET_PREVIEW;
        else if(gsp_pet_event_is_sound(event)){action=PET_SET_SOUND;value=!p->preferences.sound;}
        else if(gsp_pet_event_is_color0(event)){action=PET_SET_COLOR;value=0;}
        else if(gsp_pet_event_is_color1(event)){action=PET_SET_COLOR;value=1;}
        else if(gsp_pet_event_is_color2(event)){action=PET_SET_COLOR;value=2;}
        else if(gsp_pet_event_is_color3(event)){action=PET_SET_COLOR;value=3;}
        else if(gsp_pet_event_is_color4(event)){action=PET_SET_COLOR;value=4;}
        else if(gsp_pet_event_is_color5(event)){action=PET_SET_COLOR;value=5;}
        else return;
        bool sent=p->action_handler && p->action_handler(p->controls_ctx,action,(unsigned)value);
        check(p,gsp_pet_settings_result_set_text(ui,sent?"":"未保存"));
        return;
    }
    check(p,gsp_pet_details_set_visible(ui,p->details));
    check(p,gsp_pet_settings_set_visible(ui,p->settings));
}
esp_gsp_err_t app_pet_create(esp_gsp_handle_t ui, app_pet_source_t source, void *source_ctx, app_pet_handle_t *out)
{
    if (!ui || !out) return ESP_GSP_ERR_INVALID_ARG;
    *out = NULL;
    struct app_pet *p = calloc(1, sizeof(*p));
    if (!p) return ESP_GSP_ERR_NO_MEM;
    p->ui = ui; p->source = source; p->source_ctx = source_ctx; p->shown_eyes = -1;p->shown_color=-1;
    p->data.remaining = -1;
    for(unsigned i=0;i<PET_TASKS;++i)p->shown_icons[i]=-1;
    render(p); eyes(p, 3);
    esp_gsp_err_t e = esp_gsp_on_event(ui, on_event, p);
    if (e == ESP_GSP_OK) {
        p->timer = esp_gsp_timer_create(ui, TICK_MS, timer_tick, p);
        if (!p->timer) e = ESP_GSP_ERR_NO_MEM;
    }
    if (e != ESP_GSP_OK) { esp_gsp_on_event(ui, NULL, NULL); free(p); return e; }
    *out = p;
    return ESP_GSP_OK;
}
esp_gsp_err_t app_pet_destroy(app_pet_handle_t *handle)
{
    if (!handle) return ESP_GSP_ERR_INVALID_ARG;
    struct app_pet *p = *handle;
    if (!p) return ESP_GSP_OK;
    esp_gsp_err_t e = esp_gsp_on_event(p->ui, NULL, NULL);
    if (e != ESP_GSP_OK) return e;
    e = esp_gsp_timer_delete(p->ui, p->timer);
    if (e != ESP_GSP_OK) return e;
    free(p); *handle = NULL;
    return ESP_GSP_OK;
}

void app_pet_set_battery_source(app_pet_handle_t p, app_pet_battery_source_t source, void *ctx)
{
    if(!p)return;
    p->battery_source=source;p->battery_ctx=ctx;
}

void app_pet_set_controls(app_pet_handle_t p,app_pet_preferences_source_t source,app_pet_action_handler_t handler,void *ctx)
{
    if(!p)return;
    p->controls_ctx=ctx;p->preferences_source=source;p->action_handler=handler;
}

void app_pet_set_management(app_pet_handle_t p,app_pet_management_source_t source,app_pet_management_handler_t action,void *ctx)
{
    if(!p)return;
    p->management_source=source;p->management_action=action;p->management_ctx=ctx;
}

void app_pet_set_join(app_pet_handle_t p,app_pet_join_handler_t handler){if(p)p->join=handler;}
