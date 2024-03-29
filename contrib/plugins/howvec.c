/*
 * Copyright (C) 2019, Alex Bennée <alex.bennee@linaro.org>
 *
 * How vectorised is this code?
 *
 * Attempt to measure the amount of vectorisation that has been done
 * on some code by counting classes of instruction.
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */
#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef enum {
    COUNT_CLASS,
    COUNT_INDIVIDUAL,
    COUNT_NONE
} CountType;

static int limit = 50;
static bool do_inline;
static bool verbose;

static GMutex lock;
static GHashTable *insns;

typedef struct {
    const char *class;
    const char *opt;
    uint32_t mask;
    uint32_t pattern;
    CountType what;
    uint64_t count;
} InsnClassExecCount;

typedef struct {
    char *insn;
    uint32_t opcode;
    uint64_t count;
    InsnClassExecCount *class;
} InsnExecCount;

/*
 * Matchers for classes of instructions, order is important.
 *
 * Your most precise match must be before looser matches. If no match
 * is found in the table we can create an individual entry.
 *
 * 31..28 27..24 23..20 19..16 15..12 11..8 7..4 3..0
 */
static InsnClassExecCount aarch64_insn_classes[] = {
    /* "Reserved"" */
    { "  UDEF",              "udef",   0xffff0000, 0x00000000, COUNT_NONE},
    { "  SVE",               "sve",    0x1e000000, 0x04000000, COUNT_CLASS},
    { "Reserved",            "res",    0x1e000000, 0x00000000, COUNT_CLASS},
    /* Data Processing Immediate */
    { "  PCrel addr",        "pcrel",  0x1f000000, 0x10000000, COUNT_CLASS},
    { "  Add/Sub (imm,tags)", "asit",   0x1f800000, 0x11800000, COUNT_CLASS},
    { "  Add/Sub (imm)",     "asi",    0x1f000000, 0x11000000, COUNT_CLASS},
    { "  Logical (imm)",     "logi",   0x1f800000, 0x12000000, COUNT_CLASS},
    { "  Move Wide (imm)",   "movwi",  0x1f800000, 0x12800000, COUNT_CLASS},
    { "  Bitfield",          "bitf",   0x1f800000, 0x13000000, COUNT_CLASS},
    { "  Extract",           "extr",   0x1f800000, 0x13800000, COUNT_CLASS},
    { "Data Proc Imm",       "dpri",   0x1c000000, 0x10000000, COUNT_CLASS},
    /* Branches */
    { "  Cond Branch (imm)", "cndb",   0xfe000000, 0x54000000, COUNT_CLASS},
    { "  Exception Gen",     "excp",   0xff000000, 0xd4000000, COUNT_CLASS},
    { "    NOP",             "nop",    0xffffffff, 0xd503201f, COUNT_NONE},
    { "  Hints",             "hint",   0xfffff000, 0xd5032000, COUNT_CLASS},
    { "  Barriers",          "barr",   0xfffff000, 0xd5033000, COUNT_CLASS},
    { "  PSTATE",            "psta",   0xfff8f000, 0xd5004000, COUNT_CLASS},
    { "  System Insn",       "sins",   0xffd80000, 0xd5080000, COUNT_CLASS},
    { "  System Reg",        "sreg",   0xffd00000, 0xd5100000, COUNT_CLASS},
    { "  Branch (reg)",      "breg",   0xfe000000, 0xd6000000, COUNT_CLASS},
    { "  Branch (imm)",      "bimm",   0x7c000000, 0x14000000, COUNT_CLASS},
    { "  Cmp & Branch",      "cmpb",   0x7e000000, 0x34000000, COUNT_CLASS},
    { "  Tst & Branch",      "tstb",   0x7e000000, 0x36000000, COUNT_CLASS},
    { "Branches",            "branch", 0x1c000000, 0x14000000, COUNT_CLASS},
    /* Loads and Stores */
    { "  AdvSimd ldstmult",  "advlsm", 0xbfbf0000, 0x0c000000, COUNT_CLASS},
    { "  AdvSimd ldstmult++", "advlsmp", 0xbfb00000, 0x0c800000, COUNT_CLASS},
    { "  AdvSimd ldst",      "advlss", 0xbf9f0000, 0x0d000000, COUNT_CLASS},
    { "  AdvSimd ldst++",    "advlssp", 0xbf800000, 0x0d800000, COUNT_CLASS},
    { "  ldst excl",         "ldstx",  0x3f000000, 0x08000000, COUNT_CLASS},
    { "    Prefetch",        "prfm",   0xff000000, 0xd8000000, COUNT_CLASS},
    { "  Load Reg (lit)",    "ldlit",  0x1b000000, 0x18000000, COUNT_CLASS},
    { "  ldst noalloc pair", "ldstnap", 0x3b800000, 0x28000000, COUNT_CLASS},
    { "  ldst pair",         "ldstp",  0x38000000, 0x28000000, COUNT_CLASS},
    { "  ldst reg",          "ldstr",  0x3b200000, 0x38000000, COUNT_CLASS},
    { "  Atomic ldst",       "atomic", 0x3b200c00, 0x38200000, COUNT_CLASS},
    { "  ldst reg (reg off)", "ldstro", 0x3b200b00, 0x38200800, COUNT_CLASS},
    { "  ldst reg (pac)",    "ldstpa", 0x3b200200, 0x38200800, COUNT_CLASS},
    { "  ldst reg (imm)",    "ldsti",  0x3b000000, 0x39000000, COUNT_CLASS},
    { "Loads & Stores",      "ldst",   0x0a000000, 0x08000000, COUNT_CLASS},
    /* Data Processing Register */
    { "Data Proc Reg",       "dprr",   0x0e000000, 0x0a000000, COUNT_CLASS},
    /* Scalar FP */
    { "Scalar FP ",          "fpsimd", 0x0e000000, 0x0e000000, COUNT_CLASS},
    /* Unclassified */
    { "Unclassified",        "unclas", 0x00000000, 0x00000000, COUNT_CLASS},
};

