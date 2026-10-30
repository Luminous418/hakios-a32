/*
 * mali_job_test: one job through the A32's RedwoodMali service, from a program.
 *
 * Opens the IOKit service the kernel's Mali driver starts (iokit/Kernel/hakios.mali.cpp),
 * asks for the GPU's ID, makes a 4 KB buffer, maps it, writes into it the same "store
 * 0xc0ffee01" job the kernel's bring-up runs (bsd/dev/hakios.mali.c), submits it to the GPU
 * and reads the word back. The result goes to kern.hakios.say, which the kernel repeats in
 * its log once a minute.
 *
 * Build (in the common tree, with the iOS SDK and ld64):
 *   xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 \
 *       -framework IOKit -framework CoreFoundation -o mali_job_test mali_job_test.c
 *
 * Run it from a boot job list (RC_JOBS=rc.jobs.malitest, with the program in EXTRA).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>

/* Linux's include/uapi/drm/panfrost_drm.h, as hakios.mali.cpp implements it. */
enum {
	kPanfrostSubmit   = 0x40,
	kPanfrostCreateBO = 0x42,
	kPanfrostGetParam = 0x44,
	kRedwoodWait      = 0x7d,
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};

struct redwood_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

static void
say(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	(void)sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
}

int
main(void)
{
	io_service_t service;
	io_connect_t connection;
	struct drm_panfrost_get_param param = { 0 };
	struct drm_panfrost_create_bo create;
	struct drm_panfrost_submit submit;
	struct redwood_wait wait;
	mach_vm_address_t at = 0;
	mach_vm_size_t size = 0;
	uint64_t gpu, high, low;
	size_t out;
	kern_return_t kr;

	service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodMali"));
	if (service == IO_OBJECT_NULL) {
		say("mali_job_test: no RedwoodMali service");
		return 1;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (kr != KERN_SUCCESS) {
		say("mali_job_test: IOServiceOpen %#x", kr);
		return 1;
	}

	out = sizeof(param);
	param.param = 0;
	kr = IOConnectCallStructMethod(connection, kPanfrostGetParam, &param, sizeof(param), &param, &out);
	high = kr == KERN_SUCCESS ? param.value : 0;
	out = sizeof(param);
	param.param = 1;
	kr = IOConnectCallStructMethod(connection, kPanfrostGetParam, &param, sizeof(param), &param, &out);
	low = kr == KERN_SUCCESS ? param.value : 0;

	memset(&create, 0, sizeof(create));
	create.size = 4096;
	out = sizeof(create);
	kr = IOConnectCallStructMethod(connection, kPanfrostCreateBO, &create, sizeof(create), &create, &out);
	if (kr != KERN_SUCCESS) {
		say("mali_job_test: create %#x", kr);
		IOServiceClose(connection);
		return 1;
	}
	gpu = create.offset;

	out = sizeof(size);
	size = 4096;
	kr = IOConnectMapMemory64(connection, create.handle, mach_task_self(), &at, &size, kIOMapAnywhere);
	if (kr != KERN_SUCCESS) {
		say("mali_job_test: map %#x (handle %u)", kr, create.handle);
		IOServiceClose(connection);
		return 1;
	}

	{
		volatile uint32_t *job = (volatile uint32_t *)at;
		uint32_t word;

		memset((void *)at, 0, 4096);
		job[4] = (2u << 1) | 1u | (1u << 16);   /* write value; 64-bit pointers; job index 1 */
		job[8] = (uint32_t)(gpu + 0x800);
		job[9] = (uint32_t)((gpu + 0x800) >> 32);
		job[10] = 6;                            /* a 32-bit immediate */
		job[12] = 0xc0ffee01u;

		memset(&submit, 0, sizeof(submit));
		submit.jc = gpu;
		out = sizeof(submit);
		kr = IOConnectCallStructMethod(connection, kPanfrostSubmit, &submit, sizeof(submit), &submit, &out);
		if (kr != KERN_SUCCESS) {
			say("mali_job_test: submit %#x", kr);
			IOServiceClose(connection);
			return 1;
		}
		wait.seq = submit.jc;                   /* the service hands the job's number back in jc */
		wait.timeout_ns = 2000000000;
		out = sizeof(wait);
		kr = IOConnectCallStructMethod(connection, kRedwoodWait, &wait, sizeof(wait), &wait, &out);
		word = job[0x800 / 4];
		say("mali_job_test: GPU %04llx:%04llx, gpu address %#llx, job %llu, wait %#x, word %08x -- %s",
		    high, low, gpu, submit.jc, kr, word, word == 0xc0ffee01u ? "THE GPU RAN IT" : "NOT WRITTEN");
	}

	IOServiceClose(connection);
	return 0;
}
