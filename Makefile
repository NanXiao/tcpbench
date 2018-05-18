CC ?=		cc
CFLAGS ?=	-g -O2 -Wall
LDLIBS ?= -levent

all:
	${CC} ${CFLAGS} -o tcpbench tcpbench.c ${LDLIBS}

clean:
	rm tcpbench
