/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Disque nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "disque.h"
#include "sha1.h"   /* SHA1 is used for DEBUG DIGEST */
#include "crc64.h"
#include "job.h"
#include "queue.h"

#include <arpa/inet.h>
#include <signal.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#include <fcntl.h>
#include "bio.h"
#endif /* HAVE_BACKTRACE */

#ifdef __CYGWIN__
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif
#endif

/* ================================= Debugging ============================== */

void debugCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr,"segfault")) {
        *((char*)-1) = 'x';
    } else if (!strcasecmp(c->argv[1]->ptr,"oom")) {
        void *ptr = zmalloc(ULONG_MAX); /* Should trigger an out of memory. */
        zfree(ptr);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"assert")) {
        if (c->argc >= 3) c->argv[2] = tryObjectEncoding(c->argv[2]);
        serverAssertWithInfo(c,c->argv[0],1 == 2);
    } else if (!strcasecmp(c->argv[1]->ptr,"flushall")) {
        flushServerData();
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"loadaof")) {
        flushServerData();
        if (loadAppendOnlyFile(server.aof_filename) != DISQUE_OK) {
            addReply(c,shared.err);
            return;
        }
        serverLog(DISQUE_WARNING,"Append Only File loaded by DEBUG LOADAOF");
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"lockdown")) {
        /* Calling DEBUG LOCKDOWN without enable or disable will enable the
        * lockdown mode. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"enable")) {
                server.node_state = DISQUE_NODE_LOCKED;
            } else if (!strcasecmp(c->argv[2]->ptr,"disable")) {
                server.node_state = DISQUE_NODE_ACTIVE;
            } else {
                addReply(c,shared.err);
                return;
            }
        } else {
            server.node_state = DISQUE_NODE_LOCKED;
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"sleep") && c->argc == 3) {
        double dtime = strtod(c->argv[2]->ptr,NULL);
        long long utime = dtime*1000000;
        struct timespec tv;

        tv.tv_sec = utime / 1000000;
        tv.tv_nsec = (utime % 1000000) * 1000;
        nanosleep(&tv, NULL);
        addReply(c,shared.ok);
    } else if (!strcasecmp(c->argv[1]->ptr,"error") && c->argc == 3) {
        sds errstr = sdsnewlen("-",1);

        errstr = sdscatsds(errstr,c->argv[2]->ptr);
        errstr = sdsmapchars(errstr,"\n\r","  ",2); /* no newlines in errors. */
        errstr = sdscatlen(errstr,"\r\n",2);
        addReplySds(c,errstr);
    } else if (!strcasecmp(c->argv[1]->ptr,"structsize") && c->argc == 2) {
        sds sizes = sdsempty();
        sizes = sdscatprintf(sizes,"bits:%d ", (sizeof(void*) == 8)?64:32);
        sizes = sdscatprintf(sizes,"job:%d ", (int)sizeof(job));
        sizes = sdscatprintf(sizes,"queue:%d ", (int)sizeof(queue));
        sizes = sdscatprintf(sizes,"sdshdr:%d", (int)sizeof(struct sdshdr));
        addReplyBulkSds(c,sizes);
    } else {
        addReplyErrorFormat(c, "Unknown DEBUG subcommand or wrong number of arguments for '%s'",
            (char*)c->argv[1]->ptr);
    }
}

/* =========================== Crash handling  ============================== */

void _serverAssert(char *estr, char *file, int line) {
    bugReportStart();
    serverLog(DISQUE_WARNING,"=== ASSERTION FAILED ===");
    serverLog(DISQUE_WARNING,"==> %s:%d '%s' is not true",file,line,estr);
#ifdef HAVE_BACKTRACE
    server.assert_failed = estr;
    server.assert_file = file;
    server.assert_line = line;
    serverLog(DISQUE_WARNING,"(forcing SIGSEGV to print the bug report.)");
#endif
    *((char*)-1) = 'x';
}

void _serverAssertPrintClientInfo(client *c) {
    int j;

    bugReportStart();
    serverLog(DISQUE_WARNING,"=== ASSERTION FAILED CLIENT CONTEXT ===");
    serverLog(DISQUE_WARNING,"client->flags = %d", c->flags);
    serverLog(DISQUE_WARNING,"client->fd = %d", c->fd);
    serverLog(DISQUE_WARNING,"client->argc = %d", c->argc);
    for (j=0; j < c->argc; j++) {
        char buf[128];
        char *arg;

        if (c->argv[j]->type == DISQUE_STRING && sdsEncodedObject(c->argv[j])) {
            arg = (char*) c->argv[j]->ptr;
        } else {
            snprintf(buf,sizeof(buf),"Object type: %d, encoding: %d",
                c->argv[j]->type, c->argv[j]->encoding);
            arg = buf;
        }
        serverLog(DISQUE_WARNING,"client->argv[%d] = \"%s\" (refcount: %d)",
            j, arg, c->argv[j]->refcount);
    }
}

