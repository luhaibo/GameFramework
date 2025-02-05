﻿//
// Project: CMiniLang
// Author: bajdcc
//

#include "stdafx.h"
#include <cassert>
#include <memory.h>
#include <cstring>
#include <regex>
#include <random>
#include <sstream>
#include "cvm.h"
#include "cgen.h"
#include "cexception.h"
#include "cgui.h"
#include "cnet.h"
#include "cmusic.h"
#include <ui\window\Window.h>

#define REPORT_ERROR 1
#define REPORT_ERROR_FILE "error.log"

#define REPORT_MEMORY 0
#define REPORT_MEMORY_FILE "mem.log"

#define REPORT_DEBUG_FILE "debug.log"

#define LOG_INS 0
#define LOG_STACK 0
#define LOG_SYSTEM 1
#define LOG_MAX 10

#define SOFT_KILL_SIGNAL 9
#define KILL_SIGNAL 99
#define VFS_LINK_MAX_NUM 5

int g_argc;
char** g_argv;

namespace clib {

#define INC_PTR 4

    cvm::global_state_t cvm::global_state;

    uint32_t cvm::pmm_alloc() {
        ctx->pages.emplace_back();
        ctx->pages.back().resize(PAGE_SIZE);
        std::fill(ctx->pages.back().begin(), ctx->pages.back().end(), 0);
        auto ptr = PAGE_ALIGN_UP((uint32)ctx->pages.back().data());
        ctx->allocation.push_back(ptr);
        return ptr;
    }

    void cvm::vmm_init() {
        init_fs();
        init_global();
    }

    void cvm::init_fs() {
        fs.as_root(true);
        fs.mkdir("/sys");
        fs.func("/sys/ps", this);
        fs.func("/sys/mem", this);
        fs.func("/sys/time", this);
        fs.func("/sys/uptime", this);
        fs.func("/sys/pstree", this);
        fs.mkdir("/init");
        fs.mkdir("/proc");
        fs.mkdir("/handle");
        fs.mkdir("/dev");
        fs.func("/dev/random", this);
        fs.func("/dev/null", this);
        fs.func("/dev/console", this);
        fs.func("/dev/uuid", this);
        fs.func("/dev/debug", this);
        fs.magic("/http", this, fss_net);
        fs.magic("/music", this, fss_music);
        fs.mkdir("/log");
        fs.func("/log/info", this);
        fs.func("/log/err", this);
        fs.magic("/server", this, fss_server);
        fs.load("/usr/logo.txt");
        fs.load("/usr/badapple.txt");
        fs.load("/usr/test_command.txt");
        fs.load("/usr/test_lua.txt");
        fs.load("/init/init.txt");
        fs.load("/init/ipc.txt");
        fs.load_bin("/usr/github.png");
        fs.load_bin("/usr/loading.gif");
        fs.load_dir("/www");
        fs.as_root(false);
        fs.mkdir("/pipe");
        fs.mkdir("/semaphore");
        fs.mkdir("/mutex");
        fs.mkdir("/fifo");
    }

    void cvm::init_global()
    {
        global_state.interrupt = false;
        global_state.input_lock = -1;
        global_state.input_waiting_list.clear();
        global_state.input_content.clear();
        global_state.input_success = false;
        global_state.input_read_ptr = -1;
        global_state.input_code = 0;
        global_state.input_single = false;
        global_state.hostname = "ccos";
        global_state.gui = false;
        global_state.gui_blur = 0.0f;
        global_state.now = std::chrono::system_clock::now();
#if LOG_VM
        global_state.log_info.clear();
        global_state.log_err.clear();
#endif
        global_state.mouse_x = 0;
        global_state.mouse_y = 0;
        global_state.window_focus = -1;
        global_state.window_hover = -1;
        global_state.logging.clear();
    }

    int cvm::vfs_open(const string_t& path, vfs_node_dec** dec)
    {
        if (path[0] != '/') {
            auto s = 0;
            decltype(ctx->paths) ps;
            auto pwd = fs.get_pwd();
            ps.push_back(pwd == "/" ? "" : pwd);
            std::copy(ctx->paths.begin(), ctx->paths.end(), std::back_inserter(ps));
            for (auto& p : ps) {
                auto pp = p + '/' + path;
                s = fs.get(pp, dec, this);
                if (s == 0)
                    break;
            }
            if (s != 0) {
                ctx->ax._i = s;
                return s;
            }
        }
        else {
            auto s = fs.get(path, dec, this);
            if (s != 0) {
                ctx->ax._i = s;
                return s;
            }
        }
        return 0;
    }

    void cvm::logging(CString s)
    {
        if (global_state.logging.size() > LOG_MAX)
            global_state.logging.pop_front();
        global_state.logging.push_back(s);
    }

    // 虚页映射
    // va = 虚拟地址  pa = 物理地址
    void cvm::vmm_map(uint32_t va, uint32_t pa) {
        ctx->pgdir.insert(std::make_pair(PAGE_INDEX(va), pa));
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] MEM  | MAP: PA= %p, VA= %p\n", (void*)pa, (void*)va);
#endif
    }

    // 释放虚页
    void cvm::vmm_unmap(uint32_t va) {
        ctx->pgdir.erase(PAGE_INDEX(va));
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] MEM  | UNMAP: VA= %p\n", (void*)va);
#endif
    }

    // 是否已分页
    int cvm::vmm_ismap(uint32_t va, uint32_t* pa) const {
        auto f = ctx->pgdir.find(PAGE_INDEX(va));
        if (f != ctx->pgdir.end()) {
            *pa = f->second;
            return 1;
        }
        return 0;
    }

    string_t cvm::vmm_getstr(uint32_t va) const {
        std::stringstream ss;
        char c;
        while (c = vmm_get<char>(va++), c) ss << c;
        return ss.str();
    }

    void cvm::vmm_getmem(uint32_t va, int len, std::vector<byte>& data) const {
        byte c;
        for (auto i = 0; i < len; i++) {
            c = vmm_get<byte>(va++);
            data.push_back(c);
        }
    }

    void cvm::vmm_setmem(uint32_t va, int len, const std::vector<byte>& data)
    {
        for (auto i = 0; i < len; i++) {
            vmm_set<byte>(va++, data[i]);
        }
    }

    template<class T>
    T cvm::vmm_get(uint32_t va) const {
        if (va == 0)
            error("vmm::get nullptr deref!!");
        if (!(ctx->flag & CTX_KERNEL))
            va |= ctx->mask;
        uint32_t pa;
        if (PAGE_INDEX(va) == PAGE_INDEX(va + sizeof(T) - 1)) {
            if (vmm_ismap(va, &pa)) {
                return *(T*)((byte*)pa + OFFSET_INDEX(va));
            }
        }
        else {
            T t;
            byte* p = (byte*)& t;
            auto pid = -1;
            auto succ = true;
            for (auto addr = va; addr < va + sizeof(T); addr++) {
                if (OFFSET_INDEX(addr) != pid) {
                    pid = OFFSET_INDEX(addr);
                    if (!vmm_ismap(addr, &pa)) {
                        succ = false;
                        break;
                    }
                }
                *p++ = *((byte*)pa + OFFSET_INDEX(addr));
            }
            if (succ)
                return t;
        }
        //vmm_map(va, pmm_alloc(), PTE_U | PTE_P | PTE_R);
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] MEM  | Invalid VA: %08X\n", va);
#endif
#if REPORT_ERROR
        {
            std::ofstream log(REPORT_ERROR_FILE, std::ios::app | std::ios::out);
            log << std::endl << "VMM::GET invalid ptr: " << std::hex << va << std::endl;
        }
#endif
        error("vmm::get error");
        return vmm_get<T>(va);
    }

    template<class T>
    bool cvm::vmm_valid(uint32_t va) const {
        if (va == 0)
            return false;
        if (!(ctx->flag & CTX_KERNEL))
            va |= ctx->mask;
        uint32_t pa;
        if (vmm_ismap(va, &pa)) {
            return true;
        }
        return false;
    }

    template<class T>
    T cvm::vmm_set(uint32_t va, T value) {
        if (!(ctx->flag & CTX_KERNEL)) {
            if ((ctx->flag & CTX_USER_MODE) && (va & 0xF0000000) == USER_BASE) {
                error("code segment cannot be written");
            }
            va |= ctx->mask;
        }
        uint32_t pa;
        if (vmm_ismap(va, &pa)) {
            *(T*)((byte*)pa + OFFSET_INDEX(va)) = value;
            return value;
        }
        //vmm_map(va, pmm_alloc(), PTE_U | PTE_P | PTE_R);
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] MEM  | Invalid VA: %08X\n", va);
#endif
#if REPORT_ERROR
        {
            std::ofstream log(REPORT_ERROR_FILE, std::ios::app | std::ios::out);
            log << std::endl << "VMM::SET invalid ptr: " << std::hex << va <<
                ", value: " << std::hex << va << std::endl;
        }
#endif
        error("vmm::set error");
        return vmm_set(va, value);
    }

    void cvm::vmm_setstr(uint32_t va, const string_t& str) {
        auto len = str.length();
        for (uint32_t i = 0; i < len; i++) {
            vmm_set(va + i, str[i]);
        }
        vmm_set(va + len, '\0');
    }

    uint32_t cvm::vmm_malloc(uint32_t size) {
        auto r = ctx->heap | (ctx->pool->alloc(size) & SEGMENT_MASK);
#if REPORT_MEMORY
        {
            static char sz2[200];
            snprintf(sz2, sizeof(sz2), "[%4d] %-20s : ALLOC %8d -> %08X (%d)", ctx->id, ctx->path.c_str(), size, r, ctx->pool->page_size());
            std::ofstream log(REPORT_MEMORY_FILE, std::ios::app | std::ios::out);
            log << sz2 << std::endl;
            ctx->pool->dump_str(log);
        }
#endif
        return r;
    }

    uint32_t cvm::vmm_free(uint32_t addr) {
        if ((addr & HEAP_BASE) != HEAP_BASE) {
            return 0;
        }
        auto r = ctx->pool->free(addr & SEGMENT_MASK);
#if REPORT_MEMORY
        {
            static char sz2[200];
            snprintf(sz2, sizeof(sz2), "[%4d] %-20s : FREE  %08X -> %8d", ctx->id, ctx->path.c_str(), addr, r);
            std::ofstream log(REPORT_MEMORY_FILE, std::ios::app | std::ios::out);
            log << sz2 << std::endl;
            //ctx->pool->dump_str(log);
        }
#endif
        return r;
    }

    uint32_t cvm::vmm_memset(uint32_t va, uint32_t value, uint32_t count) {
#if 0
        uint32_t pa;
        if (vmm_ismap(va, &pa))
        {
            pa |= OFFSET_INDEX(va);
            ATLTRACE("MEMSET> V=%08X P=%08X S=%08X\n", va, pa, count);
        }
        else
        {
            ATLTRACE("MEMSET> V=%08X P=ERROR S=%08X\n", va, count);
        }
#endif
        for (uint32_t i = 0; i < count; i++) {
#if 0
            ATLTRACE("MEMSET> V=%08X\n", va + i);
#endif
            vmm_set<byte>(va + i, value);
        }
        return 0;
    }

    uint32_t cvm::vmm_memcmp(uint32_t src, uint32_t dst, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
#if 0
            ATLTRACE("MEMCMP> '%c':'%c'\n", vmm_get<byte>(src + i), vmm_get<byte>(dst + i));
#endif
            if (vmm_get<byte>(src + i) > vmm_get<byte>(dst + i))
                return 1;
            if (vmm_get<byte>(src + i) < vmm_get<byte>(dst + i))
                return 2;
        }
        return 0;
    }

    template<class T>
    void cvm::vmm_pushstack(uint32_t& sp, T value) {
        sp -= sizeof(T);
        vmm_set<T>(sp, value);
    }

    template<class T>
    T cvm::vmm_popstack(uint32_t& sp) {
        T t = vmm_get<T>(sp);
        sp += sizeof(T);
        return t;
    }

    //-----------------------------------------

    cvm::cvm() {
        vmm_init();
    }

    cvm::~cvm() {
        reset();
    }

    void cvm::reset() {
        // 释放句柄
        for (int i = 0; i < TASK_NUM; ++i) {
            if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                ctx = tasks[i].get();
                const auto handles = ctx->handles;
                for (auto& h : handles) {
                    destroy_handle(h);
                }
            }
        }
        ctx = nullptr;
    }

    bool cvm::run(int cycle, int& cycles) {
        int c;
        for (int i = 0; i < TASK_NUM; ++i) {
            if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                if (tasks[i]->state == CTS_RUNNING) {
                    ctx = tasks[i].get();
                    c = 0;
                    exec(cycle, c);
                    if (tasks[i])
                        tasks[i]->ips += c;
                    cycles += c;
                }
            }
        }
        if (global_state.interrupt) {
            global_state.interrupt = false;
            std::vector<int> foreground_pids;
            for (int i = 1; i < TASK_NUM; ++i) {
                if (tasks[i] && (tasks[i]->flag & CTX_VALID) &&
                    !(tasks[i]->flag & CTX_SERVICE) &&
                    (tasks[i]->flag & CTX_FOREGROUND) &&
                    tasks[i]->parent != 0)
                    foreground_pids.push_back(i);
            }
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] SIG  | Received Ctrl-C!\n");
#endif
#if LOG_VM
            logging(CString(L"[SYSTEM] SIG  | Received Ctrl-C!"));
