/*
 * C port of the Perl `wakeonlan` utility
 * <https://github.com/jpoliv/wakeonlan/blob/master/wakeonlan>
 *
 * SPDX-FileCopyrightText: 2026 Albert Ou <aou@eecs.berkeley.edu>
 * SPDX-FileCopyrightText: 2000-2024 José Pedro Oliveira <jose.p.oliveira.oss@gmail.com>
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define VERSION		"0.42_90-c"
#define DEFAULT_IP	"255.255.255.255"
#define DEFAULT_PORT	9		/* discard */
#define MAGIC_LEN	(6 + 16 * 6)

static int    verbose = 1;
static int    dryrun;
static double delay_s;

struct queue_entry {
	unsigned char hw[6];
	char hw_str[18]; /* canonical xx:xx:xx:xx:xx:xx + NUL */
	char ip[64];
	int port;
};

struct stats {
	unsigned long total;
	unsigned long valid;
	unsigned long invalid;
	unsigned long sent;
};

/* -------------------------------------------------------------------- */

static void warn_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static int hex_nibble(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/*
 * Parse a hardware address string into 6 bytes
 *
 * Accepted hardware address formats:
 *   xx:xx:xx:xx:xx:xx        (canonical)
 *   xx-xx-xx-xx-xx-xx        (Windows)
 *   xxxxxx-xxxxxx            (HP switches)
 *   xxxxxxxxxxxx             (Intel Landesk)
 */
static int parse_hwaddr(const char *s, unsigned char out[6])
{
	int nibbles[12];
	int n = 0;
	int i;
	int v;
	size_t len;
	int ok = 0;
	const char *p;

	/* Collect hex nibbles, allowing ':' or '-' as separators */
	for (p = s; *p; p++) {
		if (*p == ':' || *p == '-') {
			continue;
		}
		v = hex_nibble((unsigned char)*p);
		if (v < 0) {
			return 0;
		}
		if (n >= 12) {
			return 0;
		}
		nibbles[n++] = v;
	}
	if (n != 12) {
		return 0;
	}

	/* Validate separator structure against accepted formats */
	len = strlen(s);
	if (len == 12) {
		ok = 1;				/* xxxxxxxxxxxx */
	} else if (len == 13 && s[6] == '-') {
		ok = 1;				/* xxxxxx-xxxxxx */
	} else {
		/*
		 * xx:xx:xx:xx:xx:xx or xx-xx-xx-xx-xx-xx
		 * with 1- or 2-digit groups.
		 */
		char sep = 0;
		int groups = 0;
		int digits = 0;

		ok = 1;
		for (p = s; *p; p++) {
			if (*p == ':' || *p == '-') {
				if (sep == 0) {
					sep = *p;
				} else if (sep != *p) {
					ok = 0;
					break;
				}
				if (digits == 0) {
					ok = 0;
					break;
				}
				groups++;
				digits = 0;
			} else if (hex_nibble((unsigned char)*p) >= 0) {
				digits++;
				if (digits > 2) {
					ok = 0;
					break;
				}
			} else {
				ok = 0;
				break;
			}
		}
		if (digits > 0) {
			groups++;
		}
		if (groups != 6) {
			ok = 0;
		}
	}
	if (!ok) {
		return 0;
	}

	for (i = 0; i < 6; i++) {
		out[i] = (unsigned char)((nibbles[2 * i] << 4) |
					 nibbles[2 * i + 1]);
	}
	return 1;
}

static void canonical_hwaddr(const unsigned char hw[6], char out[18])
{
	snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
		 hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
}

static int is_valid_port(long p)
{
	return p >= 0 && p <= 65535;
}

static int is_valid_ip(const char *s)
{
	struct in_addr a;

	return inet_pton(AF_INET, s, &a) == 1;
}

/* -------------------------------------------------------------------- */

static int queue_push(struct queue_entry **q, size_t *n, size_t *cap,
		      const unsigned char hw[6], const char *ip, int port)
{
	size_t newcap;
	struct queue_entry *nq;
	struct queue_entry *e;

	if (*n == *cap) {
		newcap = *cap ? *cap * 2 : 16;
		nq = realloc(*q, newcap * sizeof(**q));
		if (!nq) {
			warn_msg("out of memory\n");
			return 0;
		}
		*q = nq;
		*cap = newcap;
	}
	e = &(*q)[(*n)++];
	memcpy(e->hw, hw, 6);
	canonical_hwaddr(hw, e->hw_str);
	strncpy(e->ip, ip, sizeof(e->ip) - 1);
	e->ip[sizeof(e->ip) - 1] = '\0';
	e->port = port;
	return 1;
}

static void load_cmdline(int argc, char *const argv[],
			 struct queue_entry **q, size_t *n, size_t *cap,
			 const char *def_ip, int def_port,
			 struct stats *st)
{
	int i;
	unsigned char hw[6];

	for (i = 0; i < argc; i++) {
		st->total++;
		if (!parse_hwaddr(argv[i], hw)) {
			warn_msg("Invalid hardware address: %s\n", argv[i]);
			st->invalid++;
			continue;
		}
		st->valid++;
		queue_push(q, n, cap, hw, def_ip, def_port);
	}
}

/* Trim leading/trailing whitespace in-place */
static char *strtrim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		end--;
	}
	*end = '\0';
	return s;
}

