/* Spouštění externích nástrojů (assembler, linker) a cesty pro režim -x.
 * Jen C reference: soubor se nepřekládá do LSL, generovaná verze používá
 * ruční protějšky v lsl/luxir_libc.lsl (c_luxir_run, c_luxir_exists,
 * c_luxir_exedir, c_luxir_unlink) se stejným chováním. */
#define _POSIX_C_SOURCE 200809L
#include "all.h"
#include <errno.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Spustí argv[0] (absolutní cesta, bez hledání v PATH) a počká na konec.
 * Vrací exit status, 128 + signál, 126 při selhání čekání a 127, když
 * program nejde spustit; stejně jako run() v LSL. */
int
luxir_run(char **argv)
{
	pid_t pid;
	int status;

	fflush(stdout);
	fflush(stderr);
	if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0)
		return 127;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return 126;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 126;
}

/* 1, když cesta existuje a je to běžný soubor. */
int
luxir_exists(char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Adresář spuštěného luxir (bez koncového lomítka), nebo NULL. */
char *
luxir_exedir(void)
{
	static char buf[4096];
	ssize_t n;

	n = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (n <= 0)
		return NULL;
	buf[n] = 0;
	while (n > 0 && buf[n-1] != '/')
		n--;
	if (n <= 1)
		return NULL;
	buf[n-1] = 0;
	return buf;
}

int
luxir_unlink(char *path)
{
	return unlink(path);
}
