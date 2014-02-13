/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2014 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <net/if.h>
#endif
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__sun__)
#include <stropts.h>
#include <sys/sockio.h>
#include <net/if_tun.h>
#ifndef TUNNEWPPA
#error "Install TAP driver from http://www.whiteboard.ne.jp/~admin2/tuntap/"
#endif
#endif

#include "openconnect-internal.h"

/*
 * If an if_tun.h include file was found anywhere (by the Makefile), it's
 * included. Else, we end up assuming that we have BSD-style devices such
 * as /dev/tun0 etc.
 */
#ifdef IF_TUN_HDR
#include IF_TUN_HDR
#endif

/*
 * The OS X tun/tap driver doesn't provide a header file; you're expected
 * to define this for yourself.
 */
#ifdef __APPLE__
#define TUNSIFHEAD  _IOW('t', 96, int)
#endif

/*
 * OpenBSD always puts the protocol family prefix onto packets. Other
 * systems let us enable that with the TUNSIFHEAD ioctl, and some of them
 * (e.g. FreeBSD) _need_ it otherwise they'll interpret IPv6 packets as IPv4.
 */
#if defined(__OpenBSD__) || defined(TUNSIFHEAD)
#define TUN_HAS_AF_PREFIX 1
#endif

static int set_tun_mtu(struct openconnect_info *vpninfo)
{
#if !defined(__sun__) && !defined(_WIN32) /* We don't know how to do this on Solaris */
	struct ifreq ifr;
	int net_fd;

	net_fd = socket(PF_INET, SOCK_DGRAM, 0);
	if (net_fd < 0) {
		perror(_("open net"));
		return -EINVAL;
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, vpninfo->ifname, sizeof(ifr.ifr_name) - 1);
	ifr.ifr_mtu = vpninfo->ip_info.mtu;

	if (ioctl(net_fd, SIOCSIFMTU, &ifr) < 0)
		perror(_("SIOCSIFMTU"));

	close(net_fd);
#endif
	return 0;
}
int script_config_tun(struct openconnect_info *vpninfo, const char *reason)
{
	int ret;

	if (!vpninfo->vpnc_script || vpninfo->script_tun)
		return 0;

	setenv("reason", reason, 1);
	ret = system(vpninfo->vpnc_script);
	if (ret == -1) {
		int e = errno;
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to spawn script '%s' for %s: %s\n"),
			     vpninfo->vpnc_script, reason, strerror(e));
		return -e;
	}
#ifdef _WIN32
	if (ret == 0x2331) {
		/* This is what cmd.exe returns for unrecognised commands */
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to spawn script '%s' for %s: %s\n"),
			     vpninfo->vpnc_script, reason, strerror(ENOENT));
		return -ENOENT;
	}
#else
	if (!WIFEXITED(ret)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Script '%s' exited abnormally (%x)\n"),
			       vpninfo->vpnc_script, ret);
		return -EIO;
	}

	ret = WEXITSTATUS(ret);
#endif
	if (ret) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Script '%s' returned error %d\n"),
			     vpninfo->vpnc_script, ret);
		return -EIO;
	}
	return 0;
}

#ifdef __sun__
static int link_proto(int unit_nr, const char *devname, uint64_t flags)
{
	int ip_fd, mux_id, tun2_fd;
	struct lifreq ifr;

	tun2_fd = open("/dev/tun", O_RDWR);
	if (tun2_fd < 0) {
		perror(_("Could not open /dev/tun for plumbing"));
		return -EIO;
	}
	if (ioctl(tun2_fd, I_PUSH, "ip") < 0) {
		perror(_("Can't push IP"));
		close(tun2_fd);
		return -EIO;
	}

	sprintf(ifr.lifr_name, "tun%d", unit_nr);
	ifr.lifr_ppa = unit_nr;
	ifr.lifr_flags = flags;

	if (ioctl(tun2_fd, SIOCSLIFNAME, &ifr) < 0) {
		perror(_("Can't set ifname"));
		close(tun2_fd);
		return -1;
	}

	ip_fd = open(devname, O_RDWR);
	if (ip_fd < 0) {
		fprintf(stderr, _("Can't open %s: %s"), devname,
			strerror(errno));
		close(tun2_fd);
		return -1;
	}

	mux_id = ioctl(ip_fd, I_LINK, tun2_fd);
	if (mux_id < 0) {
		fprintf(stderr, _("Can't plumb %s for IPv%d: %s\n"),
			 ifr.lifr_name, (flags == IFF_IPV4) ? 4 : 6,
			 strerror(errno));
		close(tun2_fd);
		close(ip_fd);
		return -1;
	}

	close(tun2_fd);

	return ip_fd;
}
#endif

