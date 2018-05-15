/*	$OpenBSD: tcpbench.c,v 1.55 2018/05/10 14:29:17 benno Exp $	*/

/*
 * Copyright (c) 2008 Damien Miller <djm@mindrot.org>
 * Copyright (c) 2011 Christiano F. Haesbaert <haesbaert@haesbaert.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/resource.h>
#include <sys/queue.h>
#include <sys/un.h>

#include <net/route.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>

#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <event.h>
#include <netdb.h>
#include <signal.h>
#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

#include <nlist.h>

#define	INVALID		1
#define	TOOSMALL	2
#define	TOOLARGE	3

#define DEFAULT_PORT "12345"
#define DEFAULT_STATS_INTERVAL 1000 /* ms */
#define DEFAULT_BUF (256 * 1024)
#define DEFAULT_UDP_PKT (1500 - 28) /* TODO don't hardcode this */
#define TCP_MODE !ptb->uflag
#define UDP_MODE ptb->uflag
#define MAX_FD 1024

/* Our tcpbench globals */
struct {
	int	  Sflag;	/* Socket buffer size */
	u_int	  rflag;	/* Report rate (ms) */
	int	  sflag;	/* True if server */
	int	  vflag;	/* Verbose */
	int	  uflag;	/* UDP mode */
	int	  Uflag;	/* UNIX (AF_LOCAL) mode */
	char	 *dummybuf;	/* IO buffer */
	size_t	  dummybuf_len;	/* IO buffer len */
} tcpbench, *ptb;

struct tcpservsock {
	struct event ev;
	struct event evt;
	int fd;
};

/* stats for a single tcp connection, udp uses only one  */
struct statctx {
	TAILQ_ENTRY(statctx) entry;
	struct timeval t_start, t_last;
	unsigned long long bytes;
	int fd;
	char *buf;
	size_t buflen;
	struct event ev;
	/* TCP only */
	struct tcpservsock *tcp_ts;
	u_long tcp_tcbaddr;
	/* UDP only */
	u_long udp_slice_pkts;
};

static long long	strtonum(const char*, long long, long long,
    const char **);
static void	signal_handler(int, short, void *);
static void	saddr_ntop(const struct sockaddr *, socklen_t, char *, size_t);
static void	drop_gid(void);
static void	set_slice_timer(int);
static void	print_tcp_header(void);
static void	stats_prepare(struct statctx *);
static void	tcp_stats_display(unsigned long long, long double, float,
    struct statctx *);
static void	tcp_process_slice(int, short, void *);
static void	tcp_server_handle_sc(int, short, void *);
static void	tcp_server_accept(int, short, void *);
static void	server_init(struct addrinfo *, struct statctx *);
static void	client_handle_sc(int, short, void *);
static void	client_init(struct addrinfo *, int, struct statctx *,
    struct addrinfo *);
static int	clock_gettime_tv(clockid_t, struct timeval *);
static void	udp_server_handle_sc(int, short, void *);
static void	udp_process_slice(int, short, void *);
static void	set_socket_unblock(int);
static void	quit(int, short,void *);

/*
 * We account the mainstats here, that is the stats
 * for all connections, all variables starting with slice
 * are used to account information for the timeslice
 * between each output. Peak variables record the highest
 * between all slices so far.
 */
static struct {
	unsigned long long slice_bytes; /* bytes for last slice */
	long double peak_mbps;		/* peak mbps so far */
	int nconns; 		        /* connected clients */
	struct event timer;		/* process timer */
} mainstats;

TAILQ_HEAD(, statctx) sc_queue;

