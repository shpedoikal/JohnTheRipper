/*
 * This file is part of John the Ripper password cracker,
 * Copyright (c) 1996-2003,2006,2010,2013 by Solar Designer
 *
 * ...with changes in the jumbo patch, by JimF and magnum.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted.
 *
 * There's ABSOLUTELY NO WARRANTY, express or implied.
 */

#define _XOPEN_SOURCE 500 /* for setitimer(2) and siginterrupt(3) */

#ifdef __ultrix__
#define __POSIX
#define _POSIX_SOURCE
#endif

#define NEED_OS_TIMER
#define NEED_OS_FORK
#include "os.h"

#if HAVE_WINDOWS_H
#define WIN32_SIGNAL_HANDLER
#define SIGALRM SIGFPE
#define SIGHUP SIGILL
#endif

#ifdef _SCO_C_DIALECT
#include <limits.h>
#endif
#include <stdio.h>
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#if HAVE_DOS_H
#include <dos.h>
#endif

#if HAVE_WINDOWS_H
#include <windows.h>
#endif

#include "arch.h"
#include "misc.h"
#include "params.h"
#include "tty.h"
#include "options.h"
#include "config.h"
#include "options.h"
#include "bench.h"
#include "john.h"
#include "status.h"
#include "signals.h"
#ifdef HAVE_MPI
#include "john-mpi.h"
#endif

volatile int event_pending = 0, event_reload = 0;
volatile int event_abort = 0, event_save = 0, event_status = 0;
volatile int event_ticksafety = 0;

volatile int timer_reload = 0, timer_abort = 0, timer_status = 0;
static int timer_save_interval, timer_save_value;
static clock_t timer_ticksafety_interval, timer_ticksafety_value;

#if !OS_TIMER

#include <time.h>
#if !defined (__MINGW32__) && !defined (_MSC_VER)
#include <sys/times.h>
#endif

static clock_t timer_emu_interval = 0;
static unsigned int timer_emu_count = 0, timer_emu_max = 0;

void sig_timer_emu_init(clock_t interval)
{
	timer_emu_interval = interval;
	timer_emu_count = 0; timer_emu_max = 0;
}

void sig_timer_emu_tick(void)
{
	static clock_t last = 0;
	clock_t current;
#if !defined (__MINGW32__) && !defined (_MSC_VER)
	struct tms buf;
#endif

	if (++timer_emu_count < timer_emu_max) return;

#if defined (__MINGW32__) || defined (_MSC_VER)
	current = clock();
#else
	current = times(&buf);
#endif

	if (!last) {
		last = current;
		return;
	}

	if (current - last < timer_emu_interval && current >= last) {
		timer_emu_max += timer_emu_max + 1;
		return;
	}

	last = current;
	timer_emu_count = 0;
	timer_emu_max >>= 1;

	raise(SIGALRM);
}

#endif

static void sig_install_update(void);

static void sig_handle_update(int signum)
{
	event_save = event_pending = 1;

#ifndef SA_RESTART
	sig_install_update();
#endif
}

static void sig_install_update(void)
{
#ifdef SA_RESTART
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handle_update;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &sa, NULL);
#else
	signal(SIGHUP, sig_handle_update);
#endif
}

static void sig_remove_update(void)
{
	signal(SIGHUP, SIG_IGN);
}

void check_abort(int be_async_signal_safe)
{
	if (!event_abort) return;

	tty_done();

	if (be_async_signal_safe) {
		if (timer_abort >= 0) {
			if (john_main_process)
				write_loop(2, "Session aborted\n", 16);
#if defined(HAVE_MPI) && defined(JOHN_MPI_ABORT)
			if (mpi_p > 1)
				MPI_Abort(MPI_COMM_WORLD,1);
#endif
		} else
		if (john_main_process)
			write_loop(2, "Session stopped (max run-time"
			           " reached)\n", 39);
		_exit(1);
	}

	if (john_main_process)
		fprintf(stderr, "Session %s\n", (timer_abort >= 0) ?
		        "aborted" : "stopped (max run-time reached)");
	error();
}

