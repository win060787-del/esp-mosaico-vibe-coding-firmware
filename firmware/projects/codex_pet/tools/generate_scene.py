"""Authored scene source; regenerate JSON instead of editing generated output."""
import json, math
from pathlib import Path
chars=set(range(32,127));chars.add(0x00b7)
for hi in range(0xa1,0xf8):
 for lo in range(0xa1,0xff):
  try:chars.update(map(ord,bytes([hi,lo]).decode('gb2312')))
  except UnicodeDecodeError:pass
Path('components/app_pet/ui/fonts/task-charset.txt').write_text(''.join(chr(c) for c in sorted(chars)))
objects=[]
def obj(kind,x,y,w,h,parent=-1,**kw):
 objects.append(dict(type=kind,parent=parent,x=x,y=y,w=w,h=h,**kw));return len(objects)-1
def box(x,y,w,h,color='#FFFFFF',parent=-1,**kw):
 return obj('container',x,y,w,h,parent,bg_color=color,**kw)
def text(x,y,w,h,value,size=18,color='#FFFFFF',parent=-1,**kw):
 size={13:16,15:18,17:20,18:22,20:24,21:25,22:26,29:35,30:36}.get(size,round(size*1.2))
 return obj('label',x,y,w,h,parent,text=value,font_size=size,fg_color=color,**kw)
def bindtext(name,x,y,w,h,value,size=18,color='#FFFFFF',parent=-1,**kw):

 if name.startswith('task') or name.startswith('mg_') or name in ('done_detail','done_title'): kw['font_charset_file']='fonts/task-charset.txt'
 else: kw['font_charset']=''.join(chr(x) for x in range(32,127))+'最近交互本周剩余额度小时距重置天分刷新中等待连接离线空闲正在工作你确认任务已完成失败刚项设置音量提示音试听震动亮度开启关闭无线更新就绪配对未保存声音颜色按终端查看全部USB'
 return text(x,y,w,h,value,size,color,parent,name=name,bind=name,**kw)
bindtext('clock',170,20,140,30,'--:--',20,text_align='center')
# Local battery status, sampled independently from cloud data.
bindtext('battery',397,20,58,30,'--%',15,text_align='right')
for kind,color,level in [('unknown','#92999F',0),('green','#34C759',18),('yellow','#FFCC00',9),('red','#FF3B30',4)]:
 g=box(360,29,30,15,'#000000',name='batt_'+kind,bind='batt_'+kind,bind_target='visible',hidden=kind!='unknown')
 box(0,0,26,15,'#AEB7BE',parent=g,radius=3);box(2,2,22,11,'#000000',parent=g,radius=1)
 if level:box(4,4,level,7,color,parent=g,radius=1)
 box(27,4,3,7,'#AEB7BE',parent=g,radius=1)
face=obj('button',55,68,370,244,bg_color='#000000',name='face',callback='open_voice')
# Native geometry only: overlapping circles form each tilted capsule.
def capsule(cx,cy,w,h,parent,color='#FFFFFF'):
 r=min(w,h)/2;theta=math.radians(18)
 dx,dy=((-math.sin(theta)*(h-w)/2,math.cos(theta)*(h-w)/2) if h>=w else (math.cos(theta)*(w-h)/2,math.sin(theta)*(w-h)/2))
 x1,y1=cx-dx,cy-dy;x2,y2=cx+dx,cy+dy
 steps=max(1,math.ceil(math.hypot(x2-x1,y2-y1)/3))
 for i in range(steps+1):
  x=x1+(x2-x1)*i/steps;y=y1+(y2-y1)*i/steps
  box(round(x-r),round(y-r),round(2*r),round(2*r),color,parent=parent,radius=round(r))
for name,mode in [('open',0),('blink',1),('waiting',2),('idle',3),('happy',4),('failed',5)]:
 g=box(0,0,370,220,'#000000',parent=face,name=name,bind=name,bind_target='visible',hidden=mode!=3)
 box(87,9,196,196,'#7028E8',parent=g,radius=98,name='head_'+name,bind='head_'+name,bind_target='color')
 for cx in [147,222]:
  if mode==4:
   for x in range(-14,15,2):
    y=110-13.5*(1-(x/14)**2)
    box(round(cx+x-4),round(y-4),8,8,parent=g,radius=4)
  else:
   h=6 if mode==1 else 18 if mode==5 else 48 if mode==3 else 34 if mode==2 and cx==222 else 66
   capsule(cx,102,32,h,g)
