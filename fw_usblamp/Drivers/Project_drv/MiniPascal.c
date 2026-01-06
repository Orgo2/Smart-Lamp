#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <MiniPascal.h>

#include "stm32u0xx_hal.h"
#include "usb_cli.h"
#include "led.h"
#include "bme280.h"
#include "analog.h"
#include "charger.h"
#include "rtc.h"
#include "mic.h"
#include "alarm.h"
#include "main.h"

/* External peripherals from main.c */
extern RNG_HandleTypeDef hrng;

/* ============================ Minimal utils ============================ */
static void mp_puts(const char *s){ while (*s) mp_hal_putchar(*s++); }
static void mp_putcrlf(void){ mp_puts("\r\n"); }
static void mp_prompt(void);  /* forward declaration */

static int mp_stricmp(const char *a, const char *b){
  while (*a && *b){
    char ca = (char)tolower((unsigned char)*a++);
    char cb = (char)tolower((unsigned char)*b++);
    if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void mp_itoa(int v, char *out){
  char tmp[16];
  int n=0;
  if (v==0){ out[0]='0'; out[1]=0; return; }
  bool neg = (v<0);
  unsigned int x = neg ? (unsigned int)(-v) : (unsigned int)v;
  while (x && n < (int)sizeof(tmp)){
    tmp[n++] = (char)('0' + (x % 10));
    x /= 10;
  }
  int i=0;
  if (neg) out[i++]='-';
  while (n>0) out[i++] = tmp[--n];
  out[i]=0;
}

static bool parse_int(const char **p, int *out){
  while (**p==' '||**p=='\t') (*p)++;
  if (!isdigit((unsigned char)**p) && **p!='-') return false;
  int sign=1;
  if (**p=='-'){ sign=-1; (*p)++; }
  int v=0;
  while (isdigit((unsigned char)**p)){ v = v*10 + (**p - '0'); (*p)++; }
  *out = v*sign;
  return true;
}

static uint16_t fnv1a16_ci(const char *s){
  uint32_t h = 2166136261u;
  while (*s){
    char c = (char)tolower((unsigned char)*s++);
    h ^= (uint8_t)c;
    h *= 16777619u;
  }
  return (uint16_t)((h ^ (h>>16)) & 0xFFFFu);
}

/* ============================ Weak abort button ============================ */
#if defined(__GNUC__)
#define MP_WEAK __attribute__((weak))
#else
#define MP_WEAK
#endif

/* BL button abort: returns 1 if BL pressed, 0 otherwise */
MP_WEAK int mp_hal_abort_pressed(void){
  return (HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin) == GPIO_PIN_SET) ? 1 : 0;
}

/* Abort state tracking */
static uint32_t g_abort_press_start = 0;
static bool     g_abort_active = false;

/* ============================ Program storage (editor) ============================ */
typedef struct {
  int line_no;
  char text[MP_LINE_LEN];
} mp_line_t;

typedef struct {
  mp_line_t lines[MP_MAX_LINES];
  uint8_t count;
} mp_editor_t;

static void ed_init(mp_editor_t *ed){ memset(ed, 0, sizeof(*ed)); }

static int ed_find(const mp_editor_t *ed, int line_no){
  for (uint8_t i=0;i<ed->count;i++) if (ed->lines[i].line_no == line_no) return (int)i;
  return -1;
}

static int ed_insert_pos(const mp_editor_t *ed, int line_no){
  int pos=0;
  while (pos < (int)ed->count && ed->lines[pos].line_no < line_no) pos++;
  return pos;
}

static bool ed_delete(mp_editor_t *ed, int line_no){
  int idx = ed_find(ed, line_no);
  if (idx < 0) return false;
  for (int i=idx; i<(int)ed->count-1; i++) ed->lines[i] = ed->lines[i+1];
  ed->count--;
  return true;
}

static bool ed_set(mp_editor_t *ed, int line_no, const char *text){
  if (line_no <= 0) return false;
  if (!text || text[0]==0) { (void)ed_delete(ed, line_no); return true; }

  int idx = ed_find(ed, line_no);
  if (idx >= 0){
    strncpy(ed->lines[idx].text, text, MP_LINE_LEN);
    ed->lines[idx].text[MP_LINE_LEN-1]=0;
    return true;
  }

  if (ed->count >= MP_MAX_LINES) return false;
  int pos = ed_insert_pos(ed, line_no);
  for (int i=(int)ed->count; i>pos; i--) ed->lines[i] = ed->lines[i-1];
  ed->lines[pos].line_no = line_no;
  strncpy(ed->lines[pos].text, text, MP_LINE_LEN);
  ed->lines[pos].text[MP_LINE_LEN-1]=0;
  ed->count++;
  return true;
}

static int ed_max_line(const mp_editor_t *ed){
  int m = 0;
  for (uint8_t i=0;i<ed->count;i++) if (ed->lines[i].line_no > m) m = ed->lines[i].line_no;
  return m;
}

static void ed_list(const mp_editor_t *ed){
  char num[16];
  for (uint8_t i=0;i<ed->count;i++){
    mp_itoa(ed->lines[i].line_no, num);
    mp_puts(num); mp_puts(" "); mp_puts(ed->lines[i].text); mp_putcrlf();
  }
}

/* ============================ System variables (fixed slots) ============================ */
typedef struct { const char *name; uint8_t idx; } sysvar_t;

enum {
  SV_CMDID = 0,
  SV_NARG  = 1,
  SV_A0    = 2, SV_A1, SV_A2, SV_A3, SV_A4, SV_A5, SV_A6, SV_A7, /* 2..9 */
  SV_LEDI  = 10, SV_LEDR, SV_LEDG, SV_LEDB, SV_LEDW,             /* 10..14 */
  SV_TIMEH = 15, SV_TIMEM, SV_TIMES,                             /* 15..17 */
  SV_ALH   = 18, SV_ALM, SV_ALS                                  /* 18..20 */
};
#define SYSVAR_COUNT 21

static const sysvar_t g_sysvars[] = {
  {"CMDID", SV_CMDID}, {"NARG", SV_NARG},
  {"A0", SV_A0}, {"A1", SV_A1}, {"A2", SV_A2}, {"A3", SV_A3}, {"A4", SV_A4}, {"A5", SV_A5}, {"A6", SV_A6}, {"A7", SV_A7},
  {"A", SV_A0}, {"B", SV_A1}, {"C", SV_A2}, {"D", SV_A3},
  {"LEDI", SV_LEDI}, {"LEDR", SV_LEDR}, {"LEDG", SV_LEDG}, {"LEDB", SV_LEDB}, {"LEDW", SV_LEDW},
  {"TIMEH", SV_TIMEH}, {"TIMEM", SV_TIMEM}, {"TIMES", SV_TIMES},
  {"ALH", SV_ALH}, {"ALM", SV_ALM}, {"ALS", SV_ALS},
};

static int sysvar_find(const char *name){
  for (unsigned i=0; i<sizeof(g_sysvars)/sizeof(g_sysvars[0]); i++){
    if (!mp_stricmp(name, g_sysvars[i].name)) return g_sysvars[i].idx;
  }
  return -1;
}

/* ============================ Lexer ============================ */
typedef enum {
  T_EOF=0, T_NUM, T_ID,
  T_ASSIGN, /* := */
  T_SEMI, T_LP, T_RP, T_COMMA,
  T_PLUS, T_MINUS, T_MUL, T_DIV, T_MOD,
  T_EQ, T_NEQ, T_LT, T_LTE, T_GT, T_GTE,
  T_IF, T_THEN, T_ELSE,
  T_WHILE, T_DO,
  T_BEGIN, T_END,
  T_REPEAT, T_UNTIL,
  T_GOTO,
  T_AND, T_OR, T_NOT
} tok_t;

typedef struct {
  tok_t k;
  int32_t num;
  char id[MP_NAME_LEN];
} token_t;

typedef struct {
  const char *s;
  uint16_t pos;
  token_t cur;
} lex_t;

static bool is_id0(char c){ return (c=='_') || isalpha((unsigned char)c); }
static bool is_idn(char c){ return (c=='_') || isalnum((unsigned char)c); }

static void lex_init(lex_t *lx, const char *s){ lx->s=s; lx->pos=0; memset(&lx->cur,0,sizeof(lx->cur)); }

static void lex_skip_ws(lex_t *lx){
  while (lx->s[lx->pos] && (lx->s[lx->pos]==' ' || lx->s[lx->pos]=='\t' || lx->s[lx->pos]=='\r' || lx->s[lx->pos]=='\n')) lx->pos++;
}

static bool kw(const char *id, const char *w){
  while (*id && *w){
    char a=(char)tolower((unsigned char)*id++);
    char b=(char)tolower((unsigned char)*w++);
    if (a!=b) return false;
  }
  return (*id==0 && *w==0);
}

static tok_t kw_kind(const char *id){
  if (kw(id,"if")) return T_IF;
  if (kw(id,"then")) return T_THEN;
  if (kw(id,"else")) return T_ELSE;
  if (kw(id,"while")) return T_WHILE;
  if (kw(id,"do")) return T_DO;
  if (kw(id,"begin")) return T_BEGIN;
  if (kw(id,"end")) return T_END;
  if (kw(id,"repeat")) return T_REPEAT;
  if (kw(id,"until")) return T_UNTIL;
  if (kw(id,"goto")) return T_GOTO;
  if (kw(id,"and")) return T_AND;
  if (kw(id,"or")) return T_OR;
  if (kw(id,"not")) return T_NOT;
  return T_ID;
}

static void lex_next(lex_t *lx){
  lex_skip_ws(lx);
  char c = lx->s[lx->pos];
  token_t t; memset(&t,0,sizeof(t)); t.k=T_EOF;

  if (!c){ lx->cur=t; return; }

  if (isdigit((unsigned char)c)){
    int32_t v=0;
    while (isdigit((unsigned char)lx->s[lx->pos])){
      v = (int32_t)(v*10 + (lx->s[lx->pos]-'0'));
      lx->pos++;
    }
    t.k=T_NUM; t.num=v; lx->cur=t; return;
  }

  if (is_id0(c)){
    char buf[MP_NAME_LEN];
    uint16_t i=0;
    while (is_idn(lx->s[lx->pos]) && i < (MP_NAME_LEN-1)) buf[i++] = lx->s[lx->pos++];
    buf[i]=0;
    t.k = kw_kind(buf);
    strncpy(t.id, buf, MP_NAME_LEN);
    t.id[MP_NAME_LEN-1]=0;
    lx->cur=t; return;
  }

  if (c==':' && lx->s[lx->pos+1]=='='){ lx->pos+=2; t.k=T_ASSIGN; lx->cur=t; return; }
  if (c=='<' && lx->s[lx->pos+1]=='='){ lx->pos+=2; t.k=T_LTE; lx->cur=t; return; }
  if (c=='>' && lx->s[lx->pos+1]=='='){ lx->pos+=2; t.k=T_GTE; lx->cur=t; return; }
  if (c=='<' && lx->s[lx->pos+1]=='>'){ lx->pos+=2; t.k=T_NEQ; lx->cur=t; return; }

  lx->pos++;
  switch(c){
    case ';': t.k=T_SEMI; break;
    case '(': t.k=T_LP; break;
    case ')': t.k=T_RP; break;
    case ',': t.k=T_COMMA; break;
    case '+': t.k=T_PLUS; break;
    case '-': t.k=T_MINUS; break;
    case '*': t.k=T_MUL; break;
    case '/': t.k=T_DIV; break;
    case '%': t.k=T_MOD; break;
    case '=': t.k=T_EQ; break;
    case '<': t.k=T_LT; break;
    case '>': t.k=T_GT; break;
    default:  t.k=T_EOF; break;
  }
  lx->cur=t;
}

/* ============================ Compiler (to tiny bytecode) ============================ */
typedef enum {
  OP_HALT=0,
  OP_PUSHI, OP_LOAD, OP_STORE,
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
  OP_EQ, OP_NEQ, OP_LT, OP_LTE, OP_GT, OP_GTE,
  OP_AND, OP_OR, OP_NOT,
  OP_JMP, OP_JZ,
  OP_CALL,      /* u8 id, u8 argc */
  OP_SLEEP      /* u32 ms */
} op_t;

typedef struct { char name[MP_NAME_LEN]; uint8_t idx; } sym_t;
typedef struct { sym_t syms[MP_MAX_VARS]; uint8_t count; } symtab_t;

typedef struct { uint16_t line_no; uint16_t bc_patch; } fixup_t;

typedef struct {
  uint8_t bc[MP_BC_MAX];
  uint16_t len;
  symtab_t st;
  uint16_t line_addr[MP_MAX_LINES];
  fixup_t fix[MP_MAX_FIXUPS];
  uint8_t fix_n;
} program_t;

static const char *g_err=0;
static int g_err_line=-1;
static void set_err(const char *e, int line){ if(!g_err){ g_err=e; g_err_line=line; } }

static bool emit_u8(program_t *p, uint8_t b){ if (p->len+1>MP_BC_MAX) return false; p->bc[p->len++]=b; return true; }
static bool emit_u16(program_t *p, uint16_t v){
  if (p->len+2>MP_BC_MAX) return false;
  p->bc[p->len++]=(uint8_t)(v&0xFF);
  p->bc[p->len++]=(uint8_t)((v>>8)&0xFF);
  return true;
}
static bool emit_u32(program_t *p, uint32_t v){
  if (p->len+4>MP_BC_MAX) return false;
  p->bc[p->len++]=(uint8_t)(v&0xFF);
  p->bc[p->len++]=(uint8_t)((v>>8)&0xFF);
  p->bc[p->len++]=(uint8_t)((v>>16)&0xFF);
  p->bc[p->len++]=(uint8_t)((v>>24)&0xFF);
  return true;
}
static bool emit_pushi(program_t *p, int32_t v){ return emit_u8(p, OP_PUSHI) && emit_u32(p, (uint32_t)v); }
static bool patch_u16(program_t *p, uint16_t at, uint16_t v){
  if (at+1 >= p->len) return false;
  p->bc[at]=(uint8_t)(v&0xFF);
  p->bc[at+1]=(uint8_t)((v>>8)&0xFF);
  return true;
}

static int sym_find(const symtab_t *st, const char *name){
  int sv = sysvar_find(name);
  if (sv >= 0) return sv;
  for (uint8_t i=0;i<st->count;i++) if (!strncmp(st->syms[i].name, name, MP_NAME_LEN)) return st->syms[i].idx;
  return -1;
}
static int sym_get_or_add(symtab_t *st, const char *name){
  int sv = sysvar_find(name);
  if (sv >= 0) return sv;

  int f=sym_find(st,name);
  if (f>=0) return f;
  if ((uint16_t)SYSVAR_COUNT + st->count >= MP_MAX_VARS) return -1;

  uint8_t idx = (uint8_t)(SYSVAR_COUNT + st->count);
  strncpy(st->syms[st->count].name, name, MP_NAME_LEN);
  st->syms[st->count].name[MP_NAME_LEN-1]=0;
  st->syms[st->count].idx=idx;
  st->count++;
  return idx;
}

/* -------- Builtin name -> id table (EDIT HERE) --------
   - led(i, v) for digital
   - led(i, r, g, b, w) for RGBW (your use-case)
*/
static int builtin_id(const char *name){
  if (!mp_stricmp(name,"led"))   return 1;
  if (!mp_stricmp(name,"wait"))  return 2;
  if (!mp_stricmp(name,"delay")) return 2;  /* alias for wait */
  if (!mp_stricmp(name,"battery")) return 3;
  if (!mp_stricmp(name,"rng"))   return 4;
  if (!mp_stricmp(name,"temp"))  return 5;
  if (!mp_stricmp(name,"hum"))   return 6;
  if (!mp_stricmp(name,"press")) return 7;
  if (!mp_stricmp(name,"btn"))   return 8;
  if (!mp_stricmp(name,"mic"))   return 9;
  if (!mp_stricmp(name,"settime")) return 10; /* settime(h,m,s) */
  if (!mp_stricmp(name,"setalarm"))return 11; /* setalarm(h,m,s) */
  if (!mp_stricmp(name,"alarm")) return 11; /* alias for setalarm */
  if (!mp_stricmp(name,"light")) return 12;
  if (!mp_stricmp(name,"ledon")) return 13;
  if (!mp_stricmp(name,"ledoff")) return 14;
  if (!mp_stricmp(name,"beep")) return 15;
  return -1;
}

/* -------- Recursive descent expression -------- */
typedef struct { lex_t lx; program_t *p; int line; } Ctx;
static void nx(Ctx *c){ lex_next(&c->lx); }
static bool ac(Ctx *c, tok_t k){ if (c->lx.cur.k==k){ nx(c); return true; } return false; }
static bool ex(Ctx *c, tok_t k, const char *msg){ if (ac(c,k)) return true; set_err(msg,c->line); return false; }

static bool expr(Ctx *c);

static bool primary(Ctx *c){
  if (c->lx.cur.k==T_NUM){
    int32_t v=c->lx.cur.num; nx(c);
    if (!emit_pushi(c->p, v)){ set_err("bytecode overflow", c->line); return false; }
    return true;
  }
  if (c->lx.cur.k==T_ID){
    char nm[MP_NAME_LEN]; strncpy(nm,c->lx.cur.id,MP_NAME_LEN); nm[MP_NAME_LEN-1]=0;
    nx(c);

    if (ac(c, T_LP)){
      int id = builtin_id(nm);
      if (id<0){ set_err("unknown function", c->line); return false; }

      uint8_t argc=0;
      if (!ac(c, T_RP)){
        while (1){
          if (!expr(c)) return false;
          argc++;
          if (argc>8){ set_err("too many args", c->line); return false; }
          if (ac(c, T_COMMA)) continue;
          if (!ex(c, T_RP, "expected ')'")) return false;
          break;
        }
      }

      if (id==2){
        set_err("wait/delay only as statement", c->line); return false;
      }

      if (!emit_u8(c->p, OP_CALL) || !emit_u8(c->p, (uint8_t)id) || !emit_u8(c->p, argc)){
        set_err("bytecode overflow", c->line); return false;
      }
      return true;
    }

    int idx=sym_get_or_add(&c->p->st, nm);
    if (idx<0){ set_err("too many variables", c->line); return false; }
    if (!emit_u8(c->p, OP_LOAD) || !emit_u8(c->p, (uint8_t)idx)){ set_err("bytecode overflow", c->line); return false; }
    return true;
  }
  if (ac(c, T_LP)){
    if (!expr(c)) return false;
    return ex(c, T_RP, "expected ')'");
  }
  set_err("expected number/identifier/(expr)", c->line);
  return false;
}

static bool unary(Ctx *c){
  if (ac(c, T_MINUS)){ if(!unary(c)) return false; if(!emit_u8(c->p, OP_NEG)){ set_err("bytecode overflow", c->line); return false; } return true; }
  if (ac(c, T_NOT)){ if(!unary(c)) return false; if(!emit_u8(c->p, OP_NOT)){ set_err("bytecode overflow", c->line); return false; } return true; }
  return primary(c);
}

static bool mul(Ctx *c){
  if (!unary(c)) return false;
  while (c->lx.cur.k==T_MUL || c->lx.cur.k==T_DIV || c->lx.cur.k==T_MOD){
    tok_t op=c->lx.cur.k; nx(c);
    if(!unary(c)) return false;
    uint8_t bc=(op==T_MUL)?OP_MUL:(op==T_DIV)?OP_DIV:OP_MOD;
    if(!emit_u8(c->p, bc)){ set_err("bytecode overflow", c->line); return false; }
  }
  return true;
}

static bool add(Ctx *c){
  if (!mul(c)) return false;
  while (c->lx.cur.k==T_PLUS || c->lx.cur.k==T_MINUS){
    tok_t op=c->lx.cur.k; nx(c);
    if(!mul(c)) return false;
    uint8_t bc=(op==T_PLUS)?OP_ADD:OP_SUB;
    if(!emit_u8(c->p, bc)){ set_err("bytecode overflow", c->line); return false; }
  }
  return true;
}

static bool cmp(Ctx *c){
  if (!add(c)) return false;
  while (c->lx.cur.k==T_EQ || c->lx.cur.k==T_NEQ || c->lx.cur.k==T_LT || c->lx.cur.k==T_LTE || c->lx.cur.k==T_GT || c->lx.cur.k==T_GTE){
    tok_t op=c->lx.cur.k; nx(c);
    if(!add(c)) return false;
    uint8_t bc=OP_EQ;
    switch(op){
      case T_EQ: bc=OP_EQ; break;
      case T_NEQ: bc=OP_NEQ; break;
      case T_LT: bc=OP_LT; break;
      case T_LTE: bc=OP_LTE; break;
      case T_GT: bc=OP_GT; break;
      case T_GTE: bc=OP_GTE; break;
      default: break;
    }
    if(!emit_u8(c->p, bc)){ set_err("bytecode overflow", c->line); return false; }
  }
  return true;
}

static bool land(Ctx *c){
  if (!cmp(c)) return false;
  while (ac(c, T_AND)){ if(!cmp(c)) return false; if(!emit_u8(c->p, OP_AND)){ set_err("bytecode overflow", c->line); return false; } }
  return true;
}

static bool lor(Ctx *c){
  if (!land(c)) return false;
  while (ac(c, T_OR)){ if(!land(c)) return false; if(!emit_u8(c->p, OP_OR)){ set_err("bytecode overflow", c->line); return false; } }
  return true;
}

static bool expr(Ctx *c){ return lor(c); }

/* -------- Statements -------- */
static bool stmt(Ctx *c);

static bool stmt_list_until(Ctx *c, tok_t until){
  while (c->lx.cur.k != T_EOF && c->lx.cur.k != until){
    if(!stmt(c)) return false;
    ac(c, T_SEMI);
  }
  return true;
}

static bool block_or_single(Ctx *c){
  if (ac(c, T_BEGIN)){
    if(!stmt_list_until(c, T_END)) return false;
    return ex(c, T_END, "expected 'end'");
  }
  return stmt(c);
}

static bool st_if(Ctx *c){
  if(!expr(c)) return false;
  if(!ex(c, T_THEN, "expected 'then'")) return false;

  if(!emit_u8(c->p, OP_JZ) || !emit_u16(c->p, 0)){ set_err("bytecode overflow", c->line); return false; }
  uint16_t jz_patch = (uint16_t)(c->p->len - 2);

  if(!block_or_single(c)) return false;

  if (ac(c, T_ELSE)){
    if(!emit_u8(c->p, OP_JMP) || !emit_u16(c->p, 0)){ set_err("bytecode overflow", c->line); return false; }
    uint16_t jmp_patch = (uint16_t)(c->p->len - 2);

    if(!patch_u16(c->p, jz_patch, c->p->len)){ set_err("patch failed", c->line); return false; }
    if(!block_or_single(c)) return false;
    if(!patch_u16(c->p, jmp_patch, c->p->len)){ set_err("patch failed", c->line); return false; }
  } else {
    if(!patch_u16(c->p, jz_patch, c->p->len)){ set_err("patch failed", c->line); return false; }
  }
  return true;
}

static bool st_while(Ctx *c){
  uint16_t start = c->p->len;
  if(!expr(c)) return false;
  if(!ex(c, T_DO, "expected 'do'")) return false;

  if(!emit_u8(c->p, OP_JZ) || !emit_u16(c->p, 0)){ set_err("bytecode overflow", c->line); return false; }
  uint16_t jz_patch = (uint16_t)(c->p->len - 2);

  if(!block_or_single(c)) return false;

  if(!emit_u8(c->p, OP_JMP) || !emit_u16(c->p, start)){ set_err("bytecode overflow", c->line); return false; }
  if(!patch_u16(c->p, jz_patch, c->p->len)){ set_err("patch failed", c->line); return false; }
  return true;
}

static bool st_repeat(Ctx *c){
  uint16_t start = c->p->len;
  if(!stmt_list_until(c, T_UNTIL)) return false;
  if(!ex(c, T_UNTIL, "expected 'until'")) return false;
  if(!expr(c)) return false;
  if(!emit_u8(c->p, OP_JZ) || !emit_u16(c->p, start)){ set_err("bytecode overflow", c->line); return false; }
  return true;
}

static bool st_goto(Ctx *c){
  if (c->lx.cur.k != T_NUM){ set_err("goto needs line number", c->line); return false; }
  uint16_t tgt = (uint16_t)c->lx.cur.num;
  nx(c);

  if(!emit_u8(c->p, OP_JMP) || !emit_u16(c->p, 0)){ set_err("bytecode overflow", c->line); return false; }
  uint16_t patchpos = (uint16_t)(c->p->len - 2);

  if (c->p->fix_n >= MP_MAX_FIXUPS){ set_err("too many gotos", c->line); return false; }
  c->p->fix[c->p->fix_n].line_no = tgt;
  c->p->fix[c->p->fix_n].bc_patch = patchpos;
  c->p->fix_n++;
  return true;
}

static bool st_assign_or_call(Ctx *c){
  char nm[MP_NAME_LEN]; strncpy(nm,c->lx.cur.id,MP_NAME_LEN); nm[MP_NAME_LEN-1]=0;
  nx(c);

  if (ac(c, T_ASSIGN)){
    if(!expr(c)) return false;
    int idx = sym_get_or_add(&c->p->st, nm);
    if (idx<0){ set_err("too many variables", c->line); return false; }
    if(!emit_u8(c->p, OP_STORE) || !emit_u8(c->p, (uint8_t)idx)){ set_err("bytecode overflow", c->line); return false; }
    return true;
  }

  if (!ac(c, T_LP)){ set_err("expected ':=' or '('", c->line); return false; }
  int id = builtin_id(nm);
  if (id<0){ set_err("unknown function", c->line); return false; }

  uint8_t argc=0;
  if (!ac(c, T_RP)){
    while (1){
      if(!expr(c)) return false;
      argc++;
      if (argc>8){ set_err("too many args", c->line); return false; }
      if (ac(c, T_COMMA)) continue;
      if(!ex(c, T_RP, "expected ')'")) return false;
      break;
    }
  }

  if (id==2 && argc!=1){ set_err("wait/delay expects 1 arg", c->line); return false; }

  if(!emit_u8(c->p, OP_CALL) || !emit_u8(c->p, (uint8_t)id) || !emit_u8(c->p, argc)){ set_err("bytecode overflow", c->line); return false; }

  int dump = sym_get_or_add(&c->p->st, "__");
  if (dump<0){ set_err("too many variables", c->line); return false; }
  if(!emit_u8(c->p, OP_STORE) || !emit_u8(c->p, (uint8_t)dump)){ set_err("bytecode overflow", c->line); return false; }
  return true;
}

static bool stmt(Ctx *c){
  if (ac(c, T_IF)) return st_if(c);
  if (ac(c, T_WHILE)) return st_while(c);
  if (ac(c, T_REPEAT)) return st_repeat(c);
  if (ac(c, T_GOTO)) return st_goto(c);

  if (ac(c, T_BEGIN)){
    if(!stmt_list_until(c, T_END)) return false;
    return ex(c, T_END, "expected 'end'");
  }

  if (c->lx.cur.k == T_ID) return st_assign_or_call(c);
  return true;
}

static bool compile_line(program_t *p, int line_no, const char *text){
  Ctx c; memset(&c,0,sizeof(c));
  c.p = p; c.line = line_no;
  lex_init(&c.lx, text);
  nx(&c);
  if(!stmt_list_until(&c, T_EOF)) return false;
  return true;
}

static int editor_index_by_line(const mp_editor_t *ed, uint16_t line_no){
  for (uint8_t i=0;i<ed->count;i++) if (ed->lines[i].line_no == (int)line_no) return i;
  return -1;
}

static bool compile_program(const mp_editor_t *ed, program_t *out){
  memset(out, 0, sizeof(*out));
  g_err=0; g_err_line=-1;

  if (MP_MAX_VARS < SYSVAR_COUNT + 8){
    set_err("MP_MAX_VARS too small for system vars", -1);
    return false;
  }

  for (uint8_t i=0;i<ed->count;i++){
    out->line_addr[i] = out->len;
    if(!compile_line(out, ed->lines[i].line_no, ed->lines[i].text)){
      if(!g_err) set_err("compile error", ed->lines[i].line_no);
      break;
    }
  }
  if (!g_err){
    if(!emit_u8(out, OP_HALT)) set_err("bytecode overflow", -1);
  }

  if (!g_err){
    for (uint8_t f=0; f<out->fix_n; f++){
      int idx = editor_index_by_line(ed, out->fix[f].line_no);
      if (idx < 0){ set_err("goto target line not found", (int)out->fix[f].line_no); break; }
      if(!patch_u16(out, out->fix[f].bc_patch, out->line_addr[idx])) { set_err("patch failed", (int)out->fix[f].line_no); break; }
    }
  }
  return (g_err==0);
}

/* ============================ VM ============================ */
typedef struct {
  int32_t stack[MP_STACK_SIZE];
  int sp;
  int32_t vars[MP_MAX_VARS];
  uint16_t ip;
  bool running;
  bool stop_req;
  bool sleeping;
  uint32_t wake_ms;
} vm_t;

static void vm_reset(vm_t *vm){
  memset(vm,0,sizeof(*vm));
  vm->running = true;
}
static bool push(vm_t *vm, int32_t v){ if(vm->sp>=MP_STACK_SIZE) return false; vm->stack[vm->sp++]=v; return true; }
static bool pop(vm_t *vm, int32_t *o){ if(vm->sp<=0) return false; *o=vm->stack[--vm->sp]; return true; }
static uint16_t rd_u16(const uint8_t *bc, uint16_t *ip){ uint16_t v=(uint16_t)bc[*ip] | ((uint16_t)bc[*ip+1]<<8); *ip+=2; return v; }
static int32_t rd_i32(const uint8_t *bc, uint16_t *ip){
  uint32_t v=(uint32_t)bc[*ip] | ((uint32_t)bc[*ip+1]<<8) | ((uint32_t)bc[*ip+2]<<16) | ((uint32_t)bc[*ip+3]<<24);
  *ip+=4; return (int32_t)v;
}

static bool vm_step(vm_t *vm, const program_t *p, uint32_t now_ms, uint16_t max_ops){
  if (!vm->running) return false;
  if (vm->stop_req){ vm->running=false; return false; }

  if (vm->sleeping){
    if ((int32_t)(now_ms - vm->wake_ms) < 0) return true;
    vm->sleeping=false;
  }

  uint16_t ops=0;
  while (vm->running && ops < max_ops){
    if (vm->ip >= p->len){ vm->running=false; break; }
    uint8_t op = p->bc[vm->ip++];

    int32_t a,b;
    switch((op_t)op){
      case OP_HALT: vm->running=false; break;
      case OP_PUSHI: if(!push(vm, rd_i32(p->bc, &vm->ip))) vm->running=false; break;
      case OP_LOAD: { uint8_t idx=p->bc[vm->ip++]; if(idx>=MP_MAX_VARS||!push(vm, vm->vars[idx])) vm->running=false; } break;
      case OP_STORE:{ uint8_t idx=p->bc[vm->ip++]; if(!pop(vm,&a)) vm->running=false; else if(idx<MP_MAX_VARS) vm->vars[idx]=a; } break;

      case OP_ADD: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,a+b)) vm->running=false; break;
      case OP_SUB: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,a-b)) vm->running=false; break;
      case OP_MUL: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,a*b)) vm->running=false; break;
      case OP_DIV: if(!pop(vm,&b)||!pop(vm,&a)||b==0||!push(vm,a/b)) vm->running=false; break;
      case OP_MOD: if(!pop(vm,&b)||!pop(vm,&a)||b==0||!push(vm,a%b)) vm->running=false; break;
      case OP_NEG: if(!pop(vm,&a)||!push(vm,-a)) vm->running=false; break;

      case OP_EQ:  if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a==b)?1:0)) vm->running=false; break;
      case OP_NEQ: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a!=b)?1:0)) vm->running=false; break;
      case OP_LT:  if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a<b)?1:0)) vm->running=false; break;
      case OP_LTE: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a<=b)?1:0)) vm->running=false; break;
      case OP_GT:  if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a>b)?1:0)) vm->running=false; break;
      case OP_GTE: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a>=b)?1:0)) vm->running=false; break;

      case OP_AND: if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a&&b)?1:0)) vm->running=false; break;
      case OP_OR:  if(!pop(vm,&b)||!pop(vm,&a)||!push(vm,(a||b)?1:0)) vm->running=false; break;
      case OP_NOT: if(!pop(vm,&a)||!push(vm,(!a)?1:0)) vm->running=false; break;

      case OP_JMP: { uint16_t addr=rd_u16(p->bc,&vm->ip); vm->ip=addr; } break;
      case OP_JZ:  { uint16_t addr=rd_u16(p->bc,&vm->ip); if(!pop(vm,&a)) vm->running=false; else if(a==0) vm->ip=addr; } break;

      case OP_CALL: {
        uint8_t id = p->bc[vm->ip++];
        uint8_t argc = p->bc[vm->ip++];
        int32_t argv[8];
        if (argc>8 || argc>(uint8_t)vm->sp){ vm->running=false; break; }
        for (int i=(int)argc-1;i>=0;i--){ if(!pop(vm,&argv[i])){ vm->running=false; break; } }
        if (!vm->running) break;

        if (id==2){
          if (argc!=1){ vm->running=false; break; }
          int32_t ms = argv[0];
          if (ms < 0) ms = 0;
          vm->sleeping=true;
          vm->wake_ms = now_ms + (uint32_t)ms;
          if(!push(vm,0)) vm->running=false;
          return vm->running;
        }

        int32_t r = mp_user_builtin(id, argc, argv);
        if(!push(vm,r)) vm->running=false;
      } break;

      case OP_SLEEP: {
        uint32_t ms = (uint32_t)rd_i32(p->bc, &vm->ip);
        vm->sleeping=true;
        vm->wake_ms = now_ms + ms;
        return true;
      } break;

      default: vm->running=false; break;
    }
    ops++;
  }
  return vm->running;
}

