/*
 * overcommit libhugetlbfs and check the statistics.
 *
 * libhugetlbfs allows to overcommit hugepages and there are tunables in
 * sysfs and procfs. The test here want to ensure it is possible to
 * overcommit by either mmap or shared memory. Also ensure those
 * reservation can be read/write, and several statistics work correctly.
 *
 * First, it resets nr_hugepages and nr_overcommit_hugepages. Then, set
 * both to a specify value - N, and allocate N + %50 x N hugepages.
 * Finally, it reads and writes every page. There are command options to
 * choose either to manage hugepages from sysfs or procfs, and reserve
 * them by mmap or shmget.
 *
 * Copyright (C) 2010  Red Hat, Inc.
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it
 * is free of the rightful claim of any third person regarding
 * infringement or the like.  Any license provided herein, whether
 * implied or otherwise, applies only to this software file.  Patent
 * licenses, if any, provided herein do not apply to combinations of
 * this program with other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include "test.h"
#include "usctest.h"

#define PROTECTION		(PROT_READ | PROT_WRITE)
#define _PATH_MEMINFO		"/proc/meminfo"
#define _PATH_SYS_HUGE		"/sys/kernel/mm/hugepages"
#define _PATH_SYS_2M		_PATH_SYS_HUGE "/hugepages-2048kB/"
#define _PATH_SYS_2M_OVER	_PATH_SYS_2M "nr_overcommit_hugepages"
#define _PATH_SYS_2M_FREE	_PATH_SYS_2M "free_hugepages"
#define _PATH_SYS_2M_RESV	_PATH_SYS_2M "resv_hugepages"
#define _PATH_SYS_2M_SURP	_PATH_SYS_2M "surplus_hugepages"
#define _PATH_SYS_2M_HUGE	_PATH_SYS_2M "nr_hugepages"
#define _PATH_PROC_VM		"/proc/sys/vm/"
#define _PATH_PROC_OVER		_PATH_PROC_VM "nr_overcommit_hugepages"
#define _PATH_PROC_HUGE		_PATH_PROC_VM "nr_hugepages"
#define _PATH_SHMMAX		"/proc/sys/kernel/shmmax"
#define MB			(1024 * 1024)

/* Only ia64 requires this */
#ifdef __ia64__
#define ADDR (void *)(0x8000000000000000UL)
#define FLAGS (MAP_SHARED | MAP_FIXED)
#define SHMAT_FLAGS (SHM_RND)
#else
#define ADDR (void *)(0x0UL)
#define FLAGS (MAP_SHARED)
#define SHMAT_FLAGS (0)
#endif

#ifndef SHM_HUGETLB
#define SHM_HUGETLB 04000
#endif

char *TCID = "hugemmap05";
char *TESTDIR;
int TST_TOTAL = 1, Tst_count;
static char nr_hugepages[BUFSIZ], nr_overcommit_hugepages[BUFSIZ];
static char buf[BUFSIZ], line[BUFSIZ], path[BUFSIZ], pathover[BUFSIZ];
static char shmmax[BUFSIZ];
static char *opt_allocstr;
static int opt_sysfs, opt_alloc;
static int shmid = -1;
static int restore_shmmax = 0;
static size_t size = 128, length = 384;
static option_t options[] = {
	{ "s", &opt_sysfs,	NULL},
	{ "m", &shmid,		NULL},
	{ "a:", &opt_alloc,	&opt_allocstr},
	{ NULL, NULL,		NULL}
};
static void setup(void);
static void cleanup(void) LTP_ATTRIBUTE_NORETURN;
static void overcommit(void);
static void write_bytes(void *addr);
static void read_bytes(void *addr);
static int lookup (char *line, char *pattern);
static void usage(void);
static int checkproc(FILE *fp, char *string, int value);
static int checksys(char *path, char *pattern, int value);

