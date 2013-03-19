/* libevent-based STOMP client */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/queue.h>
#include <sys/param.h>

#include <openssl/ssl.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>

#include "stomp.h"

#define	STOMP_VERSION_1_0		(1 << 0)
#define	STOMP_VERSION_1_1		(1 << 1)
#define	STOMP_VERSION_1_2		(1 << 2)
#define	STOMP_VERSION_ANY		(STOMP_VERSION_1_0|STOMP_VERSION_1_1|STOMP_VERSION_1_2)

#define	STOMP_ACK_AUTO			0
#define	STOMP_ACK_CLIENT		1
#define	STOMP_ACK_CLIENT_INDIVIDUAL	2

char *stomp_server_commands[SERVER_MAX_COMMAND] = {
	"CONNECTED",
	"MESSAGE",
	"RECEIPT",
	"ERROR"
};

enum stomp_client_command {
	CLIENT_SEND,
	CLIENT_SUBSCRIBE,
	CLIENT_UNSUBSCRIBE,
	CLIENT_BEGIN,
	CLIENT_COMMIT,
	CLIENT_ABORT,
	CLIENT_ACK,
	CLIENT_NACK,
	CLIENT_DISCONNECT,
	CLIENT_CONNECT,
	CLIENT_STOMP,
	CLIENT_MAX_COMMAND
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

struct stomp_transaction	*transaction;

struct stomp_header
*stomp_header_new(void)
{
	struct stomp_header	*header;

	if ((header = calloc(1, sizeof(struct stomp_header))) != NULL) {
	}

	return (header);
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

	fprintf(stderr, "Timeout\n");

	/* Cancel pending heartbeat */
	if (evtimer_pending(connection->heartbeat_ev, NULL))
		evtimer_del(connection->heartbeat_ev);
	/* Tear down connection */
	bufferevent_free(connection->bev);
	/* FIXME Need more free()'s here */
	if (connection->frame.body)
		free(connection->frame.body);
	free(connection);
}

void
stomp_heartbeat(int fd, short event, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;

	fprintf(stderr, "Sending heartbeat\n");
	evbuffer_add_printf(bufferevent_get_output(connection->bev), "\r\n");

	/* Schedule next keepalive */
	evtimer_add(connection->heartbeat_ev, &connection->heartbeat_tv);
}

struct stomp_header
*stomp_header_find(struct stomp_headers *headers, char *name)
{
	struct stomp_header	*header = NULL;

	/* Return the first match if there are multiple headers with the same
	 * requested name, as per the STOMP specification
	 */
	for (header = TAILQ_FIRST(headers); header;
	    header = TAILQ_NEXT(header, entry))
		if (!strcmp(header->name, name))
			break;

	return (header);
}

void
stomp_frame_publish(struct stomp_connection *connection,
    struct stomp_frame *frame)
{
	struct stomp_header	*header;

	fprintf(stderr, ">>> Frame -> %s\n", stomp_client_commands[frame->command]);

	for (header = TAILQ_FIRST(&frame->headers); header;
	    header = TAILQ_NEXT(header, entry))
		fprintf(stderr, ">>> Header -> %s = %s\n", header->name,
		    header->value);

	/* Dependent on negotiated version, check required headers, etc. */
	switch (frame->command) {
	case CLIENT_SEND:
		break;
	case CLIENT_SUBSCRIBE:
		if ((header = stomp_header_find(&frame->headers,
		    "id")) == NULL) {
			fprintf(stderr, "No id header\n");
			return;
		}
		if ((header = stomp_header_find(&frame->headers,
		    "destination")) == NULL) {
			fprintf(stderr, "No destination header\n");
			return;
		}
		break;
	case CLIENT_UNSUBSCRIBE:
		break;
	case CLIENT_BEGIN:
		/* FALLTHROUGH */
	case CLIENT_COMMIT:
		/* FALLTHROUGH */
	case CLIENT_ABORT:
		if ((header = stomp_header_find(&frame->headers,
		    "transaction")) == NULL) {
		}
		break;
	case CLIENT_ACK:
		/* FALLTHROUGH */
	case CLIENT_NACK:
		switch (connection->version) {
		case STOMP_VERSION_1_1:
			if ((header = stomp_header_find(&frame->headers,
			    "subscription")) == NULL) {
			}
			if ((header = stomp_header_find(&frame->headers,
			    "message-id")) == NULL) {
			}
			break;
		case STOMP_VERSION_1_2:
			if ((header = stomp_header_find(&frame->headers,
			    "id")) == NULL) {
			}
			break;
		}
		break;
	case CLIENT_DISCONNECT:
		break;
	case CLIENT_CONNECT:
		/* FALLTHROUGH */
	case CLIENT_STOMP:
		if ((header = stomp_header_find(&frame->headers,
		    "accept-version")) == NULL) {
			fprintf(stderr, "No accept-version header\n");
			return;
		}
		if ((header = stomp_header_find(&frame->headers,
		    "host")) == NULL) {
			fprintf(stderr, "No host header\n");
			return;
		}
		break;
	default:
		fprintf(stderr, "Unknown client command\n");
		return;
		/* NOT REACHED */
		break;
	}