#ifdef SIOCIFCREATE
static int bsd_open_tun(char *tun_name)
{
	int fd;
	int s;
	struct ifreq ifr;

	fd = open(tun_name, O_RDWR);
	if (fd >= 0) {
		return fd;

		s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s < 0)
			return -1;

		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_name, tun_name + 5, sizeof(ifr.ifr_name) - 1);
		if (!ioctl(s, SIOCIFCREATE, &ifr))
			fd = open(tun_name, O_RDWR);

		close(s);
	}
	return fd;
}
#else
#define bsd_open_tun(tun_name) open(tun_name, O_RDWR)
#endif

static int os_setup_tun(struct openconnect_info *vpninfo)
{
	int tun_fd = -1;
#ifdef _WIN32
	tun_fd = win32_setup_tun(vpninfo);
#elif defined(IFF_TUN) /* Linux */
	struct ifreq ifr;
	int tunerr;

	tun_fd = open("/dev/net/tun", O_RDWR);
	if (tun_fd < 0) {
		/* Android has /dev/tun instead of /dev/net/tun
		   Since other systems might have too, just try it
		   as a fallback instead of using ifdef __ANDROID__ */
		tunerr = errno;
		tun_fd = open("/dev/tun", O_RDWR);
	}
	if (tun_fd < 0) {
		/* If the error on /dev/tun is ENOENT, that's boring.
		   Use the error we got on /dev/net/tun instead */
		if (errno != ENOENT)
			tunerr = errno;

		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to open tun device: %s\n"),
			     strerror(tunerr));
		return -EIO;
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	if (vpninfo->ifname)
		strncpy(ifr.ifr_name, vpninfo->ifname,
			sizeof(ifr.ifr_name) - 1);
	if (ioctl(tun_fd, TUNSETIFF, (void *) &ifr) < 0) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("TUNSETIFF failed: %s\n"),
			     strerror(errno));
		return -EIO;
	}
	if (!vpninfo->ifname)
		vpninfo->ifname = strdup(ifr.ifr_name);
#elif defined(__sun__)
	static char tun_name[80];
	int unit_nr;

	tun_fd = open("/dev/tun", O_RDWR);
	if (tun_fd < 0) {
		perror(_("open /dev/tun"));
		return -EIO;
	}

	unit_nr = ioctl(tun_fd, TUNNEWPPA, -1);
	if (unit_nr < 0) {
		perror(_("Failed to create new tun"));
		close(tun_fd);
		return -EIO;
	}

	if (ioctl(tun_fd, I_SRDOPT, RMSGD) < 0) {
		perror(_("Failed to put tun file descriptor into message-discard mode"));
		close(tun_fd);
		return -EIO;
	}

	sprintf(tun_name, "tun%d", unit_nr);
	vpninfo->ifname = strdup(tun_name);

	vpninfo->ip_fd = link_proto(unit_nr, "/dev/udp", IFF_IPV4);
	if (vpninfo->ip_fd < 0) {
		close(tun_fd);
		return -EIO;
	}

	if (vpninfo->ip_info.addr6) {
		vpninfo->ip6_fd = link_proto(unit_nr, "/dev/udp6", IFF_IPV6);
		if (vpninfo->ip6_fd < 0) {
			close(tun_fd);
			close(vpninfo->ip_fd);
			vpninfo->ip_fd = -1;
			return -EIO;
		}
	} else
		vpninfo->ip6_fd = -1;

#else /* BSD et al have /dev/tun$x devices */
	static char tun_name[80];
	int i;

	if (vpninfo->ifname) {
		char *endp = NULL;
		if (strncmp(vpninfo->ifname, "tun", 3) ||
		    ((void)strtol(vpninfo->ifname + 3, &endp, 10), !endp) ||
		    *endp) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Invalid interface name '%s'; must match 'tun%%d'\n"),
				     vpninfo->ifname);
			return -EINVAL;
		}
		snprintf(tun_name, sizeof(tun_name),
			 "/dev/%s", vpninfo->ifname);
		tun_fd = bsd_open_tun(tun_name);
		if (tun_fd < 0) {
			int err = errno;
			vpn_progress(vpninfo, PRG_ERR,
				     _("Cannot open '%s': %s\n"),
				     tun_name, strerror(err));
			return -EINVAL;
		}
	}
#ifdef HAVE_FDEVNAME_R
	/* We don't have to iterate over the possible devices; on FreeBSD
	   at least, opening /dev/tun will give us the next available
	   device. */
	if (tun_fd < 0) {
		tun_fd = open("/dev/tun", O_RDWR);
		if (tun_fd >= 0) {
			if (!fdevname_r(tun_fd, tun_name, sizeof(tun_name)) ||
			    strncmp(tun_name, "tun", 3)) {
				close(tun_fd);
				tun_fd = -1;
			} else
				vpninfo->ifname = strdup(tun_name);
		}
	}