static void
usage(void)
{
	fprintf(stderr,
	    "Usage:\n"
	    "Client:     tcpbench [-46Uuv] [-B buf] [-b addr] [-n connections]\n"
	    "                [-p port] [-r interval] [-S space] [-t secs] hostname\n"
	    "Server:     tcpbench -s [-46Uuv] [-B buf] [-p port] [-r interval]\n"
	    "                [-S space] [hostname]\n"
	    "\n"
	    "Options:\n"
	    "-4          Use IPv4 addresses.\n"
	    "-6          Use IPv6 addresses.\n"
	    "-B buf      Specify the size of the internal read/write buffer used by\n"
	    "            tcpbench. The default is 262144 bytes for TCP client/server and\n"
	    "            UDP server. In UDP client mode this may be used to specify the\n"
	    "            packet size on the test stream.\n"
	    "-b addr     Specify the IP address of the interface which is used to send\n"
	    "            the packets.\n"
	    "-n conns    Use the given number of TCP connections (default: 1). UDP is\n"
	    "            connectionless so this option isn't valid.\n"
	    "-p port     Specify the port used for the test stream (default: 12345).\n"
	    "-r interval Specify the statistics interval reporting rate in milliseconds\n"
	    "            (default: 1000).\n"
	    "-S space    Set the size of the socket buffer used for the test stream. On \n"
	    "            the client this option will resize the send buffer; on the server\n"
	    "            it will resize the receive buffer.\n"
	    "-s          Place tcpbench in server mode, where it will listen on all\n"
	    "            interfaces for incoming connections. It defaults to using TCP if\n"
	    "            -u is not specified.\n"
	    "-t secs     Stop after secs seconds.\n"
	    "-U          Use AF_UNIX sockets instead of IPv4 or IPv6 sockets. In client\n"
	    "            and server mode hostname is used as the path to the AF_UNIX socket.\n"
	    "-u          Use UDP instead of TCP; this must be specified on both the client\n"
	    "            and the server. Transmitted packets per second (TX PPS) will be\n"
	    "            accounted on the client side, while received packets per second\n"
	    "            (RX PPS) will be accounted on the server side.\n"
	    "-v          Display verbose output. If specified more than once, increase\n"
	    "            the detail of information displayed.\n");
	exit(1);
}

static long long
strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp)
{
	long long ll = 0;
	int error = 0;
	char *ep;
	struct errval {
		const char *errstr;
		int err;
	} ev[4] = {
		{ NULL,		0 },
		{ "invalid",	EINVAL },
		{ "too small",	ERANGE },
		{ "too large",	ERANGE },
	};

	ev[0].err = errno;
	errno = 0;
	if (minval > maxval) {
		error = INVALID;
	} else {
		ll = strtoll(numstr, &ep, 10);
		if (numstr == ep || *ep != '\0')
			error = INVALID;
		else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
			error = TOOSMALL;
		else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
			error = TOOLARGE;
	}
	if (errstrp != NULL)
		*errstrp = ev[error].errstr;
	errno = ev[error].err;
	if (error)
		ll = 0;

	return (ll);
}

static void
signal_handler(int sig, short event, void *bula)
{
	/*
	 * signal handler rules don't apply, libevent decouples for us
	 */
	switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGHUP:
		warnx("Terminated by signal %d", sig);
		exit(0);
		break;		/* NOTREACHED */
	default:
		errx(1, "unexpected signal %d", sig);
		break;		/* NOTREACHED */
	}
}

static void
saddr_ntop(const struct sockaddr *addr, socklen_t alen, char *buf, size_t len)
{
	char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV];
	int herr;

	if (addr->sa_family == AF_UNIX) {
		struct sockaddr_un *sun = (struct sockaddr_un *)addr;
		snprintf(buf, len, "%s", sun->sun_path);
		return;
	}
	if ((herr = getnameinfo(addr, alen, hbuf, sizeof(hbuf),
	    pbuf, sizeof(pbuf), NI_NUMERICHOST|NI_NUMERICSERV)) != 0) {
		if (herr == EAI_SYSTEM)
			err(1, "getnameinfo");
		else
			errx(1, "getnameinfo: %s", gai_strerror(herr));
	}
	snprintf(buf, len, "[%s]:%s", hbuf, pbuf);
}

static void
drop_gid(void)
{
	gid_t gid;

	gid = getgid();
	if (setregid(gid, gid) == -1)
		err(1, "setregid");
}

static void
set_slice_timer(int on)
{
	struct timeval tv;

	if (ptb->rflag == 0)
		return;

	if (on) {
		if (evtimer_pending(&mainstats.timer, NULL))
			return;
		/* XXX Is there a better way to do this ? */
		tv.tv_sec = ptb->rflag / 1000;
		tv.tv_usec = (ptb->rflag % 1000) * 1000;
		
		evtimer_add(&mainstats.timer, &tv);
	} else if (evtimer_pending(&mainstats.timer, NULL))
		evtimer_del(&mainstats.timer);
}
		
