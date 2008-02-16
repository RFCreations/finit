/*
Improved fast init

Copyright (c) 2008 Claudio Matsuoka

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <linux/fs.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#ifdef DEBUG
#define debug(x...) do { \
		printf("finit: %d: ", __LINE__); printf(x); printf("\n"); \
	} while (0)
#else
#define debug(x...)
#endif

/* Distribution configuration */

#ifdef DIST_MDV		/* Mandriva */
#define RANDOMSEED	"/var/lib/random-seed"
#define SYSROOT		"/sysroot"
#define GETTY		"/sbin/mingetty tty3"
#define RUNPARTS	"/usr/bin/run-parts --arg=-i"
#define DEFUSER		"user"
#define HOMEDEV		"/dev/sda6"
#define AGPDRV		"intel-agp"
#define REMOUNT_ROOTFS_RW
#define MAKE_DEVICES
#else			/* original Eeepc distribution */
#define RANDOMSEED	"/var/lib/urandom/random-seed"
#define SYSROOT		"/mnt"
#define GETTY		"/sbin/getty 38400 tty3"
#define RUNPARTS	"/bin/run-parts --arg=-i"
#define DEFUSER		"user"
#define TOUCH_ETC_NETWORK_RUN_IFSTATE
#endif

#ifdef DIRECTISA
#define HWCLOCK_DIRECTISA " --directisa"
#else
#define HWCLOCK_DIRECTISA
#endif


/* From sysvinit */
/* Set a signal handler. */
#define SETSIG(sa, sig, fun, flags) \
		do { \
			sa.sa_handler = fun; \
			sa.sa_flags = flags; \
			sigemptyset(&sa.sa_mask); \
			sigaction(sig, &sa, NULL); \
		} while (0)

#define touch(x) mknod((x), S_IFREG|0644, 0)
#define chardev(x,m,maj,min) mknod((x), S_IFCHR|(m), makedev((maj),(min)))


int makepath(char *p)
{
	char *x, path[PATH_MAX];
	int ret;
	
	x = path;

	do {
		do { *x++ = *p++; } while (*p && *p != '/');
		ret = mkdir(path, 0777);
	} while (*p && (*p != '/' || *(p + 1))); /* ignore trailing slash */

	return ret;
}

void ifconfig(char *name, char *inet, char *mask, int flags)
{
	struct ifreq ifr;
	struct sockaddr_in *a = (struct sockaddr_in *)&(ifr.ifr_addr);
	int sock;

	if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0)
		return;

	memset(&ifr, 0, sizeof (ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ);
	a->sin_family = AF_INET;
	inet_aton(inet, &a->sin_addr);
	ioctl(sock, SIOCSIFADDR, &ifr);
	inet_aton(mask, &a->sin_addr);
	ioctl(sock, SIOCSIFNETMASK, &ifr);
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	ifr.ifr_flags |= flags;
	ioctl(sock, SIOCSIFFLAGS, &ifr);
	close(sock);
}



void do_shutdown(int);
void signal_handler(int);
void chld_handler(int);


