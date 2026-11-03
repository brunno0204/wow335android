#include "CrashReport.h"
#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <initializer_list>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

namespace wow::android {
namespace {
char reportPath[512];
char crumb[256] = "start";
struct sigaction previous[32];
const int signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP};
volatile sig_atomic_t reportWritten = 0;
uintptr_t faultPc = 0, faultLr = 0;
char notes[6][200];
unsigned noteCount = 0;

void put(int fd, const char *text) {
    if (text)
        (void)!write(fd, text, std::strlen(text));
}
void putHex(int fd, uintptr_t value) {
    char text[2 + 16 + 1];
    text[0] = '0';
    text[1] = 'x';
    for (int i = 0; i < 16; ++i)
        text[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    text[18] = 0;
    put(fd, text);
}
struct Trace {
    uintptr_t pcs[48];
    int count = 0;
};
_Unwind_Reason_Code collect(_Unwind_Context *context, void *arg) {
    auto *trace = static_cast<Trace *>(arg);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc && trace->count < 48)
        trace->pcs[trace->count++] = pc;
    return trace->count < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}
void writeReport(const char *reason, int signal, void *address) {
    if (!reportPath[0])
        return;
    const int fd = open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    put(fd, "Motivo: ");
    put(fd, reason);
    if (signal) {
        char text[32];
        std::snprintf(text, sizeof(text), " (sinal %d) endereço ", signal);
        put(fd, text);
        putHex(fd, uintptr_t(address));
    }
    char thread[32] = {};
    prctl(PR_GET_NAME, thread, 0, 0, 0);
    put(fd, "\nThread: ");
    put(fd, thread);
    if (faultPc) {
        put(fd, "\nPC: ");
        Dl_info pcInfo{};
        for (uintptr_t pc : {faultPc, faultLr}) {
            if (dladdr(reinterpret_cast<void *>(pc), &pcInfo) && pcInfo.dli_fname) {
                const char *file = std::strrchr(pcInfo.dli_fname, '/');
                put(fd, file ? file + 1 : pcInfo.dli_fname);
                put(fd, " +");
                putHex(fd, pc - uintptr_t(pcInfo.dli_fbase));
                if (pcInfo.dli_sname) {
                    put(fd, " ");
                    put(fd, pcInfo.dli_sname);
                }
            } else
                putHex(fd, pc);
            put(fd, pc == faultPc ? "  LR: " : "");
        }
    }
    put(fd, "\nÚltima etapa: ");
    put(fd, crumb);
    put(fd, "\n");
    if (noteCount) {
        put(fd, "Últimas mensagens da UI:\n");
        const unsigned first = noteCount > 6 ? noteCount - 6 : 0;
        for (unsigned i = first; i < noteCount; ++i) {
            put(fd, "  ");
            put(fd, notes[i % 6]);
            put(fd, "\n");
        }
    }
    put(fd, "Pilha:\n");
    Trace trace;
    _Unwind_Backtrace(collect, &trace);
    for (int i = 0; i < trace.count; ++i) {
        Dl_info info{};
        put(fd, "  ");
        if (dladdr(reinterpret_cast<void *>(trace.pcs[i]), &info) && info.dli_fname) {
            const char *file = std::strrchr(info.dli_fname, '/');
            put(fd, file ? file + 1 : info.dli_fname);
            put(fd, " +");
            putHex(fd, trace.pcs[i] - uintptr_t(info.dli_fbase));
            if (info.dli_sname) {
                put(fd, " ");
                put(fd, info.dli_sname);
            }
        } else
            putHex(fd, trace.pcs[i]);
        put(fd, "\n");
    }
    close(fd);
}
void onSignal(int signal, siginfo_t *info, void *context) {
    static volatile sig_atomic_t busy = 0;
    if (!busy && !reportWritten) {
        busy = 1;
#if defined(__aarch64__)
        if (context) {
            const auto *uc = static_cast<const ucontext_t *>(context);
            faultPc = uintptr_t(uc->uc_mcontext.pc);
            faultLr = uintptr_t(uc->uc_mcontext.regs[30]);
        }
#endif
        writeReport("falha nativa", signal, info ? info->si_addr : nullptr);
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Fatal signal %d at %s", signal, crumb);
    }
    // Chain to the system handler (debuggerd tombstone) and die.
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        if (signals[i] == signal) {
            sigaction(signal, &previous[signal], nullptr);
            break;
        }
    (void)context;
    raise(signal);
}
void onTerminate() {
    const char *what = "std::terminate";
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &error) {
            static char text[256];
            std::snprintf(text, sizeof(text), "exceção C++ não tratada: %s", error.what());
            what = text;
        } catch (...) {
            what = "exceção C++ desconhecida";
        }
    }
    writeReport(what, 0, nullptr);
    reportWritten = 1;
    std::abort();
}
} // namespace
void installCrashReport(const char *path) {
    std::snprintf(reportPath, sizeof(reportPath), "%s", path ? path : "");
    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal : signals)
        sigaction(signal, &action, &previous[signal]);
    std::set_terminate(onTerminate);
    crashReportThread();
}
void crashReportThread() {
    static thread_local bool installed = false;
    if (installed)
        return;
    installed = true;
    stack_t stack{};
    stack.ss_size = 128 * 1024;
    stack.ss_sp = std::malloc(stack.ss_size);
    if (stack.ss_sp)
        sigaltstack(&stack, nullptr);
}
void crashNote(const char *text) {
    std::snprintf(notes[noteCount % 6], sizeof(notes[0]), "%s", text ? text : "");
    ++noteCount;
}
void crashBreadcrumb(const char *text) {
    std::snprintf(crumb, sizeof(crumb), "%s", text ? text : "");
}
void crashBreadcrumb2(const char *prefix, const char *text) {
    std::snprintf(crumb, sizeof(crumb), "%s%s", prefix ? prefix : "", text ? text : "");
}
} // namespace wow::android
