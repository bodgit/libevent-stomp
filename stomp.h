#ifndef _STOMP_H
#define _STOMP_H

#define	STOMP_VERSION_1_0	(1 << 0)
#define	STOMP_VERSION_1_1	(1 << 1)
#define	STOMP_VERSION_1_2	(1 << 2)
#define	STOMP_VERSION_ANY	(STOMP_VERSION_1_0|STOMP_VERSION_1_1|STOMP_VERSION_1_2)

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
	struct {
		void	(*cb)(struct stomp_connection *,
			    struct stomp_frame *, void *);
		void	 *arg;
	}			  callback[SERVER_MAX_COMMAND];

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

void				 stomp_init(struct event_base *,
				    struct evdns_base *);
struct stomp_connection		*stomp_connection_new(char *, unsigned short,
				    int, char *, SSL_CTX *, struct timeval,
				    int, int);
void				 stomp_connection_setcb(struct stomp_connection *,
				    enum stomp_server_command,
				    void (*callback)(struct stomp_connection *,
				    struct stomp_frame *, void *), void *);
void				 stomp_connect(struct stomp_connection *);
void				 stomp_connection_free(struct stomp_connection *);
void				 stomp_send(struct stomp_connection *);
struct stomp_subscription	*stomp_subscribe(struct stomp_connection *,
				    char *);
void				 stomp_unsubscribe(struct stomp_connection *,
				    struct stomp_subscription *);
struct stomp_transaction	*stomp_begin(struct stomp_connection *);
void				 stomp_commit(struct stomp_connection *,
				    struct stomp_transaction *);
void				 stomp_abort(struct stomp_connection *,
				    struct stomp_transaction *);
void				 stomp_ack(struct stomp_connection *, char *,
				    struct stomp_transaction *);
void				 stomp_nack(struct stomp_connection *, char *,
				    struct stomp_transaction *);
void				 stomp_disconnect(struct stomp_connection *);

#endif /* _STOMP_H */
