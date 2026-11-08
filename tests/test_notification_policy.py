"""Compile and exercise the same de-duplication policy used by the device."""
from pathlib import Path
import subprocess

ROOT=Path(__file__).parents[1]
COMPONENT=ROOT/'vendor/esp-mosaico-vibe-legacy-v26.0.1/projects/codex_pet/components/app_notify'

def test_device_policy_boot_dedup_concurrent_completions_and_restart(tmp_path):
    source=tmp_path/'policy_test.c'
    source.write_text(r'''
#include "notification_policy.h"
#include <assert.h>
#include <string.h>
int main(void){
 notification_policy_t p={0};char a[65],b[65];memset(a,'a',64);memset(b,'b',64);a[64]=b[64]=0;
 assert(!notification_policy_is_new(&p,a,101));
 p.armed=true;p.baseline=100;
 assert(!notification_policy_is_new(&p,a,99));
 assert(!notification_policy_is_new(&p,a,100));
 assert(notification_policy_is_new(&p,a,101));
 /* A failed queue submission does not consume the event. */
 assert(notification_policy_is_new(&p,a,101));
 notification_policy_record(&p,a);
 assert(!notification_policy_is_new(&p,a,101));
 assert(notification_policy_is_new(&p,b,101));
 notification_policy_record(&p,b);
 assert(!notification_policy_is_new(&p,b,101));
 assert(!notification_policy_is_new(&p,"malformed",102));
 notification_policy_t rebooted={.armed=true,.baseline=200};
 assert(!notification_policy_is_new(&rebooted,a,101));
 assert(notification_policy_is_new(&rebooted,a,201));
 return 0;
}
''')
    binary=tmp_path/'policy_test'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(COMPONENT/'include'),str(source),str(COMPONENT/'notification_policy.c'),'-o',str(binary)],check=True,capture_output=True)
    subprocess.run([str(binary)],check=True)
