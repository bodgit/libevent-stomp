/*
 * Copyright (c) 2013 Matt Dainty <matt@bodgit-n-scarper.com>
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

#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>

#include "stomp.h"

int				 stomp_connected(struct stomp_connection *,
				    struct stomp_frame *);
int				 stomp_message(struct stomp_connection *,
				    struct stomp_frame *);
int				 stomp_receipt(struct stomp_connection *,
				    struct stomp_frame *);
int				 stomp_error(struct stomp_connection *,
				    struct stomp_frame *);
struct stomp_header		*stomp_header_new(void);
void				 stomp_headers_destroy(struct stomp_headers *);
void				 stomp_heartbeat(int, short, void *);
void				 stomp_timeout(int, short, void *);
struct stomp_subscription	*stomp_subscription_find(struct stomp_connection *,
				    char *);
void				 stomp_frame_publish(struct stomp_connection *,
				    struct stomp_frame *);
void				 stomp_frame_receive(struct stomp_connection *,
				    struct stomp_frame *);
void				 stomp_read(struct bufferevent *, void *);
void				 stomp_event(struct bufferevent *, short,
				    void *);
void				 stomp_count_rx(struct evbuffer *,
				    const struct evbuffer_cb_info *, void *);
void				 stomp_count_tx(struct evbuffer *,
				    const struct evbuffer_cb_info *, void *);
void				 stomp_reconnect(int, short, void *);
void				 stomp_shutdown(struct stomp_connection *);

/* Server frame dispatch table */
int	(*stomp_server_dispatch[SERVER_MAX_COMMAND])(struct stomp_connection *,
	    struct stomp_frame *) = {
	stomp_connected,
	stomp_message,
	stomp_receipt,
	stomp_error
};

char *stomp_server_commands[SERVER_MAX_COMMAND] = {
	"CONNECTED",
	"MESSAGE",
	"RECEIPT",
	"ERROR"
};

char *stomp_client_commands[CLIENT_MAX_COMMAND] = {
	"SEND",
	"SUBSCRIBE",
	"UNSUBSCRIBE",
	"BEGIN",
	"COMMIT",
	"ABORT",
	"ACK",
	"NACK",
	"DISCONNECT",
	"CONNECT",
	"STOMP"
};

struct event_base	*base;

struct stomp_header *
stomp_header_new(void)
{
	return (calloc(1, sizeof(struct stomp_header)));
}

void
stomp_headers_destroy(struct stomp_headers *headers)
{
	struct stomp_header	*header;

	while (!TAILQ_EMPTY(headers)) {
		header = TAILQ_FIRST(headers);
		TAILQ_REMOVE(headers, header, entry);

		/* From the STOMP specification, it implies that a header
		 * value is optional
		 */
		free(header->name);
		if (header->value)
			free(header->value);
		free(header);
	}
}

void
stomp_timeout(int fd, short event, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;

	stomp_shutdown(connection);

	if (connection->disconnectcb)
		connection->disconnectcb(connection, connection->arg);

	stomp_connect(connection);
}

void
stomp_heartbeat(int fd, short event, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;

	evbuffer_add_printf(bufferevent_get_output(connection->bev), "\r\n");

	/* Schedule next keepalive */
	evtimer_add(connection->heartbeat_ev, &connection->heartbeat_tv);
}

struct stomp_header *
stomp_frame_header_find(struct stomp_frame *frame, char *name)
{
	struct stomp_header	*header = NULL;

	/* Return the first match if there are multiple headers with the same
	 * requested name, as per the STOMP specification
	 */
	for (header = TAILQ_FIRST(&frame->headers); header;
	    header = TAILQ_NEXT(header, entry))
		if (!strcmp(header->name, name))
			break;

	return (header);
}

struct stomp_subscription *
stomp_subscription_find(struct stomp_connection *connection, char *id)
{
	struct stomp_subscription	*subscription = NULL;

	for (subscription = TAILQ_FIRST(&connection->subscriptions);
	    subscription; subscription = TAILQ_NEXT(subscription, entry))
		if (!strcmp(subscription->id, id))
			break;

	return (subscription);
}