#endif
	if (tun_fd < 0) {
		for (i = 0; i < 255; i++) {
			sprintf(tun_name, "/dev/tun%d", i);
			tun_fd = bsd_open_tun(tun_name);
			if (tun_fd >= 0)
				break;
		}
		if (tun_fd < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to open tun device: %s\n"),
				     strerror(errno));
			return -EIO;
		}
		vpninfo->ifname = strdup(tun_name + 5);
	}
#ifdef TUNSIFHEAD
	i = 1;
	if (ioctl(tun_fd, TUNSIFHEAD, &i) < 0) {
		perror(_("TUNSIFHEAD"));
		return -EIO;
	}
#endif
#endif /* BSD-style */
	return tun_fd;
}

#ifdef _WIN32
int openconnect_setup_tun_fd(struct openconnect_info *vpninfo, int tun_fd)
{
	return 0;
}
#else
int openconnect_setup_tun_fd(struct openconnect_info *vpninfo, int tun_fd)
{
	set_fd_cloexec(tun_fd);

	if (vpninfo->tun_fd != -1)
		unmonitor_read_fd(vpninfo, tun);

	vpninfo->tun_fd = tun_fd;

	monitor_fd_new(vpninfo, tun);
	monitor_read_fd(vpninfo, tun);

	set_sock_nonblock(tun_fd);

	return 0;
}

int openconnect_setup_tun_script(struct openconnect_info *vpninfo, char *tun_script)
{
	pid_t child;
	int fds[2];

	vpninfo->vpnc_script = tun_script;
	vpninfo->script_tun = 1;

	set_script_env(vpninfo);
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds)) {
		vpn_progress(vpninfo, PRG_ERR, _("socketpair failed: %s\n"), strerror(errno));
		return -EIO;
	}
	child = fork();
	if (child < 0) {
		vpn_progress(vpninfo, PRG_ERR, _("fork failed: %s\n"), strerror(errno));
		return -EIO;
	} else if (!child) {
		if (setpgid(0, getpid()) < 0)
			perror(_("setpgid"));
		close(fds[0]);
		setenv_int("VPNFD", fds[1]);
		execl("/bin/sh", "/bin/sh", "-c", vpninfo->vpnc_script, NULL);
		perror(_("execl"));
		exit(1);
	}
	close(fds[1]);
	vpninfo->script_tun = child;
	vpninfo->ifname = strdup(_("(script)"));

	return openconnect_setup_tun_fd(vpninfo, fds[0]);
}
#endif /* !_WIN32 */

int openconnect_setup_tun_device(struct openconnect_info *vpninfo, char *vpnc_script, char *ifname)
{
	int tun_fd;

	vpninfo->vpnc_script = vpnc_script;
	vpninfo->ifname = ifname;

	set_script_env(vpninfo);
	script_config_tun(vpninfo, "pre-init");

	tun_fd = os_setup_tun(vpninfo);
	if (tun_fd < 0)
		return tun_fd;

	setenv("TUNDEV", vpninfo->ifname, 1);
	script_config_tun(vpninfo, "connect");

	/* Ancient vpnc-scripts might not get this right */
	set_tun_mtu(vpninfo);

	return openconnect_setup_tun_fd(vpninfo, tun_fd);
}

static struct pkt *out_pkt;

