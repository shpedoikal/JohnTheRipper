/* MS Office 2007 cracker patch for JtR. Hacked together during March of 2012 by
 * Dhiru Kholia <dhiru.kholia at gmail.com>
 *
 * OpenCL support by magnum.
 *
 * This software is Copyright (c) 2012, Dhiru Kholia <dhiru.kholia at gmail.com>
 * and Copyright (c) 2012, magnum and it is hereby released to the general public
 * under the following terms:
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 */

#include "sha.h"
#include <openssl/aes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "arch.h"
#include "misc.h"
#include "common.h"
#include "formats.h"
#include "params.h"
#include "options.h"
#include "base64.h"
#include "unicode.h"
#include "common-opencl.h"
#include "config.h"

#define PLAINTEXT_LENGTH	51
#define UNICODE_LENGTH		104 /* In octets, including 0x80 */
#define HASH_LOOPS		128 /* Lower figure gives less X hogging */

#define FORMAT_LABEL		"office2007-opencl"
#define FORMAT_NAME		"MS Office 2007"
#define ALGORITHM_NAME		"SHA1 AES OpenCL"
#define BENCHMARK_COMMENT	" (50,000 iterations)"
#define BENCHMARK_LENGTH	-1
#define BINARY_SIZE		0
#define BINARY_ALIGN		1
#define SALT_LENGTH		16
#define SALT_SIZE		sizeof(*cur_salt)
#define SALT_ALIGN		1
#define MIN_KEYS_PER_CRYPT	1
#define MAX_KEYS_PER_CRYPT	1

#define OCL_CONFIG		"office2007"

#define MIN(a, b)		(((a) > (b)) ? (b) : (a))
#define MAX(a, b)		(((a) > (b)) ? (a) : (b))

static struct fmt_tests tests[] = {
	{"$office$*2007*20*128*16*8b2c9e8c878844fc842012273be4bea8*aa862168b80d8c45c852696a8bb499eb*a413507fabe2d87606595f987f679ff4b5b4c2cd", "Password"},
	/* 2007-Default_myhovercraftisfullofeels_.docx */
	{"$office$*2007*20*128*16*91f095a1fd02595359fe3938fa9236fd*e22668eb1347957987175079e980990f*659f50b9062d36999bf3d0911068c93268ae1d86", "myhovercraftisfullofeels"},
	/* 2007-Default_myhovercraftisfullofeels_.dotx */
	{"$office$*2007*20*128*16*56ea65016fbb4eac14a6770b2dbe7e99*8cf82ce1b62f01fd3b2c7666a2313302*21443fe938177e648c482da72212a8848c2e9c80", "myhovercraftisfullofeels"},
	/* 2007-Default_myhovercraftisfullofeels_.xlsb */
	{"$office$*2007*20*128*16*fbd4cc5dab9b8e341778ddcde9eca740*3a040a9cef3d3675009b22f99718e39c*48053b27e95fa53b3597d48ca4ad41eec382e0c8", "myhovercraftisfullofeels"},
	/* 2007-Default_myhovercraftisfullofeels_.xlsm */
	{"$office$*2007*20*128*16*fbd4cc5dab9b8e341778ddcde9eca740*92bb2ef34ca662ca8a26c8e2105b05c0*0261ba08cd36a324aa1a70b3908a24e7b5a89dd6", "myhovercraftisfullofeels"},
	/* 2007-Default_myhovercraftisfullofeels_.xlsx */
	{"$office$*2007*20*128*16*fbd4cc5dab9b8e341778ddcde9eca740*46bef371486919d4bffe7280110f913d*b51af42e6696baa097a7109cebc3d0ff7cc8b1d8", "myhovercraftisfullofeels"},
	/* 2007-Default_myhovercraftisfullofeels_.xltx */
	{"$office$*2007*20*128*16*fbd4cc5dab9b8e341778ddcde9eca740*1addb6823689aca9ce400be8f9e55fc9*e06bf10aaf3a4049ffa49dd91cf9e7bbf88a1b3b", "myhovercraftisfullofeels"},
	{NULL}
};

