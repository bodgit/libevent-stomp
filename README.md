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
    
    void
    connect_cb(struct stomp_connection *connection, struct stomp_frame *frame,
        void *arg)
    {
            struct stomp_header *header;

            if ((header = stomp_header_find(&frame->headers, "server")) != NULL)
                    printf("Connected to %s\n", header->value);
    
            stomp_subscribe(connection, "/queue/test");
    }
    
    void
    message_cb(struct stomp_connection *connection, struct stomp_frame *frame,
        void *arg)
    {
            printf("Got message %s\n", frame->body);
    }
    
    int
    main(int argc, char *argv[])
    {
            struct timeval tv = { 10, 0 };
            struct stomp_connection *c;
    
            base = event_base_new();
            stomp_init(base, NULL);
    
            /* Pass an SSL_CTX * as argument #5 for SSL/TLS support */
            if ((c = stomp_connection_new("192.0.2.1", 61613,
                STOMP_VERSION_ANY, "/", NULL, tv, 1000, 1000)) == NULL)
                    return (-1);
    
            stomp_connection_setcb(c, SERVER_CONNECTED, connect_cb, NULL);
            stomp_connection_setcb(c, SERVER_MESSAGE, message_cb, NULL);
    
            stomp_connect(c);
    
            event_dispatch(base);
    
            return (0);
    }
