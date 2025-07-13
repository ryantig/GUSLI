# ********** Define compiler and compilation flags *********
BUILD_RELEASE?=1
BUILD_FOR_UNITEST?=1
UNAME := $(shell uname)# Which operating system we use?
HT = $(shell uname -m)
CFLAGS = $(EXTRA_CFLAGS)
ifeq ($(UNAME),Darwin)
	USE_DARWIN=1
	CFLAGS += -m64
	LIBS_PATH = -L/usr/lib
else ifeq ($(HT),x86_64)
	USE_64_BITS=1
	CFLAGS += -m64
	LIBS_PATH = -L/usr/lib64
else ifeq ($(HT),aarch64)
	USE_ARM=1
	CFLAGS += -march=armv8.1-a+crc+fp+simd
	LIBS_PATH = -L/usr/lib64
else
	$(error Unknown OS/Host)
endif

VERBOSE?=0
ifeq ($(VERBOSE), 1)
	ECHO_CMD =
else
	ECHO_CMD = @
endif

# ********** Included directories of H files *********
# Main project directory. (relative to this Makefile)
SSDA = $(NVMESH_ROOT_DIR)
INCLUDES :=
INCLUDES += -I/usr/include -I. -I./00utils -I./01common -I./03backend -I./05clnt

# ********** *.cpp sources *********
SOURCES_BASE = 00utils/utils.cpp
SOURCES_CLNT = 05clnt/client_imp.cpp
SOURCES_SRVR = 03backend/server_imp.cpp
SOURCES_TEST = 89tests/unitest.cpp
SOURCES_ALL = $(SOURCES_BASE) $(SOURCES_CLNT) $(SOURCES_SRVR) $(SOURCES_TEST)

# ********** All objects will reside in separate directory **********
OBJ_DIR_ROOT=99bin
ifeq ($(BUILD_RELEASE),1)
	OBJ_DIR=./$(OBJ_DIR_ROOT)/release
else
	OBJ_DIR=./$(OBJ_DIR_ROOT)/debug
endif

$(shell mkdir -p $(OBJ_DIR))
OBJECTS_BASE = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_BASE))
OBJECTS_CLNT = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_CLNT))
OBJECTS_SRVR = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_SRVR))
OBJECTS_TEST = $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SOURCES_TEST))
OBJECTS_ALL = $(OBJECTS_BASE) $(OBJECTS_CLNT) $(OBJECTS_SRVR) $(OBJECTS_TEST)

# Include dependency files if they exist, causes recompilation .cpp files when .hpp files cahnge
DEPEND_FILES_ALL = $(OBJECTS_ALL:.o=.d)
DFLAGS = -MP -MMD

# Compilation output (Lib/Exe)
PROJECT_PREFIX=gusli
LIB_COMN_NAME=$(PROJECT_PREFIX)_comn
LIB_CLNT_NAME=$(PROJECT_PREFIX)_clnt
LIB_SRVR_NAME=$(PROJECT_PREFIX)_srvr
ifeq ($(BUILD_FOR_UNITEST),1)
	INSTALL_DIR=./$(OBJ_DIR_ROOT)/inst
else
	INSTALL_DIR=/usr
endif
UNITEST_EXE=z_$(LIB_CLNT_NAME)_unitest