int main(int argc, char *argv[])
{
	int lc;
	char *msg;

	msg = parse_opts(argc, argv, options, usage);
	if (msg != NULL)
		tst_brkm(TBROK, tst_exit, "OPTION PARSING ERROR - %s", msg);
	if (opt_sysfs) {
		strncpy(path, _PATH_SYS_2M_HUGE,
			strlen(_PATH_SYS_2M_HUGE) + 1);
		strncpy(pathover, _PATH_SYS_2M_OVER,
			strlen(_PATH_SYS_2M_OVER) + 1);
	} else {
		strncpy(path, _PATH_PROC_HUGE,
			strlen(_PATH_PROC_HUGE) + 1);
		strncpy(pathover, _PATH_PROC_OVER,
			strlen(_PATH_PROC_OVER) + 1);
	}
	if (opt_alloc) {
		size = atoi(opt_allocstr);
		length = (int)(size + size * 0.5) * 2;
	}
	setup();
	for (lc = 0; TEST_LOOPING(lc); lc++) {
		Tst_count = 0;
		overcommit();
	}	
	cleanup();
}

static void overcommit(void)
{
	void *addr = NULL, *shmaddr = NULL;
	int fd = -1, key = -1;
	char s[BUFSIZ];
	FILE *fp;

	if (shmid != -1) {
		/* Use /proc/meminfo to generate an IPC key. */
		key = ftok(_PATH_MEMINFO, strlen(_PATH_MEMINFO));
		if (key == -1)
			tst_brkm(TBROK|TERRNO, cleanup, "ftok");
		shmid = shmget(key, (long)(length * MB),
			SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W);
		if (shmid == -1)
			tst_brkm(TBROK|TERRNO, cleanup, "shmget");
	} else {
		snprintf(s, BUFSIZ, "%s/hugemmap05/file", TESTDIR);
		fd = open(s, O_CREAT | O_RDWR, 0755);
		if (fd == -1)
			tst_brkm(TBROK|TERRNO, cleanup, "open");
		addr = mmap(ADDR, (long)(length * MB), PROTECTION, FLAGS, fd,
			0);
		if (addr == MAP_FAILED)
			tst_brkm(TBROK|TERRNO, cleanup, "mmap");
	}

	if (opt_sysfs) {
		tst_resm(TINFO, "check sysfs before allocation.");
		if (checksys(_PATH_SYS_2M_HUGE, "HugePages_Total",
				length / 2) != 0)
			return;
		if (checksys(_PATH_SYS_2M_FREE, "HugePages_Free",
				length / 2) != 0)
			return;
		if (checksys(_PATH_SYS_2M_SURP, "HugePages_Surp",
				length / 2 - size) != 0)
			return;
		if (checksys(_PATH_SYS_2M_RESV, "HugePages_Rsvd",
				length / 2) != 0)
			return;
	} else {
		tst_resm(TINFO,
			"check /proc/meminfo before allocation.");
		fp = fopen(_PATH_MEMINFO, "r");
		if (fp == NULL)
			tst_brkm(TBROK|TERRNO, cleanup, "fopen");
		if (checkproc(fp, "HugePages_Total", length / 2) != 0)
			return;
		if (checkproc(fp, "HugePages_Free", length / 2 ) != 0)
			return;
		if (checkproc(fp, "HugePages_Surp", length / 2 - size)
			!= 0)
			return;
		if (checkproc(fp, "HugePages_Rsvd", length / 2) != 0)
			return;
		fclose(fp);
	}
	if (shmid != -1) {
		tst_resm(TINFO, "shmid: 0x%x", shmid);
		shmaddr = shmat(shmid, ADDR, SHMAT_FLAGS);
		if (shmaddr == (void *)-1)
			tst_brkm(TBROK|TERRNO, cleanup, "shmat");
		write_bytes(shmaddr);
		read_bytes(shmaddr);
        } else {
		write_bytes(addr);
		read_bytes(addr);
	}
	if (opt_sysfs) {
		tst_resm(TINFO, "check sysfs.");
		if (checksys(_PATH_SYS_2M_HUGE, "HugePages_Total",
				length / 2) != 0)
			return;
		if (checksys(_PATH_SYS_2M_FREE, "HugePages_Free", 0)
			!= 0)
			return;
		if (checksys(_PATH_SYS_2M_SURP, "HugePages_Surp",
				length / 2 - size) != 0)
			return;
		if (checksys(_PATH_SYS_2M_RESV, "HugePages_Rsvd", 0)
			!= 0)
			return;
	} else {
		tst_resm(TINFO, "check /proc/meminfo.");
		fp = fopen(_PATH_MEMINFO, "r");
		if (fp == NULL)
			tst_brkm(TBROK|TERRNO, cleanup, "fopen");
		if (checkproc(fp, "HugePages_Total", length / 2) != 0)
			return;
		if (checkproc(fp, "HugePages_Free", 0) != 0)
			return;
		if (checkproc(fp, "HugePages_Surp", length / 2 - size)
			!= 0)
			return;
		if (checkproc(fp, "HugePages_Rsvd", 0) != 0)
			return;
		fclose(fp);
	}
	if (shmid != -1) {
		if (shmdt(shmaddr) != 0)
			tst_brkm(TBROK|TERRNO, cleanup, "shmdt");
	} else {
		munmap(addr, (long)(length * MB));
		close(fd);
		unlink(s);
	}
}