void
stomp_frame_publish(struct stomp_connection *connection,
    struct stomp_frame *frame)
{
	struct stomp_header	*header;

	/* Disable any pending heartbeat as we're about to send some data */
	if (connection->heartbeat_ev &&
	    evtimer_pending(connection->heartbeat_ev, NULL))
		evtimer_del(connection->heartbeat_ev);

	/* Send command */
	evbuffer_add_printf(bufferevent_get_output(connection->bev),
	    "%s\r\n", stomp_client_commands[frame->command]);

	/* Send headers */
	for (header = TAILQ_FIRST(&frame->headers); header;
	    header = TAILQ_NEXT(header, entry))
		evbuffer_add_printf(bufferevent_get_output(connection->bev),
		    "%s:%s\r\n", header->name, header->value);
	evbuffer_add_printf(bufferevent_get_output(connection->bev), "\r\n");

	/* Send body */
	if (frame->body)
		evbuffer_add_printf(bufferevent_get_output(connection->bev),
		    "%s", frame->body);

	/* Send NUL */
	evbuffer_add_printf(bufferevent_get_output(connection->bev),
	    "%c", '\0');

	/* Track frame Tx */
	connection->frames_tx++;

	/* Set up a pending heartbeat if we're configured to send one */
	if (connection->heartbeat_ev)
		evtimer_add(connection->heartbeat_ev,
		    &connection->heartbeat_tv);
}

void
stomp_frame_receive(struct stomp_connection *connection,
    struct stomp_frame *frame)
{
	if (frame->command < SERVER_MAX_COMMAND)
		if (stomp_server_dispatch[frame->command](connection,
		    frame) < 0)
			return;

	/* Track frame Rx */
	connection->frames_rx++;
}

void
stomp_read(struct bufferevent *bev, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;
	size_t			 n;
	struct evbuffer		*input = bufferevent_get_input(bev);
	struct evbuffer_ptr	 p, p2;
	struct stomp_header	*header;
	size_t			 length;

	/* Cancel inactivity timeout */
	if (connection->timeout_ev &&
	    evtimer_pending(connection->timeout_ev, NULL))
		evtimer_del(connection->timeout_ev);

	while (evbuffer_get_length(input))
		switch (connection->state) {
		case STOMP_FRAME_OR_KEEPALIVE: /* Frame from server or EOL */
			p = evbuffer_search_eol(input, NULL, &n,
			    EVBUFFER_EOL_CRLF);

			if (p.pos < 0)
				goto loop;

			if (p.pos > 0) {
				int i;
				unsigned char *command = evbuffer_pullup(input, p.pos);
				for (i = 0; i < SERVER_MAX_COMMAND; i++)
					if (!strncmp(stomp_server_commands[i],
					    (char *)command, p.pos))
						break;

				/* Invalid command */
				if (i == SERVER_MAX_COMMAND)
					goto loop;

				connection->frame.command = i;
				evbuffer_drain(input, p.pos);
				connection->state = STOMP_FRAME_HEADERS;
			} // else heartbeat
			evbuffer_drain(input, n);
			break;
		case STOMP_FRAME_HEADERS: /* Zero or more headers */
			p = evbuffer_search_eol(input, NULL, &n,
			    EVBUFFER_EOL_CRLF);

			if (p.pos < 0)
				goto loop;

			if (p.pos > 0) {
				/* Find the ':' between header name:value */
				p2 = evbuffer_search_range(input, ":", 1, NULL,
				    &p);

				/* Invalid header */
				if (p2.pos <= 0)
					goto loop;

				header = calloc(1, sizeof(struct stomp_header));
				header->name = calloc(1, p2.pos + 1);
				evbuffer_remove(input, header->name, p2.pos);

				/* Remove ':' */
				evbuffer_drain(input, 1);

				header->value = calloc(1, p.pos - p2.pos);
				evbuffer_remove(input, header->value, p.pos - p2.pos - 1);

				TAILQ_INSERT_TAIL(&connection->frame.headers,
				    header, entry);
			} else {
				connection->state = STOMP_FRAME_BODY;
			}
			evbuffer_drain(input, n);
			break;
		case STOMP_FRAME_BODY: /* Optional frame body */
			/* Check for a content-length header */
			if ((header =
			    stomp_frame_header_find(&connection->frame,
			    "content-length")) != NULL)
				length = atoi(header->value);
			else
				length = 0;

#if defined(_EVENT_NUMERIC_VERSION) && _EVENT_NUMERIC_VERSION >= 0x02010100
			p = evbuffer_search_eol(input, NULL, &n,
			    EVBUFFER_EOL_NUL);
			//if (n != 1) ...
#else
			p = evbuffer_search(input, "\0", 1, NULL);
#endif

			if (p.pos < 0)
				goto loop;

			if (p.pos > 0) {
				/* If we don't have enough to match the
				 * content-length header value, wait for more
				 */
				if (length) {
					if (evbuffer_get_length(input) < length)
						goto loop;
				} else
					length = p.pos;

				connection->frame.body = calloc(length + 1,
				    sizeof(unsigned char));
				evbuffer_remove(input, connection->frame.body,
				    length);
			}

			evbuffer_drain(input, 1);

			/* We have a whole frame by now */
			stomp_frame_receive(connection, &connection->frame);

			/* Clear headers */
			stomp_headers_destroy(&connection->frame.headers);

			/* Free body */
			if (connection->frame.body) {
				free(connection->frame.body);
				connection->frame.body = NULL;
			}

			connection->state = STOMP_FRAME_OR_KEEPALIVE;
			break;
		}

loop:
	/* Reenable inactivity timeout (only if we're still connected) */
	if (connection->bev && connection->timeout_ev)
		evtimer_add(connection->timeout_ev, &connection->timeout_tv);

	return;
}

