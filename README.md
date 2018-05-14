# tcpbench
Porting OpenBSD [tcpbench](https://github.com/openbsd/src/tree/master/usr.bin/tcpbench) into Linux (Just keep the basic features, and you need to install [libevent](http://libevent.org/) before compiling `tcpbench`).  

## Usage

	$ git clone https://github.com/NanXiao/tcpbench.git
	$ cd tcpbench
	$ make

Start server:  

	$ ./tcpbench -s
	  elapsed_ms          bytes         mbps  bwidth
	         999     4544790528    36394.719  100.00%
	Conn:   1 Mbps:    36394.719 Peak Mbps:    36394.719 Avg Mbps:    36394.719
	        1999     3391750038    27161.161  100.00%
	Conn:   1 Mbps:    27161.161 Peak Mbps:    36394.719 Avg Mbps:    27161.161
	        2999     3464953909    27719.631  100.00%
	Conn:   1 Mbps:    27719.631 Peak Mbps:    36394.719 Avg Mbps:    27719.631
	        3999     3436118069    27488.945  100.00%
	Conn:   1 Mbps:    27488.945 Peak Mbps:    36394.719 Avg Mbps:    27488.945
	......

Start client:  

	$ ./tcpbench 127.0.0.1
	  elapsed_ms          bytes         mbps  bwidth
	         999     4545052672    36396.818  100.00%
	Conn:   1 Mbps:    36396.818 Peak Mbps:    36396.818 Avg Mbps:    36396.818
	        1999     3391881216    27135.050  100.00%
	Conn:   1 Mbps:    27135.050 Peak Mbps:    36396.818 Avg Mbps:    27135.050
	        2999     3465019392    27720.155  100.00%
	Conn:   1 Mbps:    27720.155 Peak Mbps:    36396.818 Avg Mbps:    27720.155
	        3999     3435921408    27514.886  100.00%
	Conn:   1 Mbps:    27514.886 Peak Mbps:    36396.818 Avg Mbps:    27514.886
	......

Check options:  

	$ ./tcpbench -h
	Usage:
	Client:     tcpbench [-46Uuv] [-B buf] [-b addr] [-n connections]
	                [-p port] [-r interval] [-S space] [-t secs] hostname
	Server:     tcpbench -s [-46Uuv] [-B buf] [-p port] [-r interval]
	                [-S space] [hostname]
	
	Options:
	-4          Use IPv4 addresses.
	-6          Use IPv6 addresses.
	-B buf      Specify the size of the internal read/write buffer used by
	            tcpbench. The default is 262144 bytes for TCP client/server and
	            UDP server. In UDP client mode this may be used to specify the
	            packet size on the test stream.
	-b addr     Specify the IP address of the interface which is used to send
	            the packets.
	-n conns    Use the given number of TCP connections (default: 1). UDP is
	            connectionless so this option isn't valid.
	-p port     Specify the port used for the test stream (default: 12345).
	-r interval Specify the statistics interval reporting rate in milliseconds
	            (default: 1000).
	-S space    Set the size of the socket buffer used for the test stream. On
	            the client this option will resize the send buffer; on the server
	            it will resize the receive buffer.
	-s          Place tcpbench in server mode, where it will listen on all
	            interfaces for incoming connections. It defaults to using TCP if
	            -u is not specified.
	-t secs     Stop after secs seconds.
	-U          Use AF_UNIX sockets instead of IPv4 or IPv6 sockets. In client
	            and server mode hostname is used as the path to the AF_UNIX socket.
	-u          Use UDP instead of TCP; this must be specified on both the client
	            and the server. Transmitted packets per second (TX PPS) will be
	            accounted on the client side, while received packets per second
	            (RX PPS) will be accounted on the server side.
	-v          Display verbose output. If specified more than once, increase
	            the detail of information displayed.