static void sig_install_abort(void);

static void sig_handle_abort(int signum)
{
	int saved_errno = errno;

	check_abort(1);

	event_abort = event_pending = 1;

	write_loop(2, "Wait...\r", 8);

	sig_install_abort();

	errno = saved_errno;
}

#ifdef WIN32_SIGNAL_HANDLER
#ifdef __CYGWIN32__
static CALLBACK BOOL sig_handle_abort_ctrl(DWORD ctrltype)
#else
static BOOL WINAPI sig_handle_abort_ctrl(DWORD ctrltype)
#endif
{
	sig_handle_abort(SIGINT);
	return TRUE;
}
#endif

static void sig_install_abort(void)
{
#ifdef __DJGPP__
	setcbrk(1);
#elif defined(WIN32_SIGNAL_HANDLER)
	SetConsoleCtrlHandler(sig_handle_abort_ctrl, TRUE);
#endif

	signal(SIGINT, sig_handle_abort);
#ifndef HAVE_MPI
	signal(SIGTERM, sig_handle_abort);
#endif
#ifdef SIGXCPU
	signal(SIGXCPU, sig_handle_abort);
#endif
#ifdef SIGXFSZ
	signal(SIGXFSZ, sig_handle_abort);
#endif
}

static void sig_remove_abort(void)
{
#ifdef WIN32_SIGNAL_HANDLER
	SetConsoleCtrlHandler(sig_handle_abort_ctrl, FALSE);
#endif

	signal(SIGINT, SIG_DFL);
#ifndef HAVE_MPI
	signal(SIGTERM, SIG_DFL);
#endif
#ifdef SIGXCPU
	signal(SIGXCPU, SIG_DFL);
#endif
#ifdef SIGXFSZ
	signal(SIGXFSZ, SIG_DFL);
#endif
}

#if OS_FORK
static void signal_children(int signum)
{
	int i;
	for (i = 0; i < john_child_count; i++)
		if (john_child_pids[i])
			kill(john_child_pids[i], signum);
}
#endif

static void sig_install_timer(void);

static void sig_handle_timer(int signum)
{
	int saved_errno = errno;
#ifndef BENCH_BUILD

#if OS_TIMER
	if (!--timer_save_value) {
		timer_save_value = timer_save_interval;
		event_save = event_pending = 1;
	}

	if (timer_abort && !--timer_abort) {
		timer_abort = -1;
		event_abort = event_pending = 1;
	}
	if (timer_status && !--timer_status) {
		timer_status = options.status_interval;
		event_status = event_pending = 1;
	}
	if (timer_reload && !--timer_reload) {
		event_save = event_reload = event_pending = 1;
	}
#else /* no OS_TIMER */
	unsigned int time = status_get_time();

	if (time >= timer_save_value) {
		timer_save_value += timer_save_interval;
		event_save = event_pending = 1;
	}

	if (timer_abort && time >= timer_abort) {
		timer_abort = -1;
		event_abort = event_pending = 1;
	}
	if (timer_status && time >= timer_status) {
		timer_status += options.status_interval;
		event_status = event_pending = 1;
	}
	if (timer_reload && time >= timer_reload) {
		timer_reload = 0;
		event_save = event_reload = event_pending = 1;
	}
#endif /* OS_TIMER */
#endif /* !BENCH_BUILD */

	if (!--timer_ticksafety_value) {
		timer_ticksafety_value = timer_ticksafety_interval;

		event_ticksafety = event_pending = 1;
	}

#ifndef HAVE_MPI
	if (john_main_process)
#endif
	{
		int c;
#if OS_FORK
		int new_abort = 0, new_status = 0;
#endif
		while ((c = tty_getchar()) >= 0) {
			if (c == 3 || c == 'q') {
#if OS_FORK
				new_abort = 1;
#endif
				sig_handle_abort(0);
			} else {
#if OS_FORK
				new_status = 1;
#endif
				event_status = event_pending = 1;
			}
		}

#if OS_FORK
		if (new_abort || new_status)
			signal_children(new_abort ? SIGTERM : SIGUSR1);
#endif
	}

#if !OS_TIMER
	signal(SIGALRM, sig_handle_timer);
#elif !defined(SA_RESTART) && !defined(__DJGPP__)
	sig_install_timer();
#endif

	errno = saved_errno;
}