/* ============================ Flash storage: 3 slots at end of flash ============================ */
#define MP_MAGIC 0x4D505033u /* 'MPP3' */
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t version;   /* 2 */
  uint16_t count;
  uint8_t  autorun;
  uint8_t  reserved[3];
  uint32_t data_len;
  uint32_t checksum;
} mp_hdr_t;

static uint32_t fnv1a32_update(uint32_t h, const void *data, uint32_t len){
  const uint8_t *p=(const uint8_t*)data;
  for (uint32_t i=0;i<len;i++){ h ^= p[i]; h *= 16777619u; }
  return h;
}

static uint32_t slot_size_bytes(void){
  return (uint32_t)MP_FLASH_SLOT_PAGES * (uint32_t)MP_FLASH_PAGE_SIZE;
}

static uint32_t flash_end_addr(void){
  return (uint32_t)FLASH_BASE + (uint32_t)MP_FLASH_TOTAL_SIZE;
}

static uint32_t slot_base_addr(uint8_t slot){
  uint32_t ss = slot_size_bytes();
  uint32_t end = flash_end_addr();
  if (slot < 1) slot = 1;
  if (slot > MP_FLASH_SLOT_COUNT) slot = MP_FLASH_SLOT_COUNT;
  uint32_t offset = ss * (uint32_t)(MP_FLASH_SLOT_COUNT - slot + 1);
  return end - offset;
}

