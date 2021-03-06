/*
 * Redistribution and use in source and binary forms, with
 * or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain this list
 *    of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce this
 *    list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */
#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "program.h"
#include "thecl.h"
#include "util.h"
#include "mygetopt.h"

const char* gool_ename_charmap = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_!";
const int gool_null_eid = 0x6396347F;
const char* gool_null_ename = "NONE!";

parser_state_t* g_parser_state = NULL;
int g_rate = 30; /* ntsc default */
char* g_region = NULL;
char g_lev = '0';
int g_reg_block_depth = 0;
int* g_reg_blocks = NULL;
char* g_module_fmt = NULL;

bool g_warn_deprecate_getcolor = true;
bool g_warn_deprecate_setcolor = true;
bool g_warn_deprecate_setfield = true;
bool g_warn_deprecate_getfield = true;
bool g_warn_deprecate_anim = true;

#define PI 3.1415926535897932384626433832795028841971693993751058209749445923078164062
short sine_table[1025];
static void
make_sin_table(
    void)
{
    for (int i = 0; i <= 1024; ++i) {
        sine_table[i] = (short)lround(sin(i / 1024.0 * (PI / 2.0)) * 0x1000);
    }
}

int
sin_psx(
    int r)
{
    r &= 0xfff;
    if (r < 0x400) {
        return sine_table[r];
    }
    else if (r < 0x800) {
        return sine_table[0x800 - r];
    }
    else if (r < 0xC00) {
        return -sine_table[r - 0x800];
    }
    else {
        return -sine_table[0x1000 - r];
    }
}
#undef PI

extern const thecl_module_t c1_gool;
extern const thecl_module_t c2_gool;

bool g_was_error = false;

thecl_t*
thecl_new(
    void)
{
    thecl_t* ecl = malloc(sizeof(thecl_t));
    list_init(&ecl->subs);
    list_init(&ecl->states);
    list_init(&ecl->anims);
    list_init(&ecl->spawns);
    list_init(&ecl->interrupts);
    ecl->vars = malloc(0);
    ecl->consts = malloc(0);
    ecl->eids = malloc(0);
    list_init(&ecl->arrays);
    ecl->var_count = 0;
    ecl->const_count = 0;
    ecl->eid_count = 0;
    ecl->array_off = 0;
    ecl->no_warn = false;
    ecl->eid = gool_null_eid;
    ecl->id = 0;
    ecl->type = 0;
    ecl->is_defined = 0;
    ecl->ins_offset = 0;
    ecl->purge_data = false;
    ecl->purge_tested = false;
    return ecl;
}

