/*
 * SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
 * Copyright (C) 2026 Gordon Heydon
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * test_main.c -- native unit tests for the parts of SerialTCPd that are pure
 * logic: the FIFO, the telnet codec and the AT command parser.
 *
 * These compile on the build host against the stub Amiga headers in
 * tests/fake, so the trickiest code can actually be run and checked rather
 * than only cross-compiled and hoped for.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "daemon.h"

/* ------------------------------------------------------------------ */
/* Stubs                                                               */
/* ------------------------------------------------------------------ */

struct Config  g_Config;
struct STNode *g_Nodes;
struct Listener g_Listeners[MAX_LISTENERS];
LONG           g_NumListeners;
BOOL           g_Quit;
ULONG          g_TotalCalls, g_BusyCalls;
struct timeval g_StartTime;

/* What the stubs recorded, for assertions. */
static UBYTE  cap_raw[4096];  static ULONG cap_raw_len;   /* node_raw_out */
static UBYTE  cap_app[4096];  static ULONG cap_app_len;   /* node_to_app  */
static char   cap_dial[256];
static int    cap_answer, cap_hangup;
static LONG   fake_elapsed_ms = 100000;                   /* "long ago"   */

static void cap_reset(void)
{
    cap_raw_len = cap_app_len = 0;
    cap_dial[0] = '\0';
    cap_answer = cap_hangup = 0;
}

ULONG node_raw_out(struct STNode *n, const UBYTE *d, ULONG len)
{
    if (cap_raw_len + len > sizeof(cap_raw)) len = sizeof(cap_raw) - cap_raw_len;
    memcpy(cap_raw + cap_raw_len, d, len);
    cap_raw_len += len;
    return len;
}

void node_to_app(struct STNode *n, const UBYTE *d, ULONG len)
{
    if (cap_app_len + len > sizeof(cap_app)) len = sizeof(cap_app) - cap_app_len;
    memcpy(cap_app + cap_app_len, d, len);
    cap_app_len += len;
}

ULONG node_app_out(struct STNode *n, const UBYTE *d, ULONG len) { return len; }

/*
 * node.c normally supplies these: the codec stages its negotiation replies and
 * these wrappers flush them at the socket. The test provides the same halves,
 * so what is exercised here is exactly what runs on the Amiga.
 */
static void telnet_flush(struct STNode *n)
{
    UBYTE resp[SUBNEG_SIZE];
    ULONG len = telnet_take_responses(&n->n_Tel, resp, sizeof(resp));
    if (len)
        node_raw_out(n, resp, len);
}

void telnet_start(struct STNode *n)
{
    telnet_start_t(&n->n_Tel);
    telnet_flush(n);
}

ULONG telnet_decode(struct STNode *n, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax)
{
    ULONG got = telnet_decode_t(&n->n_Tel, in, inlen, out, outmax);
    telnet_flush(n);
    return got;
}

ULONG telnet_encode(struct STNode *n, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax)
{
    return telnet_encode_t(&n->n_Tel, in, inlen, out, outmax);
}
void  node_reply_text(struct STNode *n, const char *s) { node_to_app(n, (const UBYTE *)s, strlen(s)); }
void  node_answer(struct STNode *n)  { cap_answer++; }
void  node_hangup(struct STNode *n, BOOL nc) { cap_hangup++; }
void  node_dial(struct STNode *n, const char *t) { strncpy(cap_dial, t, sizeof(cap_dial) - 1); }
void  node_set_status(struct STNode *n, UWORD s) { if (n->n_Unit) n->n_Unit->su_Status = s; }
void  log_printf(const char *fmt, ...) { (void)fmt; }
void  st_gettime(struct timeval *tv) { tv->tv_secs = 0; tv->tv_micro = 0; }
LONG  st_elapsed_ms(const struct timeval *since) { (void)since; return fake_elapsed_ms; }