bindtext('mg_source',326,279,130,30,'',17,'#92999F',text_align='right')
bindtext('status',24,274,432,34,'等待连接',18,text_align='center')
bindtext('brand',28,279,140,30,'',20)
quota_card=obj('button',24,321,432,78,bg_color='#111315',radius=16,name='quota_card',callback='show_tasks')
bindtext('quota_label',43,333,130,25,'剩余额度',13,'#92999F')
bindtext('quota',43,355,130,38,'--%',29)
text(184,333,150,25,'重置',13,'#92999F')
bindtext('reset',184,358,158,33,'--',21)
text(355,333,90,25,'进行中',13,'#92999F')
bindtext('active',355,355,90,38,'--',29)
obj('button',24,414,432,52,bg_color='#000000',name='recent_card',callback='show_tasks')
box(29,438,8,8,'#4C9CFF',radius=4,name='blue',bind='blue',bind_target='visible',hidden=True)
bindtext('done',47,427,76,30,'',13,'#BAC2CA')
bindtext('done_title',134,427,272,30,'',17)
for kind,color in [('online','#92999F'),('offline','#4A4F54')]:
 g=box(82,23,28,28,'#000000',name='link_'+kind,bind='link_'+kind,bind_target='visible',hidden=kind=='online')
 for x,y,w in [(1,0,26),(6,6,16)]:obj('arc',x,y,w,w,parent=g,min=0,max=100,value=100,start_angle=315,sweep=90,thickness=2,bg_color='#000000',fg_color=color)
 box(12,17,4,4,color,parent=g,radius=2)
 if kind=='offline':obj('shape',4,4,20,20,parent=g,shape='line',line_direction='bl_tr',border_color='#92999F',border_width=2)
# A visible settings button; artwork is native geometry too.
gear=obj('button',24,15,44,40,bg_color='#000000',name='gear',callback='show_settings')
for x,y,w,h in [(18,5,8,30),(7,16,30,8)]:box(x,y,w,h,'#92999F',parent=gear,radius=2)
box(11,9,22,22,'#92999F',parent=gear,radius=11);box(17,15,10,10,'#000000',parent=gear,radius=5)
# Task drawer opens from either lower information region.
detail=box(0,0,480,480,'#000000',name='details',bind='details',bind_target='visible',hidden=True)
back=obj('button',16,12,74,54,parent=detail,bg_color='#111315',radius=12,name='back',callback='back')
text(0,0,74,54,'‹',30,parent=back,text_align='center')
text(108,24,330,32,'当前任务',22,parent=detail)
for i in range(4):
 box(24,92+i*65,432,56,'#111315',parent=detail,radius=12)
 for kind in ['run','unread','unknown']:
  icon=box(36,100+i*65,20,40,'#111315',parent=detail,name=f'task{i}_{kind}',bind=f'task{i}_{kind}',bind_target='visible',hidden=True)
  if kind=='run':
   for phase in range(6):
    group=box(0,10,20,20,'#111315',parent=icon,name=f'task{i}_spin{phase}',bind=f'task{i}_spin{phase}',bind_target='visible',hidden=phase!=0)
    obj('arc',0,0,20,20,parent=group,min=0,max=100,value=100,start_angle=phase*60,sweep=270,thickness=3,bg_color='#111315',fg_color='#73D6A0')
  elif kind=='unread':box(3,13,14,14,'#73D6A0',parent=icon,radius=7)
  else:
   box(3,13,14,14,'#92999F',parent=icon,radius=7);box(6,16,8,8,'#111315',parent=icon,radius=4)
 bindtext('task'+str(i),66,100+i*65,376,40,'',17,parent=detail)
box(28,379,9,9,'#4C9CFF',parent=detail,radius=4)
bindtext('mg_detail_heading',48,366,390,30,'最近完成',17,'#92999F',parent=detail)
bindtext('done_detail',28,405,424,55,'暂无最近完成任务',17,parent=detail)
# Settings hub keeps all connection and appearance controls discoverable.
hub=box(0,0,480,480,'#000000',name='settings_home',bind='settings_home',bind_target='visible',hidden=True)
b=obj('button',16,12,74,54,parent=hub,bg_color='#111315',radius=12,name='hub_back',callback='hub_back')
text(0,0,74,54,'‹',30,parent=b,text_align='center')
text(108,24,200,32,'设置',22,parent=hub)
bindtext('home_version',330,25,124,28,'3.1.0',13,'#92999F',parent=hub,text_align='right')
for y,title,callback,subtitle in [(92,'外观与提醒','open_preferences','声音 · 震动 · 亮度 · 颜色'),(180,'终端','open_terminals',None),(268,'Wi-Fi','open_wifi',None)]:
 b=obj('button',24,y,432,72,parent=hub,bg_color='#111315',radius=16,name=callback,callback=callback)
 text(19,7,340,28,title,18,parent=b)
 if subtitle:text(19,38,360,25,subtitle,13,'#92999F',parent=b)
 else:bindtext('mg_hub_source' if callback=='open_terminals' else 'mg_hub_wifi',19,36,360,30,'',17,'#92999F',parent=b)
 text(380,14,36,44,'›',22,parent=b,text_align='center')
