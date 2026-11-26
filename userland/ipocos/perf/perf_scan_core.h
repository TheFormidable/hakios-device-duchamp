/* SPDX-License-Identifier: MIT */
#ifndef HAKIOS_PERF_SCAN_CORE_H
#define HAKIOS_PERF_SCAN_CORE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct perf_scan {unsigned gpu_faults,process_faults,refusals,panics,expected_end;};
/* Record explicit evidence only. This classifier does not infer a root cause from low FPS. */
static inline unsigned perf_scan_line(struct perf_scan *s,const char *line) {
 if(strstr(line,"perf_scan:"))return 0;
 if(strstr(line,"panic(cpu")) {
  if(strstr(line,"mtkkeys:") && strstr(line,"the run ends here")){s->expected_end++;return 0;}
  s->panics++;return 1;
 }
 if(strstr(line,"metal_fault:") && (strstr(line,"signal ") || strstr(line,"uncaught "))){s->process_faults++;return 1;}
 const char *f=strstr(line,"GPU_FAULT_STATUS");
 if(f) {f+=strlen("GPU_FAULT_STATUS");while(*f==' ' || *f=='=' || *f==':')f++;
  char *end;unsigned long v=strtoul(f,&end,0);if(end!=f && v){s->gpu_faults++;return 1;}}
 if((strstr(line,"native-metal:") || strstr(line,"HakiosMetal[")) &&
    (strstr(line,"refused:") || strstr(line,"FALLBACK") || strstr(line,"could not be found in library"))){s->refusals++;return 1;}
 return 0;
}
#endif
