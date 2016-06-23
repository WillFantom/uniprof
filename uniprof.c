#include <sys/mman.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include "xenctrl.h"

#ifdef DEBUG
#define DBG(args...) printf(args)
#else
#define DBG(args...)
#endif /* DEBUG */

// big enough for 32 bit and 64 bit
typedef uint64_t guest_word_t;

typedef struct mapped_page {
	guest_word_t base; // page number, i.e. addr>>XC_PAGE_SHIFT
	unsigned long mfn;
	void *buf;
	struct mapped_page *next;
} mapped_page_t;

static bool verbose = false;
#define VERBOSE(args...) if (verbose) printf(args);

/* since some versions of sys/time.h do not include the
 * timespecadd/sub function, here's a macro (adapted from
 * the macros in sys/time.h) to do the job. */
#ifndef timespecadd
#define timespecadd(a, b, result)				\
do {								\
	(result)->tv_sec = (a)->tv_sec + (b)->tv_sec;		\
	(result)->tv_nsec = (a)->tv_nsec + (b)->tv_nsec;	\
	if ((result)->tv_nsec >= 1000000000) {			\
		++(result)->tv_sec;				\
		(result)->tv_nsec -= 1000000000;		\
	}							\
} while (0)
#endif /* timespecadd */
#ifndef timespecsub
#define timespecsub(a, b, result)				\
do {								\
	(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;		\
	(result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec;	\
	if ((result)->tv_nsec < 0) {				\
		--(result)->tv_sec;				\
		(result)->tv_nsec += 1000000000;		\
	}							\
} while (0)
#endif /* timespecsub */
#ifndef timespeccmp
#define timespeccmp(a, b, CMP)					\
(((a)->tv_sec == (b)->tv_sec) ?					\
((a)->tv_nsec CMP (b)->tv_nsec) :				\
((a)->tv_sec CMP (b)->tv_sec))
#endif /* timespeccmp */
/* invert a negative value (e.g., -300usecs = -1.000700000)
 * to a positive value (300 usecs = 0.000300000). This is
 * useful to print negative timespec values. */
#define timespecnegtopos(a, b)					\
do {								\
	(b)->tv_sec = -((a)->tv_sec+1);				\
	(b)->tv_nsec = 1000000000 - (a)->tv_nsec;		\
} while (0)

static unsigned long get_time_nsec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static void busywait(unsigned long nsecs)
{
	unsigned long deadline = get_time_nsec() + nsecs;
	do {
	} while (get_time_nsec() < deadline);
}

static void measure_overheads(struct timespec *gettime_overhead, struct timespec *minsleep, int rounds)
{
	int i;
	struct timespec before = { .tv_sec = 0, .tv_nsec = 0 };
	struct timespec after  = { .tv_sec = 0, .tv_nsec = 0 };
	struct timespec sleep  = { .tv_sec = 0, .tv_nsec = 0 };
	unsigned long long sleepsecs = 0, sleepnanosecs = 0, timesecs = 0, timenanosecs = 0;

	for (i=0; i<rounds; i++) {
		clock_gettime(CLOCK_MONOTONIC, &before);
		nanosleep(&sleep, NULL);
		clock_gettime(CLOCK_MONOTONIC, &after);
		timespecsub(&after, &before, &before);
		sleepsecs += before.tv_sec;
		sleepnanosecs += before.tv_nsec;
	}
	for (i=0; i<rounds; i++) {
		clock_gettime(CLOCK_MONOTONIC, &before);
		clock_gettime(CLOCK_MONOTONIC, &after);
		timespecsub(&after, &before, &before);
		timesecs += before.tv_sec;
		timenanosecs += before.tv_nsec;
	}
	gettime_overhead->tv_sec  = timesecs / rounds;
	gettime_overhead->tv_nsec = timenanosecs / rounds;
	minsleep->tv_sec          = (sleepsecs + timesecs) / rounds;
	minsleep->tv_nsec         = (sleepnanosecs + timenanosecs) / rounds;
}

static int get_word_size(xc_interface *xc_handle, int domid) {
	//TODO: support for HVM
	unsigned int guest_word_size;

	if (xc_domain_get_guest_width(xc_handle, domid, &guest_word_size))
		return -1;
	return guest_word_size;
}

static guest_word_t frame_pointer(vcpu_guest_context_any_t *vc, int wordsize) {
	// only possible word sizes are 4 and 8, everything else leads to an
	// early exit during initialization, since we can't handle it
	if (wordsize == 4)
		return vc->x32.user_regs.ebp;
	else
		return vc->x64.user_regs.rbp;
}

static guest_word_t instruction_pointer(vcpu_guest_context_any_t *vc, int wordsize) {
	//TODO: currently no support for real-mode 32 bit
	if (wordsize == 4)
		return vc->x32.user_regs.eip;
	else
		return vc->x64.user_regs.rip;
}

void *guest_to_host(xc_interface *xc_handle, int domid, int vcpu, guest_word_t gaddr) {
	static mapped_page_t *map_head = NULL;
	mapped_page_t *map_iter;
	mapped_page_t *new_item;
	guest_word_t base = gaddr & XC_PAGE_MASK;
	guest_word_t offset = gaddr & ~XC_PAGE_MASK;

	map_iter = map_head;
	while (map_iter != NULL) {
		if (base == map_iter->base)
			return map_iter->buf + offset;
		// preserve last item in map_iter by jumping out
		if (map_iter->next == NULL)
			break;
		map_iter = map_iter->next;
	}

	// no matching page found, we need to map a new one.
	// At this pointer, map_iter conveniently points to the last item.
	new_item = malloc(sizeof(mapped_page_t));
	if (new_item == NULL) {
		fprintf(stderr, "failed to allocate memory for page struct.\n");
		return NULL;
	}
	new_item->base = base;
	new_item->mfn = xc_translate_foreign_address(xc_handle, domid, vcpu, base);
	new_item->buf = xc_map_foreign_range(xc_handle, domid, XC_PAGE_SIZE, PROT_READ, new_item->mfn);
	VERBOSE("mapping new page %#"PRIx64"->%p\n", new_item->base, new_item->buf);
	if (new_item->buf == NULL) {
		fprintf(stderr, "failed to allocate memory mapping page.\n");
		return NULL;
	}
	new_item->next = NULL;
	if (map_head == NULL)
		map_head = new_item;
	else
		map_iter->next = new_item;
	return new_item->buf + offset;
}

void walk_stack(xc_interface *xc_handle, int domid, int vcpu, int wordsize, FILE *file) {
	int ret;
	guest_word_t fp, retaddr;
	void *hfp;
	vcpu_guest_context_any_t vc;

	DBG("tracing vcpu %d\n", vcpu);
	if ((ret = xc_vcpu_getcontext(xc_handle, domid, vcpu, &vc)) < 0) {
		printf("Failed to get context for VCPU %d, skipping trace. (ret=%d)\n", vcpu, ret);
		return;
	}

	// our first "return" address is the instruction pointer
	retaddr = instruction_pointer(&vc, wordsize);
	fp = frame_pointer(&vc, wordsize);
	while (fp != 0) {
		hfp = guest_to_host(xc_handle, domid, vcpu, fp);
		DBG("vcpu %d, fp = %#"PRIx64"->%p->%#"PRIx64", return addr = %#"PRIx64"\n",
				vcpu, fp, hfp, *((uint64_t*)hfp), retaddr);
		fprintf(file, "%#"PRIx64"\n", retaddr);
		// walk the frame pointers: new fp = content of old fp
		memcpy(&fp, hfp, wordsize);
		// and return address is always the next address on the stack
		memcpy(&retaddr, hfp+wordsize, wordsize);
		DBG("after: return addr = %#"PRIx64", fp = %#"PRIx64"\n", retaddr, fp);
	}
	fprintf(file, "1\n\n");
}

/**
 * returns 0 on success.
 */
int do_stack_trace(xc_interface *xc_handle, int domid, xc_dominfo_t *dominfo, int wordsize, FILE *file) {
	unsigned int vcpu;

	if (xc_domain_pause(xc_handle, domid) < 0) {
		fprintf(stderr, "Could not pause domid %d\n", domid);
		return -7;
	}
	for (vcpu = 0; vcpu <= dominfo->max_vcpu_id; vcpu++) {
		walk_stack(xc_handle, domid, vcpu, wordsize, file);
	}
	if (xc_domain_unpause(xc_handle, domid) < 0) {
		fprintf(stderr, "Could not unpause domid %d\n", domid);
		return -7;
	}
	return 0;
}

void write_file_header(FILE *f, int domid)
{
	char timestring[64];
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME_COARSE, &ts);
	strftime(timestring, 63, "%Y-%m-%d %H:%M:%S %Z (%z)", localtime(&ts.tv_sec));
	fprintf(f, "#unikernel stack tracer\n#tracing domid %d on %s\n\n", domid, timestring);
}

static void print_usage(char *name) {
	printf("usage:\n");
	printf("  %s [options] <outfile> <domid>\n\n", name);
	printf("options:\n");
	printf("  -F --frequency          Frequency of traces (in per second, default 1)\n");
	printf("  -T --time               How long to run the tracer (in seconds, default 1)\n");
	printf("  -M --missed-deadlines   Print a warning to STDERR whenever a deadline is\n");
	printf("                          missed. Note that this may exacerbate the problem,\n");
	printf("                          or it may treacherously appear to improve it,\n");
	printf("                          while it actually doesn't (due to timing quirks)\n");
	printf("  -v --verbose            Show some more informational output.\n");
	printf("  -h --help               Print this help message.\n");
}

int main(int argc, char **argv) {
	int domid, ret;
	FILE *outfile;
	xc_interface *xc_handle;
	xc_dominfo_t dominfo;
	int wordsize;
	const int measure_rounds = 100;
	struct timespec gettime_overhead, minsleep, sleep;
	struct timespec begin, end, ts;
	static const char *sopts = "hF:T:Mv";
	static const struct option lopts[] = {
		{"help",             no_argument,       NULL, 'h'},
		{"frequency",        required_argument, NULL, 'F'},
		{"time",             required_argument, NULL, 'T'},
		{"missed-deadlines", no_argument,       NULL, 'M'},
		{"verbose",          no_argument,       NULL, 'v'},
		{0, 0, 0, 0}
	};
	char *exename;
	int opt;
	unsigned int freq = 1;
	unsigned int time = 1;
	bool warn_missed_deadlines = false;
	unsigned int i,j;
	unsigned long long missed_deadlines = 0;

	while ((opt = getopt_long(argc, argv, sopts, lopts, NULL)) != -1) {
		switch(opt) {
			case 'h':
				print_usage(argv[0]);
				return 0;
			case 'F':
				freq = strtoul(optarg, NULL, 10);
				break;
			case 'T':
				time = strtoul(optarg, NULL, 10);
				break;
			case 'M':
				warn_missed_deadlines = true;
				break;
			case 'v':
				verbose = true;
				break;
			case '?':
				fprintf(stderr, "%s --help for usage\n", argv[0]);
				return -1;
		}
	}
	sleep.tv_sec = 0; sleep.tv_nsec = (1000000000/freq);
	exename = argv[0];
	argv += optind; argc -= optind;

	if (argc < 2 || argc > 3) {
		print_usage(exename);
		return -1;
	}

	domid = strtol(argv[0], NULL, 10);
	if (domid == 0) {
		fprintf(stderr, "invalid domid (unparseable domid string %s, or cannot trace dom0)\n", argv[0]);
		return -2;
	}

	if ((strlen(argv[1]) == 1) && (!(strncmp(argv[1], "-", 1)))) {
		outfile = stdout;
	}
	else {
		outfile = fopen(argv[1], "w");
		if (!outfile) {
			fprintf(stderr, "cannot open file %s: %s\n", argv[0], strerror(errno));
			return -3;
		}
	}

	xc_handle = xc_interface_open(0,0,0);
	if (xc_handle == NULL) {
		fprintf(stderr, "Cannot connect to the hypervisor. (Is this Xen?)\n");
		return -4;
	}

	ret = xc_domain_getinfo(xc_handle, domid, 1, &dominfo);
	if (ret < 0) {
		fprintf(stderr, "Could not access information for domid %d. (Does domid %d exist?)\n", domid, domid);
		return -5;
	}

	wordsize = get_word_size(xc_handle, domid);
	if (wordsize < 0) {
		fprintf(stderr, "Failed to retrieve word size for domid %d\n", domid);
		return -6;
	}
	else if ((wordsize != 8) && (wordsize != 4)) {
		fprintf(stderr, "Unexpected wordsize (%d) for domid %d, cannot trace.\n", wordsize, domid);
		return -6;
	}
	DBG("wordsize is %d\n", wordsize);

	// Initialization stuff: write file header, measure overhead of clock_gettime/minimal sleeptime, etc.
	write_file_header(outfile, domid);
	measure_overheads(&gettime_overhead, &minsleep, measure_rounds);
	DBG("gettime overhead is %ld.%09ld, minimal nanosleep() sleep time is %ld.%09ld\n",
		gettime_overhead.tv_sec, gettime_overhead.tv_nsec, minsleep.tv_sec, minsleep.tv_nsec);

	// The actual stack tracing loop
	for (i = 0; i < time; i++) {
		for (j = 0; j < freq; j++) {
			clock_gettime(CLOCK_MONOTONIC, &begin);
			ret = do_stack_trace(xc_handle, domid, &dominfo, wordsize, outfile);
			if (ret) {
				return ret;
			}
			clock_gettime(CLOCK_MONOTONIC, &end);
			timespecadd(&begin, &sleep, &ts);
			if (timespeccmp(&ts, &end, <)) {
				missed_deadlines++;
				// don't sleep, but warn if --missed-deadlines is set
				if (warn_missed_deadlines) {
					timespecsub(&ts, &end, &ts);
					timespecnegtopos(&ts, &ts);
					fprintf(stderr, "we're falling behind by %ld.%09ld!\n", ts.tv_sec, ts.tv_nsec);
				}
			}
			else {
				timespecsub(&ts, &end, &ts);
				if (timespeccmp(&ts, &minsleep, <)) {
					// we finished so close to the next deadline that nanosleep() cannot
					// reliably wake us up in time, so do busywaiting
					busywait(ts.tv_nsec);
				}
				else {
					nanosleep(&ts, NULL);
				}
			}
		}
	}

	if (missed_deadlines)
		printf("Missed %lld deadlines\n", missed_deadlines);

	return 0;
}