	if (connection->heartbeat_ev &&
	    evtimer_pending(connection->heartbeat_ev, NULL))
		evtimer_del(connection->heartbeat_ev);

	/* Publish frame */

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
	    "%c\r\n", '\0');

	/* Track frame Tx */
	connection->frames_tx++;

	if (connection->heartbeat_ev)
		evtimer_add(connection->heartbeat_ev,
		    &connection->heartbeat_tv);
}

void
stomp_frame_receive(struct stomp_connection *connection,
    struct stomp_frame *frame)
{
	struct stomp_header	*header;

	/* Receive frame */
	fprintf(stderr, "<<< Frame -> %s\n", stomp_server_commands[frame->command]);

	for (header = TAILQ_FIRST(&frame->headers); header;
	    header = TAILQ_NEXT(header, entry))
		fprintf(stderr, "<<< Header -> %s = %s\n", header->name,
		    header->value);

	/* Dependent on negotiated version, check required headers, etc. */
	switch (frame->command) {
	case SERVER_CONNECTED:
		if ((header = stomp_header_find(&frame->headers,
		    "version")) != NULL) {
			if (!strcmp(header->value, "1.2")) {
				connection->version = STOMP_VERSION_1_2;
			} else if (!strcmp(header->value, "1.1")) {
				connection->version = STOMP_VERSION_1_1;
			} else if (!strcmp(header->value, "1.0")) {
				connection->version = STOMP_VERSION_1_0;
			} else {
				fprintf(stderr, "Invalid version header\n");
				/* FIXME handle error */
			}
		}
		if ((header = stomp_header_find(&frame->headers,
		    "heart-beat")) != NULL) {
			int	 sx, sy;

			if (sscanf(header->value, "%u,%u", &sx, &sy) != 2) {
				fprintf(stderr, "Invalid heartbeat header\n");
				/* FIXME handle error */
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
			connection->connectcb(connection);
		break;
	case SERVER_MESSAGE:
		if (connection->readcb)
			connection->readcb(connection, &connection->frame);
		break;
	case SERVER_RECEIPT:
	case SERVER_ERROR:
		fprintf(stderr, "Error -> %s", frame->body);
		break;
	default:
		break;
	}

	/* Track frame Rx */
	connection->frames_rx++;

#if 0
	/* Call user-defined callback */
	if (connection->readcb)
		connection->readcb(&connection->frame);
#endif
}

void
stomp_read(struct bufferevent *bev, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;
	size_t			 n;
	struct evbuffer		*input = bufferevent_get_input(bev);
	struct evbuffer_ptr	 p, p2;
	struct stomp_header	*header;
	int			 length;

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
			} else
				fprintf(stderr, "Received keepalive\n");
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

				struct stomp_header *header = calloc(1, sizeof(struct stomp_header));
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
			    stomp_header_find(&connection->frame.headers,
			    "content-length")) != NULL)
				length = atoi(header->value);
			else
				length = 0;

#if 0
			p = evbuffer_search_eol(input, NULL, &n,
			    EVBUFFER_EOL_NUL);
			if (n != 1) ...
