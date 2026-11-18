// ★ 11-18 M-1 멈춤 잡기 — nox_stack: 바깥에서 다른 프로세스의 모든 스레드가 어느 함수 · 어느 줄에 있는지와
//   스레드별 CPU 몫을 찍고, 원하면 전체 덤프를 남긴다(`Docs/MO_STALL_HUNT_PLAN_1118.md` 3절 ①).
//
//   왜 바깥 도구인가 — ① 서버 실행 파일을 안 바꾸니 멈췄던 판과 같은 파일로 다시 돌릴 수 있다
//   ② 다른 프로세스에서 찍으니 멈춘 스레드가 쥔 락(메모리 할당 · 출력)에 이쪽이 같이 걸리지 않는다
//   ③ 관리자 권한이 안 든다(같은 사용자의 프로세스 — 기존 표본 도구 xperf 는 관리자 창이 있어야 한다).
//
//   쓰기:
//     nox_stack --pid P [--samples 5] [--gap-ms 200] --out 파일 [--dump 파일] [--sym 경로;경로] [--game-fn 이름]
//               [--run-thr 0.5] [--idle-thr 0.05]
//     nox_stack --self-test [--out 폴더] [--sym 경로] [--run-thr X] [--idle-thr X]   — 일부러 틀리기는 이 인자로 준다
//     nox_stack --victim spin|wait|fakeserver|sleep ...   — 시험용으로 일부러 갇히는 프로세스(tools/stall_hunt.ps1 -SelfTest 도 쓴다)
//
//   종료 코드: 0 = 됨 · 1 = 인자 · 2 = 대상 프로세스를 못 엶 · 3 = 기호 초기화 실패 · 4 = 덤프 실패 · 10 = 자가 시험 틀림
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(sizeof(void*) == 8, "x64 만 — 호출 사슬을 AMD64 로 거슬러 올라간다");