static void load_file(const char *filename,
		      struct queue_entry **q, size_t *n, size_t *cap,
		      const char *def_ip, int def_port,
		      struct stats *st)
{
	FILE *fp;
	char line[1024];
	char *s;
	char *hw_tok;
	char *ip_tok;
	char *pt_tok;
	char *endp;
	const char *ip;
	long v;
	int port;
	unsigned char hw[6];

	fp = fopen(filename, "r");
	if (!fp) {
		warn_msg("open : %s\n", strerror(errno));
		exit(1);
	}

	while (fgets(line, sizeof(line), fp)) {
		s = strtrim(line);
		if (*s == '\0' || *s == '#') {
			continue;
		}

		st->total++;

		/* Split on whitespace into up to 3 tokens */
		hw_tok = strtok(s, " \t");
		ip_tok = strtok(NULL, " \t");
		pt_tok = strtok(NULL, " \t");

		if (!hw_tok || !parse_hwaddr(hw_tok, hw)) {
			warn_msg("Invalid hardware address: %s\n",
				 hw_tok ? hw_tok : "");
			st->invalid++;
			continue;
		}

		ip = ip_tok ? ip_tok : def_ip;
		if (!is_valid_ip(ip)) {
			warn_msg("Invalid IP address: %s\n", ip);
			st->invalid++;
			continue;
		}

		port = def_port;
		if (pt_tok) {
			v = strtol(pt_tok, &endp, 10);
			if (*endp != '\0' || !is_valid_port(v)) {
				warn_msg("Invalid port number: %s\n", pt_tok);
				st->invalid++;
				continue;
			}
			port = (int)v;
		}

		st->valid++;
		queue_push(q, n, cap, hw, ip, port);
	}
	fclose(fp);
}

/* -------------------------------------------------------------------- */

/*
 * The 'magic packet' consists of 6 times 0xFF followed by 16 times
 * the hardware address of the NIC. This sequence can be encapsulated
 * in any kind of packet, in this case an UDP packet targeted at the
 * discard port (9) by default.
 */
static int wake(int sock, const struct queue_entry *e)
{
	unsigned char pkt[MAGIC_LEN];
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	char portbuf[8];
	int gai;
	int ok = 1;
	int i;
	ssize_t s;

	memset(pkt, 0xFF, 6);
	for (i = 0; i < 16; i++) {
		memcpy(pkt + 6 + i * 6, e->hw, 6);
	}

	/* Resolve destination */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	snprintf(portbuf, sizeof(portbuf), "%d", e->port);

	gai = getaddrinfo(e->ip, portbuf, &hints, &res);
	if (gai != 0) {
		warn_msg("getaddrinfo %s: %s\n", e->ip, gai_strerror(gai));
		return 0;
	}

	if (verbose) {
		printf("Sending magic packet to %s:%d with payload %s\n",
		       e->ip, e->port, e->hw_str);
	}

	if (!dryrun) {
		s = sendto(sock, pkt, sizeof(pkt), 0,
			   res->ai_addr, res->ai_addrlen);
		if (s < 0) {
			warn_msg("send : %s\n", strerror(errno));
			ok = 0;
		}
	}
	freeaddrinfo(res);
	return ok;
}

static void send_all(struct queue_entry *q, size_t n, struct stats *st)
{
	int sock;
	int on = 1;
	size_t i;
	struct timespec ts = { 0, 0 };

	if (n == 0) {
		warn_msg("Nothing to do!\n");
		return;
	}

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		warn_msg("socket : %s\n", strerror(errno));
		exit(1);
	}
	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) < 0) {
		warn_msg("setsockopt : %s\n", strerror(errno));
		close(sock);
		exit(1);
	}

	if (delay_s > 0.0) {
		ts.tv_sec  = (time_t)delay_s;
		ts.tv_nsec = (long)((delay_s - (double)ts.tv_sec) * 1e9);
	}

	for (i = 0; i < n; i++) {
		if (wake(sock, &q[i])) {
			st->sent++;
		}
		if (delay_s > 0.0 && i + 1 < n) {
			nanosleep(&ts, NULL);
		}
	}
	close(sock);
}

