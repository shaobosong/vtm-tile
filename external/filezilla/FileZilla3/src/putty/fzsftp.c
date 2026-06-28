#include "putty.h"
#include "misc.h"
#if defined(HAVE_GETTIMEOFDAY)
  #include <sys/time.h>
#elif defined(HAVE_FTIME)
  #include <sys/timeb.h>
#endif

int bytesAvailable[2] = { 0, 0 };
int limit[2] = { 0, 0 };

char* input_pushback = 0;

#ifndef _WINDOWS
#include <unistd.h>
#endif

/* Line-assembly buffer for read_input_line(). Available on both platforms: on Windows
 * ReadFile() does NOT respect line boundaries (it can coalesce several protocol lines
 * into one read, or split one across reads), which corrupts line-oriented parsing once
 * the applet sends unsolicited "-G" credit grants. Reading one byte at a time and
 * stopping exactly at '\n' (as on Unix) guarantees line boundaries and never over-reads,
 * so it stays consistent with any other consumer of stdin. */
char *input_buf = 0;
int input_buflen = 0, input_bufsize = 0;

/* ---- Proactive-grant download credit FIFO (PARVION_CREDIT_IO) -------------------- *
 * The applet pre-grants free ring slots as unsolicited "-G<off> <len>" lines on the
 * same stdin pipe that carries quota / io_open / io_finalize replies. Every consumer
 * of that pipe (priority_read and next_grant) funnels raw lines through
 * route_async_line(), which siphons grant lines into this FIFO so a grant can never be
 * mistaken for a quota reply (ProcessQuotaCmd would abort) or land in an RPC's reply
 * slot. The OS pipe buffer plus this FIFO form the credit window; write_to_file drains
 * it via next_grant() and only blocks when it is empty (ring full = disk behind). */
#define PARVION_CREDIT_MAX 257            /* > max ring_count (256) */
static size_t grant_off_[PARVION_CREDIT_MAX];
static int    grant_len_[PARVION_CREDIT_MAX];
static int    grant_head_ = 0, grant_tail_ = 0;

int credit_io_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PARVION_CREDIT_IO");
        v = (e && e[0] == '0') ? 0 : 1;
    }
    return v;
}

/* Route parallel-download chunks through the shm-ring/credit-IO io_thread (overlaps the
 * disk write with the network, ~+25% on loopback) instead of the legacy inline write(fd).
 * DEFAULT OFF: the ring path can occasionally wedge a channel at progress 0 under heavy
 * parallelism (16 channels in the UI). Enable with PARVION_CHUNK_RING=1. Both the helper
 * (psftp.c open path) and the applet inherit the same env, so they always agree. */
int chunk_ring_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PARVION_CHUNK_RING");
        v = (e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y')) ? 1 : 0;
    }
    return v;
}

/* Discard any leftover grants. A helper process can serve several transfers (reused
 * connection), and a finished download leaves ~ring_count grants siphoned into the FIFO
 * (or buffered in the pipe and read by finalize's priority_read). Call at each download
 * open, before requesting io_open, so a fresh batch of grants isn't mixed with stale
 * offsets from the previous transfer. */
void grant_fifo_reset(void)
{
    grant_head_ = grant_tail_ = 0;
}

/* If `line` is a credit grant ("-G<off> <len>"), queue it and return 1; else 0. */
static int route_async_line(char* line)
{
    if (line[0] == '-' && line[1] == 'G') {
        char* p = line + 2;
        size_t off = (size_t)next_int(&p);
        int    len = (int)next_int(&p);
        int    i   = grant_tail_ % PARVION_CREDIT_MAX;
        grant_off_[i] = off;
        grant_len_[i] = len;
        ++grant_tail_;
        return 1;
    }
    return 0;
}

int next_grant(size_t* off, int* len)
{
    while (grant_head_ == grant_tail_) {
        int error = 0;
        char* line = read_input_line(1, &error);
        if (line == NULL || error)
            return 0;
        if (route_async_line(line)) { sfree(line); break; } /* a grant -> FIFO; stop reading */
        if (line[0] == '-' && line[1] == '-') { sfree(line); return 0; } /* "--1" error grant */
        else if (line[0] == '-' && (line[1] == '0' || line[1] == '1') &&
                 (line[2] == '-' || (line[2] >= '0' && line[2] <= '9')))
            { ProcessQuotaCmd(line); sfree(line); }      /* a quota reply arriving mid-wait */
        else { if (input_pushback == 0) input_pushback = line; else sfree(line); }
    }
    {
        int i = grant_head_ % PARVION_CREDIT_MAX;
        *off = grant_off_[i];
        *len = grant_len_[i];
        ++grant_head_;
    }
    return 1;
}