static bool flash_unlock(void){ return (HAL_FLASH_Unlock()==HAL_OK); }
static void flash_lock(void){ (void)HAL_FLASH_Lock(); }

static bool flash_erase_region(uint32_t addr, uint32_t size){
  if (size==0) return true;
  uint32_t start=addr;
  uint32_t end=addr + size - 1;
  uint32_t page = (start - FLASH_BASE) / MP_FLASH_PAGE_SIZE;
  uint32_t page_end = (end - FLASH_BASE) / MP_FLASH_PAGE_SIZE;

  if (page_end < page) return false;

  FLASH_EraseInitTypeDef ei; memset(&ei,0,sizeof(ei));
  uint32_t page_error=0;
  ei.TypeErase = FLASH_TYPEERASE_PAGES;
  ei.Page = page;
  ei.NbPages = (page_end - page) + 1;
  return (HAL_FLASHEx_Erase(&ei, &page_error) == HAL_OK);
}

static bool flash_prog_bytes(uint32_t addr, const uint8_t *data, uint32_t len){
  uint32_t i=0;
  while (i < len){
    uint64_t dw = 0xFFFFFFFFFFFFFFFFull;
    uint8_t *pdw = (uint8_t*)&dw;
    for (uint32_t k=0;k<8;k++){
      uint32_t idx=i+k;
      pdw[k] = (idx < len) ? data[idx] : 0xFF;
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dw) != HAL_OK) return false;
    addr += 8; i += 8;
  }
  return true;
}