namespace {

std::string U8(const wchar_t* w, int len) {
    if (!w || len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    if (len < 0 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
std::string U8(const std::wstring& w) { return U8(w.c_str(), static_cast<int>(w.size())); }

std::string Fmt(const char* f, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string BaseName(const std::string& p) {
    const size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? p : p.substr(k + 1);
}

uint64_t FtU64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

double NowSec() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

// ── 찍기 ─────────────────────────────────────────────────────────
struct Frame {
    uint64_t pc = 0;
    DWORD inlineCtx = 0;
    bool inl = false;    // 녹은 함수 — 부른 쪽 몸통 안에 펼쳐져 따로 프레임이 없는 함수(Release /O2)
    bool ours = false;   // 대상 실행 파일 모듈 안(시스템 DLL 이 아님)
    std::string mod, fn, file;
    DWORD line = 0;
    uint64_t disp = 0;
};
struct Sample {
    bool ok = false;
    DWORD err = 0;
    std::vector<Frame> frames;
};
struct Thread {
    DWORD tid = 0;
    HANDLE h = nullptr;
    uint64_t created = 0, cpu0 = 0, cpu1 = 0;
    double ratio = -1;
    const char* verdict = "못 잼";
    std::string name;
    std::vector<Sample> samples;
    bool hasGameFn = false;
};
struct Opts {
    DWORD pid = 0;
    int samples = 5;
    int gapMs = 200;
    std::wstring out, dump, sym;
    std::string gameFn = "CGameThread::Run";
    // 돈다 · 서 있다 문턱 — 첫 표본과 끝 표본 사이 CPU 시간 ÷ 지난 시간. 고리에 갇히면 1 가까이 · 기다리면 0 가까이라
    //   둘 사이를 넓게 비워 둔다(GetThreadTimes 눈금 약 15.6ms ÷ 0.8초 창 = 약 2% 흔들림)
    double runThr = 0.5, idleThr = 0.05;
};
struct Result {
    int rc = 0;
    std::string error, header, symLine, summary;
    std::vector<Thread> threads;
    int game = -1;
    std::string gameWhy;
    double wallSec = 0;
    DWORD dumpMs = 0;
    uint64_t dumpBytes = 0;
};

const char* Verdict(double r, const Opts& o) {
    if (r < 0) return "못 잼";
    if (r >= o.runThr) return "돈다";
    if (r <= o.idleThr) return "서 있다";
    return "섞임";
}

// 스레드 하나를 잠깐 세워 레지스터를 읽고 호출 사슬을 거슬러 올라간 뒤 바로 다시 돌린다. 이름 풀기는 다시 돌린 뒤에 한다(세운 시간을 줄이려고)
void Walk(HANDLE proc, Thread& t, Sample& s) {
    if (SuspendThread(t.h) == static_cast<DWORD>(-1)) { s.err = GetLastError(); return; }
    alignas(16) CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(t.h, &ctx)) {   // 세우기가 실제로 끝날 때까지 여기서 기다린다
        s.err = GetLastError();
        ResumeThread(t.h);
        return;
    }
    STACKFRAME_EX f{};
    f.StackFrameSize = sizeof(f);
    f.AddrPC.Offset = ctx.Rip;    f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    f.InlineFrameContext = INLINE_FRAME_CONTEXT_INIT;
    for (int i = 0; i < 128; ++i) {
        if (!StackWalkEx(IMAGE_FILE_MACHINE_AMD64, proc, t.h, &f, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr, SYM_STKWALK_DEFAULT))
            break;
        if (f.AddrPC.Offset == 0) break;
        Frame fr;
        fr.pc = f.AddrPC.Offset;
        fr.inlineCtx = f.InlineFrameContext;
        fr.inl = ((f.InlineFrameContext >> 8) & 0xFF & 0x02) != 0;   // 프레임 종류 바이트 0x02 = 녹은 함수(DbgEng.h STACK_FRAME_TYPE_INLINE)
        s.frames.push_back(std::move(fr));
    }
    ResumeThread(t.h);
    s.ok = true;
}

void Resolve(HANDLE proc, uint64_t exeBase, Sample& s) {
    if (s.frames.empty()) return;
    const uint64_t topPc = s.frames[0].pc;
    for (Frame& fr : s.frames) {
        // 맨 위(지금 도는 자리)가 아닌 프레임의 pc 는 돌아갈 자리 — 한 바이트 앞이 부른 줄이다
        const uint64_t q = (fr.pc == topPc) ? fr.pc : fr.pc - 1;
        fr.ours = exeBase != 0 && SymGetModuleBase64(proc, fr.pc) == exeBase;
        IMAGEHLP_MODULEW64 mi{};
        mi.SizeOfStruct = sizeof(mi);
        if (SymGetModuleInfoW64(proc, fr.pc, &mi)) fr.mod = U8(mi.ModuleName, -1);
        alignas(SYMBOL_INFOW) unsigned char buf[sizeof(SYMBOL_INFOW) + 1024 * sizeof(wchar_t)] = {};
        auto* si = reinterpret_cast<SYMBOL_INFOW*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFOW);
        si->MaxNameLen = 1024;
        DWORD64 disp = 0;
        if (SymFromInlineContextW(proc, q, fr.inlineCtx, &disp, si) || SymFromAddrW(proc, q, &disp, si)) {
            fr.fn = U8(si->Name, static_cast<int>(si->NameLen));
            fr.disp = disp + (q != fr.pc ? 1 : 0);
        }
        IMAGEHLP_LINEW64 ln{};
        ln.SizeOfStruct = sizeof(ln);
        DWORD d32 = 0;
        if (SymGetLineFromInlineContextW(proc, q, fr.inlineCtx, 0, &d32, &ln) || SymGetLineFromAddrW64(proc, q, &d32, &ln)) {
            fr.file = U8(ln.FileName, -1);
            fr.line = ln.LineNumber;
        }
    }
}

std::string FrameText(const Frame& f, bool fullPath) {
    std::string s;
    if (!f.ours) s += (f.mod.empty() ? "?" : f.mod) + "!";
    if (!f.fn.empty()) s += f.fn + Fmt("+0x%llx", static_cast<unsigned long long>(f.disp));
    else s += Fmt("0x%llx", static_cast<unsigned long long>(f.pc));
    if (!f.file.empty()) s += Fmt(" (%s:%lu)", (fullPath ? f.file : BaseName(f.file)).c_str(), f.line);
    if (f.inl) s += " [녹은 함수]";
    return s;
}

// 표본마다 맨 위 프레임(oursOnly 면 대상 실행 파일 안 첫 프레임) — 같은 함수 · 줄끼리 세어 처음 나온 차례로
std::string TopList(const Thread& t, bool oursOnly) {
    std::vector<std::pair<std::string, int>> v;
    for (const Sample& s : t.samples) {
        std::string key = s.ok ? "(사슬 없음)" : Fmt("(못 찍음 %lu)", s.err);
        for (const Frame& f : s.frames) {
            if (oursOnly && !f.ours) continue;
            key = (f.ours ? "" : (f.mod.empty() ? "?" : f.mod) + "!") + (f.fn.empty() ? Fmt("0x%llx", static_cast<unsigned long long>(f.pc)) : f.fn);
            if (!f.file.empty()) key += Fmt(" (%s:%lu)", BaseName(f.file).c_str(), f.line);
            break;
        }
        bool found = false;
        for (auto& e : v) if (e.first == key) { ++e.second; found = true; break; }
        if (!found) v.emplace_back(key, 1);
    }
    std::string out;
    for (const auto& e : v) out += (out.empty() ? "" : " · ") + e.first + Fmt(" ×%d", e.second);
    return out;
}

bool SameFrames(const Sample& a, const Sample& b) {
    if (a.ok != b.ok || a.frames.size() != b.frames.size()) return false;
    for (size_t i = 0; i < a.frames.size(); ++i)
        if (a.frames[i].pc != b.frames[i].pc || a.frames[i].inlineCtx != b.frames[i].inlineCtx) return false;
    return true;
}

void Snap(const Opts& o, Result& r) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, o.pid);
    if (!proc) { r.rc = 2; r.error = Fmt("대상 프로세스를 못 엶(OpenProcess 오류 %lu)", GetLastError()); return; }
    wchar_t img[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring exePath = QueryFullProcessImageNameW(proc, 0, img, &n) ? std::wstring(img, n) : std::wstring();
    const std::wstring exeDir = exePath.substr(0, exePath.find_last_of(L"\\/"));
    const std::wstring sym = o.sym.empty() ? exeDir : o.sym;
    // 기호는 준 경로에서만 찾는다 — 실행 파일에 적힌 .pdb 절대 경로 · 실행 파일 폴더 · 환경 변수 경로를 끈다.
    //   판 도중 누가 빌드해 build\Release 의 .pdb 가 바뀌어도 판 폴더에 복사한 짝으로 풀게(대본이 --sym 첫 자리에 판 폴더를 준다).
    //   지연 읽기도 끈다 — 첫 표본에서 .pdb 를 읽느라 스레드를 오래 세우면 그 스레드의 CPU 몫이 낮게 나온다
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_CVREC |
                  SYMOPT_IGNORE_IMAGEDIR | SYMOPT_IGNORE_NT_SYMPATH);
    if (!SymInitializeW(proc, sym.c_str(), TRUE)) {
        r.rc = 3;
        r.error = Fmt("기호 초기화 실패(SymInitialize 오류 %lu)", GetLastError());
        CloseHandle(proc);
        return;
    }
    uint64_t exeBase = 0;
    if (HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, o.pid); ms != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(ms, &me)) exeBase = reinterpret_cast<uint64_t>(me.modBaseAddr);   // 첫 항목 = 실행 파일
        CloseHandle(ms);
    }
    IMAGEHLP_MODULEW64 mi{};
    mi.SizeOfStruct = sizeof(mi);
    const bool haveMi = exeBase && SymGetModuleInfoW64(proc, exeBase, &mi);
    r.symLine = Fmt("[기호] 경로 %s · 실행 파일 기호 종류 %d%s · %s", U8(sym).c_str(), haveMi ? static_cast<int>(mi.SymType) : -1,
                    haveMi && mi.SymType == SymPdb ? "(pdb)" : "(pdb 아님 — 함수 이름 못 풂)", haveMi ? U8(mi.LoadedPdbName, -1).c_str() : "");

    if (HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); ts != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(ts, &te); ok; ok = Thread32Next(ts, &te)) {
            if (te.th32OwnerProcessID != o.pid) continue;
            Thread t;
            t.tid = te.th32ThreadID;
            t.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t.tid);
            if (!t.h) continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(t.h, &c, &e, &k, &u)) t.created = FtU64(c);
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(t.h, &desc)) && desc) { t.name = U8(desc, -1); LocalFree(desc); }
            r.threads.push_back(std::move(t));
        }
        CloseHandle(ts);
    }
    // 생긴 차례로 — 게임 스레드 0 은 주 스레드에서 돈다(main.cpp:981)
    for (size_t i = 1; i < r.threads.size(); ++i)
        for (size_t j = i; j > 0 && r.threads[j].created < r.threads[j - 1].created; --j) std::swap(r.threads[j], r.threads[j - 1]);

    auto cpuOf = [](HANDLE h) -> uint64_t {
        FILETIME c, e, k, u;
        return GetThreadTimes(h, &c, &e, &k, &u) ? FtU64(k) + FtU64(u) : 0;
    };
    for (Thread& t : r.threads) t.cpu0 = cpuOf(t.h);
    const double t0 = NowSec();
    for (int s = 0; s < o.samples; ++s) {
        for (Thread& t : r.threads) { t.samples.emplace_back(); Walk(proc, t, t.samples.back()); }
        if (s + 1 < o.samples) Sleep(static_cast<DWORD>(o.gapMs));
    }
    for (Thread& t : r.threads) t.cpu1 = cpuOf(t.h);
    r.wallSec = NowSec() - t0;
    for (Thread& t : r.threads) {
        for (Sample& s : t.samples) Resolve(proc, exeBase, s);
        t.ratio = r.wallSec > 0 ? static_cast<double>(t.cpu1 - t.cpu0) / 1e7 / r.wallSec : -1;
        t.verdict = Verdict(t.ratio, o);
        for (const Sample& s : t.samples)
            for (const Frame& f : s.frames)
                if (f.fn.find(o.gameFn) != std::string::npos) t.hasGameFn = true;
    }

    // 게임 스레드 고르기 — 사슬에 게임 고리 함수가 있는 스레드 중 가장 먼저 생긴 것 · 없으면 가장 먼저 생긴 스레드
    int withFn = 0;
    for (size_t i = 0; i < r.threads.size(); ++i)
        if (r.threads[i].hasGameFn) { if (r.game < 0) r.game = static_cast<int>(i); ++withFn; }
    if (r.game >= 0) r.gameWhy = Fmt("%s 있음%s", o.gameFn.c_str(), withFn > 1 ? Fmt(" · 그런 스레드 %d개 중 가장 먼저 생김", withFn).c_str() : "");
    else if (!r.threads.empty()) { r.game = 0; r.gameWhy = Fmt("%s 없음 → 가장 먼저 생긴 스레드", o.gameFn.c_str()); }

    r.header = Fmt("[nox_stack] pid=%lu · %s · 표본 %d · 간격 %dms · 지난 %.2f초 · 스레드 %zu · 문턱 돈다 ≥ %.2f · 서 있다 ≤ %.2f", o.pid,
                   U8(exePath).c_str(), o.samples, o.gapMs, r.wallSec, r.threads.size(), o.runThr, o.idleThr);
    if (r.game >= 0) {
        const Thread& g = r.threads[static_cast<size_t>(r.game)];
        r.summary = Fmt("[요약] 게임 스레드 tid=%lu(%s) · %s CPU %.2f · 우리 코드 맨 위 = %s · 맨 위 = %s", g.tid, r.gameWhy.c_str(), g.verdict,
                        g.ratio, TopList(g, true).c_str(), TopList(g, false).c_str());
    } else {
        r.summary = "[요약] 스레드를 하나도 못 엶";
    }

    if (!o.dump.empty()) {
        const double d0 = NowSec();
        HANDLE f = CreateFileW(o.dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                                     MiniDumpWithFullMemoryInfo | MiniDumpWithUnloadedModules);
        const bool ok = f != INVALID_HANDLE_VALUE && MiniDumpWriteDump(proc, o.pid, f, type, nullptr, nullptr, nullptr);
        const DWORD err = ok ? 0 : GetLastError();
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(f, &sz)) r.dumpBytes = static_cast<uint64_t>(sz.QuadPart);
            CloseHandle(f);
        }
        r.dumpMs = static_cast<DWORD>((NowSec() - d0) * 1000);
        if (!ok) { r.rc = 4; r.error = Fmt("덤프 실패(오류 %lu)", err); }
    }

    for (Thread& t : r.threads) CloseHandle(t.h);
    SymCleanup(proc);
    CloseHandle(proc);
}