static int
clock_gettime_tv(clockid_t clock_id, struct timeval *tv)
{
	struct timespec ts;

	if (clock_gettime(clock_id, &ts) == -1)
		return (-1);
	
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
	
	return (0);
}

static void
print_tcp_header(void)
{
	printf("%12s %14s %12s %8s\n", "elapsed_ms", "bytes", "mbps", "bwidth");
}

static void
stats_prepare(struct statctx *sc)
{
	sc->buf = ptb->dummybuf;
	sc->buflen = ptb->dummybuf_len;

	if (clock_gettime_tv(CLOCK_MONOTONIC, &sc->t_start) == -1)
		err(1, "clock_gettime_tv");
	sc->t_last = sc->t_start;

}

static void
tcp_stats_display(unsigned long long total_elapsed, long double mbps,
    float bwperc, struct statctx *sc)
{
	printf("%12llu %14llu %12.3Lf %7.2f%%\n", total_elapsed, sc->bytes,
	    mbps, bwperc);
}

static void
tcp_process_slice(int fd, short event, void *bula)
{
	unsigned long long total_elapsed, since_last;
	long double mbps, slice_mbps = 0;
	float bwperc;
	struct statctx *sc;
	struct timeval t_cur, t_diff;
	
	TAILQ_FOREACH(sc, &sc_queue, entry) {
		if (clock_gettime_tv(CLOCK_MONOTONIC, &t_cur) == -1)
			err(1, "clock_gettime_tv");
		
		timersub(&t_cur, &sc->t_start, &t_diff);
		total_elapsed = t_diff.tv_sec * 1000 + t_diff.tv_usec / 1000;
		timersub(&t_cur, &sc->t_last, &t_diff);
		since_last = t_diff.tv_sec * 1000 + t_diff.tv_usec / 1000;
		bwperc = (sc->bytes * 100.0) / mainstats.slice_bytes;
		mbps = (sc->bytes * 8) / (since_last * 1000.0);
		slice_mbps += mbps;
		
		tcp_stats_display(total_elapsed, mbps, bwperc, sc);
		
		sc->t_last = t_cur;
		sc->bytes = 0;
	}

	/* process stats for this slice */
	if (slice_mbps > mainstats.peak_mbps)
		mainstats.peak_mbps = slice_mbps;
	printf("Conn: %3d Mbps: %12.3Lf Peak Mbps: %12.3Lf Avg Mbps: %12.3Lf\n",
	    mainstats.nconns, slice_mbps, mainstats.peak_mbps,
	    mainstats.nconns ? slice_mbps / mainstats.nconns : 0); 
	mainstats.slice_bytes = 0;

	set_slice_timer(mainstats.nconns > 0);
}

static void
udp_process_slice(int fd, short event, void *v_sc)
{
	struct statctx *sc = v_sc;
	unsigned long long total_elapsed, since_last, pps;
	long double slice_mbps;
	struct timeval t_cur, t_diff;

	if (clock_gettime_tv(CLOCK_MONOTONIC, &t_cur) == -1)
		err(1, "clock_gettime_tv");
	/* Calculate pps */
	timersub(&t_cur, &sc->t_start, &t_diff);
	total_elapsed = t_diff.tv_sec * 1000 + t_diff.tv_usec / 1000;
	timersub(&t_cur, &sc->t_last, &t_diff);
	since_last = t_diff.tv_sec * 1000 + t_diff.tv_usec / 1000;
	slice_mbps = (sc->bytes * 8) / (since_last * 1000.0);
	pps = (sc->udp_slice_pkts * 1000) / since_last;
	if (slice_mbps > mainstats.peak_mbps)
		mainstats.peak_mbps = slice_mbps;
	printf("Elapsed: %11llu Mbps: %11.3Lf Peak Mbps: %11.3Lf %s PPS: %7llu\n",
	    total_elapsed, slice_mbps, mainstats.peak_mbps,
	    ptb->sflag ? "Rx" : "Tx", pps);

	/* Clean up this slice time */
	sc->t_last = t_cur;
	sc->bytes = 0;
	sc->udp_slice_pkts = 0;
	set_slice_timer(1);
}

