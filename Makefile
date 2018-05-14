CC ?=		cc
CFLAGS ?=	-g -O2 -Wall
LDFLAGS ?= -levent

all:
	${CC} ${CFLAGS} ${LDFLAGS} -o tcpbench tcpbench.c

clean:
	rm tcpbench
