/*	$OpenBSD: log.c,v 1.15 2015/07/21 04:52:29 renato Exp $ */

/*
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "ldpd.h"
#include "lde.h"
#include "log.h"

static const char * const procnames[] = {
	"parent",
	"ldpe",
	"lde"
};

int	debug;
int	verbose;

void
log_init(int n_debug)
{
	extern char	*__progname;

	debug = n_debug;

	if (!debug)
		openlog(__progname, LOG_PID | LOG_NDELAY, LOG_DAEMON);

	tzset();
}

void
log_verbose(int v)
{
	verbose = v;
}

void
logit(int pri, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vlog(pri, fmt, ap);
	va_end(ap);
}

void
vlog(int pri, const char *fmt, va_list ap)
{
	char	*nfmt;

	if (debug) {
		/* best effort in out of mem situations */
		if (asprintf(&nfmt, "%s\n", fmt) == -1) {
			vfprintf(stderr, fmt, ap);
			fprintf(stderr, "\n");
		} else {
			vfprintf(stderr, nfmt, ap);
			free(nfmt);
		}
		fflush(stderr);
	} else
		vsyslog(pri, fmt, ap);
}

void
log_warn(const char *emsg, ...)
{
	char	*nfmt;
	va_list	 ap;

	/* best effort to even work in out of memory situations */
	if (emsg == NULL)
		logit(LOG_CRIT, "%s", strerror(errno));
	else {
		va_start(ap, emsg);

		if (asprintf(&nfmt, "%s: %s", emsg, strerror(errno)) == -1) {
			/* we tried it... */
			vlog(LOG_CRIT, emsg, ap);
			logit(LOG_CRIT, "%s", strerror(errno));
		} else {
			vlog(LOG_CRIT, nfmt, ap);
			free(nfmt);
		}
		va_end(ap);
	}
}

void
log_warnx(const char *emsg, ...)
{
	va_list	 ap;

	va_start(ap, emsg);
	vlog(LOG_CRIT, emsg, ap);
	va_end(ap);
}

void
log_info(const char *emsg, ...)
{
	va_list	 ap;

	va_start(ap, emsg);
	vlog(LOG_INFO, emsg, ap);
	va_end(ap);
}

void
log_debug(const char *emsg, ...)
{
	va_list	 ap;

	if (verbose & LDPD_OPT_VERBOSE) {
		va_start(ap, emsg);
		vlog(LOG_DEBUG, emsg, ap);
		va_end(ap);
	}
}

void
fatal(const char *emsg)
{
	if (emsg == NULL)
		logit(LOG_CRIT, "fatal in %s: %s", procnames[ldpd_process],
		    strerror(errno));
	else
		if (errno)
			logit(LOG_CRIT, "fatal in %s: %s: %s",
			    procnames[ldpd_process], emsg, strerror(errno));
		else
			logit(LOG_CRIT, "fatal in %s: %s",
			    procnames[ldpd_process], emsg);

	if (ldpd_process == PROC_MAIN)
		exit(1);
	else				/* parent copes via SIGCHLD */
		_exit(1);
}

void
fatalx(const char *emsg)
{
	errno = 0;
	fatal(emsg);
}

/* names */
const char *
nbr_state_name(int state)
{
	switch (state) {
	case NBR_STA_PRESENT:
		return ("PRESENT");
	case NBR_STA_INITIAL:
		return ("INITIALIZED");
	case NBR_STA_OPENREC:
		return ("OPENREC");
	case NBR_STA_OPENSENT:
		return ("OPENSENT");
	case NBR_STA_OPER:
		return ("OPERATIONAL");
	default:
		return ("UNKNW");
	}
}

const char *
if_state_name(int state)
{
	switch (state) {
	case IF_STA_DOWN:
		return ("DOWN");
	case IF_STA_ACTIVE:
		return ("ACTIVE");
	default:
		return ("UNKNW");
	}
}

const char *
if_type_name(enum iface_type type)
{
	switch (type) {
	case IF_TYPE_POINTOPOINT:
		return ("POINTOPOINT");
	case IF_TYPE_BROADCAST:
		return ("BROADCAST");
	}
	/* NOTREACHED */
	return ("UNKNOWN");
}