static InsnClassExecCount sparc32_insn_classes[] = {
    { "Call",                "call",   0xc0000000, 0x40000000, COUNT_CLASS},
    { "Branch ICond",        "bcc",    0xc1c00000, 0x00800000, COUNT_CLASS},
    { "Branch Fcond",        "fbcc",   0xc1c00000, 0x01800000, COUNT_CLASS},
    { "SetHi",               "sethi",  0xc1c00000, 0x01000000, COUNT_CLASS},
    { "FPU ALU",             "fpu",    0xc1f00000, 0x81a00000, COUNT_CLASS},
    { "ALU",                 "alu",    0xc0000000, 0x80000000, COUNT_CLASS},
    { "Load/Store",          "ldst",   0xc0000000, 0xc0000000, COUNT_CLASS},
    /* Unclassified */
    { "Unclassified",        "unclas", 0x00000000, 0x00000000, COUNT_INDIVIDUAL},
};

static InsnClassExecCount sparc64_insn_classes[] = {
    { "SetHi & Branches",     "op0",   0xc0000000, 0x00000000, COUNT_CLASS},
    { "Call",                 "op1",   0xc0000000, 0x40000000, COUNT_CLASS},
    { "Arith/Logical/Move",   "op2",   0xc0000000, 0x80000000, COUNT_CLASS},
    { "Arith/Logical/Move",   "op3",   0xc0000000, 0xc0000000, COUNT_CLASS},
    /* Unclassified */
    { "Unclassified",        "unclas", 0x00000000, 0x00000000, COUNT_INDIVIDUAL},
};