static void
thecl_free(
    thecl_t* ecl)
{
    size_t datacount = 0;
    gool_sub_t** datas = malloc(0);

    free(ecl->consts);

    for (size_t v = 0; v < ecl->var_count; ++v)
        free(ecl->vars[v]);
    free(ecl->vars);

    thecl_sub_t* sub;
    list_for_each(&ecl->subs, sub) {
        if (sub->instr_data && !sub->deleted) {
            datas = realloc(datas, sizeof(gool_sub_t*) * ++datacount);
            datas[datacount - 1] = sub->instr_data;
        }

        thecl_instr_t* instr;
        list_for_each(&sub->instrs, instr) {
            thecl_instr_free(instr);
        }
        list_free_nodes(&sub->instrs);

        for (size_t v = 0; v < sub->var_count; ++v)
            thecl_variable_free(sub->vars[v]);
        free(sub->vars);

        thecl_label_t* label;
        list_for_each(&sub->labels, label) {
            free(label);
        }
        list_free_nodes(&sub->labels);

        thecl_line_t* line;
        list_for_each(&sub->lines, line) {
            thecl_param_t* p;
            switch (line->type) {
            case LINE_ASSIGNMENT:
                param_free(line->ass.address);
                expression_free(line->ass.expr);
                break;
            case LINE_INSTRUCTION:
            case LINE_CALL:
                free(line->call.name);
                if (line->call.expr_list) {
                    expression_t* e;
                    list_for_each(line->call.expr_list, e) {
                        expression_free(e);
                    }
                    list_free_nodes(line->call.expr_list);
                    free(line->call.expr_list);
                }
                break;
            case LINE_LABEL:
                free(line->label);
                break;
            case LINE_GOTO:
                if (line->go.expr) expression_free(line->go.expr);
                free(line->go.label);
                break;
            case LINE_LOAD:
                expression_free(line->expression);
                break;
            case LINE_VAR_DECL_ASSIGN:
                expression_free(line->var.expr);
            case LINE_VAR_DECL: /* intentional fallthrough */
                free(line->var.name);
                break;
            case LINE_SAVE_START:
                list_for_each(line->list, p) {
                    param_free(p);
                }
                list_free_nodes(line->list);
                free(line->list);
                break;
            }
            free(line);
        }
        list_free_nodes(&sub->lines);

        free(sub->name);
        free(sub);
    }
    list_free_nodes(&ecl->subs);

    for (size_t i = 0; i < datacount; ++i) {
        free(datas[i]);
    }
    free(datas);

    thecl_state_t* state;
    list_for_each(&ecl->states, state) {
        if (state->code) { free(state->code->lambda_name); free(state->code); }
        if (state->event) { free(state->event->lambda_name); free(state->event); }
        if (state->trans) { free(state->trans->lambda_name); free(state->trans); }
        free(state->name);
        free(state);
    }
    list_free_nodes(&ecl->states);

    gool_anim_t* anim;
    list_for_each(&ecl->anims, anim) {
        free(anim->name);

        free(anim->anim);
        free(anim);
    }
    list_free_nodes(&ecl->anims);

    thecl_spawn_t* spawn;
    list_for_each(&ecl->spawns, spawn) {
        free(spawn->name);

        free(spawn->state_name);
        free(spawn);
    }
    list_free_nodes(&ecl->spawns);

    thecl_interrupt_t* interrupt;
    list_for_each(&ecl->interrupts, interrupt) {
        free(interrupt->lambda_name);
        free(interrupt->event);

        free(interrupt);
    }
    list_free_nodes(&ecl->interrupts);

    gooc_array_t* array;
    list_for_each(&ecl->arrays, array) {
        free(array->data);
        free(array);
    }
    list_free_nodes(&ecl->arrays);

    free(ecl);
}

char* gool_to_ename(
    char* ename,
    int eid)
{
    for (int i = 0; i < 5; ++i) {
        ename[4 - i] = gool_ename_charmap[(eid >> (1 + 6 * i)) & 0x3F];
    }
    ename[5] = '\0';

    return ename;
}

int gool_to_eid(
    const char* ename)
{
    if (strlen(ename) != 5) {
        fprintf(stderr, "%s: ename is not 5 characters long: %s\n", argv0, ename);
        return gool_null_eid;
    }
    int i = 5;
    int eid = 1;
    while (i--) {
        int c = strchr_o(gool_ename_charmap, ename[i]);
        if (strchr(gool_ename_charmap, ename[i]) == NULL) {
            fprintf(stderr, "%s: ename has invalid character '%c': %s\n", argv0, ename[i], ename);
            return gool_null_eid;
        }
        eid |= c << (1 + 6*(4-i));
    }
    return eid;
}

int eid_pool_get_index(
    thecl_t* ecl,
    uint32_t eid)
{
    for (int i = 0; i < ecl->eid_count; ++i) {
        if (ecl->eids[i] == eid)
            return i;
    }
    return -1;
}

int eid_pool_force_get_index(
    thecl_t* ecl,
    uint32_t eid)
{
    for (int i = 0; i < ecl->eid_count; ++i) {
        if (ecl->eids[i] == eid)
            return i;
    }
    ecl->eids = realloc(ecl->eids, (ecl->eid_count + 1) * sizeof(uint32_t));
    ecl->eids[ecl->eid_count] = eid;
    return ecl->eid_count++;
}

int gool_pool_get_index(
    thecl_t* ecl,
    uint32_t val)
{
    for (int i = 0; i < ecl->const_count; ++i) {
        if (ecl->consts[i] == val)
            return i;
    }
    return -1;
}

int gool_pool_force_get_index(
    thecl_t* ecl,
    uint32_t val)
{
    for (int i = 0; i < ecl->const_count; ++i) {
        if (ecl->consts[i] == val)
            return i;
    }
    ecl->consts = realloc(ecl->consts, (ecl->const_count + 1) * sizeof(uint32_t));
    ecl->consts[ecl->const_count] = val;
    return ecl->const_count++;
}