box(24,356,432,72,'#111315',parent=hub,radius=16)
text(43,363,340,28,'无线更新',18,parent=hub)
bindtext('mg_hub_update',43,392,360,30,'已就绪',17,'#92999F',parent=hub)
settings=box(0,0,480,480,'#000000',name='settings',bind='settings',bind_target='visible',hidden=True)
back=obj('button',16,12,74,54,parent=settings,bg_color='#111315',radius=12,name='settings_back',callback='settings_back')
text(0,0,74,54,'‹',30,parent=back,text_align='center')
text(108,24,230,32,'外观与提醒',22,parent=settings)
bindtext('version',330,25,124,28,'3.1.0',13,'#92999F',parent=settings,text_align='right')
box(24,80,432,110,'#111315',parent=settings,radius=16)
text(43,88,150,30,'声音',18,parent=settings)
bindtext('volume_text',235,88,73,28,'50%',15,parent=settings,text_align='right')
b=obj('button',326,83,110,38,parent=settings,bg_color='#33224D',radius=12,name='sound',callback='sound')
bindtext('sound_text',0,0,110,38,'已开启',15,parent=b,text_align='center')
obj('slider',43,143,278,24,parent=settings,name='volume',min=0,max=100,value=50,track_size=6,radius=12,bg_color='#34303D',fg_color='#A78BFA',knob_color='#FFFFFF',callback='volume_changed')
b=obj('button',346,138,90,36,parent=settings,bg_color='#33224D',radius=12,name='preview',callback='preview',enabled=True)
text(0,0,90,36,'试听',15,parent=b,text_align='center')
box(24,202,432,54,'#111315',parent=settings,radius=16)
text(43,214,170,30,'震动',18,parent=settings)
b=obj('button',326,208,110,40,parent=settings,bg_color='#33224D',radius=12,name='vibration',callback='vibration')
bindtext('vibration_text',0,0,110,40,'已开启',15,parent=b,text_align='center')
box(24,268,432,90,'#111315',parent=settings,radius=16)
text(43,275,180,30,'亮度',18,parent=settings)
bindtext('brightness_text',326,275,110,28,'100%',15,parent=settings,text_align='right')
obj('slider',43,318,393,24,parent=settings,name='brightness',min=10,max=100,value=100,track_size=6,radius=12,bg_color='#34303D',fg_color='#A78BFA',knob_color='#FFFFFF',callback='brightness_changed')
box(24,370,432,84,'#111315',parent=settings,radius=16)
text(43,375,180,24,'颜色',13,parent=settings)
for i,color in enumerate(['#7028E8','#1677FF','#17B897','#D97706','#D94F9D','#169DB9']):
 b=obj('button',54+64*i,404,52,48,parent=settings,bg_color='#111315',name=f'color{i}',callback=f'color{i}')
 selected=box(4,2,44,44,'#FFFFFF',parent=b,radius=22,name=f'selected{i}',bind=f'selected{i}',bind_target='visible',hidden=i!=0)
 box(6,4,40,40,'#111315',parent=b,radius=20)
 box(8,6,36,36,color,parent=b,radius=18)
bindtext('wireless_text',24,456,300,22,'无线更新已就绪',13,'#92999F',parent=settings)
bindtext('settings_result',326,456,130,22,'',13,'#92999F',parent=settings,text_align='right')
# Each management page uses the same native geometry and embedded text font.
def management_page(name,title,back_callback):
 g=box(0,0,480,480,'#000000',name=name,bind=name,bind_target='visible',hidden=True)
 b=obj('button',16,12,74,54,parent=g,bg_color='#111315',radius=12,name=back_callback,callback=back_callback)
 text(0,0,74,54,'‹',30,parent=b,text_align='center');text(108,24,330,32,title,22,parent=g)
 return g
def action_button(name,x,y,w,h,title,parent,color='#111315'):
 b=obj('button',x,y,w,h,parent=parent,bg_color=color,radius=12,name=name,callback=name,enabled=True)
 text(0,0,w,h,title,17,parent=b,text_align='center');return b