static void
udp_server_handle_sc(int fd, short event, void *v_sc)
{
	ssize_t n;
	struct statctx *sc = v_sc;

	n = read(fd, ptb->dummybuf, ptb->dummybuf_len);
	if (n == 0)
		return;
	else if (n == -1) {
		if (errno != EINTR && errno != EWOULDBLOCK)
			warn("fd %d read error", fd);
		return;
	}
		
	if (ptb->vflag >= 3)
		fprintf(stderr, "read: %zd bytes\n", n);
	/* If this was our first packet, start slice timer */
	if (mainstats.peak_mbps == 0)
		set_slice_timer(1);
	/* Account packet */
	sc->udp_slice_pkts++;
	sc->bytes += n;
}

static void
tcp_server_handle_sc(int fd, short event, void *v_sc)
{
	struct statctx *sc = v_sc;
	ssize_t n;

	n = read(sc->fd, sc->buf, sc->buflen);
	if (n == -1) {
		if (errno != EINTR && errno != EWOULDBLOCK)
			warn("fd %d read error", sc->fd);
		return;
	} else if (n == 0) {
		if (ptb->vflag)
			fprintf(stderr, "%8d closed by remote end\n", sc->fd);

		TAILQ_REMOVE(&sc_queue, sc, entry);

		event_del(&sc->ev);
		close(sc->fd);

		/* Some file descriptors are available again. */
		if (evtimer_pending(&sc->tcp_ts->evt, NULL)) {
			evtimer_del(&sc->tcp_ts->evt);
			event_add(&sc->tcp_ts->ev, NULL);
		}

		free(sc);
		mainstats.nconns--;
		return;
	}
	if (ptb->vflag >= 3)
		fprintf(stderr, "read: %zd bytes\n", n);
	sc->bytes += n;
	mainstats.slice_bytes += n;
}
	
static void
tcp_server_accept(int fd, short event, void *arg)
{
	struct tcpservsock *ts = arg;
	int sock;
	struct statctx *sc;
	struct sockaddr_storage ss;
	socklen_t sslen;
	char tmp[128];
	
	sslen = sizeof(ss);

	event_add(&ts->ev, NULL);
	if (event & EV_TIMEOUT)
		return;
	set_socket_unblock(fd);
	if ((sock = accept(fd, (struct sockaddr *)&ss, &sslen)) == -1) {
		/*
		 * Pause accept if we are out of file descriptors, or
		 * libevent will haunt us here too.
		 */
		if (errno == ENFILE || errno == EMFILE) {
			struct timeval evtpause = { 1, 0 };

			event_del(&ts->ev);
			evtimer_add(&ts->evt, &evtpause);
		} else if (errno != EWOULDBLOCK && errno != EINTR &&
		    errno != ECONNABORTED)
			warn("accept");
		return;
	}
	saddr_ntop((struct sockaddr *)&ss, sslen,
	    tmp, sizeof(tmp));
	/* Alloc client structure and register reading callback */
	if ((sc = calloc(1, sizeof(*sc))) == NULL)
		err(1, "calloc");
	sc->tcp_ts = ts;
	sc->fd = sock;
	stats_prepare(sc);
	event_set(&sc->ev, sc->fd, EV_READ | EV_PERSIST,
	    tcp_server_handle_sc, sc);
	event_add(&sc->ev, NULL);
	TAILQ_INSERT_TAIL(&sc_queue, sc, entry);
	mainstats.nconns++;
	if (mainstats.nconns == 1)
		set_slice_timer(1);
	if (ptb->vflag)
		fprintf(stderr, "Accepted connection from %s, fd = %d\n",
		    tmp, sc->fd);
}

