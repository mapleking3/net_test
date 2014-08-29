CC = gcc

CFLAGS = -Wall -O2 -s -g

#CFLAGS += -DDEBUG

OBJS = $(patsubst %.c,%.o,$(wildcard *.c))

DEPS = $(patsubst %.o,%.d,$(OBJS))
CFLAGS += -MD
MISSING_DEPS = $(filter-out $(wildcard $(DEPS)),$(DEPS))
MISSING_DEPS_SOURCES = $(wildcard $(patsubst %.d,%.c,$(MISSING_DEPS)))

LDFLAGS	+= -lrt -lpthread

LIBS=

TARGET = jproxyd jproxy

all : $(TARGET)

ifneq ($(MISSING_DEPS),)
$(MISSING_DEPS) :
	@$(RM) $(patsubst %.d,%.o,$@)
endif

-include $(DEPS)

jproxyd:jproxyd.o
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

jproxy:jproxy.o
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(OBJS):%.o:%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.d *.s *.o $(TARGET)

