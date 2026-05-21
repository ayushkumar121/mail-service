MAIN=mail-service
CC=cc
CFLAGS= -std=gnu11 -Wall -g -D_GNU_SOURCE -I/opt/homebrew/Cellar/openssl@3/3.6.2/include
LIBS= -lresolv -latomic -lm `(pkg-config --libs openssl)`
LDFLAGS= -rdynamic -L/opt/homebrew/opt/openssl@3/lib

OBJS=http.o basic.o config.o maildir.o smtp.o imap.o handlers.o metrics.o

all: $(MAIN)

$(MAIN): $(MAIN).c $(OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LIBS)

%.o: %.c %.h
	$(CC) -c -o $@ $< $(CFLAGS)

test: $(MAIN)
	@pkill -f $(MAIN) 2>/dev/null; sleep 0.3; true
	@bash tests/setup_fixture.sh
	@./$(MAIN) tests/config.json > /tmp/mail-service.log 2>&1 & SERVER_PID=$$!; \
	  bash tests/tests.sh; STATUS=$$?; \
	  kill $$SERVER_PID 2>/dev/null; exit $$STATUS

clean:
	rm -f $(MAIN) $(OBJS)