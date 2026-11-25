ifeq ($(MM_WITH_SQLITE),1)
ifndef MM_SQLITE_BUILD
$(error Set MM_SQLITE_BUILD to the host CMake build directory configured with MM_WITH_SQLITE=ON and MM_SANITIZE=OFF)
endif
SRC_USERMOD_C += $(USERMOD_DIR)/modmmsqlite.c
LDFLAGS_USERMOD += -L$(MM_SQLITE_BUILD) -lmicromeerschaum_sqlite -lmm_sqlite_engine -lm -pthread
endif
