/* Improved fast init
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2012  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <ctype.h>
#include <sys/mount.h>
#include <dirent.h>
#include <linux/fs.h>

#include "finit.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "svc.h"
#include "signal.h"

int   debug    = 0;
int   verbose  = 1;
char *sdown    = NULL;
char *network  = NULL;
char *username = NULL;
char *hostname = NULL;


static void parse_kernel_cmdline(void)
{
	FILE *fp;
	char line[LINE_SIZE];

	if ((fp = fopen("/proc/cmdline", "r")) != NULL) {
		fgets(line, sizeof(line), fp);

		if (strstr(line, "finit_debug") || strstr(line, "--debug"))
			debug = 1;

		if (!debug && strstr(line, "quiet"))
			verbose = 0;

		fclose(fp);
	}
}

int main(int UNUSED(args), char *argv[])
{
	struct ev_loop *loop = ev_default_loop(0);

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Mount base file system, kernel is assumed to run devtmpfs for /dev
	 */
	chdir("/");
	umask(0);
	mount("none", "/proc", "proc", 0, NULL);
	mount("none", "/proc/bus/usb", "usbfs", 0, NULL);
	mount("none", "/sys", "sysfs", 0, NULL);
	mkdir("/dev/pts", 0755);
	mkdir("/dev/shm", 0755);
	mount("none", "/dev/pts", "devpts", 0, "gid=5,mode=620");
	mount("none", "/dev/shm", "tmpfs", 0, NULL);
	umask(022);

	/*
	 * Populate /dev and prepare for runtime events from kernel.
	 */
#if defined(USE_UDEV)
	run_interactive("udevd --daemon", "Populating device tree");
#elif defined (MDEV)
	run_interactive(MDEV "-s", "Populating device tree");
#endif

	/*
	 * Parse kernel parameters
	 */
	parse_kernel_cmdline();

	cls();
	echo("finit " VERSION " (built " __DATE__ " " __TIME__ " by " WHOAMI ")");

	/*
	 * Parse configuration file
	 */
	parse_finit_conf(FINIT_CONF);

	/*
	 * Mount filesystems
	 */
	_d("Mount filesystems in /etc/fstab ...");

#ifdef REMOUNT_ROOTFS_RW
	run("/bin/mount -n -o remount,rw /");
#endif
#ifdef SYSROOT
	run(SYSROOT, "/", NULL, MS_MOVE, NULL);
#endif
	umask(0);
	run("/bin/mount -na");
	run("/sbin/swapon -ea");
	umask(0022);

	/* Cleanup stale files, if any still linger on. */
	run_interactive("rm -rf /tmp/* /var/run/* /var/lock/*", "Cleanup temporary directories");

	/*
	 * Base FS up, enable standard SysV init signals
	 */
	sig_setup();

	/*
	 * Load plugins and run first level hooks.
	 */
	_d("Loading plugins ...");
	load_plugins(loop, PLUGIN_PATH);

	_d("Running first level hooks ...");
	run_hooks(HOOK_BASEFS_UP);

	/*
	 * Network stuff
	 */

	/* Setup kernel specific settings, e.g. allow broadcast ping, etc. */
	run("/sbin/sysctl -e -p /etc/sysctl.conf >/dev/null");

	/* Set initial hostname. */
	set_hostname(hostname);

	ifconfig("lo", "127.0.0.1", "255.0.0.0", 1);
	if (network)
		run_interactive(network, "Starting networking: %s", network);
	umask(022);

	/*
	 * Hooks that rely on loopback, or basic networking being up.
	 */
	run_hooks(HOOK_NETWORK_UP);

	/*
	 * Start service monitor framework
	 */
	_d("Starting all static services from %s", FINIT_CONF);
	svc_start_all();

	/*
	 * Run startup scripts in /etc/finit.d/, if any.
	 */
	_d("Running startup scripts in /etc/finit.d/ ...");
	run_parts(FINIT_RCSD);

	/*
	 * Hooks that should run at the very end
	 */
	run_hooks(HOOK_SYSTEM_UP);

	/* Start GETTY on console */
	_d("Starting getty on console ...");
	run_getty(GETTY, argv);

	/*
	 * Enter main loop to monior /dev/initctl and services
	 */
	_d("Entering main loop ...");
	ev_run(loop, 0);

	return 0;
}

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