#endif
			p = evbuffer_search(input, "\0", 1, NULL);

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
	/* Track how many bytes we've received */
	fprintf(stderr, "Bytes rx -> %lld\n", connection->bytes_rx);
	fprintf(stderr, "Bytes tx -> %lld\n", connection->bytes_tx);

	/* Reenable inactivity timeout */
	if (connection->timeout_ev)
		evtimer_add(connection->timeout_ev, &connection->timeout_tv);

	return;
}

void
eventcb(struct bufferevent *bev, short events, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	if (events & BEV_EVENT_CONNECTED) {
		int	 size;

		fprintf(stderr, "Connected (libevent)\n");

		/* Reset backoff to immediate */
		connection->connect_index = 0;

		memset(&frame, 0, sizeof(frame));
		frame.command = CLIENT_CONNECT;
		TAILQ_INIT(&frame.headers);

		header = calloc(1, sizeof(struct stomp_header));
		header->name = strdup("accept-version");
		header->value = strdup("1.2");
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
		fprintf(stderr, "Error\n");
		if (connection->timeout_ev &&
		    evtimer_pending(connection->timeout_ev, NULL))
			evtimer_del(connection->timeout_ev);
		if (connection->heartbeat_ev &&
		    evtimer_pending(connection->heartbeat_ev, NULL))
			evtimer_del(connection->heartbeat_ev);
		/* Tear down connection */
		bufferevent_free(connection->bev);
		/* FIXME Need more free()'s here */
		if (connection->frame.body)
			free(connection->frame.body);

		/* Schedule a reconnect attempt */
		evtimer_add(connection->connect_ev,
		    &connection->connect_tv[connection->connect_index]);

		/* If this attempt is after no delay, set the next attempt (and
		 * all subsequent ones) to be after a delay
		 */
		if (connection->connect_index == 0)
			connection->connect_index++;
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

void
stomp_init(struct event_base *b)
{
	base = b;
}

void
stomp_send(struct stomp_connection *connection)
{
	/* Track message Tx */
	connection->messages_tx++;
}

struct stomp_subscription
*stomp_subscribe(struct stomp_connection *connection, char *destination)
{
	struct stomp_frame		 frame;
	struct stomp_header		*header;
	struct stomp_subscription	*subscription;
	int				 size;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_SUBSCRIBE;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("id");
	size = snprintf(NULL, 0, "%d", connection->subscription_id);
	header->value = calloc(size + 1, sizeof(char));
	sprintf(header->value, "%d", connection->subscription_id++);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("destination");
	header->value = strdup(destination);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("ack");
	header->value = strdup("client-individual");
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

	stomp_frame_publish(connection, &frame);

	/* Clear down headers */
	stomp_headers_destroy(&frame.headers);

	return (subscription);
}

void
stomp_unsubscribe(struct stomp_connection *connection,
    struct stomp_subscription *subscription)
{
}

struct stomp_transaction
*stomp_begin(struct stomp_connection *connection)
{
	struct stomp_frame		 frame;
	struct stomp_header		*header;
	struct stomp_transaction	*transaction;
	int				 size;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_BEGIN;
	TAILQ_INIT(&frame.headers);

	transaction = calloc(1, sizeof(struct stomp_transaction));
	size = snprintf(NULL, 0, "tx%d", connection->transaction_id);
	transaction->id = calloc(size + 1, sizeof(char));
	sprintf(transaction->id, "tx%d", connection->transaction_id++);

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
stomp_ack(struct stomp_connection *connection, char *ack,
    struct stomp_transaction *transaction)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_ACK;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("id");
	header->value = strdup(ack);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

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
stomp_nack(struct stomp_connection *connection, char *ack,
    struct stomp_transaction *transaction)
{
	struct stomp_frame	 frame;
	struct stomp_header	*header;

	memset(&frame, 0, sizeof(frame));
	frame.command = CLIENT_NACK;
	TAILQ_INIT(&frame.headers);

	header = calloc(1, sizeof(struct stomp_header));
	header->name = strdup("id");
	header->value = strdup(ack);
	TAILQ_INSERT_TAIL(&frame.headers, header, entry);

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
}

void
stomp_reconnect(int fd, short event, void *arg)
{
	struct stomp_connection	*connection = (struct stomp_connection *)arg;
	struct sockaddr_in	 sin;

	fprintf(stderr, "Connection attempt\n");

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0xc0a8ff80);	/* 192.168.255.128 */
	sin.sin_port = htons(connection->port);

	connection->bev = bufferevent_socket_new(base, -1,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);

	if (bufferevent_socket_connect(connection->bev, (struct sockaddr *)&sin,
	    sizeof(sin)) < 0) {
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

	bufferevent_setcb(connection->bev, stomp_read, NULL, eventcb,
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

struct stomp_connection
*stomp_connection_new(void)
{
	struct stomp_connection	*connection;

	if ((connection = calloc(1, sizeof(struct stomp_connection))) != NULL) {
		connection->state = STOMP_FRAME_OR_KEEPALIVE;
		connection->connect_ev = evtimer_new(base, stomp_reconnect,
		    (void *)connection);
		TAILQ_INIT(&connection->frame.headers);
		TAILQ_INIT(&connection->subscriptions);
		TAILQ_INIT(&connection->transactions);
	}

	return (connection);
}

struct stomp_connection
*stomp_connect(char *host, short port, int version, char *vhost, SSL_CTX *ctx,
    struct timeval tv, int cx, int cy,
    void (*connect_cb)(struct stomp_connection *c),
    void (*read_cb)(struct stomp_connection *c, struct stomp_frame *f))
{
	struct stomp_connection	*connection;

	if ((connection = stomp_connection_new()) == NULL)
		return (NULL);

	/* Set up timer to (re)connect */
	connection->connect_tv[1] = tv;
	evtimer_add(connection->connect_ev,
	    &connection->connect_tv[connection->connect_index]);
	connection->connect_index++;

	/* TCP port */
	connection->port = port;

	/* Desired version */
	connection->version = version;

	/* vhost */
	connection->vhost = strdup(vhost);

	/* SSL */
	connection->ctx = ctx;

	/* Desired heartbeat rates */
	connection->cx = cx;
	connection->cy = cy;

	connection->connectcb = connect_cb;
	connection->readcb = read_cb;

	return (connection);
}

void
stomp_connected(struct stomp_connection *connection, struct stomp_frame *frame)
{
}

void
stomp_message(struct stomp_connection *connection, struct stomp_frame *frame)
{
	/* Track message Rx */
	connection->messages_rx++;
}

void
stomp_receipt(struct stomp_connection *connection, struct stomp_frame *frame)
{
}

void
stomp_error(struct stomp_connection *connection, struct stomp_frame *frame)
{
}

void
test_connect_cb(struct stomp_connection *connection)
{
	fprintf(stderr, "Connected (STOMP)\n");
	stomp_subscribe(connection, "/queue/foo");
	transaction = stomp_begin(connection);
	//stomp_subscribe(connection, "/exchange/foo/bar");
}

void
test_read_cb(struct stomp_connection *connection, struct stomp_frame *frame)
{
	struct stomp_header	*header;
	static int		 count = 0;

	fprintf(stderr, "Got frame -> %s\n",
	    stomp_server_commands[frame->command]);
	if (frame->body)
		fprintf(stderr, "Frame body -> %s\n", frame->body);
	count++;
	if ((header = stomp_header_find(&frame->headers, "ack")) != NULL) {
		stomp_ack(connection, header->value, transaction);
		//stomp_nack(connection, header->value, transaction);
	}
	/* After receiving three messages, commit or abort the transaction */
	if (count == 3) {
		//stomp_abort(connection, transaction);
		stomp_commit(connection, transaction);
		//stomp_disconnect(connection);
		count = 0;
		transaction = stomp_begin(connection);
	}
		
}

int
main(int argc, char *argv[])
{
	struct event_base	*b;
	SSL_CTX			*ctx;
	struct timeval		 tv = { 10, 0 };	/* 10 seconds */

	SSL_load_error_strings();
	SSL_library_init();

	ctx = SSL_CTX_new(SSLv23_client_method());

	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2);
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

	b = event_base_new();

	stomp_init(b);

	if (stomp_connect("192.168.255.128", 61614, STOMP_VERSION_ANY, "/",
	    ctx, tv, 1000, 1000, test_connect_cb, test_read_cb) == NULL)
		return (-1);

	event_base_dispatch(b);

	SSL_CTX_free(ctx);

	return (0);
}
