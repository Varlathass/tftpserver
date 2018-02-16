
CC := gcc
CFLAGS := -Wall -g -D DEBUG

LINKER := gcc
LFLAGS := -Wall -g

SRC := $(wildcard src/*.c)
INCLUDES := $(wildcard src/*.h)
OBJECTS := $(SRC:src/%.c=bin/%.o)


MAIN := tftp.out


all: $(MAIN)

run: $(MAIN)
	./$(MAIN)

$(MAIN): $(OBJECTS) 
	$(LINKER) -o $(MAIN) $(LFLAGS) $(OBJECTS)

$(OBJECTS): bin/%.o : src/%.c
	$(CC) $(CFLAGS) -c $< -o $@
	@echo "Compiled "$<"successfully!"

clean:
	$(RM) *.o *~ $(MAIN)

depend: $(SRCS)
	makedepend $(INCLUDES) $^

# DO NOT DELETE THIS LINE -- make depend needs it