bool WriteResult(const Opts& o, const Result& r) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, o.out.c_str(), L"wb") != 0 || !f) return false;
    std::fputs("\xEF\xBB\xBF", f);   // 사람이 메모장으로 열 글 — UTF-8 표시
    std::fprintf(f, "%s\n%s\n%s\n", r.header.c_str(), r.symLine.c_str(), r.summary.c_str());
    if (!o.dump.empty())
        std::fprintf(f, "[덤프] %s · %.1fMB · %lums%s\n", U8(o.dump).c_str(), static_cast<double>(r.dumpBytes) / 1048576.0, r.dumpMs,
                     r.rc == 4 ? (" · " + r.error).c_str() : "");
    // 게임 스레드 먼저, 나머지는 생긴 차례로
    std::vector<size_t> order;
    if (r.game >= 0) order.push_back(static_cast<size_t>(r.game));
    for (size_t i = 0; i < r.threads.size(); ++i) if (static_cast<int>(i) != r.game) order.push_back(i);
    for (size_t idx : order) {
        const Thread& t = r.threads[idx];
        std::fprintf(f, "\n[스레드] tid=%lu · 생긴 차례 %zu · CPU %.2f %s%s%s\n", t.tid, idx + 1, t.ratio, t.verdict,
                     t.name.empty() ? "" : (" · 이름 " + t.name).c_str(), static_cast<int>(idx) == r.game ? " · ← 게임 스레드" : "");
        for (size_t s = 0; s < t.samples.size(); ++s) {
            const Sample& sm = t.samples[s];
            if (s > 0 && SameFrames(sm, t.samples[s - 1])) { std::fprintf(f, "  [표본 %zu] 앞 표본과 같음\n", s + 1); continue; }
            std::fprintf(f, "  [표본 %zu]%s\n", s + 1, sm.ok ? "" : Fmt(" 못 찍음(오류 %lu)", sm.err).c_str());
            for (size_t i = 0; i < sm.frames.size(); ++i) std::fprintf(f, "    #%zu %s\n", i, FrameText(sm.frames[i], true).c_str());
        }
    }
    std::fclose(f);
    return true;
}