int gool_pool_force_make_index(
    thecl_t* ecl,
    uint32_t val)
{
    ecl->consts = realloc(ecl->consts, (ecl->const_count + 1) * sizeof(uint32_t));
    ecl->consts[ecl->const_count] = val;
    return ecl->const_count++;
}

thecl_instr_t*
thecl_instr_new(void)
{
    thecl_instr_t* instr = calloc(1, sizeof(thecl_instr_t));
    instr->type = THECL_INSTR_INSTR;
    list_init(&instr->params);
    instr->mips = false;
    return instr;
}

thecl_instr_t*
thecl_instr_label(unsigned int offset)
{
    thecl_instr_t* instr = thecl_instr_new();
    instr->type = THECL_INSTR_LABEL;
    instr->offset = offset;
    return instr;
}

void
thecl_instr_free(thecl_instr_t* instr)
{
    free(instr->string);

    thecl_param_t* param;
    list_for_each(&instr->params, param) {
        value_free(&param->value);
        free(param);
    }
    list_free_nodes(&instr->params);

    free(instr);
}

void
thecl_variable_free(thecl_variable_t* var) {
    free(var->name);
    free(var);
}

thecl_param_t*
param_new(
    int type)
{
    thecl_param_t* param = calloc(1, sizeof(thecl_param_t));
    param->type = type;
    param->value.type = type;
    param->is_expression_param = 0;
    param->object_link = -1;
    param->val_type = PARAM_LITERAL;
    param->is_eid = 0;
    return param;
}

thecl_param_t*
param_copy(
    thecl_param_t* param)
{
    thecl_param_t* copy = malloc(sizeof(thecl_param_t));
    memcpy(copy, param, sizeof(thecl_param_t));
    /* Handle possible pointers in the value. */
    if (copy->value.type == 'z')
        copy->value.val.z = strdup(param->value.val.z);
    return copy;
}

void
param_free(
    thecl_param_t* param)
{
    value_free(&param->value);
    free(param);
}

thecl_param_t*
param_val_new(
    int val)
{
    thecl_param_t* param = param_new('S');
    param->value.val.S = val;
    return param;
}

thecl_param_t*
param_sp_new(void)
{
    thecl_param_t* param = param_new('S');
    param->value.val.S = 0x1F;
    param->val_type = PARAM_FIELD;
    param->object_link = 0;
    return param;
}

thecl_param_t*
param_sp2_new(void)
{
    thecl_param_t* param = param_new('S');
    param->value.val.S = 1;
    param->val_type = PARAM_FIELD;
    param->object_link = -2;
    return param;
}

thecl_param_t*
param_var_new(
    char* var)
{
    thecl_param_t* param = param_new('S');
    param->value.val.S = field_get(var)->offset;
    param->object_link = 0;
    param->val_type = PARAM_FIELD;
    return param;
}

thecl_param_t*
param_null_new(
    void)
{
    thecl_param_t* param = param_new('S');
    param->value.val.S = 0;
    param->val_type = PARAM_FIELD;
    param->object_link = -2;
    return param;
}

thecl_label_t*
label_find(
    thecl_sub_t* sub,
    const char* name)
{
    thecl_label_t* label;
    list_for_each(&sub->labels, label) {
        if (strcmp(label->name, name) == 0)
            return label;
    }
    return NULL;
}

int32_t
label_offset(
    thecl_sub_t* sub,
    const char* name)
{
    thecl_label_t* label;
    list_for_each(&sub->labels, label) {
        if (strcmp(label->name, name) == 0)
            return label->offset;
    }
    fprintf(stderr, "%s: label not found: %s\n", argv0, name);
    return 0;
}

void
expression_free(
    expression_t* expr)
{
    expression_t* child_expr;
    if (expr->type == EXPRESSION_OP || expr->type == EXPRESSION_TERNARY) {
        list_for_each(&expr->children, child_expr)
            expression_free(child_expr);
        list_free_nodes(&expr->children);
    }
    free(expr);
}

bool
expression_is_number(
    expression_t* expr)
{
    return expr->type == EXPRESSION_VAL && expr->value->val_type == PARAM_LITERAL;
}

bool
expression_is_zero(
    expression_t* expr)
{
    return expression_is_number(expr) && expr->value->value.val.S == 0;
}