int main()
{
	int i;
	FILE *f;
	char line[2096];
	int fd;
	DIR *dir;
	struct dirent *d;
	char filename[1024];
	struct sigaction sa, act;
	char hline[1024];
	char *x;
	sigset_t nmask, nmask2;

	puts("finit-alt " VERSION " (built " __DATE__ " by " WHOAMI ")");

	chdir("/");
	umask(022);
	
	/*
	 * Signal management
	 */
	for (i = 1; i < NSIG; i++)
		SETSIG(sa, i, SIG_IGN, SA_RESTART);

	SETSIG(sa, SIGINT,  do_shutdown,    0);
	SETSIG(sa, SIGPWR,  signal_handler, 0);
	SETSIG(sa, SIGUSR1, do_shutdown,    0);
	SETSIG(sa, SIGUSR2, do_shutdown,    0);
	SETSIG(sa, SIGTERM, signal_handler, 0);
	SETSIG(sa, SIGALRM, signal_handler, 0);
	SETSIG(sa, SIGHUP,  signal_handler, 0);
	SETSIG(sa, SIGCONT, signal_handler, SA_RESTART);
	SETSIG(sa, SIGCHLD, chld_handler,   SA_RESTART);
	
	/* Block sigchild while forking */
	sigemptyset(&nmask);
	sigaddset(&nmask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &nmask, NULL);

	reboot(RB_DISABLE_CAD);

	mount("proc", "/proc", "proc", 0, NULL);

	/*
	 * Parse kernel parameters
	 */
	if ((f = fopen("/proc/cmdline", "r")) != NULL) {
		fgets(line, 2095, f);
		if (strstr(line, "quiet")) {
			close(0);
			close(1);
			close(2);
		}
		fclose(f);
	}

	setsid();

	/*
	 * Mount filesystems
	 */
	debug("mount filesystems");

#ifdef REMOUNT_ROOTFS_RW
	system("/bin/mount -n -o remount,rw /");
#endif
	umask(0);

	mkdir("/dev/shm", 0755);
	mkdir("/dev/pts", 0755);

	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620");
	mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
	mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777,size=128m");
	mount(SYSROOT, "/", NULL, MS_MOVE, NULL);

	unlink("/etc/mtab");
	system("mount -a");

#ifdef MAKE_DEVICES
	debug("make devices");
	mkdir("/dev/input", 0755);
	chardev("/dev/urandom", 0666, 1, 9);
	chardev("/dev/ptmx", 0666, 5, 2);
	chardev("/dev/null", 0666, 1, 3);
	chardev("/dev/mem",  0640, 1, 1);
	chmod("/dev/null", 0667);
	chmod("/dev/mem", 0640);
	chardev("/dev/tty",  0666, 5, 0);
	chardev("/dev/input/mice",  0660, 13, 63);
	chardev("/dev/input/event0",  0660, 13, 64);
	chardev("/dev/agpgart",  0660, 10, 175);
#endif

	umask(0022);

	/*
	 * Time adjustments
	 */
	if ((fd = open("/etc/adjtime", O_CREAT|O_WRONLY|O_TRUNC, 0644)) >= 0) {
		write(fd, "0.0 0 0.0\n", 10);
		close(fd);
	}
#ifndef NO_HCTOSYS
	system("/sbin/hwclock --hctosys --localtime" HWCLOCK_DIRECTISA);
#endif

	/*
	 * Network stuff
	 */
	makepath("/dev/shm/network");
	makepath("/dev/shm/resolvconf/interface");

	if ((dir = opendir("/etc/resolvconf/run/interface")) != NULL) {
		while ((d = readdir(dir)) != NULL) {
			if (isalnum(d->d_name[0]))
				continue;
			sprintf(filename, "/etc/resolvconf/run/interface/%s",
								d->d_name);
			unlink(filename);
		}

		closedir(dir);
	}

	touch("/var/run/utmp");
	touch("/etc/resolvconf/run/enable-updates");

	chdir("/etc/resolvconf/run/interface");
	system(RUNPARTS " /etc/resolvconf/update.d");
	chdir("/");
	
#ifdef TOUCH_ETC_NETWORK_RUN_IFSTATE
	touch("/etc/network/run/ifstate");
#endif

	if ((f = fopen("/etc/hostname", "r")) != NULL) {
		fgets(hline, 1023, f);	
		if ((x = strchr(hline, 0x0a)) != NULL)
			*x = 0;

		sethostname(hline, strlen(hline)); 
		fclose(f);
	}

	ifconfig("lo", "127.0.0.1", "255.0.0.0", IFF_UP);

	/*
	 * Set random seed
	 */
	system("/bin/cat " RANDOMSEED " >/dev/urandom 2> /dev/null");
	unlink(RANDOMSEED);

	umask(077);
	system("/bin/dd if=/dev/urandom of=" RANDOMSEED "bs=4096 count=1 "
							">/dev/null 2>&1");

	/*
	 * Misc setup
	 */
	unlink("/var/run/.clean");
	unlink("/var/lock/.clean");
	umask(0);
	mkdir("/tmp/.X11-unix", 01777);
	mkdir("/tmp/.ICE-unix", 01777);
	umask(022);

	debug("forking");

	if (!fork()) {
		/* child process */

		vhangup();

		close(2);
		close(1);
		close(0);

		if (open("/dev/tty1", O_RDWR, 0))
			exit(1);

		sigemptyset(&act.sa_mask);
		act.sa_handler = SIG_DFL;

		sigemptyset(&nmask2);
		sigaddset(&nmask2, SIGCHLD);
		sigprocmask(SIG_UNBLOCK, &nmask2, NULL);

		for (i = 1; i < NSIG; i++)
			sigaction(i, &sa, NULL);

		dup2(0, 0);
		dup2(0, 1);
		dup2(0, 2);

		touch("/tmp/nologin");

#ifdef AGPDRV
		system("/sbin/modprobe agpgart");
		system("/sbin/modprobe " AGPDRV);
#endif
		
		while (access("/tmp/shutdown", F_OK) < 0) {
			debug("start X as " DEFUSER);
#ifdef DEBUG
			system("su -c startx -l " DEFUSER);
			system("/bin/sh");
#else
			system("su -c startx -l " DEFUSER " &> /dev/null");
#endif
		}

		exit(0);
	}
	
	/* parent process */

	system(GETTY " &");
	sleep(1);
	system("/usr/sbin/services.sh &> /dev/null &");

	while (1) {
		sigemptyset(&nmask);
		pselect(0, NULL, NULL, NULL, NULL, &nmask);
	}
}


/*
 * Shut down on INT USR1 USR2
 */
void do_shutdown(int sig)
{
	int fd;

	debug("shutdown");
	touch("/tmp/shutdown");

	kill(-1, SIGTERM);

	system("/bin/echo -e \"\033[?25l\033[30;40m\"; "
				"/bin/cp /boot/shutdown.fb /dev/fb/0");
	sleep(1);

	system("/usr/sbin/alsactl store > /dev/null 2>&1");
	system("/sbin/hwclock --systohc --localtime" HWCLOCK_DIRECTISA);
	system("/sbin/unionctl.static / --remove / > /dev/null 2>&1");

	kill(-1, SIGKILL);

	sync();
	sync();
	system("/bin/mount -n -o remount,ro /");

	system("/sbin/unionctl.static / --remove / > /dev/null 2>&1");

	if (sig == SIGINT || sig == SIGUSR1)
		reboot(RB_AUTOBOOT);

	if ((fd = open("/sys/power/state", O_WRONLY)) >= 0) {
		write(fd, "5", 1);
		close(fd);
	}
}


/*
 * SIGCHLD: one of our children has died
 */
void chld_handler(int sig)
{
	int status;

	while (waitpid(-1, &status, WNOHANG) != 0) {
		if (errno == ECHILD)
			break;
	}
}


/*
 * We got a signal (PWR TERM ALRM HUP CONT)
 */
void signal_handler(int sig)
{
	/* do nothing */
}