void
stomp_event(struct bufferevent *bev, short events, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	if (events & BEV_EVENT_CONNECTED) {
		int	 size;

		/* Reset backoff to immediate */
		connection->connect_index = 0;

		memset(&frame, 0, sizeof(frame));

		/* If we're only willing to accept version 1.2+ use the newer
		 * STOMP frame, otherwise use the traditional CONNECT frame
		 */
		if (connection->version_req &
		    (STOMP_VERSION_1_0|STOMP_VERSION_1_1))
			frame.command = CLIENT_CONNECT;
		else
			frame.command = CLIENT_STOMP;
		TAILQ_INIT(&frame.headers);

		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("accept-version");
		/* 'x.y' + (',x.y')* + '\0' */
		header->value = calloc(__builtin_popcount(connection->version_req) << 2, sizeof(char));
		if (connection->version_req & STOMP_VERSION_1_0)
			strcpy(header->value, "1.0");
		if (connection->version_req & STOMP_VERSION_1_1) {
			if (strlen(header->value))
				strcpy(header->value + strlen(header->value),
				    ",1.1");
			else
				strcpy(header->value, "1.1");
		}
		if (connection->version_req & STOMP_VERSION_1_2) {
			if (strlen(header->value))
				strcpy(header->value + strlen(header->value),
				    ",1.2");
			else
				strcpy(header->value, "1.2");
		}
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);

		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("host");
		header->value = strdup(connection->vhost);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);

		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("heart-beat");
		size = snprintf(NULL, 0, "%u,%u", connection->cx,
		    connection->cy);
		header->value = calloc(size + 1, sizeof(char));
		sprintf(header->value, "%u,%u", connection->cx,
		    connection->cy);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);

		stomp_frame_publish(connection, &frame);

		/* Clear down headers */
		stomp_headers_destroy(&frame.headers);
	} else if (events & (BEV_EVENT_ERROR|BEV_EVENT_EOF)) {
		if (events & BEV_EVENT_ERROR) {
			fprintf(stderr, "Error\n");
			int err = bufferevent_socket_get_dns_error(bev);
			if (err)
				fprintf(stderr, "DNS error: %s\n",
				    evutil_gai_strerror(err));
			unsigned long ssl = bufferevent_get_openssl_error(connection->bev);
			if (ssl)
				fprintf(stderr, "SSL error: %s\n",
				    ERR_error_string(ssl, NULL));
		}

		stomp_shutdown(connection);

		if ((events & BEV_EVENT_EOF) && connection->disconnectcb)
			connection->disconnectcb(connection, connection->arg);

		/* Schedule a reconnect attempt */
		evtimer_add(connection->connect_ev,
		    &connection->connect_tv[connection->connect_index]);

		/* If this attempt is after no delay, set the next attempt (and
		 * all subsequent ones) to be after a delay
		 */
		if (connection->connect_index == 0)
			connection->connect_index++;
	}

	if (c->dns) {
		evdns_base_free(c->dns, 1);
		c->dns = NULL;
	}
}

