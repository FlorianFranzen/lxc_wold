/*
 * lxc-wold: linux container wake on LAN daemon
 *
 * (C) Copyright Florian Franzen, 2013
 *
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <signal.h>

#include <lxc/log.h>
#include <lxc/caps.h>
#include <lxc/lxc.h>
#include <lxc/conf.h>
#include <lxc/cgroup.h>
#include <lxc/utils.h>

// Stolen from lxc v0.8
#include "lxc/arguments.h"

// Global shutdown flag
int shutdown_flag = 0;

// WOL constants
#define WOL_PORT 9
#define MAGIC_PKT_SIZE 102

// network constant
#define PACKET_SIZE 65536

// LXC constants and variables
#define LXCPATH "/var/lib/lxc"

lxc_log_define(lxc_wold, lxc_start);

static struct lxc_list defines;

static int my_parser(struct lxc_arguments* args, int c, char* arg)
{
	switch (c) {
	case 'c': args->console = arg; break;
	case 'd': args->daemonize = 0; break;
	case 'f': args->rcfile = arg; break;
	case 'C': args->close_all_fds = 1; break;
	case 's': return lxc_config_define_add(&defines, arg);
	}
	return 0;
}

static const struct option my_longopts[] = {
	{"debug", no_argument, 0, 'd'},
	{"rcfile", required_argument, 0, 'f'},
	{"define", required_argument, 0, 's'},
	{"console", required_argument, 0, 'c'},
	LXC_COMMON_OPTIONS
};

static struct lxc_arguments my_args = {
	.progname = "lxc-wold",
	.help     = "\
--name=NAME -- COMMAND\n\
\n\
lxc-start start specified container when wol package arrives.\n\
\n\
Options :\n\
  -n, --name=NAME      NAME for name of the container\n\
  -d, --debug          debugging mode, run in foreground, do not daemonize\n\
  -f, --rcfile=FILE    Load configuration file FILE\n\
  -c, --console=FILE   Set the file output for the container console\n\
  -s, --define KEY=VAL Assign VAL to configuration variable KEY\n",
	.options   = my_longopts,
	.parser    = my_parser,
	.checker   = NULL,
	.daemonize = 1,
	.log_priority = "INFO"
};

// Checks if certain hwaddr is in conf
int lxc_test_hwaddr(struct lxc_conf* conf, char* hwaddr)
{
	struct lxc_list *network = &conf->network;
	struct lxc_list *iterator;
	struct lxc_netdev *netdev;

	lxc_list_for_each(iterator, network) {
		netdev = iterator->elem;
		if(!strncmp(netdev->hwaddr, hwaddr, 17)) return 1;
	}
	return 0;
}

// Signal handler
static void lxc_wold_shutdown(int sig) {
	shutdown_flag = 1;
}

int main(int argc, char *argv[])
{

	// Enable signals
	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, lxc_wold_shutdown);
	signal(SIGTERM, lxc_wold_shutdown);

	int err = -1;
	struct lxc_conf *conf;
	char *const *args;
	char *rcfile = NULL;
	char *const default_args[] = {
		"/sbin/init",
		'\0',
	};

	lxc_list_init(&defines);

	if (lxc_caps_init())
		return err;

	if (lxc_arguments_parse(&my_args, argc, argv))
		return err;

	if (!my_args.argc)
		args = default_args; 
	else
		args = my_args.argv;

	if (lxc_log_init(my_args.log_file, my_args.log_priority,
			 my_args.progname, my_args.quiet))
		return err;

	if (putenv("container=lxc")) {
		SYSERROR("failed to set environment variable");
		return err;
	}

	/* rcfile is specified in the cli option */
	if (my_args.rcfile)
		rcfile = (char *)my_args.rcfile;
	else {
		int rc;

		rc = asprintf(&rcfile, LXCPATH "/%s/config", my_args.name);
		if (rc == -1) {
			SYSERROR("failed to allocate memory");
			return err;
		}

		/* container configuration does not exist */
		if (access(rcfile, F_OK)) {
			free(rcfile);
			rcfile = NULL;
		}
	}

	conf = lxc_conf_init();
	if (!conf) {
		ERROR("failed to initialize configuration");
		return err;
	}

	if (rcfile && lxc_config_read(rcfile, conf)) {
		ERROR("failed to read configuration file");
		return err;
	}

	if (lxc_config_define_load(&defines, conf))
		return err;

	if (!rcfile && !strcmp("/sbin/init", args[0])) {
		ERROR("no configuration file for '/sbin/init' (may crash the host)");
		return err;
	}

	if (my_args.console) {

		char *console, fd;

		if (access(my_args.console, W_OK)) {

			fd = creat(my_args.console, 0600);
			if (fd < 0) {
				SYSERROR("failed to touch file '%s'",
					 my_args.console);
				return err;
			}
			close(fd);
		}

		console = realpath(my_args.console, NULL);
		if (!console) {
			SYSERROR("failed to get the real path of '%s'",
				 my_args.console);
			return err;
		}

		conf->console.path = strdup(console);
		if (!conf->console.path) {
			ERROR("failed to dup string '%s'", console);
			return err;
		}

		free(console);
	}

	if (!lxc_caps_check()) {
		ERROR("Not running with sufficient privilege");
		return err;
	}

	if(my_args.daemonize){
		if (daemon(0, 0)) {
			SYSERROR("daemon() failed");
			return err;
		}
	}


	// Outer while: init socket -> wait for package -> close socket -> run lxc
	int socket_fd;
	unsigned char* pkt_data;
	while (!shutdown_flag) { 
		
		int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (socket_fd < 0) {
			SYSERROR("socket() failed");
			return err;
		}

		int on = 1;
		if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
			SYSERROR("setsockopt(SO_REUSEADDR) failed");
			return err;
		}

		/* bind to an address */
		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(WOL_PORT);
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
			SYSERROR("bind() failed");
		}
		
		pkt_data = malloc(PACKET_SIZE);
		if (pkt_data == NULL) {
			SYSERROR("cannot malloc() packet buffer");
			return err;
		}

		// Inner while: wait for package
		fd_set sockfd_set;
		while(!shutdown_flag && !conf->reboot) {
			struct timeval tv = {
				.tv_sec = 10,
				.tv_usec = 0,
			};

			FD_ZERO(&sockfd_set);
			FD_SET(socket_fd, &sockfd_set);
			int numfd = select(socket_fd + 1, &sockfd_set, NULL, NULL, &tv);
			if (numfd <= 0)
				continue;

			if (FD_ISSET(socket_fd, &sockfd_set)) {
				struct sockaddr_in fromaddr;
				socklen_t sockaddr_size = sizeof(struct sockaddr_in);

				ssize_t recvsize = recvfrom(socket_fd,(void*) pkt_data, PACKET_SIZE, 0, 
					(struct sockaddr *) &fromaddr, &sockaddr_size);
				if (recvsize < 0) {
					SYSERROR("recv() failed");
				}

				// Check if magic package
				char unicast[6] = {(char) 0xFF, (char) 0xFF, (char) 0xFF, (char) 0xFF, (char) 0xFF, (char) 0xFF};
				if(recvsize == MAGIC_PKT_SIZE && strncmp(pkt_data, unicast, 6) == 0) {
					
					// Check if addresses do not differ
					int result = 1;
					int offset;
					for(offset = 6; offset < MAGIC_PKT_SIZE - 6; offset += 6) {
						if(strncmp(pkt_data + offset, pkt_data + offset + 6, 6) != 0) {
							result = 0;
							break;
						}
					}

					if(result) {
						char hwaddr[17];
						sprintf(hwaddr, "%02x:%02x:%02x:%02x:%02x:%02x", pkt_data[6], pkt_data[7], pkt_data[8], pkt_data[9], pkt_data[10], pkt_data[11]);
						
						INFO("WOL received for '%s'", hwaddr);
						
						if(lxc_test_hwaddr(conf, hwaddr)){
							// End while, package was received
							break;
						}

					} else {
						WARN("16 repeats of mac address differ");
					}

				} else {
					WARN("Non-magic package on port %d received", WOL_PORT);
				}
			} // No new packages
		} // Inner while

		if(pkt_data != NULL) 
			free(pkt_data);

		if (socket_fd >= 0)
			close(socket_fd);

		if(!shutdown_flag) {
			if (conf->reboot) {
				INFO("rebooting container");
				conf->reboot = 0;
			}

			// Start container		
			INFO("starting container");					
			conf->close_all_fds = 1;
			err = lxc_start(my_args.name, args, conf);
		}
	} // Outer while

	INFO("shutting down");

	return err;
}


