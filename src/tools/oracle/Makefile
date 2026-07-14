ROOT := ../../..
BUILD := $(ROOT)/build/oracle
BIN := $(ROOT)/release
CC ?= cc
AR ?= ar
PYTHON ?= python3

REQUIRED_CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -fno-strict-aliasing \
	-fwrapv -DDEDICATED_ONLY -DYQ2OSTYPE=\"oracle\" -DYQ2ARCH=\"offline\"
CFLAGS ?=
ORACLE_CFLAGS := $(REQUIRED_CFLAGS) $(CFLAGS) -I$(BUILD) -I$(ROOT)/src
LDLIBS := -lm
ORACLE_LDFLAGS := $(LDFLAGS) $(LDLIBS)

export Q2_ORACLE_CC := $(CC)
export Q2_ORACLE_AR := $(AR)
export Q2_ORACLE_CFLAGS := $(ORACLE_CFLAGS)
export Q2_ORACLE_LDFLAGS := $(ORACLE_LDFLAGS)

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

.PHONY: all clean test FORCE

all: $(BIN)/q2-cm-oracle $(BIN)/q2-pmove-oracle

$(BUILD) $(BIN):
	mkdir -p $@

$(IDENTITY): gen_identity.py $(ROOT)/src/common/collision.c \
		$(ROOT)/src/common/pmove.c $(ROOT)/src/common/header/shared.h \
		$(ROOT)/src/common/shared/shared.c FORCE | $(BUILD)
	$(PYTHON) gen_identity.py $(ROOT) $@

FORCE:

$(BUILD)/collision.o: $(ROOT)/src/common/collision.c $(IDENTITY)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/pmove.o: $(ROOT)/src/common/pmove.c $(IDENTITY)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/shared.o: $(ROOT)/src/common/shared/shared.c $(IDENTITY) | $(BUILD)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/crc.o: $(ROOT)/src/common/crc.c $(IDENTITY) | $(BUILD)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/md4.o: $(ROOT)/src/common/md4.c $(IDENTITY) | $(BUILD)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/oracle_support.o: oracle_support.c q2_oracle.h sha256.h $(IDENTITY)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/oracle_json.o: oracle_json.c q2_oracle.h $(IDENTITY) | $(BUILD)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/sha256.o: sha256.c sha256.h $(IDENTITY) | $(BUILD)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/q2_cm_oracle.o: q2_cm_oracle.c q2_oracle.h $(IDENTITY)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BUILD)/q2_pmove_oracle.o: q2_pmove_oracle.c q2_oracle.h $(IDENTITY)
	$(CC) $(ORACLE_CFLAGS) -c $< -o $@

$(BIN)/libq2oracle.a: $(LIB_OBJECTS) | $(BIN)
	$(AR) rcs $@ $(LIB_OBJECTS)

$(BIN)/q2-cm-oracle: $(BUILD)/q2_cm_oracle.o $(BIN)/libq2oracle.a | $(BIN)
	$(CC) $(ORACLE_CFLAGS) $< $(BIN)/libq2oracle.a $(ORACLE_LDFLAGS) -o $@

$(BIN)/q2-pmove-oracle: $(BUILD)/q2_pmove_oracle.o $(BIN)/libq2oracle.a | $(BIN)
	$(CC) $(ORACLE_CFLAGS) $< $(BIN)/libq2oracle.a $(ORACLE_LDFLAGS) -o $@

test: all
	$(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

clean:
	rm -rf $(BUILD) $(BIN)/q2-cm-oracle $(BIN)/q2-pmove-oracle \
		$(BIN)/libq2oracle.a
