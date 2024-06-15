NAME := kitserv

KITSERV_LIBDIR ?= $(error KITSERV_LIBDIR not set: no library install directory)
KITSERV_INCDIR ?= $(error KITSERV_INCDIR not set: no include install directory)
KITSERV_BINDIR ?= $(error KITSERV_BINDIR not set: no binary install directory)
KITSERV_MANDIR ?= $(error KITSERV_MANDIR not set: no manual page install directory)

INCLUDE_DIR := include
OBJ_DIR := obj
BIN_DIR := bin
LIB_DIR := lib
SRC_DIR := src
SRC_INCLUDE_DIR := $(SRC_DIR)/include
MAN_DIR := man

WARNINGS := -Wall -Wextra -Wmissing-prototypes -Winline -pedantic
CFLAGS := -MMD -MP -O2 $(WARNINGS) -I$(INCLUDE_DIR) -I$(SRC_INCLUDE_DIR) -fpie -DNDEBUG
LDFLAGS := -pthread

SOURCES := $(shell find $(SRC_DIR) -type f \( -name *.c \! -name main.c \) )
OBJS := $(patsubst $(SRC_DIR)/%, $(OBJ_DIR)/%, $(SOURCES:.c=.o))
DEPENDS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.d, $(SOURCES))

BIN_SOURCES := $(SOURCES) $(SRC_DIR)/main.c
BIN_OBJS := $(patsubst $(SRC_DIR)/%, $(OBJ_DIR)/%, $(BIN_SOURCES:.c=.o))
BIN_DEPENDS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.d, $(BIN_SOURCES))

LIB := $(LIB_DIR)/lib$(NAME).a
STANDALONE := $(BIN_DIR)/$(NAME)



.PHONY:	all install debug clean

all:	$(LIB) $(STANDALONE)

debug:	CFLAGS += -g -O0 -UNDEBUG
debug:	all

-include $(DEPENDS) $(BIN_DEPENDS)

$(OBJ_DIR)/%.o:	$(SRC_DIR)/%.c Makefile
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -c $< -o $@

$(LIB):	$(OBJS) Makefile
	@mkdir -p $(LIB_DIR)
	@$(RM) -f $(LIB)
	$(AR) rcs -o $@ $(OBJS)

$(STANDALONE):	$(BIN_OBJS) Makefile
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BIN_OBJS)

install:	all
	@mkdir -p $(KITSERV_INCDIR)
	@mkdir -p $(KITSERV_LIBDIR)
	@mkdir -p $(KITSERV_BINDIR)
	@mkdir -p $(KITSERV_MANDIR)
	@cp $(INCLUDE_DIR)/* $(KITSERV_INCDIR)/
	@cp $(STANDALONE) $(KITSERV_BINDIR)/
	@cp $(LIB) $(KITSERV_LIBDIR)/
	@cp -r $(MAN_DIR)/* $(KITSERV_MANDIR)/

clean:
	@$(RM) -f $(OBJS) $(BIN_OBJS) $(DEPENDS) $(BIN_DEPENDS) $(LIB) $(STANDALONE)
	-@rmdir $(OBJ_DIR)
	-@rmdir $(BIN_DIR)
	-@rmdir $(LIB_DIR)