static InsnClassExecCount riscv64_insn_classes[] = {
    { "Jal",         "jal", 0x0000007f, 0b1101111, COUNT_CLASS}, // 0
    { "Jalr",        "jalr", 0x0000007f, 0b1100111, COUNT_CLASS}, // 1
    { "Branch",      "br", 0x0000007f, 0b1100011, COUNT_CLASS}, // 2
    { "Load",        "ld", 0x0000007f, 0b0000011, COUNT_CLASS}, // 3
    { "Store",        "st", 0x0000007f, 0b0100011, COUNT_CLASS}, // 4
    { "Fence",        "fence", 0x0000007f, 0b0001111, COUNT_CLASS}, // 5
    { "Csr", "csr", 0x0000007f, 0b1110011, COUNT_CLASS}, // 6
    { "Mul/Div", "mul/div", 0x0000007f, 0b0110011 & 0b0111011, COUNT_CLASS}, // 7
    { "Atomic", "atomic", 0x0000007f, 0b0101111, COUNT_CLASS}, // 8
    { "Alu",   "alu", 0x7f, 0x7f, COUNT_CLASS}, // 9
    { "FLD", "fld", 0x7f, 0b0000111, COUNT_CLASS},
    { "FSD", "fsd", 0x7f, 0b0100111, COUNT_CLASS},
    { "FAlu", "falu", 0x40, 0b1000000, COUNT_CLASS},
    { "Unclassified",        "unclas", 0x00000000, 0x00000000, COUNT_INDIVIDUAL},
};

/* Default matcher for currently unclassified architectures */
static InsnClassExecCount default_insn_classes[] = {
    { "Unclassified",        "unclas", 0x00000000, 0x00000000, COUNT_INDIVIDUAL},
};

typedef struct {
    const char *qemu_target;
    InsnClassExecCount *table;
    int table_sz;
} ClassSelector;

static ClassSelector class_tables[] = {
    { "aarch64", aarch64_insn_classes, ARRAY_SIZE(aarch64_insn_classes) },
    { "sparc",   sparc32_insn_classes, ARRAY_SIZE(sparc32_insn_classes) },
    { "sparc64", sparc64_insn_classes, ARRAY_SIZE(sparc64_insn_classes) },
    { "riscv64", riscv64_insn_classes, ARRAY_SIZE(riscv64_insn_classes) },
    { NULL, default_insn_classes, ARRAY_SIZE(default_insn_classes) },
};

static InsnClassExecCount *class_table;
static int class_table_sz;

static gint cmp_exec_count(gconstpointer a, gconstpointer b)
{
    InsnExecCount *ea = (InsnExecCount *) a;
    InsnExecCount *eb = (InsnExecCount *) b;
    return ea->count > eb->count ? -1 : 1;
}

static void free_record(gpointer data)
{
    InsnExecCount *rec = (InsnExecCount *) data;
    g_free(rec->insn);
    g_free(rec);
}

