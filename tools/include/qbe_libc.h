#ifndef QBE_PORT_LIBC
#define QBE_PORT_LIBC
/* LP64 reference declarations, parsed only by the source migration tool. */
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long int64_t;
typedef unsigned long uint64_t;
typedef unsigned long uintptr_t;
typedef struct QbeFile FILE;
typedef unsigned long va_list;
#define NULL ((void *)0)
#define EOF (-1)
#define CHAR_BIT 8
#define INT_MAX 2147483647
#define INT_MIN (-2147483647-1)
#define UINT_MAX 4294967295U
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#define INT64_MAX 9223372036854775807L
#define INT64_MIN (-9223372036854775807L-1)
#define UINT64_MAX 18446744073709551615UL
#define SIZE_MAX ULONG_MAX
#define PRId64 "ld"
#define PRIi64 "li"
#define PRId32 "d"
#define INT32_MAX INT_MAX
#define INT32_MIN INT_MIN
#define UINT32_MAX UINT_MAX
#define CHAR_MAX 127
#define UCHAR_MAX 255
#define PRIu64 "lu"
#define PRIx64 "lx"
#define bool _Bool
#define true 1
#define false 0
#define assert(x) __qbe_assert((x), #x, __FILE__, __LINE__)
#define va_start(a,b) __qbe_va_start(a,b)
#define va_arg(a,t) ((t)__qbe_va_arg(a))
#define va_end(a) __qbe_va_end(a)
extern FILE *stdin, *stdout, *stderr;
extern int optind, opterr, optopt;
extern char *optarg;
void __qbe_assert(int, const char *, const char *, int);
void __qbe_va_start(va_list, ...);
unsigned long __qbe_va_arg(va_list);
void __qbe_va_end(va_list);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void abort(void);
void exit(int);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int atoi(const char *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **);
float strtof(const char *, char **);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
int fprintf(FILE *, const char *, ...);
int vfprintf(FILE *, const char *, va_list);
int printf(const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int sprintf(char *, const char *, ...);
int fputs(const char *, FILE *);
int puts(const char *);
int fputc(int, FILE *);
int putchar(int);
int getc(FILE *);
int fgetc(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
int ungetc(int, FILE *);
int feof(FILE *);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
int fflush(FILE *);
int abs(int);
int isblank(int);
int fscanf(FILE *, const char *, ...);
int isalpha(int);
int isdigit(int);
int isspace(int);
int isalnum(int);
int isprint(int);
int toupper(int);
int tolower(int);
int getopt(int, char **, const char *);
#endif
