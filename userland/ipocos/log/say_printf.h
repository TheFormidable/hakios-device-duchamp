/* SPDX-License-Identifier: MIT
 * For a test program built unchanged: -include this, link say_printf.c. Its printf lines also go to the log that comes
 * back from the phone (kern.hakios.say), and so does whatever reaches stderr (Mesa's messages).
 */
#ifndef SAY_PRINTF_H
#define SAY_PRINTF_H
#ifdef __cplusplus
extern "C" {
#endif
int hakios_say_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
#define printf hakios_say_printf
#endif