static bool storage_erase_slot(uint8_t slot){
  uint32_t base = slot_base_addr(slot);
  uint32_t size = slot_size_bytes();
  if(!flash_unlock()) return false;
  bool ok = flash_erase_region(base, size);
  flash_lock();
  return ok;
}

static bool storage_save_slot(uint8_t slot, const mp_editor_t *ed, bool autorun){
  mp_hdr_t hdr; memset(&hdr,0,sizeof(hdr));
  hdr.magic = MP_MAGIC;
  hdr.version = 2;
  hdr.count = ed->count;
  hdr.autorun = autorun ? 1 : 0;

  uint32_t data_len=0;
  for (uint8_t i=0;i<ed->count;i++){
    uint8_t slen = (uint8_t)strnlen(ed->lines[i].text, MP_LINE_LEN-1);
    data_len += 2 + 1 + slen;
  }
  hdr.data_len = data_len;
  hdr.checksum = 0;

  uint32_t h = 2166136261u;
  h = fnv1a32_update(h, &hdr, sizeof(hdr));
  for (uint8_t i=0;i<ed->count;i++){
    uint16_t ln = (uint16_t)ed->lines[i].line_no;
    uint8_t slen = (uint8_t)strnlen(ed->lines[i].text, MP_LINE_LEN-1);
    h = fnv1a32_update(h, &ln, 2);
    h = fnv1a32_update(h, &slen, 1);
    h = fnv1a32_update(h, ed->lines[i].text, slen);
  }
  hdr.checksum = h;

  uint32_t total = sizeof(hdr) + data_len;
  uint32_t slot_size = slot_size_bytes();
  if (total > slot_size) return false;

  uint32_t base = slot_base_addr(slot);

  if(!flash_unlock()) return false;
  bool ok = flash_erase_region(base, slot_size);
  if (ok) ok = flash_prog_bytes(base, (const uint8_t*)&hdr, sizeof(hdr));

  uint32_t addr = base + sizeof(hdr);
  for (uint8_t i=0;i<ed->count && ok;i++){
    uint16_t ln = (uint16_t)ed->lines[i].line_no;
    uint8_t slen = (uint8_t)strnlen(ed->lines[i].text, MP_LINE_LEN-1);
    uint8_t rec_hdr[3] = { (uint8_t)(ln&0xFF), (uint8_t)((ln>>8)&0xFF), slen };
    ok = flash_prog_bytes(addr, rec_hdr, 3); addr += 3;
    if (ok){ ok = flash_prog_bytes(addr, (const uint8_t*)ed->lines[i].text, slen); addr += slen; }
  }

  flash_lock();
  return ok;
}

