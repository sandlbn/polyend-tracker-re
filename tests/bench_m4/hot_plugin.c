/* QEMU TCG plugin for the bench-m4 rig (make -C tests bench-m4-hot): counts
 * the instructions executed per translation block start address and writes
 * "start end count insns" lines (hex start / end address, decimal executions
 * and instructions per execution) to the file given as out=<path> at exit.
 * tests/bench_m4/hot_funcs.py folds them into functions (nm) to pick the
 * code that goes to SRAM_L (.fastrun, docs/OPTIMIZATION_2X.md item 6).
 *
 * [I] phase=<hex address> (fork #21, make -C tests bench-m4-synth-modes): each
 * time the block at that address (the bench's otHotPhase()) runs, the counts
 * so far are written as one phase ("P <k>" then its lines) and start again
 * from zero; the last phase is written at exit. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    uint64_t start, end, count;
    uint32_t insns;
    int used;
} Tb;

#define TB_SLOTS (1u << 18)
static Tb *tbs;
static char out_path[512] = "hot.txt";
static uint64_t phase_addr = 0;   /* 0: no phases */
static int phase_no = 0;
static FILE *phase_out = NULL;

static Tb *slot(uint64_t start, uint32_t insns) {
    uint64_t h = (start * 0x9E3779B97F4A7C15ull ^ insns) & (TB_SLOTS - 1);
    for (;;) {
        Tb *t = &tbs[h];
        if (!t->used) {
            t->used = 1;
            t->start = start;
            t->insns = insns;
            return t;
        }
        if (t->start == start && t->insns == insns) return t;
        h = (h + 1) & (TB_SLOTS - 1);
    }
}

static void on_exec(unsigned int vcpu, void *udata) {
    (void)vcpu;
    ((Tb *)udata)->count++;
}

static void dump(FILE *f, int reset) {
    for (uint32_t i = 0; i < TB_SLOTS; ++i)
        if (tbs[i].used && tbs[i].count) {
            fprintf(f, "%" PRIx64 " %" PRIx64 " %" PRIu64 " %u\n", tbs[i].start, tbs[i].end, tbs[i].count,
                    tbs[i].insns);
            if (reset) tbs[i].count = 0;
        }
}

static void on_phase(unsigned int vcpu, void *udata) {
    (void)vcpu;
    (void)udata;
    if (!phase_out) return;
    fprintf(phase_out, "P %d\n", phase_no++);
    dump(phase_out, 1);
}

static void on_trans(struct qemu_plugin_tb *tb, void *udata) {
    (void)udata;
    const size_t n = qemu_plugin_tb_n_insns(tb);
    if (!n) return;
    if (phase_addr && (qemu_plugin_tb_vaddr(tb) & ~1ull) == phase_addr)
        qemu_plugin_register_vcpu_tb_exec_cb(tb, on_phase, QEMU_PLUGIN_CB_NO_REGS, NULL);
    struct qemu_plugin_insn *last = qemu_plugin_tb_get_insn(tb, n - 1);
    Tb *t = slot(qemu_plugin_tb_vaddr(tb), (uint32_t)n);
    t->end = qemu_plugin_insn_vaddr(last) + qemu_plugin_insn_size(last);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, on_exec, QEMU_PLUGIN_CB_NO_REGS, t);
}

static void at_exit(void *p) {
    (void)p;
    if (phase_out) {
        on_phase(0, NULL);
        fclose(phase_out);
        return;
    }
    FILE *f = fopen(out_path, "w");
    if (!f) return;
    dump(f, 0);
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
    (void)info;
    for (int i = 0; i < argc; ++i)
        if (strncmp(argv[i], "out=", 4) == 0) snprintf(out_path, sizeof out_path, "%s", argv[i] + 4);
        else if (strncmp(argv[i], "phase=", 6) == 0) phase_addr = strtoull(argv[i] + 6, NULL, 16) & ~1ull;
    tbs = calloc(TB_SLOTS, sizeof(Tb));
    if (!tbs) return -1;
    if (phase_addr && !(phase_out = fopen(out_path, "w"))) return -1;
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_trans, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
