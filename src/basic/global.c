#include "global.h"
#include <time.h>
#include <sys/time.h> // for seeding random
#include <unistd.h> // getpid
#include <pthread.h>

#include "util.h"

FILE *ctx_msg_out = NULL;
pthread_mutex_t biglock;

void* ctx_malloc(size_t mem, const char *file, int line)
{
  void *ptr = malloc(mem);
  if(ptr == NULL) {
    char memstr[100];
    bytes_to_str(mem, 1, memstr);
    call_die(file, line, "Out of memory (malloc %s)", memstr);
  }
  return ptr;
}

void* ctx_calloc(size_t nel, size_t elsize, const char *file, int line)
{
  void *ptr = calloc(nel, elsize);
  if(ptr == NULL) {
    char nelstr[100], elsizestr[100], memstr[100];
    ulong_to_str(nel, nelstr);
    bytes_to_str(elsize, 1, elsizestr);
    bytes_to_str(nel * elsize, 1, memstr);
    call_die(file, line, "Out of memory (calloc %s x %s = %s)",
             nelstr, elsizestr, memstr);
  }
  return ptr;
}

void* ctx_realloc(void *ptr, size_t mem, const char *file, int line)
{
  void *ptr2 = realloc(ptr, mem);
  if(ptr2 == NULL) {
    char memstr[100];
    bytes_to_str(mem, 1, memstr);
    call_die(file, line, "Out of memory (realloc %s)", memstr);
  }
  return ptr2;
}

void call_die(const char *file, int line, const char *fmt, ...)
{
  pthread_mutex_lock(&biglock);
  va_list argptr;
  fflush(stdout);
  fprintf(stderr, "[%s:%i] Error: ", file, line);
  va_start(argptr, fmt);
  vfprintf(stderr, fmt, argptr);
  va_end(argptr);
  if(*(fmt + strlen(fmt) - 1) != '\n') fputc('\n', stderr);
  ftimestamp(stderr);
  fputs(" Fatal Error\n", stderr);
  exit(EXIT_FAILURE);
}

void call_warn(const char *file, int line, const char *fmt, ...)
{
  pthread_mutex_lock(&biglock);
  va_list argptr;
  fflush(stdout);
  fprintf(stderr, "[%s:%i] Warning: ", file, line);
  va_start(argptr, fmt);
  vfprintf(stderr, fmt, argptr);
  va_end(argptr);
  if(*(fmt + strlen(fmt) - 1) != '\n') fputc('\n', stderr);
  fflush(stderr);
  pthread_mutex_unlock(&biglock);
}

// A function for standard output
void fmessage(FILE *fh, const char *fmt, ...)
{
  if(fh != NULL) {
    va_list argptr;
    va_start(argptr, fmt);
    vfprintf(fh, fmt, argptr);
    va_end(argptr);
    fflush(fh);
  }
}

void ftimestamp(FILE *fh)
{
  time_t t;
  char tstr[100];

  if(fh != NULL) {
    time(&t);
    strftime(tstr, 100, "[%d %b %Y %H:%M:%S]", localtime(&t));
    fputs(tstr, fh);
  }
}

// A function for standard output
void fstatus(FILE *fh, const char *fmt, ...)
{
  va_list argptr;

  if(fh != NULL) {
    pthread_mutex_lock(&biglock);
    ftimestamp(fh);
    if(fmt[0] != ' ' && fmt[0] != '[') fputc(' ', fh);
    va_start(argptr, fmt);
    vfprintf(fh, fmt, argptr);
    va_end(argptr);
    if(fmt[strlen(fmt)-1] != '\n') fputc('\n', fh);
    fflush(fh);
    pthread_mutex_unlock(&biglock);
  }
}

void print_usage(const char *msg, const char *errfmt,  ...)
{
  if(errfmt != NULL) {
    pthread_mutex_lock(&biglock);
    fprintf(stderr, "Error: ");
    va_list argptr;
    va_start(argptr, errfmt);
    vfprintf(stderr, errfmt, argptr);
    va_end(argptr);

    if(errfmt[strlen(errfmt)-1] != '\n') fputc('\n', stderr);
  }

  fputs(msg, stderr);
  exit(EXIT_FAILURE);
}

void seed_random()
{
  struct timeval now;
  gettimeofday(&now, NULL);
  srand((unsigned int)(((now.tv_sec ^ getpid()) * 1000001) + now.tv_usec));
  srand48(((now.tv_sec ^ getpid()) * 1000003) + now.tv_usec);
}

void cortex_init()
{
  seed_random();
  if(pthread_mutex_init(&biglock, NULL) != 0) {
    printf("%s:%i: mutex init failed\n", __FILE__, __LINE__);
    exit(EXIT_FAILURE);
  }
}

void cortex_destroy()
{
  pthread_mutex_destroy(&biglock);
}