void _serverAssertPrintObject(robj *o) {
    bugReportStart();
    serverLog(DISQUE_WARNING,"=== ASSERTION FAILED OBJECT CONTEXT ===");
    serverLogObjectDebugInfo(o);
}

void serverLogObjectDebugInfo(robj *o) {
    serverLog(DISQUE_WARNING,"Object type: %d", o->type);
    serverLog(DISQUE_WARNING,"Object encoding: %d", o->encoding);
    serverLog(DISQUE_WARNING,"Object refcount: %d", o->refcount);
    if (o->type == DISQUE_STRING && sdsEncodedObject(o)) {
        serverLog(DISQUE_WARNING,"Object raw string len: %zu", sdslen(o->ptr));
        if (sdslen(o->ptr) < 4096) {
            sds repr = sdscatrepr(sdsempty(),o->ptr,sdslen(o->ptr));
            serverLog(DISQUE_WARNING,"Object raw string content: %s", repr);
            sdsfree(repr);
        }
    }
}

void _serverAssertWithInfo(client *c, robj *o, char *estr, char *file, int line) {
    if (c) _serverAssertPrintClientInfo(c);
    if (o) _serverAssertPrintObject(o);
    _serverAssert(estr,file,line);
}

void _serverPanic(char *msg, char *file, int line) {
    bugReportStart();
    serverLog(DISQUE_WARNING,"------------------------------------------------");
    serverLog(DISQUE_WARNING,"!!! Software Failure. Press left mouse button to continue");
    serverLog(DISQUE_WARNING,"Guru Meditation: %s #%s:%d",msg,file,line);
#ifdef HAVE_BACKTRACE
    serverLog(DISQUE_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
#endif
    serverLog(DISQUE_WARNING,"------------------------------------------------");
    *((char*)-1) = 'x';
}

void bugReportStart(void) {
    if (server.bug_report_start == 0) {
        serverLog(DISQUE_WARNING,
            "\n\n=== DISQUE BUG REPORT START: Cut & paste starting from here ===");
        server.bug_report_start = 1;
    }
}

#ifdef HAVE_BACKTRACE
static void *getMcontextEip(ucontext_t *uc) {
#if defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
    /* OSX < 10.6 */
    #if defined(__x86_64__)
    return (void*) uc->uc_mcontext->__ss.__rip;
    #elif defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__eip;
    #else
    return (void*) uc->uc_mcontext->__ss.__srr0;
    #endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
    /* OSX >= 10.6 */
    #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
    #else
    return (void*) uc->uc_mcontext->__ss.__eip;
    #endif
#elif defined(__linux__)
    /* Linux */
    #if defined(__i386__)
    return (void*) uc->uc_mcontext.gregs[14]; /* Linux 32 */
    #elif defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[16]; /* Linux 64 */
    #elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
    #endif
#else
    return NULL;
#endif
}

void logStackContent(void **sp) {
    int i;
    for (i = 15; i >= 0; i--) {
        unsigned long addr = (unsigned long) sp+i;
        unsigned long val = (unsigned long) sp[i];

        if (sizeof(long) == 4)
            serverLog(DISQUE_WARNING, "(%08lx) -> %08lx", addr, val);
        else
            serverLog(DISQUE_WARNING, "(%016lx) -> %016lx", addr, val);
    }
}

void logRegisters(ucontext_t *uc) {
    serverLog(DISQUE_WARNING, "--- REGISTERS");

/* OSX */
#if defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  /* OSX AMD64 */
    #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    serverLog(DISQUE_WARNING,
    "\n"
    "RAX:%016lx RBX:%016lx\nRCX:%016lx RDX:%016lx\n"
    "RDI:%016lx RSI:%016lx\nRBP:%016lx RSP:%016lx\n"
    "R8 :%016lx R9 :%016lx\nR10:%016lx R11:%016lx\n"
    "R12:%016lx R13:%016lx\nR14:%016lx R15:%016lx\n"
    "RIP:%016lx EFL:%016lx\nCS :%016lx FS:%016lx  GS:%016lx",
        (unsigned long) uc->uc_mcontext->__ss.__rax,
        (unsigned long) uc->uc_mcontext->__ss.__rbx,
        (unsigned long) uc->uc_mcontext->__ss.__rcx,
        (unsigned long) uc->uc_mcontext->__ss.__rdx,
        (unsigned long) uc->uc_mcontext->__ss.__rdi,
        (unsigned long) uc->uc_mcontext->__ss.__rsi,
        (unsigned long) uc->uc_mcontext->__ss.__rbp,
        (unsigned long) uc->uc_mcontext->__ss.__rsp,
        (unsigned long) uc->uc_mcontext->__ss.__r8,
        (unsigned long) uc->uc_mcontext->__ss.__r9,
        (unsigned long) uc->uc_mcontext->__ss.__r10,
        (unsigned long) uc->uc_mcontext->__ss.__r11,
        (unsigned long) uc->uc_mcontext->__ss.__r12,
        (unsigned long) uc->uc_mcontext->__ss.__r13,
        (unsigned long) uc->uc_mcontext->__ss.__r14,
        (unsigned long) uc->uc_mcontext->__ss.__r15,
        (unsigned long) uc->uc_mcontext->__ss.__rip,
        (unsigned long) uc->uc_mcontext->__ss.__rflags,
        (unsigned long) uc->uc_mcontext->__ss.__cs,
        (unsigned long) uc->uc_mcontext->__ss.__fs,
        (unsigned long) uc->uc_mcontext->__ss.__gs
    );
    logStackContent((void**)uc->uc_mcontext->__ss.__rsp);
    #else
    /* OSX x86 */
    serverLog(DISQUE_WARNING,
    "\n"
    "EAX:%08lx EBX:%08lx ECX:%08lx EDX:%08lx\n"
    "EDI:%08lx ESI:%08lx EBP:%08lx ESP:%08lx\n"
    "SS:%08lx  EFL:%08lx EIP:%08lx CS :%08lx\n"
    "DS:%08lx  ES:%08lx  FS :%08lx GS :%08lx",
        (unsigned long) uc->uc_mcontext->__ss.__eax,
        (unsigned long) uc->uc_mcontext->__ss.__ebx,
        (unsigned long) uc->uc_mcontext->__ss.__ecx,
        (unsigned long) uc->uc_mcontext->__ss.__edx,
        (unsigned long) uc->uc_mcontext->__ss.__edi,
        (unsigned long) uc->uc_mcontext->__ss.__esi,
        (unsigned long) uc->uc_mcontext->__ss.__ebp,
        (unsigned long) uc->uc_mcontext->__ss.__esp,
        (unsigned long) uc->uc_mcontext->__ss.__ss,
        (unsigned long) uc->uc_mcontext->__ss.__eflags,
        (unsigned long) uc->uc_mcontext->__ss.__eip,
        (unsigned long) uc->uc_mcontext->__ss.__cs,
        (unsigned long) uc->uc_mcontext->__ss.__ds,
        (unsigned long) uc->uc_mcontext->__ss.__es,
        (unsigned long) uc->uc_mcontext->__ss.__fs,
        (unsigned long) uc->uc_mcontext->__ss.__gs
    );
    logStackContent((void**)uc->uc_mcontext->__ss.__esp);
    #endif
/* Linux */
#elif defined(__linux__)
    /* Linux x86 */
    #if defined(__i386__)
    serverLog(DISQUE_WARNING,
    "\n"
    "EAX:%08lx EBX:%08lx ECX:%08lx EDX:%08lx\n"
    "EDI:%08lx ESI:%08lx EBP:%08lx ESP:%08lx\n"
    "SS :%08lx EFL:%08lx EIP:%08lx CS:%08lx\n"
    "DS :%08lx ES :%08lx FS :%08lx GS:%08lx",
        (unsigned long) uc->uc_mcontext.gregs[11],
        (unsigned long) uc->uc_mcontext.gregs[8],
        (unsigned long) uc->uc_mcontext.gregs[10],
        (unsigned long) uc->uc_mcontext.gregs[9],
        (unsigned long) uc->uc_mcontext.gregs[4],
        (unsigned long) uc->uc_mcontext.gregs[5],
        (unsigned long) uc->uc_mcontext.gregs[6],
        (unsigned long) uc->uc_mcontext.gregs[7],
        (unsigned long) uc->uc_mcontext.gregs[18],
        (unsigned long) uc->uc_mcontext.gregs[17],
        (unsigned long) uc->uc_mcontext.gregs[14],
        (unsigned long) uc->uc_mcontext.gregs[15],
        (unsigned long) uc->uc_mcontext.gregs[3],
        (unsigned long) uc->uc_mcontext.gregs[2],
        (unsigned long) uc->uc_mcontext.gregs[1],
        (unsigned long) uc->uc_mcontext.gregs[0]
    );
    logStackContent((void**)uc->uc_mcontext.gregs[7]);
    #elif defined(__X86_64__) || defined(__x86_64__)
    /* Linux AMD64 */
    serverLog(DISQUE_WARNING,
    "\n"
    "RAX:%016lx RBX:%016lx\nRCX:%016lx RDX:%016lx\n"
    "RDI:%016lx RSI:%016lx\nRBP:%016lx RSP:%016lx\n"
    "R8 :%016lx R9 :%016lx\nR10:%016lx R11:%016lx\n"
    "R12:%016lx R13:%016lx\nR14:%016lx R15:%016lx\n"
    "RIP:%016lx EFL:%016lx\nCSGSFS:%016lx",
        (unsigned long) uc->uc_mcontext.gregs[13],
        (unsigned long) uc->uc_mcontext.gregs[11],
        (unsigned long) uc->uc_mcontext.gregs[14],
        (unsigned long) uc->uc_mcontext.gregs[12],
        (unsigned long) uc->uc_mcontext.gregs[8],
        (unsigned long) uc->uc_mcontext.gregs[9],
        (unsigned long) uc->uc_mcontext.gregs[10],
        (unsigned long) uc->uc_mcontext.gregs[15],
        (unsigned long) uc->uc_mcontext.gregs[0],
        (unsigned long) uc->uc_mcontext.gregs[1],
        (unsigned long) uc->uc_mcontext.gregs[2],
        (unsigned long) uc->uc_mcontext.gregs[3],
        (unsigned long) uc->uc_mcontext.gregs[4],
        (unsigned long) uc->uc_mcontext.gregs[5],
        (unsigned long) uc->uc_mcontext.gregs[6],
        (unsigned long) uc->uc_mcontext.gregs[7],
        (unsigned long) uc->uc_mcontext.gregs[16],
        (unsigned long) uc->uc_mcontext.gregs[17],
        (unsigned long) uc->uc_mcontext.gregs[18]
    );
    logStackContent((void**)uc->uc_mcontext.gregs[15]);
    #endif
#else
    serverLog(DISQUE_WARNING,
        "  Dumping of registers not supported for this OS/arch");
#endif
}

