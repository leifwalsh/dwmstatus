CFLAGS += -std=c99 -g -O2 -Wall -Wextra
LDLIBS += -lX11 -lasound

prefix=$(HOME)

all: dwmstatus

install: dwmstatus
	install -m 0755 dwmstatus $(prefix)/bin

clean:
	$(RM) dwmstatus
