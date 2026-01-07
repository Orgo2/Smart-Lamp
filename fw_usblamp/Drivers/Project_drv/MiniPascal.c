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
extern const uint32_t __flash_data_start__;
extern const uint32_t __flash_data_end__;

/* ============================ Minimal utils ============================ */
static void mp_puts(const char *s){ while (*s) mp_hal_putchar(*s++); }
static void mp_putcrlf(void){ mp_puts("\r\n"); }
static void mp_prompt(void);  /* forward declaration */
static bool is_id0(char c);
static bool is_idn(char c);
static int builtin_id(const char *name);
static void time_update_vars(int32_t *vars);
static void time_print_ymdhm(void);
static bool is_time0_call(const char *line);

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

static void mp_put2(uint8_t v){
  char b[3];
  b[0] = (char)('0' + ((v / 10u) % 10u));
  b[1] = (char)('0' + (v % 10u));
  b[2] = 0;
  mp_puts(b);
}

static void mp_utoa_hex(uint32_t v, char *out){
  static const char hex[] = "0123456789ABCDEF";
  char tmp[9];
  int n=0;
  if (v==0){ out[0]='0'; out[1]=0; return; }
  while (v && n < (int)sizeof(tmp)){
    tmp[n++] = hex[v & 0xFu];
    v >>= 4;
  }
  int i=0;
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

bool mp_exec_builtin_line(const char *line, int32_t *ret_out, bool *has_ret){
  if (!line) return false;
  const char *p = line;
  while (*p==' '||*p=='\t') p++;
  if (!is_id0(*p)) return false;

  char name[MP_NAME_LEN];
  uint16_t i=0;
  while (is_idn(*p) && i < (MP_NAME_LEN-1)) name[i++] = *p++;
  name[i]=0;

  while (*p==' '||*p=='\t') p++;
  if (*p!='(') return false;
  p++;

  int32_t argv[8];
  uint8_t argc=0;
  while (1){
    while (*p==' '||*p=='\t') p++;
    if (*p==')'){ p++; break; }
    if (argc>=8) return false;
    int v=0;
    if (!parse_int(&p, &v)) return false;
    argv[argc++] = (int32_t)v;
    while (*p==' '||*p=='\t') p++;
    if (*p==','){ p++; continue; }
    if (*p==')'){ p++; break; }
    return false;
  }

  while (*p==' '||*p=='\t') p++;
  if (*p) return false;

  int id = builtin_id(name);
  if (id < 0) return false;

  bool ret = false;
  if (id==3 || id==4 || id==5 || id==6 || id==7 || id==8 || id==9 || id==12) ret = true;
  if (id==11 && argc==0) ret = true;
  if (has_ret) *has_ret = ret;

  int32_t r = 0;
  if (id==2){
    if (argc!=1 || argv[0] < 0) return false;
    uint32_t ms = (uint32_t)argv[0];
    uint32_t start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < ms){
      HAL_PWR_EnterSLEEPMode(PWR_LOWPOWERREGULATOR_ON, PWR_SLEEPENTRY_WFI);
    }
    r = 0;
  } else {
    r = mp_user_builtin((uint8_t)id, argc, argv);
    if (r < 0) return false;
  }
  if (ret_out) *ret_out = r;
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

/* B2 button abort: returns 1 if B2 pressed, 0 otherwise */
MP_WEAK int mp_hal_abort_pressed(void){
  return (HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin) == GPIO_PIN_SET) ? 1 : 0;
}

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
  SV_ALH   = 18, SV_ALM, SV_ALS,                                 /* 18..20 */
  SV_TIMEY = 21, SV_TIMEMO, SV_TIMED                             /* 21..23 */
};
#define SYSVAR_COUNT 24

