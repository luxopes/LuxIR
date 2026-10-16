.POSIX:
.SUFFIXES: .o .c

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

COMMOBJ  = main.o util.o parse.o abi.o cfg.o mem.o ssa.o alias.o load.o \
           copy.o fold.o gvn.o gcm.o simpl.o ifopt.o live.o spill.o rega.o \
           emit.o w32.o
AMD64OBJ = amd64/targ.o amd64/sysv.o amd64/isel.o amd64/emit.o amd64/winabi.o
ARM64OBJ = arm64/targ.o arm64/abi.o arm64/isel.o arm64/emit.o
RV64OBJ  = rv64/targ.o rv64/abi.o rv64/isel.o rv64/emit.o
WASMOBJ  = wasm/targ.o wasm/emit.o
COBJ     = c/targ.o c/emit.o
I386OBJ  = i386/targ.o i386/abi.o i386/isel.o i386/emit.o
ARMOBJ   = arm/targ.o arm/abi.o arm/isel.o arm/emit.o
ASOBJ    = as/as.o as/x86.o
OBJ      = $(COMMOBJ) $(AMD64OBJ) $(ARM64OBJ) $(RV64OBJ) $(WASMOBJ) $(COBJ) \
           $(I386OBJ) $(ARMOBJ) $(ASOBJ)
# Spouštění nástrojů jen v C; do LSL se nepřekládá (viz tool.c).
TOOLOBJ  = tool.o

SRCALL   = $(OBJ:.o=.c)

CC       = cc
CFLAGS   = -std=c99 -O2 -Wall -Wextra -Wpedantic

luxir: $(OBJ) $(TOOLOBJ)
	$(CC) $(LDFLAGS) $(OBJ) $(TOOLOBJ) -o $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ) $(TOOLOBJ): all.h ops.h
$(AMD64OBJ): amd64/all.h
$(ARM64OBJ): arm64/all.h
$(RV64OBJ): rv64/all.h
$(I386OBJ): i386/all.h
$(ARMOBJ): arm/all.h
$(ASOBJ) main.o: as/as.h
main.o: config.h
main.o parse.o: version.h

config.h:
	@case `uname` in                               \
	*Darwin*)                                      \
		case `uname -m` in                     \
		*arm64*)                               \
			echo "#define Deftgt T_arm64_apple";\
			;;                             \
		*)                                     \
			echo "#define Deftgt T_amd64_apple";\
			;;                             \
		esac                                   \
		;;                                     \
	*)                                             \
		case `uname -m` in                     \
		*aarch64*|*arm64*)                     \
			echo "#define Deftgt T_arm64"; \
			;;                             \
		*riscv64*)                             \
			echo "#define Deftgt T_rv64";  \
			;;                             \
		*)                                     \
			echo "#define Deftgt T_amd64_sysv";\
			;;                             \
		esac                                   \
		;;                                     \
	esac > $@

install: luxir
	mkdir -p "$(DESTDIR)$(BINDIR)"
	install -m755 luxir "$(DESTDIR)$(BINDIR)/luxir"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/luxir"

clean:
	rm -f *.o */*.o luxir

clean-gen: clean
	rm -f config.h

src:
	@echo $(SRCALL)

80:
	@for F in $(SRCALL);                       \
	do                                         \
		awk "{                             \
			gsub(/\\t/, \"        \"); \
			if (length(\$$0) > $@)     \
				printf(\"$$F:%d: %s\\n\", NR, \$$0); \
		}" < $$F;                          \
	done

wc:
	@wc -l $(SRCALL)

.PHONY: clean clean-gen src 80 wc install uninstall