// ── 시험용 가짜 — 일부러 갇히는 함수들 ─────────────────────────────
//   /OPT:ICF 가 같은 기계어 함수를 하나로 접으면 이름이 섞이므로, 고리마다 다른 셈을 둔다
volatile uint64_t g_sinkA = 0, g_sinkB = 0;

constexpr int kHelperFirstLine = __LINE__;
__forceinline void VictimSpinHelper(uint64_t seed) {   // 녹는 도우미 — 이 안의 고리에서 못 나온다
    uint64_t x = seed;
    for (;;) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        g_sinkA = x;
    }
}
constexpr int kHelperLastLine = __LINE__;

__declspec(noinline) void VictimSpin(uint64_t seed) { VictimSpinHelper(seed ^ 0x9E3779B97F4A7C15ULL); }

// 기다림 뒤에 할 일(g_sink 쓰기)을 둔다 — 기다림이 함수 맨 끝이면 꼬리 호출(jmp)이 되어 그 함수 프레임이 사슬에서 사라진다(11-18 첫 시험)
__forceinline void VictimWaitHelper(HANDLE h) {   // 녹는 도우미 — 이 안에서 안 오는 이벤트를 기다린다
    WaitForSingleObject(h, INFINITE);
    g_sinkB = 1;
}

__declspec(noinline) void VictimWait() {
    HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // 아무도 안 켠다
    VictimWaitHelper(never);
    g_sinkA = 2;
}