static const sysvar_t g_sysvars[] = {
  {"CMDID", SV_CMDID}, {"NARG", SV_NARG},
  {"A0", SV_A0}, {"A1", SV_A1}, {"A2", SV_A2}, {"A3", SV_A3}, {"A4", SV_A4}, {"A5", SV_A5}, {"A6", SV_A6}, {"A7", SV_A7},
  {"A", SV_A0}, {"B", SV_A1}, {"C", SV_A2}, {"D", SV_A3},
  {"LEDI", SV_LEDI}, {"LEDR", SV_LEDR}, {"LEDG", SV_LEDG}, {"LEDB", SV_LEDB}, {"LEDW", SV_LEDW},
  {"TIMEH", SV_TIMEH}, {"TIMEM", SV_TIMEM}, {"TIMES", SV_TIMES},
  {"ALH", SV_ALH}, {"ALM", SV_ALM}, {"ALS", SV_ALS},
  {"TIMEY", SV_TIMEY}, {"TIMEMO", SV_TIMEMO}, {"TIMED", SV_TIMED},
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
  const uint16_t *line_nos;
  uint8_t line_count;
  uint8_t line_idx;
  uint16_t line_no;
} lex_t;

static bool is_id0(char c){ return (c=='_') || isalpha((unsigned char)c); }
static bool is_idn(char c){ return (c=='_') || isalnum((unsigned char)c); }


static void lex_init_prog(lex_t *lx, const char *s, const uint16_t *line_nos, uint8_t line_count){
  lx->s = s;
  lx->pos = 0;
  memset(&lx->cur, 0, sizeof(lx->cur));
  lx->line_nos = line_nos;
  lx->line_count = line_count;
  lx->line_idx = 0;
  lx->line_no = (line_nos && line_count) ? line_nos[0] : 0;
}

static void lex_skip_ws(lex_t *lx){
  while (lx->s[lx->pos] && (lx->s[lx->pos]==' ' || lx->s[lx->pos]=='\t' || lx->s[lx->pos]=='\r' || lx->s[lx->pos]=='\n')){
    if (lx->s[lx->pos]=='\n'){
      if (lx->line_nos && lx->line_count && (uint8_t)(lx->line_idx + 1) < lx->line_count){
        lx->line_idx++;
        lx->line_no = lx->line_nos[lx->line_idx];
      } else if (!lx->line_nos){
        lx->line_no++;
      }
    }
    lx->pos++;
  }
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
  if (!mp_stricmp(name,"time")) return 10; /* time() or time(yy,mo,dd,hh,mm) */
  if (!mp_stricmp(name,"settime")) return 10; /* alias */
  if (!mp_stricmp(name,"setalarm"))return 11; /* setalarm(h,m,s) */
  if (!mp_stricmp(name,"alarm")) return 11; /* alias for setalarm */
  if (!mp_stricmp(name,"light")) return 12;
  if (!mp_stricmp(name,"ledon")) return 13;
  if (!mp_stricmp(name,"ledoff")) return 14;
  if (!mp_stricmp(name,"beep")) return 15;
  return -1;
}

/* -------- Recursive descent expression -------- */
typedef struct {
  lex_t lx;
  program_t *p;
  int line;
  uint8_t line_count;
  int16_t last_line_idx;
} Ctx;
static void nx(Ctx *c){
  lex_next(&c->lx);
  if (c->line_count && c->p){
    int16_t cur = (int16_t)c->lx.line_idx;
    if (c->last_line_idx != cur){
      int16_t from = (c->last_line_idx < 0) ? 0 : (int16_t)(c->last_line_idx + 1);
      for (int16_t i = from; i <= cur && i < (int16_t)c->line_count; i++){
        if (c->p->line_addr[i] == 0xFFFFu) c->p->line_addr[i] = c->p->len;
      }
      c->last_line_idx = cur;
    }
  }
  c->line = c->lx.line_no;
}
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
  if (ac(c, T_END)) return true;

  if (c->lx.cur.k == T_ID) return st_assign_or_call(c);
  set_err("expected statement", c->line);
  return false;
}