#if OS_TIMER
static void sig_init_timer(void)
{
	struct itimerval it;

	it.it_value.tv_sec = TIMER_INTERVAL;
	it.it_value.tv_usec = 0;
#if defined(SA_RESTART) || defined(__DJGPP__)
	it.it_interval = it.it_value;
#else
	memset(&it.it_interval, 0, sizeof(it.it_interval));
#endif
	if (setitimer(ITIMER_REAL, &it, NULL))
		pexit("setitimer");
}
#endif

static void sig_install_timer(void)
{
#if !OS_TIMER
	signal(SIGALRM, sig_handle_timer);
	sig_timer_emu_init(TIMER_INTERVAL * clk_tck);
#else
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handle_timer;
#ifdef SA_RESTART
	sa.sa_flags = SA_RESTART;
#endif
	sigaction(SIGALRM, &sa, NULL);
#if !defined(SA_RESTART) && !defined(__DJGPP__)
	siginterrupt(SIGALRM, 0);
#endif

	sig_init_timer();
#endif
}

static void sig_remove_timer(void)
{
#if OS_TIMER
	struct itimerval it;

	memset(&it, 0, sizeof(it));
	if (setitimer(ITIMER_REAL, &it, NULL)) perror("setitimer");
#endif

	signal(SIGALRM, SIG_DFL);
}

#ifdef SIGUSR1
static void sig_handle_status(int signum)
{
	event_status = event_pending = 1;
	signal(SIGUSR1, sig_handle_status);
}
#endif

#ifdef SIGUSR2
static void sig_handle_reload(int signum)
{
#if OS_FORK
	signal_children(SIGUSR2);
#endif
#ifdef HAVE_MPI
	if (mpi_p == 1 && !getenv("OMPI_COMM_WORLD_SIZE"))
#endif
		event_save = event_reload = event_pending = 1;
	signal(SIGUSR2, sig_handle_reload);
}
#endif

void sig_init(void)
{
	clk_tck_init();

	timer_save_interval = cfg_get_int(SECTION_OPTIONS, NULL, "Save");
	if (timer_save_interval < 0)
		timer_save_interval = TIMER_SAVE_DELAY;
	else
	if ((timer_save_interval /= TIMER_INTERVAL) <= 0)
		timer_save_interval = 1;
#if OS_TIMER
	timer_save_value = timer_save_interval;
#elif !defined(BENCH_BUILD)
	timer_save_value = status_get_time() + timer_save_interval;
#endif
	timer_ticksafety_interval = (clock_t)1 << (sizeof(clock_t) * 8 - 4);
	timer_ticksafety_interval /= clk_tck;
	if ((timer_ticksafety_interval /= TIMER_INTERVAL) <= 0)
		timer_ticksafety_interval = 1;
	timer_ticksafety_value = timer_ticksafety_interval;

	atexit(sig_done);

	sig_install_update();
	sig_install_abort();
	sig_install_timer();
#ifdef SIGUSR1
	signal(SIGUSR1, sig_handle_status);
#endif
#ifdef SIGUSR2
	signal(SIGUSR2, sig_handle_reload);
#endif
}

void sig_init_child(void)
{
#if OS_TIMER
	sig_init_timer();
#endif
}

void sig_done(void)
{
	sig_remove_update();
	sig_remove_abort();
	sig_remove_timer();
}
