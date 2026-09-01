export SHEN_C_HOME=${CURDIR}
SRC_ROOT=src/c
BIN_ROOT=bin
OBJ_ROOT=obj
TEST_SRC_ROOT=test/c
PROFILE_ROOT=prof

SRCS=$(wildcard ${SRC_ROOT}/*.c)
SRC_OBJS=$(patsubst ${SRC_ROOT}/%.c, obj/%.o, $(SRCS))
LIB_OBJS=$(filter-out obj/main.o,${SRC_OBJS})
TARGET=${BIN_ROOT}/shen-c
LIB_TARGET=${BIN_ROOT}/libshenc.a
TEST_BIN=${BIN_ROOT}/test_foundation
ABI_TEST_BIN=${BIN_ROOT}/test_abi
EMIT_TEST_BIN=${BIN_ROOT}/test_emit
BUILDER_BIN=${BIN_ROOT}/yggdrasil-build
SUM_FIXTURE=test/fixtures/sum
SUM_APP=${BIN_ROOT}/sum-app
SUM_BIN=${SUM_APP}/app
FIB_FIXTURE=test/fixtures/fib-small
FIB_APP=${BIN_ROOT}/fib-small-app
FIB_BIN=${FIB_APP}/app
HELLO_FIXTURE=test/fixtures/hello-ygg
HELLO_APP=${BIN_ROOT}/hello-ygg-app
HELLO_BIN=${HELLO_APP}/app
FIB_YGG_FIXTURE=test/fixtures/fib-ygg
FIB_YGG_APP=${BIN_ROOT}/fib-ygg-app
FIB_YGG_BIN=${FIB_YGG_APP}/app
TC_YGG_FIXTURE=test/fixtures/tc-ygg
TC_YGG_APP=${BIN_ROOT}/tc-ygg-app
TC_YGG_BIN=${TC_YGG_APP}/app
INTERP_AOT_FIXTURE=test/fixtures/interp-aot
INTERP_AOT_APP=${BIN_ROOT}/interp-aot-app
INTERP_AOT_BIN=${INTERP_AOT_APP}/app
RUNME_AOT_FIXTURE=test/fixtures/runme-aot
RUNME_AOT_APP=${BIN_ROOT}/runme-aot-app
RUNME_AOT_BIN=${RUNME_AOT_APP}/app
AR ?= ar
PROFILE=${PROFILE_ROOT}/shen-c.prof
PROFILE_TEXT=${PROFILE_ROOT}/shen-c.prof.txt
PROFILE_PDF=${PROFILE_ROOT}/shen-c.prof.pdf
PROFILE_SIGNAL=${PROFILE_ROOT}/shen-c.prof.0
PROFILE_SIGNAL_TEXT=${PROFILE_ROOT}/shen-c.prof.0.txt

CC ?= clang
PKG_CONFIG ?= pkg-config
SANITIZE ?= 0

BDWGC_PKG := $(shell $(PKG_CONFIG) --exists bdw-gc && echo yes)
ifeq ($(BDWGC_PKG),yes)
    BDWGC_CFLAGS := $(shell $(PKG_CONFIG) --cflags bdw-gc)
    BDWGC_LIBS := $(shell $(PKG_CONFIG) --libs bdw-gc)
else
    $(error bdw-gc not found via pkg-config. Use the Nix shell: nix develop)
endif

ifneq ($(findstring Homebrew,$(BDWGC_CFLAGS)$(BDWGC_LIBS)),)
    $(error Homebrew libgc paths are not allowed; use Nix pkg-config bdw-gc)
endif
ifneq ($(findstring /opt/homebrew,$(BDWGC_CFLAGS)$(BDWGC_LIBS)),)
    $(error Homebrew libgc paths are not allowed; use Nix pkg-config bdw-gc)
endif

CFLAGS ?= -O3
CFLAGS += -std=c17 -fno-optimize-sibling-calls -fsigned-char $(BDWGC_CFLAGS)
LDFLAGS += $(BDWGC_LIBS)

ifeq ($(SANITIZE),1)
    # ASan intercepts mmap and deadlocks uninstrumented Boehm GC_init on Darwin.
    ifeq ($(shell uname -s),Darwin)
        CFLAGS += -fsanitize=undefined -fno-omit-frame-pointer -g -O1
        LDFLAGS += -fsanitize=undefined
    else
        CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
        LDFLAGS += -fsanitize=address,undefined
    endif
endif

ifeq ($(OS),Windows_NT)
    $(error "Windows is not supported.")
else ifeq ($(shell uname -s),Darwin)
    OS_NAME=macos
    ARCH_NAME=$(shell uname -p)
	ARCHIVE_SUFFIX=.tar.gz
else ifeq ($(shell uname -s),Linux)
    OS_NAME=linux
    ARCH_NAME=$(shell uname -p)
	ARCHIVE_SUFFIX=.tar.gz
else
    $(error "The OS is not supported.")
endif

GIT := $(shell command -v git 2>/dev/null)
ifeq ($(GIT),)
    GIT_VERSION=unknown
else
    GIT_VERSION=$(shell git tag -l --contains HEAD 2>/dev/null)
    ifeq ("$(GIT_VERSION)","")
	GIT_VERSION=$(shell git rev-parse --short HEAD 2>/dev/null)
    endif
endif

RELEASE_ROOT=release
RELEASE_ARCHIVE_DIR_NAME=shen-c-${GIT_VERSION}-${OS_NAME}-${ARCH_NAME}
RELEASE_ARCHIVE_DIR=${RELEASE_ROOT}/${RELEASE_ARCHIVE_DIR_NAME}
RELEASE_ARCHIVE_NAME=${RELEASE_ARCHIVE_DIR_NAME}${ARCHIVE_SUFFIX}
CMAKE_BUILD_DIR=build

all: ${OBJ_ROOT} ${BIN_ROOT} ${TARGET} ${LIB_TARGET} ${BUILDER_BIN}
${TARGET}: ${SRC_OBJS}
	${CC} -o $@ $^ ${LDFLAGS}

${LIB_TARGET}: ${LIB_OBJS} | ${BIN_ROOT}
	rm -f $@
	${AR} rcs $@ ${LIB_OBJS}

${OBJ_ROOT}/%.o: $(SRC_ROOT)/%.c | ${OBJ_ROOT}
	${CC} ${CFLAGS} -c -o $@ $<

${OBJ_ROOT}:
	mkdir -p ${OBJ_ROOT}

${BIN_ROOT}:
	mkdir -p ${BIN_ROOT}

${TEST_BIN}: ${LIB_OBJS} ${TEST_SRC_ROOT}/test_foundation.c | ${BIN_ROOT} ${OBJ_ROOT}
	${CC} ${CFLAGS} -iquote ${SRC_ROOT} -o $@ ${TEST_SRC_ROOT}/test_foundation.c ${LIB_OBJS} ${LDFLAGS}

${ABI_TEST_BIN}: ${LIB_TARGET} ${TEST_SRC_ROOT}/test_abi.c | ${BIN_ROOT}
	${CC} ${CFLAGS} -iquote ${SRC_ROOT} -o $@ ${TEST_SRC_ROOT}/test_abi.c ${LIB_TARGET} ${LDFLAGS}

${EMIT_TEST_BIN}: ${LIB_TARGET} ${TEST_SRC_ROOT}/test_emit.c | ${BIN_ROOT}
	${CC} ${CFLAGS} -iquote ${SRC_ROOT} -o $@ ${TEST_SRC_ROOT}/test_emit.c ${LIB_TARGET} ${LDFLAGS}

${BUILDER_BIN}: ${LIB_TARGET} tools/yggdrasil-build.c | ${BIN_ROOT}
	${CC} ${CFLAGS} -iquote ${SRC_ROOT} -o $@ tools/yggdrasil-build.c ${LIB_TARGET} ${LDFLAGS}

${SUM_APP}: ${BUILDER_BIN} ${SUM_FIXTURE}/user.kl ${SUM_FIXTURE}/kernel.kl
	rm -rf ${SUM_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${SUM_FIXTURE} ${SUM_APP}

${FIB_APP}: ${BUILDER_BIN} ${FIB_FIXTURE}/user.kl ${FIB_FIXTURE}/kernel.kl
	rm -rf ${FIB_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${FIB_FIXTURE} ${FIB_APP}

${HELLO_APP}: ${BUILDER_BIN} ${HELLO_FIXTURE}/hello.kl ${HELLO_FIXTURE}/kernel.kl
	rm -rf ${HELLO_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${HELLO_FIXTURE} ${HELLO_APP}

${FIB_YGG_APP}: ${BUILDER_BIN} ${FIB_YGG_FIXTURE}/fib.kl ${FIB_YGG_FIXTURE}/kernel.kl
	rm -rf ${FIB_YGG_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${FIB_YGG_FIXTURE} ${FIB_YGG_APP}

${TC_YGG_APP}: ${BUILDER_BIN} ${TC_YGG_FIXTURE}/tc-interp.kl ${TC_YGG_FIXTURE}/kernel.kl ${TC_YGG_FIXTURE}/interpreter.shen
	rm -rf ${TC_YGG_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${TC_YGG_FIXTURE} ${TC_YGG_APP}

${INTERP_AOT_APP}: ${BUILDER_BIN} ${INTERP_AOT_FIXTURE}/interp-aot.kl ${INTERP_AOT_FIXTURE}/kernel.kl
	rm -rf ${INTERP_AOT_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${INTERP_AOT_FIXTURE} ${INTERP_AOT_APP}

OVERLAY_KL_DIR=obj/overlay
INTERPRETER_SHEN=shen/test/s42/interpreter.shen
PROLOGINTERP_SHEN=shen/test/s42/prologinterp.shen
CODEGEN_SHEN_AOT=scripts/codegen-shen-aot.sh

${OVERLAY_KL_DIR}/interpreter.kl: ${TARGET} ${INTERPRETER_SHEN} ${CODEGEN_SHEN_AOT} | ${OBJ_ROOT}
	mkdir -p ${OVERLAY_KL_DIR}
	${CODEGEN_SHEN_AOT} ${OVERLAY_KL_DIR} ${INTERPRETER_SHEN}

${OVERLAY_KL_DIR}/prologinterp.kl: ${TARGET} ${PROLOGINTERP_SHEN} ${CODEGEN_SHEN_AOT} | ${OBJ_ROOT}
	mkdir -p ${OVERLAY_KL_DIR}
	${CODEGEN_SHEN_AOT} ${OVERLAY_KL_DIR} ${PROLOGINTERP_SHEN}

# 579-defun runme AOT runner: kerneltests shake + incremental sidecar KL
# (seed maxinferences, qmachine exists, depth, sum). O2 overlay-after-load
# is killed for this app: L-interp typecheck is inside load-help (1.18M
# inf) before any defun swap, so install cannot drop the ~16s wall.
# Overlay emit/install stay in libshenc + tests; do not wrap load here.
${RUNME_AOT_APP}: ${BUILDER_BIN} ${RUNME_AOT_FIXTURE}/kernel.kl ${RUNME_AOT_FIXTURE}/seed.kl ${RUNME_AOT_FIXTURE}/qmachine.kl ${RUNME_AOT_FIXTURE}/depth.kl ${RUNME_AOT_FIXTURE}/sum.kl ${RUNME_AOT_FIXTURE}/runme.kl
	rm -rf ${RUNME_AOT_APP}
	SHEN_C_HOME=${CURDIR} ${BUILDER_BIN} ${RUNME_AOT_FIXTURE} ${RUNME_AOT_APP}
	! grep -F shen_wrap_load_for_overlays ${RUNME_AOT_APP}/app.c
	! grep -F shen_register_overlay ${RUNME_AOT_APP}/app.c
	test ! -f ${RUNME_AOT_APP}/overlay_interpreter.c
	grep -F shen_add ${RUNME_AOT_APP}/app.c
	grep -F shen_cons ${RUNME_AOT_APP}/app.c
	grep -F shen_eq ${RUNME_AOT_APP}/app.c
	grep -F shen_cons_p ${RUNME_AOT_APP}/app.c
	grep -F shen_mul ${RUNME_AOT_APP}/app.c
	grep -F shen_string_p ${RUNME_AOT_APP}/app.c
	grep -F shen_symbol_p ${RUNME_AOT_APP}/app.c
	grep -F shen_absvector_p ${RUNME_AOT_APP}/app.c
	grep -F 'apply_direct(ctx, "vector?"' ${RUNME_AOT_APP}/app.c
	grep -F shen_apply_direct ${RUNME_AOT_APP}/app.c

test: ${TEST_BIN} ${ABI_TEST_BIN} ${EMIT_TEST_BIN} ${SUM_APP} ${FIB_APP} ${HELLO_APP} ${FIB_YGG_APP} ${TC_YGG_APP} ${INTERP_AOT_APP} ${TARGET}
	ASAN_OPTIONS=detect_leaks=0 ${TEST_BIN}
	ASAN_OPTIONS=detect_leaks=0 ${ABI_TEST_BIN}
	ASAN_OPTIONS=detect_leaks=0 ${EMIT_TEST_BIN}
	ASAN_OPTIONS=detect_leaks=0 ${SUM_BIN} | grep -Fq 42
	test -f ${SUM_APP}/Makefile
	test -f ${SUM_APP}/CMakeLists.txt
	grep -F shen_register_defun ${SUM_APP}/app.c
	grep -F native_ ${SUM_APP}/app.c
	! grep -F shen_eval_kl ${SUM_APP}/app.c
	cmake -G Ninja -S ${SUM_APP} -B ${SUM_APP}/build -D SHEN_C_HOME=${CURDIR}
	cmake --build ${SUM_APP}/build
	ASAN_OPTIONS=detect_leaks=0 ${SUM_BIN} | grep -Fq 42
	ASAN_OPTIONS=detect_leaks=0 ${FIB_BIN} | grep -Fq 6765
	test -f ${FIB_APP}/Makefile
	test -f ${FIB_APP}/CMakeLists.txt
	grep -F shen_register_defun ${FIB_APP}/app.c
	grep -F native_ ${FIB_APP}/app.c
	grep -F 'goto tail_start_' ${FIB_APP}/app.c
	grep -F shen_tail_apply ${FIB_APP}/app.c
	! grep -F shen_eval_kl ${FIB_APP}/app.c
	test $$(grep -c shen_register_defun ${HELLO_APP}/app.c) -eq 54
	grep -q 'static KLObject\* native_' ${HELLO_APP}/app.c
	grep -F shen_apply_port_overwrites ${HELLO_APP}/app.c
	test -f ${HELLO_APP}/Makefile
	test -f ${HELLO_APP}/CMakeLists.txt
	! grep -F shen_eval_kl ${HELLO_APP}/app.c
	ASAN_OPTIONS=detect_leaks=0 ${HELLO_BIN} | grep -Fq 'hello from shaken shen'
	test $$(grep -c shen_register_defun ${FIB_YGG_APP}/app.c) -eq 55
	grep -q 'static KLObject\* native_' ${FIB_YGG_APP}/app.c
	grep -F shen_apply_port_overwrites ${FIB_YGG_APP}/app.c
	test -f ${FIB_YGG_APP}/Makefile
	test -f ${FIB_YGG_APP}/CMakeLists.txt
	! grep -F shen_eval_kl ${FIB_YGG_APP}/app.c
	! grep -F load_kl_file ${FIB_YGG_APP}/app.c
	ASAN_OPTIONS=detect_leaks=0 ${FIB_YGG_BIN} | grep -Fqx 'fib 20 = 6765'
	test $$(grep -c shen_register_defun ${TC_YGG_APP}/app.c) -eq 568
	grep -F 'shen.typecheck' ${TC_YGG_APP}/app.c
	grep -F 'shen.t*' ${TC_YGG_APP}/app.c
	grep -F shen_register_defun ${TC_YGG_APP}/app.c
	grep -q 'static KLObject\* native_' ${TC_YGG_APP}/app.c
	grep -F shen_eval_kl ${TC_YGG_APP}/app.c
	grep -F shen_apply_port_overwrites ${TC_YGG_APP}/app.c
	! grep -F load_kl_file ${TC_YGG_APP}/app.c
	test -f ${TC_YGG_APP}/Makefile
	test -f ${TC_YGG_APP}/CMakeLists.txt
	out=$$(cd ${TC_YGG_FIXTURE} && ASAN_OPTIONS=detect_leaks=0 ${CURDIR}/${TC_YGG_BIN}) && \
	  printf '%s\n' "$$out" | grep -F 'inferences = 224811' && \
	  printf '%s\n' "$$out" | grep -F 'normal-form = 7'
	test $$(grep -c shen_register_defun ${INTERP_AOT_APP}/app.c) -eq 83
	grep -F 'native_kl_normal_2dform' ${INTERP_AOT_APP}/app.c
	grep -F shen_register_defun ${INTERP_AOT_APP}/app.c
	grep -q 'static KLObject\* native_' ${INTERP_AOT_APP}/app.c
	grep -F shen_native_closure ${INTERP_AOT_APP}/app.c
	grep -F shen_apply_port_overwrites ${INTERP_AOT_APP}/app.c
	! grep -F shen_eval_kl ${INTERP_AOT_APP}/app.c
	! grep -F load_kl_file ${INTERP_AOT_APP}/app.c
	! grep -F eval_kl_object ${INTERP_AOT_APP}/app.c
	test -f ${INTERP_AOT_APP}/Makefile
	test -f ${INTERP_AOT_APP}/CMakeLists.txt
	ASAN_OPTIONS=detect_leaks=0 ${INTERP_AOT_BIN} | grep -F 'normal-form = 7'
	${TARGET} --version
	SHEN_C_HOME=${CURDIR} ${TARGET} eval -e '(version)'
	env -u SHEN_C_HOME ${TARGET} eval -e '(version)'
	env -u SHEN_C_HOME ${TARGET} eval -e '(version)' | grep -qx 42
	SHEN_C_HOME=${CURDIR} ${TARGET} eval -e '(+ 1 1)'
	env -u SHEN_C_HOME ${TARGET} eval -e '(@s "foo" "bar")' | grep -Fqx foobar
	env -u SHEN_C_HOME ${TARGET} eval -e '(tlstr "hello")' | grep -Fqx ello
	env -u SHEN_C_HOME ${TARGET} eval -e '(n->string 65)' | grep -Fqx A
	env -u SHEN_C_HOME ${TARGET} eval -e '(trap-error (simple-error "boom") (lambda E (error-to-string E)))' | grep -Fqx boom
	@d=$$(mktemp -d) && \
	  printf '%s\n' "(let S (open \"$$d/out\" out) (do (pr \"hello\" S) (close S)))" > "$$d/w.shen" && \
	  env -u SHEN_C_HOME ${TARGET} eval -q -l "$$d/w.shen" && \
	  test -s "$$d/out" && \
	  rm -rf "$$d"

# Canonical S42 suite (Tarver runme.shen). Pipe yes for y-or-n? on fail.
# Grep the last passed/failed report; intermediate groups also print failed ... 0.
# Ship only when evidence/certify.log contains "passed ... 134" and "failed ... 0".
certify: ${TARGET}
	mkdir -p evidence
	{ \
	  echo "start_utc: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	  git rev-parse HEAD 2>/dev/null || true; \
	  echo "bin: ${CURDIR}/${TARGET}"; \
	  echo "cwd: ${CURDIR}/shen/test/s42"; \
	  echo "cmd: yes | env -u SHEN_C_HOME ${CURDIR}/${TARGET} script runme.shen"; \
	} > evidence/certify.log
	cd shen/test/s42 && yes | env -u SHEN_C_HOME ../../../${TARGET} script runme.shen >> ../../../evidence/certify.log 2>&1; \
	  echo "exit=$$?" >> ${CURDIR}/evidence/certify.log
	echo "end_utc: $$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> evidence/certify.log
	cp evidence/certify.log evidence/corpus.log
	grep -E 'passed \.\.\. 134' evidence/certify.log
	tail -n 40 evidence/certify.log | grep -E 'failed \.\.\. 0'

repl: all
	${TARGET}

rrepl: all
	rlwrap -n ${TARGET}

gperf: all
	mkdir -p prof
	env CPUPROFILE=${PROFILE} ${TARGET}

gperf_signal: all
	rm ${PROFILE}
	mkdir -p prof
	env CPUPROFILE=${PROFILE} CPUPROFILESIGNAL=12 ${TARGET}

pprof: all
	pprof ${TARGET} ${PROFILE}

pprof_text: all
	pprof ${TARGET} ${PROFILE} --text > ${PROFILE_TEXT}

pprof_signal_text: all
	pprof ${TARGET} ${PROFILE_SIGNAL} --text > ${PROFILE_SIGNAL_TEXT}

pprof_pdf: all
	pprof ${TARGET} ${PROFILE} --pdf > ${PROFILE_PDF}

cmake-ninja:
	cmake -G Ninja -B ${CMAKE_BUILD_DIR} -D CMAKE_C_COMPILER=$(CC) -D SHEN_C_SANITIZE=$(if $(filter 1,$(SANITIZE)),ON,OFF)
	cmake --build ${CMAKE_BUILD_DIR}

cmake-test: cmake-ninja
	ASAN_OPTIONS=detect_leaks=0 ${TEST_BIN}
	ASAN_OPTIONS=detect_leaks=0 ${ABI_TEST_BIN}
	ASAN_OPTIONS=detect_leaks=0 ${EMIT_TEST_BIN}

clean:
	rm -rf ${OBJ_ROOT}
	rm -rf ${BIN_ROOT}
	rm -rf ${RELEASE_ROOT}
	rm -rf ${CMAKE_BUILD_DIR}
	rm -rf build-ci

release: clean all
	mkdir -p ${RELEASE_ARCHIVE_DIR}
	cp -r shen ${RELEASE_ARCHIVE_DIR}
	cp -r bin ${RELEASE_ARCHIVE_DIR}
	cp shen-c ${RELEASE_ARCHIVE_DIR}
	cd ${RELEASE_ROOT}; tar -czvf ${RELEASE_ARCHIVE_NAME} ${RELEASE_ARCHIVE_DIR_NAME}
	rm -rf ${RELEASE_ARCHIVE_DIR}

.PHONY: all test repl rrepl clean pprof pprof_text pprof_signal_text pprof_pdf release cmake-ninja cmake-test certify