static void cleanup(void)
{
	int fd;

	TEST_CLEANUP;
	if (restore_shmmax) {
		fd = open(_PATH_SHMMAX, O_WRONLY);
		if (fd == -1)
			tst_resm(TWARN|TERRNO, "open");
		if (write(fd, shmmax, strlen(shmmax)) != strlen(shmmax))
			tst_resm(TWARN|TERRNO, "write");
		close(fd);
	}
	fd = open(path, O_WRONLY);
	if (fd == -1)
		tst_resm(TWARN|TERRNO, "open");
	tst_resm(TINFO, "restore nr_hugepages to %s.", nr_hugepages);
	if (write(fd, nr_hugepages,
			strlen(nr_hugepages)) != strlen(nr_hugepages))
		tst_resm(TWARN|TERRNO, "write");
	close(fd);

	fd = open(pathover, O_WRONLY);
	if (fd == -1)
		tst_resm(TWARN|TERRNO, "open");
	tst_resm(TINFO, "restore nr_overcommit_hugepages to %s.",
		nr_overcommit_hugepages);
	if (write(fd, nr_overcommit_hugepages, strlen(nr_overcommit_hugepages))
		!= strlen(nr_overcommit_hugepages))
		tst_resm(TWARN|TERRNO, "write");
	close(fd);
	
	snprintf(buf, BUFSIZ, "%s/hugemmap05", TESTDIR);
	if (umount(buf) == -1)
		tst_resm(TWARN|TERRNO, "umount");
	if (shmid != -1) {
		tst_resm(TINFO|TERRNO, "shmdt");
		shmctl(shmid, IPC_RMID, NULL);
	}
	tst_exit();
}

