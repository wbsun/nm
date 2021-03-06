/*
 * Copyright (C) 2012 Luigi Rizzo. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * $Id: testlock.c 11469 2012-07-30 16:23:55Z luigi $
 *
 * Test program to study various concurrency issues.
 * Create multiple threads, possibly bind to cpus, and run a workload.
 */

#include <inttypes.h>
#include <sys/types.h>
#include <pthread.h>	/* pthread_* */

#if defined(__APPLE__)

#include <libkern/OSAtomic.h>
#define atomic_add_int(p, n) OSAtomicAdd32(n, (int *)p)
#define	atomic_cmpset_32(p, o, n)	OSAtomicCompareAndSwap32(o, n, (int *)p)

#elif defined(linux)

#include <time.h>

int atomic_cmpset_32(volatile uint32_t *p, uint32_t old, uint32_t new)
{
	int ret = *p == old;
	*p = new;
	return ret;
}

#if defined(HAVE_GCC_ATOMICS)
int atomic_add_int(volatile int *p, int v)
{
        return __sync_fetch_and_add(p, v);
}
#else
inline
uint32_t atomic_add_int(uint32_t *p, int v)
{
        __asm __volatile (
        "       lock   xaddl   %0, %1 ;        "
        : "+r" (v),                     /* 0 (result) */
          "=m" (*p)                     /* 1 */
        : "m" (*p));                    /* 2 */
        return (v);
}
#endif

#else /* FreeBSD */
#include <sys/param.h>
#include <machine/atomic.h>
#include <pthread_np.h>	/* pthread w/ affinity */

#if __FreeBSD_version > 500000
#include <sys/cpuset.h>	/* cpu_set */
#if __FreeBSD_version > 800000
#define HAVE_AFFINITY
#endif

inline void prefetch (const void *x)
{
        __asm volatile("prefetcht0 %0" :: "m" (*(const unsigned long *)x));
}


#else /* FreeBSD 4.x */
int atomic_cmpset_32(volatile uint32_t *p, uint32_t old, uint32_t new)
{
	int ret = *p == old;
	*p = new;
	return ret;
}

#define PRIu64	"llu"
#endif /* FreeBSD 4.x */

#endif /* FreeBSD */

#include <signal.h>	/* signal */
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <inttypes.h>	/* PRI* macros */
#include <string.h>	/* strcmp */
#include <fcntl.h>	/* open */
#include <unistd.h>	/* getopt */


#include <sys/sysctl.h>	/* sysctl */
#include <sys/time.h>	/* timersub */

static inline int min(int a, int b) { return a < b ? a : b; }