static int editor_index_by_line(const mp_editor_t *ed, uint16_t line_no);

static bool compile_program(const mp_editor_t *ed, program_t *out){
  memset(out, 0, sizeof(*out));
  for (uint8_t i=0;i<MP_MAX_LINES;i++) out->line_addr[i]=0xFFFFu;
  g_err=0; g_err_line=-1;

  if (MP_MAX_VARS < SYSVAR_COUNT + 8){
    set_err("MP_MAX_VARS too small for system vars", -1);
    return false;
  }

  if (ed->count == 0){
    if(!emit_u8(out, OP_HALT)) set_err("bytecode overflow", -1);
    return (g_err==0);
  }

  char buf[MP_MAX_LINES * MP_LINE_LEN];
  uint16_t line_nos[MP_MAX_LINES];
  size_t pos = 0;
  for (uint8_t i=0;i<ed->count;i++){
    line_nos[i] = (uint16_t)ed->lines[i].line_no;
    size_t len = strnlen(ed->lines[i].text, MP_LINE_LEN-1);
    if (pos + len + 1 >= sizeof(buf)){
      set_err("program too long", ed->lines[i].line_no);
      return false;
    }
    memcpy(&buf[pos], ed->lines[i].text, len);
    pos += len;
    if (i + 1 < ed->count) buf[pos++] = '\n';
  }
  buf[pos] = 0;

  Ctx c; memset(&c, 0, sizeof(c));
  c.p = out;
  c.line_count = ed->count;
  c.last_line_idx = -1;
  lex_init_prog(&c.lx, buf, line_nos, ed->count);
  nx(&c);
  if(!stmt_list_until(&c, T_EOF)) return false;

  if (!g_err){
    if(!emit_u8(out, OP_HALT)) set_err("bytecode overflow", -1);
  }

  for (uint8_t i=0;i<ed->count;i++){
    if (out->line_addr[i] == 0xFFFFu) out->line_addr[i] = out->len;
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

static int editor_index_by_line(const mp_editor_t *ed, uint16_t line_no){
  for (uint8_t i=0;i<ed->count;i++) if (ed->lines[i].line_no == (int)line_no) return i;
  return -1;
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

/* ============================ Flash storage: slots inside linker FLASH_DATA region ============================ */
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

static uint32_t flash_data_start(void){
  return (uint32_t)&__flash_data_start__;
}

static uint32_t flash_data_end(void){
  return (uint32_t)&__flash_data_end__;
}

static uint32_t flash_data_size(void){
  uint32_t start = flash_data_start();
  uint32_t end = flash_data_end();
  return (end > start) ? (end - start) : 0u;
}

static const char *g_flash_err = 0;
static uint32_t g_flash_hal_err = 0;

static void flash_err_clear(void){
  g_flash_err = 0;
  g_flash_hal_err = 0;
}

static void flash_err_set(const char *msg){
  g_flash_err = msg;
  g_flash_hal_err = HAL_FLASH_GetError();
}

static void flash_clear_errors(void){
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PROGERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_WRPERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGAERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_SIZERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGSERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_MISERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_FASTERR);
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);
}

static uint32_t slot_size_bytes(void){
  uint32_t total = flash_data_size();
  if (total == 0) return 0u;
  uint32_t slot = total / (uint32_t)MP_FLASH_SLOT_COUNT;
  slot = (slot / (uint32_t)MP_FLASH_PAGE_SIZE) * (uint32_t)MP_FLASH_PAGE_SIZE;
  return slot;
}

static uint32_t slot_base_addr(uint8_t slot){
  uint32_t ss = slot_size_bytes();
  uint32_t start = flash_data_start();
  if (slot < 1) slot = 1;
  if (slot > MP_FLASH_SLOT_COUNT) slot = MP_FLASH_SLOT_COUNT;
  return start + ss * (uint32_t)(slot - 1);
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

typedef struct {
  uint32_t addr;
  uint8_t buf[8];
  uint8_t fill;
} flash_stream_t;

static bool flash_prog_dw(uint32_t addr, const uint8_t *buf){
  uint64_t dw = 0xFFFFFFFFFFFFFFFFull;
  uint8_t *pdw = (uint8_t*)&dw;
  for (uint32_t k=0;k<8;k++) pdw[k] = buf[k];
  return (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dw) == HAL_OK);
}

static bool flash_stream_init(flash_stream_t *s, uint32_t base){
  if (!s || (base & 0x7u)) return false;
  s->addr = base;
  s->fill = 0;
  memset(s->buf, 0xFF, sizeof(s->buf));
  return true;
}

static bool flash_stream_write(flash_stream_t *s, const uint8_t *data, uint32_t len){
  if (!s || !data) return false;
  for (uint32_t i=0;i<len;i++){
    s->buf[s->fill++] = data[i];
    if (s->fill == 8){
      if (!flash_prog_dw(s->addr, s->buf)) return false;
      s->addr += 8;
      s->fill = 0;
      memset(s->buf, 0xFF, sizeof(s->buf));
    }
  }
  return true;
}

static bool flash_stream_flush(flash_stream_t *s){
  if (!s) return false;
  if (s->fill == 0) return true;
  if (!flash_prog_dw(s->addr, s->buf)) return false;
  s->addr += 8;
  s->fill = 0;
  memset(s->buf, 0xFF, sizeof(s->buf));
  return true;
}

static bool storage_erase_slot(uint8_t slot){
  flash_err_clear();
  uint32_t base = slot_base_addr(slot);
  uint32_t size = slot_size_bytes();
  if (size == 0){ flash_err_set("slot size"); return false; }
  if ((base + size) > flash_data_end()){ flash_err_set("slot range"); return false; }
  flash_clear_errors();
  if(!flash_unlock()){ flash_err_set("unlock"); return false; }
  bool ok = flash_erase_region(base, size);
  if (!ok) flash_err_set("erase");
  flash_lock();
  return ok;
}

static bool storage_save_slot(uint8_t slot, const mp_editor_t *ed, bool autorun){
  flash_err_clear();
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
  if (slot_size == 0){ flash_err_set("slot size"); return false; }
  if (total > slot_size){ flash_err_set("too big"); return false; }

  uint32_t base = slot_base_addr(slot);
  if ((base + slot_size) > flash_data_end()){ flash_err_set("slot range"); return false; }

  flash_clear_errors();
  if(!flash_unlock()){ flash_err_set("unlock"); return false; }
  bool ok = flash_erase_region(base, slot_size);
  if (!ok){ flash_err_set("erase"); }

  flash_stream_t fs;
  if (ok && !flash_stream_init(&fs, base)){
    flash_err_set("align");
    ok = false;
  }
  if (ok){
    ok = flash_stream_write(&fs, (const uint8_t*)&hdr, sizeof(hdr));
    if (!ok) flash_err_set("prog hdr");
  }

  for (uint8_t i=0;i<ed->count && ok;i++){
    uint16_t ln = (uint16_t)ed->lines[i].line_no;
    uint8_t slen = (uint8_t)strnlen(ed->lines[i].text, MP_LINE_LEN-1);
    uint8_t rec_hdr[3] = { (uint8_t)(ln&0xFF), (uint8_t)((ln>>8)&0xFF), slen };
    ok = flash_stream_write(&fs, rec_hdr, 3);
    if (ok){ ok = flash_stream_write(&fs, (const uint8_t*)ed->lines[i].text, slen); }
    if (!ok){ flash_err_set("prog data"); }
  }
  if (ok && !flash_stream_flush(&fs)){
    flash_err_set("prog data");
    ok = false;
  }

  flash_lock();
  return ok;
}

static bool storage_load_slot(uint8_t slot, mp_editor_t *ed, bool *autorun_out){
  flash_err_clear();
  uint32_t base = slot_base_addr(slot);
  uint32_t slot_size = slot_size_bytes();
  if (slot_size == 0){ flash_err_set("slot size"); return false; }
  if ((base + slot_size) > flash_data_end()){ flash_err_set("slot range"); return false; }
  const mp_hdr_t *hdr = (const mp_hdr_t*)base;
  if (hdr->magic != MP_MAGIC || hdr->version != 2) return false;
  if (hdr->count > MP_MAX_LINES) return false;

  uint32_t total = sizeof(*hdr) + hdr->data_len;
  if (total > slot_size) return false;

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
  mp_puts("MiniPascal monitor\r\n");
  mp_puts("\r\n");
  mp_puts("=== COMMANDS ===\r\n");
  mp_puts("  EDIT         enter edit mode (new program)\r\n");
  mp_puts("  END.         exit edit mode\r\n");
  mp_puts("  NEW          clear program\r\n");
  mp_puts("  LIST         show program\r\n");
  mp_puts("  RUN          compile and run\r\n");
  mp_puts("  STOP         stop running\r\n");
  mp_puts("  EXIT         exit Pascal mode\r\n");
  mp_puts("\r\n");
  mp_puts("=== EDIT MODE ===\r\n");
  mp_puts("  Type statements without line numbers.\r\n");
  mp_puts("  (Line numbers are optional and will be stripped.)\r\n");
  mp_puts("\r\n");
  mp_puts("=== FLASH STORAGE ===\r\n");
  mp_puts("  SAVE 1       save to slot 1 (1-3)\r\n");
  mp_puts("  SAVE 1 START save and run\r\n");
  mp_puts("  LOAD 1       load from slot\r\n");
  mp_puts("  ERASE 1      erase slot\r\n");
  mp_puts("\r\n");
  mp_puts("=== PASCAL FUNCTIONS ===\r\n");
  mp_puts("  LED(idx,r,g,b,w)    set LED color (idx 1-12)\r\n");
  mp_puts("  LEDON(r,g,b,w)      set all LEDs on\r\n");
  mp_puts("  LEDOFF()            turn all LEDs off\r\n");
  mp_puts("  WAIT(ms)            delay milliseconds\r\n");
  mp_puts("  DELAY(ms)           same as WAIT\r\n");
  mp_puts("  BEEP(freq,vol,ms)   beep tone (vol 0-50)\r\n");
  mp_puts("  GOTO n              jump to line n\r\n");
  mp_puts("  TIME()              update TIMEY/TIMEMO/TIMED/TIMEH/TIMEM (and TIMES)\r\n");
  mp_puts("  TIME() in CLI prints: YY,MM,DD,HH,MM\r\n");
  mp_puts("  TIME(yy,mo,dd,hh,mm) set RTC date/time\r\n");
  mp_puts("  SETTIME(...)        alias for TIME\r\n");
  mp_puts("  ALARM(h,m,s)        set alarm\r\n");
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
  mp_puts("=== FLOW CONTROL ===\r\n");
  mp_puts("  10 x:=1\r\n");
  mp_puts("  20 if (x>0) then led(1,255,0,0,0)\r\n");
  mp_puts("  30 end\r\n");
  mp_puts("\r\n");
  mp_puts("  10 x:=1\r\n");
  mp_puts("  20 if (x>0) then begin\r\n");
  mp_puts("  30 led(1,255,0,0,0)\r\n");
  mp_puts("  40 end\r\n");
  mp_puts("  50 end\r\n");
  mp_puts("\r\n");
  mp_puts("  10 x:=3\r\n");
  mp_puts("  20 while (x>0) do begin\r\n");
  mp_puts("  30 led(x,255,0,0,0)\r\n");
  mp_puts("  40 x:=x-1\r\n");
  mp_puts("  50 end\r\n");
  mp_puts("  60 end\r\n");
  mp_puts("\r\n");
  mp_puts("  10 x:=3\r\n");
  mp_puts("  20 repeat\r\n");
  mp_puts("  30 x:=x-1\r\n");
  mp_puts("  40 until (x<1)\r\n");
  mp_puts("  50 end\r\n");
  mp_puts("\r\n");
  mp_puts("=== VARIABLES ===\r\n");
  mp_puts("  x := 5       assign\r\n");
  mp_puts("  x := x + 1   expression\r\n");
  mp_puts("  IF x>5 THEN GOTO 100\r\n");
  mp_puts("  TIME() then TIMEY/TIMEMO/TIMED/TIMEH/TIMEM\r\n");
  mp_puts("\r\n");
  mp_puts("Tip: hold B2 5s to reset MCU\r\n");
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
  /* In EDIT mode, END. finishes editing (so END can be used in code). */
  if (!mp_stricmp(line, "END.")){
    g_edit=false;
    mp_puts("EDIT OFF\r\n");
    return;
  }

  int line_no = g_next_line;
  const char *p = line;
  while (*p==' '||*p=='\t') p++;
  int ln=0;
  const char *p_num = p;
  if (parse_int(&p_num,&ln)){
    if (*p_num==' '||*p_num=='\t'){
      while (*p_num==' '||*p_num=='\t') p_num++;
      if (ln > 0){
        line_no = ln;
        line = p_num;
      }
    }
  }

  /* Store the line at the current (or provided) line number. */
  if (!ed_set(&g_ed, line_no, line)){
    mp_puts("ERR: Line store failed");
    return;
  }

  /* Move to next line number - no confirmation, just next prompt */
  if (line_no == g_next_line) g_next_line += g_step;
  else g_next_line = line_no + g_step;
}

static bool parse_slot_opt(const char *args, uint8_t *slot_out){
  const char *p=args;
  int s=0;
  if (!parse_int(&p,&s)) return false;
  if (s<1 || s>(int)MP_FLASH_SLOT_COUNT) return false;
  *slot_out=(uint8_t)s;
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
      mp_puts("SAVE FAIL");
      if (g_flash_err){
        mp_puts(": ");
        mp_puts(g_flash_err);
        if (g_flash_hal_err){
          mp_puts(" err=0x");
          char b[12];
          mp_utoa_hex(g_flash_hal_err, b);
          mp_puts(b);
        }
      }
      mp_putcrlf();
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
    mp_puts("NEW PROGRAM (type END. to finish, no line numbers)\r\n");
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

  {
    int32_t ret = 0;
    bool has_ret = false;
    if (mp_exec_builtin_line(line, &ret, &has_ret)){
      if (has_ret){
        char b[16];
        mp_itoa(ret, b);
        mp_puts(b);
        mp_putcrlf();
      } else if (is_time0_call(line)){
        time_print_ymdhm();
      } else {
        mp_puts("OK\r\n");
      }
      return;
    }
  }

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

  static uint32_t last_time_ms = 0;
  if ((uint32_t)(now - last_time_ms) >= 1000u){
    last_time_ms = now;
    time_update_vars(g_vm.vars);
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

static bool time_read_ymdhms(int *yy, int *mo, int *dd, int *hh, int *mm, int *ss){
  if (!yy || !mo || !dd || !hh || !mm || !ss) return false;
  char dt[RTC_DATETIME_STRING_SIZE];
  if (RTC_ReadClock(dt) != HAL_OK) return false;
  int t_h=0,t_m=0,t_s=0,t_y=0,t_mo=0,t_d=0;
  if (sscanf(dt, "%02d:%02d:%02d_%02d.%02d.%02d", &t_h,&t_m,&t_s,&t_y,&t_mo,&t_d) != 6) return false;
  *yy = t_y; *mo = t_mo; *dd = t_d; *hh = t_h; *mm = t_m; *ss = t_s;
  return true;
}

static void time_update_vars(int32_t *vars){
  if (!vars) return;
  int yy=0,mo=0,dd=0,hh=0,mm=0,ss=0;
  if (!time_read_ymdhms(&yy,&mo,&dd,&hh,&mm,&ss)) return;
  vars[SV_TIMEY] = yy;
  vars[SV_TIMEMO] = mo;
  vars[SV_TIMED] = dd;
  vars[SV_TIMEH] = hh;
  vars[SV_TIMEM] = mm;
  vars[SV_TIMES] = ss;
}

static void time_print_ymdhm(void){
  time_update_vars(g_vm.vars);
  mp_put2((uint8_t)g_vm.vars[SV_TIMEY]); mp_puts(",");
  mp_put2((uint8_t)g_vm.vars[SV_TIMEMO]); mp_puts(",");
  mp_put2((uint8_t)g_vm.vars[SV_TIMED]); mp_puts(",");
  mp_put2((uint8_t)g_vm.vars[SV_TIMEH]); mp_puts(",");
  mp_put2((uint8_t)g_vm.vars[SV_TIMEM]);
  mp_putcrlf();
}

static bool is_time0_call(const char *line){
  if (!line) return false;
  const char *p = line;
  while (*p==' '||*p=='\t') p++;
  if (!is_id0(*p)) return false;
  char name[MP_NAME_LEN];
  uint16_t i=0;
  while (is_idn(*p) && i < (MP_NAME_LEN-1)) name[i++] = *p++;
  name[i]=0;
  if (mp_stricmp(name, "time") != 0) return false;
  while (*p==' '||*p=='\t') p++;
  if (*p!='(') return false;
  p++;
  while (*p==' '||*p=='\t') p++;
  if (*p!=')') return false;
  p++;
  while (*p==' '||*p=='\t') p++;
  return (*p==0);
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
    case 10: /* time() or time(yy,mo,dd,hh,mm) */
      if (argc==0){
        time_update_vars(g_vm.vars);
        return 0;
      }
      if (argc==5){
        uint8_t yy = (argv[0]<0)?0:(argv[0]>99?99:(uint8_t)argv[0]);
        uint8_t mo = (argv[1]<1)?1:(argv[1]>12?12:(uint8_t)argv[1]);
        uint8_t dd = (argv[2]<1)?1:(argv[2]>31?31:(uint8_t)argv[2]);
        uint8_t hh = (argv[3]<0)?0:(argv[3]>23?23:(uint8_t)argv[3]);
        uint8_t mm = (argv[4]<0)?0:(argv[4]>59?59:(uint8_t)argv[4]);
        char buf[RTC_DATETIME_STRING_SIZE];
        snprintf(buf,sizeof(buf), "%02u:%02u:%02u_%02u.%02u.%02u", hh, mm, 0u, yy, mo, dd);
        if (RTC_SetClock(buf)==HAL_OK){
          time_update_vars(g_vm.vars);
          return 0;
        }
        return -1;
      }
      if (argc==3){
        int yy=0,mo=0,dd=0,hh=0,mm=0,ss=0;
        if (!time_read_ymdhms(&yy,&mo,&dd,&hh,&mm,&ss)) return -1;
        char buf[RTC_DATETIME_STRING_SIZE];
        snprintf(buf,sizeof(buf), "%02ld:%02ld:%02ld_%02d.%02d.%02d", (long)argv[0], (long)argv[1], (long)argv[2], yy, mo, dd);
        if (RTC_SetClock(buf)==HAL_OK){
          time_update_vars(g_vm.vars);
          return 0;
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
    case 15: /* beep(freq,vol,ms) */
      if (argc==3){
        int32_t f = argv[0];
        int32_t v = argv[1];
        int32_t ms = argv[2];
        if (f < 1) f = 1;
        if (f > 20000) f = 20000;
        if (v < 0) v = 0;
        if (v > 50) v = 50;
        if (ms < 0) ms = 0;
        BEEP((uint16_t)f, (uint8_t)v, (float)ms / 1000.0f);
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