void node_result(struct STNode *n, UWORD code, ULONG baud)
{
    static const char *names[] = { "OK","CONNECT","RING","NO CARRIER","ERROR","?","NO DIALTONE","BUSY","NO ANSWER" };
    char buf[64];

    if (n->n_Quiet) return;

    if (n->n_Verbose)
    {
        if (code == RC_CONNECT && baud) snprintf(buf, sizeof(buf), "\r\nCONNECT %lu\r\n", (unsigned long)baud);
        else snprintf(buf, sizeof(buf), "\r\n%s\r\n", code < 9 ? names[code] : "?");
    }
    else snprintf(buf, sizeof(buf), "%u\r", (unsigned)code);

    node_reply_text(n, buf);
}

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                    */
/* ------------------------------------------------------------------ */

static int tests = 0, failures = 0;

#define CHECK(cond, ...) do {                            \
    tests++;                                             \
    if (!(cond)) {                                       \
        failures++;                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);    \
        printf(__VA_ARGS__);                             \
        printf("\n");                                    \
    }                                                    \
} while (0)

static BOOL app_contains(const char *needle)
{
    ULONG nlen = strlen(needle);
    ULONG i;
    if (nlen > cap_app_len) return FALSE;
    for (i = 0; i + nlen <= cap_app_len; i++)
        if (!memcmp(cap_app + i, needle, nlen)) return TRUE;
    return FALSE;
}