#define ONE_MILLION	1000000
/* debug support */
#define ND(format, ...)			
#define D(format, ...)				\
	fprintf(stderr, "%s [%d] " format "\n",	\
	__FUNCTION__, __LINE__, ##__VA_ARGS__)

int verbose = 0;

#if 1//def MY_RDTSC
/* Wrapper around `rdtsc' to take reliable timestamps flushing the pipeline */ 
#define my_rdtsc(t)				\
	do {					\
		u_int __regs[4];		\
						\
		do_cpuid(0, __regs);		\
		(t) = rdtsc();			\
	} while (0)

static __inline void
do_cpuid(u_int ax, u_int *p)
{
	__asm __volatile("cpuid"
			 : "=a" (p[0]), "=b" (p[1]), "=c" (p[2]), "=d" (p[3])
			 :  "0" (ax) );
}

static __inline uint64_t
rdtsc(void)
{
	uint64_t rv;

	// XXX does not work on linux-64 bit
	__asm __volatile("rdtscp" : "=A" (rv) : : "%rax");
	return (rv);
}
#endif /* 1 */

struct targ;

/*** global arguments for all threads ***/
struct glob_arg {
	struct  {
		uint32_t	ctr[1024];
	} v __attribute__ ((aligned(256) ));
	int m_cycles;	/* million cycles */
	int nthreads;
	int cpus;
	int privs;	// 1 if has IO privileges
	int arg;	// microseconds in usleep
	char *test_name;
	void (*fn)(struct targ *);
	uint64_t scale;	// scaling factor
	char *scale_name;	// scaling factor
};

/*
 * Arguments for a new thread.
 */
struct targ {
	struct glob_arg *g;
	int		completed;
	u_int		*glob_ctr;
	uint64_t volatile count;
	struct timeval	tic, toc;
	int		me;
	pthread_t	thread;
	int		affinity;
};


static struct targ *ta;
static int global_nthreads;

/* control-C handler */
static void
sigint_h(int sig)
{
	int i;

	(void)sig;	/* UNUSED */
	for (i = 0; i < global_nthreads; i++) {
		/* cancel active threads. */
		if (ta[i].completed)
			continue;
		D("Cancelling thread #%d\n", i);
		pthread_cancel(ta[i].thread);
		ta[i].completed = 0;
	}
	signal(SIGINT, SIG_DFL);
}


/* sysctl wrapper to return the number of active CPUs */
static int
system_ncpus(void)
{
#ifdef linux
	return 1;
#else
	int mib[2] = { CTL_HW, HW_NCPU}, ncpus;
	size_t len = sizeof(mib);
	sysctl(mib, len / sizeof(mib[0]), &ncpus, &len, NULL, 0);
	D("system had %d cpus", ncpus);

	return (ncpus);
#endif
}

/*
 * try to get I/O privileges so we can execute cli/sti etc.
 */
int
getprivs(void)
{
	int fd = open("/dev/io", O_RDWR);
	if (fd < 0) {
		D("cannot open /dev/io, fd %d", fd);
		return 0;
	}
	return 1;
}

/* set the thread affinity. */
/* ARGSUSED */
#ifdef HAVE_AFFINITY
static int
setaffinity(pthread_t me, int i)
{
	cpuset_t cpumask;

	if (i == -1)
		return 0;

	/* Set thread affinity affinity.*/
	CPU_ZERO(&cpumask);
	CPU_SET(i, &cpumask);

	if (pthread_setaffinity_np(me, sizeof(cpuset_t), &cpumask) != 0) {
		D("Unable to set affinity");
		return 1;
	}
	return 0;
}
#endif


static void *
td_body(void *data)
{
	struct targ *t = (struct targ *) data;

#ifdef HAVE_AFFINITY
	if (0 == setaffinity(t->thread, t->affinity))
#endif
	{
		/* main loop.*/
		D("testing %d cycles", t->g->m_cycles);
		gettimeofday(&t->tic, NULL);
		t->g->fn(t);
		gettimeofday(&t->toc, NULL);
	}
	t->completed = 1;
	return (NULL);
}

void
test_sel(struct targ *t)
{
	int m;
	for (m = 0; m < t->g->m_cycles; m++) {
		fd_set r;
		struct timeval to = { 0, t->g->arg};
		FD_ZERO(&r);
		FD_SET(0,&r);
		// FD_SET(1,&r);
		select(1, &r, NULL, NULL, &to);
		t->count++;
	}
}

void
test_poll(struct targ *t)
{
	int m, ms = t->g->arg/1000;
	for (m = 0; m < t->g->m_cycles; m++) {
		struct pollfd x;
		x.fd = 0;
		x.events = POLLIN;
		poll(&x, 1, ms);
		t->count++;
	}
}

void
test_usleep(struct targ *t)
{
	int m;
	for (m = 0; m < t->g->m_cycles; m++) {
		usleep(t->g->arg);
		t->count++;
	}
}

void
test_cli(struct targ *t)
{
        int m, i;
	if (!t->g->privs) {	
		D("%s", "privileged instructions not available");
		return;
	}
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
			__asm __volatile("cli;");
			__asm __volatile("and %eax, %eax;");
			__asm __volatile("sti;");
			t->count++;
		}
        }
}

void
test_nop(struct targ *t)
{
        int m, i;
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
			__asm __volatile("nop;");
			__asm __volatile("nop; nop; nop; nop; nop;");
			//__asm __volatile("nop; nop; nop; nop; nop;");
			t->count++;
		}
	}
}

void
test_rdtsc1(struct targ *t)
{
        int m, i;
	uint64_t v;
	(void)v;
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
                	my_rdtsc(v);
			t->count++;
		}
        }
}

void
test_rdtsc(struct targ *t)
{
        int m, i;
	volatile uint64_t v;
	(void)v;
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
                	v = rdtsc();
			t->count++;
		}
        }
}

void
test_add(struct targ *t)
{
        int m, i;
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
                	t->glob_ctr[0] ++;
			t->count++;
		}
        }
}

void
test_atomic_add(struct targ *t)
{
        int m, i;
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
                	atomic_add_int(t->glob_ctr, 1);
			t->count++;
		}
        }
}

