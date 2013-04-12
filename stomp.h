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

#ifndef _STOMP_H
#define _STOMP_H

#include <sys/queue.h>

#include <event2/event.h>
#include <event2/dns.h>

#include <openssl/ssl.h>

#define	STOMP_DEFAULT_PORT	(61613)
#define	STOMP_DEFAULT_SSL_PORT	(61614)

#define	STOMP_VERSION_1_0	(1 << 0)
#define	STOMP_VERSION_1_1	(1 << 1)
#define	STOMP_VERSION_1_2	(1 << 2)
#define	STOMP_VERSION_ANY	(STOMP_VERSION_1_0|STOMP_VERSION_1_1|STOMP_VERSION_1_2)

#define	STOMP_ACK_AUTO			0
#define	STOMP_ACK_CLIENT		1
#define	STOMP_ACK_CLIENT_INDIVIDUAL	2

enum stomp_command {
	CLIENT_SEND = 0,
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
	CLIENT_MAX_COMMAND,
	SERVER_CONNECTED = 0,
	SERVER_MESSAGE,
	SERVER_RECEIPT,
	SERVER_ERROR,
	SERVER_MAX_COMMAND
};

struct stomp_header {
	TAILQ_ENTRY(stomp_header)	 entry;
	char				*name;
	char				*value;
};

struct stomp_frame {
	enum stomp_command			 command;
	TAILQ_HEAD(stomp_headers, stomp_header)	 headers;
	unsigned char				*body;
};

struct stomp_transaction {
	TAILQ_ENTRY(stomp_transaction)	 entry;
	char				*id;
};

enum stomp_frame_state {
	STOMP_FRAME_OR_KEEPALIVE,
	STOMP_FRAME_HEADERS,
	STOMP_FRAME_BODY
};

struct stomp_connection {
	struct bufferevent	 *bev;

	char			 *host;
	unsigned short		  port;

	/* Version(s) requested & negotiated */
	int			  version_req;
	int			  version_neg;

	char			 *vhost;

	/* SSL */
	SSL_CTX			 *ctx;

	/* (Re)connect */
	struct event		 *connect_ev;
	struct timeval		  connect_tv[2];
	int			  connect_index;

	/* Bytes sent/received */
	unsigned long long	  bytes_rx;
	unsigned long long	  bytes_tx;

	/* Frames sent/received */
	unsigned long long	  frames_rx;
	unsigned long long	  frames_tx;

	/* Messages sent/received */
	unsigned long long	  messages_rx;
	unsigned long long	  messages_tx;

	/* Frame we're currently receiving */
	enum stomp_frame_state	  state;
	struct stomp_frame	  frame;

	/* Callbacks */
	void			(*connectcb)(struct stomp_connection *,
				    struct stomp_frame *, void *);
	void			(*receiptcb)(struct stomp_connection *,
				    struct stomp_frame *, void *);
	void			(*errorcb)(struct stomp_connection *,
				    struct stomp_frame *, void *);
	void			(*disconnectcb)(struct stomp_connection *,
				    void *);
	void			 *arg;

	/* Heartbeat support */
	int			  cx;
	int			  cy;
	struct event		 *heartbeat_ev;
	struct timeval		  heartbeat_tv;
	struct event		 *timeout_ev;
	struct timeval		  timeout_tv;

	/* Subscriptions */
	unsigned long long	  subscription_id;
	TAILQ_HEAD(stomp_subscriptions, stomp_subscription)	 subscriptions;

	/* Transactions */
	unsigned long long	  transaction_id;
	TAILQ_HEAD(stomp_transactions, stomp_transaction)	 transactions;

	/* Receipts */
	unsigned long long	  receipt_id;
};

struct stomp_subscription {
	TAILQ_ENTRY(stomp_subscription)	  entry;
	char				 *id;
	char				 *destination;
	int				  ack;
	void				(*callback)(struct stomp_connection *,
					    struct stomp_subscription *,
					    struct stomp_frame *, void *);
	void				 *arg;
};

struct stomp_header		*stomp_frame_header_find(struct stomp_frame *,
				    char *);
void				 stomp_init(struct event_base *,
				    struct evdns_base *);
struct stomp_connection		*stomp_connection_new(char *, unsigned short,
				    int, char *, SSL_CTX *, struct timeval,
				    int, int);
void				 stomp_connection_setcb(struct stomp_connection *,
				    void (*connectcb)(struct stomp_connection *,
				    struct stomp_frame *, void *),
				    void (*receiptcb)(struct stomp_connection *,
				    struct stomp_frame *, void *),
				    void (*errorcb)(struct stomp_connection *,
				    struct stomp_frame *, void *),
				    void (*disconnectcb)(struct stomp_connection *,
				    void *), void *);
void				 stomp_connect(struct stomp_connection *);
void				 stomp_disconnect(struct stomp_connection *);
void				 stomp_connection_free(struct stomp_connection *);
void				 stomp_send(struct stomp_connection *, char *,
				    unsigned char *,
				    struct stomp_transaction *);
struct stomp_subscription	*stomp_subscription_new(struct stomp_connection *,
				    char *, int);
void				 stomp_subscription_setcb(struct stomp_subscription *,
				    void (*callback)(struct stomp_connection *,
				    struct stomp_subscription *,
				    struct stomp_frame *, void *), void *);
void				 stomp_subscribe(struct stomp_connection *,
				    struct stomp_subscription *);
void				 stomp_unsubscribe(struct stomp_connection *,
				    struct stomp_subscription *);
void				 stomp_subscription_free(struct stomp_subscription *);
struct stomp_transaction	*stomp_begin(struct stomp_connection *);
void				 stomp_commit(struct stomp_connection *,
				    struct stomp_transaction *);
void				 stomp_abort(struct stomp_connection *,
				    struct stomp_transaction *);
void				 stomp_ack(struct stomp_connection *,
				    struct stomp_subscription *, char *,
				    struct stomp_transaction *);
void				 stomp_nack(struct stomp_connection *,
				    struct stomp_subscription *, char *,
				    struct stomp_transaction *);

#endif /* _STOMP_H */