static struct custom_salt {
	char unsigned osalt[SALT_LENGTH];
	char unsigned encryptedVerifier[16];
	char unsigned encryptedVerifierHash[32];
	int version;
	int verifierHashSize;
	int keySize;
	int saltSize;
} *cur_salt;

static int *cracked, any_cracked;
static unsigned int v_width = 1;	/* Vector width of kernel */

static char *saved_key;	/* Password encoded in UCS-2 */
static int *saved_len;	/* UCS-2 password length, in octets */
static char *saved_salt;
static unsigned char *key;	/* Output key from kernel */
static int new_keys;

static cl_mem cl_saved_key, cl_saved_len, cl_salt, cl_pwhash, cl_key;
static cl_mem pinned_saved_key, pinned_saved_len, pinned_salt, pinned_key;
static cl_kernel GenerateSHA1pwhash, Generate2007key;

static void create_clobj(size_t gws, struct fmt_main *self)
{
	int i;
	int bench_len = strlen(tests[0].plaintext) * 2;

	global_work_size = gws;
	gws *= v_width;
	self->params.max_keys_per_crypt = gws;

	pinned_saved_key = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, UNICODE_LENGTH * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating page-locked memory");
	cl_saved_key = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_ONLY, UNICODE_LENGTH * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating device memory");
	saved_key = (char*)clEnqueueMapBuffer(queue[ocl_gpu_id], pinned_saved_key, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, UNICODE_LENGTH * gws, 0, NULL, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error mapping page-locked memory saved_key");
	memset(saved_key, 0, UNICODE_LENGTH * gws);

	pinned_saved_len = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, sizeof(cl_int) * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating page-locked memory");
	cl_saved_len = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_ONLY, sizeof(cl_int) * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating device memory");
	saved_len = (int*)clEnqueueMapBuffer(queue[ocl_gpu_id], pinned_saved_len, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, sizeof(cl_int) * gws, 0, NULL, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error mapping page-locked memory saved_len");
	for (i = 0; i < gws; i++)
		saved_len[i] = bench_len;

	pinned_salt = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR, SALT_LENGTH, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating page-locked memory");
	cl_salt = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_ONLY, SALT_LENGTH, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating device memory");
	saved_salt = (char*) clEnqueueMapBuffer(queue[ocl_gpu_id], pinned_salt, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, SALT_LENGTH, 0, NULL, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error mapping page-locked memory saved_salt");
	memset(saved_salt, 0, SALT_LENGTH);

	cl_pwhash = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, sizeof(cl_uint) * 6 * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating device state buffer");

	pinned_key = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, 16 * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating page-locked memory");
	cl_key = clCreateBuffer(context[ocl_gpu_id], CL_MEM_READ_WRITE, 16 * gws, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error allocating device memory");
	key = (unsigned char*) clEnqueueMapBuffer(queue[ocl_gpu_id], pinned_key, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, 16 * gws, 0, NULL, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Error mapping page-locked memory key");
	memset(key, 0, 16 * gws);

	HANDLE_CLERROR(clSetKernelArg(GenerateSHA1pwhash, 0, sizeof(cl_mem), (void*)&cl_saved_key), "Error setting argument 0");
	HANDLE_CLERROR(clSetKernelArg(GenerateSHA1pwhash, 1, sizeof(cl_mem), (void*)&cl_saved_len), "Error setting argument 1");
	HANDLE_CLERROR(clSetKernelArg(GenerateSHA1pwhash, 2, sizeof(cl_mem), (void*)&cl_salt), "Error setting argument 2");
	HANDLE_CLERROR(clSetKernelArg(GenerateSHA1pwhash, 3, sizeof(cl_mem), (void*)&cl_pwhash), "Error setting argument 3");

	HANDLE_CLERROR(clSetKernelArg(crypt_kernel, 0, sizeof(cl_mem), (void*)&cl_pwhash), "Error setting argument 0");

	HANDLE_CLERROR(clSetKernelArg(Generate2007key, 0, sizeof(cl_mem), (void*)&cl_pwhash), "Error setting argument 0");
	HANDLE_CLERROR(clSetKernelArg(Generate2007key, 1, sizeof(cl_mem), (void*)&cl_key), "Error setting argument 1");

	cracked = mem_alloc(sizeof(*cracked) * gws);
}

static void release_clobj(void)
{
	HANDLE_CLERROR(clEnqueueUnmapMemObject(queue[ocl_gpu_id], pinned_key, key, 0, NULL, NULL), "Error Unmapping key");
	HANDLE_CLERROR(clEnqueueUnmapMemObject(queue[ocl_gpu_id], pinned_saved_key, saved_key, 0, NULL, NULL), "Error Unmapping saved_key");
	HANDLE_CLERROR(clEnqueueUnmapMemObject(queue[ocl_gpu_id], pinned_saved_len, saved_len, 0, NULL, NULL), "Error Unmapping saved_len");
	HANDLE_CLERROR(clEnqueueUnmapMemObject(queue[ocl_gpu_id], pinned_salt, saved_salt, 0, NULL, NULL), "Error Unmapping saved_salt");
	HANDLE_CLERROR(clFinish(queue[ocl_gpu_id]), "Error releasing memory mappings");

	HANDLE_CLERROR(clReleaseMemObject(pinned_key), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(pinned_saved_key), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(pinned_saved_len), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(pinned_salt), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(cl_key), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(cl_saved_key), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(cl_saved_len), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(cl_salt), "Release GPU buffer");
	HANDLE_CLERROR(clReleaseMemObject(cl_pwhash), "Release GPU buffer");

	MEM_FREE(cracked);
}

static void done(void)
{
	release_clobj();

	HANDLE_CLERROR(clReleaseKernel(crypt_kernel), "Release kernel");
	HANDLE_CLERROR(clReleaseKernel(GenerateSHA1pwhash), "Release kernel");
	HANDLE_CLERROR(clReleaseKernel(Generate2007key), "Release kernel");
	HANDLE_CLERROR(clReleaseProgram(program[ocl_gpu_id]), "Release Program");
}

static void clear_keys(void)
{
	memset(saved_key, 0, UNICODE_LENGTH * global_work_size * v_width);
	memset(saved_len, 0, sizeof(*saved_len) * global_work_size * v_width);
}

static void set_key(char *key, int index)
{
	UTF16 *utfkey = (UTF16*)&saved_key[index * UNICODE_LENGTH];

	/* convert key to UTF-16LE */
	saved_len[index] = enc_to_utf16(utfkey, PLAINTEXT_LENGTH, (UTF8*)key, strlen(key));
	if (saved_len[index] < 0)
		saved_len[index] = strlen16(utfkey);

	/* Prepare for GPU */
	utfkey[saved_len[index]] = 0x80;

	saved_len[index] <<= 1;

	new_keys = 1;
	//dump_stuff_msg("key buffer", &saved_key[index*UNICODE_LENGTH], UNICODE_LENGTH);
}

static void *get_salt(char *ciphertext)
{
	int i, length;
	char *ctcopy = strdup(ciphertext);
	char *keeptr = ctcopy, *p;
	ctcopy += 9;	/* skip over "$office$*" */
	cur_salt = mem_alloc_tiny(sizeof(struct custom_salt), MEM_ALIGN_WORD);
	p = strtok(ctcopy, "*");
	cur_salt->version = atoi(p);
	p = strtok(NULL, "*");
	cur_salt->verifierHashSize = atoi(p);
	p = strtok(NULL, "*");
	cur_salt->keySize = atoi(p);
	p = strtok(NULL, "*");
	cur_salt->saltSize = atoi(p);
	if (cur_salt->saltSize > SALT_LENGTH) {
		fprintf(stderr, "** error: salt longer than supported:\n%s\n", ciphertext);
		cur_salt->saltSize = SALT_LENGTH; /* will not work, but protects us from segfault */
	}
	p = strtok(NULL, "*");
	for (i = 0; i < cur_salt->saltSize; i++)
		cur_salt->osalt[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtok(NULL, "*");
	for (i = 0; i < 16; i++)
		cur_salt->encryptedVerifier[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtok(NULL, "*");
	length = strlen(p) / 2;
	for (i = 0; i < length; i++)
		cur_salt->encryptedVerifierHash[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	MEM_FREE(keeptr);
	return (void *)cur_salt;
}

static void set_salt(void *salt)
{
	cur_salt = (struct custom_salt *)salt;
	memcpy(saved_salt, cur_salt->osalt, SALT_LENGTH);
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], cl_salt, CL_FALSE, 0, SALT_LENGTH, saved_salt, 0, NULL, NULL), "failed in clEnqueueWriteBuffer saved_salt");
}

static cl_ulong gws_test(size_t gws, struct fmt_main *self)
{
	cl_ulong startTime, endTime;
	cl_command_queue queue_prof;
	cl_event Event[6];
	cl_int ret_code;
	int i;
	size_t scalar_gws = v_width * gws;
	size_t *lws = local_work_size ? &local_work_size : NULL;

	create_clobj(gws, self);
	queue_prof = clCreateCommandQueue(context[ocl_gpu_id], devices[ocl_gpu_id], CL_QUEUE_PROFILING_ENABLE, &ret_code);
	for (i = 0; i < scalar_gws; i++)
		set_key(tests[0].plaintext, i);
	set_salt(get_salt(tests[0].ciphertext));

	HANDLE_CLERROR(clEnqueueWriteBuffer(queue_prof, cl_saved_key, CL_TRUE, 0, UNICODE_LENGTH * scalar_gws, saved_key, 0, NULL, &Event[0]), "Failed transferring keys");
	HANDLE_CLERROR(clEnqueueWriteBuffer(queue_prof, cl_saved_len, CL_TRUE, 0, sizeof(int) * scalar_gws, saved_len, 0, NULL, &Event[1]), "Failed transferring lengths");

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, GenerateSHA1pwhash, 1, NULL, &scalar_gws, lws, 0, NULL, &Event[2]), "running kernel");

	//for (i = 0; i < 50000 / HASH_LOOPS - 1; i++)
	// warm-up run without measuring
	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, crypt_kernel, 1, NULL, &global_work_size, lws, 0, NULL, NULL), "running kernel");
	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, crypt_kernel, 1, NULL, &global_work_size, lws, 0, NULL, &Event[3]), "running kernel");

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue_prof, Generate2007key, 1, NULL, &global_work_size, lws, 0, NULL, &Event[4]), "running kernel");

	HANDLE_CLERROR(clEnqueueReadBuffer(queue_prof, cl_key, CL_TRUE, 0, 16 * scalar_gws, key, 0, NULL, &Event[5]), "failed in reading key back");

