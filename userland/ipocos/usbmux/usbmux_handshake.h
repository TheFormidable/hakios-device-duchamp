/* SPDX-License-Identifier: MIT
 * Recover the port's incomplete receive frame after a host-side reconnection.
 * Both writes are VERSION requests, which intentionally begin a new mux session.
 * Padding is bounded by the existing device parser's maximum frame size.
 */
#ifndef HAKIOS_USBMUX_HANDSHAKE_H
#define HAKIOS_USBMUX_HANDSHAKE_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
static inline int husbmux_resynchronize(int(*send_frame)(void*,const uint8_t*,size_t),void *ctx){
 enum{limit=140000};
 uint8_t *p=calloc(1,limit);if(!p)return 0;
 p[4]=(uint8_t)(limit>>24);p[5]=(uint8_t)(limit>>16);p[6]=(uint8_t)(limit>>8);p[7]=(uint8_t)limit;p[11]=2;
 int ok=send_frame(ctx,p,limit);free(p);if(!ok)return 0;
 const uint8_t version[20]={0,0,0,0,0,0,0,20,0,0,0,2,0,0,0,0,0,0,0,0};
 return send_frame(ctx,version,sizeof(version));
}
#endif
