libevent-stomp - asynchronous non-blocking STOMP client
=======================================================

Based around libevent bufferevents and designed to be used as part of a bigger
libevent-based application.

+ Should support STOMP 1.0, 1.1 & 1.2.
+ SSL/TLS support thanks to libevent\_openssl.
+ Heartbeat support.

Example usage:

    #include <stdio.h>
    
    #include "stomp.h"
    
    void message_cb(struct stomp_connection *, struct stomp_subscription *,
        struct stomp_frame *, void *);
    void connect_cb(struct stomp_connection *, struct stomp_frame *, void *);
    
    void
    message_cb(struct stomp_connection *connection,
        struct stomp_subscription *, struct stomp_frame *frame, void *arg)
    {
            printf("Got message %s\n", frame->body);
    }
    
    void
    connect_cb(struct stomp_connection *connection, struct stomp_frame *frame,
        void *arg)
    {
            struct stomp_header *header;
            struct stomp_subscription *subscription;
    
            if ((header = stomp_frame_header_find(frame, "server")) != NULL)
                    printf("Connected to %s\n", header->value);
    
            subscription = stomp_subscription_new(connection, "/queue/test",
                STOMP_ACK_AUTO);
            stomp_subscription_setcb(subscription, message_cb, NULL);
            stomp_subscribe(connection, subscription);
    }
    
    int
    main(int argc, char *argv[])
    {
            struct event_base *base;
            struct timeval tv = { 10, 0 };
            struct stomp_connection *c;
            char *user = "guest";
            char *pass = "guest";
    
            base = event_base_new();
            if (stomp_init(base) < 0)
                    return (-1);
    
            /* Pass an SSL_CTX * as argument #7 for SSL/TLS support */
            if ((c = stomp_connection_new("192.0.2.1", STOMP_DEFAULT_PORT,
                STOMP_VERSION_ANY, "/", user, pass, NULL, tv, 1000,
                1000)) == NULL)
                    return (-1);
    
            stomp_connection_setcb(c, connect_cb, NULL, NULL, NULL, NULL);
    
            stomp_connect(c);
    
            event_base_dispatch(base);
    
            return (0);
    }
