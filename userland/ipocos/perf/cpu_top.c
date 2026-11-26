/* SPDX-License-Identifier: MIT
 * cpu_top: where the CPU goes while the desktop is used (trial 100).
 *
 * Trial 99: with the CPU at its top rows while touching (3.2/3.35 GHz), the big cores were still 86-89% busy and frames
 * came late to the refresh. Every 5 s, for 5 minutes: the six processes that took the most CPU time in the window (in %
 * of one core), and the five busiest threads of the two busiest processes, by name. Built with -include say_printf.h
 * so its lines reach the phone's log; sys/proc_info.h comes from the kernel tree at build time (-idirafter), not copied.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

#include <mach/mach_time.h>
#include <sys/proc_info.h>

int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);
kern_return_t task_for_pid(mach_port_t target, int pid, mach_port_t *task);

/*
 * (trial 101) Where the always-busy processes spin. Trial 100: launchd.darwin took 72-75% of a core and mediaserverd
 * 47-51% all the time, at rest too. Every 100 ms, each RUNNING thread of theirs is stopped for a look: its PC as the
 * mapping it lies in (the file's last name, the offset in the mapping) and x16, the system call number on arm64 when
 * the thread is in one (negative: a Mach trap). Said every 5 s, the commonest first.
 */
static const char *const watched[] = { "launchd.darwin", "mediaserverd", "vmd" };   /* (trial 104) vmd: 86-98% in trial 102 */
#define WATCHED 3
#define WHERE_KEYS 64
struct where { char key[120]; unsigned n; };
static struct where wheres[WATCHED][WHERE_KEYS];
static unsigned nwheres[WATCHED], looks[WATCHED];
static int watched_pid[WATCHED];

static void where_note(int w, const char *key)
{
	for (unsigned i = 0; i < nwheres[w]; i++) if (!strcmp(wheres[w][i].key, key)) { wheres[w][i].n++; return; }
	if (nwheres[w] < WHERE_KEYS) { snprintf(wheres[w][nwheres[w]].key, sizeof(wheres[w][0].key), "%s", key); wheres[w][nwheres[w]++].n = 1; }
}

/* the first printable strings (5 characters or more) of `len` bytes at `at` in `task`, into out */
static void strings_at(mach_port_t task, uint64_t at, char *out, size_t size)
{
	vm_offset_t data = 0;
	mach_msg_type_number_t got = 0;
	out[0] = 0;
	if (!at || vm_read(task, (vm_address_t)at, 768, &data, &got) != KERN_SUCCESS) return;
	const unsigned char *b = (const unsigned char *)data;
	size_t n = 0;
	for (unsigned i = 0; i < got && n + 2 < size;) {
		unsigned j = i;
		while (j < got && b[j] >= 0x20 && b[j] < 0x7f) j++;
		if (j - i >= 5) {
			unsigned len = j - i;
			if (len > 40) len = 40;
			n += (size_t)snprintf(out + n, size - n, "%s%.*s", n ? "," : "", (int)len, (const char *)b + i);
		}
		i = j + 1;
	}
	vm_deallocate(mach_task_self(), data, got);
}