static bool storage_load_slot(uint8_t slot, mp_editor_t *ed, bool *autorun_out){
  uint32_t base = slot_base_addr(slot);
  const mp_hdr_t *hdr = (const mp_hdr_t*)base;
  if (hdr->magic != MP_MAGIC || hdr->version != 2) return false;
  if (hdr->count > MP_MAX_LINES) return false;

  uint32_t total = sizeof(*hdr) + hdr->data_len;
  if (total > slot_size_bytes()) return false;

  mp_hdr_t h0 = *hdr;
  uint32_t stored = h0.checksum;
  h0.checksum = 0;

  uint32_t h = 2166136261u;
  h = fnv1a32_update(h, &h0, sizeof(h0));

  const uint8_t *p = (const uint8_t*)base + sizeof(*hdr);
  uint32_t remain = hdr->data_len;

  ed_init(ed);
  for (uint16_t i=0;i<hdr->count;i++){
    if (remain < 3) return false;
    uint16_t ln = (uint16_t)p[0] | ((uint16_t)p[1]<<8);
    uint8_t slen = p[2];
    h = fnv1a32_update(h, p, 3);
    p += 3; remain -= 3;
    if (remain < slen) return false;
    h = fnv1a32_update(h, p, slen);

    ed->lines[i].line_no = (int)ln;
    uint8_t cpy = slen;
    if (cpy > (MP_LINE_LEN-1)) cpy = (uint8_t)(MP_LINE_LEN-1);
    memcpy(ed->lines[i].text, p, cpy);
    ed->lines[i].text[cpy]=0;
    p += slen; remain -= slen;
    ed->count++;
  }

  if (h != stored) return false;
  if (autorun_out) *autorun_out = (hdr->autorun != 0);
  return true;
}

/* ============================ Terminal CLI ============================ */
static mp_editor_t g_ed;
static program_t   g_prog;
static vm_t        g_vm;

static bool        g_have_prog=false;
static bool        g_autorun=false;
static uint8_t     g_slot=1;
static bool        g_session_active=false;
static bool        g_exit_pending=false;


static bool g_edit=false;
static int  g_next_line=10;
static int  g_step=10;

static void mp_prompt(void){
  if (g_edit){
    /* In EDIT mode, show line number with > prefix for alignment */
    char b[16];
    mp_itoa(g_next_line, b);
    mp_puts("> ");
    mp_puts(b);
    mp_puts(" ");
  } else {
    /* Normal mode, show > prompt */
    mp_puts("> ");
  }
}

