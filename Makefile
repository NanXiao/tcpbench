CC ?=		cc
CFLAGS ?=	-g -O2 -Wall
LDFLAGS ?= -levent

all:
	${CC} ${CFLAGS} -o tcpbench tcpbench.c ${LDFLAGS}

clean:
	rm tcpbench