expr_macro_t*
macro_get(
    parser_state_t* state,
    const char* name
) {
    expr_macro_t* macro;
    list_for_each(&state->expr_macros, macro) {
        if (!strcmp(name, macro->name))
            return macro;
    }
    return NULL;
}

thecl_line_t*
line_make(
    enum thecl_line_type type)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = type;
    return line;
}

thecl_line_t*
line_make_var_decl(
    const char* name)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_VAR_DECL;
    line->var.name = strdup(name);
    line->var.expr = NULL;
    return line;
}

thecl_line_t*
line_make_var_decl_assign(
    const char* name,
    expression_t* expression)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_VAR_DECL_ASSIGN;
    line->var.name = strdup(name);
    line->var.expr = expression;
    return line;
}

thecl_line_t*
line_make_assignment(
    thecl_param_t* address,
    expression_t* expression)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_ASSIGNMENT;
    line->ass.address = address;
    line->ass.expr = expression;
    return line;
}

thecl_line_t*
line_make_call(
    const char* name,
    list_t* expr_list)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_CALL;
    line->call.name = strdup(name);
    line->call.expr_list = expr_list;
    return line;
}

thecl_line_t*
line_make_label(
    const char* label_name)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_LABEL;
    line->name = label_name;
    return line;
}

thecl_line_t*
line_make_goto(
    int type,
    const char* label_name,
    expression_t* expression)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_GOTO;
    line->go.type = type;
    line->go.label = label_name;
    line->go.expr = expression;
    return line;
}

thecl_line_t*
line_make_save_start(
    list_t* address_list)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_SAVE_START;
    line->list = address_list;
    return line;
}

thecl_line_t*
line_make_save_end(void)
{
    thecl_line_t* line = calloc(1, sizeof(thecl_line_t));
    line->type = LINE_SAVE_END;
    return line;
}

int
get_obj_proc_offset(
    unsigned int version)
{
    return is_post_c2(version) ? 0x40 : 0x60;
}

bool
is_post_c2(
    unsigned int version)
{
    switch (version) {
    case 2: case 3: return true;
    default: return false;
    }
}

static const char* kana[] = {
    "あ", "い", "う", "え", "お",
    "か", "き", "く", "け", "こ",
    "さ", "し", "す", "せ", "そ",
    "た", "ち", "つ", "て", "と",
    "な", "に", "ぬ", "ね", "の",
    "は", "ひ", "ふ", "へ", "ほ",
    "ま", "み", "む", "め", "も",
    "や", "ゆ", "よ", "ら", "り",
    "る", "れ", "ろ", "わ", "を",
    "ん", "ぁ", "ぃ", "ぅ", "ぇ",
    "ぉ", "ゃ", "ゅ", "ょ", "っ",
    "ー", "「", "」", "。", "、",
    "゜", "゛", "゛", NULL,
    "ア", "イ", "ウ", "エ", "オ",
    "カ", "キ", "ク", "ケ", "コ",
    "サ", "シ", "ス", "セ", "ソ",
    "タ", "チ", "ツ", "テ", "ト",
    "ナ", "ニ", "ヌ", "ネ", "ノ",
    "ハ", "ヒ", "フ", "ヘ", "ホ",
    "マ", "ミ", "ム", "メ", "モ",
    "ヤ", "ユ", "ヨ", "ラ", "リ",
    "ル", "レ", "ロ", "ワ", "ヲ",
    "ン", "ァ", "ィ", "ゥ", "ェ",
    "ォ", "ャ", "ュ", "ョ", "ッ",
    "１", "２", "３", NULL, NULL,
    "／", "？", "！", "　"
};

static int
find_in_extended_map(
    char* str)
{
    int k;
    for (k=0;k < 128;++k) {
        if (kana[k] != NULL && !strncmpa(str, kana[k])) break;
    }
    return k;
}

