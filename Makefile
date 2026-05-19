MAIN=mail-service
CC=cc
CFLAGS=-std=c11 -Wall -g -D_GNU_SOURCE
LIBS= -lresolv -lm
LDFLAGS= -rdynamic

OBJS=http.o basic.o config.o maildir.o smtp.o imap.o handlers.o metrics.o

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