terminals=management_page('terminals','终端','terminals_back')
for i,title in enumerate(['自动跟随','固定终端','全部汇总']):
 b=action_button('mode'+str(i),24+i*147,84,138,44,title,terminals)
 # Selection is an unobtrusive native underline.
 box(16,39,106,3,'#A78BFA',parent=b,radius=1,name='mg_mode'+str(i),bind='mg_mode'+str(i),bind_target='visible',hidden=i!=0)
for i in range(4):
 b=obj('button',24,142+i*62,432,52,parent=terminals,bg_color='#111315',radius=12,name='terminal'+str(i),callback='terminal'+str(i))
 box(0,0,4,52,'#A78BFA',parent=b,name='mg_terminal_selected'+str(i),bind='mg_terminal_selected'+str(i),bind_target='visible',hidden=True)
 bindtext('mg_terminal'+str(i),18,7,396,38,'',17,parent=b)
bindtext('mg_terminal_status',24,385,432,24,'正在读取终端',17,'#92999F',parent=terminals)
action_button('new_terminal',24,416,216,44,'添加终端',terminals,'#33224D')
action_button('terminal_previous',254,416,70,44,'‹',terminals)
bindtext('mg_terminal_page',326,421,60,30,'1/1',17,'#92999F',parent=terminals,text_align='center')
action_button('terminal_next',390,416,66,44,'›',terminals)
wifi=management_page('wifi','Wi-Fi','wifi_back')
bindtext('mg_connected',24,80,432,36,'',17,'#92999F',parent=wifi)
for i in range(4):
 b=obj('button',24,130+i*62,432,52,parent=wifi,bg_color='#111315',radius=12,name='wifi'+str(i),callback='wifi'+str(i))
 bindtext('mg_wifi'+str(i),18,7,396,38,'',17,parent=b)
bindtext('mg_wifi_message',24,375,432,30,'',17,'#92999F',parent=wifi)
action_button('new_wifi',24,416,216,44,'添加网络',wifi,'#33224D')
action_button('wifi_previous',254,416,70,44,'‹',wifi)
bindtext('mg_wifi_page',326,421,60,30,'1/1',17,'#92999F',parent=wifi,text_align='center')
action_button('wifi_next',390,416,66,44,'›',wifi)
detail_wifi=management_page('wifi_detail','网络','wifi_detail_back')
bindtext('mg_wifi_name',24,108,432,60,'',17,parent=detail_wifi,text_align='center')
bindtext('mg_wifi_priority',24,176,432,36,'',17,'#92999F',parent=detail_wifi,text_align='center')
action_button('wifi_connect',24,234,432,56,'连接此网络',detail_wifi,'#33224D')
action_button('wifi_promote',24,304,432,56,'设为首选',detail_wifi)
action_button('wifi_delete',24,374,432,56,'删除网络',detail_wifi)
confirm=management_page('wifi_confirm','删除网络','delete_cancel')
bindtext('mg_delete_name',24,132,432,60,'',17,parent=confirm,text_align='center')
text(24,215,432,60,'删除后将不再自动连接此网络',17,'#92999F',parent=confirm,text_align='center')
action_button('delete_cancel_button',24,350,204,56,'取消',confirm)
action_button('delete_confirm',252,350,204,56,'删除',confirm,'#64272C')
enroll=management_page('enroll','添加终端','enrollment_back')
text(24,112,432,70,'在另一台 Mac 打开采集端\n输入下面的配对码',17,'#92999F',parent=enroll,text_align='center')
bindtext('enrollment_code',24,210,432,60,'...',29,parent=enroll,text_align='center')
text(24,282,432,36,'10 分钟有效，仅可使用一次',15,'#92999F',parent=enroll,text_align='center')
bindtext('mg_enrollment_status',24,330,432,45,'',17,'#92999F',parent=enroll,text_align='center')
action_button('regenerate_code',24,402,432,56,'重新生成',enroll,'#33224D')
portal=management_page('portal','添加 Wi-Fi','portal_back')
text(24,95,432,35,'用手机连接以下热点',17,'#92999F',parent=portal,text_align='center')
bindtext('mg_portal_ssid',24,137,432,35,'',17,parent=portal,text_align='center')
bindtext('mg_portal_password',24,183,432,35,'',17,parent=portal,text_align='center')
text(24,242,432,30,'然后在浏览器打开',15,'#92999F',parent=portal,text_align='center')
text(24,278,432,35,'http://192.168.4.1',17,parent=portal,text_align='center')
bindtext('mg_portal_message',24,326,432,60,'',17,'#92999F',parent=portal,text_align='center')
action_button('close_portal',24,402,432,56,'关闭热点',portal,'#33224D')
scan=management_page('wifi_scan','附近 Wi-Fi','scan_back')
for i in range(5):
 b=action_button('scan_row'+str(i),24,77+i*57,432,52,'',scan)
 bindtext('mg_scan'+str(i),14,4,400,44,'',17,parent=b)