static void help(void){
  mp_puts("MiniPascal monitor (v4)\r\n");
  mp_puts("\r\n");
  mp_puts("=== COMMANDS ===\r\n");
  mp_puts("  EDIT         enter edit mode (starts new program)\r\n");
  mp_puts("  END          exit edit mode\r\n");
  mp_puts("  NEW          clear program\r\n");
  mp_puts("  LIST         show program\r\n");
  mp_puts("  RUN          compile and run\r\n");
  mp_puts("  STOP         stop running\r\n");
  mp_puts("  EXIT         exit Pascal mode\r\n");
  mp_puts("\r\n");
  mp_puts("=== FLASH STORAGE ===\r\n");
  mp_puts("  SAVE 1       save to slot 1 (1-3)\r\n");
  mp_puts("  SAVE 1 START save and run\r\n");
  mp_puts("  LOAD 1       load from slot\r\n");
  mp_puts("  ERASE 1      erase slot\r\n");
  mp_puts("\r\n");
  mp_puts("=== PASCAL FUNCTIONS (use comma separator) ===\r\n");
  mp_puts("  LED(idx,r,g,b,w)    set LED color (idx 1-12)\r\n");
  mp_puts("  LEDON(r,g,b,w)      set all LEDs on\r\n");
  mp_puts("  LEDOFF()            turn all LEDs off\r\n");
  mp_puts("  WAIT(ms)            delay milliseconds\r\n");
  mp_puts("  DELAY(ms)           same as WAIT\r\n");
  mp_puts("  BEEP(freq,vol,s)    beep tone (vol 0-50)\r\n");
  mp_puts("  GOTO n              jump to line n\r\n");
  mp_puts("  SETTIME(h,m,s)      set RTC time\r\n");
  mp_puts("  SETALARM(h,m,s)     set alarm\r\n");
  mp_puts("  ALARM(h,m,s)        alias for SETALARM\r\n");
  mp_puts("  ALARM()             read alarm state (0/1)\r\n");
  mp_puts("\r\n");
  mp_puts("=== READ FUNCTIONS (return value) ===\r\n");
  mp_puts("  BATTERY()    battery mV\r\n");
  mp_puts("  LIGHT()      light lux\r\n");
  mp_puts("  RNG()        random number\r\n");
  mp_puts("  TEMP()       temperature (x100)\r\n");
  mp_puts("  HUM()        humidity (x100)\r\n");
  mp_puts("  PRESS()      pressure (x100)\r\n");
  mp_puts("  BTN()        button state\r\n");
  mp_puts("  MIC()        microphone level\r\n");
  mp_puts("\r\n");
  mp_puts("=== VARIABLES ===\r\n");
  mp_puts("  x = 5        assign\r\n");
  mp_puts("  x = x + 1    expression\r\n");
  mp_puts("  IF x>5 THEN GOTO 100\r\n");
  mp_puts("\r\n");
  mp_puts("Tip: hold BL 10s to ABORT stuck program\r\n");
}
static void compile_or_report(void){
  g_have_prog=false;
  memset(&g_prog,0,sizeof(g_prog));
  if (!compile_program(&g_ed, &g_prog)){
    mp_puts("Compile error");
    if (g_err_line>0){
      char b[16]; mp_puts(" at line "); mp_itoa(g_err_line,b); mp_puts(b);
    }
    if (g_err){ mp_puts(": "); mp_puts(g_err); }
    mp_putcrlf();
    return;
  }
  g_have_prog=true;
}

static void cmd_run(void){
  compile_or_report();
  if (!g_have_prog) return;
  vm_reset(&g_vm);
  mp_puts("RUN\r\n");
}

static void cmd_stop(void){
  g_vm.stop_req=true;
  mp_puts("STOP\r\n");
}

static void handle_edit_line(const char *line){
  /* In EDIT mode we do NOT use special characters like '.' to terminate,
     because beginners might accidentally use them inside code.
     So we use a simple word on its own line: END
       - END exits EDIT mode and returns to the normal prompt.
  */
  if (!mp_stricmp(line, "END")){
    g_edit=false;
    mp_puts("EDIT OFF\r\n");
    return;
  }

  /* Store the line at the current auto line number. */
  if (!ed_set(&g_ed, g_next_line, line)){
    mp_puts("ERR: Line store failed");
    return;
  }

  /* Move to next line number - no confirmation, just next prompt */
  g_next_line += g_step;
}

static bool parse_slot_opt(const char *args, uint8_t *slot_out){
  const char *p=args;
  int s=0;
  if (!parse_int(&p,&s)) return false;
  if (s<1 || s>(int)MP_FLASH_SLOT_COUNT) return false;
  *slot_out=(uint8_t)s;
  return true;
}

static void sv_set(uint8_t idx, int32_t v){
  if (idx < MP_MAX_VARS) g_vm.vars[idx]=v;
}
static void sv_set_a(uint8_t n, int32_t v){
  if (n<8) sv_set((uint8_t)(SV_A0 + n), v);
}

static void packet_apply_named(const char *word, const int32_t *vals, uint8_t n){
  sv_set(SV_NARG, n);
  for (uint8_t i=0;i<8;i++) sv_set_a(i, (i<n)?vals[i]:0);

  if (!mp_stricmp(word,"LED")){
    sv_set(SV_CMDID, 1);
    if (n>=1) sv_set(SV_LEDI, vals[0]);
    if (n>=2) sv_set(SV_LEDR, vals[1]);
    if (n>=3) sv_set(SV_LEDG, vals[2]);
    if (n>=4) sv_set(SV_LEDB, vals[3]);
    if (n>=5) sv_set(SV_LEDW, vals[4]);
    return;
  }
  if (!mp_stricmp(word,"TIME")){
    sv_set(SV_CMDID, 2);
    if (n>=1) sv_set(SV_TIMEH, vals[0]);
    if (n>=2) sv_set(SV_TIMEM, vals[1]);
    if (n>=3) sv_set(SV_TIMES, vals[2]);
    return;
  }
  if (!mp_stricmp(word,"ALARM")){
    sv_set(SV_CMDID, 3);
    if (n>=1) sv_set(SV_ALH, vals[0]);
    if (n>=2) sv_set(SV_ALM, vals[1]);
    if (n>=3) sv_set(SV_ALS, vals[2]); else sv_set(SV_ALS, 0);
    return;
  }

  sv_set(SV_CMDID, (int32_t)fnv1a16_ci(word));
}

static bool try_parse_packet(const char *line){
  char word[MP_NAME_LEN]={0};
  int wi=0;
  while (*line==' '||*line=='\t') line++;
  if (!isalpha((unsigned char)*line)) return false;
  while (*line && !isspace((unsigned char)*line) && wi < (MP_NAME_LEN-1)){
    word[wi++] = *line++;
  }
  word[wi]=0;

  int32_t vals[8]; uint8_t n=0;
  const char *p=line;
  while (n<8){
    int v=0;
    if (!parse_int(&p,&v)) break;
    vals[n++] = (int32_t)v;
  }
  if (n==0) return false;

  packet_apply_named(word, vals, n);
  mp_puts("PKT OK\r\n");
  return true;
}