void
stomp_count_rx(struct evbuffer *buffer, const struct evbuffer_cb_info *info,
    void *arg)
{
	struct stomp_connection *connection = (struct stomp_connection *)arg;

	connection->bytes_rx += info->n_added;
}

void
stomp_count_tx(struct evbuffer *buffer, const struct evbuffer_cb_info *info,
    void *arg)
{
	struct stomp_connection *connection = (struct stomp_connection *)arg;

	connection->bytes_tx += info->n_deleted;
}

int
stomp_init(struct event_base *b)
{
#ifdef __APPLE__
	if (!strcmp(event_base_get_method(b), "kqueue"))
		return (-1);
#endif

	base = b;

	return (0);
}

void
stomp_send(struct stomp_connection *connection,
    char *destination, unsigned char *body,
    struct stomp_transaction *transaction)
{
	struct stomp_frame		 frame;
	struct stomp_header		*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_SEND;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("destination");
	header->value = strdup(destination);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	frame.body = body;

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	/* Track message Tx */
	connection->messages_tx++;
}

struct stomp_subscription *
stomp_subscription_new(struct stomp_connection *connection,
    char *destination, int ack)
{
	struct stomp_subscription	*subscription;
	size_t				 size;

	subscription = calloc(1, sizeof(struct stomp_subscription));
	size = snprintf(NULL, 0, "%lld", connection->subscription_id);
	subscription->id = calloc(size + 1, sizeof(char));
	sprintf(subscription->id, "%lld", connection->subscription_id++);

	subscription->destination = strdup(destination);

	subscription->ack = ack;

	return (subscription);
}

void
stomp_subscription_setcb(struct stomp_subscription *subscription,
    void (*callback)(struct stomp_connection *, struct stomp_subscription *,
    struct stomp_frame *, void *), void *arg)
{
	subscription->callback = callback;
	subscription->arg = arg;
}

void
stomp_subscribe(struct stomp_connection *connection,
    struct stomp_subscription *subscription)
{
	struct stomp_frame		 frame;
	struct stomp_header		*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_SUBSCRIBE;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("id");
	header->value = strdup(subscription->id);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("destination");
	header->value = strdup(subscription->destination);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("ack");

	switch (subscription->ack) {
	case STOMP_ACK_CLIENT:
		header->value = strdup("client");
		break;
	case STOMP_ACK_CLIENT_INDIVIDUAL:
		header->value = strdup("client-individual");
		break;
	case STOMP_ACK_AUTO:
		/* FALLTHROUGH */
	default:
		header->value = strdup("auto");
		break;
	}

	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	TAILQ_INSERT_TAIL(&connection->subscriptions, subscription, entry);
}

void
stomp_unsubscribe(struct stomp_connection *connection,
    struct stomp_subscription *subscription)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_UNSUBSCRIBE;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("id");
	header->value = strdup(subscription->id);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	TAILQ_REMOVE(&connection->subscriptions, subscription, entry);
}

void
stomp_subscription_free(struct stomp_subscription *subscription)
{
	free(subscription->destination);
	free(subscription->id);
	free(subscription);
}

