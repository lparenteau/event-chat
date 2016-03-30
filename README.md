# event-chat
Simple event-based chat server; you can use telnet for client.

This is a good showcase of an event-based server in C, using epoll().  It also provide some insight with using branch prediction to improve performance.

To run the server, build and run :
./a.out -p <port>

To connect using telnet :
telnet <ip_of_server> <port>

The first line you type in your telnet client will become the chatroom's name.  Every other telnet connection that connect to that same chatroom will see what you type, and vice versa.