char* priority_read()
{
    /* Unified across platforms via the line-buffered read_input_line() (see its note):
     * the old Windows path used a 255-byte ReadFile that ignored line boundaries, which
     * coalesced/split the unsolicited "-G" credit grants and tripped 'input_pushback
     * not null!'. Siphon grants into the FIFO; return the next '-' reply. */
    char* ret = 0;
    while (!ret) {
        int error = 0;
        char* line = read_input_line(1, &error);
        if (line == NULL || error) {
            fzprintf(sftpError, "read_input_line failed in priority_read");
            cleanup_exit(1);
        }

        if (route_async_line(line)) {  /* siphon credit grants into the FIFO */
            sfree(line);
            continue;
        }
        if (line[0] != '-') {
            if (input_pushback != 0) {
                sfree(line);
                fzprintf(sftpError, "input_pushback not null!");
                cleanup_exit(1);
            }
            else {
                input_pushback = line;
            }
        }
        ret = line;
    }
    return ret;
}

static int ReadQuotas(int i)
{
    char* line = priority_read();
    
    ProcessQuotaCmd(line);
    sfree(line);

    return 1;
}

int RequestQuota(int i, int bytes)
{
#ifndef _WINDOWS
    static int tty = -1;
    if (tty == -1) {
        char* debug = getenv("FZDEBUG");
        tty = isatty(0) && isatty(1) && isatty(2) && debug && !strcmp(debug, "1");
        if (tty) {
            return bytes;
        }
    }
    else if (tty) {
        return bytes;
    }
#endif

    if (bytesAvailable[i] < -100) {
        bytesAvailable[i] = 0;
    }
    else if (bytesAvailable[i] < 0) {
        bytesAvailable[i]--;
        return bytes;
    }
    if (bytesAvailable[i] == 0) {
        fznotify(sftpUsedQuotaRecv + i);
        ReadQuotas(i);
    }

    if (bytesAvailable[i] < 0 || bytesAvailable[i] > bytes) {
        return bytes;
    }

    return bytesAvailable[i];
}

void UpdateQuota(int i, int bytes)
{
    if (bytesAvailable[i] < 0)
        return;

    if (bytesAvailable[i] > bytes)
        bytesAvailable[i] -= bytes;
    else
        bytesAvailable[i] = 0;
}

int ProcessQuotaCmd(const char* line)
{
    int direction = 0, number, pos;

    if (line[0] != '-')
        return 0;

    if (line[1] == '0')
        direction = 0;
    else if (line[1] == '1')
        direction = 1;
        else {
                fzprintf(sftpError, "Invalid data received in ReadQuotas: Unknown direction");
                cleanup_exit(1);
        }

    if (line[2] == '-') {
        bytesAvailable[direction] = -1;
        limit[direction] = -1;
        return 0;
    }

    number = 0;
    for (pos = 2;; ++pos) {
        if (line[pos] == ',')
            break;
        if (line[pos] < '0' || line[pos] > '9') {
                fzprintf(sftpError, "Invalid data received in ReadQuotas: Bytecount not a number");
                cleanup_exit(1);
        }

        number *= 10;
        number += line[pos] - '0';
    }

    ++pos;
    limit[direction] = 0;
    for (;; ++pos) {
        if (line[pos] == 0 || line[pos] == '\r' || line[pos] == '\n')
            break;
        if (line[pos] < '0' || line[pos] > '9') {
                fzprintf(sftpError, "Invalid data received in ReadQuotas: Limit not a number");
                cleanup_exit(1);
        }

        limit[direction] *= 10;
        limit[direction] += line[pos] - '0';
    }

    if (bytesAvailable[direction] == -1)
        bytesAvailable[direction] = number;
    else
        bytesAvailable[direction] += number;

    return 1;
}

char* get_input_pushback()
{
    char* pushback = input_pushback;
    input_pushback = 0;
    return pushback;
}

int has_input_pushback()
{
    if (input_pushback != 0)
        return 1;
    else
        return 0;
}

static void clear_input_buffers(int free)
{
    if (free && input_buf != NULL)
        sfree(input_buf);
    input_buf = 0;
    input_bufsize = 0;
    input_buflen = 0;
}