struct stomp_transaction *
stomp_begin(struct stomp_connection *connection)
{
	struct stomp_frame		 frame;
	struct stomp_header		*header;
	struct stomp_transaction	*transaction;
	int				 size;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_BEGIN;
	TAILQ_INIT(&frame.headers);

	transaction = calloc(1, sizeof(struct stomp_transaction));
	size = snprintf(NULL, 0, "tx%lld", connection->transaction_id);
	transaction->id = calloc(size + 1, sizeof(char));
	sprintf(transaction->id, "tx%lld", connection->transaction_id++);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("transaction");
	header->value = strdup(transaction->id);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	TAILQ_INSERT_TAIL(&connection->transactions, transaction, entry);

	return (transaction);
}

void
stomp_commit(struct stomp_connection *connection,
    struct stomp_transaction *transaction)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_COMMIT;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("transaction");
	header->value = strdup(transaction->id);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	TAILQ_REMOVE(&connection->transactions, transaction, entry);
	free(transaction->id);
	free(transaction);
}

void
stomp_abort(struct stomp_connection *connection,
    struct stomp_transaction *transaction)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_ABORT;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("transaction");
	header->value = strdup(transaction->id);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	TAILQ_REMOVE(&connection->transactions, transaction, entry);
	free(transaction->id);
	free(transaction);
}

void
stomp_ack(struct stomp_connection *connection,
    struct stomp_subscription *subscription, char *ack,
    struct stomp_transaction *transaction)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_ACK;
	TAILQ_INIT(&frame.headers);

	/* Note the order here!
	 *
	 * - STOMP 1.0 sends just the "message-id" header set to the value of
	 *   the same named header from the to-be-acknowledged message.
	 * - STOMP 1.1 sends in addition to the above the "subscription" header
	 *   set to the value of the subscription id.
	 * - STOMP 1.2 throws all of that away and uses a dedicated "id"
	 *   header set to the value of the "ack" header from the 
	 *   to-be-acknowledged message.
	 *
	 * All versions of the specification can pass an optional transaction
	 * header
	 */
	switch (connection->version_neg) {
	case STOMP_VERSION_1_1:
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("subscription");
		header->value = strdup(subscription->id);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
		/* FALLTHROUGH */
	case STOMP_VERSION_1_0:
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("message-id");
		header->value = strdup(ack);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
		break;
	case STOMP_VERSION_1_2:
		/* FALLTHROUGH */
	default:
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("id");
		header->value = strdup(ack);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
		break;
	}

	if (transaction) {
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("transaction");
		header->value = strdup(transaction->id);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
	}

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);
}

void
stomp_nack(struct stomp_connection *connection,
    struct stomp_subscription *subscription, char *ack,
    struct stomp_transaction *transaction)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_NACK;
	TAILQ_INIT(&frame.headers);

	/* See the notes in stomp_ack() above, only difference is this frame
	 * isn't in the STOMP 1.0 specification so it's currently a no-op.
	 */
	switch (connection->version_neg) {
	case STOMP_VERSION_1_0:
		return;
		/* NOTREACHED */
	case STOMP_VERSION_1_1:
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("message-id");
		header->value = strdup(ack);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);

		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("subscription");
		header->value = strdup(subscription->id);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
		break;
	case STOMP_VERSION_1_2:
		/* FALLTHROUGH */
	default:
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("id");
		header->value = strdup(ack);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
		break;
	}

	if (transaction) {
		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("transaction");
		header->value = strdup(transaction->id);
		TAILQ_INSERT_TAIL(&frame.headers, header, entry);
	}

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);
}

void
stomp_disconnect(struct stomp_connection *connection)
{
	struct stomp_frame	 frame;
	//struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_DISCONNECT;
	TAILQ_INIT(&frame.headers);

#if 0
	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("receipt");
	header->value = strdup("77");
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);
#endif

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	stomp_shutdown(connection);

	if (connection->disconnectcb)
		connection->disconnectcb(connection, connection->arg);
}

