DEP_BASE_DIR := deps
DEP_LIB_DIR := $(abspath $(DEP_BASE_DIR)/lib)
DEP_INCLUDE_DIR := $(DEP_BASE_DIR)/include
DEP_OBJ_DIR := $(DEP_BASE_DIR)/obj
DEP_SRC_DIR := $(DEP_BASE_DIR)/src

INCLUDE_DIR := include
OBJ_DIR := obj
SRC_DIR := src

WARNINGS := -Wall -Wextra -Wmissing-prototypes -Winline -pedantic

DEP_CFLAGS := -O2 $(WARNINGS) -I$(DEP_INCLUDE_DIR) -fpie
CFLAGS := -MMD -MP -O2 $(WARNINGS) -I$(DEP_INCLUDE_DIR) -I$(INCLUDE_DIR) -fpie -DNDEBUG

# include lib directory in runtime path for dynamic linking
LDFLAGS := -pthread -Wl,-rpath -Wl,$(DEP_LIB_DIR)
LDLIBS := -L$(DEP_LIB_DIR) -pthread -ljwt -ljansson -lcrypto -ldl

# -------------------------

DEP_SOURCES := $(shell find $(DEP_SRC_DIR) -type f -name *.c)
DEP_OBJS := $(patsubst $(DEP_SRC_DIR)/%, $(DEP_OBJ_DIR)/%, $(DEP_SOURCES:.c=.o))

SOURCES := $(shell find $(SRC_DIR) -type f -name *.c)
OBJS := $(patsubst $(SRC_DIR)/%, $(OBJ_DIR)/%, $(SOURCES:.c=.o))
DEPENDS := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.d, $(SOURCES))

.PHONY:	all debug clean cleaner

all:	filekit

debug: CFLAGS += -g -O0 -UNDEBUG
debug: all

# don't rebuild deps even on make -B (since they shouldn't be changing)
# use `make cleaner` to remove and force this rebuild
$(DEP_OBJ_DIR)/%.o:	$(DEP_SRC_DIR)/%.c
	@mkdir -p $(DEP_OBJ_DIR)
	if [ ! -f $@ ]; then \
		$(CC) $(DEP_CFLAGS) -c $< -o $@ ; \
	fi

-include $(DEPENDS)

$(OBJ_DIR)/%.o:	$(SRC_DIR)/%.c Makefile
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

filekit:	$(DEP_OBJS) $(OBJS) Makefile
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(DEP_OBJS) $(LDLIBS)

# just binary and objects
clean:
	@$(RM) -f $(OBJS) filekit

# everything including dependencies
cleaner:
	@$(RM) -f $(DEP_OBJS) $(OBJS) filekit