DWORD WINAPI VictimDecoySpin(LPVOID) {
    uint64_t x = 88172645463325252ULL;
    for (;;) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        g_sinkB = x;
    }
}

DWORD WINAPI VictimDecoyWait(LPVOID) {
    SleepEx(INFINITE, FALSE);
    g_sinkB = 3;
    return 0;
}

void SignalReady() {
    wchar_t name[64];
    std::swprintf(name, 64, L"nox_stack_ready_%lu", GetCurrentProcessId());
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) { SetEvent(ev); CloseHandle(ev); }
}

// 가짜 서버 — 진짜 서버처럼 1초마다 [구간] 줄 · 사이사이 [판 결과] 줄을 찍고 끝내기 이벤트 nox_server_stop_<pid> 를 듣는다.
//   stallAt 초에 도는 함수에 갇히고(끝내기 신호를 못 듣는다) · dieAt 초에 dieCode 로 스스로 끝난다
int VictimFakeServer(int stallAt, int dieAt, int dieCode) {
    wchar_t name[64];
    std::swprintf(name, 64, L"nox_server_stop_%lu", GetCurrentProcessId());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, name);
    std::printf("옵션: 가짜 서버(nox_stack --victim fakeserver) · 갇힘 %d초 · 끝남 %d초(0 = 안 함)\n", stallAt, dieAt);
    std::fflush(stdout);
    for (int sec = 1;; ++sec) {
        for (int q = 0; q < 4; ++q) {
            if (WaitForSingleObject(stop, 250) == WAIT_OBJECT_0) {
                std::printf("가짜 서버 — 끝내기 신호로 끝\n");
                std::fflush(stdout);
                return 0;
            }
            std::printf("[판 결과] %d-%d · 방 %d · estate(arena) · score_player · 승자 #1 · 82초 · #1 5/4 · #2 1/3 · #3 8/4 · #4 1/2 · #5 5/2"
                        " · #6 1/3 · #7 10/3 · #8 1/2 · #9 7/3 · #10 0/2\n", sec, q, q);
            std::fflush(stdout);
        }
        if (sec == dieAt) {
            std::printf("가짜 서버 — %d초에 스스로 끝남(코드 %d)\n", sec, dieCode);
            std::fflush(stdout);
            return dieCode;
        }
        if (sec == stallAt) VictimSpin(static_cast<uint64_t>(sec));
        std::printf("[구간] 초 %d · 틱 30 · 틱합 990.0 · 틱최대 2.0 · 사건 0.0(수 0) · 방 만들기 0.0(수 0 · 최대 0.0) · 방 닫기 0.0(수 0 · 최대 0.0)"
                    " · 뒷일 0.0 · 앞 0.0 · 걸음 0.0 · 생각 0.0 · 시전 0.0 · 세계 0.0 · 자기 0.0 · 사람 0.0 · 물체 0.0 · 소환수 0.0 · 정적 0.0"
                    " · 묶음 0.0 · 통신 재전송 0.0 · 보내기 0.0 · 받기 0.0(수 0) · 넘기기 0.0 · CPU 게임 990 통신 0 기록 0 · 끊김 0(시간 초과 0 · 0a 0)"
                    " · 넘침 0 · 지난 1000.0 · 기다림 10.0 · 바쁨 99.0%% · 고리 나머지 0.0 · 메모리 개인 9.9MB 작업 13.7MB · 방 416(덜 참 0)\n", sec);
        std::fflush(stdout);
    }
}