#endif
            for (auto& pid : foreground_pids) {
                destroy(pid);
            }
        }
        ctx = nullptr;
        return available_tasks > 0;
    }

    void cvm::exec(int cycle, int& cycles) {
        if (!ctx)
            error("no process!");
#if LOG_STACK
        std::ofstream log(REPORT_DEBUG_FILE, std::ios::app | std::ios::out);
#endif
        for (auto i = 0; i < cycle; ++i) {
            i++;
            cycles++;
            if (global_state.interrupt) break;
            if ((ctx->pc & 0xF0000000) != USER_BASE) {
                if (ctx->pc != 0xE0000FF4 && ctx->pc != 0xE0000FFC) {
#if LOG_SYSTEM
                    ATLTRACE("[SYSTEM] ERR  | Invalid PC: %p\n", (void*)ctx->pc);
#endif
                    error("only code segment can execute");
                }
            }
            auto op = vmm_get(ctx->pc); // get next operation code
            ctx->pc += INC_PTR;
            auto old_pc = ctx->pc;
#if LOG_INS
            assert(op <= EXIT);
            // print debug info
            if (ctx->debug) {
                CStringA a, b;
                a.Format("%04d> [%08X] %02d %.4s", i, ctx->pc, op, INS_STRING((ins_t)op).c_str());
                if ((op >= PUSH && op <= LNT) || op == LOAD || op == SAVE)
                    b.Format(" %d\n", vmm_get(ctx->pc));
                else if (op == IMX)
                    b.Format(" %08X(%d) %08X(%d)\n", vmm_get(ctx->pc), vmm_get(ctx->pc),
                        vmm_get(ctx->pc + INC_PTR), vmm_get(ctx->pc + INC_PTR));
                else if (op <= ADJ)
                    b.Format(" %08X(%d)\n", vmm_get(ctx->pc), vmm_get(ctx->pc));
                else
                    b.Format("\n");
                ATLTRACE((a + b).GetBuffer(0));
            }
            if (ctx->debug) {
                CStringA a, b;
                a.Format("%04d> [%08X] %02d %.4s", i, ctx->pc, op, INS_STRING((ins_t)op).c_str());
                if ((op >= PUSH && op <= LNT) || op == LOAD || op == SAVE)
                    b.Format(" %d", vmm_get(ctx->pc));
                else if (op == IMX)
                    b.Format(" %08X(%d) %08X(%d)", vmm_get(ctx->pc), vmm_get(ctx->pc),
                        vmm_get(ctx->pc + INC_PTR), vmm_get(ctx->pc + INC_PTR));
                else if (op <= ADJ)
                    b.Format(" %08X(%d)", vmm_get(ctx->pc), vmm_get(ctx->pc));
                log << ((a + b).GetBuffer(0)) << std::endl;
            }
#endif
            switch (op) {
            case NOP:
                break;
            case IMM: {
                ctx->ax._i = vmm_get(ctx->pc);
                ctx->pc += INC_PTR;
            } /* load immediate value to ctx->ax._i */
                      break;
            case IMX: {
                ctx->ax._u._1 = vmm_get(ctx->pc);
                ctx->pc += INC_PTR;
                ctx->ax._u._2 = vmm_get(ctx->pc);
                ctx->pc += INC_PTR;
            } /* load immediate value to ctx->ax._i */
                      break;
            case LOAD: {
                auto n = vmm_get(ctx->pc);
                if (n <= 8) {
                    switch (n) {
                    case 1:
                        ctx->ax._i = vmm_get<byte>((uint32_t)ctx->ax._i);
                        break;
                    case 2:
                    case 3:
                    case 4:
                        ctx->ax._i = vmm_get((uint32_t)ctx->ax._i);
                        break;
                    case 8:
                        ctx->ax._uq = vmm_get<uint64>((uint32_t)ctx->ax._i);
                        break;
                    default:
                        error("load: not supported");
                        break;
                    }
                }
                else if (n <= BIG_DATA_NUM) {
                    auto addr = (uint32_t)ctx->ax._i;
                    for (auto j = 0; j < n / 4; ++j) {
                        *((int*)& ctx->ax.big_data[j * 4]) = vmm_get((addr)+j * 4);
                    }
                    if (n % 4 != 0) {
                        *((int*)& ctx->ax.big_data[n & ~3]) = vmm_get((addr)+(n & ~3));
                        memset(&ctx->ax.big_data[n], 0, (size_t)(4 - (n % 3)));
                    }
                }
                else {
                    error("load: not supported big data");
                }
                ctx->pc += INC_PTR;
            } /* load integer to ctx->ax._i, address in ctx->ax._i */
                       break;
            case SAVE: {
                auto n = vmm_get(ctx->pc);
                if (n <= 8) {
                    switch (n) {
                    case 1:
                        vmm_set<byte>((uint32_t)vmm_popstack(ctx->sp), (byte)ctx->ax._i);
                        break;
                    case 2:
                    case 3:
                    case 4:
                        vmm_set((uint32_t)vmm_popstack(ctx->sp), ctx->ax._i);
                        break;
                    case 8:
                        vmm_set((uint32_t)vmm_popstack(ctx->sp), ctx->ax._uq);
                        break;
                    default:
                        error("save: not supported");
                        break;
                    }
                }
                else if (n <= BIG_DATA_NUM) {
                    auto addr = (uint32_t)vmm_popstack(ctx->sp);
                    for (auto j = 0; j < n / 4; ++j) {
                        vmm_set(addr + j * 4, *((uint32_t*)& ctx->ax.big_data[j * 4]));
                    }
                    if (n % 4 != 0) {
                        memset(&ctx->ax.big_data[n], 0, (size_t)(4 - (n % 3)));
                        vmm_set(addr + (n & ~3), *((uint32_t*)& ctx->ax.big_data[n & ~3]));
                    }
                }
                else {
                    error("save: not supported big data");
                }
                ctx->pc += INC_PTR;
            } /* save integer to address, value in ctx->ax._i, address on stack */
                       break;
            case PUSH: {
                auto n = vmm_get(ctx->pc);
                if (n <= 8) {
                    switch (n) {
                    case 4:
                        vmm_pushstack(ctx->sp, ctx->ax._i);
                        break;
                    case 8:
                        vmm_pushstack(ctx->sp, ctx->ax._u._2);
                        vmm_pushstack(ctx->sp, ctx->ax._u._1);
                        break;
                    default:
                        error("push: not supported");
                        break;
                    }
                }
                else if (n <= BIG_DATA_NUM) {
                    if (n % 4 != 0) {
                        memset(&ctx->ax.big_data[n], 0, (size_t)(4 - (n % 3)));
                        vmm_pushstack(ctx->sp, *((uint32_t*)& ctx->ax.big_data[n & ~3]));
                    }
                    for (auto j = n / 4 - 1; j >= 0; --j) {
                        vmm_pushstack(ctx->sp, *((uint32_t*)& ctx->ax.big_data[j * 4]));
                    }
                }
                else {
                    error("push: not supported big data");
                }
                ctx->pc += INC_PTR;
            } /* push the value of ctx->ax._i onto the stack */
                       break;
            case POP: {
                auto n = vmm_get(ctx->pc);
                if (n <= 8) {
                    switch (n) {
                    case 4:
                        ctx->ax._i = vmm_popstack(ctx->sp);
                        break;
                    case 8:
                        ctx->ax._q = vmm_popstack<int64>(ctx->sp);
                        break;
                    default:
                        error("pop: not supported");
                        break;
                    }
                }
                else if (n <= BIG_DATA_NUM) {
                    for (auto j = 0; j < n / 4; ++j) {
                        *((uint32_t*)& ctx->ax.big_data[j * 4]) = vmm_popstack<uint32_t>(ctx->sp);
                    }
                    if (n % 4 != 0) {
                        *((uint32_t*)& ctx->ax.big_data[n & ~3]) = vmm_popstack<uint32_t>(ctx->sp);
                        memset(&ctx->ax.big_data[n], 0, (size_t)(4 - (n % 3)));
                    }
                }
                else {
                    error("pop: not supported big data");
                }
                ctx->pc += INC_PTR;
            } /* pop the value of ctx->ax._i from the stack */
                      break;
            case JMP: {
                ctx->pc = ctx->base + vmm_get(ctx->pc) * INC_PTR;
            } /* jump to the address */
                      break;
            case JZ: {
                ctx->pc = ctx->ax._i ? ctx->pc + INC_PTR : (ctx->base + vmm_get(ctx->pc) * INC_PTR);
            } /* jump if ctx->ax._i is zero */
                     break;
            case JNZ: {
                ctx->pc = ctx->ax._i ? (ctx->base + vmm_get(ctx->pc) * INC_PTR) : ctx->pc + INC_PTR;
            } /* jump if ctx->ax._i is zero */
                      break;
            case CALL: {
                vmm_pushstack(ctx->sp, ctx->pc);
                ctx->stacktrace_pc2.push_back(ctx->pc);
                ctx->pc = ctx->base + (ctx->ax._ui) * INC_PTR;
                ctx->stacktrace_pc.push_back(ctx->pc);
            } /* call subroutine */
                /* break;case RET: {pc = (int *)*sp++;} // return from subroutine; */
                       break;
            case ENT: {
                ctx->stacktrace.push_back(ctx->bp);
                vmm_pushstack(ctx->sp, ctx->bp);
                ctx->bp = ctx->sp;
                ctx->sp = ctx->sp - vmm_get(ctx->pc);
                ctx->pc += INC_PTR;
            } /* make new stack frame */
                      break;
            case ADJ: {
                ctx->sp = ctx->sp + vmm_get(ctx->pc) * INC_PTR;
                ctx->pc += INC_PTR;
            } /* add esp, <size> */
                      break;
            case LEV: {
                ctx->sp = ctx->bp;
                ctx->bp = (uint32_t)vmm_popstack(ctx->sp);
                if (ctx->bp != ctx->stacktrace.back()) {
                    error("invalid stack bp");
                }
                else {
                    ctx->stacktrace.pop_back();
                }
                ctx->pc = (uint32_t)vmm_popstack(ctx->sp);
                ctx->stacktrace_pc.pop_back();
                ctx->stacktrace_pc2.pop_back();
            } /* restore call frame and PC */
                      break;
            case LEA: {
                ctx->ax._i = ctx->bp + vmm_get(ctx->pc);
                ctx->pc += INC_PTR;
            } /* load address for arguments. */
                      break;
            case CASE:
                if (vmm_get(ctx->sp) == ctx->ax._i) {
                    ctx->sp += INC_PTR;
                    ctx->ax._i = 0; // 0 for same
                }
                else {
                    ctx->ax._i = 1;
                }
                break;
                // OPERATOR
            case OR:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) | ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) | ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) | ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) | ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case XOR:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) ^ ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) ^ ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) ^ ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) ^ ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case AND:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) & ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) & ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) & ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) & ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case EQ:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) == ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                case t_ptr:
                    ctx->ax._i = vmm_popstack<uint>(ctx->sp) == ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._i = vmm_popstack<int64>(ctx->sp) == ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._i = vmm_popstack<uint64>(ctx->sp) == ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._i = vmm_popstack<float>(ctx->sp) == ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._i = vmm_popstack<double>(ctx->sp) == ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case NE:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) != ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                case t_ptr:
                    ctx->ax._i = vmm_popstack<uint>(ctx->sp) != ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._i = vmm_popstack<int64>(ctx->sp) != ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._i = vmm_popstack<uint64>(ctx->sp) != ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._i = vmm_popstack<float>(ctx->sp) != ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._i = vmm_popstack<double>(ctx->sp) != ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case LT:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) < ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                case t_ptr:
                    ctx->ax._i = vmm_popstack<uint>(ctx->sp) < ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._i = vmm_popstack<int64>(ctx->sp) < ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._i = vmm_popstack<uint64>(ctx->sp) < ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._i = vmm_popstack<float>(ctx->sp) < ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._i = vmm_popstack<double>(ctx->sp) < ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case LE:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) <= ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                case t_ptr:
                    ctx->ax._i = vmm_popstack<uint>(ctx->sp) <= ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._i = vmm_popstack<int64>(ctx->sp) <= ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._i = vmm_popstack<uint64>(ctx->sp) <= ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._i = vmm_popstack<float>(ctx->sp) <= ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._i = vmm_popstack<double>(ctx->sp) <= ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case GT:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) > ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                case t_ptr:
                    ctx->ax._i = vmm_popstack<uint>(ctx->sp) > ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._i = vmm_popstack<int64>(ctx->sp) > ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._i = vmm_popstack<uint64>(ctx->sp) > ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._i = vmm_popstack<float>(ctx->sp) > ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._i = vmm_popstack<double>(ctx->sp) > ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case GE:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) >= ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                case t_ptr:
                    ctx->ax._i = vmm_popstack<uint>(ctx->sp) >= ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._i = vmm_popstack<int64>(ctx->sp) >= ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._i = vmm_popstack<uint64>(ctx->sp) >= ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._i = vmm_popstack<float>(ctx->sp) >= ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._i = vmm_popstack<double>(ctx->sp) >= ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case SHL:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) << ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) << ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) << ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) << ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case SHR:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) >> ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) >> ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) >> ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) >> ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case ADD:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) + ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) + ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) + ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) + ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._f = vmm_popstack<float>(ctx->sp) + ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._d = vmm_popstack<double>(ctx->sp) + ctx->ax._d;
                    break;
                case t_ptr:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) + (uint)ctx->ax._i;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case SUB:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) - ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) - ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) - ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) - ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._f = vmm_popstack<float>(ctx->sp) - ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._d = vmm_popstack<double>(ctx->sp) - ctx->ax._d;
                    break;
                case t_ptr:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) - (uint)ctx->ax._i;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case MUL:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) * ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) * ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) * ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) * ctx->ax._uq;
                    break;
                case t_float:
                    ctx->ax._f = vmm_popstack<float>(ctx->sp) * ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._d = vmm_popstack<double>(ctx->sp) * ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case DIV:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    if (ctx->ax._i == 0)
                        error("divide zero exception");
                    ctx->ax._i = vmm_popstack(ctx->sp) / ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    if (ctx->ax._ui == 0)
                        error("divide zero exception");
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) / ctx->ax._ui;
                    break;
                case t_long:
                    if (ctx->ax._q == 0)
                        error("divide zero exception");
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) / ctx->ax._q;
                    break;
                case t_ulong:
                    if (ctx->ax._uq == 0)
                        error("divide zero exception");
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) / ctx->ax._uq;
                    break;
                case t_float:
                    if (ctx->ax._f == 0)
                        error("divide zero exception");
                    ctx->ax._f = vmm_popstack<float>(ctx->sp) / ctx->ax._f;
                    break;
                case t_double:
                    if (ctx->ax._d == 0)
                        error("divide zero exception");
                    ctx->ax._d = vmm_popstack<double>(ctx->sp) / ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case MOD:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = vmm_popstack(ctx->sp) % ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = vmm_popstack<uint>(ctx->sp) % ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = vmm_popstack<int64>(ctx->sp) % ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = vmm_popstack<uint64>(ctx->sp) % ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case NEG:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = -ctx->ax._i;
                    break;
                case t_long:
                    ctx->ax._q = -ctx->ax._q;
                    break;
                case t_float:
                    ctx->ax._f = -ctx->ax._f;
                    break;
                case t_double:
                    ctx->ax._d = -ctx->ax._d;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case NOT:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = ~ctx->ax._i;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._ui = ~ctx->ax._ui;
                    break;
                case t_long:
                    ctx->ax._q = ~ctx->ax._q;
                    break;
                case t_ulong:
                    ctx->ax._uq = ~ctx->ax._uq;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case LNT:
                switch ((cast_t)vmm_get(ctx->pc)) {
                case t_char:
                case t_short:
                case t_int:
                    ctx->ax._i = ctx->ax._i ? 0 : 1;
                    break;
                case t_uchar:
                case t_ushort:
                case t_uint:
                    ctx->ax._i = ctx->ax._ui ? 0 : 1;
                    break;
                case t_long:
                    ctx->ax._i = ctx->ax._q ? 0 : 1;
                    break;
                case t_ulong:
                    ctx->ax._i = ctx->ax._uq ? 0 : 1;
                    break;
                case t_float:
                    ctx->ax._i = ctx->ax._f == 0.0f ? 0 : 1;
                    break;
                case t_double:
                    ctx->ax._i = ctx->ax._d == 0 ? 0 : 1;
                    break;
                case t_ptr:
                    ctx->ax._i = ctx->ax._ui ? 0 : 1;
                    break;
                default:
                    error("unsupport operator: " + INS_STRING((ins_t)op));
                    break;
                }
                ctx->pc += INC_PTR;
                break;
            case EXIT: {
#if LOG_SYSTEM
                ATLTRACE("[SYSTEM] PROC | Exit: PID= #%d, CODE= %d\n", ctx->id, ctx->ax._i);
#endif
#if LOG_VM
                {
                    CStringA s;
                    s.Format("[SYSTEM] PROC | Exit: PID= #%d, CODE= %d", ctx->id, ctx->ax._i);
                    logging(CString(s));
                }
#endif
                destroy(ctx->id);
                return;
            }
            case INTR: // 中断调用，以寄存器ax传参
                if (interrupt())
                    return;
                break;
            case CAST: // 类型转换，以寄存器ax传参
                cast();
                break;
            default: {
#if LOG_SYSTEM
                ATLTRACE("[SYSTEM] ERR  | AX: %08X BP: %08X SP: %08X PC: %08X\n", ctx->ax._i, ctx->bp, ctx->sp, ctx->pc);
                for (uint32_t j = ctx->sp; j < STACK_BASE + PAGE_SIZE; j += 4) {
                    ATLTRACE("[SYSTEM] ERR  | [%08X]> %08X\n", j, vmm_get<uint32_t>(j));
                }
                ATLTRACE("[SYSTEM] ERR  | unknown instruction: %d\n", op);
#endif
                error("unknown instruction");
            }
            }

#if LOG_STACK
            if (ctx->debug) {
                ATLTRACE("\n---------------- STACK BEGIN <<<< \n");
                ATLTRACE("AX: %08X BX: %08X BP: %08X SP: %08X PC: %08X\n", ctx->ax._u._1, ctx->ax._u._2, ctx->bp, ctx->sp, ctx->pc);
                auto k = 0;
                CStringA a;
                CStringA b;
                for (uint32_t j = ctx->sp; j < STACK_BASE + PAGE_SIZE; j += 4, ++k) {
                    b.Format("[%08X]> %08X", j, vmm_get<uint32_t>(j)); a += b;
                    if (k % 4 == 3) {
                        a += "\n";
                        ATLTRACE(a.GetBuffer(0));
                        a = "";
                    }
                    else
                        a += "  |  ";
                }
                if (!a.IsEmpty())
                    ATLTRACE(a.GetBuffer(0));
                if (k % 4 != 0)
                    ATLTRACE("\n");
                ATLTRACE("---------------- STACK END >>>>\n\n");
            }
            if (ctx->debug) {
                log << std::endl << "---------------- STACK BEGIN <<<< " << std::endl;
                log << std::endl << source() << std::endl;
                std::stringstream a;
                CStringA b;
                b.Format("AX: %08X BX: %08X BP: %08X SP: %08X PC: %08X", ctx->ax._u._1, ctx->ax._u._2, ctx->bp, ctx->sp, ctx->pc);
                log << b.GetBuffer(0) << std::endl;
                auto k = 0;
                for (uint32_t j = ctx->sp; j < STACK_BASE + PAGE_SIZE; j += 4, ++k) {
                    b.Format("[%08X]> %08X", j, vmm_get<uint32_t>(j)); a << b.GetBuffer(0);
                    if (k % 4 == 3) {
                        a << std::endl;
                        log << a.str();
                        a.str("");
                    }
                    else
                        a << "  |  ";
                }
                if (!a.str().empty())
                    log << a.str();
                if (k % 4 != 0)
                    log << std::endl;
                log << "---------------- STACK END >>>>" << std::endl << std::endl;
            }
            if (!(ctx->sp & STACK_BASE)) {
                ctx->pc = old_pc;
                log.close();
                error("RUNTIME ERROR: invalid sp");
            }
            if (ctx->bp && !(ctx->bp & STACK_BASE)) {
                ctx->pc = old_pc;
                log.close();
                error("RUNTIME ERROR: invalid bp");
            }