#if 1
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[0],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[1],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "keys xfer %.2f us, ", (endTime-startTime)/1000.);

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[2],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[2],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "1st kernel %.2f ms, ", (endTime-startTime)/1000000.);
#endif

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");

	if (options.verbosity > 3)
		fprintf(stderr, "loop kernel %.2f ms x %u = %.2f s, ", (endTime - startTime)/1000000., 50000/HASH_LOOPS, (50000/HASH_LOOPS) * (endTime - startTime) / 1000000000.);

	/* 200 ms duration limit for GCN to avoid ASIC hangs */
	if (amd_gcn(device_info[ocl_gpu_id]) && (endTime - startTime) > 200000000) {
		if (options.verbosity > 3)
			fprintf(stderr, "- exceeds 200 ms\n");
		clReleaseCommandQueue(queue_prof);
		release_clobj();
		return 0;
	}

#if 1
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[4],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[4],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "final kernel %.2f ms, ", (endTime-startTime)/1000000.);

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[5],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[5],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	if (options.verbosity > 3)
		fprintf(stderr, "result xfer %.2f us\n", (endTime-startTime)/1000.);
#endif

	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &startTime,
			NULL), "Failed to get profiling info");
	HANDLE_CLERROR(clGetEventProfilingInfo(Event[3],
			CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &endTime,
			NULL), "Failed to get profiling info");
	clReleaseCommandQueue(queue_prof);
	release_clobj();

	return (endTime - startTime) * (50000 / HASH_LOOPS - 1);
}