static void where_look(int w)
{
	mach_port_t task = MACH_PORT_NULL;
	if (watched_pid[w] <= 0 || task_for_pid(mach_task_self(), watched_pid[w], &task) != KERN_SUCCESS) return;
	thread_act_array_t list;
	mach_msg_type_number_t count = 0;
	if (task_threads(task, &list, &count) == KERN_SUCCESS) {
		for (unsigned i = 0; i < count; i++) {
			struct thread_basic_info info;
			mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
			if (thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&info, &n) == KERN_SUCCESS && info.run_state == TH_STATE_RUNNING) {
				arm_thread_state64_t st;
				mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
				if (thread_get_state(list[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS) {
					uint64_t pc = (uint64_t)arm_thread_state64_get_pc(st);
					struct proc_regionwithpathinfo r;
					char key[120];
					const char *file = "?";
					uint64_t offset = pc;
					if (proc_pidinfo(watched_pid[w], PROC_PIDREGIONPATHINFO, pc, &r, sizeof(r)) == (int)sizeof(r)) {
						const char *slash = strrchr(r.prp_vip.vip_path, '/');
						file = slash ? slash + 1 : (r.prp_vip.vip_path[0] ? r.prp_vip.vip_path : "anon");
						offset = pc - r.prp_prinfo.pri_address;
					}
					int64_t call = (int64_t)st.__x[16];
					if (call == -47) {
						/* mach_msg2_trap(data, options, bits|send size, remote|local port, voucher|id, descriptors|receive name,
						 * receive size|priority, timeout): what is sent or awaited, the message's id, the time allowed */
						/* what it sends, as text (a service's name in a lookup): the message, or the one its vector names */
						char text[48] = "";
						if (st.__x[1] & 1) {
							strings_at(task, st.__x[0], text, sizeof(text));
							if (!text[0]) {
								uint64_t first = 0;
								vm_offset_t d = 0;
								mach_msg_type_number_t g = 0;
								if (vm_read(task, (vm_address_t)st.__x[0], 8, &d, &g) == KERN_SUCCESS) {
									memcpy(&first, (const void *)d, 8);
									vm_deallocate(mach_task_self(), d, g);
									strings_at(task, first, text, sizeof(text));
								}
							}
						}
						snprintf(key, sizeof(key), "msg2 opt=%#llx id=%#llx tmo=%llu %s",
						    (unsigned long long)(st.__x[1] & 0xffff), (unsigned long long)(st.__x[4] >> 32),
						    (unsigned long long)st.__x[7], text);
					} else if (call == 368 || call == 375) {
						snprintf(key, sizeof(key), "%s x0=%#llx x1=%#llx x2=%#llx", call == 368 ? "workq_kernreturn" : "kevent_id",
						    (unsigned long long)st.__x[0], (unsigned long long)st.__x[1], (unsigned long long)st.__x[2]);
					} else {
						snprintf(key, sizeof(key), "%.40s+%#llx x16=%lld", file, (unsigned long long)offset, (long long)call);
					}
					where_note(w, key);
				}
				looks[w]++;
			}
			mach_port_deallocate(mach_task_self(), list[i]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof(list[0]));
	}
	mach_port_deallocate(mach_task_self(), task);
}

static void where_say(void)
{
	for (int w = 0; w < WATCHED; w++) {
		if (!looks[w]) continue;
		char line[640];
		int n = snprintf(line, sizeof(line), "cpu_top:   %s running, %u looks:", watched[w], looks[w]);
		for (int k = 0; k < 4; k++) {
			unsigned best = 0;
			int at = -1;
			for (unsigned i = 0; i < nwheres[w]; i++) if (wheres[w][i].n > best) { best = wheres[w][i].n; at = (int)i; }
			if (at < 0) break;
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %s *%u;", wheres[w][at].key, best);
			wheres[w][at].n = 0;
		}
		printf("%s\n", line);
		nwheres[w] = 0;
		looks[w] = 0;
	}
}

#define PIDS 2048
#define THREADS 256
#define WINDOW_S 5

struct seen { int pid; uint64_t total; };
static struct seen procs[PIDS];
static int nprocs;
struct tseen { uint64_t id; uint64_t total; };
static struct tseen threads[2][THREADS];
static int nthreads[2], tpid[2];

static uint64_t abs_to_ns(uint64_t t)
{
	static mach_timebase_info_data_t tb;
	if (!tb.denom) mach_timebase_info(&tb);
	return t * tb.numer / tb.denom;
}

static uint64_t *last_total(int pid)
{
	for (int i = 0; i < nprocs; i++) if (procs[i].pid == pid) return &procs[i].total;
	if (nprocs == PIDS) return NULL;
	procs[nprocs].pid = pid;
	procs[nprocs].total = 0;
	return &procs[nprocs++].total;
}

/* the busiest threads of `pid` over the window, slot `k` keeping their totals */
static void say_threads(int k, int pid, const char *name)
{
	uint64_t ids[THREADS];
	int got = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, ids, sizeof(ids));
	if (got <= 0) return;
	if (tpid[k] != pid) { nthreads[k] = 0; tpid[k] = pid; }
	struct { uint64_t delta; char name[64]; } top[5];
	memset(top, 0, sizeof(top));
	for (int i = 0; i < got / (int)sizeof(uint64_t); i++) {
		struct proc_threadinfo t;
		if (proc_pidinfo(pid, PROC_PIDTHREADINFO, ids[i], &t, sizeof(t)) != (int)sizeof(t)) continue;
		uint64_t total = t.pth_user_time + t.pth_system_time, before = 0;   /* (ns) */
		int at = -1;
		for (int j = 0; j < nthreads[k]; j++) if (threads[k][j].id == ids[i]) { at = j; break; }
		if (at < 0 && nthreads[k] < THREADS) { at = nthreads[k]++; threads[k][at].id = ids[i]; threads[k][at].total = total; continue; }
		if (at < 0) continue;
		before = threads[k][at].total;
		threads[k][at].total = total;
		uint64_t delta = total > before ? total - before : 0;
		for (int s = 0; s < 5; s++) if (delta > top[s].delta) {
			memmove(&top[s + 1], &top[s], sizeof(top[0]) * (4 - s));
			top[s].delta = delta;
			snprintf(top[s].name, sizeof(top[s].name), "%s", t.pth_name[0] ? t.pth_name : "?");
			break;
		}
	}
	char line[400];
	int n = snprintf(line, sizeof(line), "cpu_top:   %s threads:", name);
	for (int s = 0; s < 5 && top[s].delta; s++)
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %llu%%", top[s].name, (unsigned long long)(top[s].delta * 100 / (WINDOW_S * 1000000000ull)));
	printf("%s\n", line);
}

int main(void)
{
	static int pids[PIDS];
	for (int round = 0; round <= 60; round++) {
		int bytes = proc_listpids(1 /* PROC_ALL_PIDS */, 0, pids, sizeof(pids));
		struct { uint64_t delta; int pid; char name[40]; } top[6];
		memset(top, 0, sizeof(top));
		uint64_t all = 0;
		for (int i = 0; bytes > 0 && i < bytes / (int)sizeof(int); i++) {
			char pname[40] = "";
			proc_name(pids[i], pname, sizeof(pname));
			for (int w = 0; w < WATCHED; w++) if (!strcmp(pname, watched[w])) watched_pid[w] = pids[i];
			struct proc_taskinfo ti;
			if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) != (int)sizeof(ti)) continue;
			uint64_t total = abs_to_ns(ti.pti_total_user + ti.pti_total_system);
			uint64_t *last = last_total(pids[i]);
			if (!last) continue;
			uint64_t delta = round && total > *last ? total - *last : 0;
			*last = total;
			all += delta;
			for (int s = 0; s < 6; s++) if (delta > top[s].delta) {
				memmove(&top[s + 1], &top[s], sizeof(top[0]) * (5 - s));
				top[s].delta = delta;
				top[s].pid = pids[i];
				top[s].name[0] = 0;
				proc_name(pids[i], top[s].name, sizeof(top[s].name));
				if (!top[s].name[0]) snprintf(top[s].name, sizeof(top[s].name), "%s", pids[i] ? "?" : "kernel_task");
				break;
			}
		}
		if (round) {
			char line[400];
			int n = snprintf(line, sizeof(line), "cpu_top %d s: all %llu%% of a core;", WINDOW_S,
			    (unsigned long long)(all * 100 / (WINDOW_S * 1000000000ull)));
			for (int s = 0; s < 6 && top[s].delta; s++)
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %s(%d) %llu%%", top[s].name, top[s].pid,
				    (unsigned long long)(top[s].delta * 100 / (WINDOW_S * 1000000000ull)));
			printf("%s\n", line);
			for (int k = 0; k < 2 && top[k].delta; k++) say_threads(k, top[k].pid, top[k].name);
			where_say();
		} else {
			/* the first look only takes the totals; the threads' too */
			for (int k = 0; k < 2; k++) { nthreads[k] = 0; tpid[k] = 0; }
		}
		/* the window, looked into every 100 ms */
		for (int t = 0; t < WINDOW_S * 10; t++) {
			usleep(100000);
			for (int w = 0; w < WATCHED; w++) where_look(w);
		}
	}
	return 0;
}