#endif
        }
    }

    string_t cvm::source() const
    {
        return source(ctx->pc);
    }

    string_t cvm::source(uint pc) const
    {
        auto pe = (PE*)ctx->file.data();
        auto pdb = (PDB*)(&pe->data + pe->data_len + pe->text_len);
        auto len = pdb->code_len;
        auto addr = (PDB_ADDR*)& pdb->data;
        auto code = (char*)(addr + len);
        pc = (pc & SEGMENT_MASK) / INC_PTR;
        uint idx = 0;
        auto found = false;
        for (uint i = 0; i < len; i++, addr++) {
            if (addr->idx > pc) {
                idx = (addr - 1)->addr;
                found = true;
                break;
            }
        }
        if (!found) {
            idx = (addr - 1)->addr;
        }
        if (idx < pe->pdb_len) {
            return code + idx;
        }
        return code + ((PDB_ADDR*)& pdb->data)->addr;
    }

    string_t cvm::get_func_info(int pc) const
    {
        pc = (pc & SEGMENT_MASK) / INC_PTR;
        auto f = ctx->stacktrace_dbg.find(pc);
        if (f != ctx->stacktrace_dbg.end())
            return f->second;
        return "UNKNOWN";
    }

    void cvm::error(const string_t& str) const {
#if REPORT_ERROR
        {
            std::ofstream log(REPORT_ERROR_FILE, std::ios::app | std::ios::out);
            log << std::endl << "---------------- STACK BEGIN <<<< " << std::endl;
            CStringA a, b;
            a.Format("AX: %08X BX: %08X BP: %08X SP: %08X PC: %08X", ctx->ax._u._1, ctx->ax._u._2, ctx->bp, ctx->sp, ctx->pc);
            log << a.GetBuffer(0) << std::endl;
            a = "";
            auto k = 0;
            if (ctx->sp & STACK_BASE) {
                for (uint32_t j = ctx->sp; j < STACK_BASE + PAGE_SIZE; j += 4, ++k) {
                    if (vmm_valid<uint32_t>(j)) {
                        b.Format("[%08X]> %08X", j, vmm_get<uint32_t>(j)); a += b;
                    }
                    else {
                        b.Format("[%08X]> ????????", j); a += b;
                    }
                    if (k % 4 == 3) {
                        a += "\n";
                        log << (a.GetBuffer(0));
                        a = "";
                    }
                    else
                        a += "  |  ";
                }
            }
            if (k % 4 != 0)
                log << std::endl;
            log << "---------------- STACK END >>>>" << std::endl << std::endl;
            log << get_stacktrace() << std::endl << std::endl;
            log << (str + ", PATH: " + ctx->path + ", SOURCE: " + source()) << std::endl;
        }
#endif
        throw cexception(ex_vm, str + ", PATH: " + ctx->path + ", SOURCE: " + source());
    }

    int cvm::load(const string_t& path, const std::vector<byte>& file, const std::vector<string_t>& _args) {
        auto old_ctx = ctx;
        new_pid();
        ctx->file = file;
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] PROC | Create: PID= #%d\n", ctx->id);
#endif
#if LOG_VM
        {
            CStringA s; s.Format("[SYSTEM] PROC | Create: PID= #%d\n", ctx->id);
            cvm::global_state.log_info.push_back(s.GetBuffer(0));
            logging(CString(s));
        }
#endif
        PE* pe = (PE*)file.data();
        // TODO: VALID PE FILE
        uint32_t pa;
        ctx->poolsize = PAGE_SIZE;
        ctx->mask = 0;
        ctx->entry = pe->entry;
        ctx->stack = STACK_BASE | ctx->mask;
        ctx->data = DATA_BASE | ctx->mask;
        ctx->base = USER_BASE | ctx->mask;
        ctx->heap = HEAP_BASE | ctx->mask;
        ctx->pool = std::make_unique<cmem>(this);
        ctx->flag |= CTX_KERNEL;
        ctx->state = CTS_RUNNING;
        ctx->path = path;
        auto args = _args;
        {
            if (args.empty()) {
                args.push_back(path);
            }
            else {
                args[0] = path;
            }
            if (args.size() < 2) {
                ctx->cmd = ctx->path;
            }
            else {
                auto arr = args;
                arr.erase(arr.begin());
                std::stringstream ss;
                ss << ctx->path << " ";
                std::copy(arr.begin(), arr.end(), std::ostream_iterator<string_t>(ss, " "));
                auto a = ss.str();
                if (a.empty())
                    a.erase(a.begin() + a.size() - 1);
                ctx->cmd = ss.str();
            }
        }
        /* 映射4KB的代码空间 */
        {
            auto size = PAGE_SIZE / sizeof(int);
            auto text_size = pe->text_len / sizeof(int);
            auto text_start = (uint32_t*)(&pe->data + pe->data_len);
            for (uint32_t i = 0, start = 0; start < text_size; ++i, start += size) {
                auto new_page = (uint32_t)pmm_alloc();
                ctx->text_mem.push_back(new_page);
                vmm_map(ctx->base + PAGE_SIZE * i, new_page); // 用户代码空间
                if (vmm_ismap(ctx->base + PAGE_SIZE * i, &pa)) {
                    auto s = start + size > text_size ? (text_size & (size - 1)) : size;
                    for (uint32_t j = 0; j < s; ++j) {
                        *((uint32_t*)pa + j) = (uint)text_start[start + j];
#if 0
                        ATLTRACE("[%p]> [%08X] %08X\n", (void*)((int*)pa + j), ctx->base + PAGE_SIZE * i + j * 4, vmm_get<uint32_t>(ctx->base + PAGE_SIZE * i + j * 4));
#endif
                    }
                }
            }
        }
        /* 映射4KB的数据空间 */
        {
            auto size = PAGE_SIZE;
            auto data_size = pe->data_len;
            auto data_start = (char*)& pe->data;
            for (uint32_t i = 0, start = 0; start < data_size; ++i, start += size) {
                auto new_page = (uint32_t)pmm_alloc();
                ctx->data_mem.push_back(new_page);
                vmm_map(ctx->data + PAGE_SIZE * i, new_page); // 用户数据空间
                if (vmm_ismap(ctx->data + PAGE_SIZE * i, &pa)) {
                    auto s = start + size > data_size ? ((sint)data_size & (size - 1)) : size;
                    for (auto j = 0; j < s; ++j) {
                        *((char*)pa + j) = data_start[start + j];
#if 0
                        ATLTRACE("[%p]> [%08X] %d\n", (void*)((char*)pa + j), ctx->data + PAGE_SIZE * i + j, vmm_get<byte>(ctx->data + PAGE_SIZE * i + j));
#endif
                    }
                }
            }
        }
        /* 映射4KB的栈空间 */
        {
            auto new_page = (uint32_t)pmm_alloc();
            ctx->stack_mem.push_back(new_page);
            vmm_map(ctx->stack, new_page); // 用户栈空间
        }
        ctx->flag &= ~CTX_KERNEL;
        {
            ctx->stack = STACK_BASE;
            ctx->data = DATA_BASE;
            ctx->base = USER_BASE;
            ctx->heap = HEAP_BASE;
            ctx->sp = ctx->stack + ctx->poolsize; // 4KB / sizeof(int) = 1024
            ctx->pc = ctx->base | (ctx->entry * INC_PTR);
            ctx->ax._i = 0;
            ctx->bp = 0;

            auto _argc = args.size();
            vmm_pushstack(ctx->sp, EXIT);
            vmm_pushstack(ctx->sp, 4);
            vmm_pushstack(ctx->sp, PUSH);
            auto tmp = ctx->sp;
            auto argvs = vmm_malloc(_argc * INC_PTR);
            vmm_pushstack(ctx->sp, argvs);
            vmm_pushstack(ctx->sp, _argc);
            for (size_t i = 0; i < _argc; i++) {
                auto str = vmm_malloc(args[i].length() + 1);
                vmm_setstr(str, args[i]);
                vmm_set(argvs + INC_PTR * i, str);
            }
            vmm_pushstack(ctx->sp, tmp);
            ctx->stacktrace_pc.push_back((ctx->entry * INC_PTR) | ctx->base);
            ctx->stacktrace_pc2.push_back(0);
        }
        ctx->flag |= CTX_USER_MODE;
        if (old_ctx) {
            if ((old_ctx->flag & CTX_SERVICE) && ((old_ctx->flag & CTX_SERVICE_USER_SHELL) == 0))
                ctx->flag |= CTX_SERVICE;
            else
                ctx->flag |= CTX_FOREGROUND;
        }
        {
            auto pe = (PE*)ctx->file.data();
            auto pdb = (PDB*)(&pe->data + pe->data_len + pe->text_len + pe->pdb_len);
            auto len = pdb->code_len;
            auto addr = (PDB_ADDR*)& pdb->data;
            auto code = (char*)(addr + len);
            for (uint i = 0; i < len; i++, addr++) {
                ctx->stacktrace_dbg.insert(std::make_pair(addr->idx, code + addr->addr));
            }
        }
        ctx->debug = false;
        ctx->waiting_ms = 0;
        ctx->input_redirect = -1;
        ctx->output_redirect = -1;
        ctx->input_queue.clear();
        ctx->input_stop = false;
        available_tasks++;
        auto pid = ctx->id;
        ctx = old_ctx;
        return pid;
    }

    void cvm::destroy(int id) {
        auto old_ctx = ctx;
        ctx = tasks[id].get();
        {
            if (global_state.input_lock == ctx->id) {
                global_state.input_lock = -1;
                global_state.input_read_ptr = -1;
                global_state.input_content.clear();
                global_state.input_success = false;
                global_state.input_code = 0;
                cgui::singleton().reset_cmd();
            }
            if (set_cycle_id == ctx->id) {
                cgui::singleton().set_cycle(0);
                set_cycle_id = -1;
            }
            if (set_resize_id.find(ctx->id) != set_resize_id.end()) {
                set_resize_id.erase(ctx->id);
                if (set_resize_id.empty()) {
                    cgui::singleton().resize(0, 0);
                }
            }
            if (ctx->output_redirect != -1 && tasks[ctx->output_redirect] && tasks[ctx->output_redirect]->flag & CTX_VALID) {
                if (!ctx->input_queue.empty()) {
                    std::copy(ctx->input_queue.begin(), ctx->input_queue.end(),
                        std::back_inserter(tasks[ctx->output_redirect]->input_queue));
                    ctx->input_redirect = -1;
                }
                tasks[ctx->output_redirect]->input_stop = true;
                ctx->output_redirect = -1;
            }
        }
        if (!ctx->child.empty()) {
            ctx->state = CTS_ZOMBIE;
            ctx = old_ctx;
            return;
        }
#if LOG_SYSTEM
#if LOG_VM
        {
            CStringA s; s.Format("[SYSTEM] PROC | Destroy: PID= #%d, Code= %d\n", ctx->id, ctx->ax._i);
            cvm::global_state.log_info.push_back(s.GetBuffer(0));
            logging(CString(s));
        }
#endif
        ATLTRACE("[SYSTEM] PROC | Destroy: PID= #%d, Code= %d\n", ctx->id, ctx->ax._i);
#endif
        ctx->flag = 0;
        {
            PE* pe = (PE*)ctx->file.data();
            ctx->poolsize = PAGE_SIZE;
            ctx->mask = 0;
            ctx->entry = pe->entry;
            ctx->stack = STACK_BASE | ctx->mask;
            ctx->data = DATA_BASE | ctx->mask;
            ctx->base = USER_BASE | ctx->mask;
            ctx->heap = HEAP_BASE | ctx->mask;
            ctx->flag |= CTX_KERNEL;
            /* 映射4KB的代码空间 */
            {
                auto size = PAGE_SIZE / sizeof(int);
                auto text_size = pe->text_len / sizeof(int);
                for (uint32_t i = 0, start = 0; start < text_size; ++i, start += size) {
                    vmm_unmap(ctx->base + PAGE_SIZE * i); // 用户代码空间
                }
            }
            /* 映射4KB的数据空间 */
            {
                auto size = PAGE_SIZE;
                auto data_size = pe->data_len;
                for (uint32_t i = 0, start = 0; start < data_size; ++i, start += size) {
                    vmm_unmap(ctx->data + PAGE_SIZE * i); // 用户数据空间
                }
            }
            /* 映射4KB的栈空间 */
            vmm_unmap(ctx->stack); // 用户栈空间
            /* 映射16KB的堆空间 */
            {
                for (int i = 0; i < ctx->pool->page_size(); ++i) {
                    vmm_unmap(ctx->heap + PAGE_SIZE * i);
                }
            }
            ctx->child.clear();
            ctx->state = CTS_DEAD;
            ctx->file.clear();
            ctx->allocation.clear();
            ctx->pool.reset();
            ctx->flag = 0;
            auto handles = ctx->handles;
            for (auto& h : handles) {
                destroy_handle(h);
            }
            ctx->handles.clear();
            if (ctx->parent != -1) {
                auto& parent = *(tasks[ctx->parent].get());
                parent.child.erase(ctx->id);
                parent.exited_child.push_back(ctx->id);
                if (parent.state == CTS_ZOMBIE)
                    destroy(ctx->parent);
                else if (parent.state == CTS_WAIT)
                    parent.state = CTS_RUNNING;
                ctx->parent = -1;
            }
            ctx->data_mem.clear();
            ctx->text_mem.clear();
            ctx->stack_mem.clear();
            ctx->input_queue.clear();
            ctx->stacktrace.clear();
            ctx->stacktrace_pc.clear();
            ctx->stacktrace_pc2.clear();
            ctx->stacktrace_dbg.clear();
            ctx->exited_child.clear();
            ctx->ips = 0ULL;
            ctx->ips_disp = 0ULL;
            while (!ctx->sigs.empty())ctx->sigs.pop();
            {
                std::stringstream ss;
                ss << "/proc/" << ctx->id;
                fs.rm(ss.str());
            }
        }
        tasks[id].reset(nullptr);
        ctx = old_ctx;
        available_tasks--;
    }

    string_t get_args(const string_t& path, std::vector<string_t>& args) {
        std::stringstream ss(path);
        string_t temp;
        while (std::getline(ss, temp, ' ')) {
            args.push_back(temp);
        }
        return args.front();
    }

    string_t trim(string_t str) {
        string_t::size_type pos = str.find_last_not_of(' ');
        if (pos != string_t::npos) {
            str.erase(pos + 1);
            pos = str.find_first_not_of(' ');
            if (pos != string_t::npos) str.erase(0, pos);
        }
        else str.erase(str.begin(), str.end());
        return str;
    }

    int cvm::exec_file(const string_t& path) {
        if (path.empty())
            return -1;
        auto new_path = trim(path);
#if LOG_SYSTEM
#if LOG_VM
        {
            CStringA s; s.Format("[SYSTEM] PROC | Exec: Command= %s\n", new_path.data());
            cvm::global_state.log_info.push_back(s.GetBuffer(0));
            logging(CString(s));
        }
#endif
        ATLTRACE("[SYSTEM] PROC | Exec: Command= %s\n", new_path.data());
#endif
        std::vector<string_t> args;
        auto file = get_args(new_path, args);
        decltype(ctx->paths) pt(ctx->paths.size() + 1);
        pt[0] = fs.get_pwd();
        std::copy(ctx->paths.begin(), ctx->paths.end(), pt.begin() + 1);
        auto pid = cgui::singleton().compile(file, args, pt);
        if (pid >= 0) { // SUCCESS
            ctx->child.insert(pid);
            tasks[pid]->parent = ctx->id;
            tasks[pid]->paths = ctx->paths;
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] PROC | Exec: Parent= #%d, Child= #%d\n", ctx->id, pid);
#endif
#if LOG_VM
            {
                CStringA s; s.Format("[SYSTEM] PROC | Exec: Parent= #%d, Child= #%d\n", ctx->id, pid);
                cvm::global_state.log_info.push_back(s.GetBuffer(0));
                logging(CString(s));
            }
#endif
        }
        return pid;
    }

    int cvm::fork() {
        auto old_ctx = ctx;
        new_pid();
        ctx->file = old_ctx->file;
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] PROC | Fork: Parent= #%d, Child= #%d\n", old_ctx->id, ctx->id);
#endif
#if LOG_VM
        {
            CStringA s; s.Format("[SYSTEM] PROC | Fork: Parent= #%d, Child= #%d\n", old_ctx->id, ctx->id);
            cvm::global_state.log_info.push_back(s.GetBuffer(0));
            logging(CString(s));
        }