int tun_mainloop(struct openconnect_info *vpninfo, int *timeout)
{
	int work_done = 0;
	int prefix_size = 0;
#ifdef _WIN32
	DWORD pkt_size = 0;
#endif

#ifdef TUN_HAS_AF_PREFIX
	if (!vpninfo->script_tun)
		prefix_size = sizeof(int);
#endif

	if (read_fd_monitored(vpninfo, tun)) {
		while (1) {
			int len = vpninfo->ip_info.mtu;

			if (!out_pkt) {
				out_pkt = malloc(sizeof(struct pkt) + len);
				if (!out_pkt) {
					vpn_progress(vpninfo, PRG_ERR, "Allocation failed\n");
					break;
				}
#ifdef _WIN32
				if (!ReadFile(vpninfo->tun_fh, out_pkt->data, len, &pkt_size, &vpninfo->tun_rd_overlap)) {
					DWORD err = GetLastError();
					if (err != ERROR_IO_PENDING)
						vpn_progress(vpninfo, PRG_ERR,
							  _("Failed to read from TAP device: %lx\n"),
							  err);
					break;
				}
				len = pkt_size;
			} else {
				/* if out_pkt was already non-NULL then there was lready a pending read on it. */
				if (!GetOverlappedResult(vpninfo->tun_fh, &vpninfo->tun_rd_overlap, &pkt_size, FALSE)) {
					DWORD err = GetLastError();

					if (err != ERROR_IO_INCOMPLETE)
						vpn_progress(vpninfo, PRG_ERR,
							     _("Failed to complete read from TAP device: %lx\n"),
							     err);
					break;
				}
				len = pkt_size;
#endif /* _WIN32 */
			}
#ifndef _WIN32
			/* Sanity. Just non-blocking reads on a select()able file descriptor... */
			len = read(vpninfo->tun_fd, out_pkt->data - prefix_size, len + prefix_size);
#endif
			if (len <= prefix_size)
				break;
			out_pkt->len = len - prefix_size;

			vpninfo->stats.tx_pkts++;
			vpninfo->stats.tx_bytes += out_pkt->len;

			queue_packet(&vpninfo->outgoing_queue, out_pkt);
			out_pkt = NULL;

			work_done = 1;
			vpninfo->outgoing_qlen++;
			if (vpninfo->outgoing_qlen == vpninfo->max_qlen) {
				unmonitor_read_fd(vpninfo, tun);
				break;
			}
		}
	} else if (vpninfo->outgoing_qlen < vpninfo->max_qlen) {
		monitor_read_fd(vpninfo, tun);
	}

	/* The kernel returns -ENOMEM when the queue is full, so theoretically
	   we could handle that and retry... but it doesn't let us poll() for
	   the no-longer-full situation, so let's not bother. */
	while (vpninfo->incoming_queue) {
		struct pkt *this = vpninfo->incoming_queue;
		unsigned char *data = this->data;
		int len = this->len;

		vpninfo->stats.rx_pkts++;
		vpninfo->stats.rx_bytes += len;

#ifdef TUN_HAS_AF_PREFIX
		if (!vpninfo->script_tun) {
			struct ip *iph = (void *)data;
			int type;

			if (iph->ip_v == 6)
				type = AF_INET6;
			else if (iph->ip_v == 4)
				type = AF_INET;
			else {
				static int complained = 0;
				if (!complained) {
					complained = 1;
					vpn_progress(vpninfo, PRG_ERR,
						     _("Unknown packet (len %d) received: %02x %02x %02x %02x...\n"),
						     len, data[0], data[1], data[2], data[3]);
				}
				free(this);
				continue;
			}
			data -= 4;
			len += 4;
			*(int *)data = htonl(type);
		}
#endif
		vpninfo->incoming_queue = this->next;
#ifdef _WIN32
		if (!WriteFile(vpninfo->tun_fh, data, len, &pkt_size, &vpninfo->tun_wr_overlap)) {
			DWORD err = GetLastError();

			if (err == ERROR_IO_PENDING) {
				/* Theoretically we should let the mainloop handle this blocking,
				   but that's non-trivial and it doesn't ever seem to happen in
				   practice anyway. */
				vpn_progress(vpninfo, PRG_TRACE,
					     _("Waiting for tun write...\n"));
				if (!GetOverlappedResult(vpninfo->tun_fh, &vpninfo->tun_wr_overlap, &pkt_size, TRUE)) {
					err = GetLastError();
					goto report_write_err;
				}
				vpn_progress(vpninfo, PRG_TRACE,
					     _("Wrote %ld bytes to tun after waiting\n"), pkt_size);
			} else {
			report_write_err:
				vpn_progress(vpninfo, PRG_ERR,
					     _("Failed to write to TAP device: %lx\n"), err);
			}
		} else {
			vpn_progress(vpninfo, PRG_TRACE,
				    _("Wrote %ld bytes to tun\n"), pkt_size);
		}
#else
		if (write(vpninfo->tun_fd, data, len) < 0) {
			/* Handle death of "script" socket */
			if (vpninfo->script_tun && errno == ENOTCONN) {
				vpninfo->quit_reason = "Client connection terminated";
				return 1;
			}
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to write incoming packet: %s\n"),
				     strerror(errno));
		}
#endif
		free(this);
	}
	/* Work is not done if we just got rid of packets off the queue */
	return work_done;
}

void shutdown_tun(struct openconnect_info *vpninfo)
{
#ifdef _WIN32
	script_config_tun(vpninfo, "disconnect");
	CloseHandle(vpninfo->tun_fh);
	vpninfo->tun_fh = NULL;
	CloseHandle(vpninfo->tun_rd_overlap.hEvent);
	vpninfo->tun_rd_overlap.hEvent = NULL;
#else
	if (vpninfo->script_tun) {
		/* nuke the whole process group */
		kill(-vpninfo->script_tun, SIGHUP);
	} else {
		script_config_tun(vpninfo, "disconnect");
#ifdef __sun__
		close(vpninfo->ip_fd);
		vpninfo->ip_fd = -1;
		if (vpninfo->ip6_fd != -1) {
			close(vpninfo->ip6_fd);
			vpninfo->ip6_fd = -1;
		}
#endif
	}

	if (vpninfo->vpnc_script)
		close(vpninfo->tun_fd);
	vpninfo->tun_fd = -1;
#endif
}
