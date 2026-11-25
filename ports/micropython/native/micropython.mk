MM_NATIVE_DIR := $(USERMOD_DIR)
MM_ROOT := $(abspath $(MM_NATIVE_DIR)/../../..)
SRC_USERMOD_C += $(MM_NATIVE_DIR)/modmm.c
MM_CORE := src/batch.c src/crc32.c src/document.c src/protocol.c src/record.c src/segment.c src/wire.c storage/journal.c storage/volume.c posix/peer.c
vpath core/%.c $(MM_ROOT)
SRC_USERMOD_LIB_C += $(addprefix $(USER_C_MODULES)/core/,$(MM_CORE))
CFLAGS_USERMOD += -I$(MM_ROOT)/core/include