static void
server_init(struct addrinfo *aitop, struct statctx *udp_sc)
{
	char tmp[128];
	int sock, on = 1;
	struct addrinfo *ai;
	struct event *ev;
	struct tcpservsock *ts;
	nfds_t lnfds;

	lnfds = 0;
	for (ai = aitop; ai != NULL; ai = ai->ai_next) {
		saddr_ntop(ai->ai_addr, ai->ai_addrlen, tmp, sizeof(tmp));
		if (ptb->vflag)
			fprintf(stderr, "Try to bind to %s\n", tmp);
		if ((sock = socket(ai->ai_family, ai->ai_socktype,
		    ai->ai_protocol)) == -1) {
			if (ai->ai_next == NULL)
				err(1, "socket");
			if (ptb->vflag)
				warn("socket");
			continue;
		}
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
		    &on, sizeof(on)) == -1)
			warn("reuse port");
		if (bind(sock, ai->ai_addr, ai->ai_addrlen) != 0) {
			if (ai->ai_next == NULL)
				err(1, "bind");
			if (ptb->vflag)
				warn("bind");
			close(sock);
			continue;
		}
		if (ptb->Sflag) {
			if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
			    &ptb->Sflag, sizeof(ptb->Sflag)) == -1)
				warn("set receive socket buffer size");
		}
		if (TCP_MODE) {
			if (listen(sock, 64) == -1) {
				if (ai->ai_next == NULL)
					err(1, "listen");
				if (ptb->vflag)
					warn("listen");
				close(sock);
				continue;
			}
		}
		if (UDP_MODE) {
			if ((ev = calloc(1, sizeof(*ev))) == NULL)
				err(1, "calloc");
			event_set(ev, sock, EV_READ | EV_PERSIST,
			    udp_server_handle_sc, udp_sc);
			event_add(ev, NULL);
		} else {
			if ((ts = calloc(1, sizeof(*ts))) == NULL)
				err(1, "calloc");

			ts->fd = sock;
			evtimer_set(&ts->evt, tcp_server_accept, ts);
			event_set(&ts->ev, ts->fd, EV_READ,
			    tcp_server_accept, ts);
			event_add(&ts->ev, NULL);
		}
		if (ptb->vflag >= 3)
			fprintf(stderr, "bound to fd %d\n", sock);
		lnfds++;
	}
	if (!ptb->Uflag)
		freeaddrinfo(aitop);
	if (lnfds == 0)
		errx(1, "No working listen addresses found");
}	

static void
client_handle_sc(int fd, short event, void *v_sc)
{
	struct statctx *sc = v_sc;
	ssize_t n;
	size_t blen = sc->buflen;

	if ((n = write(sc->fd, sc->buf, blen)) == -1) {
		if (errno == EINTR || errno == EWOULDBLOCK ||
		    (UDP_MODE && errno == ENOBUFS))
			return;
		err(1, "write");
	}
	if (TCP_MODE && n == 0) {
		fprintf(stderr, "Remote end closed connection");
		exit(1);
	}
	if (ptb->vflag >= 3)
		fprintf(stderr, "write: %zd bytes\n", n);
	sc->bytes += n;
	mainstats.slice_bytes += n;
	if (UDP_MODE)
		sc->udp_slice_pkts++;
}

static void
client_init(struct addrinfo *aitop, int nconn, struct statctx *udp_sc,
    struct addrinfo *aib)
{
	struct statctx *sc;
	struct addrinfo *ai;
	char tmp[128];
	int i, sock;

	sc = udp_sc;
	for (i = 0; i < nconn; i++) {
		for (sock = -1, ai = aitop; ai != NULL; ai = ai->ai_next) {
			saddr_ntop(ai->ai_addr, ai->ai_addrlen, tmp,
			    sizeof(tmp));
			if (ptb->vflag && i == 0)
				fprintf(stderr, "Trying %s\n", tmp);
			if ((sock = socket(ai->ai_family, ai->ai_socktype,
			    ai->ai_protocol)) == -1) {
				if (ai->ai_next == NULL)
					err(1, "socket");
				if (ptb->vflag)
					warn("socket");
				continue;
			}
			if (aib != NULL) {
				saddr_ntop(aib->ai_addr, aib->ai_addrlen,
				    tmp, sizeof(tmp));
				if (ptb->vflag)
					fprintf(stderr,
					    "Try to bind to %s\n", tmp);
				if (bind(sock, (struct sockaddr *)aib->ai_addr,
				    aib->ai_addrlen) == -1)
					err(1, "bind");
			}
			if (ptb->Sflag) {
				if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
				    &ptb->Sflag, sizeof(ptb->Sflag)) == -1)
					warn("set send socket buffer size");
			}
			if (connect(sock, ai->ai_addr, ai->ai_addrlen) != 0) {
				if (ai->ai_next == NULL)
					err(1, "connect");
				if (ptb->vflag)
					warn("connect");
				close(sock);
				sock = -1;
				continue;
			}
			break;
		}
		if (sock == -1)
			errx(1, "No host found");
		set_socket_unblock(sock);
		/* Alloc and prepare stats */
		if (TCP_MODE) {
			if ((sc = calloc(1, sizeof(*sc))) == NULL)
				err(1, "calloc");
		}
		sc->fd = sock;
		stats_prepare(sc);
		event_set(&sc->ev, sc->fd, EV_WRITE | EV_PERSIST,
		    client_handle_sc, sc);
		event_add(&sc->ev, NULL);
		TAILQ_INSERT_TAIL(&sc_queue, sc, entry);
		mainstats.nconns++;
		if (mainstats.nconns == 1)
			set_slice_timer(1);
	}
	if (!ptb->Uflag)
		freeaddrinfo(aitop);
	if (aib != NULL)
		freeaddrinfo(aib);

	if (ptb->vflag && nconn > 1)
		fprintf(stderr, "%d connections established\n",
		    mainstats.nconns);
}