/* Logs the stack trace using the backtrace() call. This function is designed
 * to be called from signal handlers safely. */
void logStackTrace(ucontext_t *uc) {
    void *trace[100];
    int trace_size = 0, fd;
    int log_to_stdout = server.logfile[0] == '\0';

    /* Open the log file in append mode. */
    fd = log_to_stdout ?
        STDOUT_FILENO :
        open(server.logfile, O_APPEND|O_CREAT|O_WRONLY, 0644);
    if (fd == -1) return;

    /* Generate the stack trace */
    trace_size = backtrace(trace, 100);

    /* overwrite sigaction with caller's address */
    if (getMcontextEip(uc) != NULL)
        trace[1] = getMcontextEip(uc);

    /* Write symbols to log file */
    backtrace_symbols_fd(trace, trace_size, fd);

    /* Cleanup */
    if (!log_to_stdout) close(fd);
}

/* Log information about the "current" client, that is, the client that is
 * currently being served by Disque. May be NULL if Disque is not serving a
 * client right now. */
void logCurrentClient(void) {
    if (server.current_client == NULL) return;

    client *cc = server.current_client;
    sds client;
    int j;

    serverLog(DISQUE_WARNING, "--- CURRENT CLIENT INFO");
    client = catClientInfoString(sdsempty(),cc);
    serverLog(DISQUE_WARNING,"client: %s", client);
    sdsfree(client);
    for (j = 0; j < cc->argc; j++) {
        robj *decoded;

        decoded = getDecodedObject(cc->argv[j]);
        serverLog(DISQUE_WARNING,"argv[%d]: '%s'", j, (char*)decoded->ptr);
        decrRefCount(decoded);
    }
}