void
stomp_reconnect(int fd, short event, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;

	connection->dns = evdns_base_new(base, 1);

	connection->bev = bufferevent_socket_new(base, -1,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);

	if (bufferevent_socket_connect_hostname(connection->bev,
	    connection->dns, AF_UNSPEC, connection->host,
	    connection->port) < 0) {
		/* Error starting connection */
		bufferevent_free(connection->bev);
		return;
	}

	/* SSL support */
	if (connection->ctx) {
		struct bufferevent	*bevssl;
		SSL			*ssl = SSL_new(connection->ctx);

		if ((bevssl = bufferevent_openssl_filter_new(base,
		    connection->bev, ssl, BUFFEREVENT_SSL_CONNECTING,
		    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS)) == NULL) {
			bufferevent_free(connection->bev);
			return;
		}
		connection->bev = bevssl;
	}

	bufferevent_setcb(connection->bev, stomp_read, NULL, stomp_event,
	    (void *)connection);
	bufferevent_enable(connection->bev, EV_READ|EV_WRITE);

	/* Add callbacks to the input & output buffers to track how much data
	 * we're receiving/transmitting
	 */
	evbuffer_add_cb(bufferevent_get_input(connection->bev), stomp_count_rx,
	    (void *)connection);
	evbuffer_add_cb(bufferevent_get_output(connection->bev), stomp_count_tx,
	    (void *)connection);
}

struct stomp_connection *
stomp_connection_new(char *host, unsigned short port, int version, char *vhost,
    SSL_CTX *ctx, struct timeval tv, int cx, int cy)
{
	struct stomp_connection	*connection;

	if ((connection = calloc(1, sizeof(struct stomp_connection))) != NULL) {
		/* Set initial state */
		connection->state = STOMP_FRAME_OR_KEEPALIVE;

		/* Set up timer to (re)connect */
		connection->connect_ev = evtimer_new(base, stomp_reconnect,
		    (void *)connection);
		connection->connect_tv[1] = tv;

		TAILQ_INIT(&connection->frame.headers);
		TAILQ_INIT(&connection->subscriptions);
		TAILQ_INIT(&connection->transactions);

		/* Hostname */
		connection->host = strdup(host);

		/* TCP port */
		connection->port = port;

		/* Requested version(s) */
		connection->version_req = version;

		/* vhost */
		connection->vhost = strdup(vhost);

		/* SSL */
		connection->ctx = ctx;

		/* Desired heartbeat rates */
		connection->cx = cx;
		connection->cy = cy;
	}

	return (connection);
}

void
stomp_connection_setcb(struct stomp_connection *connection,
    void (*connectcb)(struct stomp_connection *, struct stomp_frame *, void *),
    void (*receiptcb)(struct stomp_connection *, struct stomp_frame *, void *),
    void (*errorcb)(struct stomp_connection *, struct stomp_frame *, void *),
    void (*disconnectcb)(struct stomp_connection *, void *), void *arg)
{
	connection->connectcb = connectcb;
	connection->receiptcb = receiptcb;
	connection->errorcb = errorcb;
	connection->disconnectcb = disconnectcb;
	connection->arg = arg;
}

void
stomp_connect(struct stomp_connection *connection)
{
	evtimer_add(connection->connect_ev, &connection->connect_tv[0]);
	connection->connect_index = 1;
}

void
stomp_connection_free(struct stomp_connection *connection)
{
	free(connection);
}