void
test_atomic_cmpset(struct targ *t)
{
        int m, i;
        for (m = 0; m < t->g->m_cycles; m++) {
		for (i = 0; i < ONE_MILLION; i++) {
		        atomic_cmpset_32(t->glob_ctr, m, i);
			t->count++;
		}
        }
}

void
test_time(struct targ *t)
{
        int m;
        for (m = 0; m < t->g->m_cycles; m++) {
#ifndef __APPLE__
		struct timespec ts;
		clock_gettime(t->g->arg, &ts);
#endif
		t->count++;
        }
}

void
test_gettimeofday(struct targ *t)
{
        int m;
	struct timeval ts;
        for (m = 0; m < t->g->m_cycles; m++) {
		gettimeofday(&ts, NULL);
		t->count++;
        }
}

static inline void
fast_bcopy(void *_src, void *_dst, int l)
{
	uint64_t *src = _src;
	uint64_t *dst = _dst;
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)       __builtin_expect(!!(x), 0)
	if (unlikely(l >= 1024)) {
		bcopy(src, dst, l);
		return;
	}
	for (; likely(l > 0); l-=64) {
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
#if 1
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
#endif
	}
}
	
#define HU	0x3ffff
static struct glob_arg huge[HU+1];
void
test_bcopy(struct targ *t)
{
        int m, len;
	len = t->g->arg;
	if (len > (int)sizeof(struct glob_arg))
		len = sizeof(struct glob_arg);
	D("copying %d bytes", len);
        for (m = 0; m < t->g->m_cycles; m++) {
		//bcopy(t->g, (void *)&huge[m & HU], len);
		fast_bcopy(t->g, (void *)&huge[m & HU], len);
		//memcpy((void *)&huge[m & HU], t->g, len);
		t->count+=1;
        }
}

struct entry {
	void (*fn)(struct targ *);
	char *name;
	uint64_t scale;
};
struct entry tests[] = {
	{ test_sel, "select", 1 },
	{ test_poll, "poll", 1 },
	{ test_usleep, "usleep", 1 },
	{ test_time, "time", 1 },
	{ test_gettimeofday, "gettimeofday", 1 },
	{ test_bcopy, "bcopy", 1 },
	{ test_add, "add", ONE_MILLION },
	{ test_nop, "nop", ONE_MILLION },
	{ test_atomic_add, "atomic-add", ONE_MILLION },
	{ test_cli, "cli", ONE_MILLION },
	{ test_rdtsc, "rdtsc", ONE_MILLION },	// unserialized
	{ test_rdtsc1, "rdtsc1", ONE_MILLION },	// serialized
	{ test_atomic_cmpset, "cmpset", ONE_MILLION },
	{ NULL, NULL, 0 }
};

static void
usage(void)
{
	const char *cmd = "test";
	int i;

	fprintf(stderr,
		"Usage:\n"
		"%s arguments\n"
		"\t-t threads		total threads\n"
		"\t-c cores		cores to use\n"
		"\t-a n			force affinity every n cores\n"
		"\t-A n			cache contention every n bytes\n"
		"\t-n cycles		(millions) of cycles\n"
		"\t-w report_ms		milliseconds between reports\n"
		"\t-m name		test name\n"
		"\t-N arg		usec for select/usleep\n"
		"",
		cmd);
	fprintf(stderr, "Available tests:\n");
	for (i = 0; tests[i].name; i++) {
		fprintf(stderr, "%12s\n", tests[i].name);
	}

	exit(0);
}


