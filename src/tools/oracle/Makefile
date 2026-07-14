ROOT := ../../..
BUILD := $(ROOT)/build/oracle
BIN := $(ROOT)/release
CC ?= cc
AR ?= ar
PYTHON ?= python3

BASE_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -fno-strict-aliasing \
	-fwrapv -DDEDICATED_ONLY -DYQ2OSTYPE=\"oracle\" -DYQ2ARCH=\"offline\"
CFLAGS ?= $(BASE_CFLAGS)
LDLIBS := -lm

IDENTITY := $(BUILD)/oracle_identity.h
LIB_OBJECTS := \
	$(BUILD)/collision.o \
	$(BUILD)/pmove.o \
	$(BUILD)/shared.o \
	$(BUILD)/crc.o \
	$(BUILD)/md4.o \
	$(BUILD)/oracle_support.o \
	$(BUILD)/oracle_json.o \
	$(BUILD)/sha256.o

.PHONY: all clean test

all: $(BIN)/q2-cm-oracle $(BIN)/q2-pmove-oracle

$(BUILD) $(BIN):
	mkdir -p $@

$(IDENTITY): gen_identity.py $(ROOT)/src/common/collision.c \
		$(ROOT)/src/common/pmove.c $(ROOT)/src/common/header/shared.h \
		$(ROOT)/src/common/shared/shared.c | $(BUILD)
	$(PYTHON) gen_identity.py $(ROOT) $@

$(BUILD)/collision.o: $(ROOT)/src/common/collision.c $(IDENTITY)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BUILD)/pmove.o: $(ROOT)/src/common/pmove.c $(IDENTITY)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BUILD)/shared.o: $(ROOT)/src/common/shared/shared.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(ROOT)/src -c $< -o $@

$(BUILD)/crc.o: $(ROOT)/src/common/crc.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(ROOT)/src -c $< -o $@

$(BUILD)/md4.o: $(ROOT)/src/common/md4.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(ROOT)/src -c $< -o $@

$(BUILD)/oracle_support.o: oracle_support.c q2_oracle.h sha256.h $(IDENTITY)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BUILD)/oracle_json.o: oracle_json.c q2_oracle.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BUILD)/sha256.o: sha256.c sha256.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BUILD)/q2_cm_oracle.o: q2_cm_oracle.c q2_oracle.h $(IDENTITY)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BUILD)/q2_pmove_oracle.o: q2_pmove_oracle.c q2_oracle.h $(IDENTITY)
	$(CC) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src -c $< -o $@

$(BIN)/libq2oracle.a: $(LIB_OBJECTS) | $(BIN)
	$(AR) rcs $@ $(LIB_OBJECTS)

$(BIN)/q2-cm-oracle: $(BUILD)/q2_cm_oracle.o $(BIN)/libq2oracle.a | $(BIN)
	$(CC) $(CFLAGS) $< $(BIN)/libq2oracle.a $(LDLIBS) -o $@

$(BIN)/q2-pmove-oracle: $(BUILD)/q2_pmove_oracle.o $(BIN)/libq2oracle.a | $(BIN)
	$(CC) $(CFLAGS) $< $(BIN)/libq2oracle.a $(LDLIBS) -o $@

test: all
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

clean:
	rm -rf $(BUILD) $(BIN)/q2-cm-oracle $(BIN)/q2-pmove-oracle \
		$(BIN)/libq2oracle.a