/* -------------------------------------------------------------------- */

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
		"Usage: %s [options] [hardware_address ...]\n"
		"\n"
		"  -h, --help              show this help\n"
		"  -v, --version           show version\n"
		"  -q, --quiet             quiet mode\n"
		"  -n, --dry-run           do not actually send packets\n"
		"  -i, --ip=IP_ADDRESS     destination IP (default %s)\n"
		"  -p, --port=PORT         destination UDP port (default %d)\n"
		"  -f, --file=FILE         read hardware addresses from FILE\n"
		"  -d, --delay=SECONDS     delay between packets (fractional, e.g. 0.5)\n"
		"\n"
		"Hardware address formats accepted:\n"
		"  xx:xx:xx:xx:xx:xx   xx-xx-xx-xx-xx-xx   xxxxxx-xxxxxx   xxxxxxxxxxxx\n",
		prog, DEFAULT_IP, DEFAULT_PORT);
}

int main(int argc, char **argv)
{
	const char *ip = DEFAULT_IP;
	int port = DEFAULT_PORT;
	const char *file = NULL;
	int first;
	int n_positional;
	struct queue_entry *q = NULL;
	size_t qn = 0;
	size_t qcap = 0;
	struct stats st = { 0, 0, 0, 0 };

	{
		static const struct option longs[] = {
			{ "help",    no_argument,       NULL, 'h' },
			{ "version", no_argument,       NULL, 'v' },
			{ "quiet",   no_argument,       NULL, 'q' },
			{ "dry-run", no_argument,       NULL, 'n' },
			{ "ip",      required_argument, NULL, 'i' },
			{ "port",    required_argument, NULL, 'p' },
			{ "file",    required_argument, NULL, 'f' },
			{ "delay",   required_argument, NULL, 'd' },
			{ NULL, 0, NULL, 0 }
		};
		int opt;
		char *endp;
		long lv;
		double dv;

		while ((opt = getopt_long(argc, argv, "hvqni:p:f:d:", longs, NULL)) != -1) {
			switch (opt) {
			case 'h':
				usage(stdout, argv[0]);
				return 0;
			case 'v':
				printf("wakeonlan %s\n", VERSION);
				return 0;
			case 'q':
				verbose = 0;
				break;
			case 'n':
				dryrun = 1;
				break;
			case 'i':
				ip = optarg;
				break;
			case 'p':
				lv = strtol(optarg, &endp, 10);
				if (*endp != '\0') {
					warn_msg("invalid port: %s\n", optarg);
					usage(stderr, argv[0]);
					return 1;
				}
				port = (int)lv;
				break;
			case 'f':
				file = optarg;
				break;
			case 'd':
				dv = strtod(optarg, &endp);
				if (*endp != '\0' || dv < 0.0) {
					warn_msg("invalid delay: %s\n", optarg);
					usage(stderr, argv[0]);
					return 1;
				}
				delay_s = dv;
				break;
			default:
				usage(stderr, argv[0]);
				return 1;
			}
		}
		first = optind;
	}

	if (!is_valid_port(port)) {
		warn_msg("Invalid default port number: %d\n", port);
		return 2;
	}
	if (!is_valid_ip(ip)) {
		warn_msg("Invalid default IP address: %s\n", ip);
		return 3;
	}
	if (file && access(file, R_OK) != 0) {
		warn_msg("Invalid filename: %s\n", file);
		return 4;
	}

	n_positional = argc - first;
	if (!file && n_positional <= 0) {
		usage(stdout, argv[0]);
		return 0;
	}

	if (n_positional > 0) {
		load_cmdline(n_positional, argv + first, &q, &qn, &qcap,
			     ip, port, &st);
	}
	if (file) {
		load_file(file, &q, &qn, &qcap, ip, port, &st);
	}

	send_all(q, qn, &st);

	if (verbose) {
		printf("Hardware addresses: <total=%lu, valid=%lu, invalid=%lu>\n",
		       st.total, st.valid, st.invalid);
		printf("Magic packets: <sent=%lu>\n", st.sent);
	}

	free(q);
	return 0;
}
