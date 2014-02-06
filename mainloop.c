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

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openconnect-internal.h"

void queue_packet(struct pkt **q, struct pkt *new)
{
	while (*q)
		q = &(*q)->next;

	new->next = NULL;
	*q = new;
}

int queue_new_packet(struct pkt **q, void *buf, int len)
{
	struct pkt *new = malloc(sizeof(struct pkt) + len);
	if (!new)
		return -ENOMEM;

	new->len = len;
	new->next = NULL;
	memcpy(new->data, buf, len);
	queue_packet(q, new);
	return 0;
}

/* Return value:
 *  = 0, when successfully paused (may call again)
 *  = -EINTR, if aborted locally via cmd_fd
 *  = -EPIPE, if the remote end explicitly terminated the session
 *  = -EPERM, if the gateway sent 401 Unauthorized (cookie expired)
 *  < 0, for any other error
 */
int openconnect_mainloop(struct openconnect_info *vpninfo,
			 int reconnect_timeout,
			 int reconnect_interval)
{
	int ret = 0;

	vpninfo->reconnect_timeout = reconnect_timeout;
	vpninfo->reconnect_interval = reconnect_interval;

	if (vpninfo->cmd_fd != -1) {
		FD_SET(vpninfo->cmd_fd, &vpninfo->select_rfds);
		if (vpninfo->cmd_fd >= vpninfo->select_nfds)
			vpninfo->select_nfds = vpninfo->cmd_fd + 1;
	}

	while (!vpninfo->quit_reason) {
		int did_work = 0;
		int timeout = INT_MAX;
		struct timeval tv;
		fd_set rfds, wfds, efds;

#ifdef HAVE_DTLS
		if (vpninfo->new_dtls_ssl)
			dtls_try_handshake(vpninfo);

		if (vpninfo->dtls_attempt_period && !vpninfo->dtls_ssl && !vpninfo->new_dtls_ssl &&
		    vpninfo->new_dtls_started + vpninfo->dtls_attempt_period < time(NULL) &&
		    vpninfo->ssl_fd != -1) {
			vpn_progress(vpninfo, PRG_TRACE, _("Attempt new DTLS connection\n"));
			connect_dtls_socket(vpninfo);
		}
		if (vpninfo->dtls_ssl) {
			ret = dtls_mainloop(vpninfo, &timeout);
			did_work += ret;
		}
#endif
		if (vpninfo->quit_reason)
			break;

		ret = cstp_mainloop(vpninfo, &timeout);
		if (vpninfo->quit_reason)
			break;
		did_work += ret;

		/* Tun must be last because it will set/clear its bit
		   in the select_rfds according to the queue length */
		did_work += tun_mainloop(vpninfo, &timeout);
		if (vpninfo->quit_reason)
			break;

		poll_cmd_fd(vpninfo, 0);
		if (vpninfo->got_cancel_cmd) {
			vpninfo->quit_reason = "Aborted by caller";
			ret = -EINTR;
			break;
		}
		if (vpninfo->got_pause_cmd) {
			/* close all connections and wait for the user to call
			   openconnect_mainloop() again */
			openconnect_close_https(vpninfo, 0);
			dtls_close(vpninfo, 1);
			vpninfo->new_dtls_started = 0;

			vpninfo->got_pause_cmd = 0;
			vpn_progress(vpninfo, PRG_INFO, _("Caller paused the connection\n"));
			return 0;
		}

		if (did_work)
			continue;

		vpn_progress(vpninfo, PRG_TRACE,
			     _("No work to do; sleeping for %d ms...\n"), timeout);
		memcpy(&rfds, &vpninfo->select_rfds, sizeof(rfds));
		memcpy(&wfds, &vpninfo->select_wfds, sizeof(wfds));
		memcpy(&efds, &vpninfo->select_efds, sizeof(efds));

		tv.tv_sec = timeout / 1000;
		tv.tv_usec = (timeout % 1000) * 1000;

		select(vpninfo->select_nfds, &rfds, &wfds, &efds, &tv);
	}

	cstp_bye(vpninfo, vpninfo->quit_reason);

	shutdown_tun(vpninfo);
	return ret < 0 ? ret : -EIO;
}

/* Called when the socket is unwritable, to get the deadline for DPD.
   Returns 1 if DPD deadline has already arrived. */
int ka_stalled_action(struct keepalive_info *ka, int *timeout)
{
	time_t due, now = time(NULL);

	if (ka->rekey) {
		due = ka->last_rekey + ka->rekey;

		if (now >= due)
			return KA_REKEY;

		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	if (!ka->dpd)
		return KA_NONE;

	due = ka->last_rx + (2 * ka->dpd);

	if (now > due)
		return KA_DPD_DEAD;

	if (*timeout > (due - now) * 1000)
		*timeout = (due - now) * 1000;

	return KA_NONE;
}


int keepalive_action(struct keepalive_info *ka, int *timeout)
{
	time_t now = time(NULL);

	if (ka->rekey) {
		time_t due = ka->last_rekey + ka->rekey;

		if (now >= due)
			return KA_REKEY;

		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	/* DPD is bidirectional -- PKT 3 out, PKT 4 back */
	if (ka->dpd) {
		time_t due = ka->last_rx + ka->dpd;
		time_t overdue = ka->last_rx + (2 * ka->dpd);

		/* Peer didn't respond */
		if (now > overdue)
			return KA_DPD_DEAD;

		/* If we already have DPD outstanding, don't flood. Repeat by
		   all means, but only after half the DPD period. */
		if (ka->last_dpd > ka->last_rx)
			due = ka->last_dpd + ka->dpd / 2;

		/* We haven't seen a packet from this host for $DPD seconds.
		   Prod it to see if it's still alive */
		if (now >= due) {
			ka->last_dpd = now;
			return KA_DPD;
		}
		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	/* Keepalive is just client -> server */
	if (ka->keepalive) {
		time_t due = ka->last_tx + ka->keepalive;

		/* If we haven't sent anything for $KEEPALIVE seconds, send a
		   dummy packet (which the server will discard) */
		if (now >= due)
			return KA_KEEPALIVE;

		if (*timeout > (due - now) * 1000)
			*timeout = (due - now) * 1000;
	}

	return KA_NONE;
}
