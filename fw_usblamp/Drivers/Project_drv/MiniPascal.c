/*
 * MiniPascal.c - A tiny Pascal-like interpreter used by the USB CLI.
 *
 * Pipeline: text lines -> lexer (tokens) -> compiler (bytecode) -> VM (runs bytecode in small steps).
 */

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <MiniPascal.h>

#include "stm32u0xx_hal.h"
#include "led.h"
#include "bme280.h"
#include "analog.h"
#include "charger.h"
#include "rtc.h"
#include "mic.h"
#include "alarm.h"
#include "main.h"
#include "lp_delay.h"

/* External peripherals from main.c */
extern RNG_HandleTypeDef hrng;
extern RTC_HandleTypeDef hrtc;
extern void SystemClock_Config(void);
extern const uint32_t __flash_data_start__;
extern const uint32_t __flash_data_end__;

/* Minimal helpers used by the interpreter (printing, formatting, small utilities). */
static void mp_puts(const char *s){ while (*s) mp_hal_putchar(*s++); }
static void mp_putcrlf(void){ mp_puts("\r\n"); }
static void mp_prompt(void);  /* forward declaration */
static bool is_id0(char c);
static bool is_idn(char c);
static int builtin_id(const char *name);
static void time_update_vars(int32_t *vars);
static void time_print_ymdhm(void);
static bool is_time0_call(const char *line);
static int time_sel_id(const char *name);
static bool g_session_active;

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

/*
 * Low-power delay (battery mode).
 * Implementation is provided by the board layer (main.c) so MiniPascal stays hardware-agnostic.
 */
#define MP_DELAY_STOP2_THRESHOLD_MS 20u

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