bindtext('mg_scan_status',24,365,432,32,'',17,parent=scan)
action_button('scan_previous',24,410,54,48,'‹',scan)
action_button('scan_next',86,410,54,48,'›',scan)
action_button('scan_refresh',150,410,130,48,'重新扫描',scan)
action_button('scan_manual',290,410,166,48,'手动输入',scan)
entry=management_page('wifi_entry','连接 Wi-Fi','entry_back')
bindtext('mg_entry_title',24,72,432,34,'',17,parent=entry)
bindtext('mg_entry_display',24,112,350,36,'',17,parent=entry)
show=action_button('entry_show',378,110,78,40,'',entry)
bindtext('mg_entry_toggle',0,0,78,40,'显示',17,parent=show,text_align='center')
bindtext('mg_entry_status',24,155,432,38,'',17,parent=entry)
keysets=['0123456789abcdefghijklmnopqrstuvwxyz ', '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ', '0123456789!"#$%&\'()*+,-./:;<=>?@[\\]^_`{|}~ ']
for mode,keys in enumerate(keysets):
 layer=box(0,199,480,225,'#000000',parent=entry,name='key_layer'+str(mode),bind='key_layer'+str(mode),bind_target='visible',hidden=mode!=0)
 for i,key in enumerate(keys):
  name=f'entry_key{mode}_{i:02}'
  b=obj('button',4+(i%10)*48,4+(i//10)*44,44,40,parent=layer,bg_color='#26364D',radius=5,name=name,callback=f'entry_key{i:02}')
  text(0,0,44,40,key if key!=' ' else '空格',17,parent=b,text_align='center')
action_button('entry_lower',4,427,65,46,'abc',entry)
action_button('entry_upper',73,427,65,46,'ABC',entry)
action_button('entry_symbols',142,427,75,46,'123#',entry)
action_button('entry_delete',221,427,92,46,'删除',entry)
action_button('entry_ok',317,427,159,46,'连接 / OK',entry,'#167D66')
# Created after the home control, before overlay root; home control remains below other pages.
voice=box(0,0,480,480,'#000000',name='voice_page',bind='voice_page',bind_target='visible',hidden=True)
action_button('voice_back',16,12,64,52,'‹',voice)
bindtext('mg_voice_target',96,22,290,36,'语音输入',17,parent=voice)
bindtext('mg_voice_status',24,78,432,96,'点击麦克风开始',17,'#92999F',parent=voice,text_align='center')
# Microphone and meter are native geometry; the Mac is the only text editor.
mic=obj('button',170,175,140,150,bg_color='#7028E8',radius=50,parent=voice,name='voice_record',callback='voice_record',enabled=True)
box(56,20,28,66,'#FFFFFF',radius=14,parent=mic)
box(42,60,7,36,'#FFFFFF',radius=3,parent=mic)
box(91,60,7,36,'#FFFFFF',radius=3,parent=mic)
box(45,93,50,7,'#FFFFFF',radius=3,parent=mic)
box(67,99,7,22,'#FFFFFF',radius=3,parent=mic)
box(54,118,32,6,'#FFFFFF',radius=3,parent=mic)
for i in range(9):
 h=18+(4-abs(i-4))*7
 box(176+i*15,354-h//2,7,h,'#7028E8',parent=voice,radius=3,name=f'voice_bar{i}',bind=f'voice_bar{i}',bind_target='visible',hidden=True)
b=obj('button',24,405,204,58,bg_color='#21252B',radius=14,parent=voice,name='voice_delete',enabled=True,events=[{'event':'press','action':'call','target_name':'voice_delete_press','arg':0},{'event':'long','action':'call','target_name':'voice_clear','arg':0},{'event':'release','action':'call','target_name':'voice_delete_release','arg':0}])
text(0,0,204,58,'删除',18,parent=b,text_align='center')
action_button('voice_send',252,405,204,58,'发送',voice,'#167D66')
scene=dict(screen='pet',w=480,h=480,screen_bg='#000000',font='fonts/NotoSansSC.ttf',default_font_size=18,swipe=False,objects=objects)
assert not any(o['type']=='image' or 'image' in o for o in objects)
Path('components/app_pet/ui/main.json').write_text(json.dumps(scene,ensure_ascii=False,indent=2)+'\n')