const char *
notification_name(u_int32_t status)
{
	static char buf[16];

	switch (status) {
	case S_SUCCESS:
		return ("Success");
	case S_BAD_LDP_ID:
		return ("Bad LDP Identifier");
	case S_BAD_PROTO_VER:
		return ("Bad Protocol Version");
	case S_BAD_PDU_LEN:
		return ("Bad PDU Length");
	case S_UNKNOWN_MSG:
		return ("Unknown Message Type");
	case S_BAD_MSG_LEN:
		return ("Bad Message Length");
	case S_UNKNOWN_TLV:
		return ("Unknown TLV");
	case S_BAD_TLV_LEN:
		return ("Bad TLV Length");
	case S_BAD_TLV_VAL:
		return ("Malformed TLV Value");
	case S_HOLDTIME_EXP:
		return ("Hold Timer Expired");
	case S_SHUTDOWN:
		return ("Shutdown");
	case S_LOOP_DETECTED:
		return ("Loop Detected");
	case S_UNKNOWN_FEC:
		return ("Unknown FEC");
	case S_NO_ROUTE:
		return ("No Route");
	case S_NO_LABEL_RES:
		return ("No Label Resources");
	case S_AVAILABLE:
		return ("Label Resources Available");
	case S_NO_HELLO:
		return ("Session Rejected, No Hello");
	case S_PARM_ADV_MODE:
		return ("Rejected Advertisement Mode Parameter");
	case S_MAX_PDU_LEN:
		return ("Rejected Max PDU Length Parameter");
	case S_PARM_L_RANGE:
		return ("Rejected Label Range Parameter");
	case S_KEEPALIVE_TMR:
		return ("KeepAlive Timer Expired");
	case S_LAB_REQ_ABRT:
		return ("Label Request Aborted");
	case S_MISS_MSG:
		return ("Missing Message Parameters");
	case S_UNSUP_ADDR:
		return ("Unsupported Address Family");
	case S_KEEPALIVE_BAD:
		return ("Bad KeepAlive Time");
	case S_INTERN_ERR:
		return ("Internal Error");
	case S_ILLEGAL_CBIT:
		return ("Illegal C-Bit");
	case S_WRONG_CBIT:
		return ("Wrong C-Bit");
	case S_INCPT_BITRATE:
		return ("Incompatible bit-rate");
	case S_CEP_MISCONF:
		return ("CEP-TDM mis-configuration");
	case S_PW_STATUS:
		return ("PW Status");
	case S_UNASSIGN_TAI:
		return ("Unassigned/Unrecognized TAI");
	case S_MISCONF_ERR:
		return ("Generic Misconfiguration Error");
	case S_WITHDRAW_MTHD:
		return ("Label Withdraw PW Status Method");
	default:
		snprintf(buf, sizeof(buf), "[%08x]", status);
		return (buf);
	}
}

const char *
pw_type_name(u_int16_t pw_type)
{
	static char buf[64];

	switch (pw_type) {
	case PW_TYPE_ETHERNET_TAGGED:
		return ("Eth Tagged");
	case PW_TYPE_ETHERNET:
		return ("Ethernet");
	default:
		snprintf(buf, sizeof(buf), "[%0x]", pw_type);
		return (buf);
	}
}

const char *
log_map(struct map *map)
{
	static char	buf[64];
	char		pstr[64];

	switch (map->type) {
	case FEC_WILDCARD:
		if (snprintf(buf, sizeof(buf), "wildcard"))
			return ("???");
		break;
	case FEC_PREFIX:
		if (snprintf(buf, sizeof(buf), "%s/%u",
		    inet_ntop(AF_INET, &map->fec.ipv4.prefix, pstr,
		    sizeof(pstr)), map->fec.ipv4.prefixlen) == -1)
			return ("???");
		break;
	case FEC_PWID:
		if (snprintf(buf, sizeof(buf), "pwid %u (%s)",
		    map->fec.pwid.pwid,
		    pw_type_name(map->fec.pwid.type)) == -1)
			return ("???");
		break;
	default:
		return ("???");
	}

	return (buf);
}

const char *
log_fec(struct fec *fec)
{
	static char	buf[64];
	char		pstr[32];

	switch (fec->type) {
	case FEC_TYPE_IPV4:
		if (snprintf(buf, sizeof(buf), "%s/%u",
		    inet_ntop(AF_INET, &fec->u.ipv4.prefix, pstr,
		    sizeof(pstr)), fec->u.ipv4.prefixlen) == -1)
			return ("???");
		break;
	case FEC_TYPE_PWID:
		if (snprintf(buf, sizeof(buf),
		    "pwid %u (%s) - %s",
		    fec->u.pwid.pwid, pw_type_name(fec->u.pwid.type),
		    inet_ntoa(fec->u.pwid.nexthop)) == -1)
			return ("???");
		break;
	default:
		return ("???");
	}

	return (buf);
}

static char *msgtypes[] = {
	"",
	"RTM_ADD: Add Route",
	"RTM_DELETE: Delete Route",
	"RTM_CHANGE: Change Metrics or flags",
	"RTM_GET: Report Metrics",
	"RTM_LOSING: Kernel Suspects Partitioning",
	"RTM_REDIRECT: Told to use different route",
	"RTM_MISS: Lookup failed on this address",
	"RTM_LOCK: fix specified metrics",
	"RTM_OLDADD: caused by SIOCADDRT",
	"RTM_OLDDEL: caused by SIOCDELRT",
	"RTM_RESOLVE: Route created by cloning",
	"RTM_NEWADDR: address being added to iface",
	"RTM_DELADDR: address being removed from iface",
	"RTM_IFINFO: iface status change",
	"RTM_IFANNOUNCE: iface arrival/departure",
	"RTM_DESYNC: route socket overflow",
};

void
log_rtmsg(u_char rtm_type)
{
	if (!(verbose & LDPD_OPT_VERBOSE2))
		return;

	if (rtm_type > 0 &&
	    rtm_type < sizeof(msgtypes)/sizeof(msgtypes[0]))
		log_debug("rtmsg_process: %s", msgtypes[rtm_type]);
	else
		log_debug("rtmsg_process: rtm_type %d out of range",
		    rtm_type);
}