static void handle_line(char *line){
  while (*line==' '||*line=='\t') line++;
  if (!*line && !g_edit) return;  /* Empty line in normal mode - skip */
  if (!*line) return;  /* Empty line in EDIT mode - just show next prompt */

  if (g_edit){
    handle_edit_line(line);
    return;
  }

  if (isdigit((unsigned char)line[0])){
    const char *p=line;
    int ln=0;
    if (!parse_int(&p,&ln)){ mp_puts("Bad line\r\n"); return; }
    while (*p==' '||*p=='\t') p++;
    if (!ed_set(&g_ed, ln, p)) mp_puts("Line store failed\r\n");
    return;
  }

  char cmd[16]={0};
  int i=0;
  while (line[i] && !isspace((unsigned char)line[i]) && i < (int)sizeof(cmd)-1){ cmd[i]=line[i]; i++; }
  cmd[i]=0;
  char *args=line+i; while (*args==' '||*args=='\t') args++;

  if (!mp_stricmp(cmd,"HELP")) { help(); return; }
  if (!mp_stricmp(cmd,"NEW"))  { ed_init(&g_ed); mp_puts("OK\r\n"); return; }
  if (!mp_stricmp(cmd,"LIST")) { ed_list(&g_ed); return; }
  if (!mp_stricmp(cmd,"DEL"))  { int ln=0; const char *p=args; if(parse_int(&p,&ln) && ed_delete(&g_ed,ln)) mp_puts("OK\r\n"); else mp_puts("Not found\r\n"); return; }
  if (!mp_stricmp(cmd,"RUN"))  { cmd_run(); return; }
  if (!mp_stricmp(cmd,"STOP")) { cmd_stop(); return; }
  if (!mp_stricmp(cmd,"EXIT")) { g_exit_pending=true; return; }

  if (!mp_stricmp(cmd,"SLOT")) {
    uint8_t s=g_slot;
    if (*args && parse_slot_opt(args,&s)) { g_slot=s; mp_puts("OK\r\n"); }
    else { char b[16]; mp_puts("SLOT "); mp_itoa(g_slot,b); mp_puts(b); mp_putcrlf(); }
    return;
  }
  if (!mp_stricmp(cmd,"SAVE") || !mp_stricmp(cmd,"FLASH")) {
    /* Beginner-friendly "deploy" command.
       SAVE <slot>:
         1) compile (syntax check)
         2) erase the flash slot pages
         3) save the program text into the slot
       SAVE <slot> START:
         does the same AND:
         4) starts RUN immediately
    */
    uint8_t s = g_slot;
    bool want_autorun = false;

    if (*args){
      /* First optional number = slot */
      (void)parse_slot_opt(args, &s);

      /* Look for keyword START anywhere after the slot number */
      if (strstr(args, "START") || strstr(args, "start") || strstr(args, "Start")){
        want_autorun = true;
      }
    }

    /* compile first; if compile fails, do not touch flash */
    compile_or_report();
    if (!g_have_prog) return;

    /* Save autorun flag only into the saved slot */
    bool old_ar = g_autorun;
    g_autorun = want_autorun;

    if (storage_save_slot(s, &g_ed, g_autorun)){
      g_slot = s;
      mp_puts("SAVED\r\n");

      if (want_autorun){
        /* Run immediately (VM reset), without re-compiling again */
        vm_reset(&g_vm);
        mp_puts("RUN\r\n");
        g_session_active=false; /* drop back to caller */
      }
    } else {
      mp_puts("SAVE FAIL\r\n");
      g_autorun = old_ar;
    }
    return;
  }
  if (!mp_stricmp(cmd,"LOAD")) {
    uint8_t s=g_slot;
    if (*args) (void)parse_slot_opt(args,&s);
    bool ar=false;
    if (storage_load_slot(s, &g_ed, &ar)){ g_autorun=ar; g_slot=s; mp_puts("LOADED\r\n"); } else mp_puts("LOAD FAIL\r\n");
    return;
  }
  if (!mp_stricmp(cmd,"ERASE")) {
    uint8_t s=g_slot;
    if (*args) (void)parse_slot_opt(args,&s);
    if (storage_erase_slot(s)){ g_slot=s; mp_puts("ERASED\r\n"); } else mp_puts("ERASE FAIL\r\n");
    return;
  }
  if (!mp_stricmp(cmd,"AUTORUN")) {
    /* Optional global flag for boot autorun. */
    if (!mp_stricmp(args,"ON")) { g_autorun=true; mp_puts("OK\r\n"); }
    else if (!mp_stricmp(args,"OFF")) { g_autorun=false; mp_puts("OK\r\n"); }
    else mp_puts("Use: AUTORUN ON|OFF\r\n");
    return;
  }
  if (!mp_stricmp(cmd,"EDIT")) {
    /* Auto line-number mode:
       - You type Pascal statements line by line, WITHOUT numbers.
       - The monitor assigns 10,20,30,... (or your STEP) automatically.
       - To leave EDIT mode, type END on its own line.
       - Always starts fresh (clears previous code).
    */
    ed_init(&g_ed);  /* Clear previous code */
    g_edit=true;
    g_next_line = 10;
    mp_puts("NEW PROGRAM (type END to finish)\r\n");
    return;
  }

  if (!mp_stricmp(cmd,"STEP")) {
    int v=0; const char *p=args;
    if (parse_int(&p,&v) && v>0){ g_step=v; mp_puts("OK\r\n"); }
    else mp_puts("Use: STEP <n>\r\n");
    return;
  }

  if (!mp_stricmp(cmd,"ID")) {
    if (!*args){ mp_puts("Use: ID <word>\r\n"); return; }
    char w[MP_NAME_LEN]={0};
    int wi=0;
    while (*args && !isspace((unsigned char)*args) && wi < (MP_NAME_LEN-1)) w[wi++]=*args++;
    w[wi]=0;
    uint16_t id = fnv1a16_ci(w);
    char b[16];
    mp_puts("ID="); mp_itoa((int)id,b); mp_puts(b); mp_putcrlf();
    return;
  }

  if (try_parse_packet(line)) return;

  mp_puts("Unknown command. Type HELP\r\n");
}

void mp_init(void){
  /* Print help once at boot so beginners immediately see what is available. */
  help();
  ed_init(&g_ed);
  g_have_prog=false;
  g_autorun=false;
  g_edit=false;
  g_step=10;
  g_slot=1;

  bool loaded=false;
  for (uint8_t s=1; s<=MP_FLASH_SLOT_COUNT; s++){
    bool ar=false;
    mp_editor_t tmp;
    if (storage_load_slot(s, &tmp, &ar) && ar){
      g_ed = tmp;
      g_autorun=true;
      g_slot=s;
      loaded=true;
      mp_puts("Program loaded (AUTORUN) from slot ");
      char b[8]; mp_itoa(s,b); mp_puts(b); mp_putcrlf();
      break;
    }
  }
  if (!loaded){
    bool ar=false;
    if (storage_load_slot(1, &g_ed, &ar)){
      g_autorun=ar;
      g_slot=1;
      loaded=true;
      mp_puts("Program loaded from slot 1\r\n");
    }
  }

  if (loaded && g_autorun){
    mp_puts("AUTORUN\r\n");
    cmd_run();
  }
  mp_prompt();
}

void mp_request_stop(void){ g_vm.stop_req=true; }

void mp_start_session(void){
  g_session_active = true;
  g_exit_pending = false;
  g_edit = false;
  
  mp_putcrlf();
  mp_puts("PASCAL READY (HELP for commands, EDIT to program, EXIT to quit)\r\n");
  mp_prompt();
}

void mp_stop_session(void){ g_session_active=false; }
bool mp_is_active(void){ return g_session_active; }
bool mp_exit_pending(void){ return g_exit_pending; }

void mp_feed_char(char c){
  static char line[MP_LINE_LEN];
  static uint16_t n=0;
  
  /* Handle CR - some terminals send CR+LF, treat CR as Enter */
  if (c=='\r' || c=='\n'){
    mp_puts("\r\n");  /* Echo newline */
    line[n]=0; n=0;
    handle_line(line);  /* Always call - even empty line needs g_edit prompt */
    mp_prompt();
    return;
  }
  
  /* Backspace handling with echo */
  if (c==0x08 || c==0x7F){ 
    if (n>0) {
      n--;
      mp_puts("\b \b");  /* Erase character on screen */
    }
    return; 
  }
  
  /* Regular character - store and echo */
  if (n < (MP_LINE_LEN-1)) {
    line[n++] = c;
    char echo[2] = {c, 0};
    mp_puts(echo);  /* Echo the character */
  }
}

void mp_task(void){ mp_poll(); if (g_exit_pending) g_session_active=false; }

void mp_poll(void){
  uint32_t now = mp_hal_millis();
  
  /* Check BL abort button: 10 second hold to stop program */
  if (g_vm.running && g_have_prog){
    if (mp_hal_abort_pressed()){
      if (!g_abort_active){
        g_abort_active = true;
        g_abort_press_start = now;
      } else {
        if ((now - g_abort_press_start) >= MP_ABORT_HOLD_MS){
          g_vm.stop_req = true;
          g_abort_active = false;
          mp_puts("\r\nABORT (BL held 10s)\r\n");
        }
      }
    } else {
      g_abort_active = false;
    }
  }

  if (g_vm.running && g_have_prog && g_vm.stop_req){
    g_vm.running = false;
    g_vm.sleeping = false;
    mp_puts("\r\nDONE\r\n");
    mp_prompt();
    return;
  }

  if (g_vm.running && g_have_prog && g_vm.sleeping){
    if ((int32_t)(now - g_vm.wake_ms) < 0){
      HAL_PWR_EnterSLEEPMode(PWR_LOWPOWERREGULATOR_ON, PWR_SLEEPENTRY_WFI);
      return;
    }
    g_vm.sleeping = false;
  }

  if (g_vm.running && g_have_prog){
    (void)vm_step(&g_vm, &g_prog, now, 64);
    if (!g_vm.running){
      mp_puts("\r\nDONE\r\n");
      mp_prompt();
    }
  }
}