char*
convert_extended_string(
    unsigned int version,
    char* old_string)
{
    char* buf = calloc(0x10000, sizeof(char));
    size_t i = 0;
    while (*old_string) {
        if (*old_string & 0x80) { /* UTF-8 continuation bit */
            int k = find_in_extended_map(old_string);
            if (k == 128) {
#define SplitDakuten(KOrig, KPart) if (!strncmpa(old_string, KOrig)) { buf[i++] = find_in_extended_map(KPart) | 0x80; buf[i++] = find_in_extended_map("゛") | 0x80; old_string += strlen(KOrig); }
#define SplitDakuten2(KOrig, KPart) if (!strncmpa(old_string, KOrig)) { buf[i++] = find_in_extended_map(KPart) | 0x80; buf[i++] = (find_in_extended_map("゛")+1) | 0x80; old_string += strlen(KOrig); }
#define SplitHandakuten(KOrig, KPart) if (!strncmpa(old_string, KOrig)) { buf[i++] = find_in_extended_map(KPart) | 0x80; buf[i++] = find_in_extended_map("゜") | 0x80; old_string += strlen(KOrig); }
                     SplitDakuten("が", "か")
                else SplitDakuten("ぎ", "き")
                else SplitDakuten("ぐ", "く")
                else SplitDakuten("げ", "け")
                else SplitDakuten("ご", "こ")
                else SplitDakuten("だ", "た")
                else SplitDakuten("ぢ", "ち")
                else SplitDakuten("づ", "つ")
                else SplitDakuten2("で", "て")
                else SplitDakuten("ど", "と")
                else SplitDakuten("ざ", "さ")
                else SplitDakuten("じ", "し")
                else SplitDakuten2("ず", "す")
                else SplitDakuten("ぜ", "せ")
                else SplitDakuten("ぞ", "そ")
                else SplitDakuten("ば", "は")
                else SplitDakuten("び", "ひ")
                else SplitDakuten("ぶ", "ふ")
                else SplitDakuten("べ", "へ")
                else SplitDakuten("ぼ", "ほ")
                else SplitHandakuten("ぱ", "は")
                else SplitHandakuten("ぴ", "ひ")
                else SplitHandakuten("ぷ", "ふ")
                else SplitHandakuten("ぺ", "へ")
                else SplitHandakuten("ぽ", "ほ")
                else SplitDakuten("ガ", "カ")
                else SplitDakuten("ギ", "キ")
                else SplitDakuten("グ", "ク")
                else SplitDakuten("ゲ", "ケ")
                else SplitDakuten("ゴ", "コ")
                else SplitDakuten("ダ", "タ")
                else SplitDakuten("ヂ", "チ")
                else SplitDakuten("ヅ", "ツ")
                else SplitDakuten("デ", "テ")
                else SplitDakuten("ド", "ト")
                else SplitDakuten("ザ", "サ")
                else SplitDakuten("ジ", "シ")
                else SplitDakuten("ズ", "ス")
                else SplitDakuten("ゼ", "セ")
                else SplitDakuten("ゾ", "ソ")
                else SplitDakuten("バ", "ハ")
                else SplitDakuten("ビ", "ヒ")
                else SplitDakuten("ブ", "フ")
                else SplitDakuten("ベ", "ヘ")
                else SplitDakuten("ボ", "ホ")
                else SplitHandakuten("パ", "ハ")
                else SplitHandakuten("ピ", "ヒ")
                else SplitHandakuten("プ", "フ")
                else SplitHandakuten("ペ", "ヘ")
                else SplitHandakuten("ポ", "ホ")
                else fprintf(stderr, "%s:%s: Invalid extended character.\n", argv0, current_input);
                continue;
#undef SplitDakuten
#undef SplitDakuten2
#undef SplitHandakuten
            }
            buf[i++] = k | 0x80;
            old_string += strlen(kana[k]);
        }
        else {
            buf[i++] = *(old_string++);
        }
    }
    return realloc(buf, i+1);
}

static void
free_globals(void)
{
    if (g_region)
        free(g_region);
    if (g_module_fmt)
        free(g_module_fmt);
    free(g_reg_blocks);
}

static void
print_usage(void)
{
    printf("Usage: %s [-V] [-c VERSION] [-r REGION] [-l LEVEL] [-h OUTPUT] [-e OUTPUT]... [INPUT [OUTPUT]]\n"
           "Options:\n"
           "  -c  create GOOL file\n"
           "  -V  display version information and exit\n"
           "  -r  set the region, used for specific time and frame calculations, and #ifreg parse blocks\n"
           "  -l  set the level, used for #iflev parse blocks\n"
           "  -h  set the output path for a \"header\" file which contains definitions for spawns and the GOOL ID\n"
           "  -e  set the output path format for external GOOL modules, where the first parameter is the entry-name\n"
           "VERSION can be:\n"
           "  1, 2\n"
           "LEVEL can be:\n"
           "  Any valid ename character (0-9, a-z, A-Z, '_', '!')\n"
           "REGION can be:\n"
           "  ntsc-u, ntsc-j, pal\n"
           "Report bugs to <" PACKAGE_BUGREPORT ">.\n", argv0);
}

