from pathlib import Path
import subprocess
P=Path(__file__).parents[1]/'vendor/esp-mosaico-vibe-legacy-v26.0.1/projects/codex_pet/components/app_network'

def test_real_profile_crud_priority_and_failover(tmp_path):
 c=tmp_path/'profiles.c';c.write_text('''#include "wifi_profiles.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 pet_wifi_profiles_t p={.version=1};
 assert(pet_wifi_put(&p,"Home","password1"));
 assert(pet_wifi_put(&p,"Office","password2"));
 assert(pet_wifi_next(&p,2,0)==1); /* skip unavailable higher-priority SSID */
 assert(pet_wifi_next(&p,3,1)==1); /* retry the next saved network */
 assert(pet_wifi_next(&p,3,3)==-1);
 assert(pet_wifi_promote(&p,1) && strcmp(p.profiles[0].ssid,"Office")==0);
 assert(pet_wifi_put(&p,"Office","updated123") && p.count==2);
 assert(!pet_wifi_put(&p,"bad","short"));
 for(unsigned i=2;i<8;++i){char ssid[16];snprintf(ssid,sizeof(ssid),"Network%u",i);assert(pet_wifi_put(&p,ssid,"password"));}
 assert(!pet_wifi_put(&p,"Ninth","password"));
 assert(pet_wifi_remove(&p,0) && p.count==7 && pet_wifi_find(&p,"Office")==-1);
 assert(pet_wifi_put(&p,"Open network","") && pet_wifi_valid(&p));
 p.count=9;assert(!pet_wifi_valid(&p));
 return 0;
}''');binary=tmp_path/'profiles'
 subprocess.run(['cc','-std=c11','-D_DARWIN_C_SOURCE','-Wall','-Wextra','-Werror','-I',str(P),str(c),str(P/'wifi_profiles.c'),'-o',str(binary)],check=True,capture_output=True)
 subprocess.run([str(binary)],check=True)
