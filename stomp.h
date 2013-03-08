#ifndef _STOMP_H
#define _STOMP_H

enum stomp_server_command {
	SERVER_CONNECTED,
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
	enum stomp_server_command		 command;
	TAILQ_HEAD(stomp_headers, stomp_header)	 headers;
	unsigned char				*body;
};

struct stomp_subscription {
	TAILQ_ENTRY(stomp_subscription)	 entry;
	char				*id;
	char				*destination;
	int				 ack;
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

	int			  version;

	char			 *vhost;

	/* Bytes sent/received */
	unsigned long long	  rx;
	unsigned long long	  tx;

	/* Frame we're currently receiving */
	enum stomp_frame_state	  state;
	struct stomp_frame	  frame;

	/* Callbacks */
	void			(*connectcb)(struct stomp_connection *c);
	void			(*readcb)(struct stomp_connection *c, struct stomp_frame *f);

	/* Heartbeat support */
	int			  cx;
	int			  cy;
	struct event		 *heartbeat_ev;
	struct timeval		  heartbeat_tv;
	struct event		 *timeout_ev;
	struct timeval		  timeout_tv;

	/* Subscriptions */
	int			  subscription_id;
	TAILQ_HEAD(stomp_subscriptions, stomp_subscription)	 subscriptions;

	/* Transactions */
	int			  transaction_id;
	TAILQ_HEAD(stomp_transactions, stomp_transaction)	 transactions;
};

void				 stomp_init(struct event_base *);
struct stomp_connection		*stomp_connect(char *, int, int, char *,
				    SSL_CTX *, int, int,
				    void (*connect_cb)(struct stomp_connection *),
				    void (*read_cb)(struct stomp_connection *, struct stomp_frame *));
struct stomp_subscription	*stomp_subscribe(struct stomp_connection *,
				    char *);
struct stomp_transaction	*stomp_begin(struct stomp_connection *);
void				 stomp_commit(struct stomp_connection *,
				    struct stomp_transaction *);
void				 stomp_abort(struct stomp_connection *,
				    struct stomp_transaction *);
void				 stomp_ack(struct stomp_connection *, char *,
				    struct stomp_transaction *);
void				 stomp_nack(struct stomp_connection *, char *,
				    struct stomp_transaction *);

#endif /* _STOMP_H */
