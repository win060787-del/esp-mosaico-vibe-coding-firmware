"""Exercise the same sample scaler linked into production notification audio."""
from pathlib import Path
import subprocess

COMPONENT=Path(__file__).parents[1]/'vendor/esp-mosaico-vibe-legacy-v26.0.1/projects/codex_pet/components/app_notify'

def test_quiet_volume_curve_and_zero_output(tmp_path):
 source=tmp_path/'levels.c'
 source.write_text('''#include "audio_level.h"
#include <assert.h>
int main(void){
 assert(notification_scale_sample(9500,0)==0);
 assert(notification_scale_sample(-9500,0)==0);
 assert(notification_scale_sample(9500,50)==1187);
 assert(notification_scale_sample(-9500,50)==-1187);
 assert(notification_scale_sample(9500,100)==4750);
 assert(notification_scale_sample(9500,25)==296);
 for(unsigned v=1;v<=100;++v)
   assert(notification_scale_sample(9500,v)>=notification_scale_sample(9500,v-1));
 return 0;
}''')
 binary=tmp_path/'levels'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(COMPONENT/'include'),str(source),str(COMPONENT/'audio_level.c'),'-o',str(binary)],check=True,capture_output=True)
 subprocess.run([str(binary)],check=True)