#if defined(HAVE_PROC_MAPS)
void memtest_non_destructive_invert(void *addr, size_t size);
void memtest_non_destructive_swap(void *addr, size_t size);
#define MEMTEST_MAX_REGIONS 128

int memtest_test_linux_anonymous_maps(void) {
    FILE *fp = fopen("/proc/self/maps","r");
    char line[1024];
    size_t start_addr, end_addr, size;
    size_t start_vect[MEMTEST_MAX_REGIONS];
    size_t size_vect[MEMTEST_MAX_REGIONS];
    int regions = 0, j;
    uint64_t crc1 = 0, crc2 = 0, crc3 = 0;

    while(fgets(line,sizeof(line),fp) != NULL) {
        char *start, *end, *p = line;

        start = p;
        p = strchr(p,'-');
        if (!p) continue;
        *p++ = '\0';
        end = p;
        p = strchr(p,' ');
        if (!p) continue;
        *p++ = '\0';
        if (strstr(p,"stack") ||
            strstr(p,"vdso") ||
            strstr(p,"vsyscall")) continue;
        if (!strstr(p,"00:00")) continue;
        if (!strstr(p,"rw")) continue;

        start_addr = strtoul(start,NULL,16);
        end_addr = strtoul(end,NULL,16);
        size = end_addr-start_addr;

        start_vect[regions] = start_addr;
        size_vect[regions] = size;
        printf("Testing %lx %lu\n", (unsigned long) start_vect[regions],
                                    (unsigned long) size_vect[regions]);
        regions++;
    }

    /* Test all the regions as an unique sequential region.
     * 1) Take the CRC64 of the memory region. */
    for (j = 0; j < regions; j++) {
        crc1 = crc64(crc1,(void*)start_vect[j],size_vect[j]);
    }

    /* 2) Invert bits, swap adjacent words, swap again, invert bits.
     * This is the error amplification step. */
    for (j = 0; j < regions; j++)
        memtest_non_destructive_invert((void*)start_vect[j],size_vect[j]);
    for (j = 0; j < regions; j++)
        memtest_non_destructive_swap((void*)start_vect[j],size_vect[j]);
    for (j = 0; j < regions; j++)
        memtest_non_destructive_swap((void*)start_vect[j],size_vect[j]);
    for (j = 0; j < regions; j++)
        memtest_non_destructive_invert((void*)start_vect[j],size_vect[j]);

    /* 3) Take the CRC64 sum again. */
    for (j = 0; j < regions; j++)
        crc2 = crc64(crc2,(void*)start_vect[j],size_vect[j]);

    /* 4) Swap + Swap again */
    for (j = 0; j < regions; j++)
        memtest_non_destructive_swap((void*)start_vect[j],size_vect[j]);
    for (j = 0; j < regions; j++)
        memtest_non_destructive_swap((void*)start_vect[j],size_vect[j]);

    /* 5) Take the CRC64 sum again. */
    for (j = 0; j < regions; j++)
        crc3 = crc64(crc3,(void*)start_vect[j],size_vect[j]);

    /* NOTE: It is very important to close the file descriptor only now
     * because closing it before may result into unmapping of some memory
     * region that we are testing. */
    fclose(fp);

    /* If the two CRC are not the same, we trapped a memory error. */
    return crc1 != crc2 || crc2 != crc3;
}
#endif