static void find_best_gws(struct fmt_main *self)
{
	int num;
	cl_ulong run_time, min_time = CL_ULONG_MAX;
	unsigned int SHAspeed, bestSHAspeed = 0, max_gws;
	int optimal_gws = get_kernel_preferred_multiple(ocl_gpu_id,
	                                                crypt_kernel);
	const int sha1perkey = 50004;
	unsigned long long int MaxRunTime = cpu(device_info[ocl_gpu_id]) ? 1000000000ULL : 10000000000ULL;

	max_gws = get_max_mem_alloc_size(ocl_gpu_id) / (UNICODE_LENGTH * v_width);

	if (options.verbosity > 3) {
		fprintf(stderr, "Calculating best keys per crypt (GWS) for LWS=%zd and max. %llu s duration.\n\n", local_work_size, MaxRunTime / 1000000000UL);
		fprintf(stderr, "Raw GPU speed figures including buffer transfers:\n");
	}

	for (num = optimal_gws; max_gws; num *= 2) {
		if (!(run_time = gws_test(num, self)))
			break;

		SHAspeed = sha1perkey * (1000000000UL * v_width * num / run_time);

		if (run_time < min_time)
			min_time = run_time;

		if (options.verbosity > 3)
			fprintf(stderr, "gws %6d%8llu c/s%14u sha1/s%8.2f sec per crypt_all()", num, (1000000000ULL * v_width * num / run_time), SHAspeed, (float)run_time / 1000000000.);
		else
			advance_cursor();

		if (((float)run_time / (float)min_time) < ((float)SHAspeed / (float)bestSHAspeed)) {
			if (options.verbosity > 3)
				fprintf(stderr, "!\n");
			bestSHAspeed = SHAspeed;
			optimal_gws = num;
		} else {
			if (run_time < MaxRunTime && SHAspeed > bestSHAspeed) {
				if (options.verbosity > 3)
					fprintf(stderr, "+\n");
				bestSHAspeed = SHAspeed;
				optimal_gws = num;
				continue;
			}
			if (options.verbosity > 3)
				fprintf(stderr, "\n");
			if (run_time >= MaxRunTime)
				break;
		}
	}
	global_work_size = optimal_gws;
}