struct glob_arg g;
int
main(int argc, char **argv)
{
	int i, ch, report_interval, affinity, align;

	ND("g has size %d", (int)sizeof(g));
	report_interval = 250;	/* ms */
	affinity = 0;		/* no affinity */
	align = 0;		/* global variable */

	bzero(&g, sizeof(g));

	g.privs = getprivs();
	g.nthreads = 1;
	g.cpus = 1;
	g.m_cycles = 1000;	/* millions */

	while ( (ch = getopt(argc, argv, "A:a:m:n:w:c:t:vN:")) != -1) {
		switch(ch) {
		default:
			D("bad option %c %s", ch, optarg);
			usage();
			break;
		case 'A':	/* align */
			align = atoi(optarg);
			break;
		case 'a':	/* force affinity */
			affinity = atoi(optarg);
			break;
		case 'n':	/* cycles */
			g.m_cycles = atoi(optarg);
			break;
		case 'w':	/* report interval */
			report_interval = atoi(optarg);
			break;
		case 'c':
			g.cpus = atoi(optarg);
			break;
		case 't':
			g.nthreads = atoi(optarg);
			break;
		case 'm':
			g.test_name = optarg;
			break;
		case 'N':
			g.arg = atoi(optarg);
			break;

		case 'v':
			verbose++;
			break;
		}
	}
	argc -= optind;
	argv += optind;
	if (!g.test_name && argc > 0)
		g.test_name = argv[0];

	if (g.test_name) {
		for (i = 0; tests[i].name; i++) {
			if (!strcmp(g.test_name, tests[i].name)) {
				g.fn = tests[i].fn;
				g.scale = tests[i].scale;
				if (g.scale == ONE_MILLION)
					g.scale_name = "M";
				else if (g.scale == 1000)
					g.scale_name = "M";
				else {
					g.scale = 1;
					g.scale_name = "";
				}
				break;
			}
		}
	}
	if (!g.fn) {
		D("%s", "missing/unknown test name");
		usage();
	}
	i = system_ncpus();
	if (g.cpus < 0 || g.cpus > i) {
		D("%d cpus is too high, have only %d cpus", g.cpus, i);
		usage();
	}
	if (g.cpus == 0)
		g.cpus = i;
	if (g.nthreads < 1) {
		D("bad nthreads %d, using 1", g.nthreads);
		g.nthreads = 1;
	}
	i = sizeof(g.v.ctr) / g.nthreads*sizeof(g.v.ctr[0]);
	if (align < 0 || align > i) {
		D("bad align %d, max is %d", align, i);
		align = i;
	}

	/* Install ^C handler. */
	global_nthreads = g.nthreads;
	signal(SIGINT, sigint_h);

	ta = calloc(g.nthreads, sizeof(*ta));
	/*
	 * Now create the desired number of threads, each one
	 * using a single descriptor.
 	 */
	D("start %d threads on %d cores", g.nthreads, g.cpus);
	for (i = 0; i < g.nthreads; i++) {
		struct targ *t = &ta[i];
		bzero(t, sizeof(*t));
		t->g = &g;
		t->me = i;
		t->glob_ctr = &g.v.ctr[(i*align)/sizeof(g.v.ctr[0])];
		D("thread %d ptr %p", i, t->glob_ctr);
		t->affinity = affinity ? (affinity*i) % g.cpus : -1;
		if (pthread_create(&t->thread, NULL, td_body, t) == -1) {
			D("Unable to create thread %d", i);
			t->completed = 1;
		}
	}
	/* the main loop */

    {
	uint64_t my_count = 0, prev = 0;
	uint64_t count = 0;
	double delta_t;
	struct timeval tic, toc;

	gettimeofday(&toc, NULL);
	for (;;) {
		struct timeval now, delta;
		uint64_t pps;
		int done = 0;

		delta.tv_sec = report_interval/1000;
		delta.tv_usec = (report_interval%1000)*1000;
		select(0, NULL, NULL, NULL, &delta);
		gettimeofday(&now, NULL);
		timersub(&now, &toc, &toc);
		my_count = 0;
		for (i = 0; i < g.nthreads; i++) {
			my_count += ta[i].count;
			if (ta[i].completed)
				done++;
		}
		pps = toc.tv_sec* ONE_MILLION + toc.tv_usec;
		if (pps < 10000)
			continue;
		pps = (my_count - prev)*ONE_MILLION / pps;
		D("%" PRIu64 " %scycles/s scale %" PRIu64, pps/g.scale,
			g.scale_name, g.scale);
		prev = my_count;
		toc = now;
		if (done == g.nthreads)
			break;
	}

	timerclear(&tic);
	timerclear(&toc);
	for (i = 0; i < g.nthreads; i++) {
		pthread_join(ta[i].thread, NULL);

		if (ta[i].completed == 0)
			continue;

		/*
		 * Collect threads o1utput and extract information about
		 * how log it took to send all the packets.
		 */
		count += ta[i].count;
		if (!timerisset(&tic) || timercmp(&ta[i].tic, &tic, <))
			tic = ta[i].tic;
		if (!timerisset(&toc) || timercmp(&ta[i].toc, &toc, >))
			toc = ta[i].toc;
	}

	/* print output. */
	timersub(&toc, &tic, &toc);
	delta_t = toc.tv_sec + 1e-6* toc.tv_usec;
	D("total %8.6f seconds", delta_t);
    }

	return (0);
}
/* end of file */