static BOOL raw_contains(const UBYTE *needle, ULONG nlen)
{
    ULONG i;
    if (nlen > cap_raw_len) return FALSE;
    for (i = 0; i + nlen <= cap_raw_len; i++)
        if (!memcmp(cap_raw + i, needle, nlen)) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* FIFO                                                                */
/* ------------------------------------------------------------------ */

static void test_fifo(void)
{
    UBYTE        buf[16];
    UBYTE        out[32];
    struct Fifo  f;
    ULONG        n;

    printf("FIFO\n");

    fifo_init(&f, buf, sizeof(buf));
    CHECK(fifo_count(&f) == 0, "fresh fifo not empty");
    CHECK(fifo_space(&f) == 16, "fresh fifo space wrong: %lu", (unsigned long)fifo_space(&f));

    n = fifo_put(&f, (const UBYTE *)"hello", 5);
    CHECK(n == 5, "put returned %lu", (unsigned long)n);
    CHECK(fifo_count(&f) == 5, "count after put");

    n = fifo_get(&f, out, 5);
    CHECK(n == 5 && !memcmp(out, "hello", 5), "round trip failed");
    CHECK(fifo_count(&f) == 0, "not drained");

    /* Overfill must be clamped, never overrun. */
    n = fifo_put(&f, (const UBYTE *)"0123456789abcdefGHIJ", 20);
    CHECK(n == 16, "overfill accepted %lu, expected 16", (unsigned long)n);
    CHECK(fifo_space(&f) == 0, "should be full");

    /* Wraparound: drain half, refill past the end of the buffer. */
    fifo_get(&f, out, 10);
    CHECK(fifo_count(&f) == 6, "count after partial get");
    n = fifo_put(&f, (const UBYTE *)"WXYZ", 4);
    CHECK(n == 4, "wrap put accepted %lu", (unsigned long)n);

    n = fifo_get(&f, out, 32);
    CHECK(n == 10, "wrapped get returned %lu, expected 10", (unsigned long)n);
    CHECK(!memcmp(out, "abcdefWXYZ", 10), "wrapped data corrupted: '%.10s'", out);

    /* peek must read without consuming, and refuse out-of-range. */
    fifo_clear(&f);
    fifo_put(&f, (const UBYTE *)"AB", 2);
    CHECK(fifo_peek(&f, 0) == 'A', "peek 0");
    CHECK(fifo_peek(&f, 1) == 'B', "peek 1");
    CHECK(fifo_peek(&f, 2) == -1, "peek past end should be -1");
    CHECK(fifo_count(&f) == 2, "peek consumed data");
}

/* ------------------------------------------------------------------ */
/* Telnet                                                              */
/* ------------------------------------------------------------------ */

static void test_telnet(void)
{
    struct STNode n;
    UBYTE         out[256];
    ULONG         got;

    printf("Telnet\n");

    memset(&n, 0, sizeof(n));
    telnet_reset(&n.n_Tel, TRUE, TRUE);
    cap_reset();

    /* IAC IAC is a literal 0xFF in the data stream. */
    {
        const UBYTE in[] = { 'a', 255, 255, 'b' };
        got = telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 3, "decode IAC IAC gave %lu bytes, expected 3", (unsigned long)got);
        CHECK(out[0] == 'a' && out[1] == 255 && out[2] == 'b', "IAC IAC not unescaped");
    }

    /* DO BINARY must be answered with WILL BINARY and flip our local state. */
    cap_reset();
    {
        const UBYTE in[] = { TEL_IAC, TEL_DO, TELOPT_BINARY };
        const UBYTE want[] = { TEL_IAC, TEL_WILL, TELOPT_BINARY };
        got = telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 0, "negotiation leaked %lu data bytes", (unsigned long)got);
        CHECK(raw_contains(want, 3), "did not answer DO BINARY with WILL BINARY");
        CHECK(n.n_Tel.t_BinaryLocal, "local binary not enabled");
    }

    /* Repeating a request we have already agreed to must stay silent, or the
     * two ends ping-pong forever. */
    cap_reset();
    {
        const UBYTE in[] = { TEL_IAC, TEL_DO, TELOPT_BINARY };
        telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(cap_raw_len == 0, "re-answered an already-agreed option (%lu bytes)",
              (unsigned long)cap_raw_len);
    }

    /* An unsupported option must be refused. */
    cap_reset();
    {
        const UBYTE in[] = { TEL_IAC, TEL_DO, 99 };
        const UBYTE want[] = { TEL_IAC, TEL_WONT, 99 };
        telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(raw_contains(want, 3), "unsupported option not refused");
    }

    /* Subnegotiation must be swallowed whole, leaving the stream in sync. */
    cap_reset();
    {
        const UBYTE in[] = { 'x', TEL_IAC, TEL_SB, TELOPT_NAWS, 0, 80, 0, 24, TEL_IAC, TEL_SE, 'y' };
        got = telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 2 && out[0] == 'x' && out[1] == 'y',
              "subnegotiation not skipped cleanly (got %lu)", (unsigned long)got);
    }

    /* The decoder is a stream machine: an escape split across two reads must
     * still work, because TCP will do exactly that. */
    cap_reset();
    telnet_reset(&n.n_Tel, TRUE, TRUE);
    {
        const UBYTE part1[] = { 'a', TEL_IAC };
        const UBYTE part2[] = { TEL_DO, TELOPT_SGA };
        const UBYTE want[]  = { TEL_IAC, TEL_WILL, TELOPT_SGA };

        got = telnet_decode(&n, part1, sizeof(part1), out, sizeof(out));
        CHECK(got == 1 && out[0] == 'a', "split escape: first half wrong");

        got = telnet_decode(&n, part2, sizeof(part2), out, sizeof(out));
        CHECK(got == 0, "split escape: second half leaked data");
        CHECK(raw_contains(want, 3), "split escape not negotiated");
    }

    /* NVT mode: CR NUL from the wire means a bare CR. */
    cap_reset();
    telnet_reset(&n.n_Tel, TRUE, TRUE);
    {
        const UBYTE in[] = { 'a', '\r', 0, 'b' };
        got = telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 3 && out[0] == 'a' && out[1] == '\r' && out[2] == 'b',
              "CR NUL not collapsed (got %lu)", (unsigned long)got);
    }

    /* Encoding: 0xFF must be doubled so it is not read as IAC. */
    telnet_reset(&n.n_Tel, TRUE, TRUE);
    n.n_Tel.t_BinaryLocal = TRUE;
    {
        const UBYTE in[] = { 'a', 255, 'b' };
        got = telnet_encode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 4 && out[1] == 255 && out[2] == 255, "0xFF not doubled on output");
    }

    /* Binary mode must leave CR alone -- this is what Zmodem depends on. */
    {
        const UBYTE in[] = { '\r', 'x' };
        got = telnet_encode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 2 && out[0] == '\r' && out[1] == 'x', "binary mode altered CR");
    }

    /* Non-binary mode must pad a bare CR with NUL, but leave CR LF alone. */
    n.n_Tel.t_BinaryLocal = FALSE;
    {
        const UBYTE in[] = { '\r', 'x' };
        got = telnet_encode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 3 && out[0] == '\r' && out[1] == 0 && out[2] == 'x',
              "bare CR not padded with NUL (got %lu)", (unsigned long)got);
    }
    {
        const UBYTE in[] = { '\r', '\n' };
        got = telnet_encode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 2 && out[0] == '\r' && out[1] == '\n', "CR LF was mangled");
    }

    /* With telnet disabled the codec must be a pure passthrough. */
    telnet_reset(&n.n_Tel, FALSE, TRUE);
    {
        const UBYTE in[] = { 255, TEL_DO, TELOPT_BINARY, '\r' };
        got = telnet_decode(&n, in, sizeof(in), out, sizeof(out));
        CHECK(got == 4 && !memcmp(out, in, 4), "raw mode is not transparent");
    }
}