static void
set_socket_unblock(int sock)
{
	int r;
	if ((r = fcntl(sock, F_GETFL)) == -1)
		err(1, "fcntl(F_GETFL)");
	r |= O_NONBLOCK;
	if (fcntl(sock, F_SETFL, r) == -1)
		err(1, "fcntl(F_SETFL, O_NONBLOCK)");
}

static void
quit(int sig, short event, void *arg)
{
	exit(0);
}

int
main(int argc, char **argv)
{
	struct timeval tv;
	unsigned int secs;

	struct addrinfo *aitop, *aib, hints;
	const char *errstr;
	struct rlimit rl;
	int ch, herr, nconn;
	int family = PF_INET;
	const char *host = NULL, *port = DEFAULT_PORT, *srcbind = NULL;
	struct event ev_sigint, ev_sigterm, ev_sighup, ev_progtimer;
	struct statctx *udp_sc = NULL;
	struct sockaddr_un sock_un;
	int random_fd = -1;

	/* Init world */
	setvbuf(stdout, NULL, _IOLBF, 0);
	ptb = &tcpbench;
	ptb->dummybuf_len = 0;
	ptb->Sflag = ptb->sflag = ptb->vflag = ptb->Uflag = 0;
	ptb->rflag = DEFAULT_STATS_INTERVAL;
	nconn = 1;
	aib = NULL;
	secs = 0;

	while ((ch = getopt(argc, argv, "46b:B:hn:p:r:sS:t:uUv")) != -1) {
		switch (ch) {
		case '4':
			family = PF_INET;
			break;
		case '6':
			family = PF_INET6;
			break;
		case 'b':
			srcbind = optarg;
			break;
		case 'r':
			ptb->rflag = strtonum(optarg, 0, 60 * 60 * 24 * 1000,
			    &errstr);
			if (errstr != NULL)
				errx(1, "statistics interval is %s: %s",
				    errstr, optarg);
			break;
		case 'p':
			port = optarg;
			break;
		case 's':
			ptb->sflag = 1;
			break;
		case 'S':
			ptb->Sflag = strtonum(optarg, 0, 1024*1024*1024,
			    &errstr);
			if (errstr != NULL)
				errx(1, "socket buffer size is %s: %s",
				    errstr, optarg);
			break;
		case 'B':
			ptb->dummybuf_len = strtonum(optarg, 0, 1024*1024*1024,
			    &errstr);
			if (errstr != NULL)
				errx(1, "read/write buffer size is %s: %s",
				    errstr, optarg);
			break;
		case 'v':
			ptb->vflag++;
			break;
		case 'n':
			nconn = strtonum(optarg, 0, 65535, &errstr);
			if (errstr != NULL)
				errx(1, "number of connections is %s: %s",
				    errstr, optarg);
			break;
		case 'u':
			ptb->uflag = 1;
			break;
		case 'U':
			ptb->Uflag = 1;
			break;
		case 't':
			secs = strtonum(optarg, 1, UINT_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "secs is %s: %s",
				    errstr, optarg);
			break;
		case 'h':
		default:
			usage();
		}
	}

	argv += optind;
	argc -= optind;
	if ((argc != (ptb->sflag && !ptb->Uflag ? 0 : 1)) ||
	    (UDP_MODE && nconn != 1))
		usage();

	drop_gid();

	if (!ptb->sflag || ptb->Uflag)
		host = argv[0];
	/*
	 * Rationale,
	 * If TCP, use a big buffer with big reads/writes.
	 * If UDP, use a big buffer in server and a buffer the size of a
	 * ethernet packet.
	 */
	if (!ptb->dummybuf_len) {
		if (ptb->sflag || TCP_MODE) 
			ptb->dummybuf_len = DEFAULT_BUF;
		else
			ptb->dummybuf_len = DEFAULT_UDP_PKT;
	}

	bzero(&hints, sizeof(hints));
	hints.ai_family = family;
	if (UDP_MODE) {
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
	} else {
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
	}
	if (ptb->Uflag) {
		hints.ai_family = AF_UNIX;
		hints.ai_protocol = 0;
		sock_un.sun_family = AF_UNIX;
		bzero(sock_un.sun_path, sizeof(sock_un.sun_path));
		strncpy(sock_un.sun_path, host, sizeof(sock_un.sun_path));
		if (sock_un.sun_path[sizeof(sock_un.sun_path) - 1] != '\0')
			errx(1, "socket name '%s' too long", host);
		hints.ai_addr = (struct sockaddr *)&sock_un;
		hints.ai_addrlen = sizeof(sock_un);
		aitop = &hints;
	} else {
		if (ptb->sflag)
			hints.ai_flags = AI_PASSIVE;
		if (srcbind != NULL) {
			hints.ai_flags |= AI_NUMERICHOST;
			herr = getaddrinfo(srcbind, NULL, &hints, &aib);
			hints.ai_flags &= ~AI_NUMERICHOST;
			if (herr != 0) {
				if (herr == EAI_SYSTEM)
					err(1, "getaddrinfo");
				else
					errx(1, "getaddrinfo: %s",
					    gai_strerror(herr));
			}
		}
		if ((herr = getaddrinfo(host, port, &hints, &aitop)) != 0) {
			if (herr == EAI_SYSTEM)
				err(1, "getaddrinfo");
			else
				errx(1, "getaddrinfo: %s", gai_strerror(herr));
		}
	}

	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		err(1, "getrlimit");
	if (rl.rlim_cur < MAX_FD)
		rl.rlim_cur = MAX_FD;
	if (setrlimit(RLIMIT_NOFILE, &rl))
		err(1, "setrlimit");
	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		err(1, "getrlimit");

	/* Init world */
	TAILQ_INIT(&sc_queue);
	if ((ptb->dummybuf = malloc(ptb->dummybuf_len)) == NULL)
		err(1, "malloc");

	random_fd = open("/dev/urandom", O_RDONLY);
	if (random_fd == -1)
		err(1, "open");
	if (read(random_fd, ptb->dummybuf, ptb->dummybuf_len) !=
	    ptb->dummybuf_len) {
		err(1, "read");
	}
	close(random_fd);

	/* Setup libevent and signals */
	event_init();
	signal_set(&ev_sigterm, SIGTERM, signal_handler, NULL);
	signal_set(&ev_sighup, SIGHUP, signal_handler, NULL);
	signal_set(&ev_sigint, SIGINT, signal_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sighup, NULL);
	signal(SIGPIPE, SIG_IGN);
	
	if (UDP_MODE) {
		if ((udp_sc = calloc(1, sizeof(*udp_sc))) == NULL)
			err(1, "calloc");
		udp_sc->fd = -1;
		stats_prepare(udp_sc);
		evtimer_set(&mainstats.timer, udp_process_slice, udp_sc);
	} else {
		print_tcp_header();
		evtimer_set(&mainstats.timer, tcp_process_slice, NULL);
	}

	if (ptb->sflag)
		server_init(aitop, udp_sc);
	else {
		if (secs > 0) {
			timerclear(&tv);
			tv.tv_sec = secs + 1;
			evtimer_set(&ev_progtimer, quit, NULL);
			evtimer_add(&ev_progtimer, &tv);
		}
		client_init(aitop, nconn, udp_sc, aib);
	}

	/* libevent main loop*/
	event_dispatch();

	return (0);
}
