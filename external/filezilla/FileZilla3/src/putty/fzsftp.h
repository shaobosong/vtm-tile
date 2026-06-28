#ifndef FILEZILLA_PUTTY_FZSFTP_HEADER
#define FILEZILLA_PUTTY_FZSFTP_HEADER

char* priority_read();

/* Proactive-grant download flow control (PARVION_CREDIT_IO; default on). The applet
 * pre-grants free ring slots as unsolicited "-G<off> <len>" lines on the same pipe
 * that carries quota/open/finalize replies; every stdin consumer (priority_read and
 * next_grant) siphons grant lines into a shared FIFO so they never reach
 * ProcessQuotaCmd or an RPC's reply slot. */
int credit_io_enabled(void);
/* Parallel-download chunks via the shm-ring io_thread (PARVION_CHUNK_RING=1; default OFF =
 * the stable inline-write path). See fzsftp.c. */
int chunk_ring_enabled(void);
/* Block until a credit grant is available; returns 1 with (*off,*len) set, or 0 on
 * EOF / applet error ("--1"). Routes any quota replies that arrive while waiting. */
int next_grant(size_t* off, int* len);
/* Discard leftover grants; call at each download open (a reused helper process serves
 * several transfers, and a finished download leaves stale grants in the FIFO). */
void grant_fifo_reset(void);

int ProcessQuotaCmd(const char* line);
int RequestQuota(int i, int bytes);
void UpdateQuota(int i, int bytes);
char* get_input_pushback(void);
int has_input_pushback(void);
char* read_input_line(int force, int* error); /* line-buffered stdin read (both platforms) */

int CurrentSpeedLimit(int direction);

#ifdef _WINDOWS
#include <windows.h>
typedef FILETIME _fztimer;
#else
typedef struct
{
	unsigned int low;
    time_t high;
} _fztimer;
#endif

void fz_timer_init(_fztimer *timer);
int fz_timer_check(_fztimer *timer);

uintptr_t next_int(char ** s);

#endif