/* ------------------------------------------------------------------ */
/* AT commands                                                         */
/* ------------------------------------------------------------------ */

static void feed(struct STNode *n, const char *s)
{
    at_feed(n, (const UBYTE *)s, strlen(s));
}

static void test_at(void)
{
    struct STNode n;
    struct STUnit u;

    printf("AT commands\n");

    g_Config.c_AutoAnswer = 1;

    memset(&n, 0, sizeof(n));
    memset(&u, 0, sizeof(u));
    n.n_Unit = &u;
    at_reset(&n);
    n.n_Echo = FALSE;

    CHECK(n.n_SReg[0] == 1, "S0 not seeded from config");
    CHECK(n.n_SReg[12] == 50, "S12 default wrong");

    cap_reset(); feed(&n, "AT\r");
    CHECK(app_contains("OK"), "bare AT did not return OK");

    cap_reset(); feed(&n, "ATZ\r");
    CHECK(app_contains("OK"), "ATZ did not return OK");

    /*
     * A realistic BBS init string.  Every one of these letters is a real
     * command some modem implemented; the parser has to walk the whole line
     * and still return a single OK.
     */
    cap_reset(); feed(&n, "AT&F&C1&D2E0Q0V1X4S0=2S7=45\r");
    CHECK(app_contains("OK"), "modem init string rejected");
    CHECK(n.n_SReg[0] == 2, "S0 not set by init string (got %u)", n.n_SReg[0]);
    CHECK(n.n_SReg[7] == 45, "S7 not set by init string (got %u)", n.n_SReg[7]);
    CHECK(n.n_DcdMode == 1, "&C1 not applied");
    CHECK(n.n_DtrMode == 2, "&D2 not applied");
    CHECK(n.n_Echo == FALSE, "E0 not applied");

    /* Query an S register. */
    cap_reset(); feed(&n, "ATS7?\r");
    CHECK(app_contains("045"), "ATS7? did not report 045");

    /* Numeric result codes. */
    cap_reset(); feed(&n, "ATV0\r");
    CHECK(app_contains("0\r"), "ATV0 did not answer with numeric 0");
    cap_reset(); feed(&n, "ATV1\r");
    CHECK(app_contains("OK"), "ATV1 did not restore verbose results");

    /* Quiet mode suppresses everything. */
    cap_reset(); feed(&n, "ATQ1\r");
    CHECK(cap_app_len == 0, "ATQ1 still emitted a result code");
    cap_reset(); feed(&n, "ATQ0\r");
    CHECK(app_contains("OK"), "ATQ0 did not restore result codes");

    /* Genuine nonsense must still be an error. */
    cap_reset(); feed(&n, "ATJZZ\r");
    CHECK(app_contains("ERROR"), "unknown command did not return ERROR");

    /* Dial string parsing, in the forms a BBS or terminal actually sends. */
    cap_reset(); feed(&n, "ATDT bbs.example.com:2323\r");
    CHECK(!strcmp(cap_dial, "bbs.example.com:2323"), "dial target wrong: '%s'", cap_dial);

    cap_reset(); feed(&n, "ATD192.168.1.10\r");
    CHECK(!strcmp(cap_dial, "192.168.1.10"), "plain ATD target wrong: '%s'", cap_dial);

    /* A/ repeats the previous command without an AT prefix. */
    cap_reset(); feed(&n, "ATS3?\r");
    cap_reset(); feed(&n, "A/\r");
    CHECK(app_contains("013"), "A/ did not repeat the previous command");

    /* Answer. */
    cap_reset(); feed(&n, "ATA\r");
    CHECK(cap_answer == 1, "ATA did not answer");

    /* Backspace editing inside a command line. */
    cap_reset(); feed(&n, "ATXX\bS0=7\r");
    CHECK(n.n_SReg[0] == 7, "backspace editing broke the command (S0=%u)", n.n_SReg[0]);

    /* Commands split across writes must still work: the BBS writes whatever
     * size chunks it likes. */
    cap_reset();
    feed(&n, "AT");
    feed(&n, "S0=");
    feed(&n, "3\r");
    CHECK(n.n_SReg[0] == 3, "split command line failed (S0=%u)", n.n_SReg[0]);
    CHECK(app_contains("OK"), "split command line did not return OK");

    /*
     * DLG Pro's command-mode hangup writes the hangup string followed by a
     * bare "\n". A bare LF must terminate the line, but CR LF must still
     * execute exactly once.
     */
    cap_reset(); n.n_SReg[0] = 0;
    feed(&n, "ATS0=5\n");
    CHECK(n.n_SReg[0] == 5, "bare LF did not terminate the command (S0=%u)", n.n_SReg[0]);

    cap_reset(); n.n_SReg[0] = 0;
    feed(&n, "ATS0=6\r\n");
    CHECK(n.n_SReg[0] == 6, "CR LF command failed (S0=%u)", n.n_SReg[0]);
    {
        /* Exactly one OK, not two. */
        int count = 0; ULONG k;
        for (k = 0; k + 2 <= cap_app_len; k++)
            if (!memcmp(cap_app + k, "OK", 2)) count++;
        CHECK(count == 1, "CR LF produced %d result codes, expected 1", count);
    }

    /* The full DLG hangup sequence: "+++" with no CR, then "ATH0" then LF. */
    cap_reset();
    feed(&n, "+++");
    feed(&n, "ATH0");
    feed(&n, "\n");
    CHECK(cap_hangup >= 1 || app_contains("OK") || app_contains("ERROR"),
          "DLG hangup sequence produced no response at all");

    /* Echo really echoes. */
    cap_reset();
    n.n_Echo = TRUE;
    feed(&n, "AT\r");
    CHECK(app_contains("AT"), "echo did not reflect the typed command");
    n.n_Echo = FALSE;
}