# ********** Extract git information *********
ifeq ($(COMPILATION_DATE),)
	ifeq ($(BUILD_FOR_UNITEST),1)
		COMPILATION_DATE := \\x1b\[1\;31m\!\!\!\!\ Non\ Production\ \!\!\!\!\\x1b\[0\;0m
		CFLAGS += -DUNITEST_ENV=1
	else
		COMPILATION_DATE := BuildDate\($(shell date +%-d/%b/%Y:"%T")\)\[$(shell uname -r)\]
	endif
endif
ifeq ($(HAS_LOCAL_GIT),)
	HAS_LOCAL_GIT=$(shell git rev-parse --is-inside-work-tree)
endif
ifeq ($(HAS_LOCAL_GIT),true)
	COMMIT_ID=$(shell git log -n1 --format=%h)
	VER_TAGID=$(shell git describe --abbrev=0)
	BRANCH_NAME=$(shell git symbolic-ref 2>/dev/null --short --quiet HEAD || git rev-parse 2>/dev/null --abbrev-ref HEAD)
endif
# Dummy commit info
ifeq ($(COMMIT_ID),)
	COMMIT_ID := 010101010
	VER_TAGID := v3.1.0
	BRANCH_NAME := Unknown
endif

# ********** Define Linker flags *********
LFLAGS_ALL = -lpthread -rdynamic
LFLAGS_EXT =
# ********** External libraries ********* heck for lib using pkg-config and update flags appropriately
EXTLIB=liburing
ifneq ($(shell pkg-config --exists $(EXTLIB) && echo yes),)
	CFLAGS += -DHAS_URING_LIB $(shell pkg-config --cflags $(EXTLIB))
	LFLAGS_EXT = $(shell pkg-config --libs $(EXTLIB))
endif

# ********** Define compilation/Linker flags *********
CFLAGS += -DCOMPILATION_DATE=${COMPILATION_DATE} -DCOMMIT_ID=0x$(COMMIT_ID)UL -DVER_TAGID=$(VER_TAGID) -DBRANCH_NAME=$(BRANCH_NAME)
CFLAGS += -Wall -Werror -Wextra -Wshadow -Werror=strict-aliasing -Wno-nonnull-compare -falign-functions=8 -std=c++2a
CFLAGS += -fPIC -fvisibility=hidden
ifeq ($(BUILD_RELEASE),1)
	USE_SANITIZERS?=0
	CFLAGS += -O2 -g -DNDEBUG
	TRACE_LEVEL?=5
else
	USE_SANITIZERS?=0
	CFLAGS += -g3 -ggdb3
	TRACE_LEVEL?=7
endif
ifeq ($(USE_SANITIZERS),1)
	CFLAGS += -fsanitize=leak -fsanitize=address -fsanitize=undefined
	LFLAGS_ALL += -fsanitize=leak -fsanitize=address -fsanitize=undefined
endif

ifeq ($(USE_THREAD_SANITIZER),1)
	CFLAGS += -fsanitize=thread
	LFLAGS_ALL += -fsanitize=thread
endif
CFLAGS += -DTRACE_LEVEL=$(TRACE_LEVEL)
LFLAGS__SO = -shared $(LFLAGS_EXT) -Wl,--no-undefined $(LFLAGS_ALL)
LFLAGS_EXE = -no-pie $(LFLAGS_ALL)
LFLAGS_EXE__STATIC = $(LFLAGS_EXT) # -static   Add this for container compilation to include libc++ and such in the exe
LFLAGS_EXE_DYNAMIC = -L$(INSTALL_DIR)/lib -l$(LIB_CLNT_NAME) -l$(LIB_SRVR_NAME)
CC=g++

# ********** Actions *********
define print_compilation_info
	@printf "===========================================\n"
	@printf "Compilation info, Release=$(BUILD_RELEASE), COMMIT_ID=$(COMMIT_ID), $(CC), TRACE_LEVEL=$(TRACE_LEVEL) HT=$(HT)|\n"
	@printf "\t* CFLAGS  | $(CFLAGS)\n"
	@printf "\t* INCLUDS | $(INCLUDES)\n"
	@printf "\t* DFLAGS  | $(DFLAGS)\n"
	@printf "\t* L-FLexe | $(LFLAGS_EXE)\n"
	@printf "\t* L-FL.so | $(LFLAGS__SO)\n"
	@printf "\t* LDynExe | $(LFLAGS_EXE_DYNAMIC)\n"
	@printf "\t* LStaExe | $(LFLAGS_EXE__STATIC)\n"
	@printf "\t* INSTALL | $(INSTALL_DIR)\n"
	@printf "===========================================\n"
endef

define print_synamic_dependencies
	@printf "Dynamic dependencies analysis:\n"
	ldd $(OBJ_DIR)/$(LIB_CLNT_NAME).so
	ldd $(OBJ_DIR)/$(LIB_SRVR_NAME).so
	ldd $(UNITEST_EXE)_st
	ldd $(UNITEST_EXE)
	@printf "===========================================\n"
endef

help:
	$(call print_compilation_info);
	@printf "Usage |\e[0;32mmake $(LIB_CLNT_NAME).a\e[0;0m| for building the client lib\n"
	@printf "Usage |\e[0;32mmake $(LIB_SRVR_NAME).a\e[0;0m| for building the server lib\n"
	@printf "Usage |\e[0;32mmake all\e[0;0m| for building libs + executable unitest\n"
	@printf "\n\n\n\n"

all: $(SOURCES_ALL) install $(UNITEST_EXE) $(UNITEST_EXE)_st
	$(if $(filter 1,$(VERBOSE)),$(call print_compilation_info))
	$(if $(filter 1,$(VERBOSE)),$(call print_synamic_dependencies))
	$(info +--->100% Done!)

define print_building_target
	@printf "+--->Building |\e[0;32m$@\e[0;0m|\n"
endef
define print_executed_rule
	@printf "   +--->: |\e[0;32m$@\e[0;0m|${1}\n"
endef
define link_executable
	$(ECHO_CMD) $(CC) -o $@ $^ $(LFLAGS_EXE) ${1} ${2} ${3} ${4};
	$(call print_executed_rule,"=Executable\\n");
endef

$(OBJ_DIR)/$(LIB_COMN_NAME).a: $(OBJECTS_BASE)
	$(call print_building_target);
	$(ECHO_CMD) ld -r $(OBJECTS_BASE) -o $@

$(OBJ_DIR)/$(LIB_CLNT_NAME).a: $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJECTS_CLNT)
	$(call print_building_target);
	$(ECHO_CMD) ld -r $(OBJECTS_CLNT) -o $@

$(OBJ_DIR)/$(LIB_SRVR_NAME).a: $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJECTS_SRVR)
	$(call print_building_target);
	$(ECHO_CMD) ld -r $(OBJECTS_SRVR) -o $@