int
main(int argc, char* argv[])
{
    FILE* in = stdin;
    FILE* out = stdout;
    FILE* h_out = NULL;
    unsigned int version = 0;
    int mode = -1;
    const thecl_module_t* module = NULL;

    current_input = "(stdin)";
    current_output = "(stdout)";

    make_sin_table();
    atexit(free_globals);

    argv0 = util_shortname(argv[0]);
    int opt;
    int ind=0;
    while(argv[util_optind]) {
        switch(opt = util_getopt(argc, argv, ":c:Vr:l:h:e:")) {
        case 'c':
            if(mode != -1) {
                fprintf(stderr,"%s: More than one mode specified\n", argv0);
                print_usage();
                exit(1);
            }
            mode = opt;
            version = parse_version(util_optarg);
            break;
        case 'r':
            if (!strncmp(util_optarg, "ntsc", 4)) {
                g_rate = 30;
            }
            else if (!strcmp(util_optarg, "pal")) {
                g_rate = 25;
            }
            else {
                fprintf(stderr,"%s: Invalid region specified\n", argv0);
                print_usage();
                exit(1);
            }
            if (g_region)
                free(g_region);
            g_region = strdup(util_optarg);
            break;
        case 'l':
            if (strchr(gool_ename_charmap, util_optarg[0]) != NULL) {
                g_lev = util_optarg[0];
            }
            else {
                fprintf(stderr, "%s: Invalid level specified\n", argv0);
                print_usage();
                exit(1);
            }
            break;
        case 'h':
            h_out = fopen(util_optarg, "w");
            if (!h_out) {
                fprintf(stderr, "%s: couldn't open %s for reading: %s\n",
                    argv0, util_optarg, strerror(errno));
            }
            break;
        case 'e':
            g_module_fmt = strdup(util_optarg);
            break;
        default:
            util_getopt_default(&ind,argv,opt,print_usage);
        }
    }
    argc = ind;
    argv[argc] = NULL;

    if (!g_region) {
        g_region = strdup("ntsc-u");
    }
    g_reg_blocks = malloc(0);

    switch (version) {
    case 2:
        module = &c2_gool;
        break;
    case 1:
        module = &c1_gool;
        break;
    default:
        if (mode == 'c') {
            if (!version)
                fprintf(stderr, "%s: version must be specified\n", argv0);
            else
                fprintf(stderr, "%s: version %u is unsupported\n", argv0, version);
            exit(1);
        }
    }

    switch (mode)
    {
    case 'c': {
        if (0 < argc) {
            current_input = argv[0];
            in = fopen(argv[0], "rb");
            if (!in) {
                fprintf(stderr, "%s: couldn't open %s for reading: %s\n",
                    argv0, argv[0], strerror(errno));
                exit(1);
            }
            if (1 < argc) {
                current_output = argv[1];
                out = fopen(argv[1], "wb");
                if (!out) {
                    fprintf(stderr, "%s: couldn't open %s for writing: %s\n",
                        argv0, argv[1], strerror(errno));
                    fclose(in);
                    exit(1);
                }
            }
        }

        if (mode == 'c') {
#ifdef _WIN32
            (void)_setmode(fileno(stdout), _O_BINARY);
#endif
            parser_state_t* parser = module->parse(in, argv[0], version);
            if (parser) {
                if (parser->main_ecl->is_defined) {
                    module->compile(parser, out);
                }
                if (h_out) {
                    module->create_header(parser->main_ecl, h_out);
                }
                thecl_free(parser->main_ecl);
                for (int i = 0; i < parser->ecl_cnt; ++i) {
                    thecl_free(parser->ecl_stack[i]);
                }
                free(parser->ecl_stack);
                free(parser);
            }
        }
        fclose(in);
        fclose(out);
        if (h_out)
            fclose(h_out);

        if(g_was_error) {
          printf("%s:%s: there were errors.\n", argv0, argv[0]);
          exit(1);
        }
        exit(0);
    }
    default:
        print_usage();
        exit(1);
    }
}