/* ------------------------------------------------------------------ */
/* Queue helpers                                                       */
/* ------------------------------------------------------------------ */

static void test_queue(void)
{
    char           buf[64];
    struct timeval a, b;

    printf("Queue helpers\n");

    /* Plain substitution. */
    queue_expand(buf, sizeof(buf), "You are number %N in the queue.", 3);
    CHECK(!strcmp(buf, "You are number 3 in the queue."), "expand gave '%s'", buf);

    /* Multi-digit, and digits must not come out backwards. */
    queue_expand(buf, sizeof(buf), "pos %N", 12);
    CHECK(!strcmp(buf, "pos 12"), "two-digit expand gave '%s'", buf);
    queue_expand(buf, sizeof(buf), "pos %N", 507);
    CHECK(!strcmp(buf, "pos 507"), "three-digit expand gave '%s'", buf);

    /* Lower case accepted too. */
    queue_expand(buf, sizeof(buf), "pos %n", 7);
    CHECK(!strcmp(buf, "pos 7"), "%%n not handled: '%s'", buf);

    /* More than one placeholder. */
    queue_expand(buf, sizeof(buf), "%N of %N", 2);
    CHECK(!strcmp(buf, "2 of 2"), "repeated placeholder gave '%s'", buf);

    /* Zero. */
    queue_expand(buf, sizeof(buf), "[%N]", 0);
    CHECK(!strcmp(buf, "[0]"), "zero gave '%s'", buf);

    /* A stray %% must stay literal -- this is the whole reason we do not hand
     * config text to printf. */
    queue_expand(buf, sizeof(buf), "100%% busy %N", 1);
    CHECK(!strcmp(buf, "100%% busy 1"), "literal %% mangled: '%s'", buf);
    queue_expand(buf, sizeof(buf), "%s and %d are literal", 1);
    CHECK(!strcmp(buf, "%s and %d are literal"), "printf specifiers not left alone: '%s'", buf);

    /* Truncation must be clean and NUL-terminated, never an overrun. */
    {
        char small[8];
        memset(small, 0xAA, sizeof(small));
        queue_expand(small, sizeof(small), "abcdefghijklmnop", 1);
        CHECK(small[7] == '\0', "expand did not terminate on truncation");
        CHECK(strlen(small) == 7, "truncated to %lu, expected 7", (unsigned long)strlen(small));
    }
    {
        /* Truncation landing mid-number must still terminate. */
        char small[6];
        queue_expand(small, sizeof(small), "ab%N", 12345);
        CHECK(small[5] == '\0', "mid-number truncation did not terminate");
    }

    /* Empty and NULL sources. */
    queue_expand(buf, sizeof(buf), "", 4);
    CHECK(buf[0] == '\0', "empty template should give an empty string");
    queue_expand(buf, sizeof(buf), NULL, 4);
    CHECK(buf[0] == '\0', "NULL template should give an empty string");

    /* Ordering: seconds dominate, microseconds break ties. */
    a.tv_secs = 100; a.tv_micro = 500;
    b.tv_secs = 101; b.tv_micro = 0;
    CHECK(queue_earlier(&a, &b), "earlier seconds not ordered first");
    CHECK(!queue_earlier(&b, &a), "ordering is not antisymmetric");

    a.tv_secs = 100; a.tv_micro = 100;
    b.tv_secs = 100; b.tv_micro = 200;
    CHECK(queue_earlier(&a, &b), "microseconds ignored in tie-break");
    CHECK(!queue_earlier(&b, &a), "microsecond ordering is not antisymmetric");

    /* Identical stamps are not "earlier" either way, or two callers could
     * both think they are at the head of the queue. */
    a.tv_secs = 5; a.tv_micro = 5;
    b.tv_secs = 5; b.tv_micro = 5;
    CHECK(!queue_earlier(&a, &b) && !queue_earlier(&b, &a),
          "equal timestamps must not compare as earlier");
}

