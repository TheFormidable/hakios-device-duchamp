/* SPDX-License-Identifier: MIT
 * Read-only, bounded on-device log scanner. Full evidence remains in run-log.bin.
 * Samples every 5s, reads at most 128KiB per sample, logs at most 4 new evidence
 * lines per sample and one cumulative summary. Never injects input or resets GPU.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include "perf_scan_core.h"
static void say(const char *s){sysctlbyname("kern.hakios.say",NULL,NULL,(void*)s,strlen(s));}
int main(void){
 uint64_t pos=0;struct perf_scan scan={0};char partial[1024];size_t used=0;unsigned beats=0,truncated=0;
 say("perf_scan: active; read-only; 5 s sampling; full evidence in run-log.bin; no automatic gestures\n");
 for(;;){unsigned shown=0;
  for(unsigned chunk=0;chunk<8;chunk++){
   char buf[16384];size_t len=sizeof(buf);
   if(sysctlbyname("kern.hakios.log_copy",buf,&len,&pos,sizeof(pos)) || len<=sizeof(pos))break;
   uint64_t next;memcpy(&next,buf,sizeof(next));
   if(next<pos){used=0;truncated++;}pos=next;
   for(size_t i=sizeof(next);i<len;i++){
    if(buf[i]=='\n'){
     partial[used]=0;
     if(perf_scan_line(&scan,partial) && shown++<4){char evidence[640];snprintf(evidence,sizeof(evidence),"perf_scan: evidence %.580s\n",partial);say(evidence);}
     used=0;
    }else if(used<sizeof(partial)-1)partial[used++]=buf[i];
   }
   if(len<sizeof(buf))break;
  }
  if(++beats%2==0){char line[256];snprintf(line,sizeof(line),"perf_scan: cumulative process_faults=%u gpu_faults=%u refusals=%u unexpected_panics=%u expected_end=%u discontinuities=%u; pacing/CPU/GPU/cache counters logged separately\n",scan.process_faults,scan.gpu_faults,scan.refusals,scan.panics,scan.expected_end,truncated);say(line);}
  usleep(5000000);
 }
}