$(OBJ_DIR)/$(LIB_CLNT_NAME).so: $(OBJECTS_BASE) $(OBJECTS_CLNT)
	$(call print_building_target);
	$(ECHO_CMD) $(CC) -o $@ $^ $(LFLAGS__SO)

$(OBJ_DIR)/$(LIB_SRVR_NAME).so: $(OBJECTS_BASE) $(OBJECTS_SRVR)
	$(call print_building_target);
	$(ECHO_CMD) $(CC) -o $@ $^ $(LFLAGS__SO)

$(UNITEST_EXE)_st: $(OBJECTS_TEST) $(OBJ_DIR)/$(LIB_CLNT_NAME).a $(OBJ_DIR)/$(LIB_COMN_NAME).a $(OBJ_DIR)/$(LIB_SRVR_NAME).a
	$(call print_building_target);
	$(call link_executable, $(LFLAGS_EXE__STATIC))
	@printf "+-->Run Exe: \e[1;45m./$@ -h\e[0;0m\n"

$(UNITEST_EXE): $(OBJECTS_TEST)
	$(call print_building_target);
	$(call link_executable, $(LFLAGS_EXE_DYNAMIC))
	@printf "+-->Run Exe: \e[1;45mLD_LIBRARY_PATH=$(INSTALL_DIR)/lib ./$@ -h\e[0;0m\n"

$(OBJ_DIR)/%.o: %.cpp Makefile
	@mkdir -p $(dir $@)
	$(ECHO_CMD) $(CC) $(CFLAGS) $(INCLUDES) $(DFLAGS) -o $@ -c $<
	$(call print_executed_rule)

install: $(OBJ_DIR)/$(LIB_CLNT_NAME).so $(OBJ_DIR)/$(LIB_SRVR_NAME).so gusli_client_api.hpp gusli_server_api.hpp
	@printf "+-->Install to |\e[1;45m$(INSTALL_DIR)\e[0;0m|\n"
	@mkdir -p $(INSTALL_DIR)/lib $(INSTALL_DIR)/include;
#	@install -m 644 $(OBJ_DIR)/$(LIB_SRVR_NAME).a  $(INSTALL_DIR)/lib
	@install -m 755 $(OBJ_DIR)/$(LIB_CLNT_NAME).so $(INSTALL_DIR)/lib/lib$(LIB_CLNT_NAME).so
	@install -m 755 $(OBJ_DIR)/$(LIB_SRVR_NAME).so $(INSTALL_DIR)/lib/lib$(LIB_SRVR_NAME).so
	@install -m 644 gusli_client_api.hpp $(INSTALL_DIR)/include
	@install -m 644 gusli_server_api.hpp $(INSTALL_DIR)/include
#	@tree $(INSTALL_DIR)
#	ldconfig

uninstall:
	@printf "+-->Unistall from |\e[1;45m$(INSTALL_DIR)\e[0;0m|\n"
	@rm -f $(INSTALL_DIR)/lib/lib$(PROJECT_PREFIX)_*
	@rm -f $(INSTALL_DIR)/lib/$(PROJECT_PREFIX)_*
	@rm -f $(INSTALL_DIR)/include/$(PROJECT_PREFIX)_*
#	ldconfig
# ********** Clean *********
define clean_compilation_intermediate
	@printf "+-->Cleaning recursive dir |\e[1;45m${1}/*/.[oda]\e[0;0m|\n"
	@find ./${1} -type f -name '*.[oda]' -delete
endef
define clean_compilation_output
	@printf "+-->Cleaning |\e[1;45mExe / Lib.so / CrashDumps\e[0;0m|\n"
	@find ./${1} -type f -name '*.so' -delete
	@rm -f $(UNITEST_EXE) $(OBJ_DIR)/*.so core.*
endef

clean: uninstall
	@printf "+-->Cleaning |\e[1;45mall\e[0;0m|\n"
	$(call clean_compilation_intermediate,$(OBJ_DIR_ROOT))
	$(call clean_compilation_output)

.PHONY: depend clean uninstall
-include $(DEPEND_FILES_ALL)