/* ------------------------------------------------------------------ */
/* Unit lists (outbound-only)                                          */
/* ------------------------------------------------------------------ */

static void test_unit_list(void)
{
    ULONG m;

    printf("Unit lists\n");

    CHECK(parse_unit_list("0", &m, 8) && m == 0x01, "single unit gave 0x%lx", (unsigned long)m);
    CHECK(parse_unit_list("3", &m, 8) && m == 0x08, "unit 3 gave 0x%lx", (unsigned long)m);

    /* Commas and spaces, mixed, because hand-written configs contain both. */
    CHECK(parse_unit_list("0,2 3", &m, 8) && m == 0x0D, "mixed separators gave 0x%lx", (unsigned long)m);
    CHECK(parse_unit_list("1, 2,3", &m, 8) && m == 0x0E, "comma-space gave 0x%lx", (unsigned long)m);
    CHECK(parse_unit_list("  2  ", &m, 8) && m == 0x04, "padded gave 0x%lx", (unsigned long)m);

    /* Empty entries are skipped, not an error. */
    CHECK(parse_unit_list("1,,2", &m, 8) && m == 0x06, "double comma gave 0x%lx", (unsigned long)m);

    /* Nothing at all parses to nothing, without complaint. */
    CHECK(parse_unit_list("", &m, 8) && m == 0, "empty list should be empty and OK");

    /* Out of range is rejected. */
    CHECK(!parse_unit_list("8", &m, 8), "unit 8 with 8 nodes should be rejected");
    CHECK(!parse_unit_list("99", &m, 8), "unit 99 should be rejected");
    CHECK(m == 0, "rejected units must not end up in the mask (0x%lx)", (unsigned long)m);

    /* Junk is rejected but must not eat the valid entries beside it, and must
     * not spin forever on the unparseable text. */
    CHECK(!parse_unit_list("1,abc,3", &m, 8), "junk should make the call fail");
    CHECK(m == 0x0A, "valid entries beside junk were lost (0x%lx)", (unsigned long)m);

    CHECK(!parse_unit_list(NULL, &m, 8), "NULL should be rejected");

    /* A node count above the bitmask width cannot be represented. */
    CHECK(!parse_unit_list("32", &m, 64), "unit 32 does not fit the mask and must be rejected");
}