static void reset_stat() {
    g_autoptr(GString) report = g_string_new("Instruction Classes:\n");
    int i;
    GList *counts;
    InsnClassExecCount *class = NULL;

    for (i = 0; i < class_table_sz; i++) {
        class = &class_table[i];
        switch (class->what) {
        case COUNT_CLASS:
            if (class->count || verbose) {
                g_string_append_printf(report,
                                       "Class: %-24s\t(%" PRId64 " hits)\n",
                                       class->class,
                                       class->count);
            }
            break;
        case COUNT_INDIVIDUAL:
            g_string_append_printf(report, "Class: %-24s\tcounted individually\n",
                                   class->class);
            break;
        case COUNT_NONE:
            g_string_append_printf(report, "Class: %-24s\tnot counted\n",
                                   class->class);
            break;
        default:
            break;
        }
        class->count = 0;
    }

    counts = g_hash_table_get_values(insns);
    if (counts && g_list_next(counts)) {
        g_string_append_printf(report, "Individual Instructions:\n");
        counts = g_list_sort(counts, cmp_exec_count);

        for (i = 0; i < limit && g_list_next(counts);
             i++, counts = g_list_next(counts)) {
            InsnExecCount *rec = (InsnExecCount *) counts->data;
            g_string_append_printf(report,
                                   "Instr: %-24s\t(%" PRId64 " hits)"
                                   "\t(op=0x%08x/%s)\n",
                                   rec->insn,
                                   rec->count,
                                   rec->opcode,
                                   rec->class ?
                                   rec->class->class : "un-categorised");
            rec->count = 0;
        }
        g_list_free(counts);
    }

    g_hash_table_destroy(insns);

    FILE *f = fopen("/tmp/stat.txt", "a+");
    fwrite(report->str, 1, report->len, f);
    fclose(f);
    qemu_plugin_outs(report->str);
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) report = g_string_new("Instruction Classes:\n");
    int i;
    GList *counts;
    InsnClassExecCount *class = NULL;

    for (i = 0; i < class_table_sz; i++) {
        class = &class_table[i];
        switch (class->what) {
        case COUNT_CLASS:
            if (class->count || verbose) {
                g_string_append_printf(report,
                                       "Class: %-24s\t(%" PRId64 " hits)\n",
                                       class->class,
                                       class->count);
            }
            break;
        case COUNT_INDIVIDUAL:
            g_string_append_printf(report, "Class: %-24s\tcounted individually\n",
                                   class->class);
            break;
        case COUNT_NONE:
            g_string_append_printf(report, "Class: %-24s\tnot counted\n",
                                   class->class);
            break;
        default:
            break;
        }
    }

    counts = g_hash_table_get_values(insns);
    if (counts && g_list_next(counts)) {
        g_string_append_printf(report, "Individual Instructions:\n");
        counts = g_list_sort(counts, cmp_exec_count);

        for (i = 0; i < limit && g_list_next(counts);
             i++, counts = g_list_next(counts)) {
            InsnExecCount *rec = (InsnExecCount *) counts->data;
            g_string_append_printf(report,
                                   "Instr: %-24s\t(%" PRId64 " hits)"
                                   "\t(op=0x%08x/%s)\n",
                                   rec->insn,
                                   rec->count,
                                   rec->opcode,
                                   rec->class ?
                                   rec->class->class : "un-categorised");
        }
        g_list_free(counts);
    }

    g_hash_table_destroy(insns);

    qemu_plugin_outs(report->str);
}

static void plugin_init(void)
{
    insns = g_hash_table_new_full(NULL, g_direct_equal, NULL, &free_record);
}

static void vcpu_insn_exec_before(unsigned int cpu_index, void *udata)
{
    uint64_t *count = (uint64_t *) udata;
    (*count)++;
}

static int stat_flag = 0;