/* Read one '\n'-terminated line, one byte at a time, so we stop exactly at the line
 * boundary and never consume bytes belonging to the next line (which would desync any
 * other reader of stdin). Cross-platform: read(2) on Unix, ReadFile on Windows. */
char* read_input_line(int force, int* error)
{
    int ret;
    do {
        if (input_buflen >= input_bufsize) {
            input_bufsize = input_buflen + 512;
            input_buf = sresize(input_buf, input_bufsize, char);
        }
#ifdef _WINDOWS
        {
            DWORD nread = 0;
            BOOL ok = ReadFile(GetStdHandle(STD_INPUT_HANDLE),
                               input_buf + input_buflen, 1, &nread, NULL);
            ret = !ok ? -1 : (int)nread;
        }
#else
        ret = read(0, input_buf+input_buflen, 1);
        if (ret < 0)
            perror("read");
#endif
        if (ret < 0) {
            *error = 1;
            clear_input_buffers(1);
            return NULL;
        }
        if (ret == 0) {
            /* eof on stdin; no error, but no answer either */
            *error = 1;
            clear_input_buffers(1);
            return NULL;
        }

        if (input_buf[input_buflen] == '\n') {
            /* we have a full line */
            char* buf = input_buf;
            buf[input_buflen] = 0;
            clear_input_buffers(0);
            return buf;
        }
        else {
            ++input_buflen;
        }
    } while(force);

    return NULL;
}

void fz_timer_init(_fztimer *timer)
{
#ifdef _WINDOWS
    timer->dwHighDateTime = timer->dwLowDateTime = 0;
#else
    timer->low = 0;
    timer->high = 0;
#endif
}

#ifndef _WINDOWS
// 1/10th of a second in microseconds
static int const notificationDelay = 1000000 / 10;
#else
// 1/10th of a second in 100 nanoseconds
static unsigned int const notificationDelay = 10000000 / 10;
#endif


int fz_timer_check(_fztimer *timer)
{
#ifdef _WINDOWS
    SYSTEMTIME sNow;
    FILETIME fNow;
    unsigned int diff;
    GetSystemTime(&sNow);
    SystemTimeToFileTime(&sNow, &fNow);

    diff = fNow.dwHighDateTime - timer->dwHighDateTime;
    if (!diff)
    {
        if ((fNow.dwLowDateTime - timer->dwLowDateTime) < notificationDelay)
            return 0;
    }
    else if (diff == 1)
    {
        if (fNow.dwLowDateTime < timer->dwLowDateTime)
        {
            if (((0xFFFFFFFFU - timer->dwLowDateTime) + fNow.dwLowDateTime) < notificationDelay)
                return 0;
        }
    }

    *timer = fNow;
#else
#if defined(HAVE_GETTIMEOFDAY)
    struct timeval tv;
    if (!gettimeofday(&tv, 0))
    {
        if (tv.tv_sec == timer->high)
        {
            if ((tv.tv_usec - timer->low) < notificationDelay)
                return 0;
        }
        else if ((tv.tv_sec - timer->high) == 1)
        {
            if (timer->low < tv.tv_usec)
            {
                if (((1000000 - tv.tv_usec) + timer->low) < notificationDelay)
                    return 0;
            }
        }

        timer->high = tv.tv_sec;
        timer->low = tv.tv_usec;
    }

#elif defined(HAVE_FTIME)
    struct timeb tp;

    ftime(&tp);
    if (tp.time == timer->high)
    {
        if ((tp.millitm - timer->low) < (notificationDelay / 1000))
            return 0;
    }
    else if ((tp.time - timer->high) == 1)
    {
        if (timer->low < tp.millitm)
        {
            if (((1000 - tp.millitm) + timer->low) < (notificationDelay / 1000))
                return 0;
        }
    }

    timer->high = tp.time;
    timer->low = tp.millitm;
#else
#error "Neither gettimeofday nor ftime available."
#endif
#endif
    return 1;
}

int CurrentSpeedLimit(int direction)
{
    return limit[direction];
}

uintptr_t next_int(char ** s)
{
    uintptr_t ret = 0;
    while (s && *s && **s && **s != ' ') {
        ret *= 10;
        ret += **s - '0';
        ++(*s);
    }
    while (s && *s && **s && **s == ' ') {
        ++(*s);
    }
    return ret;
}