#endif
        PE* pe = (PE*)ctx->file.data();
        // TODO: VALID PE FILE
        uint32_t pa;
        ctx->poolsize = PAGE_SIZE;
        ctx->mask = 0;
        ctx->entry = old_ctx->entry;
        ctx->stack = STACK_BASE | ctx->mask;
        ctx->data = DATA_BASE | ctx->mask;
        ctx->base = USER_BASE | ctx->mask;
        ctx->heap = HEAP_BASE | ctx->mask;
        ctx->pool = std::make_unique<cmem>(this);
        ctx->flag |= CTX_KERNEL;
        ctx->state = CTS_RUNNING;
        ctx->path = old_ctx->path;
        old_ctx->child.insert(ctx->id);
        ctx->parent = old_ctx->id;
        ctx->paths = old_ctx->paths;
        ctx->cmd = old_ctx->cmd;
        /* 映射4KB的代码空间 */
        {
            auto size = PAGE_SIZE / sizeof(int);
            auto text_size = pe->text_len / sizeof(int);
            for (uint32_t i = 0, start = 0; start < text_size; ++i, start += size) {
                auto new_page = (uint32_t)pmm_alloc();
                std::copy((byte*)(old_ctx->text_mem[i]),
                    (byte*)(old_ctx->text_mem[i]) + PAGE_SIZE,
                    (byte*)new_page);
                ctx->text_mem.push_back(new_page);
                vmm_map(ctx->base + PAGE_SIZE * i, new_page); // 用户代码空间
                if (!vmm_ismap(ctx->base + PAGE_SIZE * i, &pa)) {
                    destroy(ctx->id);
                    error("fork: text segment copy failed");
                }
            }
        }
        /* 映射4KB的数据空间 */
        {
            auto size = PAGE_SIZE;
            auto data_size = pe->data_len;
            for (uint32_t i = 0, start = 0; start < data_size; ++i, start += size) {
                auto new_page = (uint32_t)pmm_alloc();
                ctx->data_mem.push_back(new_page);
                std::copy((byte*)(old_ctx->data_mem[i]),
                    (byte*)(old_ctx->data_mem[i]) + PAGE_SIZE,
                    (byte*)new_page);
                vmm_map(ctx->data + PAGE_SIZE * i, new_page); // 用户数据空间
                if (!vmm_ismap(ctx->data + PAGE_SIZE * i, &pa)) {
                    destroy(ctx->id);
                    error("fork: data segment copy failed");
                }
            }
        }
        /* 映射4KB的栈空间 */
        {
            auto new_page = (uint32_t)pmm_alloc();
            ctx->stack_mem.push_back(new_page);
            std::copy((byte*)(old_ctx->stack_mem[0]),
                (byte*)(old_ctx->stack_mem[0]) + PAGE_SIZE,
                (byte*)new_page);
            vmm_map(ctx->stack, new_page); // 用户栈空间
            if (!vmm_ismap(ctx->stack, &pa)) {
                destroy(ctx->id);
                error("fork: stack segment copy failed");
            }
        }
        /* 映射堆空间 */
        ctx->pool->copy_from(*old_ctx->pool);
        ctx->flag = old_ctx->flag;
        ctx->sp = old_ctx->sp;
        ctx->stack = old_ctx->stack;
        ctx->data = old_ctx->data;
        ctx->base = old_ctx->base;
        ctx->heap = old_ctx->heap;
        ctx->stacktrace = old_ctx->stacktrace;
        ctx->stacktrace_pc = old_ctx->stacktrace_pc;
        ctx->stacktrace_pc2 = old_ctx->stacktrace_pc2;
        ctx->stacktrace_dbg = old_ctx->stacktrace_dbg;
        ctx->sigs = old_ctx->sigs;
        ctx->pc = old_ctx->pc;
        ctx->ax._i = -1;
        ctx->bp = old_ctx->bp;
        ctx->debug = old_ctx->debug;
        ctx->waiting_ms = 0;
        ctx->input_redirect = -1;
        ctx->output_redirect = -1;
        ctx->input_stop = old_ctx->input_stop;
        ctx->handles = old_ctx->handles;
        ctx->exited_child.clear();
        for (auto& h : ctx->handles) {
            handles[h]->refs++;
        }
        available_tasks++;
        if (set_resize_id.find(old_ctx->id) != set_resize_id.end()) {
            set_resize_id.insert(ctx->id);
        }
        auto pid = ctx->id;
        ctx = old_ctx;
        return pid;
    }

    string_t cvm::get_stacktrace() const
    {
        static char sz[32];
        std::stringstream ss;
        auto L = (int)ctx->stacktrace_pc.size() - 1;
        for (auto i = L; i >= 0; i--) {
            const auto& s = ctx->stacktrace_pc[i];
            const auto& s2 = ctx->stacktrace_pc2[i];
            snprintf(sz, sizeof(sz), "[%08X]", s | ctx->mask);
            ss << sz << " " << get_func_info(s) << "   => " << source(s2) << std::endl;
        }
        return ss.str();
    }

    void cvm::map_page(uint32_t addr, uint32_t id) {
        uint32_t pa;
        auto va = (ctx->heap | ctx->mask) | (PAGE_SIZE * id);
        vmm_map(va, addr);
#if LOG_SYSTEM
        ATLTRACE("[SYSTEM] MEM  | Map: PA= %p, VA= %p\n", (void*)addr, (void*)va);
#endif
        if (!vmm_ismap(va, &pa)) {
            destroy(ctx->id);
            error("heap alloc: alloc page failed");
        }
    }

    void cvm::as_root(bool flag) {
        fs.as_root(flag);
    }

    bool cvm::exist_vfs(const string_t& path) const {
        return fs.exist_vfs(path);
    }

    bool cvm::read_vfs(const string_t& path, std::vector<byte>& data) const {
        return fs.read_vfs(path, data);
    }

    bool cvm::write_vfs(const string_t& path, const std::vector<byte>& data) {
        return fs.write_vfs(path, data);
    }

    vfs_stream_t cvm::stream_type(const string_t& path) const {
        if (path.substr(0, 4) == "/dev") {
            static string_t pat{ R"(/dev/([a-z_]+))" };
            static std::regex re(pat);
            std::smatch res;
            if (std::regex_match(path, res, re)) {
                auto op = res[1].str();
                if (op == "random") {
                    return fss_random;
                }
                if (op == "null") {
                    return fss_null;
                }
                if (op == "console") {
                    return fss_console;
                }
            }
        }
        else if (path.substr(0, 5) == "/http") {
            return fss_net;
        }
        else if (path.substr(0, 6) == "/music") {
            return fss_music;
        }
        else if (path.substr(0, 7) == "/server") {
            return fss_server;
        }
        return fss_none;
    }

    string_t limit_string(const string_t& s, uint len) {
        if (s.length() <= len) {
            return s;
        }
        else {
            return s.substr(0, __max(0, len - 3)) + "...";
        }
    }

    LPCTSTR get_ips_disp(uint64 ips) {
        static TCHAR _ipsf[32];
        if (ips < 1e3) {
            _snwprintf(_ipsf, sizeof(_ipsf) / sizeof(_ipsf[0]), L"%I64u", ips);
        }
        else if (ips < 1e6) {
            _snwprintf(_ipsf, sizeof(_ipsf) / sizeof(_ipsf[0]), L"%I64uK", ips / 1000ULL);
        }
        else if (ips < 1e9) {
            _snwprintf(_ipsf, sizeof(_ipsf) / sizeof(_ipsf[0]), L"%I64uM", ips / 1000000ULL);
        }
        else {
            _snwprintf(_ipsf, sizeof(_ipsf) / sizeof(_ipsf[0]), L"%I64uG", ips / 1000000000ULL);
        }
        return _ipsf;
    }

    CString cvm::get_disp(disp_t t) const
    {
        static TCHAR sz[256];
        std::wstringstream ss;
        if (t == D_PS) {
            ss << L"[STATE] [FLAG] [PID] [IPS] [COMMAND LINE]     [PAGE]" << std::endl;
            for (auto i = 0; i < TASK_NUM; ++i) {
                if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                    _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%7S  %04X   %4d %5s %-18S   %4d",
                        state_string(tasks[i]->state),
                        tasks[i]->flag,
                        i,
                        get_ips_disp(tasks[i]->ips_disp),
                        limit_string(tasks[i]->cmd, 18).c_str(),
                        tasks[i]->allocation.size() + tasks[i]->pgdir.size());
                    ss << sz << std::endl;
                }
            }
        }
        else if (t == D_HTOP) {
            auto root_id = -1;
            // deps[父进程] = [子进程集合]
            std::unordered_map<int, std::list<int>> deps;
            for (auto i = 0; i < TASK_NUM; ++i) {
                if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                    const auto& p = tasks[i]->parent;
                    if (p != -1) {
                        auto f = deps.find(p);
                        if (f == deps.end()) {
                            deps[p] = std::list<int>();
                        }
                        deps[p].push_back(i);
                    }
                    else {
                        root_id = i;
                    }
                }
            }
            std::bitset<TASK_NUM> visited;
            std::bitset<TASK_NUM> printed;
            int current = root_id;
            int level = 0;
            std::vector<int> lvls;
            // 多叉树的非递归前序遍历
            while (current != -1) {
                if (visited.test(current)) { // 访问过了，查看有无兄弟
                    auto parent = tasks[current]->parent;
                    if (deps.find(parent) != deps.end()) { // 还有未访问的兄弟
                        auto brother = deps[parent].front(); // 进入兄弟节点
                        deps[parent].pop_front(); // 除去此节点
                        if (deps[parent].empty())
                            deps.erase(parent);
                        current = brother;
                    }
                    else { // 没有兄弟（或兄弟访问完），回到父节点
                        current = parent;
                        lvls.pop_back();
                        level--;
                    }
                }
                else { // 没访问过，进入
                    if (!printed.test(current)) {
                        for (int i = 0; i < level; i++) {
                            if (level == i + 1) {
                                if (lvls[i] > 1) {
                                    ss << L"├—";
                                }
                                else if (lvls[i] == 1) {
                                    ss << L"└—";
                                }
                                else {
                                    ss << L"  ";
                                }
                            }
                            else {
                                if (lvls[i] > 1) {
                                    ss << L"│ ";
                                }
                                else {
                                    ss << L"  ";
                                }
                            }
                        }
                        // ss << std::setfill(L' ') << std::setw(level * 2LL) << "";
                        static TCHAR sz2[64];
                        const auto& I = tasks[current]->input_redirect;
                        const auto& O = tasks[current]->output_redirect;
                        const auto& Q = tasks[current]->input_queue.size();
                        if (I == -1 && O == -1)
                            sz2[0] = 0;
                        else if (I != -1 && O == -1)
                            _snwprintf(sz2, sizeof(sz2) / sizeof(sz2[0]), L"(I=%d,Q=%d) ", I, Q);
                        else if (I == -1 && O != -1)
                            _snwprintf(sz2, sizeof(sz2) / sizeof(sz2[0]), L"(O=%d,Q=%d) ", O, Q);
                        else
                            _snwprintf(sz2, sizeof(sz2) / sizeof(sz2[0]), L"(I=%d,O=%d,Q=%d) ", I, O, Q);
                        _snwprintf(sz, sizeof(sz2) / sizeof(sz2[0]), L"#%d %s%S", current, sz2, limit_string(tasks[current]->cmd, 30).c_str());
                        ss << sz << std::endl;
                        printed.set(current);
                    }
                    if (deps.find(current) != deps.end()) { // 有孩子，进入孩子
                        lvls.push_back((int)deps[current].size());
                        auto child = deps[current].front(); // 进入子节点
                        deps[current].pop_front(); // 除去此节点
                        if (deps[current].empty())
                            deps.erase(current);
                        current = child;
                        level++;
                    }
                    else { // 没有孩子（或孩子访问完），回到父节点
                        visited.set(current);
                        current = tasks[current]->parent;
                        if (current != -1) {
                            lvls.pop_back();
                            level--;
                        }
                    }
                }
            }
        }
        else if (t == D_HANDLE) {
            for (auto i = 0; i < HANDLE_NUM; ++i) {
                if (handles[i] && handles[i]->type != h_none) {
                    auto nm = CString(CStringA(limit_string(handles[i]->name, 30).c_str()));
                    _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%4d | %6S | %s", i, handle_typename(handles[i]->type).c_str(),
                        nm.GetBuffer(0));
                    ss << sz << std::endl;
                }
            }
        }
        else if (t == D_WINDOW) {
            auto tl = draw_bounds.TopLeft();
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"Cursor: %d,%d", global_state.mouse_x - tl.x, global_state.mouse_y - tl.y);
            ss << sz << std::endl << std::endl;
            for (const auto& w : wnds) {
                ss << w->to_string() << std::endl;
            }
        }
        else if (t == D_MEM) {
            std::wstringstream ss;
            {
                if (global_state.input_lock == -1)
                    _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9s", L"Input Lock:", L"None");
                else
                    _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"Input Lock:", global_state.input_lock);
                ss << sz << std::endl;
                _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9s", L"Input Single:", global_state.input_single ? L"Yes" : L"No");
                ss << sz << std::endl;
                std::string str;
                if (global_state.input_waiting_list.empty())
                    str = "Empty";
                else {
                    std::stringstream ss2;
                    std::copy(global_state.input_waiting_list.begin(), global_state.input_waiting_list.end(),
                        std::ostream_iterator<int>(ss2, ","));
                    str = ss2.str();
                    str.pop_back();
                }
                _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9S", L"Input Waiting:  ", str.c_str()); ss << sz << std::endl;
            }
            int mems = 0, pages = 0, heaps = 0, heaps_a = 0, kernel_pages = 0;
            for (auto i = 0; i < TASK_NUM; ++i) {
                if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                    mems += tasks[i]->pages.size();
                    pages += tasks[i]->allocation.size();
                    heaps += tasks[i]->pool->page_size();
                    heaps_a += tasks[i]->pool->available();
                    kernel_pages += tasks[i]->pgdir.size();
                }
            }
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"Memory Total:", mems * PAGE_SIZE); ss << sz << std::endl;
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"Heap Total:", heaps * PAGE_SIZE); ss << sz << std::endl;
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"Heap Using:", heaps * PAGE_SIZE - heaps_a); ss << sz << std::endl;
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"Heap Free:", heaps_a); ss << sz << std::endl;
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"Kernel Page:", kernel_pages); ss << sz << std::endl;
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9d", L"User Page:", pages); ss << sz << std::endl;
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%-18s %9I64u", L"File system:", fs.size()); ss << sz << std::endl;
            std::wstringstream guiss;
            {
                if (global_state.gui) {
                    if (global_state.ui) {
                        _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"(%d,%d) F=%d R=%d",
                            global_state.ui->get_width(),
                            global_state.ui->get_height(),
                            global_state.ui->set_fresh(-1),
                            global_state.ui->ready() ? 1 : 0);
                        guiss << sz << std::endl;
                    }
                    else
                        guiss << L"INIT" << std::endl;
                }
                else {
                    guiss << L"OFF" << std::endl;
                }
            }
            _snwprintf(sz, sizeof(sz) / sizeof(sz[0]), L"%s %23s", L"GUI: ", guiss.str().c_str()); ss << sz << std::endl;
            return CString(ss.str().c_str());
        }
        return CString(ss.str().c_str());
    }

    string_t cvm::stream_callback(const string_t& path) {
        static char sz[256];
        if (path.substr(0, 6) == "/proc/") {
            static string_t pat{ R"(/proc/(\d+)/([a-z_]+))" };
            static std::regex re(pat);
            std::smatch res;
            if (std::regex_match(path, res, re)) {
                auto id = std::stoi(res[1].str());
                if (!(tasks[id] && tasks[id]->flag & CTX_VALID)) {
                    return "\033FFFF00000\033[ERROR] Invalid pid.\033S4\033";
                }
                const auto& op = res[2].str();
                if (op == "exe") {
                    return tasks[id]->path;
                }
                else if (op == "parent") {
                    snprintf(sz, sizeof(sz), "%d", tasks[id]->parent);
                    return sz;
                }
                else if (op == "heap_size") {
                    snprintf(sz, sizeof(sz), "%d", tasks[id]->pool->page_size());
                    return sz;
                }
                else if (op == "path") {
                    std::stringstream ss;
                    std::copy(tasks[id]->paths.begin(), tasks[id]->paths.end(),
                        std::ostream_iterator<string_t>(ss, "\n"));
                    return ss.str();
                }
                else if (op == "dep") {
                    auto deps = cgui::singleton().get_dep(tasks[id]->path);
                    std::stringstream ss;
                    std::copy(deps.begin(), deps.end(),
                        std::ostream_iterator<string_t>(ss, "\n"));
                    return ss.str();
                }
                else if (op == "cmd") {
                    return tasks[id]->cmd;
                }
            }
        }
        else if (path.substr(0, 5) == "/sys/") {
            static string_t pat{ R"(/sys/([a-z_]+))" };
            static std::regex re(pat);
            std::smatch res;
            if (std::regex_match(path, res, re)) {
                const auto& op = res[1].str();
                if (op == "ps") {
                    std::stringstream ss;
                    ss << "\033FFFA0A0A0\033[STATE] \033S4\033[FLAG] [PID] [PPID]\033FFFB3B920\033 [COMMAND LINE]     \033FFF51C2A8\033[PAGE]\033S4\033" << std::endl;
                    for (auto i = 0; i < TASK_NUM; ++i) {
                        if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                            snprintf(sz, sizeof(sz), "\033FFFA0A0A0\033%7s \033S4\033 %04X   %4d   %4d \033FFFB3B920\033%-18s \033FFF51C2A8\033  %4d\033S4\033",
                                state_string(tasks[i]->state),
                                tasks[i]->flag,
                                i,
                                tasks[i]->parent,
                                limit_string(tasks[i]->cmd, 18).c_str(),
                                tasks[i]->allocation.size());
                            ss << sz << std::endl;
                        }
                    }
                    return ss.str();
                }
                else if (op == "mem") {
                    std::stringstream ss;
                    int mems = 0, pages = 0, heaps = 0, heaps_a = 0, kernel_pages = 0;
                    for (auto i = 0; i < TASK_NUM; ++i) {
                        if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                            mems += tasks[i]->pages.size();
                            pages += tasks[i]->allocation.size();
                            heaps += tasks[i]->pool->page_size();
                            heaps_a += tasks[i]->pool->available();
                            kernel_pages += tasks[i]->pgdir.size();
                        }
                    }
                    snprintf(sz, sizeof(sz), "%-18s %d", "Memory Total:", mems * PAGE_SIZE); ss << sz << std::endl;
                    snprintf(sz, sizeof(sz), "%-18s %d", "Heap Total:", heaps * PAGE_SIZE); ss << sz << std::endl;
                    snprintf(sz, sizeof(sz), "%-18s %d", "Heap Using:", heaps * PAGE_SIZE - heaps_a); ss << sz << std::endl;
                    snprintf(sz, sizeof(sz), "%-18s %d", "Heap Free:", heaps_a); ss << sz << std::endl;
                    snprintf(sz, sizeof(sz), "%-18s %d", "Kernel Page:", kernel_pages); ss << sz << std::endl;
                    snprintf(sz, sizeof(sz), "%-18s %d", "User Page:", pages); ss << sz << std::endl;
                    return ss.str();
                }
                else if (op == "time") {
                    std::stringstream ss;
                    auto n = std::chrono::system_clock::now();
                    auto m = n.time_since_epoch();
                    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(m).count();
                    auto const msecs = diff % 1000;
                    std::time_t t = std::chrono::system_clock::to_time_t(n);
                    ss << std::put_time(std::localtime(&t), "%Y/%m/%d %H:%M:%S") << "." << std::setw(3) << std::setfill('0') << msecs;
                    return ss.str();
                }
                else if (op == "uptime") {
                    std::stringstream ss;
                    auto n = std::chrono::system_clock::now();
                    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(n - global_state.now).count();
                    auto const msecs = diff % 1000;
                    auto const days = diff / 86400000;
                    auto nn = std::chrono::system_clock::to_time_t(n);
                    static auto const ll = std::chrono::system_clock::to_time_t(global_state.now);
                    auto t = std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::from_time_t(nn - ll));
                    ss << days << " " << (days > 0 ? "days" : "day") << ", ";
                    ss << std::put_time(std::gmtime(&t), "%H:%M:%S") << "." << msecs;
                    return ss.str();
                }
                else if (op == "pstree") {
                    auto disp = get_disp(D_HTOP);
                    CStringA d(disp);
                    return d.GetBuffer(0);
                }
            }
        }
        else if (path.substr(0, 5) == "/log/") {
            static string_t pat{ R"(/log/([a-z_]+))" };
            static std::regex re(pat);
            std::smatch res;
            if (std::regex_match(path, res, re)) {
                const auto& op = res[1].str();
                if (op == "info") {
                    std::stringstream ss;
                    for (auto& L : global_state.log_info) {
                        ss << L;
                    }
                    return ss.str();
                }
                else if (op == "err") {
                    std::stringstream ss;
                    for (auto& L : global_state.log_err) {
                        ss << L;
                    }
                    return ss.str();
                }
            }
        }
        else if (path.substr(0, 8) == "/handle/") {
            static string_t pat{ R"(/handle/(\d+)/([a-z_]+)(/.*)?)" };
            static std::regex re(pat);
            std::smatch res;
            if (std::regex_match(path, res, re)) {
                auto id = std::stoi(res[1].str());
                if (handles[id] && handles[id]->type == h_none) {
                    return "\033FFFF00000\033[ERROR] Invalid handle.\033S4\033";
                }
                const auto& op = res[2].str();
                if (op == "type") {
                    return handle_typename(handles[id]->type);
                }
                else if (op == "name") {
                    return handles[id]->name;
                }
                else {
                    if (handles[id] && handles[id]->type == h_window) {
                        return handles[id]->data.cwnd->handle_fs(res[3].str());
                    }
                    return "\033FFFF00000\033[ERROR] Invalid handle.\033S4\033";
                }
            }
        }
        else if (path.substr(0, 5) == "/dev/") {
            static string_t pat{ R"(/dev/([a-z_]+))" };
            static std::regex re(pat);
            std::smatch res;
            if (std::regex_match(path, res, re)) {
                const auto& op = res[1].str();
                if (op == "uuid") {
                    GUID guid;
                    if (S_OK == CoCreateGuid(&guid)) {
                        snprintf(sz, sizeof(sz),
                            "%08X-%04X-%04x-%02X%02X-%02X%02X%02X%02X%02X%02X",
                            guid.Data1, guid.Data2, guid.Data3,
                            guid.Data4[0], guid.Data4[1], guid.Data4[2],
                            guid.Data4[3], guid.Data4[4], guid.Data4[5],
                            guid.Data4[6], guid.Data4[7]);
                        return sz;
                    }
                }
                else if (op == "debug") {
#if _DEBUG
                    return "1";
#else
                    return "0";
#endif
                }
            }
        }
        return "\033FFFF00000\033[ERROR] File not exists.\033S4\033";
    }

    vfs_node_dec* cvm::stream_create(const vfs_mod_query* mod, vfs_stream_t type, const string_t& path) {
        if (type == fss_net) {
            return vfs_node_stream_net::create(mod, type, this, path);
        }
        if (type == fss_console) {
            return vfs_node_stream_write::create(mod, type, this);
        }
        if (type == fss_music) {
            return vfs_node_stream_music::create(mod, type, this, path);
        }
        if (type == fss_window) {
            return vfs_node_stream_window::create(mod, type, this, path);
        }
        if (type == fss_server) {
            return vfs_node_stream_server::create(mod, type, this, path);
        }
        if (type != fss_none) {
            return vfs_node_stream::create(mod, type, this);
        }
        error("invalid vfs stream");
        return nullptr;
    }

    int cvm::stream_index(vfs_stream_t type) {
        switch (type) {
        case fss_random: {
            static std::default_random_engine e(((uint32_t)time(nullptr)) % (UINT32_MAX));
            static std::uniform_int_distribution<int> u(0, 255);
            return u(e);
        }
        case fss_null: {
            return 0;
        }
        default:
            break;
        }
        error("invalid stream type");
        return -1;
    }

    string_t cvm::stream_net(vfs_stream_t type, const string_t& path, bool& post, string_t& postfield, bool& bin) {
        switch (type) {
        case fss_net: {
            return net.http_get(path, post, postfield, bin);
        }
        default:
            break;
        }
        error("invalid stream type");
        return "";
    }

    int cvm::stream_write(vfs_stream_t type, byte c)
    {
        if (type == fss_console) {
            if (global_state.input_lock == -1) {
                cgui::singleton().put_char((char)c);
                return 0;
            }
        }
        else if (type == fss_null) {
            return 0;
        }
        return -1;
    }

    bool cvm::stream_path(const string_t& path, std::vector<byte>& data)
    {
        if (fs.read_vfs(path, data)) {
            return true;
        }
        auto p = fs.get_realpath(path);
        std::ifstream ifs(p, std::ios::binary);
        if (ifs) {
            auto p = ifs.rdbuf();
            auto size = p->pubseekoff(0, std::ios::end, std::ios::in);
            p->pubseekpos(0, std::ios::in);
            data.resize((size_t)size);
            p->sgetn((char*)data.data(), size);
            return true;
        }
        return false;
    }

    cwindow* cvm::stream_getwnd(int handle)
    {
        if (handle < 0 || handle >= HANDLE_NUM)
            error("stream_getwnd: invalid handle");
        if (handles[handle]->type != h_window) {
            error("stream_getwnd: invalid type");
        }
        return handles[handle]->data.cwnd;
    }

    const char* cvm::state_string(cvm::ctx_state_t type) {
        assert(type >= CTS_RUNNING && type < CTS_DEAD);
        switch (type) {
        case CTS_RUNNING:
            return "RUNNING";
        case CTS_WAIT:
            return "WAITING";
        case CTS_ZOMBIE:
            return "ZOMBIE";
        case CTS_DEAD:
            return "DEAD";
        default:
            return "[ERROR] Invalid state string";
        }
    }

    int cvm::new_pid() {
        if (available_tasks >= TASK_NUM) {
            error("max process num!");
        }
        auto end = TASK_NUM + pids;
        for (int i = pids; i < end; ++i) {
            auto j = i % TASK_NUM;
            if (!tasks[j]) {
                tasks[j] = std::make_unique<context_t>();
                tasks[j]->flag |= CTX_VALID;
                ctx = tasks[j].get();
                pids = (j + 1) % TASK_NUM;
                ctx->id = j;
                {
                    std::stringstream ss;
                    ss << "/proc/" << j;
                    auto dir = ss.str();
                    fs.as_root(true);
                    if (fs.mkdir(dir) == 0) { // '/proc/[pid]'
                        static std::vector<string_t> ps =
                        { "exe", "parent", "heap_size", "path", "dep", "cmd" };
                        dir += "/";
                        for (auto& _ps : ps) {
                            ss.str("");
                            ss << dir << _ps;
                            fs.func(ss.str(), this);
                        }
                    }
                    fs.as_root(false);
                }
                return j;
            }
        }
        error("max process num!");
        return -1;
    }

    int cvm::new_handle(cvm::handle_type type) {
        if (available_handles >= HANDLE_NUM)
            error("max handle num!");
        auto end = HANDLE_NUM + handle_ids;
        for (int i = handle_ids; i < end; ++i) {
            auto j = i % HANDLE_NUM;
            if (!handles[j]) {
                handles[j] = std::make_unique<handle_t>();
                handles[j]->type = type;
                handles[j]->refs = 1;
                handle_ids = (j + 1) % HANDLE_NUM;
                available_handles++;
                ctx->handles.insert(j);
                {
                    std::stringstream ss;
                    ss << "/handle/" << j;
                    auto dir = ss.str();
                    fs.as_root(true);
                    if (fs.mkdir(dir) == 0) { // '/handle/[hid]'
                        std::vector<string_t> ps = { "type", "name" };
                        if (type == h_window) {
                            ps.push_back("comctl");
                        }
                        dir += "/";
                        for (auto& _ps : ps) {
                            ss.str("");
                            ss << dir << _ps;
                            fs.func(ss.str(), this);
                        }
                        if (type == h_window) {
                            ss.str("");
                            ss << dir << "message";
                            fs.magic(ss.str(), this, fss_window);
                        }
                    }
                    fs.as_root(false);
                }
                return j;
            }
        }
        error("max handle num!");
        return -1;
    }

    string_t cvm::handle_typename(handle_type t) const
    {
        static std::tuple<handle_type, string_t> handle_typename_list[] = {
            std::make_tuple(h_none, "none"),
            std::make_tuple(h_file, "file"),
            std::make_tuple(h_window, "window"),
            std::make_tuple(h__end, "end"),
        };
        assert(t >= h_none && t < h__end);
        return std::get<1>(handle_typename_list[t]);
    }

    void cvm::destroy_handle(int handle) {
        if (handle < 0 || handle >= HANDLE_NUM)
            error("invalid handle");
        if (handles[handle] && handles[handle]->type != h_none) {
            ctx->handles.erase(handle);
            if (handles[handle]->refs > 1) {
                handles[handle]->refs--;
                return;
            }
            handles[handle]->refs = 0;
            auto h = handles[handle].get();
            if (h->type == h_file) {
                auto dec = h->data.file;
                dec->remove_handle(handle);
                delete h->data.file;
            }
            else if (h->type == h_window) {
                wnds.erase(std::remove(wnds.begin(), wnds.end(), h->data.cwnd), wnds.end());
                auto dec = h->data.cwnd;
                delete dec;
            }
            handles[handle].reset(nullptr);
            available_handles--;
            {
                std::stringstream ss;
                ss << "/handle/" << handle;
                fs.rm(ss.str());
            }
        }
        else {
            error("destroy handle failed!");
        }
    }

    int cvm::post_data(int h, int code, int param1, int param2)
    {
        if (h >= 0 && h < HANDLE_NUM) {
            if (!handles[h] || handles[h]->type != h_window) {
                return -1;
            }
            auto wnd = handles[h]->data.cwnd;
            wnd->post_data(code, param1, param2);
            if (code == WM_SETFOCUS) {
                if (wnds.size() > 1 && wnds.back() != wnd) {
                    for (size_t i = 0; i < wnds.size(); i++) {
                        if (wnds[i] == wnd) {
                            std::swap(wnds[i], wnds.back());
                        }
                    }
                }
            }
            return 0;
        }
        return -1;
    }

    char* cvm::output_fmt(int id) const {
        static char str[256];
        switch (id) {
        case 1:
            snprintf(str, sizeof(str), "%d", ctx->ax._i);
            break;
        case 2:
            snprintf(str, sizeof(str), "%p", ctx->ax._p);
            break;
        case 4:
            snprintf(str, sizeof(str), "%f", ctx->ax._f);
            break;
        case 6:
            snprintf(str, sizeof(str), "%f", ctx->ax._d);
            break;
        case 7:
            snprintf(str, sizeof(str), "%lld", ctx->ax._q);
            break;
        case 9:
            snprintf(str, sizeof(str), "%llu", ctx->ax._uq);
            break;
        default:
            snprintf(str, sizeof(str), "[Invalid format]");
            break;
        }
        return str;
    }

    int cvm::output(int id) {
        if (ctx->output_redirect != -1) {
            if (tasks[ctx->output_redirect]) {
                if (id == 0) {
                    tasks[ctx->output_redirect]->input_queue.push_back(ctx->ax._c);
                }
                else {
                    auto s = output_fmt(id);
                    while (*s) tasks[ctx->output_redirect]->input_queue.push_back(*s++);
                }
            }
            else {
                ctx->output_redirect = -1;
            }
        }
        else if (global_state.input_lock == -1) {
            if (id == 0) {
                cgui::singleton().put_char(ctx->ax._c);
            }
            else {
                auto s = output_fmt(id);
                while (*s) cgui::singleton().put_char(*s++);
            }
        }
        else {
            if (global_state.input_lock != ctx->id)
                global_state.input_waiting_list.push_back(ctx->id);
            ctx->state = CTS_WAIT;
            ctx->pc -= INC_PTR;
            return 1;
        }
        return 0;
    }

    void cvm::cast() {
        switch (vmm_get(ctx->pc)) {
        case 1:
            ctx->ax._ui = (uint)ctx->ax._i;
            break;
        case 2:
            ctx->ax._i = (int)ctx->ax._ui;
            break;
        case 3:
            ctx->ax._uq = (uint64)ctx->ax._i;
            break;
        case 4:
            ctx->ax._q = (int64)ctx->ax._ui;
            break;
        case 5:
            ctx->ax._ui = (uint)ctx->ax._q;
            break;
        case 6:
            ctx->ax._i = (int)ctx->ax._uq;
            break;
        case 7:
            ctx->ax._uq = (uint64)ctx->ax._q;
            break;
        case 8:
            ctx->ax._q = (int64)ctx->ax._uq;
            break;
        case 9:
            ctx->ax._q = (int64)ctx->ax._i;
            break;
        case 10:
            ctx->ax._uq = (uint64)ctx->ax._ui;
            break;
        case 11:
            ctx->ax._i = (int)ctx->ax._q;
            break;
        case 12:
            ctx->ax._ui = (uint)ctx->ax._uq;
            break;
        case 13:
            ctx->ax._q = (int64)ctx->ax._c;
            break;
        case 14:
            ctx->ax._q = (int64)ctx->ax._s;
            break;
        case 20:
            ctx->ax._f = (float)ctx->ax._ui;
            break;
        case 21:
            ctx->ax._f = (float)ctx->ax._i;
            break;
        case 22:
            ctx->ax._f = (float)ctx->ax._uq;
            break;
        case 23:
            ctx->ax._f = (float)ctx->ax._q;
            break;
        case 24:
            ctx->ax._ui = (uint)ctx->ax._f;
            break;
        case 25:
            ctx->ax._i = (int)ctx->ax._f;
            break;
        case 26:
            ctx->ax._uq = (uint64)ctx->ax._f;
            break;
        case 27:
            ctx->ax._q = (int64)ctx->ax._f;
            break;
        case 28:
            ctx->ax._d = (double)ctx->ax._f;
            break;
        case 30:
            ctx->ax._d = (double)ctx->ax._ui;
            break;
        case 31:
            ctx->ax._d = (double)ctx->ax._i;
            break;
        case 32:
            ctx->ax._d = (double)ctx->ax._uq;
            break;
        case 33:
            ctx->ax._d = (double)ctx->ax._q;
            break;
        case 34:
            ctx->ax._ui = (uint)ctx->ax._d;
            break;
        case 35:
            ctx->ax._i = (int)ctx->ax._d;
            break;
        case 36:
            ctx->ax._uq = (uint64)ctx->ax._d;
            break;
        case 37:
            ctx->ax._q = (int64)ctx->ax._d;
            break;
        case 38:
            ctx->ax._f = (float)ctx->ax._d;
            break;
        default:
            error("unsupported cast");
            break;
        }
        ctx->pc += INC_PTR;
    }

    bool cvm::math(int id) {
        switch (id) {
        case 201:
            ctx->ax._d = std::sqrt(std::abs(ctx->ax._d));
            break;
        case 202:
        {
            static std::default_random_engine e((uint32_t)time(nullptr));
            std::uniform_int_distribution<int> dist{ 0, std::abs(ctx->ax._i) };
            ctx->ax._i = dist(e);
        }
        break;
        case 203:
        {
            struct __math_2d_struct__ {
                double x, y;
            };
            auto s = vmm_get<__math_2d_struct__>(ctx->ax._ui);
            ctx->ax._d = atan2(s.x, s.y);
        }
        break;
        case 204:
            ctx->ax._d = fabs(ctx->ax._d);
        break;
        case 205:
        {
            struct __math_2d_struct__ {
                double x, y;
            };
            auto s = vmm_get<__math_2d_struct__>(ctx->ax._ui);
            ctx->ax._d = fmod(s.x, s.y);
        }
        break;
        case 206:
        {
            struct __math_2d_struct__ {
                double x, y;
            };
            auto s = vmm_get<__math_2d_struct__>(ctx->ax._ui);
            ctx->ax._d = pow(s.x, s.y);
        }
        break;
        case 207:
            ctx->ax._d = sin(ctx->ax._d);
            break;
        case 208:
            ctx->ax._d = cos(ctx->ax._d);
            break;
        default:
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] ERR  | unknown interrupt: %d\n", ctx->ax._i);
#endif
            error("unknown interrupt");
            break;
        }
        ctx->pc += INC_PTR;
        return false;
    }

    void exec_lua(evutil_socket_t fd, short event, void* arg)
    {
        auto s = (string_t*)arg;
        auto L = window->get_state();
        luaL_loadstring(L, s->c_str());
        lua_call(L, 0, 0);
        delete s;
    }

    bool cvm::gui(int id) {
        switch (id) {
        case 301:
        {
            if (ctx->ax._i == 1 || ctx->ax._i == 0)
            {
                global_state.gui = ctx->ax._i != 0;
                if (ctx->ax._i == 1) {
                    if (!global_state.ui->ready()) {
                        ctx->pc -= INC_PTR;
                        return true;
                    }
                }
                else {
                    if (global_state.ui->ready()) {
                        ctx->pc -= INC_PTR;
                        return true;
                    }
                }
            }
            else if (ctx->ax._i == 2 || ctx->ax._i == 3)
            {
                if (ctx->ax._i == 2)
                    ctx->ax._i = global_state.ui->get_width();
                else
                    ctx->ax._i = global_state.ui->get_height();
            }
            else if (ctx->ax._i >= 11 && ctx->ax._i <= 14)
            {
                if (draw_bounds.IsRectEmpty()) {
                    ctx->pc -= INC_PTR;
                    return true;
                }
                switch (ctx->ax._i) {
                case 11: ctx->ax._i = draw_bounds.left; break;
                case 12: ctx->ax._i = draw_bounds.top; break;
                case 13: ctx->ax._i = draw_bounds.right; break;
                case 14: ctx->ax._i = draw_bounds.bottom; break;
                }
            }
            else if (ctx->ax._i >= 4 && ctx->ax._i <= 7)
            {
                if (ctx->ax._i == 7) {
                    if (global_state.ui->set_fresh(-1) != 0) {
                        ctx->pc -= INC_PTR;
                        return true;
                    }
                }
                else {
                    global_state.ui->set_fresh(ctx->ax._i - 4);
                }
            }
            else if (ctx->ax._i == 8)
            {
                if (global_state.gui) {
                    global_state.ui->reset();
                }
                else if (global_state.ui->ready()) {
                    ctx->pc -= INC_PTR;
                    return true;
                }
            }
        }
        break;
        case 302:
        {
            auto x = ctx->ax._ui >> 16;
            auto y = ctx->ax._ui & 0xFFFF;
            global_state.ui->move_to(x, y);
        }
        break;
        case 303:
        {
            auto x = ctx->ax._ui >> 16;
            auto y = ctx->ax._ui & 0xFFFF;
            global_state.ui->line_to(x, y);
        }
        break;
        case 304:
        {
            auto x = ctx->ax._ui >> 16;
            auto y = ctx->ax._ui & 0xFFFF;
            global_state.ui->draw_point(x, y);
        }
        break;
        case 305:
        {
            global_state.ui->set_color(ctx->ax._ui);
        }
        break;
        case 306:
        {
            global_state.ui->clear(ctx->ax._ui);
        }
        break;
        case 307:
        {
            auto x = ctx->ax._ui >> 16;
            auto y = ctx->ax._ui & 0xFFFF;
            global_state.ui->fill_rect(x, y);
        }
        break;
        case 308:
        {
            global_state.ui->create_font();
        }
        break;
        case 309:
        {
            global_state.ui->set_font_size(ctx->ax._i);
        }
        break;
        case 310:
        {
            global_state.ui->set_font_family(vmm_getstr(ctx->ax._ui));
        }
        break;
        case 311:
        {
            global_state.ui->draw_font(vmm_getstr(ctx->ax._ui));
        }
        break;
        case 312:
        {
            global_state.gui_blur = ctx->ax._f;
        }
        break;
        case 350:
        {
            auto s = vmm_getstr(ctx->ax._ui);
            auto ev = window->get_event();
            struct timeval tv;
            auto evt = evtimer_new(ev, &exec_lua, new string_t(s));
            evutil_timerclear(&tv);
            tv.tv_sec = 0;
            tv.tv_usec = 10;
            evtimer_add(evt, &tv);
        }
        break;
        case 380:
        {
            auto n = ctx->ax._i;
            auto zplay = fs.get_zplay();
            if (n == 0) {
                if (zplay) {
                    libZPlay::TStreamStatus status;
                    zplay->GetStatus(&status);
                    if (status.fPause) { ctx->ax._i = 1; zplay->Resume(); }
                    else if (status.fPlay) { ctx->ax._i = 2; zplay->Pause(); }
                    else { ctx->ax._i = 3; zplay->Play(); }
                }
            }
        }
        break;
        default:
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] ERR  | unknown interrupt: %d\n", ctx->ax._i);
#endif
            error("unknown interrupt");
            break;
        }
        ctx->pc += INC_PTR;
        return false;
    }

    bool cvm::web(int id) {
        switch (id) {
        case 401:
        {
            auto json = vmm_getstr(ctx->ax._ui);
            std::stringstream ss;
            try {
                clib::cparser_json p(json);
                auto root = p.parse();
                clib::cast_json::print(root, 0, ss);
            }
            catch (const std::exception&) {
#if LOG_SYSTEM
                ATLTRACE("[SYSTEM] ERR  | JSON PARSE ERROR: %s\n", json.c_str());
#endif
                ss << "{\"Error\": \"clibparser::error\"}";
            }
            auto s = ss.str();
            ctx->ax._ui = vmm_malloc(s.length() + 1);
            vmm_setstr(ctx->ax._ui, s);
            break;
        }
        case 402:
        {
            auto json = vmm_getstr(ctx->ax._ui);
            try {
                clib::cparser_json p(json);
                auto root = p.parse();
                ctx->ax._ui = parse_json(root->child);
            }
            catch (const std::exception&) {
                ctx->ax._ui = 0;
#if LOG_SYSTEM
                ATLTRACE("[SYSTEM] ERR  | JSON PARSE ERROR: %s\n", json.substr(0, 100).c_str());
#endif
            }
            break;
        }
        default:
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] ERR  | unknown interrupt: %d\n", ctx->ax._i);
#endif
            error("unknown interrupt");
            break;
        }
        ctx->pc += INC_PTR;
        return false;
    }

    void ast_get_children(ast_node_json* node, std::list<ast_node_json*>& children) {
        node = node->child;
        if (node == nullptr)
            return;
        auto i = node;
        if (i->next == i) {
            children.push_back(i);
            return;
        }
        children.push_back(i);
        i = i->next;
        while (i != node) {
            children.push_back(i);
            i = i->next;
        }
    }

    void ast_get_children2(ast_node_json* node, std::list<ast_node_json*>& children) {
        node = node->child;
        if (node == nullptr)
            return;
        auto i = node;
        if (i->next == i) {
            children.push_back(i->child);
            children.push_back(i->child->next);
            return;
        }
        children.push_back(i->child);
        children.push_back(i->child->next);
        i = i->next;
        while (i != node) {
            children.push_back(i->child);
            children.push_back(i->child->next);
            i = i->next;
        }
    }

    uint32 cvm::parse_json(ast_node_json* node)
    {
        enum json_type {
            j_none,
            j_object,
            j_array,
            j_char,
            j_uchar,
            j_short,
            j_ushort,
            j_int,
            j_uint,
            j_long,
            j_ulong,
            j_float,
            j_double,
            j_string,
            j_obj,
            j_list,
        };

        struct json_object_obj_list {
            uint32 name; // json_object
            uint32 value; // json_object
        };

        struct json_object_obj {
            uint32 len;
            uint32 list; // json_object_obj_list
        };

        struct json_object_arr {
            uint32 len;
            uint32 list; // json_object
        };

        struct json_object {
            int type;
            union {
                uint32 obj; // json_object_obj
                uint32 arr; // json_object_arr
                char c;
                unsigned char uc;
                short s;
                unsigned short us;
                int i;
                unsigned int ui;
                int64 l;
                uint64 ul;
                float f;
                double d;
                uint32 str;
            } data;
        };

        std::vector<byte> data;
        std::vector<std::tuple<int, int>> links;
        std::vector<std::list<ast_node_json*>> params;
        std::vector<std::list<int>> params_link;
        std::vector<int> rets;
        if (node == nullptr)
            return 0;
        params.emplace_back();
        params[0].push_back(node);
        params_link.emplace_back();
        params_link[0].push_back(-1);
        rets.push_back(0);
        json_object o;
        while (!params.empty()) {
            auto& r = rets.back();
            if (r == 0) { // NOT VISIT
                if (params.back().empty()) { r = 1; continue; }
                auto cur = params.back().front();
                auto lnk = params_link.back().front();
                if (lnk != -1) {
                    links.push_back(std::make_pair(lnk, data.size()));
                    params_link.back().pop_front();
                }
                if (cur->flag >= ast_json_string && cur->flag <= ast_json_double) {
                    memset(&o, 0, sizeof(o));
                    switch (cur->flag) {
                    case ast_json_string:
                    {
                        o.type = j_string;
                        o.data.str = data.size() + sizeof(o.type) + sizeof(o.data.ul);
                        std::copy((byte*)& o.type, ((byte*)& o.type) + sizeof(o.type), std::back_inserter(data));
                        links.push_back(std::make_pair(data.size(), o.data.str));
                        std::copy((byte*)& o.data.str, ((byte*)& o.data.str) + sizeof(o.data.ul), std::back_inserter(data));
                        auto s = cur->data._string;
                        auto i = 0;
                        while (s[i]) {
                            data.push_back(s[i++]);
                        }
                        data.emplace_back();
                        i++;
                        auto end = i % 4;
                        if (end)
                            for (auto j = end; j < 4; j++)
                                data.emplace_back();
                    }
                    break;
                    case ast_json_char:
                        o.type = j_char;
                        o.data.c = cur->data._char;
                        break;
                    case ast_json_uchar:
                        o.type = j_uchar;
                        o.data.uc = cur->data._uchar;
                        break;
                    case ast_json_short:
                        o.type = j_short;
                        o.data.s = cur->data._short;
                        break;
                    case ast_json_ushort:
                        o.type = j_ushort;
                        o.data.us = cur->data._ushort;
                        break;
                    case ast_json_int:
                        o.type = j_int;
                        o.data.i = cur->data._int;
                        break;
                    case ast_json_uint:
                        o.type = j_uint;
                        o.data.ui = cur->data._uint;
                        break;
                    case ast_json_long:
                        o.type = j_long;
                        o.data.l = cur->data._long;
                        break;
                    case ast_json_ulong:
                        o.type = j_ulong;
                        o.data.ul = cur->data._ulong;
                        break;
                    case ast_json_float:
                        o.type = j_float;
                        o.data.f = cur->data._float;
                        break;
                    case ast_json_double:
                        o.type = j_double;
                        o.data.d = cur->data._double;
                        break;
                    }
                    if (cur->flag != ast_json_string) {
                        std::copy((byte*)& o.type, ((byte*)& o.type) + sizeof(o.type), std::back_inserter(data));
                        std::copy((byte*)& o.data.c, ((byte*)& o.data.c) + sizeof(o.data.ul), std::back_inserter(data));
                    }
                    params.back().pop_front();
                }
                else {
                    if (params.back().size() == 1)
                        r = 1;
                    else
                        params.back().pop_front();
                    if (cur->flag == ast_json_obj) {
                        memset(&o, 0, sizeof(o));
                        o.type = j_obj;
                        o.data.obj = 0;
                        std::copy((byte*)& o.type, ((byte*)& o.type) + sizeof(o.type), std::back_inserter(data));
                        auto lnk = (int)data.size();
                        std::copy((byte*)& o.data.c, ((byte*)& o.data.c) + sizeof(o.data.ul), std::back_inserter(data));
                        links.push_back(std::make_pair(lnk, data.size()));
                        params.emplace_back();
                        ast_get_children2(cur, params.back());
                        params_link.emplace_back();
                        json_object_obj o;
                        o.len = params.back().size();
                        auto l = o.len / 2;
                        std::copy((byte*)& l, ((byte*)& l) + sizeof(l), std::back_inserter(data));
                        for (size_t i = 0; i < o.len; i++) {
                            params_link.back().push_back(data.size());
                            data.emplace_back();
                            data.emplace_back();
                            data.emplace_back();
                            data.emplace_back();
                        }
                    }
                    else if (cur->flag == ast_json_list) {
                        memset(&o, 0, sizeof(o));
                        o.type = j_list;
                        o.data.arr = 0;
                        std::copy((byte*)& o.type, ((byte*)& o.type) + sizeof(o.type), std::back_inserter(data));
                        auto lnk = (int)data.size();
                        std::copy((byte*)& o.data.c, ((byte*)& o.data.c) + sizeof(o.data.ul), std::back_inserter(data));
                        links.push_back(std::make_pair(lnk, data.size()));
                        params.emplace_back();
                        ast_get_children(cur, params.back());
                        params_link.emplace_back();
                        json_object_arr arr;
                        arr.len = params.back().size();
                        std::copy((byte*)& arr.len, ((byte*)& arr.len) + sizeof(arr.len), std::back_inserter(data));
                        for (size_t i = 0; i < arr.len; i++) {
                            params_link.back().push_back(data.size());
                            data.emplace_back();
                            data.emplace_back();
                            data.emplace_back();
                            data.emplace_back();
                        }
                    }
                    else {
                        error("invalid json");
                    }
                    rets.push_back(0);
                }
            }
            else if (r == 1) { // VISITING
                params.pop_back();
                params_link.pop_back();
                rets.pop_back();
            }
        }
        auto vmdata = vmm_malloc(data.size());
        vmm_setmem(vmdata, data.size(), data);
        for (const auto& L : links) {
            vmm_set<uint32>(vmdata + std::get<0>(L), vmdata + std::get<1>(L));
        }
        return vmdata;
    }

    bool cvm::is_window_handle(int h) const
    {
        return ctx->handles.find(h) != ctx->handles.end() && handles[h]->type == h_window;
    }

    bool cvm::wnd(int id)
    {
        switch (id) {
        case 501:
        {
            struct __window_create_struct__ {
                uint32_t caption;
                int left, top, width, height;
            };
            auto s = vmm_get<__window_create_struct__>(ctx->ax._ui);
            auto h = new_handle(h_window);
            handles[h]->name = vmm_getstr(s.caption);
            handles[h]->data.cwnd = new cwindow(this, h, handles[h]->name,
                CRect(s.left, s.top, s.left + s.width, s.top + s.height));
            wnds.push_back(handles[h]->data.cwnd);
            handles[h]->data.cwnd->init();
            ctx->ax._i = h;
            break;
        }
        case 502:
        {
            auto s = vmm_get<cwindow::window_msg2>(ctx->ax._ui);
            auto h = s.handle;
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._i = wnd->handle_msg(s.msg);
                break;
            }
            ctx->ax._i = -1;
            break;
        }
        case 503:
        {
            struct __window_create_comctl_struct__ {
                int handle;
                cwindow::window_comctl_type type;
            };
            auto s = vmm_get<__window_create_comctl_struct__>(ctx->ax._ui);
            auto h = s.handle;
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._u._1 = h;
                ctx->ax._u._2 = wnd->create_comctl(s.type);
                break;
            }
            ctx->ax._q = -1LL;
            break;
        }
        case 504:
        {
            auto h = ctx->ax._i;
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._u._1 = h;
                ctx->ax._u._2 = wnd->get_base();
                break;
            }
            ctx->ax._q = -1LL;
            break;
        }
        case 505:
        {
            struct __window_comctl_connect_struct__ {
                int handle; int id;
                int child; int cid;
            };
            auto s = vmm_get<__window_comctl_connect_struct__>(ctx->ax._ui);
            auto h = s.handle;
            auto c = s.child;
            if (h == c && is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._i = wnd->connect(s.id, s.cid) ? 0 : -1;
                break;
            }
            ctx->ax._i = -1;
            break;
        }
        case 506:
        {
            struct __window_comctl_set_flag_struct__ {
                int handle; int id;
                int flag;
            };
            auto s = vmm_get<__window_comctl_set_flag_struct__>(ctx->ax._ui);
            auto h = s.handle;
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._i = wnd->set_flag(s.id, s.flag) ? 0 : -1;
                break;
            }
            ctx->ax._i = -1;
            break;
        }
        case 507:
        {
            struct __window_set_style_struct__ {
                int handle;
                int style;
            };
            auto s = vmm_get<__window_set_style_struct__>(ctx->ax._ui);
            auto h = s.handle;
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._i = wnd->set_style(s.style) ? 0 : -1;
                break;
            }
            ctx->ax._i = -1;
            break;
        }
        case 509:
        {
            struct __window_comctl_set_bound_struct__ {
                int handle; int id;
                int left, top, right, bottom;
            };
            auto s = vmm_get<__window_comctl_set_bound_struct__>(ctx->ax._ui);
            auto h = s.handle;
            auto c = s.id;
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._i = wnd->set_bound(c, CRect(s.left, s.top, s.right, s.bottom)) ? 0 : -1;
                break;
            }
            ctx->ax._i = -1;
            break;
        }
        case 510:
        {
            struct __window_comctl_set_text_struct__ {
                int handle; int id;
                uint32 text;
            };
            auto s = vmm_get<__window_comctl_set_text_struct__>(ctx->ax._ui);
            auto h = s.handle;
            auto c = s.id;
            auto text = vmm_getstr(s.text);
            if (is_window_handle(h)) {
                auto wnd = handles[h]->data.cwnd;
                ctx->ax._i = wnd->set_text(c, text) ? 0 : -1;
                break;
            }
            ctx->ax._i = -1;
            break;
        }
        case 511:
        {
            struct __window_comctl_set_ptr_struct__ {
                int handle; int id;
                uint32 ptr; int len;
            };
            auto s_ = vmm_get<__window_comctl_set_ptr_struct__>(ctx->ax._ui);
            auto h = s_.handle;
            auto c = s_.id;
            std::vector<byte> data;
            vmm_getmem(s_.ptr, s_.len, data);
            data.push_back(0);
            auto path = std::string((char*)data.data());
            vfs_node_dec* dec = nullptr;
            {
                if (path[0] != '/') {
                    auto s = 0;
                    decltype(ctx->paths) ps;
                    auto pwd = fs.get_pwd();
                    ps.push_back(pwd == "/" ? "" : pwd);
                    std::copy(ctx->paths.begin(), ctx->paths.end(), std::back_inserter(ps));
                    for (auto& p : ps) {
                        auto pp = p + '/' + path;
                        s = fs.get(pp, &dec, this);
                        if (s == 0)
                            break;
                    }
                    if (s != 0) {
                        ctx->ax._i = s;
                        break;
                    }
                }
                else {
                    auto s = fs.get(path, &dec, this);
                    if (s != 0) {
                        ctx->ax._i = s;
                        break;
                    }
                }
            }
            std::unique_ptr<vfs_node_dec> dec_ref;
            if (dec) dec_ref.reset(dec);
            if (dec && is_window_handle(h)) {
                data.clear();
                if (dec->get_data(data)) {
                    auto wnd = handles[h]->data.cwnd;
                    ctx->ax._i = wnd->set_ptr(c, data) ? 0 : -1;
                    break;
                }
            }
            ctx->ax._i = -1;
            break;
        }
        default:
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] ERR  | unknown interrupt: %d\n", ctx->ax._i);
#endif
            error("unknown interrupt");
            break;
        }
        ctx->pc += INC_PTR;
        return false;
    }

    bool cvm::interrupt() {
        auto id = vmm_get(ctx->pc);
        if (id > 200 && id < 300)
            return math(id);
        if (id >= 300 && id < 400)
            return gui(id);
        if (id >= 400 && id < 500)
            return web(id);
        if (id >= 500 && id < 600)
            return wnd(id);
        switch (id) {
        case 0:
        case 1:
        case 2:
        case 4:
        case 6:
        case 7:
        case 9:
            if (output(id)) return true;
            break;
        case 3:
            ctx->debug = !ctx->debug;
            break;
        case 5:
            vmm_setstr((uint32_t)ctx->ax._i, global_state.hostname);
            break;
        case 8:
            if (global_state.input_lock == ctx->id) {
                cgui::singleton().input_char(ctx->ax._c);
            }
            break;
        case 10: {
            if (ctx->input_redirect != -1) {
                ctx->ax._i = ctx->input_stop ? 0 : 1;
                ctx->pc += INC_PTR;
            }
            else {
                if (global_state.input_lock == -1) {
                    ctx->flag |= CTX_INPUT;
                    global_state.input_lock = ctx->id;
                    ctx->pc += INC_PTR;
                    cgui::singleton().input_set(true);
                }
                else {
                    global_state.input_waiting_list.push_back(ctx->id);
                    ctx->state = CTS_WAIT;
                    ctx->pc -= INC_PTR;
                }
            }
            return true;
        }
        case 11: {
            if (ctx->input_redirect != -1) {
                if (!ctx->input_queue.empty()) {
                    ctx->ax._i = ctx->input_queue.front();
                    ctx->input_queue.pop_front();
                    break;
                }
                else if (!ctx->input_stop) {
                    ctx->pc -= INC_PTR;
                    return true;
                }
                else {
                    ctx->input_redirect = -1;
                }
            }
            else if (global_state.input_lock == ctx->id) {
                if (global_state.input_success) {
                    if (global_state.input_read_ptr >= (int)global_state.input_content.length()) {
                        ctx->ax._i = -1;
                        ctx->pc += INC_PTR;
                        // INPUT COMPLETE
                        for (auto& _id : global_state.input_waiting_list) {
                            if (tasks[_id] && tasks[_id]->flag & CTX_VALID) {
                                assert(tasks[_id]->state == CTS_WAIT);
                                tasks[_id]->state = CTS_RUNNING;
                            }
                        }
                        global_state.input_lock = -1;
                        global_state.input_waiting_list.clear();
                        global_state.input_read_ptr = -1;
                        global_state.input_content.clear();
                        global_state.input_success = false;
                        global_state.input_code = 0;
                        cgui::singleton().input_set(false);
                        return true;
                    }
                    else {
                        ctx->ax._i = global_state.input_content[global_state.input_read_ptr++];
                        break;
                    }
                }
                else {
                    ctx->pc -= INC_PTR;
                    return true;
                }
            }
            ctx->ax._i = -1;
            ctx->pc += INC_PTR;
            return true;
        }
        case 12: {
            if (ctx->input_redirect == -1 && global_state.input_lock == ctx->id) {
                if (global_state.input_success) {
                    // INPUT INTERRUPT
                    for (auto& _id : global_state.input_waiting_list) {
                        if (tasks[_id] && tasks[_id]->flag & CTX_VALID) {
                            assert(tasks[_id]->state == CTS_WAIT);
                            tasks[_id]->state = CTS_RUNNING;
                        }
                    }
                    global_state.input_lock = -1;
                    global_state.input_waiting_list.clear();
                    global_state.input_read_ptr = -1;
                    global_state.input_content.clear();
                    global_state.input_success = false;
                    global_state.input_code = 0;
                    cgui::singleton().input_set(false);
                }
            }
            else {
                ctx->input_stop = false;
            }
        }
                 break;
        case 13: {
            ctx->ax._i = ctx->input_redirect != -1 ? 0 : 1;
        }
                 break;
        case 14: {
            if (ctx->input_redirect != -1) {
                if (!ctx->input_queue.empty()) {
                    ctx->ax._i = 0;
                    break;
                }
                else if (!ctx->input_stop) {
                    ctx->pc -= INC_PTR;
                    return true;
                }
                else {
                    ctx->input_redirect = -1;
                }
            }
            else if (global_state.input_lock == ctx->id) {
                if (global_state.input_success) {
                    if (global_state.input_read_ptr >= (int)global_state.input_content.length()) {
                        if (global_state.input_code != 0) {
                            ctx->ax._i = global_state.input_code;
                            global_state.input_code = 0;
                            break;
                        }
                        ctx->ax._i = -1;
                        ctx->pc += INC_PTR;
                        // INPUT COMPLETE
                        for (auto& _id : global_state.input_waiting_list) {
                            if (tasks[_id] && tasks[_id]->flag & CTX_VALID) {
                                assert(tasks[_id]->state == CTS_WAIT);
                                tasks[_id]->state = CTS_RUNNING;
                            }
                        }
                        ctx->flag &= ~CTX_INPUT;
                        global_state.input_lock = -1;
                        global_state.input_waiting_list.clear();
                        global_state.input_read_ptr = -1;
                        global_state.input_content.clear();
                        global_state.input_success = false;
                        cgui::singleton().input_set(false);
                        return true;
                    }
                    else {
                        ctx->ax._i = 0;
                        break;
                    }
                }
                else {
                    ctx->pc -= INC_PTR;
                    return true;
                }
            }
            ctx->ax._i = -1;
            ctx->pc += INC_PTR;
            return true;
        }
        case 15:
            global_state.input_single = true;
                 break;
        case 16: {
            if (ctx->parent != -1 && tasks[ctx->parent] &&
                tasks[ctx->parent]->flag & CTX_VALID) {
                ctx->output_redirect = ctx->parent;
            }
            break;
        }
        case 17: {
            if (ctx->input_queue.empty()) {
                ctx->ax._i = -1;
            }
            else {
                ctx->ax._i = ctx->input_queue.front();
                ctx->input_queue.pop_front();
            }
            break;
        }
        case 20: {
            if (global_state.input_lock == -1) {
                set_resize_id.insert(ctx->id);
                cgui::singleton().resize(ctx->ax._i >> 16, ctx->ax._i & 0xFFFF);
            }
            else {
                if (global_state.input_lock != ctx->id)
                    global_state.input_waiting_list.push_back(ctx->id);
                ctx->state = CTS_WAIT;
                ctx->pc -= INC_PTR;
                return true;
            }
            break;
        }
        case 30:
            if (ctx->ax._i != 0)
                ctx->ax._ui = vmm_malloc(ctx->ax._ui);
            break;
        case 31:
            ctx->ax._ui = vmm_free(ctx->ax._ui);
            break;
        case 40:
            destroy(ctx->id);
            return true;
        case 41:
            vmm_setstr(ctx->ax._ui, get_stacktrace());
            break;
        case 42:
        {
            auto left = ctx->ax._ui >> 16;
            auto right = ctx->ax._ui & 0xFFFF;
            if (left >= 0 && left < TASK_NUM) {
                if (tasks[left] && tasks[left]->flag & CTX_VALID) {
                    if (right == KILL_SIGNAL) {
                        if (ctx->child.find(left) != ctx->child.end()) {
                            std::vector<int> childs;
                            childs.push_back(left);
                            size_t i = 0, j = childs.size();
                            while (i < j) {
                                for (const auto& cc : tasks[childs[i]]->child) {
                                    childs.push_back(cc);
                                    j++;
                                }
                                i++;
                            }
                            for (const auto& cc : childs) {
                                destroy(cc);
                            }
                        }
                    }
                    else {
                        if (right == SOFT_KILL_SIGNAL) {
                            std::vector<int> childs;
                            childs.push_back(left);
                            size_t i = 0, j = childs.size();
                            while (i < j) {
                                for (const auto& cc : tasks[childs[i]]->child) {
                                    childs.push_back(cc);
                                    j++;
                                }
                                i++;
                            }
                            for (const auto& cc : childs) {
                                tasks[cc]->sigs.push(right);
                            }
                        }else{
                            tasks[left]->sigs.push(right);
                        }
                    }
                }
            }
        }
            break;
        case 43:
            if (ctx->sigs.empty()) {
                ctx->ax._i = -1;
            }
            else {
                ctx->ax._us = ctx->sigs.front();
                ctx->sigs.pop();
            }
            break;
        case 51:
            ctx->ax._i = exec_file(vmm_getstr(ctx->ax._ui));
            ctx->pc += INC_PTR;
            return true;
        case 50:
            if (ctx->ax._i == 1)
                ctx->ax._i = ctx->parent;
            else
                ctx->ax._i = ctx->id;
            break;
        case 52: {
            if (ctx->exited_child.empty()) {
                ctx->state = CTS_WAIT;
                ctx->pc += INC_PTR;
                return true;
            }
            else {
                ctx->ax._i = ctx->exited_child.front();
                ctx->exited_child.pop_front();
            }
        }
                 break;
        case 53: {
            ctx->ax._i = exec_file(vmm_getstr(ctx->ax._ui));
            if (ctx->ax._i >= 0 && ctx->ax._i < TASK_NUM)
                tasks[ctx->ax._i]->state = CTS_WAIT;
            break;
        }
        case 54: {
            if (ctx->ax._i >= 0 && ctx->ax._i < TASK_NUM) {
                if (ctx->child.find(ctx->ax._i) != ctx->child.end())
                    tasks[ctx->ax._i]->state = CTS_RUNNING;
            }
            break;
        }
        case 55: {
            ctx->pc += INC_PTR;
            ctx->ax._i = fork();
            return true;
        }
        case 56: {
            auto left = ctx->ax._ui >> 16;
            auto right = ctx->ax._ui & 0xFFFF;
            if ((left == ctx->id || ctx->child.find(left) != ctx->child.end()) &&
                (right == ctx->id || ctx->child.find(right) != ctx->child.end())) {
                tasks[right]->input_redirect = left;
                tasks[left]->output_redirect = right;
            }
            break;
        }
        case 57: {
            std::vector<int> ids(ctx->child.begin(), ctx->child.end());
            for (auto& id : ids) {
                destroy(id);
            }
            break;
        }
        case 58: {
            if ((ctx->flag & CTX_FOREGROUND) != 0) {
                ctx->flag &= ~CTX_FOREGROUND;
            }
            else {
                ctx->flag |= CTX_FOREGROUND;
            }
            break;
        }
        case 59: {
            if (ctx->ax._i) {
                set_cycle_id = ctx->id;
            }
            else {
                set_cycle_id = -1;
            }
            cgui::singleton().set_cycle(ctx->ax._i);
            break;
        }
        case 60:
            vmm_setstr(ctx->ax._ui, fs.get_pwd());
            break;
        case 61:
            vmm_setstr(ctx->ax._ui, fs.get_user());
            break;
        case 62:
            ctx->ax._i = fs.cd(trim(vmm_getstr(ctx->ax._ui)));
            break;
        case 63:
            ctx->ax._i = fs.mkdir(trim(vmm_getstr(ctx->ax._ui)));
            break;
        case 64:
            ctx->ax._i = fs.touch(trim(vmm_getstr(ctx->ax._ui)));
            break;
        case 65: {
            auto path = trim(vmm_getstr(ctx->ax._ui));
            vfs_node_dec* dec = nullptr;
            auto ret = vfs_open(path, &dec);
            {
                auto suc = false;
                for (auto i = 0; i < VFS_LINK_MAX_NUM; i++) {
                    if (ret < 0)
                        break;
                    if (ret == 0) {
                        if (dec->get_link(path)) {
                            assert(dec);
                            delete dec;
                            ret = vfs_open(path, &dec);
                        }
                        else {
                            suc = true;
                            break;
                        }
                    }
                }
                if (!suc) {
                    ctx->ax._i = ret;
                    break;
                }
            }
            auto h = new_handle(h_file);
#if LOG_VM
            {
                CStringA s;
                s.Format("[SYSTEM] FILE | Open: %s, handle= %d", path.c_str(), h);
                logging(CString(s));
            }
#endif
            handles[h]->name = path;
            handles[h]->data.file = dec;
            ctx->ax._i = h;
        }
                 break;
        case 66: {
            auto h = ctx->ax._i;
            if (ctx->handles.find(h) != ctx->handles.end()) {
                auto dec = handles[h]->data.file;
                auto t = dec->get_handle(h, v_read);
                if (t == v_none)
                    dec->add_handle(h, v_read);
                else if (t == v_wait) {
                    if (ctx->sigs.empty()) {
                        ctx->pc -= INC_PTR;
                        return true;
                    }
                    else {
                        ctx->ax._i = READ_EOF;
                        break;
                    }
                }
                else if (t != v_read || t == v_error) {
                    ctx->ax._i = READ_ERROR;
                    break;
                }
                ctx->ax._i = dec->index();
                if (ctx->ax._i == WAIT_CHAR) {
                    ctx->pc -= INC_PTR;
                    ctx->ax._i = h;
                    return true;
                }
                if (ctx->ax._i == DELAY_CHAR) {
                    ctx->pc -= INC_PTR;
                    ctx->ax._i = h;
                    dec->advance();
                    return true;
                }
                if (ctx->ax._i < READ_EOF) {
                    dec->advance();
                }
            }
            else {
                ctx->ax._i = READ_ERROR;
            }
        }
                 break;
        case 67: {
            auto h = ctx->ax._i;
            if (ctx->handles.find(h) != ctx->handles.end()) {
                destroy_handle(h);
            }
            else {
                ctx->ax._i = -1;
            }
        }
                 break;
        case 68: {
            ctx->ax._i = fs.rm_safe(trim(vmm_getstr(ctx->ax._ui)));
            break;
        }
        case 69: {
            auto h = ctx->ax._ui >> 16;
            auto c = (ctx->ax._ui & 0xFFFF) - 0x1000;
            if (ctx->handles.find(h) != ctx->handles.end()) {
                auto dec = handles[h]->data.file;
                auto t = dec->get_handle(h, v_write);
                if (t == v_none)
                    dec->add_handle(h, v_write);
                else if (t == v_wait) {
                    if (ctx->sigs.empty()) {
                        ctx->pc -= INC_PTR;
                        return true;
                    }
                    else {
                        ctx->ax._i = READ_EOF;
                        break;
                    }
                }
                else if (t != v_write || t == v_error) {
                    ctx->ax._i = READ_ERROR;
                    break;
                }
                ctx->ax._i = dec->write((byte)c);
            }
            else {
                ctx->ax._i = -3;
            }
        }
                 break;
        case 70: {
            auto h = ctx->ax._i;
            if (ctx->handles.find(h) != ctx->handles.end()) {
                auto dec = handles[h]->data.file;
                auto t = dec->get_handle(h, v_write);
                if (t == v_none)
                    dec->add_handle(h, v_write);
                else if (t == v_wait) {
                    if (ctx->sigs.empty()) {
                        ctx->pc -= INC_PTR;
                        return true;
                    }
                    else {
                        ctx->ax._i = READ_EOF;
                        break;
                    }
                }
                else if (t != v_write || t == v_error) {
                    ctx->ax._i = READ_ERROR;
                    break;
                }
                ctx->ax._i = dec->truncate();
            }
            else {
                ctx->ax._i = -3;
            }
        }
                 break;
        case 71: {
            auto path = trim(vmm_getstr(ctx->ax._ui));
            if (std::find(ctx->paths.begin(), ctx->paths.end(), path) == ctx->paths.end())
                ctx->paths.push_back(path);
        }
                 break;
        case 72: {
            auto path = trim(vmm_getstr(ctx->ax._ui));
            ctx->paths.erase(std::remove(
                ctx->paths.begin(), ctx->paths.end(), path), ctx->paths.end());
        }
                 break;
        case 73: {
            ctx->ax._i = exec_file(vmm_getstr(ctx->ax._ui));
            if (ctx->ax._i >= 0 && ctx->ax._i < TASK_NUM && tasks[ctx->ax._i]) {
                tasks[ctx->ax._i]->flag |= CTX_SERVICE;
                tasks[ctx->ax._i]->flag &= ~CTX_FOREGROUND;
            }
        }
                 break;
        case 74: {
            struct __copy_struct__ {
                int from, to;
            };
            auto s = vmm_get<__copy_struct__>(ctx->ax._ui);
            if (ctx->handles.find(s.from) != ctx->handles.end() &&
                ctx->handles.find(s.to) != ctx->handles.end()) {
                auto from = handles[s.from].get();
                auto to = handles[s.to].get();
                if (from->type == h_file && to->type == h_file) {
                    std::vector<byte> data;
                    if (from->data.file->get_data(data) && to->data.file->set_data(data)) {
                        ctx->ax._i = 0;
                    }
                    else {
                        ctx->ax._i = -3;
                    }
                }
                else {
                    ctx->ax._i = -2;
                }
            }
            else {
                ctx->ax._i = -1;
            }
        }
                 break;
        case 75: {
            {
                auto h = ctx->ax._i;
                if (ctx->handles.find(h) != ctx->handles.end()) {
                    auto dec = handles[h]->data.file;
                    auto t = dec->get_handle(h, v_read);
                    if (t == v_none)
                        dec->add_handle(h, v_read);
                    else if (t == v_wait) {
                        if (ctx->sigs.empty()) {
                            ctx->pc -= INC_PTR;
                            return true;
                        }
                        else {
                            ctx->ax._i = READ_EOF;
                            break;
                        }
                    }
                    else if (t != v_read || t == v_error) {
                        ctx->ax._i = READ_ERROR;
                        break;
                    }
                    ctx->ax._i = dec->index();
                    if (ctx->ax._i == WAIT_CHAR) {
                        ctx->pc -= INC_PTR;
                        ctx->ax._i = h;
                        return true;
                    }
                    ctx->ax._i = 0;
                }
                else {
                    ctx->ax._i = READ_ERROR;
                }
            }
        }
                 break;
        case 76: {
            {
                auto h = ctx->ax._i;
                if (ctx->handles.find(h) != ctx->handles.end()) {
                    auto dec = handles[h]->data.file;
                    auto t = dec->get_handle(h, v_read);
                    if (t == v_none)
                        dec->add_handle(h, v_read);
                    else if (t == v_wait) {
                        if (ctx->sigs.empty()) {
                            ctx->pc -= INC_PTR;
                            return true;
                        }
                        else {
                            ctx->ax._i = -1;
                            break;
                        }
                    }
                    else if (t != v_read || t == v_error) {
                        ctx->ax._i = -1;
                        break;
                    }
                    ctx->ax._i = dec->get_length();
                }
                else {
                    ctx->ax._i = -1;
                }
            }
        }
               break;
        case 77: {
            {
                auto h = ctx->ax._i;
                if (ctx->handles.find(h) != ctx->handles.end()) {
                    auto dec = handles[h]->data.file;
                    auto t = dec->get_handle(h, v_read);
                    if (t == v_none)
                        dec->add_handle(h, v_read);
                    else if (t == v_wait) {
                        if (ctx->sigs.empty()) {
                            ctx->pc -= INC_PTR;
                            return true;
                        }
                        else {
                            ctx->ax._i = 0;
                            break;
                        }
                    }
                    else if (t != v_read || t == v_error) {
                        ctx->ax._i = 0;
                        break;
                    }
                    std::vector<byte> data;
                    if (dec->get_data(data)) {
                        auto vmdata = vmm_malloc(data.size());
                        ctx->ax._ui = vmdata;
                        vmm_setmem(vmdata, data.size(), data);
                    }
                    else {
                        ctx->ax._i = 0;
                    }
                }
                else {
                    ctx->ax._i = 0;
                }
            }
        }
               break;
        case 78: {
            struct __mklink_struct__ {
                int from;
                uint32 to;
            };
            auto s = vmm_get<__mklink_struct__>(ctx->ax._ui);
            auto link = vmm_getstr(s.to);
            if (ctx->handles.find(s.from) != ctx->handles.end()) {
                auto from = handles[s.from].get();
                if (from->type == h_file) {
                    if (from->data.file->get_length() == 0 && from->data.file->set_link(link)) {
                        ctx->ax._i = 0;
                    }
                    else {
                        ctx->ax._i = -3;
                    }
                }
                else {
                    ctx->ax._i = -2;
                }
            }
            else {
                ctx->ax._i = -1;
            }
        }
               break;
        case 79: {
            if (ctx->ax._i != 0) {
                ctx->flag |= CTX_SERVICE_USER_SHELL;
            }
            else {
                ctx->flag &= ~CTX_SERVICE_USER_SHELL;
            }
        }
               break;
        case 80:
        {
            auto h = ctx->ax._i;
            if (ctx->handles.find(h) != ctx->handles.end()) {
                ctx->pc -= INC_PTR;
                return true;
            }
        }
        break;
        case 100: {
            if (ctx->ax._i < 0) {
                ctx->waiting_ms += (-ctx->ax._i) * 0.001;
            }
            else {
                ctx->record_now = std::chrono::system_clock::now();
                ctx->waiting_ms = ctx->ax._i * 0.001;
            }
        }
                  break;
        case 101: {
            auto now = std::chrono::system_clock::now();
            if (std::chrono::duration_cast<std::chrono::duration<decimal>>(
                now - ctx->record_now).count() <= ctx->waiting_ms) {
                if (ctx->sigs.empty()) {
                    ctx->pc -= INC_PTR;
                    return true;
                }
            }
        }
                  break;
        case 102:
            // 单位为微秒
            ctx->ax._q = std::chrono::high_resolution_clock::now().time_since_epoch().count() / 1000;
            break;
        default:
#if LOG_SYSTEM
            ATLTRACE("[SYSTEM] ERR  | unknown interrupt: %d\n", ctx->ax._i);
#endif
            error("unknown interrupt");
            break;
        }
        ctx->pc += INC_PTR;
        return false;
    }

    void cvm::paint_window(const CRect& bounds)
    {
        if (draw_bounds.Size() != bounds.Size()) {
            draw_bounds = bounds;
            for (auto& wnd : wnds) {
                wnd->post_data(WM_COMMAND, 1, 0, -1);
            }
        }
        for (auto& wnd : wnds) {
            wnd->paint(bounds);
        }
    }

    void cvm::reset_ips()
    {
        for (int i = 0; i < TASK_NUM; ++i) {
            if (tasks[i] && tasks[i]->flag & CTX_VALID) {
                tasks[i]->ips_disp = tasks[i]->ips;
                tasks[i]->ips = 0ULL;
            }
        }
    }

    void cvm::hit(int n)
    {
        if (n == 212 || n == 6 || n == 7)
            return;
        if (n == 213) {
            auto& hover = cvm::global_state.window_hover;
            if (hover != -1) { // 当前有悬停窗口
                post_data(hover, WM_MOUSELEAVE); // 给它发送MOUSELEAVE
                hover = -1;
            }
            return;
        }
        if (n == 214) {
            auto& hover = cvm::global_state.window_hover;
            if (hover != -1) { // 当前有悬停窗口
                post_data(hover, WM_MOUSEHOVER); // 给它发送MOUSEHOVER
            }
            return;
        }
        if (wnds.empty())
            return;
        for (auto i = wnds.rbegin(); i != wnds.rend(); i++) {
            if ((*i)->hit(n, global_state.mouse_x, global_state.mouse_y)) {
                return;
            }
        }
        if (n == 211) { // 没有找到活动窗口
            auto& hover = cvm::global_state.window_hover;
            if (hover != -1) { // 当前有悬停窗口
                post_data(hover, WM_MOUSELEAVE); // 给它发送MOUSELEAVE
                hover = -1; // 清空
            }
        }
        else if (n == 200) { // 没有找到焦点
            auto& focus = cvm::global_state.window_focus;
            if (focus != -1) { // 当前有焦点
                post_data(focus, WM_KILLFOCUS); // 给它发送KILLFOCUS
                focus = -1; // 清空
            }
        }
    }

    int cvm::cursor() const
    {
        if (global_state.window_hover != -1 && handles[global_state.window_hover]) {
            const auto& h = handles[global_state.window_hover].get();
            if (h->type = h_window) {
                return h->data.cwnd->get_cursor();
            }
        }
        return 1;
    }
}