static void init(struct fmt_main *self)
{
	cl_ulong maxsize, maxsize2, max_mem;
	char build_opts[64];
	static char valgo[32] = "";

	if ((v_width = opencl_get_vector_width(ocl_gpu_id,
	                                       sizeof(cl_int))) > 1) {
		/* Run vectorized kernel */
		snprintf(valgo, sizeof(valgo),
		         ALGORITHM_NAME " %ux", v_width);
		self->params.algorithm_name = valgo;
	}

	snprintf(build_opts, sizeof(build_opts),
	         "-DHASH_LOOPS=%u -DUNICODE_LENGTH=%u -DV_WIDTH=%u",
	         HASH_LOOPS,
	         UNICODE_LENGTH,
	         v_width);
	opencl_init("$JOHN/kernels/office2007_kernel.cl", ocl_gpu_id,
	            build_opts);

	/* Read LWS/GWS prefs from config or environment */
	opencl_get_user_preferences(OCL_CONFIG);

	// create kernel to execute
	GenerateSHA1pwhash = clCreateKernel(program[ocl_gpu_id], "GenerateSHA1pwhash", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel. Double-check kernel name?");
	crypt_kernel = clCreateKernel(program[ocl_gpu_id], "HashLoop", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel. Double-check kernel name?");
	Generate2007key = clCreateKernel(program[ocl_gpu_id], "Generate2007key", &ret_code);
	HANDLE_CLERROR(ret_code, "Error creating kernel. Double-check kernel name?");

	/* Enumerate GWS using *LWS=NULL (unless it was set explicitly) */
	if (!global_work_size)
		find_best_gws(self);

	/* Note: we ask for the kernels' max sizes, not the device's! */
	maxsize = get_kernel_max_lws(ocl_gpu_id, GenerateSHA1pwhash);
	maxsize2 = get_kernel_max_lws(ocl_gpu_id, crypt_kernel);
	if (maxsize2 < maxsize) maxsize = maxsize2;
	maxsize2 = get_kernel_max_lws(ocl_gpu_id, Generate2007key);
	if (maxsize2 < maxsize) maxsize = maxsize2;

#if 0
	/* Our use of local memory sets a limit for LWS */
	maxsize2 = get_local_memory_size(ocl_gpu_id) / (24 * v_width);
	while (maxsize > maxsize2)
		maxsize >>= 1;
#endif

	// Obey device limits
	max_mem = get_max_mem_alloc_size(ocl_gpu_id);
	while (global_work_size * v_width > max_mem / PLAINTEXT_LENGTH)
		global_work_size -= get_kernel_preferred_multiple(ocl_gpu_id,
		                                                  crypt_kernel);

	/* maxsize is the lowest figure from the three different kernels */
	if (!local_work_size) {
		if (getenv("LWS")) {
			/* LWS was explicitly set to 0 */
			create_clobj(global_work_size, self);
			opencl_find_best_workgroup_limit(self, maxsize,
			                                 ocl_gpu_id,
			                                 crypt_kernel);
			release_clobj();
		} else {
			if (cpu(device_info[ocl_gpu_id])) {
				if (get_platform_vendor_id(platform_id) == DEV_INTEL)
					local_work_size = MIN(maxsize, 8);
				else
					local_work_size = 1;
			} else
				local_work_size = MIN(maxsize, 64);
		}
	}

	while (local_work_size > maxsize)
		local_work_size >>= 1;

	self->params.min_keys_per_crypt = local_work_size * v_width;

	if (global_work_size < local_work_size)
		global_work_size = local_work_size;

	if (options.verbosity > 2)
		fprintf(stderr, "Local worksize (LWS) %d, Global worksize (GWS) %d\n", (int)local_work_size, (int)global_work_size);
	create_clobj(global_work_size, self);

	if (options.utf8)
		self->params.plaintext_length = MIN(125, 3 * PLAINTEXT_LENGTH);
}

static int ishex(char *q)
{
	while (atoi16[ARCH_INDEX(*q)] != 0x7F)
		q++;
	return !*q;
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *ctcopy, *ptr, *keeptr;
	int res;

	if (strncmp(ciphertext, "$office$*2007*", 14))
		return 0;
	if (!(ctcopy = strdup(ciphertext))) {
		fprintf(stderr, "Memory allocation failed in %s, unable to check if hash is valid!", FORMAT_LABEL);
		return 0;
	}
	keeptr = ctcopy;
	ctcopy += 15;
	if (!(ptr = strtok(ctcopy, "*"))) /* hash size or iterations */
		goto error;
	if (!(ptr = strtok(NULL, "*")))
		goto error;
	if (strncmp(ptr, "128", 3) && strncmp(ptr, "256", 3)) /* key size */
		goto error;
	if (!(ptr = strtok(NULL, "*"))) /* salt size */
		goto error;
	res = atoi(ptr);
	if (res != 16) /* can we handle other values? */
		goto error;
	if (!(ptr = strtok(NULL, "*"))) /* salt */
		goto error;
	if (strlen(ptr) != res * 2)
		goto error;
	if (!ishex(ptr))
		goto error;
	if (!(ptr = strtok(NULL, "*"))) /* encrypted verifier */
		goto error;
	if (!ishex(ptr))
		goto error;
	if (!(ptr = strtok(NULL, "*"))) /* encrypted verifier hash */
		goto error;
	if (!ishex(ptr))
		goto error;
	if (strlen(ptr) > 64)
		goto error;
	if ((ptr = strtok(NULL, "*")))
		goto error;

	MEM_FREE(keeptr);
	return 1;
error:
	MEM_FREE(keeptr);
	return 0;
}

static inline int PasswordVerifier(unsigned char *key)
{
	unsigned char decryptedVerifier[16];
	unsigned char decryptedVerifierHash[16];
	AES_KEY akey;
	SHA_CTX ctx;
	unsigned char checkHash[20];

	memset(&akey, 0, sizeof(AES_KEY));
	if(AES_set_decrypt_key(key, 128, &akey) < 0) {
		fprintf(stderr, "AES_set_decrypt_key failed!\n");
		return 0;
	}
	AES_ecb_encrypt(cur_salt->encryptedVerifier, decryptedVerifier, &akey, AES_DECRYPT);
	memset(&akey, 0, sizeof(AES_KEY));
	if(AES_set_decrypt_key(key, 128, &akey) < 0) {
		fprintf(stderr, "AES_set_decrypt_key failed!\n");
		return 0;
	}
	AES_ecb_encrypt(cur_salt->encryptedVerifierHash, decryptedVerifierHash, &akey, AES_DECRYPT);

	/* find SHA1 hash of decryptedVerifier */
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, decryptedVerifier, 16);
	SHA1_Final(checkHash, &ctx);

	return !memcmp(checkHash, decryptedVerifierHash, 16);
}

static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int index;
	size_t gws, scalar_gws;

	gws = ((count + (v_width * local_work_size - 1)) / (v_width * local_work_size)) * local_work_size;
	scalar_gws = gws * v_width;

	if (any_cracked) {
		memset(cracked, 0, global_work_size * sizeof(*cracked));
		any_cracked = 0;
	}

	if (new_keys) {
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], cl_saved_key, CL_FALSE, 0, UNICODE_LENGTH * scalar_gws, saved_key, 0, NULL, NULL), "failed in clEnqueueWriteBuffer saved_key");
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[ocl_gpu_id], cl_saved_len, CL_FALSE, 0, sizeof(int) * scalar_gws, saved_len, 0, NULL, NULL), "failed in clEnqueueWriteBuffer saved_len");
		new_keys = 0;
	}

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[ocl_gpu_id], GenerateSHA1pwhash, 1, NULL, &scalar_gws, &local_work_size, 0, NULL, firstEvent), "failed in clEnqueueNDRangeKernel");

	for (index = 0; index < 50000 / HASH_LOOPS; index++) {
		HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[ocl_gpu_id], crypt_kernel, 1, NULL, &gws, &local_work_size, 0, NULL, NULL), "failed in clEnqueueNDRangeKernel");
		HANDLE_CLERROR(clFinish(queue[ocl_gpu_id]), "Error running loop kernel");
		opencl_process_event();
	}

	HANDLE_CLERROR(clEnqueueNDRangeKernel(queue[ocl_gpu_id], Generate2007key, 1, NULL, &gws, &local_work_size, 0, NULL, lastEvent), "failed in clEnqueueNDRangeKernel");

	// read back aes key
	HANDLE_CLERROR(clEnqueueReadBuffer(queue[ocl_gpu_id], cl_key, CL_TRUE, 0, 16 * scalar_gws, key, 0, NULL, NULL), "failed in reading key back");

#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (index = 0; index < count; index++)
		if (PasswordVerifier(&key[index*16]))
			any_cracked = cracked[index] = 1;

	return count;
}

static int cmp_all(void *binary, int count)
{
	return any_cracked;
}

static int cmp_one(void *binary, int index)
{
	return cracked[index];
}

static int cmp_exact(char *source, int index)
{
	return 1;
}

static char *get_key(int index)
{
	UTF16 buf[PLAINTEXT_LENGTH + 1];

	memcpy(buf, &saved_key[index * UNICODE_LENGTH], saved_len[index]);
	buf[saved_len[index] >> 1] = 0;
	return (char*)utf16_to_enc(buf);
}

struct fmt_main fmt_opencl_office2007 = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		BINARY_ALIGN,
		SALT_SIZE,
		SALT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_UNICODE | FMT_UTF8 | FMT_OMP,
		tests
	}, {
		init,
		done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		fmt_default_binary,
		get_salt,
		fmt_default_source,
		{
			fmt_default_binary_hash
		},
		fmt_default_salt_hash,
		set_salt,
		set_key,
		get_key,
		clear_keys,
		crypt_all,
		{
			fmt_default_get_hash
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};