// ── 자가 시험 ────────────────────────────────────────────────────
struct Victim {
    HANDLE proc = nullptr;
    DWORD pid = 0;
};

bool SpawnVictim(const wchar_t* kind, Victim& v) {
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::wstring cmd = std::wstring(L"\"") + self + L"\" --victim " + kind;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    wchar_t name[64];
    std::swprintf(name, 64, L"nox_stack_ready_%lu", pi.dwProcessId);
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, name);   // 이름에 pid 가 들어가 띄운 뒤 · 돌리기 전에 만든다
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    const DWORD w = WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
    if (w != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 99); CloseHandle(pi.hProcess); return false; }
    Sleep(200);   // 가짜 스레드가 제 함수에 들어갈 때까지
    v.proc = pi.hProcess;
    v.pid = pi.dwProcessId;
    return true;
}

void EndVictim(Victim& v) {
    if (!v.proc) return;
    TerminateProcess(v.proc, 0);   // 일부러 갇힌 시험 프로세스 — 끝낼 길이 없게 만든 것이라 바로 끝낸다
    WaitForSingleObject(v.proc, 5000);
    CloseHandle(v.proc);
    v.proc = nullptr;
}

// 이름 끝이 맞나 — 이름공간이 붙어 나온다(`anonymous namespace'::VictimSpin · 녹은 함수는 ?A0x…::VictimSpinHelper)
bool NameIs(const std::string& fn, const char* want) {
    const std::string w = want;
    if (fn == w) return true;
    const std::string tail = "::" + w;
    return fn.size() > tail.size() && fn.compare(fn.size() - tail.size(), tail.size(), tail) == 0;
}

int SamplesWith(const Thread& t, const char* fn, bool inlineOnly = false) {
    int n = 0;
    for (const Sample& s : t.samples)
        for (const Frame& f : s.frames)
            if (NameIs(f.fn, fn) && (!inlineOnly || f.inl)) { ++n; break; }
    return n;
}

const Thread* ThreadWith(const Result& r, const char* fn) {
    for (const Thread& t : r.threads) if (SamplesWith(t, fn) == static_cast<int>(t.samples.size()) && !t.samples.empty()) return &t;
    return nullptr;
}

