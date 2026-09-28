# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += -Wall -O2 -Wno-unused-function
CFLAGS += $(KHDR_INCLUDES)
LDLIBS += -lcap

TEST_GEN_PROGS :=
TEST_GEN_PROGS += iommufd
TEST_GEN_PROGS += iommufd_fail_nth

TEST_PROGS += apple-dart-x1n1-map.py apple-dart-x1n1-group.py
TEST_FILES += apple-dart-x1n1-map.c

include ../lib.mk
