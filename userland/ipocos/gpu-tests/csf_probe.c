/* SPDX-License-Identifier: MIT
 * Read-only GPU capabilities plus a CPU buffer roundtrip; NOT a rendering test.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include "xf86drm.h"
#include "drm-uapi/panthor_drm.h"
int main(void){
 int fd=drmOpenWithType("panthor",NULL,DRM_NODE_RENDER);
 if(fd<0){printf("CSF transport unavailable: %s\n",strerror(errno));return 1;}
 struct drm_panthor_gpu_info gpu={0};
 struct drm_panthor_csif_info csif={0};
 struct drm_panthor_dev_query q={.type=DRM_PANTHOR_DEV_QUERY_GPU_INFO,.size=sizeof(gpu),.pointer=(uintptr_t)&gpu};
 if(drmIoctl(fd,DRM_IOCTL_PANTHOR_DEV_QUERY,&q))goto fail;
 q=(struct drm_panthor_dev_query){.type=DRM_PANTHOR_DEV_QUERY_CSIF_INFO,.size=sizeof(csif),.pointer=(uintptr_t)&csif};
 if(drmIoctl(fd,DRM_IOCTL_PANTHOR_DEV_QUERY,&q))goto fail;
 printf("GPU=%#x shaders=%#llx CSF groups=%u streams=%u registers=%u scoreboards=%u\n",gpu.gpu_id,(unsigned long long)gpu.shader_present,csif.csg_slot_count,csif.cs_slot_count,csif.cs_reg_count,csif.scoreboard_slot_count);
 struct drm_panthor_bo_create bo={.size=4096};
 if(drmIoctl(fd,DRM_IOCTL_PANTHOR_BO_CREATE,&bo))goto fail;
 struct drm_panthor_bo_mmap_offset map={.handle=bo.handle};
 if(drmIoctl(fd,DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET,&map))goto fail;
 volatile uint32_t*p=hp_drm_mmap(fd,map.offset,bo.size,PROT_READ|PROT_WRITE);
 if((void*)p==MAP_FAILED)goto fail;
 if(p[0]!=0){printf("BO was not zero initialized\n");hp_drm_munmap((void*)p,bo.size);drmClose(fd);return 2;}
 p[0]=0x53414d49;
 int valid=p[0]==0x53414d49;
 hp_drm_munmap((void*)p,bo.size);
 struct drm_gem_close close={.handle=bo.handle};
 if(drmIoctl(fd,DRM_IOCTL_GEM_CLOSE,&close))goto fail;
 printf("CPU buffer roundtrip=%s. No shader or GPU command submitted.\n",valid?"PASS":"FAIL");
 drmClose(fd);return valid?0:2;
fail:
 printf("CSF transport request failed: %s\n",strerror(errno));drmClose(fd);return 1;
}