std::string ReadAll(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

int SelfTest(const Opts& base, const std::wstring& outDir) {
    CreateDirectoryW(outDir.c_str(), nullptr);
    int pass = 0, fail = 0;
    auto check = [&](const char* what, bool ok, const std::string& got) {
        std::printf("%s %s — %s\n", ok ? "[통과]" : "[틀림]", what, got.c_str());
        (ok ? pass : fail)++;
    };
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    std::wstring selfDir(self);
    selfDir = selfDir.substr(0, selfDir.find_last_of(L"\\/"));

    auto run = [&](const wchar_t* kind, const wchar_t* outName, const char* gameFn, bool dump, Result& r) -> bool {
        Victim v;
        if (!SpawnVictim(kind, v)) { check(Fmt("가짜 %s 띄우기", U8(kind).c_str()).c_str(), false, "못 띄움"); return false; }
        Opts o = base;
        o.pid = v.pid;
        o.out = outDir + L"\\" + outName;
        if (o.sym.empty()) o.sym = selfDir;
        if (gameFn) o.gameFn = gameFn;
        if (dump) o.dump = outDir + L"\\selftest_wait.dmp";
        Snap(o, r);
        const bool wrote = r.rc == 0 && WriteResult(o, r);
        EndVictim(v);
        check(Fmt("%s 찍기 · 쓰기", U8(outName).c_str()).c_str(), wrote, r.rc ? r.error : U8(o.out));
        return wrote;
    };

    // ㉠ 도는 함수 — 주 스레드가 녹는 도우미 안 고리에 갇힘 · 미끼 스레드는 잠
    {
        Result r;
        if (run(L"spin", L"selftest_spin.txt", nullptr, false, r)) {
            const Thread& g = r.threads[static_cast<size_t>(r.game)];
            check("도는 쪽 — 요약 스레드의 표본마다 VictimSpin", SamplesWith(g, "VictimSpin") == base.samples,
                  Fmt("%d/%d · %s", SamplesWith(g, "VictimSpin"), base.samples, r.summary.c_str()));
            check("도는 쪽 — 요약 스레드 「돈다」", std::strcmp(g.verdict, "돈다") == 0, Fmt("%s %.2f", g.verdict, g.ratio));
            const Thread* d = ThreadWith(r, "VictimDecoyWait");
            check("도는 쪽 — 미끼 스레드 「서 있다」", d && std::strcmp(d->verdict, "서 있다") == 0,
                  d ? Fmt("%s %.2f", d->verdict, d->ratio) : "미끼 스레드 못 찾음");
            // 녹은 도우미 안 고리 — 기록만(판정 아님). 고리 되돌이 명령이 부른 쪽 줄로 적혀 있어 표본이 거기 떨어지면
            //   녹은 이름이 안 나온다(11-18 첫 시험 5개 중 4개). 녹은 함수 펼치기의 판정은 기다리는 쪽(돌아갈 자리 — 늘 같은 명령)이 맡는다
            int named = 0, lineOnly = 0;
            for (const Sample& s : g.samples) {
                if (s.frames.empty()) continue;
                bool n = false;
                for (const Frame& f : s.frames) if (NameIs(f.fn, "VictimSpinHelper") && f.inl) n = true;
                if (n) ++named;
                else if (BaseName(s.frames[0].file) == "stack_snap.cpp" && static_cast<int>(s.frames[0].line) > kHelperFirstLine &&
                         static_cast<int>(s.frames[0].line) < kHelperLastLine) ++lineOnly;
            }
            std::printf("[기록] 도는 쪽 — 고리 안 녹은 도우미: 녹은 이름 %d · 줄만 도우미 %d · 부른 쪽 줄 %d / %d\n", named, lineOnly,
                        base.samples - named - lineOnly, base.samples);
            size_t badCount = 0;
            for (const Thread& t : r.threads) if (static_cast<int>(t.samples.size()) != base.samples) ++badCount;
            check("도는 쪽 — 스레드마다 표본 수", r.threads.size() >= 2 && badCount == 0, Fmt("스레드 %zu · 표본 수 다른 스레드 %zu", r.threads.size(), badCount));
            const std::string text = ReadAll(outDir + L"\\selftest_spin.txt");
            check("도는 쪽 — 글에 [요약] · VictimSpin", text.find("[요약]") != std::string::npos && text.find("VictimSpin") != std::string::npos,
                  Fmt("%zu바이트", text.size()));
        }
    }
    // ㉡ 기다리는 함수 — 주 스레드가 안 오는 이벤트를 기다림 · 미끼 스레드는 돎 + 덤프
    {
        Result r;
        if (run(L"wait", L"selftest_wait.txt", nullptr, true, r)) {
            const Thread& g = r.threads[static_cast<size_t>(r.game)];
            check("기다리는 쪽 — 요약 스레드의 표본마다 VictimWait", SamplesWith(g, "VictimWait") == base.samples,
                  Fmt("%d/%d · %s", SamplesWith(g, "VictimWait"), base.samples, r.summary.c_str()));
            check("기다리는 쪽 — 녹은 도우미 VictimWaitHelper 가 표본마다 [녹은 함수]로", SamplesWith(g, "VictimWaitHelper", true) == base.samples,
                  Fmt("%d/%d", SamplesWith(g, "VictimWaitHelper", true), base.samples));
            check("기다리는 쪽 — 요약 스레드 「서 있다」", std::strcmp(g.verdict, "서 있다") == 0, Fmt("%s %.2f", g.verdict, g.ratio));
            const Thread* d = ThreadWith(r, "VictimDecoySpin");
            check("기다리는 쪽 — 미끼 스레드 「돈다」", d && std::strcmp(d->verdict, "돈다") == 0,
                  d ? Fmt("%s %.2f", d->verdict, d->ratio) : "미끼 스레드 못 찾음");
            check("기다리는 쪽 — 덤프", r.dumpBytes > 0, Fmt("%.1fMB · %lums", static_cast<double>(r.dumpBytes) / 1048576.0, r.dumpMs));
        }
    }
    // ㉢ 게임 고리 함수 고르기 — 같은 가짜에서 표시 함수를 미끼(나중에 생긴 스레드)로 주면 그 스레드를 골라야
    {
        Result r;
        if (run(L"wait", L"selftest_marker.txt", "VictimDecoySpin", false, r)) {
            const Thread& g = r.threads[static_cast<size_t>(r.game)];
            check("표시 함수 — 나중에 생긴 미끼 스레드를 고름", r.game > 0 && SamplesWith(g, "VictimDecoySpin") == base.samples &&
                  std::strcmp(g.verdict, "돈다") == 0, Fmt("생긴 차례 %d · %s", r.game + 1, r.summary.c_str()));
        }
    }
    std::printf("시험 %d/%d 통과\n", pass, pass + fail);
    return fail == 0 ? 0 : 10;
}

bool ArgVal(int argc, wchar_t** argv, int& i, std::wstring& out) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Opts o;
    std::wstring victim, outDir = L".";
    bool selfTest = false;
    int stallAt = 0, dieAt = 0, dieCode = 3, sleepSec = 0, sleepCode = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        std::wstring v;
        auto num = [&](auto& dst) -> bool {
            if (!ArgVal(argc, argv, i, v)) return false;
            dst = static_cast<std::remove_reference_t<decltype(dst)>>(std::wcstod(v.c_str(), nullptr));
            return true;
        };
        bool ok = true;
        if (k == L"--pid") ok = num(o.pid);
        else if (k == L"--samples") ok = num(o.samples);
        else if (k == L"--gap-ms") ok = num(o.gapMs);
        else if (k == L"--out") { ok = ArgVal(argc, argv, i, v); o.out = v; outDir = v; }
        else if (k == L"--dump") { ok = ArgVal(argc, argv, i, v); o.dump = v; }
        else if (k == L"--sym") { ok = ArgVal(argc, argv, i, v); o.sym = v; }
        else if (k == L"--game-fn") { ok = ArgVal(argc, argv, i, v); o.gameFn = U8(v); }
        else if (k == L"--run-thr") ok = num(o.runThr);
        else if (k == L"--idle-thr") ok = num(o.idleThr);
        else if (k == L"--self-test") selfTest = true;
        else if (k == L"--victim") { ok = ArgVal(argc, argv, i, v); victim = v; }
        else if (k == L"--stall-at") ok = num(stallAt);
        else if (k == L"--die-at") ok = num(dieAt);
        else if (k == L"--die-code") ok = num(dieCode);
        else if (k == L"--sec") ok = num(sleepSec);
        else if (k == L"--code") ok = num(sleepCode);
        else ok = false;
        if (!ok) { std::printf("모르는 인자 또는 값 없음: %s\n", U8(k).c_str()); return 1; }
    }

    if (victim == L"spin") {
        HANDLE d = CreateThread(nullptr, 0, VictimDecoyWait, nullptr, 0, nullptr);
        (void)d;
        SignalReady();
        VictimSpin(1);
        return 0;
    }
    if (victim == L"wait") {
        HANDLE d = CreateThread(nullptr, 0, VictimDecoySpin, nullptr, 0, nullptr);
        (void)d;
        SignalReady();
        VictimWait();
        return 0;
    }
    if (victim == L"fakeserver") return VictimFakeServer(stallAt, dieAt, dieCode);
    if (victim == L"sleep") {
        std::printf("가짜 더미 — %d초 뒤 끝(코드 %d)\n", sleepSec, sleepCode);
        std::fflush(stdout);
        Sleep(static_cast<DWORD>(sleepSec) * 1000);
        return sleepCode;
    }
    if (!victim.empty()) { std::printf("모르는 가짜: %s\n", U8(victim).c_str()); return 1; }
    if (selfTest) return SelfTest(o, outDir);

    if (o.pid == 0 || o.out.empty() || o.samples < 1 || o.gapMs < 0) {
        std::printf("쓰기: nox_stack --pid P [--samples 5] [--gap-ms 200] --out 파일 [--dump 파일] [--sym 경로;경로] [--game-fn 이름]"
                    " [--run-thr 0.5] [--idle-thr 0.05] | --self-test [--out 폴더] | --victim spin|wait|fakeserver|sleep\n");
        return 1;
    }
    Result r;
    Snap(o, r);
    if (r.rc == 2 || r.rc == 3) { std::printf("%s\n", r.error.c_str()); return r.rc; }
    if (!WriteResult(o, r)) { std::printf("결과 파일을 못 씀: %s\n", U8(o.out).c_str()); return 1; }
    std::printf("%s\n%s\n", r.summary.c_str(), r.rc ? r.error.c_str() : "");
    return r.rc;
}