int
stomp_connected(struct stomp_connection *connection, struct stomp_frame *frame)
{
	struct stomp_header	*header;
	int			 sx, sy;

	if ((header = stomp_frame_header_find(frame, "version")) != NULL) {
		if (!strcmp(header->value, "1.2")) {
			connection->version_neg = STOMP_VERSION_1_2;
		} else if (!strcmp(header->value, "1.1")) {
			connection->version_neg = STOMP_VERSION_1_1;
		} else if (!strcmp(header->value, "1.0")) {
			connection->version_neg = STOMP_VERSION_1_0;
		} else {
			fprintf(stderr, "Invalid version header\n");
			return (-1);
		}
	}
	if ((header = stomp_frame_header_find(frame, "heart-beat")) != NULL) {
		if (sscanf(header->value, "%u,%u", &sx, &sy) != 2) {
			fprintf(stderr, "Invalid heartbeat header\n");
			return (-1);
		}

		/* If client is willing to send heartbeats and server
		 * would like them, set up timer to try and send
		 * heartbeats every <n> milliseconds, reset any time
		 * we send a frame
		 */
		if (connection->cx && sy) {
			connection->heartbeat_ev = evtimer_new(base,
			    stomp_heartbeat, (void *)connection);
			connection->heartbeat_tv.tv_sec =
			    MAX(connection->cx, sy) / 1000;
			connection->heartbeat_tv.tv_usec =
			    (MAX(connection->cx, sy) % 1000) * 1000;
			evtimer_add(connection->heartbeat_ev,
			    &connection->heartbeat_tv);
		}

		/* If server is willing to send heartbeats and client
		 * would like them, set up a timeout to fire after
		 * <n>*2 milliseconds without any traffic
		 */
		if (sx && connection->cy) {
			connection->timeout_ev = evtimer_new(base,
			    stomp_timeout, (void *)connection);
			connection->timeout_tv.tv_sec =
			    (MAX(sx, connection->cy) << 1) / 1000;
			connection->timeout_tv.tv_usec =
			    ((MAX(sx, connection->cy) << 1) % 1000) *
			    1000;
			evtimer_add(connection->timeout_ev,
			    &connection->timeout_tv);
		}
	}

	if (connection->connectcb)
		connection->connectcb(connection, frame, connection->arg);

	return (0);
}

int
stomp_message(struct stomp_connection *connection, struct stomp_frame *frame)
{
	struct stomp_header		*header;
	struct stomp_subscription	*subscription;

	if ((header = stomp_frame_header_find(frame, "destination")) == NULL)
		return (-1);
	if ((header = stomp_frame_header_find(frame, "message-id")) == NULL)
		return (-1);
	if ((header = stomp_frame_header_find(frame, "subscription")) == NULL)
		return (-1);
	if ((subscription = stomp_subscription_find(connection,
	    header->value)) == NULL)
		return (-1);
	if (subscription->ack != STOMP_ACK_AUTO)
		if ((header = stomp_frame_header_find(frame, "ack")) == NULL)
			return (-1);

	if (subscription->callback)
		subscription->callback(connection, subscription, frame,
		    subscription->arg);

	/* Track message Rx */
	connection->messages_rx++;

	return (0);
}

int
stomp_receipt(struct stomp_connection *connection, struct stomp_frame *frame)
{
	if (connection->receiptcb)
		connection->receiptcb(connection, frame, connection->arg);

	return (0);
}

int
stomp_error(struct stomp_connection *connection, struct stomp_frame *frame)
{
	fprintf(stderr, "Error -> %s", frame->body);

	if (connection->errorcb)
		connection->errorcb(connection, frame, connection->arg);

	bufferevent_free(connection->bev);

	/* Schedule a reconnect attempt */
	evtimer_add(connection->connect_ev,
	    &connection->connect_tv[connection->connect_index]);

	return (0);
}

void
stomp_shutdown(struct stomp_connection *connection)
{
	/* Cancel heartbeat timers */
	if (connection->timeout_ev) {
		if (evtimer_pending(connection->timeout_ev, NULL))
			evtimer_del(connection->timeout_ev);
		event_free(connection->timeout_ev);
		connection->timeout_ev = NULL;
	}
	if (connection->heartbeat_ev) {
		if (evtimer_pending(connection->heartbeat_ev, NULL))
			evtimer_del(connection->heartbeat_ev);
		event_free(connection->heartbeat_ev);
		connection->heartbeat_ev = NULL;
	}

	/* Tear down connection */
	bufferevent_free(connection->bev);
	connection->bev = NULL;

	/* FIXME Need more free()'s here */
	if (connection->frame.body)
		free(connection->frame.body);
}