static const char* mic_err_name(mic_err_t e){
  switch (e){
    case MIC_ERR_OK: return "OK";
    case MIC_ERR_NOT_INIT: return "NOT_INIT";
    case MIC_ERR_SPI_NOT_READY: return "SPI_NOT_READY";
    case MIC_ERR_START_DMA: return "START_DMA";
    case MIC_ERR_TIMEOUT: return "TIMEOUT";
    case MIC_ERR_SPI_ERROR: return "SPI_ERROR";
    case MIC_ERR_DMA_NO_WRITE: return "DMA_NO_WRITE";
    case MIC_ERR_DATA_STUCK: return "DATA_STUCK";
    case MIC_ERR_SIGNAL_SATURATED: return "SIGNAL_SATURATED";
    case MIC_ERR_NO_DATA_YET: return "NO_DATA_YET";
    default: return "UNKNOWN";
  }
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
  if (id==3 || id==4 || id==5 || id==6 || id==7 || id==9 || id==12 || id==16) ret = true;
  if (id==10 && argc==1) ret = true; /* time(sel) */
  if (id==11 && argc==0) ret = true; /* alarm() */
  if (has_ret) *has_ret = ret;

  int32_t r = 0;
  if (id==2){
    if (argc!=1 || argv[0] < 0) return false;
    uint32_t ms = (uint32_t)argv[0];
    LP_DELAY(ms);
    r = 0;
  } else {
    r = mp_user_builtin((uint8_t)id, argc, argv);
    /* Negative values are valid (e.g. MIC() dBFS, TEMP() below zero).
       Treat them as normal results; for "void" calls, print the error code. */
    if (!ret && r < 0){
      ret = true;
      if (has_ret) *has_ret = true;
    }
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

/*
 * Program editor storage.
 * Keeps the program as numbered lines (like classic BASIC) so you can edit/replace single lines.
 */
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

static void ed_list(const mp_editor_t *ed){
  char num[16];
  for (uint8_t i=0;i<ed->count;i++){
    mp_itoa(ed->lines[i].line_no, num);
    mp_puts(num); mp_puts(" "); mp_puts(ed->lines[i].text); mp_putcrlf();
  }
}

/*
 * System variables (fixed slots in vm->vars[]).
 * These names are visible to Pascal code and are updated by the runtime (time, last command args, etc).
 */
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

/*
 * Lexer: reads program text and produces tokens (numbers, identifiers, symbols).
 * The compiler uses these tokens to understand the program structure.
 */
typedef enum {
  T_EOF=0, T_NUM, T_ID, T_STR,
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
  char str[MP_LINE_LEN];
  uint8_t slen;
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

  if (c=='\'' || c=='"'){
    char quote = c;
    lx->pos++;
    uint16_t n=0;
    while (lx->s[lx->pos] && lx->s[lx->pos]!=quote && lx->s[lx->pos]!='\n' && lx->s[lx->pos]!='\r'){
      if (n < (MP_LINE_LEN-1)) t.str[n++] = lx->s[lx->pos];
      lx->pos++;
    }
    if (lx->s[lx->pos]==quote) lx->pos++;
    t.str[n]=0;
    t.slen=(uint8_t)n;
    t.k=T_STR;
    lx->cur=t;
    return;
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

/*
 * Compiler: parses tokens and emits tiny bytecode instructions into program_t.
 * Bytecode is a compact "instruction list" that the VM can execute quickly.
 */
typedef enum {
  OP_HALT=0,
  OP_PUSHI, OP_LOAD, OP_STORE,
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
  OP_EQ, OP_NEQ, OP_LT, OP_LTE, OP_GT, OP_GTE,
  OP_AND, OP_OR, OP_NOT,
  OP_JMP, OP_JZ,
  OP_CALL,      /* u8 id, u8 argc */
  OP_SLEEP,     /* u32 ms */
  OP_PRINTI,    /* pop int, print */
  OP_PRINTS,    /* u8 len, bytes */
  OP_PRINTNL    /* print CRLF */
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
static bool emit_bytes(program_t *p, const void *data, uint16_t len){
  if (p->len+len>MP_BC_MAX) return false;
  memcpy(&p->bc[p->len], data, len);
  p->len += len;
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

/*
 * Builtin name -> id table (compiler side).
 * Pascal calls like `led(1,255)` are mapped to small numeric IDs used by the VM.
 */
static int builtin_id(const char *name){
  /* LED control (Drivers/Project_drv/led.*). */
  if (!mp_stricmp(name,"led"))   return 1;
  if (!mp_stricmp(name,"ledon")) return 13;
  if (!mp_stricmp(name,"ledoff")) return 14;

  /* Timing/power: delay is executed by the VM so programs sleep without blocking the CLI. */
  if (!mp_stricmp(name,"delay")) return 2;

  /* Analog measurements (Drivers/Project_drv/analog.*). */
  if (!mp_stricmp(name,"battery")) return 3;
  if (!mp_stricmp(name,"light")) return 12;

  /* RNG peripheral (main.c hrng). */
  if (!mp_stricmp(name,"rng"))   return 4;

  /* BME280 sensor (Drivers/Project_drv/bme280.*). */
  if (!mp_stricmp(name,"temp"))  return 5;
  if (!mp_stricmp(name,"hum"))   return 6;
  if (!mp_stricmp(name,"press")) return 7;

  /* Button events (short presses queued by mp_buttons_poll). */
  if (!mp_stricmp(name,"btn"))   return 16;
  if (!mp_stricmp(name,"btne"))  return 16; /* Backward compatible alias. */

  /* Microphone processing (Drivers/Project_drv/mic.*). */
  if (!mp_stricmp(name,"mic"))   return 9;

  /* RTC clock/alarm (Drivers/Project_drv/rtc.*). */
  if (!mp_stricmp(name,"time")) return 10;      /* time() or time(sel) */
  if (!mp_stricmp(name,"settime")) return 17;   /* settime(yy,mo,dd,hh,mm) or settime(hh,mm,ss) */
  if (!mp_stricmp(name,"alarm")) return 11;     /* alarm() -> active? */
  if (!mp_stricmp(name,"setalarm"))return 18;   /* setalarm(hh,mm[,duration_sec]) daily */

  /* Beeper (Drivers/Project_drv/alarm.*). */
  if (!mp_stricmp(name,"beep")) return 15;
  return -1;
}

/* Expression parser: turns tokens into bytecode for arithmetic/logic and function calls. */
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

static bool time_arg(Ctx *c){
  if (c->lx.cur.k==T_ID){
    int sel = time_sel_id(c->lx.cur.id);
    if (sel >= 0){
      nx(c);
      if (!emit_pushi(c->p, sel)){ set_err("bytecode overflow", c->line); return false; }
      return true;
    }
  }
  return expr(c);
}

static bool primary(Ctx *c){
  if (c->lx.cur.k==T_NUM){
    int32_t v=c->lx.cur.num; nx(c);
    if (!emit_pushi(c->p, v)){ set_err("bytecode overflow", c->line); return false; }
    return true;
  }
  if (c->lx.cur.k==T_STR){
    set_err("string literal not allowed here", c->line);
    return false;
  }
  if (c->lx.cur.k==T_ID){
    char nm[MP_NAME_LEN]; strncpy(nm,c->lx.cur.id,MP_NAME_LEN); nm[MP_NAME_LEN-1]=0;
    nx(c);

    if (ac(c, T_LP)){
      int id = builtin_id(nm);
      if (id<0){ set_err("unknown function", c->line); return false; }

      uint8_t argc=0;
      bool is_time = (id==10);
      if (!ac(c, T_RP)){
        while (1){
          if (is_time) { if (!time_arg(c)) return false; }
          else { if (!expr(c)) return false; }
          argc++;
          if (argc>8){ set_err("too many args", c->line); return false; }
          if (ac(c, T_COMMA)) continue;
          if (!ex(c, T_RP, "expected ')'")) return false;
          break;
        }
      }

      if (id==2){
        set_err("delay only as statement", c->line); return false;
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

/* Statement parser: turns IF/WHILE/BEGIN/END/REPEAT/GOTO into bytecode control flow. */
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

static bool st_writeln(Ctx *c){
  nx(c);
  if (!ex(c, T_LP, "expected '('")) return false;
  if (ac(c, T_RP)){
    if(!emit_u8(c->p, OP_PRINTNL)){ set_err("bytecode overflow", c->line); return false; }
    return true;
  }
  while (1){
    if (c->lx.cur.k==T_STR){
      uint8_t len = c->lx.cur.slen;
      if(!emit_u8(c->p, OP_PRINTS) || !emit_u8(c->p, len) || !emit_bytes(c->p, c->lx.cur.str, len)){
        set_err("bytecode overflow", c->line); return false;
      }
      nx(c);
    } else {
      if(!expr(c)) return false;
      if(!emit_u8(c->p, OP_PRINTI)){ set_err("bytecode overflow", c->line); return false; }
    }
    if (ac(c, T_COMMA)) continue;
    if(!ex(c, T_RP, "expected ')'")) return false;
    break;
  }
  if(!emit_u8(c->p, OP_PRINTNL)){ set_err("bytecode overflow", c->line); return false; }
  return true;
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
  bool is_time = (id==10);
  if (!ac(c, T_RP)){
    while (1){
      if (is_time) { if (!time_arg(c)) return false; }
      else { if(!expr(c)) return false; }
      argc++;
      if (argc>8){ set_err("too many args", c->line); return false; }
      if (ac(c, T_COMMA)) continue;
      if(!ex(c, T_RP, "expected ')'")) return false;
      break;
    }
  }

  if (id==2 && argc!=1){ set_err("delay expects 1 arg", c->line); return false; }

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

  if (c->lx.cur.k == T_ID && !mp_stricmp(c->lx.cur.id, "writeln")) return st_writeln(c);

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

/*
 * VM (virtual machine): executes the compiled bytecode.
 * It is stack-based and runs only a small number of ops per poll to keep UI responsive.
 */
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

        /* delay(ms) is handled in the VM so it can pause the program cooperatively. */
        if (id==2){
          if (argc!=1){ vm->running=false; break; }
          int32_t ms = argv[0];
          if (ms < 0) ms = 0;
          if ((mp_hal_usb_connected() == 0) && !g_session_active && ((uint32_t)ms >= MP_DELAY_STOP2_THRESHOLD_MS))
          {
            LP_DELAY((uint32_t)ms);
            if(!push(vm,0)) vm->running=false;
            break;
          }
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
      case OP_PRINTI: {
        if(!pop(vm,&a)) { vm->running=false; break; }
        if (mp_hal_usb_connected()){
          char b[16];
          mp_itoa(a, b);
          mp_puts(b);
        }
      } break;
      case OP_PRINTS: {
        uint8_t len = p->bc[vm->ip++];
        if (mp_hal_usb_connected()){
          for (uint8_t i=0;i<len;i++){
            mp_hal_putchar((char)p->bc[vm->ip++]);
          }
        } else {
          vm->ip = (uint16_t)(vm->ip + len);
        }
      } break;
      case OP_PRINTNL: {
        if (mp_hal_usb_connected()){
          mp_putcrlf();
        }
      } break;

      default: vm->running=false; break;
    }
    ops++;
  }
  return vm->running;
}

/*
 * Flash program storage.
 * Each slot stores the compiled program in the linker FLASH_DATA region (save/load/autorun).
 */
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

static bool storage_slot_has_program(uint8_t slot){
  uint32_t base = slot_base_addr(slot);
  uint32_t slot_size = slot_size_bytes();
  if (slot_size == 0) return false;
  if ((base + slot_size) > flash_data_end()) return false;

  const mp_hdr_t *hdr = (const mp_hdr_t*)base;
  if (hdr->magic != MP_MAGIC || hdr->version != 2) return false;
  if (hdr->count == 0 || hdr->count > MP_MAX_LINES) return false;

  uint32_t total = sizeof(*hdr) + hdr->data_len;
  if (total > slot_size) return false;

  mp_hdr_t h0 = *hdr;
  uint32_t stored = h0.checksum;
  h0.checksum = 0;

  uint32_t h = 2166136261u;
  h = fnv1a32_update(h, &h0, sizeof(h0));
  const uint8_t *p = (const uint8_t*)base + sizeof(*hdr);
  h = fnv1a32_update(h, p, hdr->data_len);
  return (h == stored);
}

static uint8_t slot_step(uint8_t slot, int dir){
  if (dir >= 0) return (slot < MP_FLASH_SLOT_COUNT) ? (uint8_t)(slot + 1) : 1;
  return (slot > 1) ? (uint8_t)(slot - 1) : (uint8_t)MP_FLASH_SLOT_COUNT;
}

static uint8_t slot_find_next_program(uint8_t from_slot, int dir){
  uint8_t s = from_slot;
  for (uint8_t i=0; i<MP_FLASH_SLOT_COUNT; i++){
    s = slot_step(s, dir);
    if (storage_slot_has_program(s)) return s;
  }
  return from_slot;
}

static uint8_t slot_find_first_program(void){
  for (uint8_t s = 1; s <= MP_FLASH_SLOT_COUNT; s++){
    if (storage_slot_has_program(s)) return s;
  }
  return 0;
}

static uint8_t g_first_program_slot = 0;

static void refresh_program_slot_cache(void){
  g_first_program_slot = slot_find_first_program();
}

uint8_t mp_first_program_slot(void){
  return g_first_program_slot;
}

/*
 * Terminal CLI.
 * Implements EDIT/SAVE/LOAD/RUN/STOP and prints help over USB CDC.
 */
static mp_editor_t g_ed;
static program_t   g_prog;
static vm_t        g_vm;

static bool        g_have_prog=false;
static uint8_t     g_slot=1;
static bool        g_session_active=false;
static bool        g_exit_pending=false;

static bool g_edit=false;
static int  g_step=10;
static volatile uint8_t g_run_slot_req = 0;
static volatile uint8_t g_run_loaded_req = 0;
static volatile uint8_t g_run_next_req = 0;
static volatile uint8_t g_usb_detach_req = 0;
static uint8_t g_edit_slot = 0;

typedef struct {
  uint8_t active;
  uint8_t esc_state;
  uint8_t esc_param;
  uint8_t line_idx;
  uint8_t added_tail;
  char buf[MP_LINE_LEN];
  uint8_t len;
  uint8_t cur;
  uint8_t preferred_col;
} mp_edit_t;
static mp_edit_t g_edit_state;

static void mp_prompt(void){
  if (g_edit) return;
  mp_puts("> ");
}

static void help(void){
  mp_puts("MiniPascal monitor\r\n");
  mp_puts("\r\n");
  mp_puts("=== COMMANDS ===\r\n");
  mp_puts("  EDIT         edit new program\r\n");
  mp_puts("  EDIT 1       edit program from slot 1 (1-3)\r\n");
  mp_puts("  NEW          clear program\r\n");
  mp_puts("  LIST         show program\r\n");
  mp_puts("  RUN          compile and run\r\n");
  mp_puts("  STOP         stop running\r\n");
  mp_puts("  EXIT         exit Pascal mode\r\n");
  mp_puts("\r\n");
  mp_puts("=== EDIT MODE ===\r\n");
  mp_puts("  Arrow keys move, DEL/BKSP delete, ENTER splits line.\r\n");
  mp_puts("  Ctrl+Q exits edit mode (or type QUIT on its own line).\r\n");
  mp_puts("\r\n");
  mp_puts("=== FLASH STORAGE ===\r\n");
  mp_puts("  SAVE 1       save to slot 1 (1-3)\r\n");
  mp_puts("  LOAD 1       load from slot\r\n");
  mp_puts("\r\n");
  mp_puts("=== PASCAL FUNCTIONS ===\r\n");
  mp_puts("  LED(idx,r,g,b,w)    set LED color (idx 1-12)\r\n");
  mp_puts("  LEDON(r,g,b,w)      set all LEDs on\r\n");
  mp_puts("  LEDOFF()            turn all LEDs off\r\n");
  mp_puts("  DELAY(ms)           delay milliseconds (battery: >=20ms uses low power mode)\r\n");
  mp_puts("  BEEP(freq,vol,ms)   beep tone (vol 0-50)\r\n");
  mp_puts("  GOTO n              jump to line n\r\n");
  mp_puts("  TIME()              read RTC into TIMEY/TIMEMO/TIMED/TIMEH/TIMEM/TIMES\r\n");
  mp_puts("  TIME(sel)           return part: 0=YY 1=MO 2=DD 3=HH 4=MM 5=SS (also: TIME(yy|mo|dd|hh|mm|ss))\r\n");
  mp_puts("  SETTIME(yy,mo,dd,hh,mm) set RTC date+time (sec=0) yy=0..99 mo=1..12 dd=1..31 hh=0..23 mm=0..59\r\n");
  mp_puts("  SETTIME(hh,mm,ss)   set RTC time only (keeps date) hh=0..23 mm=0..59 ss=0..59\r\n");
  mp_puts("  WRITELN(...)        print text/numbers + newline (only when USB connected)\r\n");
  mp_puts("  SETALARM(hh,mm[,dur]) set daily alarm at HH:MM (dur seconds, dur=0 disables, default dur=30)\r\n");
  mp_puts("  ALARM()             alarm active flag (1 while alarm is running, else 0)\r\n");
  mp_puts("\r\n");
  mp_puts("=== READ FUNCTIONS (return value) ===\r\n");
  mp_puts("  BATTERY()    battery mV\r\n");
  mp_puts("  LIGHT()      light lux\r\n");
  mp_puts("  RNG()        random number\r\n");
  mp_puts("  TEMP()       temperature (x100)\r\n");
  mp_puts("  HUM()        humidity (x100)\r\n");
  mp_puts("  PRESS()      pressure (x100)\r\n");
  mp_puts("  BTN()        next short-press event (0=none, 1=B1, 2=B2, 3=BL)\r\n");
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
  mp_puts("  x := time(MM)  minutes\r\n");
  mp_puts("  WRITELN('x=', x)\r\n");
  mp_puts("\r\n");
  mp_puts("Tip: hold BL to enter stop, wake with B1\r\n");
}

static void edit_load_from_ed(uint8_t idx)
{
  if (idx >= g_ed.count)
  {
    g_edit_state.len = 0;
    g_edit_state.cur = 0;
    g_edit_state.buf[0] = 0;
    return;
  }

  strncpy(g_edit_state.buf, g_ed.lines[idx].text, MP_LINE_LEN);
  g_edit_state.buf[MP_LINE_LEN - 1] = 0;
  g_edit_state.len = (uint8_t)strnlen(g_edit_state.buf, MP_LINE_LEN - 1);
  if (g_edit_state.cur > g_edit_state.len) g_edit_state.cur = g_edit_state.len;
}

static void edit_store_to_ed(uint8_t idx)
{
  if (idx >= g_ed.count) return;
  strncpy(g_ed.lines[idx].text, g_edit_state.buf, MP_LINE_LEN);
  g_ed.lines[idx].text[MP_LINE_LEN - 1] = 0;
}

static void edit_delete_line_at(uint8_t idx)
{
  if (idx >= g_ed.count) return;
  for (uint8_t i = idx; i + 1u < g_ed.count; i++)
    g_ed.lines[i] = g_ed.lines[i + 1u];
  g_ed.count--;
}

static int map_line_no(int old_no, const int *old, const int *new_no, uint8_t count)
{
  for (uint8_t i = 0; i < count; i++)
    if (old[i] == old_no) return new_no[i];
  return old_no;
}

static void renumber_update_goto_line(char *dst, size_t dstsz, const char *src,
                                      const int *old, const int *new_no, uint8_t count)
{
  if (!dst || dstsz == 0) return;
  dst[0] = 0;
  if (!src) return;

  char quote = 0;
  size_t di = 0;
  for (size_t si = 0; src[si] && di + 1 < dstsz; )
  {
    char c = src[si];
    if (quote)
    {
      dst[di++] = c;
      si++;
      if (c == quote) quote = 0;
      continue;
    }

    if (c == '\'' || c == '"')
    {
      quote = c;
      dst[di++] = c;
      si++;
      continue;
    }

    /* Replace "goto <number>" targets after renumbering. */
    if ((si == 0 || !is_idn(src[si - 1])) &&
        src[si + 0] && src[si + 1] && src[si + 2] && src[si + 3] &&
        (tolower((unsigned char)src[si + 0]) == 'g') &&
        (tolower((unsigned char)src[si + 1]) == 'o') &&
        (tolower((unsigned char)src[si + 2]) == 't') &&
        (tolower((unsigned char)src[si + 3]) == 'o') &&
        (!is_idn(src[si + 4])))
    {
      dst[di++] = src[si++]; /* g */
      if (di + 1 < dstsz) dst[di++] = src[si++]; /* o */
      if (di + 1 < dstsz) dst[di++] = src[si++]; /* t */
      if (di + 1 < dstsz) dst[di++] = src[si++]; /* o */

      while ((src[si] == ' ' || src[si] == '\t') && di + 1 < dstsz)
        dst[di++] = src[si++];

      int v = 0;
      size_t si0 = si;
      while (src[si] >= '0' && src[si] <= '9')
      {
        v = v * 10 + (src[si] - '0');
        si++;
      }

      if (si != si0)
      {
        int mapped = map_line_no(v, old, new_no, count);
        char nb[16];
        mp_itoa(mapped, nb);
        for (size_t k = 0; nb[k] && di + 1 < dstsz; k++)
          dst[di++] = nb[k];
        continue;
      }
    }

    dst[di++] = c;
    si++;
  }

  dst[di] = 0;
}

static void edit_renumber_program(void)
{
  if (g_ed.count == 0) return;

  int old_no[MP_MAX_LINES];
  int new_no[MP_MAX_LINES];
  uint8_t count = g_ed.count;
  for (uint8_t i = 0; i < count; i++)
  {
    old_no[i] = g_ed.lines[i].line_no;
    new_no[i] = 10 + ((int)i * g_step);
  }

  for (uint8_t i = 0; i < count; i++)
  {
    char tmp[MP_LINE_LEN];
    renumber_update_goto_line(tmp, sizeof(tmp), g_ed.lines[i].text, old_no, new_no, count);
    g_ed.lines[i].line_no = new_no[i];
    strncpy(g_ed.lines[i].text, tmp, MP_LINE_LEN);
    g_ed.lines[i].text[MP_LINE_LEN - 1] = 0;
  }
}

static int edit_pick_new_line_no(uint8_t after_idx)
{
  int cur_no = g_ed.lines[after_idx].line_no;
  int next_no = (after_idx + 1u < g_ed.count) ? g_ed.lines[after_idx + 1u].line_no : (cur_no + g_step);
  if ((next_no - cur_no) >= 2) return cur_no + ((next_no - cur_no) / 2);

  edit_renumber_program();
  cur_no = g_ed.lines[after_idx].line_no;
  next_no = (after_idx + 1u < g_ed.count) ? g_ed.lines[after_idx + 1u].line_no : (cur_no + g_step);
  if ((next_no - cur_no) >= 2) return cur_no + ((next_no - cur_no) / 2);
  return cur_no + 1;
}

static void edit_insert_line_after(uint8_t after_idx, const char *text)
{
  if (g_ed.count >= MP_MAX_LINES) return;

  uint8_t pos = (uint8_t)(after_idx + 1u);
  for (int i = (int)g_ed.count; i > (int)pos; i--)
    g_ed.lines[i] = g_ed.lines[i - 1];

  g_ed.lines[pos].line_no = edit_pick_new_line_no(after_idx);
  strncpy(g_ed.lines[pos].text, text ? text : "", MP_LINE_LEN);
  g_ed.lines[pos].text[MP_LINE_LEN - 1] = 0;
  g_ed.count++;
}

static void edit_render(void)
{
  mp_puts("\x1b[2J\x1b[H"); /* clear + home */
  mp_puts("MINIPASCAL EDIT  (Ctrl+Q exits, QUIT on empty line also exits)\r\n\r\n");

  for (uint8_t i = 0; i < g_ed.count; i++)
  {
    char ln[16];
    mp_itoa(g_ed.lines[i].line_no, ln);
    if (i == g_edit_state.line_idx) mp_puts("> "); else mp_puts("  ");
    mp_puts(ln); mp_puts(" ");
    if (i == g_edit_state.line_idx) mp_puts(g_edit_state.buf);
    else mp_puts(g_ed.lines[i].text);
    mp_putcrlf();
  }

  /* Place cursor on the active line at the correct column. */
  uint8_t row = (uint8_t)(3u + g_edit_state.line_idx);
  char ln[16];
  mp_itoa(g_ed.lines[g_edit_state.line_idx].line_no, ln);
  uint8_t col = (uint8_t)(3u + (uint8_t)strlen(ln) + 1u + g_edit_state.cur);
  char esc[32];
  snprintf(esc, sizeof(esc), "\x1b[%u;%uH", (unsigned)row, (unsigned)col);
  mp_puts(esc);
}

static void edit_exit(void)
{
  if (g_edit_state.active)
    edit_store_to_ed(g_edit_state.line_idx);

  if (g_edit_state.added_tail && g_ed.count > 0)
  {
    uint8_t last = (uint8_t)(g_ed.count - 1u);
    if (g_ed.lines[last].text[0] == 0)
      edit_delete_line_at(last);
  }

  g_edit = false;
  g_edit_state = (mp_edit_t){0};
  mp_puts("\r\nEDIT OFF\r\n");
  mp_prompt();
}

static void edit_enter_new(void)
{
  ed_init(&g_ed);
  g_ed.lines[0].line_no = 10;
  g_ed.lines[0].text[0] = 0;
  g_ed.count = 1;

  g_edit = true;
  g_edit_slot = 0;
  g_edit_state = (mp_edit_t){0};
  g_edit_state.active = 1u;
  g_edit_state.added_tail = 1u;
  g_edit_state.line_idx = 0u;
  edit_load_from_ed(0u);
  edit_render();
}

static bool edit_enter_slot(uint8_t slot)
{
  g_edit_state = (mp_edit_t){0};

  bool ar = false;
  if (!storage_load_slot(slot, &g_ed, &ar))
  {
    mp_puts("LOAD FAIL\r\n");
    return false;
  }

  if (g_ed.count == 0)
  {
    g_ed.lines[0].line_no = 10;
    g_ed.lines[0].text[0] = 0;
    g_ed.count = 1;
  }

  /* Add a trailing empty line for easy appending. */
  if (g_ed.count < MP_MAX_LINES)
  {
    uint8_t last = (uint8_t)(g_ed.count - 1u);
    if (g_ed.lines[last].text[0] != 0)
    {
      g_ed.lines[g_ed.count].line_no = g_ed.lines[last].line_no + g_step;
      g_ed.lines[g_ed.count].text[0] = 0;
      g_ed.count++;
      g_edit_state.added_tail = 1u;
    }
  }

  g_edit = true;
  g_edit_slot = slot;
  g_edit_state.active = 1u;
  g_edit_state.line_idx = 0u;
  g_edit_state.cur = 0u;
  edit_load_from_ed(0u);
  edit_render();
  return true;
}

static void edit_move_line(int dir)
{
  edit_store_to_ed(g_edit_state.line_idx);
  if (dir < 0)
  {
    if (g_edit_state.line_idx > 0u) g_edit_state.line_idx--;
  }
  else
  {
    if (g_edit_state.line_idx + 1u < g_ed.count) g_edit_state.line_idx++;
  }

  g_edit_state.cur = g_edit_state.preferred_col;
  edit_load_from_ed(g_edit_state.line_idx);
}

static void edit_backspace(void)
{
  if (g_edit_state.cur > 0u)
  {
    memmove(&g_edit_state.buf[g_edit_state.cur - 1u],
            &g_edit_state.buf[g_edit_state.cur],
            (size_t)(g_edit_state.len - g_edit_state.cur + 1u));
    g_edit_state.cur--;
    g_edit_state.len--;
    return;
  }

  if (g_edit_state.line_idx == 0u) return;

  /* Merge with previous line. */
  char merged[MP_LINE_LEN];
  const char *prev = g_ed.lines[g_edit_state.line_idx - 1u].text;
  size_t prev_len = strnlen(prev, MP_LINE_LEN - 1);
  if (prev_len + g_edit_state.len >= (MP_LINE_LEN - 1u)) return;
  memcpy(merged, prev, prev_len);
  memcpy(&merged[prev_len], g_edit_state.buf, g_edit_state.len);
  merged[prev_len + g_edit_state.len] = 0;

  strncpy(g_ed.lines[g_edit_state.line_idx - 1u].text, merged, MP_LINE_LEN);
  g_ed.lines[g_edit_state.line_idx - 1u].text[MP_LINE_LEN - 1] = 0;
  edit_delete_line_at(g_edit_state.line_idx);
  g_edit_state.line_idx--;
  strncpy(g_edit_state.buf, merged, MP_LINE_LEN);
  g_edit_state.buf[MP_LINE_LEN - 1] = 0;
  g_edit_state.len = (uint8_t)strnlen(g_edit_state.buf, MP_LINE_LEN - 1);
  g_edit_state.cur = (uint8_t)prev_len;
}

static void edit_delete(void)
{
  if (g_edit_state.cur < g_edit_state.len)
  {
    memmove(&g_edit_state.buf[g_edit_state.cur],
            &g_edit_state.buf[g_edit_state.cur + 1u],
            (size_t)(g_edit_state.len - g_edit_state.cur));
    g_edit_state.len--;
    return;
  }

  if (g_edit_state.line_idx + 1u >= g_ed.count) return;

  const char *next = g_ed.lines[g_edit_state.line_idx + 1u].text;
  size_t next_len = strnlen(next, MP_LINE_LEN - 1);
  if ((size_t)g_edit_state.len + next_len >= (MP_LINE_LEN - 1u)) return;
  memcpy(&g_edit_state.buf[g_edit_state.len], next, next_len);
  g_edit_state.len = (uint8_t)(g_edit_state.len + next_len);
  g_edit_state.buf[g_edit_state.len] = 0;
  edit_delete_line_at((uint8_t)(g_edit_state.line_idx + 1u));
}

static void edit_insert_char(char c)
{
  if (g_edit_state.len >= (MP_LINE_LEN - 1u)) return;
  memmove(&g_edit_state.buf[g_edit_state.cur + 1u],
          &g_edit_state.buf[g_edit_state.cur],
          (size_t)(g_edit_state.len - g_edit_state.cur + 1u));
  g_edit_state.buf[g_edit_state.cur] = c;
  g_edit_state.cur++;
  g_edit_state.len++;
}

static void edit_enter_key(void)
{
  /* "QUIT" on its own line exits edit mode. */
  {
    char tmp[MP_LINE_LEN];
    strncpy(tmp, g_edit_state.buf, MP_LINE_LEN);
    tmp[MP_LINE_LEN - 1] = 0;
    char *p = tmp;
    while (*p == ' ' || *p == '\t') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) { e--; }
    *e = 0;
    if (mp_stricmp(p, "QUIT") == 0)
    {
      edit_exit();
      return;
    }
  }

  if (g_ed.count == 0) return;

  if (g_ed.count >= MP_MAX_LINES) return;

  if (g_edit_state.line_idx >= g_ed.count) return;

  if (g_edit_state.cur > g_edit_state.len) g_edit_state.cur = g_edit_state.len;

  if (g_edit_state.len >= (MP_LINE_LEN - 1u)) g_edit_state.cur = g_edit_state.len;

  /* Split line at cursor and insert the tail as a new line below. */
  char tail[MP_LINE_LEN];
  uint8_t tail_len = (uint8_t)(g_edit_state.len - g_edit_state.cur);
  memcpy(tail, &g_edit_state.buf[g_edit_state.cur], tail_len);
  tail[tail_len] = 0;

  g_edit_state.buf[g_edit_state.cur] = 0;
  g_edit_state.len = g_edit_state.cur;
  edit_store_to_ed(g_edit_state.line_idx);
 
  edit_insert_line_after(g_edit_state.line_idx, tail);
  edit_renumber_program();
  g_edit_state.line_idx++;
  g_edit_state.cur = 0u;
  g_edit_state.preferred_col = 0u;
  edit_load_from_ed(g_edit_state.line_idx);
}

static void edit_handle_escape(char c)
{
  if (g_edit_state.esc_state == 1u)
  {
    if (c == '[') { g_edit_state.esc_state = 2u; g_edit_state.esc_param = 0u; return; }
    g_edit_state.esc_state = 0u;
    return;
  }

  if (g_edit_state.esc_state == 2u)
  {
    if (c >= '0' && c <= '9') { g_edit_state.esc_param = (uint8_t)(c - '0'); return; }
    if (c == '~')
    {
      if (g_edit_state.esc_param == 3u) edit_delete();
      g_edit_state.esc_state = 0u;
      return;
    }

    switch (c)
    {
      case 'A': edit_move_line(-1); break;
      case 'B': edit_move_line(+1); break;
      case 'C': if (g_edit_state.cur < g_edit_state.len) g_edit_state.cur++; break;
      case 'D': if (g_edit_state.cur > 0u) g_edit_state.cur--; break;
      case 'H': g_edit_state.cur = 0u; break;
      case 'F': g_edit_state.cur = g_edit_state.len; break;
      default: break;
    }

    g_edit_state.preferred_col = g_edit_state.cur;
    g_edit_state.esc_state = 0u;
    return;
  }
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
  if (g_edit) return;

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
    /* SAVE <slot>: compile + save program into the slot */
    uint8_t s = g_slot;

    if (*args){
      /* First optional number = slot */
      (void)parse_slot_opt(args, &s);
    }

    /* compile first; if compile fails, do not touch flash */
    compile_or_report();
    if (!g_have_prog) return;

    if (storage_save_slot(s, &g_ed, false)){
      g_slot = s;
      refresh_program_slot_cache();
      mp_puts("SAVED\r\n");
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
    }
    return;
  }
  if (!mp_stricmp(cmd,"LOAD")) {
    uint8_t s=g_slot;
    if (*args){
      (void)parse_slot_opt(args,&s);
    }
    bool ar=false;
    if (storage_load_slot(s, &g_ed, &ar)){
      g_slot=s;
      refresh_program_slot_cache();
      mp_puts("LOADED\r\n");
      compile_or_report();
      if (g_have_prog){
        vm_reset(&g_vm);
        mp_puts("RUN\r\n");
      }
    } else {
      mp_puts("LOAD FAIL\r\n");
    }
    return;
  }
  if (!mp_stricmp(cmd,"EDIT")) {
    uint8_t s = 0;
    if (*args && parse_slot_opt(args, &s))
    {
      (void)edit_enter_slot(s);
      return;
    }
    edit_enter_new();
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

static void mp_indicate_program_start(void){
  if (mp_hal_usb_connected()) return;
  IND_LED_On();
  LP_DELAY(200);
  IND_LED_Off();
}

void mp_init(void){
  ed_init(&g_ed);
  g_have_prog=false;
  g_edit=false;
  g_step=10;
  g_slot=1;

  bool loaded=false;
  bool ar=false;
  refresh_program_slot_cache();
  uint8_t slot = g_first_program_slot;
  if (slot != 0 && storage_load_slot(slot, &g_ed, &ar)){
    g_slot = slot;
    loaded = true;
  }

  if (loaded && (mp_hal_usb_connected() == 0)){
    compile_or_report();
    if (g_have_prog) { vm_reset(&g_vm); mp_indicate_program_start(); }
  }
}

void mp_request_stop(void){ g_vm.stop_req=true; }
void mp_request_run_slot(uint8_t slot){
  if (slot < 1 || slot > MP_FLASH_SLOT_COUNT) return;
  g_run_slot_req = slot;
}
void mp_request_run_loaded(void){ g_run_loaded_req = 1; }
void mp_request_usb_detach(void){ g_usb_detach_req = 1; }

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

static void edit_feed_char(char c)
{
  if (!g_edit_state.active) return;

  /* Ctrl+Q exits edit mode. */
  if ((uint8_t)c == 0x11u)
  {
    edit_exit();
    return;
  }

  if ((uint8_t)c == 0x1Bu)
  {
    g_edit_state.esc_state = 1u;
    return;
  }

  if (g_edit_state.esc_state != 0u)
  {
    edit_handle_escape(c);
    edit_render();
    return;
  }

  if (c == '\r' || c == '\n')
  {
    edit_enter_key();
    if (g_edit) edit_render();
    return;
  }

  if ((uint8_t)c == 0x08u || (uint8_t)c == 0x7Fu)
  {
    edit_backspace();
    edit_render();
    return;
  }

  if ((uint8_t)c < 0x20u) return;

  edit_insert_char(c);
  g_edit_state.preferred_col = g_edit_state.cur;
  edit_render();
}

void mp_feed_char(char c){
  if (g_edit)
  {
    edit_feed_char(c);
    return;
  }

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

void mp_autorun_poll(void){
  static uint8_t autorun_done = 0;

  if (mp_hal_usb_connected()){
    autorun_done = 0;
    return;
  }

  if (autorun_done) return;
  if (g_vm.running && g_have_prog){
    autorun_done = 1;
    return;
  }

  bool ar = false;
  uint8_t slot = slot_find_first_program();
  if (slot != 0 && storage_load_slot(slot, &g_ed, &ar)){
    g_slot = slot;
    compile_or_report();
    if (g_have_prog) { vm_reset(&g_vm); mp_indicate_program_start(); }
  }
  autorun_done = 1;
}

void mp_poll(void){
  uint32_t now = mp_hal_millis();

  if (g_usb_detach_req){
    g_usb_detach_req = 0;
    g_session_active = false;
  }

  if (g_run_next_req)
  {
    g_run_next_req = 0;
    if ((mp_hal_usb_connected() == 0) && !g_session_active)
    {
      uint8_t next = 0;
      if (g_first_program_slot != 0)
      {
        next = slot_find_next_program(g_slot, +1);
        if (next == 0) next = g_first_program_slot;
      }

      if (next != 0 && next != g_slot)
      {
        bool ar = false;
        if (storage_load_slot(next, &g_ed, &ar))
        {
          g_slot = next;
          refresh_program_slot_cache();
          compile_or_report();
          if (g_have_prog) { vm_reset(&g_vm); mp_indicate_program_start(); }
        }
      }
    }
  }

  if (g_run_loaded_req)
  {
    g_run_loaded_req = 0;
    if ((mp_hal_usb_connected() == 0) && !g_session_active)
    {
      if (!(g_vm.running && g_have_prog))
      {
        compile_or_report();
        if (g_have_prog) { vm_reset(&g_vm); mp_indicate_program_start(); }
      }
    }
  }

  uint8_t req = g_run_slot_req;
  if (req != 0){
    g_run_slot_req = 0;
    if ((mp_hal_usb_connected() == 0) && !g_session_active){
      bool ar = false;
      uint8_t slot = req;
      bool loaded = storage_load_slot(slot, &g_ed, &ar);
      if (!loaded){
        uint8_t alt = slot_find_next_program(slot, +1);
        if (alt != slot && storage_load_slot(alt, &g_ed, &ar)){
          slot = alt;
          loaded = true;
        }
      }

      if (loaded){
        g_slot = slot;
        compile_or_report();
        if (g_have_prog) { vm_reset(&g_vm); mp_indicate_program_start(); }
      }
    }
  }

  static uint32_t last_time_ms = 0;
  if ((uint32_t)(now - last_time_ms) >= 1000u){
    last_time_ms = now;
    time_update_vars(g_vm.vars);
  }

  static uint32_t abort_start_ms = 0;
  static uint8_t abort_latched = 0;
  if (g_vm.running && g_have_prog){
    if (mp_hal_abort_pressed()){
      if (abort_start_ms == 0) abort_start_ms = now;
      if (!abort_latched && (uint32_t)(now - abort_start_ms) >= MP_ABORT_HOLD_MS){
        abort_latched = 1;
        g_vm.stop_req = true;
        /* Long-hold B2: stop program, switch off lamp, and on battery go to STOP2. */
        Lamp_RequestOff((mp_hal_usb_connected() != 0) ? 0u : 1u);
        if (mp_hal_usb_connected()){
          mp_puts("\r\nABORT (B2 held)\r\n");
        }
      }
    } else {
      abort_start_ms = 0;
      abort_latched = 0;
    }
  } else {
    abort_start_ms = 0;
    abort_latched = 0;
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

/* Short press events latched for BTN() (0 none, 1=B1, 2=B2, 3=BL). */
static uint8_t g_btn_short_events = 0;
void mp_notify_button_short(uint8_t btn_id)
{
  if (btn_id == 1u)
  {
    /* On battery, B1 short press starts the program when idle (do not enqueue as event). */
    if ((mp_hal_usb_connected() == 0) && (!g_vm.running || !g_have_prog))
    {
      mp_request_run_loaded();
      return;
    }

    g_btn_short_events |= (1u << 0);
    return;
  }

  if (btn_id == 2u)
  {
    g_btn_short_events |= (1u << 1);
    return;
  }

  if (btn_id == 3u)
  {
    g_btn_short_events |= (1u << 2);
    return;
  }
}

void mp_notify_button_long(uint8_t btn_id)
{
  /* Only battery mode uses long-press actions. */
  if (mp_hal_usb_connected() != 0) return;
  if (btn_id == 1u) g_run_next_req = 1u;
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

static int time_sel_id(const char *name){
  if (!name) return -1;
  if (!mp_stricmp(name, "yy")) return 0;
  if (!mp_stricmp(name, "mo")) return 1;
  if (!mp_stricmp(name, "dd")) return 2;
  if (!mp_stricmp(name, "hh")) return 3;
  if (!mp_stricmp(name, "mm") || !mp_stricmp(name, "mi")) return 4;
  if (!mp_stricmp(name, "ss")) return 5;
  return -1;
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

/*
 * Builtin functions (runtime side).
 * Each ID below matches builtin_id() and calls into the corresponding driver/library.
 */
int32_t mp_user_builtin(uint8_t id, uint8_t argc, const int32_t *argv){
  switch(id){
    /* ---------------- LED control (led.c, CTL_LEN power) ---------------- */
    case 1: /* led(...) */
      if (argc==2){ /* led(index, w) simple white */
        mp_hal_led_power_on();
        uint8_t idx = (argv[0]<=0)?0:(uint8_t)(argv[0]-1);
        uint8_t w = (argv[1]<0)?0:(argv[1]>255?255:(uint8_t)argv[1]);
        led_set_RGBW(idx, 0, 0, 0, w);
        led_render();
        return 0;
      }
      if (argc==5){
        mp_hal_led_power_on();
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

    /* ---------------- Timing / sleep ---------------- */
    case 2: /* delay(ms) is executed by the VM (OP_SLEEP). */
      return 0;

    /* ---------------- Analog measurements (analog.c) ---------------- */
    case 3: /* battery() -> mV */
      if (argc==0){
        float v = ANALOG_GetBat();
        if (v < 0.0f) v = 0.0f;
        return (int32_t)(v * 1000.0f + 0.5f);
      }
      return -1;

    /* RNG (main.c hrng). */
    case 4: /* rng() -> 0..255 */
      {
        uint32_t r=0; if (HAL_RNG_GenerateRandomNumber(&hrng,&r)==HAL_OK) return (int32_t)(r & 0xFF); return -1;
      }

    /* ---------------- BME280 readings (bme280.c) ---------------- */
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

    /* ---------------- Buttons ---------------- */
    case 16: /* btn() -> next short-press event (0 none, 1=B1, 2=B2, 3=BL) */
      if (argc==0){
        uint8_t e = g_btn_short_events;
        if (e & (1u<<0)) { g_btn_short_events = (uint8_t)(e & (uint8_t)~(1u<<0)); return 1; }
        if (e & (1u<<1)) { g_btn_short_events = (uint8_t)(e & (uint8_t)~(1u<<1)); return 2; }
        if (e & (1u<<2)) { g_btn_short_events = (uint8_t)(e & (uint8_t)~(1u<<2)); return 3; }
        return 0;
      }
      return -1;

    /* ---------------- Microphone ---------------- */
    case 9: /* mic() -> dbfs*100 (fault=-99900) */
      {
        const int32_t fault = -99900;

        mic_err_t start = MIC_Start();
        if (start == MIC_ERR_NOT_INIT){
          MIC_Init();
          start = MIC_Start();
        }

        if (start != MIC_ERR_OK){
          if (mp_hal_usb_connected()){
            char b[160];
            const char *msg = MIC_LastErrorMsg();
            snprintf(b, sizeof(b), "[mic] start=%s(%ld) msg=%s\r\n",
                     mic_err_name(start), (long)start, msg ? msg : "");
            mp_puts(b);
          }
          return fault;
        }

        float dbfs = 0.0f;
        float rms  = 0.0f;
        mic_err_t st = MIC_GetLast50ms(&dbfs, &rms);

        uint32_t t0 = HAL_GetTick();
        while ((st == MIC_ERR_NO_DATA_YET) && ((HAL_GetTick() - t0) < 250u)){
          MIC_Task();
          HAL_Delay(1);
          st = MIC_GetLast50ms(&dbfs, &rms);
        }

        if (st != MIC_ERR_OK){
          if (mp_hal_usb_connected()){
            char b[220];
            const char *msg = MIC_LastErrorMsg();
            int32_t last_dbfs_x100 = (int32_t)(MIC_LastDbFS() * 100.0f);
            uint32_t last_rms_u1e6 = (uint32_t)(MIC_LastRms() * 1000000.0f);
            snprintf(b, sizeof(b),
                     "[mic] st=%s(%ld) last_dbfs_x100=%ld last_rms_u1e6=%lu msg=%s\r\n",
                     mic_err_name(st), (long)st,
                     (long)last_dbfs_x100, (unsigned long)last_rms_u1e6,
                     msg ? msg : "");
            mp_puts(b);
          }
          return fault;
        }

        return (int32_t)(dbfs * 100.0f);
      }

    /* ---------------- RTC clock + alarm ---------------- */
    case 10: /* time() or time(sel) */
      if (argc==0){
        time_update_vars(g_vm.vars);
        return 0;
      }
      if (argc==1){
        time_update_vars(g_vm.vars);
        int sel = (int)argv[0];
        switch (sel){
          case 0: return g_vm.vars[SV_TIMEY];
          case 1: return g_vm.vars[SV_TIMEMO];
          case 2: return g_vm.vars[SV_TIMED];
          case 3: return g_vm.vars[SV_TIMEH];
          case 4: return g_vm.vars[SV_TIMEM];
          case 5: return g_vm.vars[SV_TIMES];
          default: return -1;
        }
      }
      return -1;
    case 17: /* settime(yy,mo,dd,hh,mm) or settime(hh,mm,ss) */
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
        snprintf(buf,sizeof(buf), "%02ld:%02ld:%02ld_%02d.%02d.%02d",
                 (long)argv[0], (long)argv[1], (long)argv[2], yy, mo, dd);
        if (RTC_SetClock(buf)==HAL_OK){
          time_update_vars(g_vm.vars);
          return 0;
        }
        return -1;
      }
      return -1;
    case 11: /* alarm() -> active (0/1) */
      if (argc==0){
        return (int32_t)RTC_AlarmTrigger;
      }
      return -1;
    case 18: /* setalarm(hh,mm[,duration_sec]) daily */
      if (argc>=2){
        uint8_t hh = (argv[0]<0)?0:(argv[0]>23?23:(uint8_t)argv[0]);
        uint8_t mm = (argv[1]<0)?0:(argv[1]>59?59:(uint8_t)argv[1]);
        uint8_t dur= (argc>=3)? ((argv[2]<0)?0:(argv[2]>255?255:(uint8_t)argv[2])) : 30;
        if (RTC_SetDailyAlarm(hh, mm, dur)==HAL_OK) return 0;
      }
      return -1;

    /* ---------------- Light sensor (analog.c) ---------------- */
    case 12: /* light() -> lux */
      if (argc==0){
        float l = ANALOG_GetLight();
        if (l < 0.0f) l = 0.0f;
        return (int32_t)(l + 0.5f);
      }
      return -1;

    case 13: /* ledon(r,g,b,w) */
      if (argc==0){
        mp_hal_led_power_on();
        led_render();
        return 0;
      }
      if (argc==4){
        mp_hal_led_power_on();
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
        mp_hal_led_power_on();
        led_set_all_RGBW(0, 0, 0, 0);
        led_render();
        mp_hal_led_power_off();
        return 0;
      }
      return -1;

    /* ---------------- Beeper (alarm.c) ---------------- */
    case 15: /* beep(freq,vol,ms) */
      if (argc==3){
        mp_hal_led_power_on();
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

/* mp_hal_* hooks are implemented in main.c (board layer). */
