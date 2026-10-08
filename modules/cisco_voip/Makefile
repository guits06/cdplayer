CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -std=c11 -pthread
LDFLAGS ?= -pthread

TARGET = cisco_gateway
SRCS = cisco_gateway.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	systemctl stop cisco-gateway || true
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	install -m 755 cisco_sync.py /usr/local/bin/cisco_sync.py
	install -m 644 cisco-gateway.service /etc/systemd/system/cisco-gateway.service
	install -m 644 cisco-sync.service /etc/systemd/system/cisco-sync.service
	systemctl daemon-reload

.PHONY: all clean install