void sigsegvHandler(int sig, siginfo_t *info, void *secret) {
    ucontext_t *uc = (ucontext_t*) secret;
    sds infostring, clients;
    struct sigaction act;
    DISQUE_NOTUSED(info);

    bugReportStart();
    serverLog(DISQUE_WARNING,
        "    Disque %s crashed by signal: %d", DISQUE_VERSION, sig);
    serverLog(DISQUE_WARNING,
        "    Failed assertion: %s (%s:%d)", server.assert_failed,
                        server.assert_file, server.assert_line);

    /* Log the stack trace */
    serverLog(DISQUE_WARNING, "--- STACK TRACE");
    logStackTrace(uc);

    /* Log INFO and CLIENT LIST */
    serverLog(DISQUE_WARNING, "--- INFO OUTPUT");
    infostring = genDisqueInfoString("all");
    infostring = sdscatprintf(infostring, "hash_init_value: %u\n",
        dictGetHashFunctionSeed());
    serverLogRaw(DISQUE_WARNING, infostring);
    serverLog(DISQUE_WARNING, "--- CLIENT LIST OUTPUT");
    clients = getAllClientsInfoString();
    serverLogRaw(DISQUE_WARNING, clients);
    sdsfree(infostring);
    sdsfree(clients);

    /* Log the current client */
    logCurrentClient();

    /* Log dump of processor registers */
    logRegisters(uc);

#if defined(HAVE_PROC_MAPS)
    /* Test memory */
    serverLog(DISQUE_WARNING, "--- FAST MEMORY TEST");
    bioKillThreads();
    if (memtest_test_linux_anonymous_maps()) {
        serverLog(DISQUE_WARNING,
            "!!! MEMORY ERROR DETECTED! Check your memory ASAP !!!");
    } else {
        serverLog(DISQUE_WARNING,
            "Fast memory test PASSED, however your memory can still be broken. Please run a memory test for several hours if possible.");
    }
#endif

    serverLog(DISQUE_WARNING,
"\n=== DISQUE BUG REPORT END. Make sure to include from START to END. ===\n\n"
"       Please report the crash by opening an issue on github:\n\n"
"           http://github.com/antirez/disque/issues\n\n"
"  Suspect RAM error? Use disque-server --test-memory to verify it.\n\n"
);
    /* free(messages); Don't call free() with possibly corrupted memory. */
    if (server.daemonize) unlink(server.pidfile);

    /* Make sure we exit with the right signal at the end. So for instance
     * the core will be dumped if enabled. */
    sigemptyset (&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction (sig, &act, NULL);
    kill(getpid(),sig);
}
#endif /* HAVE_BACKTRACE */

/* ==================== Logging functions for debugging ===================== */

void serverLogHexDump(int level, char *descr, void *value, size_t len) {
    char buf[65], *b;
    unsigned char *v = value;
    char charset[] = "0123456789abcdef";

    serverLog(level,"%s (hexdump):", descr);
    b = buf;
    while(len) {
        b[0] = charset[(*v)>>4];
        b[1] = charset[(*v)&0xf];
        b[2] = '\0';
        b += 2;
        len--;
        v++;
        if (b-buf == 64 || len == 0) {
            serverLogRaw(level|DISQUE_LOG_RAW,buf);
            b = buf;
        }
    }
    serverLogRaw(level|DISQUE_LOG_RAW,"\n");
}

/* =========================== Software Watchdog ============================ */
#include <sys/time.h>

void watchdogSignalHandler(int sig, siginfo_t *info, void *secret) {
#ifdef HAVE_BACKTRACE
    ucontext_t *uc = (ucontext_t*) secret;
#endif
    DISQUE_NOTUSED(info);
    DISQUE_NOTUSED(sig);

    serverLogFromHandler(DISQUE_WARNING,"\n--- WATCHDOG TIMER EXPIRED ---");
#ifdef HAVE_BACKTRACE
    logStackTrace(uc);
#else
    serverLogFromHandler(DISQUE_WARNING,"Sorry: no support for backtrace().");
#endif
    serverLogFromHandler(DISQUE_WARNING,"--------\n");
}

/* Schedule a SIGALRM delivery after the specified period in milliseconds.
 * If a timer is already scheduled, this function will re-schedule it to the
 * specified time. If period is 0 the current timer is disabled. */
void watchdogScheduleSignal(int period) {
    struct itimerval it;

    /* Will stop the timer if period is 0. */
    it.it_value.tv_sec = period/1000;
    it.it_value.tv_usec = (period%1000)*1000;
    /* Don't automatically restart. */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, NULL);
}

/* Enable the software watchdog with the specified period in milliseconds. */
void enableWatchdog(int period) {
    int min_period;

    if (server.watchdog_period == 0) {
        struct sigaction act;

        /* Watchdog was actually disabled, so we have to setup the signal
         * handler. */
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_ONSTACK | SA_SIGINFO;
        act.sa_sigaction = watchdogSignalHandler;
        sigaction(SIGALRM, &act, NULL);
    }
    /* If the configured period is smaller than twice the timer period, it is
     * too short for the software watchdog to work reliably. Fix it now
     * if needed. */
    min_period = (1000/server.hz)*2;
    if (period < min_period) period = min_period;
    watchdogScheduleSignal(period); /* Adjust the current timer. */
    server.watchdog_period = period;
}

/* Disable the software watchdog. */
void disableWatchdog(void) {
    struct sigaction act;
    if (server.watchdog_period == 0) return; /* Already disabled. */
    watchdogScheduleSignal(0); /* Stop the current timer. */

    /* Set the signal handler to SIG_IGN, this will also remove pending
     * signals from the queue. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = SIG_IGN;
    sigaction(SIGALRM, &act, NULL);
    server.watchdog_period = 0;
}