void mp_buttons_poll(void){
  /* B1/B2 program slot switching when USB not connected or no session active.
     B1 = previous slot, B2 = next slot.
     Debounce with edge detection. */
  static uint8_t b1_last = 1, b2_last = 1;  /* pulled up = 1 when not pressed */
  static uint32_t last_switch_ms = 0;
  
  if (g_session_active) return;  /* Don't switch while in terminal session */
  
  uint8_t b1 = (uint8_t)HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin);
  uint8_t b2 = (uint8_t)HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin);
  uint32_t now = mp_hal_millis();
  
  /* Debounce: ignore switches within 300ms */
  if ((now - last_switch_ms) < 300) {
    b1_last = b1;
    b2_last = b2;
    return;
  }
  
  /* B1 falling edge: switch to previous slot */
  if (b1_last == 1 && b1 == 0){
    uint8_t new_slot = (g_slot > 1) ? (g_slot - 1) : MP_FLASH_SLOT_COUNT;
    bool ar = false;
    if (storage_load_slot(new_slot, &g_ed, &ar)){
      g_slot = new_slot;
      g_autorun = ar;
      /* Compile and run if autorun set */
      compile_or_report();
      if (g_have_prog && g_autorun){
        vm_reset(&g_vm);
      }
    }
    last_switch_ms = now;
  }
  
  /* B2 falling edge: switch to next slot */
  if (b2_last == 1 && b2 == 0){
    uint8_t new_slot = (g_slot < MP_FLASH_SLOT_COUNT) ? (g_slot + 1) : 1;
    bool ar = false;
    if (storage_load_slot(new_slot, &g_ed, &ar)){
      g_slot = new_slot;
      g_autorun = ar;
      /* Compile and run if autorun set */
      compile_or_report();
      if (g_have_prog && g_autorun){
        vm_reset(&g_vm);
      }
    }
    last_switch_ms = now;
  }
  
  b1_last = b1;
  b2_last = b2;
}

/* ============================ Builtin functions ============================ */
int32_t mp_user_builtin(uint8_t id, uint8_t argc, const int32_t *argv){
  switch(id){
    case 1: /* led(...) */
      if (argc==2){ /* led(index, w) simple white */
        uint8_t idx = (argv[0]<=0)?0:(uint8_t)(argv[0]-1);
        uint8_t w = (argv[1]<0)?0:(argv[1]>255?255:(uint8_t)argv[1]);
        led_set_RGBW(idx, 0, 0, 0, w);
        led_render();
        return 0;
      }
      if (argc==5){
        uint8_t idx = (argv[0]<=0)?0:(uint8_t)(argv[0]-1);
        uint8_t r = (argv[1]<0)?0:(argv[1]>255?255:(uint8_t)argv[1]);
        uint8_t g = (argv[2]<0)?0:(argv[2]>255?255:(uint8_t)argv[2]);
        uint8_t b = (argv[3]<0)?0:(argv[3]>255?255:(uint8_t)argv[3]);
        uint8_t w = (argv[4]<0)?0:(argv[4]>255?255:(uint8_t)argv[4]);
        led_set_RGBW(idx, r, g, b, w);
        led_render();
        return 0;
      }
      return -1;

    case 2: /* wait(ms) handled specially => OP_SLEEP */
      return 0;

    case 3: /* battery() -> mV */
      if (argc==0){
        ANALOG_RequestUpdate();
        float v = ANALOG_GetBat();
        if (v < 0.0f) v = 0.0f;
        return (int32_t)(v * 1000.0f + 0.5f);
      }
      return -1;

    case 4: /* rng() -> 0..255 */
      {
        uint32_t r=0; if (HAL_RNG_GenerateRandomNumber(&hrng,&r)==HAL_OK) return (int32_t)(r & 0xFF); return -1;
      }

    case 5: /* temp() -> degC*10 */
      {
        float t=0.0f; if (T(&t)==HAL_OK) return (int32_t)(t*10.0f); return -1;
      }
    case 6: /* hum() -> %*10 */
      {
        float h=0.0f; if (RH(&h)==HAL_OK) return (int32_t)(h*10.0f); return -1;
      }
    case 7: /* press() -> hPa*10 */
      {
        float p=0.0f; if (P(&p)==HAL_OK) return (int32_t)(p*10.0f); return -1;
      }
    case 8: /* btn() -> B1B2BL as decimal 0-111 */
      {
        uint8_t b1 = (uint8_t)HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin);
        uint8_t b2 = (uint8_t)HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin);
        uint8_t bl = (uint8_t)HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin);
        return (int32_t)(b1*100 + b2*10 + bl);
      }
    case 9: /* mic() -> dbfs*100 */
      {
        float db=MIC_LastDbFS();
        return (int32_t)(db*100.0f);
      }
    case 10: /* settime(h,m,s) */
      if (argc==3){
        char buf[RTC_DATETIME_STRING_SIZE];
        /* Keep date unchanged: read current, replace HMS */
        char dt[RTC_DATETIME_STRING_SIZE];
        if (RTC_ReadClock(dt)==HAL_OK){
          int hh,mm,ss,yy,mo,dd;
          if (sscanf(dt, "%02d:%02d:%02d_%02d.%02d.%02d", &hh,&mm,&ss,&yy,&mo,&dd)==6){
            snprintf(buf,sizeof(buf), "%02ld:%02ld:%02ld_%02d.%02d.%02d", (long)argv[0], (long)argv[1], (long)argv[2], yy, mo, dd);
            if (RTC_SetClock(buf)==HAL_OK) return 0;
          }
        }
      }
      return -1;
    case 11: /* setalarm(h,m,s) daily, alarm() -> active */
      if (argc==0){
        return (int32_t)RTC_AlarmTrigger;
      }
      if (argc>=2){
        uint8_t hh = (argv[0]<0)?0:(argv[0]>23?23:(uint8_t)argv[0]);
        uint8_t mm = (argv[1]<0)?0:(argv[1]>59?59:(uint8_t)argv[1]);
        uint8_t dur= (argc>=3)? ((argv[2]<0)?0:(argv[2]>255?255:(uint8_t)argv[2])) : 30;
        if (RTC_SetDailyAlarm(hh, mm, dur)==HAL_OK) return 0;
      }
      return -1;
    case 12: /* light() -> lux */
      if (argc==0){
        ANALOG_RequestUpdate();
        float l = ANALOG_GetLight();
        if (l < 0.0f) l = 0.0f;
        return (int32_t)(l + 0.5f);
      }
      return -1;
    case 13: /* ledon(r,g,b,w) */
      if (argc==0){
        led_render();
        return 0;
      }
      if (argc==4){
        uint8_t r = (argv[0]<0)?0:(argv[0]>255?255:(uint8_t)argv[0]);
        uint8_t g = (argv[1]<0)?0:(argv[1]>255?255:(uint8_t)argv[1]);
        uint8_t b = (argv[2]<0)?0:(argv[2]>255?255:(uint8_t)argv[2]);
        uint8_t w = (argv[3]<0)?0:(argv[3]>255?255:(uint8_t)argv[3]);
        led_set_all_RGBW(r, g, b, w);
        led_render();
        return 0;
      }
      return -1;
    case 14: /* ledoff() */
      if (argc==0){
        led_set_all_RGBW(0, 0, 0, 0);
        led_render();
        return 0;
      }
      return -1;
    case 15: /* beep(freq,vol,s) */
      if (argc==3){
        int32_t f = argv[0];
        int32_t v = argv[1];
        int32_t s = argv[2];
        if (f < 1) f = 1;
        if (f > 20000) f = 20000;
        if (v < 0) v = 0;
        if (v > 50) v = 50;
        if (s < 0) s = 0;
        BEEP((uint16_t)f, (uint8_t)v, (float)s);
        return 0;
      }
      return -1;
    default:
      return -1;
  }
}

int mp_hal_getchar(void){
  /* Non-blocking: use USB CDC RX buffer? Not available here. Stubbed to -1. */
  return -1;
}
void mp_hal_putchar(char c){
  /* Routed via USB CLI printf helper */
  char b[2]={c,0};
  cdc_write_str(b);
}
uint32_t mp_hal_millis(void){ return HAL_GetTick(); }