/* ------------------------------------------------------------------ */
/* Queue position reporting (including the joke)                       */
/* ------------------------------------------------------------------ */

static void test_queue_lie(void)
{
    ULONG rng = 12345;
    UWORD i, p;

    printf("Queue position reporting\n");

    /* Honest mode must be exactly honest. */
    for (i = 1; i <= 10; i++)
        CHECK(queue_reported_position(i, QL_OFF, 3, 5, 100, &rng) == i,
              "QL_OFF changed position %u", i);

    /* Inflate: starts at lie-start and creeps upward with each notice. */
    CHECK(queue_reported_position(1, QL_INFLATE, 3, 0, 0, &rng) == 3, "inflate start");
    CHECK(queue_reported_position(1, QL_INFLATE, 3, 1, 0, &rng) == 4, "inflate +1");
    CHECK(queue_reported_position(1, QL_INFLATE, 3, 2, 0, &rng) == 5, "inflate +2");
    CHECK(queue_reported_position(1, QL_INFLATE, 3, 9, 0, &rng) == 12, "inflate +9");

    /* It must never claim a position better than the truth -- telling someone
     * they are number 1 while they wait is a crueller joke than intended. */
    CHECK(queue_reported_position(8, QL_INFLATE, 3, 0, 0, &rng) == 8,
          "inflate reported better than the true position");

    /* And it must stay a sane number. */
    CHECK(queue_reported_position(1, QL_INFLATE, 999, 500, 0, &rng) <= 999,
          "inflate ran away past 999");

    /* Random with 0%% chance is honest; with 100%% it always pads. */
    for (i = 0; i < 20; i++)
        CHECK(queue_reported_position(4, QL_RANDOM, 3, 0, 0, &rng) == 4,
              "QL_RANDOM at 0%% chance told a lie");

    for (i = 0; i < 40; i++)
    {
        p = queue_reported_position(4, QL_RANDOM, 3, 0, 100, &rng);
        if (p <= 4 || p > 8)
        {
            CHECK(0, "QL_RANDOM at 100%% gave %u, expected 5..8", p);
            break;
        }
    }

    /* Whatever the mode, never report position zero. */
    CHECK(queue_reported_position(0, QL_OFF, 3, 0, 0, &rng) >= 1, "QL_OFF gave 0");
    CHECK(queue_reported_position(0, QL_INFLATE, 1, 0, 0, &rng) >= 1, "QL_INFLATE gave 0");
    CHECK(queue_reported_position(0, QL_RANDOM, 3, 0, 100, &rng) >= 1, "QL_RANDOM gave 0");

    /* The generator must not get stuck, including from a zero seed. */
    {
        ULONG z = 0, a, b;
        a = st_rand(&z);
        b = st_rand(&z);
        CHECK(a != 0 && b != 0 && a != b, "st_rand stuck (%lu, %lu)",
              (unsigned long)a, (unsigned long)b);
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("SerialTCP unit tests\n\n");

    memset(&g_Config, 0, sizeof(g_Config));
    g_Config.c_Telnet = TRUE;
    g_Config.c_AutoAnswer = 1;

    test_fifo();
    test_telnet();
    test_at();
    test_queue();
    test_unit_list();
    test_queue_lie();

    printf("\n%d checks, %d failures\n", tests, failures);
    return failures ? 1 : 0;
}