static uint64_t *find_counter(struct qemu_plugin_insn *insn)
{
    int i;
    uint64_t *cnt = NULL;
    uint32_t opcode;
    InsnClassExecCount *class = NULL;

    /*
     * We only match the first 32 bits of the instruction which is
     * fine for most RISCs but a bit limiting for CISC architectures.
     * They would probably benefit from a more tailored plugin.
     * However we can fall back to individual instruction counting.
     */
    opcode = *((uint32_t *)qemu_plugin_insn_data(insn));
    if (opcode == 0x7f) {
        if (stat_flag) {
            reset_stat();
        }
        stat_flag = !stat_flag;
    }
    if (!stat_flag) {
        return NULL;
    }

    uint32_t quo = opcode & 0b11;
    uint32_t bit_15 = (opcode >> 15) & 1;
    uint32_t bit_12 = (opcode >> 12) & 1;
    uint32_t bit_15_13 = (opcode >> 13) & 0b111;
    uint32_t bit_11_7 = (opcode >> 7) & 0b11111;
    uint32_t bit_6_2 = (opcode >> 2) & 0b11111;
    uint32_t bit_6_0 = opcode & 0x7f;
    const int jal_idx = 0;
    const int jalr_idx = 1;
    const int br_idx = 2;
    const int ld_idx = 3;
    const int st_idx = 4;
    const int alu_idx = 9;
    if (quo == 0) {
        class = &class_table[
            bit_15 ? ld_idx : st_idx
        ];
    } else if (quo == 1) {
        if (bit_15_13 == 0b111 || bit_15_13 == 0b110) {
            class = &class_table[br_idx];
        } else if (bit_15_13 == 0b101 || bit_15_13 == 0b001) {
            class = &class_table[jal_idx];
        } else {
            class = &class_table[alu_idx];
        }
    } else if (quo == 2) {
        if (bit_15_13 >= 0b001 && bit_15_13 <= 0b011) {
            class = &class_table[ld_idx];
        } else if (bit_15_13 >= 0b101) {
            class = &class_table[st_idx];
        } else if (bit_15_13 == 0b100) {
            if (bit_12 == 0 && bit_6_2 == 0) {
                class = &class_table[jal_idx];
            } else if (bit_12 == 1 && bit_11_7 != 0 && bit_6_2 == 0) {
                class = &class_table[jalr_idx];
            } else {
                class = &class_table[alu_idx];
            }
        } else {
            class = &class_table[alu_idx];
        }
    } else {
        if (bit_6_0 == 0b0110111 ||
            bit_6_0 == 0b0010111 ||
            bit_6_0 == 0b0010011 ||
            bit_6_0 == 0b0110011 ||
            bit_6_0 == 0b0011011 ||
            bit_6_0 == 0b0111011
        ) {
            class = &class_table[alu_idx];
        } else {
            for (i = 0; !cnt && i < class_table_sz; i++) {
                class = &class_table[i];
                uint32_t masked_bits = opcode & class->mask;
                if (masked_bits == class->pattern) {
                    break;
                }
            }
        }
    }

    g_assert(class);

    switch (class->what) {
    case COUNT_NONE:
        return NULL;
    case COUNT_CLASS:
        return &class->count;
    case COUNT_INDIVIDUAL:
    {
        InsnExecCount *icount;

        g_mutex_lock(&lock);
        icount = (InsnExecCount *) g_hash_table_lookup(insns,
                                                       GUINT_TO_POINTER(opcode));

        if (!icount) {
            icount = g_new0(InsnExecCount, 1);
            icount->opcode = opcode;
            icount->insn = qemu_plugin_insn_disas(insn);
            icount->class = class;

            g_hash_table_insert(insns, GUINT_TO_POINTER(opcode),
                                (gpointer) icount);
        }
        g_mutex_unlock(&lock);

        return &icount->count;
    }
    default:
        g_assert_not_reached();
    }

    return NULL;
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;

    for (i = 0; i < n; i++) {
        uint64_t *cnt;
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        cnt = find_counter(insn);

        if (cnt) {
            if (do_inline) {
                qemu_plugin_register_vcpu_insn_exec_inline(
                    insn, QEMU_PLUGIN_INLINE_ADD_U64, cnt, 1);
            } else {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, vcpu_insn_exec_before, QEMU_PLUGIN_CB_NO_REGS, cnt);
            }
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int i;

    /* Select a class table appropriate to the guest architecture */
    for (i = 0; i < ARRAY_SIZE(class_tables); i++) {
        ClassSelector *entry = &class_tables[i];
        if (!entry->qemu_target ||
            strcmp(entry->qemu_target, info->target_name) == 0) {
            class_table = entry->table;
            class_table_sz = entry->table_sz;
            break;
        }
    }

    for (i = 0; i < argc; i++) {
        char *p = argv[i];
        g_auto(GStrv) tokens = g_strsplit(p, "=", -1);
        if (g_strcmp0(tokens[0], "inline") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_inline)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", p);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "verbose") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &verbose)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", p);
                return -1;
            }
        } else if (g_strcmp0(tokens[0], "count") == 0) {
            char *value = tokens[1];
            int j;
            CountType type = COUNT_INDIVIDUAL;
            if (*value == '!') {
                type = COUNT_NONE;
                value++;
            }
            for (j = 0; j < class_table_sz; j++) {
                if (strcmp(value, class_table[j].opt) == 0) {
                    class_table[j].what = type;
                    break;
                }
            }
        } else {
            fprintf(stderr, "option parsing failed: %s\n", p);
            return -1;
        }
    }

    plugin_init();

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