static void setup(void)
{
	FILE *fp;
	int fd;

	tst_sig(FORK, DEF_HANDLER, cleanup);
	TEST_PAUSE;
	if (shmid != -1) {
		fp = fopen(_PATH_SHMMAX, "r");
		if (fp == NULL)
			tst_brkm(TBROK|TERRNO, cleanup, "fopen");
		if (fgets(shmmax, BUFSIZ, fp) == NULL)
			tst_brkm(TBROK|TERRNO, cleanup, "fgets");
		fclose(fp);

		if (atol(shmmax) < (long)(length * MB)) {
			restore_shmmax = 1;
			fd = open(_PATH_SHMMAX, O_RDWR);
			if (fd == -1)
				tst_brkm(TBROK|TERRNO, cleanup, "open");
			snprintf(buf, BUFSIZ, "%ld", (long)(length * MB));
			if (write(fd, buf, strlen(buf)) != strlen(buf))
				tst_brkm(TBROK|TERRNO, cleanup,
					"failed to change shmmax.");
		}
	}
	fp = fopen(path, "r+");
	if (fp == NULL)
		tst_brkm(TBROK|TERRNO, cleanup, "fopen");
	if (fgets(nr_hugepages, BUFSIZ, fp) == NULL)
		tst_brkm(TBROK|TERRNO, cleanup, "fgets");
	fclose(fp);
	/* Remove trialing newline. */
	nr_hugepages[strlen(nr_hugepages) - 1] = '\0';
	tst_resm(TINFO, "original nr_hugepages is %s", nr_hugepages);

	fd = open(path, O_RDWR);
	if (fd == -1)
		tst_brkm(TBROK|TERRNO, cleanup, "open");
	/* Reset. */
	if (write(fd, "0", 1) != 1)
		tst_brkm(TBROK|TERRNO, cleanup, "write");
	if (lseek(fd, 0, SEEK_SET) == -1)
		tst_brkm(TBROK|TERRNO, cleanup, "lseek");
	snprintf(buf, BUFSIZ, "%ld", size);
	if (write(fd, buf, strlen(buf)) != strlen(buf))
		tst_brkm(TBROK|TERRNO, cleanup,
			"failed to change nr_hugepages.");
	close(fd);

	fp = fopen(pathover, "r+");
	if (fp == NULL)
		tst_brkm(TBROK|TERRNO, cleanup, "fopen");
	if (fgets(nr_overcommit_hugepages, BUFSIZ, fp) == NULL)
		tst_brkm(TBROK|TERRNO, cleanup, "fgets");
	fclose(fp);
	nr_overcommit_hugepages[strlen(nr_overcommit_hugepages) - 1] = '\0';
	tst_resm(TINFO, "original nr_overcommit_hugepages is %s",
		nr_overcommit_hugepages);

	fd = open(pathover, O_RDWR);
	if (fd == -1)
		tst_brkm(TBROK|TERRNO, cleanup, "open");
	/* Reset. */
	if (write(fd, "0", 1) != 1)
		tst_brkm(TBROK|TERRNO, cleanup, "write");
	if (lseek(fd, 0, SEEK_SET) == -1)
		tst_brkm(TBROK|TERRNO, cleanup, "lseek");
	snprintf(buf, BUFSIZ, "%ld", size);
	if (write(fd, buf, strlen(buf)) != strlen(buf))
		tst_brkm(TBROK|TERRNO, cleanup,
			"failed to change nr_hugepages.");
	close(fd);

	snprintf(buf, BUFSIZ, "%s/hugemmap05", TESTDIR);
	if (mkdir(buf, 0700) == -1)
		tst_brkm(TBROK|TERRNO, cleanup, "mkdir");
	if (mount(NULL, buf, "hugetlbfs", 0, NULL) == -1)
		tst_brkm(TBROK|TERRNO, cleanup, "mount");
}

static void write_bytes(void *addr)
{
	long i;

	for (i = 0; i < (long)(length * MB); i++)
		((char *)addr)[i] = '\a';
}

static void read_bytes(void *addr)
{
	long i;

	tst_resm(TINFO, "First hex is %x", *((unsigned int *)addr));
	for (i = 0; i < (long)(length * MB); i++) {
		if (((char *)addr)[i] != '\a') {
			tst_resm(TFAIL, "mismatch at %ld", i);
			break;
		}
	}
}

/* Lookup a pattern and get the value from file*/
static int lookup (char *line, char *pattern)
{
	char buf2[BUFSIZ];

	/* empty line */
	if (line[0] == '\0')
		return 0;

	snprintf(buf2, BUFSIZ, "%s: %%s", pattern);
	if (sscanf(line, buf2, buf) != 1)
		return 0;

	return 1;
}

static void usage(void)
{
	printf("  -s      Setup hugepages from sysfs\n");
	printf("  -m      Reserve hugepages by shmget\n");
	printf("  -a      Number of overcommint hugepages\n");
}

static int checksys(char *path, char *string, int value)
{
	FILE *fp;

	fp = fopen(path, "r");
	if (fp == NULL)
		tst_brkm(TBROK|TERRNO, cleanup, "fopen");
	if (fgets(buf, BUFSIZ, fp) == NULL)
		tst_brkm(TBROK|TERRNO, cleanup, "fgets");
	tst_resm(TINFO, "%s is %d.", string, atoi(buf));
	if (atoi(buf) != value) {
		tst_resm(TFAIL, "%s is not %d but %d.", string, value,
			atoi(buf));
		return 1;
	}
	fclose(fp);
	return 0;
}

static int checkproc(FILE *fp, char *pattern, int value)
{
	memset(buf, -1, BUFSIZ);
	rewind(fp);
	while (fgets(line, BUFSIZ, fp) != NULL)
		if (lookup(line, pattern))
			break;

	tst_resm(TINFO, "%s is %d.", pattern, atoi(buf));
	if (atoi(buf) != value) {
		tst_resm(TFAIL, "%s is not %d but %d.", pattern, value,
			atoi(buf));
		return 1;
	}
	return 0;
}