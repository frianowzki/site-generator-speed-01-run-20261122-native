// engine.c -- native static-site engine for make.fun site-generator-speed-01.
// A byte-exact C port of solve.js (v8, the pure-JS drop-in for makefun-site
// 1.0.0). Usage: engine <cases.json> <answers.json> <nfkd.bin>
//   cases.json : JSON array of {id, input} (input = {site, templates, files})
//   answers.json: JSON array of answer objects (path -> file text), one per case
//   nfkd.bin : the NFKD tables produced by gen_nfkd.py
// The program is deterministic, holds nothing from one case to the next, and
// exits non-zero on any failure so the JS driver can fall back to solve.js.
//
// Strings are code-point arrays (uint32_t), the exact model of JS strings
// for this workload: every operation in solve.js that touches a string does
// so at a position found by scanning (indexOf / split on an ASCII delimiter)
// or on pure-ASCII text, so code-point indexing and UTF-16 code-unit
// indexing agree wherever it matters (the only fixed cuts are ASCII dates
// and the 64-unit slug cut, whose input is pure ASCII by construction).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

static void die(const char *msg) { fprintf(stderr, "engine: %s\n", msg); exit(1); }
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("oom"); return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) die("oom"); return q; }

// ---------------------------------------------------------------- arena
// Bump allocation in 8 MiB chunks; pointers stay valid for the run. Two
// arenas: perm (the parsed input, lives for the whole run) and case (reset
// between cases, so nothing is carried from one case to the next).
#define ARENA_CHUNK (8u << 20)
typedef struct { char **chunks; size_t nchunks, cap, off; } Arena;
static Arena perm_arena, case_arena;
static void arena_init(Arena *a) { a->chunks = NULL; a->nchunks = 0; a->cap = 0; a->off = 0; }
static void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (a->nchunks == 0 || a->off + n > ARENA_CHUNK) {
    if (a->nchunks == a->cap) { a->cap = a->cap ? a->cap * 2 : 4; a->chunks = xrealloc(a->chunks, a->cap * sizeof(char *)); }
    a->chunks[a->nchunks++] = xmalloc(ARENA_CHUNK);
    a->off = 0;
  }
  void *p = a->chunks[a->nchunks - 1] + a->off;
  a->off += n;
  return p;
}
static void arena_reset(Arena *a) { a->nchunks = a->nchunks ? 1 : 0; a->off = 0; }

// ---------------------------------------------------------------- strings
// Str: a view of code points (not NUL-terminated). StrBuf: a growable builder.
typedef struct { const uint32_t *s; size_t n; } Str;
typedef struct { uint32_t *p; size_t n, cap; } StrBuf;
static Str str_new(const uint32_t *s, size_t n) { Str r; r.s = s; r.n = n; return r; }
static Str str_empty(void) { static const uint32_t z; Str r; r.s = &z; r.n = 0; return r; }
static bool str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.s, b.s, a.n * 4) == 0); }
static int str_cmp(Str a, Str b) { // code-point order (== JS code-unit order on this data: see header)
  size_t n = a.n < b.n ? a.n : b.n;
  for (size_t i = 0; i < n; i++) { if (a.s[i] != b.s[i]) return a.s[i] < b.s[i] ? -1 : 1; }
  return a.n == b.n ? 0 : (a.n < b.n ? -1 : 1);
}
static bool str_starts(Str s, Str pre) { return s.n >= pre.n && memcmp(s.s, pre.s, pre.n * 4) == 0; }
static bool str_ends(Str s, Str suf) { return s.n >= suf.n && memcmp(s.s + s.n - suf.n, suf.s, suf.n * 4) == 0; }
static Str str_slice(Str s, size_t a, size_t b) { // [a,b), clamped; b may exceed
  if (a > s.n) a = s.n; if (b > s.n) b = s.n; if (b < a) b = a;
  Str r; r.s = s.s + a; r.n = b - a; return r;
}
static uint64_t str_hash(Str s) { // FNV-1a over code points
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.n; i++) { h ^= s.s[i]; h *= 1099511628211ULL; }
  return h;
}
// Copy an ASCII C string into code points in arena a.
static Str str_c(Arena *a, const char *c) {
  size_t n = strlen(c);
  uint32_t *p = arena_alloc(a, n * 4);
  for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c[i];
  Str r; r.s = p; r.n = n; return r;
}
// Str view of a short ASCII literal, permanently stored (deduped). Used for
// object keys and other constants; the storage is never reused or freed.
// Lookup is a hash table (open addressing), not a linear scan: str_lit is
// called ~100k times per run and the linear scan showed up in the profile.
// The store lives at file scope so str_is_lit can test pointer membership.
static uint32_t lit_store[8192];
static size_t lit_store_off = 0;
static bool str_is_lit(Str s) { return s.s >= lit_store && s.s < lit_store + 8192; }
static Str str_lit(const char *c) {
  static char keys[512][32];
  static Str vals[512];
  static bool used[512];
  static int nkeys = 0;
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)c; *p; p++) { h ^= *p; h *= 1099511628211ULL; }
  size_t j = (size_t)h & 511;
  while (used[j]) {
    if (strcmp(keys[j], c) == 0) return vals[j];
    j = (j + 1) & 511;
  }
  size_t n = strlen(c);
  if (n > 31 || nkeys >= 512 || lit_store_off + n > 8192) die("lit table full");
  strcpy(keys[j], c);
  Str r; r.s = lit_store + lit_store_off; r.n = n;
  for (size_t i = 0; i < n; i++) lit_store[lit_store_off + i] = (unsigned char)c[i];
  lit_store_off += n;
  vals[j] = r;
  used[j] = true;
  nkeys++;
  return r;
}
// Intern a short ASCII Str as a str_lit (same pointer for the same text),
// so object-key compares hit pointer equality. Non-short / non-ASCII keys
// pass through unchanged. Distinct literals stay ~100, well under the 512 cap.
static Str str_lit_of(Str s) {
  if (s.n == 0 || s.n > 31) return s;
  char buf[32];
  for (size_t i = 0; i < s.n; i++) {
    if (s.s[i] >= 128) return s;
    buf[i] = (char)s.s[i];
  }
  buf[s.n] = '\0';
  return str_lit(buf);
}

static void buf_init(StrBuf *b) { b->p = NULL; b->n = 0; b->cap = 0; }
static void buf_reserve(StrBuf *b, size_t extra) {
  if (b->n + extra > b->cap) { size_t nc = b->cap ? b->cap : 64; while (nc < b->n + extra) nc *= 2; b->p = xrealloc(b->p, nc * 4); b->cap = nc; }
}
static void buf_add(StrBuf *b, uint32_t c) { buf_reserve(b, 1); b->p[b->n++] = c; }
static void buf_addn(StrBuf *b, const uint32_t *s, size_t n) { buf_reserve(b, n); memcpy(b->p + b->n, s, n * 4); b->n += n; }
static void buf_adds(StrBuf *b, Str s) { buf_addn(b, s.s, s.n); }
static void buf_addc(StrBuf *b, const char *c) { while (*c) buf_add(b, (unsigned char)*c++); }
static void buf_clear(StrBuf *b) { b->n = 0; }
static Str buf_str(Arena *a, StrBuf *b) { // copy builder contents into arena a
  uint32_t *p = arena_alloc(a, b->n * 4);
  memcpy(p, b->p, b->n * 4);
  Str r; r.s = p; r.n = b->n; return r;
}
// Copy a [start, end) slice of a builder into the arena (for caching a
// render that went straight into the parent buffer).
static Str buf_str_range(Arena *a, StrBuf *b, size_t start, size_t end) {
  size_t n = end - start;
  uint32_t *p = arena_alloc(a, n * 4);
  if (n) memcpy(p, b->p + start, n * 4);
  Str r; r.s = p; r.n = n; return r;
}
static Str buf_view(StrBuf *b) { Str r; r.s = b->p; r.n = b->n; return r; }

// Byte buffer (for UTF-8 output).
typedef struct { char *p; size_t n, cap; } ByteBuf;
static void bbuf_init(ByteBuf *b) { b->p = NULL; b->n = 0; b->cap = 0; }
static void bbuf_reserve(ByteBuf *b, size_t extra) {
  if (b->n + extra > b->cap) { size_t nc = b->cap ? b->cap : 256; while (nc < b->n + extra) nc *= 2; b->p = xrealloc(b->p, nc); b->cap = nc; }
}
static void bbuf_addn(ByteBuf *b, const void *s, size_t n) { bbuf_reserve(b, n); memcpy(b->p + b->n, s, n); b->n += n; }
static void bbuf_addc(ByteBuf *b, char c) { bbuf_reserve(b, 1); b->p[b->n++] = c; }
static void bbuf_adds(ByteBuf *b, const char *s) { bbuf_addn(b, s, strlen(s)); }

// ---------------------------------------------------------------- UTF-8
static size_t utf8_decode_one(const unsigned char *p, size_t n, uint32_t *out) {
  // Returns bytes consumed (1..4). Invalid bytes become U+FFFD (never occurs
  // on the harness's valid UTF-8 input).
  if (n == 0) { *out = 0xFFFD; return 0; }
  unsigned char c = p[0];
  if (c < 0x80) { *out = c; return 1; }
  if (c < 0xC2) { *out = 0xFFFD; return 1; }
  if (c < 0xE0) {
    if (n < 2 || (p[1] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
    *out = ((c & 0x1F) << 6) | (p[1] & 0x3F); return 2;
  }
  if (c < 0xF0) {
    if (n < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
    *out = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3;
  }
  if (c < 0xF5) {
    if (n < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
    *out = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    if (*out > 0x10FFFF) *out = 0xFFFD;
    return 4;
  }
  *out = 0xFFFD; return 1;
}
static Str utf8_decode(Arena *a, const char *bytes, size_t n) {
  uint32_t *p = arena_alloc(a, (n + 1) * 4);
  size_t m = 0, i = 0;
  while (i < n) { uint32_t cp; size_t adv = utf8_decode_one((const unsigned char *)bytes + i, n - i, &cp); if (adv == 0) break; p[m++] = cp; i += adv; }
  Str r; r.s = p; r.n = m; return r;
}
static void utf8_encode_into(ByteBuf *b, uint32_t cp) {
  char tmp[4];
  if (cp < 0x80) { bbuf_addc(b, (char)cp); }
  else if (cp < 0x800) { tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 0x3F); bbuf_addn(b, tmp, 2); }
  else if (cp < 0x10000) { tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 0x3F); tmp[2] = 0x80 | (cp & 0x3F); bbuf_addn(b, tmp, 3); }
  else { tmp[0] = 0xF0 | (cp >> 18); tmp[1] = 0x80 | ((cp >> 12) & 0x3F); tmp[2] = 0x80 | ((cp >> 6) & 0x3F); tmp[3] = 0x80 | (cp & 0x3F); bbuf_addn(b, tmp, 4); }
}

// ---------------------------------------------------------------- hash maps
// SMap: Str key -> void * (open addressing, keys point into an arena).
typedef struct { uint64_t hash; Str key; void *val; bool used; } SMapE;
typedef struct { SMapE *e; size_t cap, n; } SMap;
static void smap_init(SMap *m, size_t cap) {
  size_t c = 16; while (c < cap) c *= 2;
  m->e = xmalloc(c * sizeof(SMapE)); memset(m->e, 0, c * sizeof(SMapE)); m->cap = c; m->n = 0;
}
static void smap_grow(SMap *m) {
  SMapE *old = m->e; size_t oc = m->cap;
  size_t on = m->n; // smap_init resets n to 0; restore it after rehashing
  smap_init(m, m->cap * 2);
  for (size_t i = 0; i < oc; i++) if (old[i].used) {
    size_t j = old[i].hash & (m->cap - 1);
    while (m->e[j].used) j = (j + 1) & (m->cap - 1);
    m->e[j] = old[i];
  }
  m->n = on;
  free(old);
}
static void **smap_slot(SMap *m, Str key, bool create) {
  if (create && (m->n + 1) * 10 > m->cap * 7) smap_grow(m);
  uint64_t h = str_hash(key);
  size_t j = h & (m->cap - 1);
  while (m->e[j].used) {
    if (m->e[j].hash == h && str_eq(m->e[j].key, key)) return &m->e[j].val;
    j = (j + 1) & (m->cap - 1);
  }
  if (!create) return NULL;
  m->e[j].used = true; m->e[j].hash = h; m->e[j].key = key; m->e[j].val = NULL; m->n++;
  return &m->e[j].val;
}
static void *smap_get(SMap *m, Str key) { void **s = smap_slot(m, key, false); return s ? *s : NULL; }
static void smap_put(SMap *m, Str key, void *val) { *smap_slot(m, key, true) = val; }
// PMap: pointer key -> int (for include-cache object ids).
typedef struct { void *key; int val; bool used; } PMapE;
typedef struct { PMapE *e; size_t cap, n; } PMap;
static void pmap_init(PMap *m, size_t cap) {
  size_t c = 16; while (c < cap) c *= 2;
  m->e = xmalloc(c * sizeof(PMapE)); memset(m->e, 0, c * sizeof(PMapE)); m->cap = c; m->n = 0;
}
static int pmap_get(PMap *m, void *key, int *out) {
  size_t j = ((uintptr_t)key >> 4) & (m->cap - 1);
  while (m->e[j].used) { if (m->e[j].key == key) { *out = m->e[j].val; return 1; } j = (j + 1) & (m->cap - 1); }
  return 0;
}
static void pmap_put(PMap *m, void *key, int val) {
  if ((m->n + 1) * 10 > m->cap * 7) { // grow (rehash)
    PMapE *old = m->e; size_t oc = m->cap; pmap_init(m, m->cap * 2);
    for (size_t i = 0; i < oc; i++) if (old[i].used) pmap_put(m, old[i].key, old[i].val);
    free(old);
  }
  size_t j = ((uintptr_t)key >> 4) & (m->cap - 1);
  while (m->e[j].used) j = (j + 1) & (m->cap - 1);
  m->e[j].used = true; m->e[j].key = key; m->e[j].val = val; m->n++;
}

// ---------------------------------------------------------------- JSON values
// One value type for the parsed input and for every template value (cards,
// pages, tags, site, pager, loop): objects are key/value lists in insertion
// order, exactly like JS objects for the lookups this engine does.
typedef enum { VNULL, VBOOL, VNUM, VSTR, VARR, VOBJ, VUNDEF } VType;
typedef struct Val Val;
typedef struct { Val **items; size_t n, cap; } VArr;
typedef struct { Str *keys; Val **vals; size_t n, cap; } VObj;
struct Val {
  VType t;
  bool b;
  double num;
  Str s;
  VArr arr;
  VObj obj;
};
static Val *val_new(Arena *a, VType t) { Val *v = arena_alloc(a, sizeof(Val)); memset(v, 0, sizeof(Val)); v->t = t; return v; }
static Val *val_str(Arena *a, Str s) { Val *v = val_new(a, VSTR); v->s = s; return v; }
static Val *val_num(Arena *a, double n) { Val *v = val_new(a, VNUM); v->num = n; return v; }
static Val *val_bool(Arena *a, bool b) { Val *v = val_new(a, VBOOL); v->b = b; return v; }
static Val *val_null(Arena *a) { return val_new(a, VNULL); }
static Val *val_undef(Arena *a) { return val_new(a, VUNDEF); }
static Val *val_arr(Arena *a) { return val_new(a, VARR); }
static Val *val_obj(Arena *a) { return val_new(a, VOBJ); }
static void val_obj_set(Arena *a, Val *obj, Str key, Val *val) {
  if (!str_is_lit(key)) key = str_lit_of(key); // intern once; already-lit keys skip the hash
  for (size_t i = 0; i < obj->obj.n; i++) if (obj->obj.keys[i].s == key.s || str_eq(obj->obj.keys[i], key)) { obj->obj.vals[i] = val; return; }
  if (obj->obj.n == obj->obj.cap) {
    size_t nc = obj->obj.cap ? obj->obj.cap * 2 : 8;
    Str *nk = arena_alloc(a, nc * sizeof(Str));
    Val **nv = arena_alloc(a, nc * sizeof(Val *));
    if (obj->obj.n) { memcpy(nk, obj->obj.keys, obj->obj.n * sizeof(Str)); memcpy(nv, obj->obj.vals, obj->obj.n * sizeof(Val *)); }
    obj->obj.keys = nk; obj->obj.vals = nv; obj->obj.cap = nc;
  }
  obj->obj.keys[obj->obj.n] = key;
  obj->obj.vals[obj->obj.n] = val;
  obj->obj.n++;
}
static Val *val_obj_get(Val *obj, Str key) {
  if (!obj || obj->t != VOBJ) return NULL;
  for (size_t i = 0; i < obj->obj.n; i++) if (obj->obj.keys[i].s == key.s || str_eq(obj->obj.keys[i], key)) return obj->obj.vals[i];
  return NULL;
}
static Val *val_obj_get_lit(Val *obj, const char *key) { return val_obj_get(obj, str_lit(key)); }
static bool val_has(Val *obj, Str key) { return val_obj_get(obj, key) != NULL; }

// ---------------------------------------------------------------- JSON parse
typedef struct { const char *p; size_t n, i; Arena *a; } JParser;
static void jws(JParser *j) { while (j->i < j->n && (j->p[j->i] == ' ' || j->p[j->i] == '\t' || j->p[j->i] == '\n' || j->p[j->i] == '\r')) j->i++; }
static Val *jvalue(JParser *j);
static int jhex4(JParser *j) {
  int v = 0;
  for (int k = 0; k < 4; k++) {
    if (j->i >= j->n) die("bad \\u escape");
    char c = j->p[j->i++];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else die("bad \\u escape");
  }
  return v;
}
static inline bool word_has_byte(uint64_t w, unsigned char c) {
  uint64_t m = 0x0101010101010101ULL * c;
  uint64_t x = w ^ m;
  return ((x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL) != 0;
}
static Str jstring(JParser *j) { // assumes p[i] == '"'; returns decoded Str in arena a
  j->i++; // opening quote
  StrBuf b; buf_init(&b);
  while (j->i < j->n) {
    // Fast path: 8 plain ASCII bytes (no '"', no '\\', no high bit) at once.
    if (j->i + 8 <= j->n) {
      uint64_t w;
      memcpy(&w, j->p + j->i, 8);
      if ((w & 0x8080808080808080ULL) == 0 && !word_has_byte(w, '"') && !word_has_byte(w, '\\')) {
        buf_reserve(&b, 8);
        for (int q = 0; q < 8; q++) b.p[b.n++] = (unsigned char)j->p[j->i + q];
        j->i += 8;
        continue;
      }
    }
    unsigned char c = (unsigned char)j->p[j->i];
    if (c == '"') { j->i++; Str r = buf_str(j->a, &b); free(b.p); return r; }
    if (c == '\\') {
      j->i++;
      if (j->i >= j->n) break;
      char e = j->p[j->i++];
      switch (e) {
        case '"': buf_add(&b, '"'); break;
        case '\\': buf_add(&b, '\\'); break;
        case '/': buf_add(&b, '/'); break;
        case 'b': buf_add(&b, '\b'); break;
        case 'f': buf_add(&b, '\f'); break;
        case 'n': buf_add(&b, '\n'); break;
        case 'r': buf_add(&b, '\r'); break;
        case 't': buf_add(&b, '\t'); break;
        case 'u': {
          int cp = jhex4(j);
          if (cp >= 0xD800 && cp <= 0xDBFF && j->i + 1 < j->n && j->p[j->i] == '\\' && j->p[j->i + 1] == 'u') {
            size_t save = j->i;
            j->i += 2;
            int lo = jhex4(j);
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            else j->i = save;
          }
          buf_add(&b, (uint32_t)cp);
          break;
        }
        default: buf_add(&b, (unsigned char)e); break; // lenient; input is valid
      }
      continue;
    }
    uint32_t cp; size_t adv = utf8_decode_one((const unsigned char *)j->p + j->i, j->n - j->i, &cp);
    if (adv == 0) break;
    buf_add(&b, cp);
    j->i += adv;
  }
  die("unterminated string");
  return str_empty();
}
static Val *jvalue(JParser *j) {
  jws(j);
  if (j->i >= j->n) die("unexpected end of JSON");
  char c = j->p[j->i];
  if (c == '{') {
    j->i++;
    Val *o = val_obj(j->a);
    jws(j);
    if (j->i < j->n && j->p[j->i] == '}') { j->i++; return o; }
    while (1) {
      jws(j);
      Str key = jstring(j);
      jws(j);
      if (j->i >= j->n || j->p[j->i] != ':') die("expected ':' in object");
      j->i++;
      Val *v = jvalue(j);
      val_obj_set(j->a, o, key, v);
      jws(j);
      if (j->i < j->n && j->p[j->i] == ',') { j->i++; continue; }
      if (j->i < j->n && j->p[j->i] == '}') { j->i++; return o; }
      die("expected ',' or '}' in object");
    }
  }
  if (c == '[') {
    j->i++;
    Val *arr = val_arr(j->a);
    jws(j);
    if (j->i < j->n && j->p[j->i] == ']') { j->i++; return arr; }
    while (1) {
      Val *v = jvalue(j);
      // Grow the items array in the arena (see val_arr_push note).
      if (arr->arr.n == arr->arr.cap) {
        size_t nc = arr->arr.cap ? arr->arr.cap * 2 : 8;
        Val **ni = arena_alloc(j->a, nc * sizeof(Val *));
        if (arr->arr.n) memcpy(ni, arr->arr.items, arr->arr.n * sizeof(Val *));
        arr->arr.items = ni; arr->arr.cap = nc;
      }
      arr->arr.items[arr->arr.n++] = v;
      jws(j);
      if (j->i < j->n && j->p[j->i] == ',') { j->i++; continue; }
      if (j->i < j->n && j->p[j->i] == ']') { j->i++; return arr; }
      die("expected ',' or ']' in array");
    }
  }
  if (c == '"') { Val *v = val_str(j->a, jstring(j)); return v; }
  if (c == 't') { if (j->n - j->i < 4 || memcmp(j->p + j->i, "true", 4)) die("bad literal"); j->i += 4; return val_bool(j->a, true); }
  if (c == 'f') { if (j->n - j->i < 5 || memcmp(j->p + j->i, "false", 5)) die("bad literal"); j->i += 5; return val_bool(j->a, false); }
  if (c == 'n') { if (j->n - j->i < 4 || memcmp(j->p + j->i, "null", 4)) die("bad literal"); j->i += 4; return val_null(j->a); }
  // number
  size_t st = j->i;
  if (j->i < j->n && j->p[j->i] == '-') j->i++;
  while (j->i < j->n && ((j->p[j->i] >= '0' && j->p[j->i] <= '9') || j->p[j->i] == '.' || j->p[j->i] == 'e' || j->p[j->i] == 'E' || j->p[j->i] == '+' || j->p[j->i] == '-')) j->i++;
  if (j->i == st) die("bad JSON value");
  char tmp[64];
  size_t tl = j->i - st;
  if (tl >= sizeof(tmp)) die("number too long");
  memcpy(tmp, j->p + st, tl); tmp[tl] = 0;
  return val_num(j->a, strtod(tmp, NULL));
}
static Val *json_parse(Arena *a, const char *text, size_t n) {
  JParser j; j.p = text; j.n = n; j.i = 0; j.a = a;
  Val *v = jvalue(&j);
  jws(&j);
  if (j.i != j.n) die("trailing data after JSON");
  return v;
}

// ---------------------------------------------------------------- JSON write
// Escape a string the way JSON.stringify would (any valid escaping parses to
// the same value, and the scorer compares parsed values, so the exact escape
// style is free; lone surrogates are emitted as \uXXXX, as JSON.stringify does).
static void json_escape_into(ByteBuf *b, Str s) {
  bbuf_addc(b, '"');
  // Reserve the whole string up front: the escaped form is ~s.n bytes for
  // typical content, so the per-run capacity check below never grows.
  bbuf_reserve(b, s.n + 2);
  size_t i = 0;
  while (i < s.n) {
    // Run of plain printable ASCII (no escape needed): one capacity check,
    // then a tight byte loop instead of one bbuf_addc call per character.
    size_t j = i;
    while (j < s.n) {
      uint32_t cp = s.s[j];
      if (cp < 0x20 || cp >= 0x7F || cp == '"' || cp == '\\') break;
      j++;
    }
    if (j > i) {
      if (b->n + (j - i) > b->cap) bbuf_reserve(b, j - i);
      for (size_t k = i; k < j; k++) b->p[b->n++] = (char)s.s[k];
      i = j;
    }
    if (i >= s.n) break;
    uint32_t cp = s.s[i];
    if (cp == '"') bbuf_adds(b, "\\\"");
    else if (cp == '\\') bbuf_adds(b, "\\\\");
    else if (cp == '\n') bbuf_adds(b, "\\n");
    else if (cp == '\r') bbuf_adds(b, "\\r");
    else if (cp == '\t') bbuf_adds(b, "\\t");
    else if (cp == '\b') bbuf_adds(b, "\\b");
    else if (cp == '\f') bbuf_adds(b, "\\f");
    else if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF)) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", cp); bbuf_adds(b, t); }
    else utf8_encode_into(b, cp);
    i++;
  }
  bbuf_addc(b, '"');
}
// JS String(number) for the numbers this engine meets (all integers: counts,
// indices, reading minutes). Falls back to %.17g for non-integers.
static void js_num_into(StrBuf *b, double n) {
  if (n == (double)(long long)n && n >= -9007199254740991.0 && n <= 9007199254740991.0) {
    char t[32]; snprintf(t, sizeof(t), "%lld", (long long)n); buf_addc(b, t);
  } else {
    char t[64]; snprintf(t, sizeof(t), "%.17g", n); buf_addc(b, t);
  }
}
// JSON.stringify of a template value, for include-cache keys. Only called on
// non-objects (objects take the identity route): string, number, boolean,
// null, undefined. Mirrors the JS exactly for those types.
static void json_stringify_val(StrBuf *b, Val *v) {
  if (!v || v->t == VUNDEF) { buf_addc(b, "undefined"); return; }
  switch (v->t) {
    case VNULL: buf_addc(b, "null"); break;
    case VBOOL: buf_addc(b, v->b ? "true" : "false"); break;
    case VNUM: js_num_into(b, v->num); break;
    case VSTR: {
      ByteBuf bb; bbuf_init(&bb);
      json_escape_into(&bb, v->s);
      for (size_t i = 0; i < bb.n; i++) buf_add(b, (unsigned char)bb.p[i]);
      free(bb.p);
      break;
    }
    default: buf_addc(b, "undefined"); break; // arrays/objects never reach here
  }
}

// ---------------------------------------------------------------- NFKD
// Tables from nfkd.bin (gen_nfkd.py). Loaded once; the file stays mapped in
// memory and the tables are binary-searched. Hangul syllables are decomposed
// algorithmically (they are excluded from the table).
typedef struct { uint32_t cp, off, len; } NfkdEnt;
static NfkdEnt *nfkd_ents; static size_t nfkd_nents;
static uint32_t *nfkd_data; static size_t nfkd_ndata;
static uint32_t (*mark_ranges)[2]; static size_t nmark_ranges;
static uint32_t (*ccc_tab)[2]; static size_t nccc;
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void nfkd_load(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) die("cannot open nfkd.bin");
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *buf = xmalloc(sz > 0 ? (size_t)sz : 1);
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("cannot read nfkd.bin");
  fclose(f);
  if (sz < 8 || memcmp(buf, "NFKD", 4) != 0 || rd32(buf + 4) != 1) die("bad nfkd.bin");
  const unsigned char *p = buf + 8;
  nfkd_nents = rd32(p); p += 4;
  nfkd_ents = (NfkdEnt *)p; p += nfkd_nents * 12;
  nfkd_ndata = rd32(p); p += 4;
  nfkd_data = (uint32_t *)p; p += nfkd_ndata * 4;
  nmark_ranges = rd32(p); p += 4;
  mark_ranges = (uint32_t (*)[2])p; p += nmark_ranges * 8;
  nccc = rd32(p); p += 4;
  ccc_tab = (uint32_t (*)[2])p; p += nccc * 8;
  (void)sz;
}
static const uint32_t *nfkd_lookup(uint32_t cp, size_t *len) {
  size_t lo = 0, hi = nfkd_nents;
  while (lo < hi) { size_t mid = (lo + hi) / 2; if (nfkd_ents[mid].cp < cp) lo = mid + 1; else hi = mid; }
  if (lo < nfkd_nents && nfkd_ents[lo].cp == cp) { *len = nfkd_ents[lo].len; return nfkd_data + nfkd_ents[lo].off; }
  return NULL;
}
static bool is_mark(uint32_t cp) {
  size_t lo = 0, hi = nmark_ranges;
  while (lo < hi) { size_t mid = (lo + hi) / 2; if (mark_ranges[mid][1] < cp) lo = mid + 1; else hi = mid; }
  return lo < nmark_ranges && mark_ranges[lo][0] <= cp && cp <= mark_ranges[lo][1];
}
static uint32_t ccc_of(uint32_t cp) {
  size_t lo = 0, hi = nccc;
  while (lo < hi) { size_t mid = (lo + hi) / 2; if (ccc_tab[mid][0] < cp) lo = mid + 1; else hi = mid; }
  return (lo < nccc && ccc_tab[lo][0] == cp) ? ccc_tab[lo][1] : 0;
}
// text.normalize('NFKD'): decompose every code point (table + Hangul), then
// canonically reorder each maximal run of combining marks by ccc (stable).
static void nfkd_normalize(Str in, StrBuf *out) {
  buf_clear(out);
  for (size_t i = 0; i < in.n; i++) {
    uint32_t cp = in.s[i];
    if (cp >= 0xAC00 && cp <= 0xD7A3) { // Hangul syllable: L, V, (T)
      uint32_t s = cp - 0xAC00;
      uint32_t l = s / (21 * 28), v = (s % (21 * 28)) / 28, t = s % 28;
      buf_add(out, 0x1100 + l);
      buf_add(out, 0x1161 + v);
      if (t) buf_add(out, 0x11A7 + t);
      continue;
    }
    size_t len = 0;
    const uint32_t *d = nfkd_lookup(cp, &len);
    if (d) { for (size_t k = 0; k < len; k++) buf_add(out, d[k]); }
    else buf_add(out, cp);
  }
  // Canonical ordering: stable sort each maximal ccc>0 run by ccc.
  size_t i = 0;
  while (i < out->n) {
    if (ccc_of(out->p[i]) == 0) { i++; continue; }
    size_t j = i;
    while (j < out->n && ccc_of(out->p[j]) != 0) j++;
    for (size_t a = i + 1; a < j; a++) { // insertion sort (stable, runs are short)
      uint32_t v = out->p[a], cv = ccc_of(v);
      size_t b = a;
      while (b > i && ccc_of(out->p[b - 1]) > cv) { out->p[b] = out->p[b - 1]; b--; }
      out->p[b] = v;
    }
    i = j;
  }
}

// ---------------------------------------------------------------- slug / words / escape
// The slug of text, exactly as lib/text.js: NFKD, combining marks dropped,
// A-Z lowered, each run of non [a-z0-9] one '-', none at either end, max 64
// (a '-' left at the cut dropped), 'untitled' when nothing is left.
static Str slugify_raw(Arena *a, Str text) {
  bool nonascii = false;
  for (size_t i = 0; i < text.n; i++) if (text.s[i] > 127) { nonascii = true; break; }
  Str plain = text;
  StrBuf nb;
  if (nonascii) {
    buf_init(&nb);
    nfkd_normalize(text, &nb);
    StrBuf fb; buf_init(&fb);
    for (size_t i = 0; i < nb.n; i++) if (!is_mark(nb.p[i])) buf_add(&fb, nb.p[i]);
    free(nb.p);
    plain = buf_str(a, &fb);
    free(fb.p);
  }
  StrBuf out; buf_init(&out);
  bool gap = false;
  for (size_t i = 0; i < plain.n; i++) {
    uint32_t ch = plain.s[i];
    int keep = -1;
    if (ch >= 'A' && ch <= 'Z') keep = (int)(ch - 'A' + 'a');
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = (int)ch;
    if (keep < 0) { gap = out.n > 0; continue; }
    if (gap) buf_add(&out, '-');
    gap = false;
    buf_add(&out, (uint32_t)keep);
  }
  if (out.n > 64) out.n = 64;
  while (out.n > 0 && out.p[out.n - 1] == '-') out.n--;
  Str r;
  if (out.n == 0) { r = str_c(a, "untitled"); }
  else r = buf_str(a, &out);
  free(out.p);
  return r;
}
static Str slugify(SMap *cache, Arena *a, Str text) {
  void *hit = smap_get(cache, text);
  if (hit) return *(Str *)hit;
  Str v = slugify_raw(a, text);
  Str *slot = arena_alloc(a, sizeof(Str));
  *slot = v;
  smap_put(cache, text, slot);
  return v;
}
// Forward declarations (definitions live below their first use).
static void num_into(StrBuf *b, long v);
typedef struct { bool error; Str errmsg; Val *data; Str body; } FMResult;
static FMResult parse_front_matter(Arena *a, Str text);
static bool str_is_ascii(Str s, const char *c);
static Str unique_slug(Arena *a, SMap *taken, Str slug) {
  // uniqueSlug: slug, slug-2, slug-3, ... first one not in `taken`.
  if (!smap_get(taken, slug)) { smap_put(taken, slug, (void *)1); return slug; }
  StrBuf b; buf_init(&b);
  for (long n = 2;; n++) {
    buf_clear(&b);
    buf_adds(&b, slug);
    buf_add(&b, '-');
    num_into(&b, n);
    Str cand = buf_str(a, &b);
    if (!smap_get(taken, cand)) { smap_put(taken, cand, (void *)1); free(b.p); return cand; }
  }
}
static bool is_js_ws(uint32_t c) { // exactly JS \s, on code points
  if (c == 32 || c == 9 || c == 10 || c == 11 || c == 12 || c == 13) return true;
  if (c < 128) return false;
  return c == 160 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
static long words_of(Str text) {
  long n = 0;
  bool inWord = false;
  for (size_t i = 0; i < text.n; i++) {
    if (is_js_ws(text.s[i])) inWord = false;
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}
static void esc_append(StrBuf *out, Str s) { // & < > " ' escaped
  size_t i = 0;
  while (i < s.n) {
    // Run of chars needing no escape: one memcpy instead of buf_add per char.
    size_t j = i;
    while (j < s.n) {
      uint32_t c = s.s[j];
      if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') break;
      j++;
    }
    if (j > i) { buf_addn(out, s.s + i, j - i); i = j; }
    if (i >= s.n) break;
    uint32_t c = s.s[i];
    if (c == '&') buf_addc(out, "&amp;");
    else if (c == '<') buf_addc(out, "&lt;");
    else if (c == '>') buf_addc(out, "&gt;");
    else if (c == '"') buf_addc(out, "&quot;");
    else buf_addc(out, "&#39;");
    i++;
  }
}
static Str esc_str(Arena *a, Str s) {
  StrBuf b; buf_init(&b);
  esc_append(&b, s);
  Str r = buf_str(a, &b);
  free(b.p);
  return r;
}
static void esc_char_into(StrBuf *out, uint32_t c) { // single-character escape
  if (c == '&') buf_addc(out, "&amp;");
  else if (c == '<') buf_addc(out, "&lt;");
  else if (c == '>') buf_addc(out, "&gt;");
  else if (c == '"') buf_addc(out, "&quot;");
  else if (c == '\'') buf_addc(out, "&#39;");
  else buf_add(out, c);
}

// ---------------------------------------------------------------- small scans
static long find_char(Str s, uint32_t c, size_t from) {
  for (size_t i = from; i < s.n; i++) if (s.s[i] == c) return (long)i;
  return -1;
}
static long find_str(Str hay, Str needle, size_t from) {
  if (needle.n == 0) return (long)from;
  for (size_t i = from; i + needle.n <= hay.n; i++)
    if (memcmp(hay.s + i, needle.s, needle.n * 4) == 0) return (long)i;
  return -1;
}
static bool is_blank_line(Str line) { // /^[ \t]*$/
  for (size_t i = 0; i < line.n; i++) if (line.s[i] != 32 && line.s[i] != 9) return false;
  return true;
}
static Str trim_end_st(Str s) { // /[ \t]+$/ removed
  while (s.n > 0 && (s.s[s.n - 1] == 32 || s.s[s.n - 1] == 9)) s.n--;
  return s;
}
static Str trim_st(Str s) { // JS String.trim(): the \s set on both ends
  size_t a = 0, b = s.n;
  while (a < b && is_js_ws(s.s[a])) a++;
  while (b > a && is_js_ws(s.s[b - 1])) b--;
  return str_slice(s, a, b);
}
static void num_into(StrBuf *b, long v) { char t[32]; snprintf(t, sizeof(t), "%ld", v); buf_addc(b, t); }
static Str err_fmt(Arena *a, StrBuf *b) { Str r = buf_str(a, b); free(b->p); return r; }

// Split on '\n' (JS semantics: trailing newline leaves a final empty piece).
typedef struct { Str *items; size_t n; } StrList;
static StrList split_lines(Arena *a, Str s) {
  size_t cap = 8, n = 0;
  Str *items = arena_alloc(a, cap * sizeof(Str));
  size_t start = 0;
  for (size_t i = 0; i <= s.n; i++) {
    if (i == s.n || s.s[i] == '\n') {
      if (n == cap) { size_t nc = cap * 2; Str *ni = arena_alloc(a, nc * sizeof(Str)); memcpy(ni, items, n * sizeof(Str)); items = ni; cap = nc; }
      items[n++] = str_slice(s, start, i);
      start = i + 1;
    }
  }
  StrList r; r.items = items; r.n = n; return r;
}

// ---------------------------------------------------------------- Markdown
typedef struct { int level; Str id; Str html; } TocEnt;
typedef struct Source Source;
typedef struct Tag Tag;
struct Source {
  Str path, text, body, slug, out_dir, url, html, excerpt;
  Val *data; // front matter object
  bool post;
  Str layout;
  TocEnt *toc; size_t ntoc;
  Tag **tag_refs; size_t ntag_refs;
  Val *card; // cached card object
  Source *newer, *older;
};
struct Tag {
  Str name, slug, url;
  Source **posts; size_t nposts, cap;
};
typedef struct { Str *v; size_t n, cap; } StrVec; // malloc-backed growable list
static void strvec_push(StrVec *vec, Str s) {
  if (vec->n == vec->cap) { vec->cap = vec->cap ? vec->cap * 2 : 16; vec->v = xrealloc(vec->v, vec->cap * sizeof(Str)); }
  vec->v[vec->n++] = s;
}
typedef struct {
  SMap *by_source;   // Str path -> Source*
  SMap *static_paths; // Str path -> (void*)1
  Str base, dir, src_path;
  StrVec *broken;    // shared: collected "broken-link ..." lines (deduped via set)
  SMap *broken_set;
} LinkEnv;
typedef struct { Str html; TocEnt *toc; size_t ntoc; Str excerpt; } MdResult;

static Str trim_line(Str line) {
  size_t a = 0, b = line.n;
  while (a < b && (line.s[a] == 32 || line.s[a] == 9)) a++;
  while (b > a && (line.s[b - 1] == 32 || line.s[b - 1] == 9)) b--;
  return str_slice(line, a, b);
}
// The fence language (empty = plain fence), or "not a fence".
static bool fence_open_of(Str line, Str *lang) {
  if (line.n < 3 || line.s[0] != 96 || line.s[1] != 96 || line.s[2] != 96) return false;
  size_t i = 3, start = 3;
  while (i < line.n) {
    uint32_t c = line.s[i];
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 95 || c == 43 || c == 45) { i++; continue; }
    if (c == 32 || c == 9) break;
    return false;
  }
  *lang = str_slice(line, start, i);
  while (i < line.n) { if (line.s[i] != 32 && line.s[i] != 9) return false; i++; }
  return true;
}
static bool is_fence_close(Str line) {
  if (line.n < 3 || line.s[0] != 96 || line.s[1] != 96 || line.s[2] != 96) return false;
  for (size_t i = 3; i < line.n; i++) if (line.s[i] != 32 && line.s[i] != 9) return false;
  return true;
}
static bool heading_of(Str line, int *depth, Str *text) {
  size_t n = 0;
  while (n < line.n && line.s[n] == 35) n++;
  if (n == 0 || n > 6 || n >= line.n) return false;
  if (line.s[n] != 32 && line.s[n] != 9) return false;
  size_t s = n;
  while (s < line.n && (line.s[s] == 32 || line.s[s] == 9)) s++;
  size_t e = line.n;
  while (e > s && (line.s[e - 1] == 32 || line.s[e - 1] == 9)) e--;
  if (e == s) return false;
  *depth = (int)n;
  *text = str_slice(line, s, e);
  return true;
}
static bool is_hr(Str line) { // /^(?:---|\*\*\*)[ \t]*$/
  if (line.n < 3) return false;
  uint32_t c0 = line.s[0];
  if (c0 != 45 && c0 != 42) return false;
  if (line.s[1] != c0 || line.s[2] != c0) return false;
  for (size_t i = 3; i < line.n; i++) if (line.s[i] != 32 && line.s[i] != 9) return false;
  return true;
}
static bool ends_paragraph(Str line) {
  if (line.n == 0) return true;
  uint32_t c0 = line.s[0];
  if (c0 == 96) { Str l; return fence_open_of(line, &l); }
  if (c0 == 35) { int d; Str t; return heading_of(line, &d, &t); }
  if (c0 == 45 || c0 == 42) return is_hr(line);
  if (c0 == 32 || c0 == 9) return is_blank_line(line);
  return false;
}
static bool bullet_of(Str line, Str *text) { // /^[-*] (.*)$/
  if (line.n >= 2 && (line.s[0] == 45 || line.s[0] == 42) && line.s[1] == 32) { *text = str_slice(line, 2, line.n); return true; }
  return false;
}
static bool ordered_of(Str line, long *num, Str *text) { // /^(\d{1,9})\. (.*)$/
  size_t i = 0;
  while (i < line.n && line.s[i] >= 48 && line.s[i] <= 57) i++;
  if (i == 0 || i > 9) return false;
  if (i >= line.n || line.s[i] != 46 || i + 1 >= line.n || line.s[i + 1] != 32) return false;
  long v = 0;
  for (size_t k = 0; k < i; k++) v = v * 10 + (long)(line.s[k] - 48);
  *num = v;
  *text = str_slice(line, i + 2, line.n);
  return true;
}
static bool is_continued(Str line) { // /^ {2,}\S/
  size_t i = 0;
  while (i < line.n && line.s[i] == 32) i++;
  if (i < 2 || i >= line.n) return false;
  uint32_t c = line.s[i];
  return c != 32 && c != 9 && c != 11 && c != 12 && c != 13;
}
static bool is_escaped_at(Str s, size_t p, size_t from) {
  // The reference scan consumes `\X` pairs left to right, so p is escaped
  // exactly when the run of backslashes immediately before p (at or after
  // `from`) has odd length.
  size_t k = 0, j = p;
  while (j > from && s.s[j - 1] == '\\') { k++; j--; }
  return k % 2 == 1;
}
static long find_unescaped(Str s, Str what, size_t from) {
  long j = find_str(s, what, from);
  while (j != -1 && is_escaped_at(s, (size_t)j, from)) j = find_str(s, what, (size_t)j + 1);
  return j;
}
static long find_single_star(Str s, size_t from) {
  long j = find_char(s, '*', from);
  while (j != -1) {
    if (is_escaped_at(s, (size_t)j, from)) { j = find_char(s, '*', (size_t)j + 1); continue; }
    if ((size_t)j + 1 < s.n && s.s[j + 1] == '*') { j = find_char(s, '*', (size_t)j + 2); continue; }
    return j;
  }
  return -1;
}
static bool is_escapable(uint32_t c) {
  return c == '\\' || c == '`' || c == '*' || c == '_' || c == '[' || c == ']' || c == '(' || c == ')' || c == '#' || c == '+' || c == '-' || c == '.' || c == '!' || c == '>';
}
static bool str_has_ws(Str s) { for (size_t i = 0; i < s.n; i++) if (is_js_ws(s.s[i])) return true; return false; }

static Str link_resolve(Arena *a, LinkEnv *env, Str target); // forward
static void inline_to(Arena *a, StrBuf *out, Str s, LinkEnv *env) {
  size_t i = 0, len = s.n;
  while (i < len) {
    uint32_t c = s.s[i];
    if (c == '\\' && i + 1 < len && is_escapable(s.s[i + 1])) { esc_char_into(out, s.s[i + 1]); i += 2; continue; }
    if (c == '`') {
      long j = find_char(s, '`', i + 1);
      if (j > (long)i + 1) {
        buf_addc(out, "<code>");
        esc_append(out, str_slice(s, i + 1, (size_t)j));
        buf_addc(out, "</code>");
        i = (size_t)j + 1;
      } else { buf_add(out, '`'); i++; }
      continue;
    }
    if (c == '*') {
      if (i + 1 < len && s.s[i + 1] == '*') {
        Str stars = str_lit("**");
        long j = find_unescaped(s, stars, i + 2);
        if (j > (long)i + 2) {
          buf_addc(out, "<strong>");
          inline_to(a, out, str_slice(s, i + 2, (size_t)j), env);
          buf_addc(out, "</strong>");
          i = (size_t)j + 2;
        } else { buf_addc(out, "**"); i += 2; }
        continue;
      }
      long j = find_single_star(s, i + 1);
      if (j > (long)i + 1) {
        buf_addc(out, "<em>");
        inline_to(a, out, str_slice(s, i + 1, (size_t)j), env);
        buf_addc(out, "</em>");
        i = (size_t)j + 1;
      } else { buf_add(out, '*'); i++; }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, str_lit("]"), i + 1);
      long m = -1;
      if (k != -1 && (size_t)k + 1 < len && s.s[k + 1] == '(') m = find_char(s, ')', (size_t)k + 2);
      Str target = m == -1 ? str_empty() : str_slice(s, (size_t)k + 2, (size_t)m);
      if (target.n != 0 && !str_has_ws(target)) {
        buf_addc(out, "<a href=\"");
        esc_append(out, link_resolve(a, env, target));
        buf_addc(out, "\">");
        inline_to(a, out, str_slice(s, i + 1, (size_t)k), env);
        buf_addc(out, "</a>");
        i = (size_t)m + 1;
        continue;
      }
      buf_add(out, '[');
      i++;
      continue;
    }
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') { esc_char_into(out, c); i++; continue; }
    // Plain run: up to the next character that needs handling.
    size_t j = i;
    while (j < len) {
      uint32_t d = s.s[j];
      if (d == '\\' || d == '`' || d == '*' || d == '[' || d == '&' || d == '<' || d == '>' || d == '"' || d == '\'') break;
      j++;
    }
    buf_addn(out, s.s + i, j - i);
    i = j;
  }
}
static Str inline_str(Arena *a, Str s, LinkEnv *env) {
  StrBuf b; buf_init(&b);
  inline_to(a, &b, s, env);
  Str r = buf_str(a, &b);
  free(b.p);
  return r;
}
static MdResult render_markdown(Arena *a, SMap *slug_cache, Str src, LinkEnv *env) {
  StrList lines = split_lines(a, src);
  size_t n = lines.n;
  SMap ids; smap_init(&ids, 32); // heading ids, unique per render (JS: new Set())
  TocEnt *toc = NULL;
  size_t ntoc = 0, toccap = 0;
  Str excerpt = str_empty();
  bool have_excerpt = false;
  StrBuf blocks; buf_init(&blocks);
  bool first_block = true;
  size_t i = 0;
  while (i < n) {
    Str line = lines.items[i];
    if (is_blank_line(line)) { i++; continue; }
    Str lang;
    if (fence_open_of(line, &lang)) {
      StrBuf parts; buf_init(&parts);
      if (lang.n == 0) buf_addc(&parts, "<pre><code>");
      else { buf_addc(&parts, "<pre><code class=\"language-"); esc_append(&parts, lang); buf_addc(&parts, "\">"); }
      i++;
      while (i < n && !is_fence_close(lines.items[i])) {
        esc_append(&parts, lines.items[i]);
        buf_add(&parts, '\n');
        i++;
      }
      i++;
      buf_addc(&parts, "</code></pre>");
      if (!first_block) buf_add(&blocks, '\n');
      first_block = false;
      buf_adds(&blocks, buf_view(&parts));
      free(parts.p);
      continue;
    }
    int depth = 0; Str htext;
    if (heading_of(line, &depth, &htext)) {
      Str sl = slugify(slug_cache, a, htext);
      Str id = unique_slug(a, &ids, sl);
      Str html = inline_str(a, htext, env);
      StrBuf hb; buf_init(&hb);
      buf_addc(&hb, "<h"); num_into(&hb, depth); buf_addc(&hb, " id=\""); buf_adds(&hb, id); buf_addc(&hb, "\">"); buf_adds(&hb, html); buf_addc(&hb, "</h"); num_into(&hb, depth); buf_addc(&hb, ">");
      if (!first_block) buf_add(&blocks, '\n');
      first_block = false;
      buf_adds(&blocks, buf_view(&hb));
      free(hb.p);
      if (depth == 2 || depth == 3) {
        if (ntoc == toccap) { toccap = toccap ? toccap * 2 : 8; toc = xrealloc(toc, toccap * sizeof(TocEnt)); }
        toc[ntoc].level = depth; toc[ntoc].id = id; toc[ntoc].html = html; ntoc++;
      }
      i++;
      continue;
    }
    if (is_hr(line)) {
      if (!first_block) buf_add(&blocks, '\n');
      first_block = false;
      buf_addc(&blocks, "<hr>");
      i++;
      continue;
    }
    if (line.n > 0 && line.s[0] == 62) {
      StrBuf body; buf_init(&body);
      while (i < n && lines.items[i].n > 0 && lines.items[i].s[0] == 62) {
        Str l = lines.items[i++];
        Str t = trim_line(l.n > 1 && l.s[1] == 32 ? str_slice(l, 2, l.n) : str_slice(l, 1, l.n));
        if (t.n != 0) {
          if (body.n > 0) buf_add(&body, '\n');
          buf_adds(&body, t);
        }
      }
      if (!first_block) buf_add(&blocks, '\n');
      first_block = false;
      if (body.n == 0) buf_addc(&blocks, "<blockquote>\n</blockquote>");
      else {
        buf_addc(&blocks, "<blockquote>\n<p>");
        Str inner = inline_str(a, buf_view(&body), env);
        buf_adds(&blocks, inner);
        buf_addc(&blocks, "</p>\n</blockquote>");
      }
      free(body.p);
      continue;
    }
    Str bullet_text;
    long ord_num = 0; Str ord_text;
    bool is_bullet = bullet_of(line, &bullet_text);
    bool is_ord = !is_bullet && ordered_of(line, &ord_num, &ord_text);
    if (is_bullet || is_ord) {
      long start = 1;
      if (is_ord) start = ord_num;
      // items: each is a list of lines (first + continuations)
      typedef struct { Str *lines; size_t n, cap; } Item;
      Item *items = NULL;
      size_t nitems = 0, itemcap = 0;
      while (i < n) {
        Str cur = lines.items[i];
        Str t; bool has = false;
        if (is_ord) { long nn; if (ordered_of(cur, &nn, &t)) has = true; }
        else if (bullet_of(cur, &t)) has = true;
        if (has) {
          if (nitems == itemcap) { itemcap = itemcap ? itemcap * 2 : 8; items = xrealloc(items, itemcap * sizeof(Item)); }
          items[nitems].lines = xmalloc(4 * sizeof(Str));
          items[nitems].cap = 4;
          items[nitems].n = 1;
          items[nitems].lines[0] = trim_line(t);
          nitems++;
          i++;
        } else if (is_continued(cur)) {
          if (nitems == 0) break;
          Item *it = &items[nitems - 1];
          if (it->n == it->cap) { it->cap *= 2; it->lines = xrealloc(it->lines, it->cap * sizeof(Str)); }
          it->lines[it->n++] = trim_line(cur);
          i++;
        } else break;
      }
      StrBuf parts; buf_init(&parts);
      if (is_ord) {
        if (start != 1) { buf_addc(&parts, "<ol start=\""); num_into(&parts, start); buf_addc(&parts, "\">"); }
        else buf_addc(&parts, "<ol>");
      } else buf_addc(&parts, "<ul>");
      buf_add(&parts, '\n');
      for (size_t k = 0; k < nitems; k++) {
        buf_addc(&parts, "<li>");
        StrBuf joined; buf_init(&joined);
        for (size_t q = 0; q < items[k].n; q++) { if (q) buf_add(&joined, '\n'); buf_adds(&joined, items[k].lines[q]); }
        inline_to(a, &parts, buf_view(&joined), env);
        free(joined.p);
        buf_addc(&parts, "</li>\n");
        free(items[k].lines);
      }
      buf_addc(&parts, is_ord ? "</ol>" : "</ul>");
      if (!first_block) buf_add(&blocks, '\n');
      first_block = false;
      buf_adds(&blocks, buf_view(&parts));
      free(parts.p);
      free(items);
      continue;
    }
    // Paragraph.
    StrBuf body; buf_init(&body);
    buf_adds(&body, trim_line(line));
    i++;
    while (i < n && !ends_paragraph(lines.items[i])) { buf_add(&body, '\n'); buf_adds(&body, trim_line(lines.items[i++])); }
    Str html = inline_str(a, buf_view(&body), env);
    free(body.p);
    if (!have_excerpt) { excerpt = html; have_excerpt = true; }
    if (!first_block) buf_add(&blocks, '\n');
    first_block = false;
    buf_addc(&blocks, "<p>");
    buf_adds(&blocks, html);
    buf_addc(&blocks, "</p>");
  }
  MdResult r;
  if (first_block) { r.html = str_empty(); }
  else { buf_add(&blocks, '\n'); r.html = buf_str(a, &blocks); }
  free(blocks.p);
  free(ids.e);
  r.toc = toc; r.ntoc = ntoc;
  r.excerpt = have_excerpt ? excerpt : str_empty();
  return r;
}

// ---------------------------------------------------------------- Templates
typedef enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE } NodeKind;
typedef struct TNode TNode;
typedef struct { TNode **v; size_t n, cap; } NodeVec; // pointers: a for/if node's
// body is filled through the frame's pointer after the node is pushed, so the
// vector must alias the same node, not a copy.
struct TNode {
  NodeKind kind;
  Str text;                  // N_TEXT
  Str *path; size_t npath;   // N_VAR, N_RAW, N_IF, N_FOR
  bool not;                  // N_IF
  NodeVec then_b, else_b;    // N_IF
  bool has_else;             // N_IF
  Str for_name;              // N_FOR
  NodeVec body;              // N_FOR
  Str inc_name;              // N_INCLUDE
};
static TNode *tnode_new(Arena *a, NodeKind kind) {
  TNode *nd = arena_alloc(a, sizeof(TNode));
  memset(nd, 0, sizeof(TNode));
  nd->kind = kind;
  return nd;
}
static void nodevec_push(Arena *a, NodeVec *vec, TNode *nd) {
  if (vec->n == vec->cap) {
    size_t nc = vec->cap ? vec->cap * 2 : 8;
    TNode **nv = arena_alloc(a, nc * sizeof(TNode *));
    if (vec->n) memcpy(nv, vec->v, vec->n * sizeof(TNode *));
    vec->v = nv; vec->cap = nc;
  }
  vec->v[vec->n++] = nd;
}
typedef struct { bool error; Str errmsg; } TplErr;
static void tpl_err(TplErr *e, Arena *a, Str name, const char *msg) {
  StrBuf b; buf_init(&b);
  buf_addc(&b, "template \""); buf_adds(&b, name); buf_addc(&b, "\": "); buf_addc(&b, msg);
  e->error = true; e->errmsg = err_fmt(a, &b);
}
// pathOf: /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/ or TemplateError.
static bool path_of(Arena *a, Str name, Str text, Str **out_path, size_t *out_n, TplErr *e) {
  // Validate and split on '.'. Each segment: [A-Za-z_][A-Za-z0-9_]*.
  size_t nseg = 1;
  for (size_t i = 0; i < text.n; i++) if (text.s[i] == '.') nseg++;
  Str *segs = arena_alloc(a, nseg * sizeof(Str));
  size_t n = 0, start = 0;
  bool ok = text.n > 0;
  for (size_t i = 0; i <= text.n && ok; i++) {
    if (i == text.n || text.s[i] == '.') {
      Str seg = str_slice(text, start, i);
      if (seg.n == 0) { ok = false; break; }
      uint32_t c0 = seg.s[0];
      if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) { ok = false; break; }
      for (size_t k = 1; k < seg.n; k++) {
        uint32_t c = seg.s[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) { ok = false; break; }
      }
      segs[n++] = str_lit_of(seg); // intern: lookups hit pointer equality
      start = i + 1;
    }
  }
  if (!ok || n != nseg) {
    StrBuf b; buf_init(&b);
    buf_addc(&b, "template \""); buf_adds(&b, name); buf_addc(&b, "\": ");
    ByteBuf bb; bbuf_init(&bb);
    json_escape_into(&bb, text);
    for (size_t i = 0; i < bb.n; i++) buf_add(&b, (unsigned char)bb.p[i]);
    free(bb.p);
    buf_addc(&b, " is not a path");
    e->error = true; e->errmsg = err_fmt(a, &b);
    return false;
  }
  *out_path = segs; *out_n = n;
  return true;
}
typedef enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG } TokKind;
typedef struct { TokKind kind; Str text; Str *path; size_t npath; Str tag; } Token;
typedef struct { Token *v; size_t n, cap; } TokVec;
static void tokvec_push(Arena *a, TokVec *vec, Token t) {
  if (vec->n == vec->cap) {
    size_t nc = vec->cap ? vec->cap * 2 : 8;
    Token *nv = arena_alloc(a, nc * sizeof(Token));
    if (vec->n) memcpy(nv, vec->v, vec->n * sizeof(Token));
    vec->v = nv; vec->cap = nc;
  }
  vec->v[vec->n++] = t;
}
static long last_index_of(Str s, uint32_t c, long pos) { // JS lastIndexOf(ch, pos)
  if (pos >= (long)s.n) pos = (long)s.n - 1;
  for (long i = pos; i >= 0; i--) if (s.s[i] == c) return i;
  return -1;
}
static bool all_sp_tab(Str s) { for (size_t i = 0; i < s.n; i++) if (s.s[i] != 32 && s.s[i] != 9) return false; return true; }
static bool tokenize(Arena *a, Str name, Str src, TokVec *toks, TplErr *e) {
  StrBuf text; buf_init(&text);
  size_t i = 0;
  while (i < src.n) {
    long open = find_char(src, '{', i);
    if (open == -1 || open == (long)src.n - 1) {
      buf_adds(&text, str_slice(src, i, src.n));
      break;
    }
    uint32_t next = src.s[open + 1];
    if (next != '{' && next != '%') {
      buf_adds(&text, str_slice(src, i, (size_t)open + 1));
      i = (size_t)open + 1;
      continue;
    }
    buf_adds(&text, str_slice(src, i, (size_t)open));
    if (next == '{') {
      bool raw = src.s[open + 2] == '{';
      Str close_s = raw ? str_lit("}}}") : str_lit("}}");
      long close = find_str(src, close_s, (size_t)open + (raw ? 3 : 2));
      if (close == -1) {
        tpl_err(e, a, name, raw ? "a {{{ is not closed" : "a {{ is not closed");
        free(text.p); return false;
      }
      if (text.n != 0) { Token t; t.kind = TK_TEXT; t.text = buf_str(a, &text); tokvec_push(a, toks, t); buf_clear(&text); }
      Str inner = trim_st(str_slice(src, (size_t)open + (raw ? 3 : 2), (size_t)close));
      Token t; t.kind = raw ? TK_RAW : TK_VAR;
      if (!path_of(a, name, inner, &t.path, &t.npath, e)) { free(text.p); return false; }
      tokvec_push(a, toks, t);
      i = (size_t)close + (raw ? 3 : 2);
      continue;
    }
    long close = find_str(src, str_lit("%}"), (size_t)open + 2);
    if (close == -1) {
      tpl_err(e, a, name, "a {% is not closed");
      free(text.p); return false;
    }
    size_t end = (size_t)close + 2;
    long lineStart = last_index_of(src, '\n', open == 0 ? 0 : open - 1) + 1;
    long nl = find_char(src, '\n', end);
    size_t lineEnd = nl == -1 ? src.n : (size_t)nl;
    bool standalone = all_sp_tab(str_slice(src, (size_t)lineStart, (size_t)open)) && all_sp_tab(str_slice(src, end, lineEnd));
    if (standalone) text.n -= (size_t)open - (size_t)lineStart;
    if (text.n != 0) { Token t; t.kind = TK_TEXT; t.text = buf_str(a, &text); tokvec_push(a, toks, t); buf_clear(&text); }
    Token t; t.kind = TK_TAG; t.tag = trim_st(str_slice(src, (size_t)open + 2, (size_t)close));
    tokvec_push(a, toks, t);
    i = standalone ? (nl == -1 ? src.n : (size_t)nl + 1) : end;
  }
  if (text.n != 0) { Token t; t.kind = TK_TEXT; t.text = buf_str(a, &text); tokvec_push(a, toks, t); }
  free(text.p);
  return true;
}
// Tag matchers: /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/, /^if (not )?(\S+)$/,
/// /^include "([a-z0-9-]+)"$/. Tags are already trimmed.
static bool is_ident_start(uint32_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_ident_char(uint32_t c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool parse_for_tag(Str tag, Str *out_name, Str *out_path) {
  if (!str_starts(tag, str_lit("for "))) return false;
  size_t i = 4;
  size_t ns = i;
  if (i >= tag.n || !is_ident_start(tag.s[i])) return false;
  while (i < tag.n && is_ident_char(tag.s[i])) i++;
  *out_name = str_slice(tag, ns, i);
  if (!str_starts(str_slice(tag, i, tag.n), str_lit(" in "))) return false;
  Str rest = str_slice(tag, i + 4, tag.n);
  if (rest.n == 0) return false;
  for (size_t k = 0; k < rest.n; k++) if (is_js_ws(rest.s[k])) return false;
  *out_path = rest;
  return true;
}
static bool parse_if_tag(Str tag, bool *out_not, Str *out_path) {
  if (!str_starts(tag, str_lit("if "))) return false;
  size_t i = 3;
  *out_not = false;
  if (str_starts(str_slice(tag, i, tag.n), str_lit("not "))) { *out_not = true; i += 4; }
  Str rest = str_slice(tag, i, tag.n);
  if (rest.n == 0) return false;
  for (size_t k = 0; k < rest.n; k++) if (is_js_ws(rest.s[k])) return false;
  *out_path = rest;
  return true;
}
static bool parse_include_tag(Str tag, Str *out_name) {
  if (!str_starts(tag, str_lit("include \""))) return false;
  if (tag.n < 11 || tag.s[tag.n - 1] != '"') return false;
  Str inner = str_slice(tag, 9, tag.n - 1);
  if (inner.n == 0) return false;
  for (size_t k = 0; k < inner.n; k++) { uint32_t c = inner.s[k]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false; }
  *out_name = inner;
  return true;
}
typedef struct { int kind; TNode *node; NodeVec *body; } Frame; // kind 0=root 1=for 2=if
static bool parse_template(Arena *a, Str name, Str src, NodeVec *out, TplErr *e) {
  TokVec toks; toks.v = NULL; toks.n = 0; toks.cap = 0;
  if (!tokenize(a, name, src, &toks, e)) return false;
  NodeVec root; root.v = NULL; root.n = 0; root.cap = 0;
  Frame stack[64];
  int sp = 0;
  stack[sp].kind = 0; stack[sp].node = NULL; stack[sp].body = &root; sp++;
  NodeVec *body = &root;
  for (size_t ti = 0; ti < toks.n; ti++) {
    Token *tok = &toks.v[ti];
    if (tok->kind == TK_TEXT) { TNode *nd = tnode_new(a, N_TEXT); nd->text = tok->text; nodevec_push(a, body, nd); continue; }
    if (tok->kind == TK_VAR || tok->kind == TK_RAW) {
      TNode *nd = tnode_new(a, tok->kind == TK_VAR ? N_VAR : N_RAW);
      nd->path = tok->path; nd->npath = tok->npath;
      nodevec_push(a, body, nd);
      continue;
    }
    Str tag = tok->tag;
    Frame *top = &stack[sp - 1];
    Str fname, fpath;
    bool inot; Str ipath;
    Str incname;
    if (parse_for_tag(tag, &fname, &fpath)) {
      TNode *nd = tnode_new(a, N_FOR);
      nd->for_name = str_lit_of(fname); // intern: per-iteration sets hit pointer equality
      if (!path_of(a, name, fpath, &nd->path, &nd->npath, e)) return false;
      nodevec_push(a, body, nd);
      if (sp >= 64) { tpl_err(e, a, name, "template nesting too deep"); return false; }
      stack[sp].kind = 1; stack[sp].node = nd; stack[sp].body = &nd->body; sp++;
      body = &nd->body;
    } else if (parse_if_tag(tag, &inot, &ipath)) {
      TNode *nd = tnode_new(a, N_IF);
      nd->not = inot;
      if (!path_of(a, name, ipath, &nd->path, &nd->npath, e)) return false;
      nodevec_push(a, body, nd);
      if (sp >= 64) { tpl_err(e, a, name, "template nesting too deep"); return false; }
      stack[sp].kind = 2; stack[sp].node = nd; stack[sp].body = &nd->then_b; sp++;
      body = &nd->then_b;
    } else if (str_is_ascii(tag, "else")) {
      if (top->kind != 2 || top->node->has_else) { tpl_err(e, a, name, "an else outside an if"); return false; }
      top->node->has_else = true;
      top->body = &top->node->else_b;
      body = &top->node->else_b;
    } else if (str_is_ascii(tag, "endfor") || str_is_ascii(tag, "endif")) {
      bool is_for = str_is_ascii(tag, "endfor");
      if (top->kind != (is_for ? 1 : 2)) {
        StrBuf b; buf_init(&b);
        buf_addc(&b, "an "); buf_adds(&b, tag); buf_addc(&b, " with no open "); buf_addc(&b, is_for ? "for" : "if");
        StrBuf full; buf_init(&full);
        buf_addc(&full, "template \""); buf_adds(&full, name); buf_addc(&full, "\": ");
        buf_adds(&full, buf_view(&b));
        free(b.p);
        e->error = true; e->errmsg = err_fmt(a, &full);
        return false;
      }
      sp--;
      Frame *parent = &stack[sp - 1];
      if (parent->kind == 0) body = parent->body;
      else if (parent->kind == 1) body = &parent->node->body;
      else body = parent->node->has_else ? &parent->node->else_b : &parent->node->then_b;
    } else if (parse_include_tag(tag, &incname)) {
      TNode *nd = tnode_new(a, N_INCLUDE);
      nd->inc_name = incname;
      nodevec_push(a, body, nd);
    } else {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "{% "); buf_adds(&b, tag); buf_addc(&b, " %} is not a tag");
      StrBuf full; buf_init(&full);
      buf_addc(&full, "template \""); buf_adds(&full, name); buf_addc(&full, "\": ");
      buf_adds(&full, buf_view(&b));
      free(b.p);
      e->error = true; e->errmsg = err_fmt(a, &full);
      return false;
    }
  }
  if (sp > 1) {
    Frame *top = &stack[sp - 1];
    StrBuf b; buf_init(&b);
    buf_addc(&b, "a "); buf_addc(&b, top->kind == 1 ? "for" : "if"); buf_addc(&b, " is not closed");
    StrBuf full; buf_init(&full);
    buf_addc(&full, "template \""); buf_adds(&full, name); buf_addc(&full, "\": ");
    buf_adds(&full, buf_view(&b));
    free(b.p);
    e->error = true; e->errmsg = err_fmt(a, &full);
    return false;
  }
  *out = root;
  return true;
}

// ---------------------------------------------------------------- template values
typedef struct Scope Scope;
struct Scope { Val *vars; Scope *parent; };
static Val *lookup(Scope *scope, Str *path, size_t npath) {
  Scope *s = scope;
  Val *v = NULL;
  while (s) { v = val_obj_get(s->vars, path[0]); if (v) break; s = s->parent; }
  if (!v) return NULL;
  for (size_t k = 1; k < npath; k++) {
    if (v->t == VOBJ) v = val_obj_get(v, path[k]);
    else if (v->t == VARR) {
      if (str_is_ascii(path[k], "length")) v = val_num(&case_arena, (double)v->arr.n);
      else {
        bool alldig = path[k].n > 0;
        long idx = 0;
        for (size_t q = 0; q < path[k].n; q++) { if (path[k].s[q] < '0' || path[k].s[q] > '9') alldig = false; else idx = idx * 10 + (long)(path[k].s[q] - '0'); }
        if (!alldig || idx >= (long)v->arr.n) return NULL;
        v = v->arr.items[idx];
      }
    } else return NULL;
    if (!v) return NULL;
  }
  return v;
}
static void text_of(Arena *a, StrBuf *out, Val *v) {
  if (!v || v->t == VUNDEF || v->t == VNULL || v->t == VOBJ) return;
  if (v->t == VSTR) { buf_adds(out, v->s); return; }
  if (v->t == VNUM) { js_num_into(out, v->num); return; }
  if (v->t == VBOOL) { buf_addc(out, v->b ? "true" : "false"); return; }
  if (v->t == VARR) {
    for (size_t i = 0; i < v->arr.n; i++) {
      if (i) buf_addc(out, ", ");
      text_of(a, out, v->arr.items[i]);
    }
    return;
  }
}
static bool truthy(Val *v) {
  if (!v || v->t == VUNDEF || v->t == VNULL) return false;
  if (v->t == VBOOL) return v->b;
  if (v->t == VNUM) return v->num != 0;
  if (v->t == VSTR) return v->s.n != 0;
  if (v->t == VARR) return v->arr.n != 0;
  return true; // objects are truthy
}

// ---------------------------------------------------------------- include cache
// computeUses: for each template, the root names it reads from its scope
// (transitively through includes), excluding names bound by its own
// for-loops. A template whose includes form a cycle is uncacheable (NULL).
typedef struct { Str *roots; size_t n, cap; } Uses;
typedef struct {
  SMap *templates; // Str name -> NodeVec* (parsed body)
  SMap *memo;      // Str name -> Uses* (NULL = uncacheable; slot presence = done)
  Str stack[64];   // the current DFS path (the JS `visiting` set)
  int sp;
} UsesCtx;
static void uses_add(Arena *a, Uses *u, Str root) {
  for (size_t i = 0; i < u->n; i++) if (str_eq(u->roots[i], root)) return;
  if (u->n == u->cap) {
    size_t nc = u->cap ? u->cap * 2 : 8;
    Str *nr = arena_alloc(a, nc * sizeof(Str));
    if (u->n) memcpy(nr, u->roots, u->n * sizeof(Str));
    u->roots = nr; u->cap = nc;
  }
  u->roots[u->n++] = root;
}
static bool bound_has(Str *bound, size_t nbound, Str name) {
  for (size_t i = 0; i < nbound; i++) if (str_eq(bound[i], name)) return true;
  return false;
}
static Uses *uses_of(Arena *a, UsesCtx *ctx, Str name);
static void uses_walk(Arena *a, UsesCtx *ctx, NodeVec *nodes, Str *bound, size_t nbound, Uses *uses, bool *bad) {
  for (size_t i = 0; i < nodes->n; i++) {
    TNode *nd = nodes->v[i];
    switch (nd->kind) {
      case N_VAR: case N_RAW:
        if (!bound_has(bound, nbound, nd->path[0])) uses_add(a, uses, nd->path[0]);
        break;
      case N_IF:
        if (!bound_has(bound, nbound, nd->path[0])) uses_add(a, uses, nd->path[0]);
        uses_walk(a, ctx, &nd->then_b, bound, nbound, uses, bad);
        if (nd->has_else) uses_walk(a, ctx, &nd->else_b, bound, nbound, uses, bad);
        break;
      case N_FOR: {
        if (!bound_has(bound, nbound, nd->path[0])) uses_add(a, uses, nd->path[0]);
        // bound + {for_name, 'loop'} (bound sets stay tiny: at most one name
        // per enclosing for, and templates nest only a few deep)
        Str nb[16];
        size_t nn = nbound < 14 ? nbound : 14;
        memcpy(nb, bound, nn * sizeof(Str));
        nb[nn++] = nd->for_name;
        nb[nn++] = str_lit("loop");
        uses_walk(a, ctx, &nd->body, nb, nn, uses, bad);
        break;
      }
      case N_INCLUDE: {
        Uses *u = uses_of(a, ctx, nd->inc_name);
        if (!u) *bad = true;
        else for (size_t k = 0; k < u->n; k++) uses_add(a, uses, u->roots[k]);
        break;
      }
      default: break;
    }
  }
}
static Uses *uses_of(Arena *a, UsesCtx *ctx, Str name) {
  void **slot = smap_slot(ctx->memo, name, false);
  if (slot) return (Uses *)*slot;
  for (int i = 0; i < ctx->sp; i++) if (str_eq(ctx->stack[i], name)) return NULL; // cycle: uncacheable, not memoized
  NodeVec **tnode = (NodeVec **)smap_slot(ctx->templates, name, false);
  if (!tnode) {
    Uses *u = arena_alloc(a, sizeof(Uses));
    memset(u, 0, sizeof(Uses));
    smap_put(ctx->memo, name, u);
    return u; // unknown template: empty uses (JS: new Set())
  }
  if (ctx->sp >= 64) { smap_put(ctx->memo, name, NULL); return NULL; }
  ctx->stack[ctx->sp++] = name;
  Uses *uses = arena_alloc(a, sizeof(Uses));
  memset(uses, 0, sizeof(Uses));
  bool bad = false;
  uses_walk(a, ctx, *tnode, NULL, 0, uses, &bad);
  ctx->sp--;
  smap_put(ctx->memo, name, bad ? NULL : uses);
  return bad ? NULL : uses;
}

// ---------------------------------------------------------------- render
typedef struct {
  SMap *templates; // Str name -> NodeVec* (parsed body)
  SMap *tpl_ops;   // Str name -> OpVec* (compiled body; same semantics, flat)
  SMap *uses;      // Str name -> Uses* (NULL = uncacheable)
  SMap *cache;     // Str key -> Str* (rendered include)
  PMap *obj_ids;
  int next_id;
  Arena *a;
} TplCtx;
static void render_nodes(Arena *a, NodeVec *nodes, Scope *scope, TplCtx *ctx, int depth, StrBuf *out) {
  for (size_t i = 0; i < nodes->n; i++) {
    TNode *nd = nodes->v[i];
    switch (nd->kind) {
      case N_TEXT:
        buf_adds(out, nd->text);
        break;
      case N_VAR: case N_RAW: {
        Val *v = lookup(scope, nd->path, nd->npath);
        bool raw = nd->kind == N_RAW;
        if (!v || v->t == VUNDEF || v->t == VNULL || v->t == VOBJ) break;
        if (v->t == VSTR) { if (raw) buf_adds(out, v->s); else esc_append(out, v->s); break; }
        if (v->t == VNUM) { StrBuf tmp; buf_init(&tmp); js_num_into(&tmp, v->num); if (raw) buf_adds(out, buf_view(&tmp)); else esc_append(out, buf_view(&tmp)); free(tmp.p); break; }
        if (v->t == VBOOL) { buf_addc(out, v->b ? "true" : "false"); break; }
        if (v->t == VARR) {
          StrBuf tmp; buf_init(&tmp);
          text_of(a, &tmp, v);
          if (raw) buf_adds(out, buf_view(&tmp)); else esc_append(out, buf_view(&tmp));
          free(tmp.p);
          break;
        }
        break;
      }
      case N_IF: {
        Val *v = lookup(scope, nd->path, nd->npath);
        bool tv = truthy(v);
        NodeVec *branch = (tv != nd->not) ? &nd->then_b : (nd->has_else ? &nd->else_b : NULL);
        if (branch) render_nodes(a, branch, scope, ctx, depth, out);
        break;
      }
      case N_FOR: {
        Val *xs = lookup(scope, nd->path, nd->npath);
        if (!xs || xs->t != VARR) break;
        Val *vars = val_obj(a);
        Val *loop = val_obj(a);
        val_obj_set(a, loop, str_lit("length"), val_num(a, (double)xs->arr.n));
        Scope child; child.vars = vars; child.parent = scope;
        for (size_t k = 0; k < xs->arr.n; k++) {
          val_obj_set(a, vars, nd->for_name, xs->arr.items[k]);
          val_obj_set(a, loop, str_lit("index"), val_num(a, (double)(k + 1)));
          val_obj_set(a, loop, str_lit("first"), val_bool(a, k == 0));
          val_obj_set(a, loop, str_lit("last"), val_bool(a, k == xs->arr.n - 1));
          val_obj_set(a, vars, str_lit("loop"), loop);
          render_nodes(a, &nd->body, &child, ctx, depth, out);
        }
        break;
      }
      case N_INCLUDE: {
        void **tslot = smap_slot(ctx->templates, nd->inc_name, false);
        if (!tslot || depth >= 8) break;
        NodeVec *t = *(NodeVec **)tslot;
        void **uslot = smap_slot(ctx->uses, nd->inc_name, false);
        if (uslot && *uslot) {
          Uses *uses = (Uses *)*uslot;
          StrBuf key; buf_init(&key);
          buf_adds(&key, nd->inc_name);
          buf_add(&key, 0);
          buf_add(&key, 'd');
          num_into(&key, depth);
          for (size_t k = 0; k < uses->n; k++) {
            Str root = uses->roots[k];
            buf_add(&key, 0);
            buf_adds(&key, root);
            Val *v = lookup(scope, &root, 1);
            if (v && (v->t == VOBJ || v->t == VARR)) {
              int id = 0;
              if (!pmap_get(ctx->obj_ids, v, &id)) { id = ++ctx->next_id; pmap_put(ctx->obj_ids, v, id); }
              buf_addc(&key, "#obj");
              num_into(&key, id);
            } else {
              buf_add(&key, '=');
              json_stringify_val(&key, v);
            }
          }
          Str keyview = buf_view(&key);
          void **hit = smap_slot(ctx->cache, keyview, false);
          if (hit) {
            buf_adds(out, **(Str **)hit);
            free(key.p);
            break;
          }
          // Cache miss: render straight into the parent buffer (no temp
          // buffer + no second copy), then copy just the rendered slice
          // into the arena for the cache.
          size_t start = out->n;
          render_nodes(a, t, scope, ctx, depth + 1, out);
          Str s = buf_str_range(a, out, start, out->n);
          Str keycopy = buf_str(a, &key);
          Str *sp = arena_alloc(a, sizeof(Str));
          *sp = s;
          smap_put(ctx->cache, keycopy, sp);
          free(key.p);
        } else {
          render_nodes(a, t, scope, ctx, depth + 1, out);
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- compiled templates
// A template compiled to a flat, contiguous op array. Same semantics as the
// NodeVec tree (same lookup, same include cache, same scope chain), but the
// render loop walks a contiguous array instead of chasing heap pointers.
// Branch targets are absolute op indices.
typedef enum { OP_TEXT, OP_VAR, OP_RAW, OP_IF, OP_JUMP, OP_FOR, OP_INC, OP_END } OpKind;
typedef struct {
  OpKind kind;
  Str text;                 // OP_TEXT
  Str *path; size_t npath;  // OP_VAR, OP_RAW, OP_IF, OP_FOR
  bool not;                 // OP_IF
  size_t jump;              // OP_IF/OP_JUMP: index past the matching OP_END; OP_FOR: index past OP_END
  size_t else_jump;         // OP_IF with else: index of the first else op (0 if none)
  Str for_name;             // OP_FOR
  Str inc_name;             // OP_INC
} Op;
typedef struct { Op *v; size_t n, cap; } OpVec;
static void opvec_push(Arena *a, OpVec *vec, Op op) {
  if (vec->n == vec->cap) {
    size_t nc = vec->cap ? vec->cap * 2 : 16;
    Op *nv = arena_alloc(a, nc * sizeof(Op));
    if (vec->n) memcpy(nv, vec->v, vec->n * sizeof(Op));
    vec->v = nv; vec->cap = nc;
  }
  vec->v[vec->n++] = op;
}
static void compile_nodes(Arena *a, NodeVec *nodes, OpVec *ops) {
  for (size_t i = 0; i < nodes->n; i++) {
    TNode *nd = nodes->v[i];
    switch (nd->kind) {
      case N_TEXT: { Op op; memset(&op, 0, sizeof(op)); op.kind = OP_TEXT; op.text = nd->text; opvec_push(a, ops, op); break; }
      case N_VAR: case N_RAW: { Op op; memset(&op, 0, sizeof(op)); op.kind = nd->kind == N_VAR ? OP_VAR : OP_RAW; op.path = nd->path; op.npath = nd->npath; opvec_push(a, ops, op); break; }
      case N_IF: {
        Op op; memset(&op, 0, sizeof(op)); op.kind = OP_IF; op.path = nd->path; op.npath = nd->npath; op.not = nd->not;
        size_t if_idx = ops->n; opvec_push(a, ops, op);
        compile_nodes(a, &nd->then_b, ops);
        if (nd->has_else) {
          Op jmp; memset(&jmp, 0, sizeof(jmp)); jmp.kind = OP_JUMP;
          size_t jmp_idx = ops->n; opvec_push(a, ops, jmp);
          ops->v[if_idx].else_jump = ops->n;
          compile_nodes(a, &nd->else_b, ops);
          Op end; memset(&end, 0, sizeof(end)); end.kind = OP_END;
          opvec_push(a, ops, end);
          ops->v[jmp_idx].jump = ops->n;
          ops->v[if_idx].jump = ops->n;
        } else {
          Op end; memset(&end, 0, sizeof(end)); end.kind = OP_END;
          opvec_push(a, ops, end);
          ops->v[if_idx].jump = ops->n;
        }
        break;
      }
      case N_FOR: {
        Op op; memset(&op, 0, sizeof(op)); op.kind = OP_FOR; op.path = nd->path; op.npath = nd->npath; op.for_name = nd->for_name;
        size_t for_idx = ops->n; opvec_push(a, ops, op);
        compile_nodes(a, &nd->body, ops);
        Op end; memset(&end, 0, sizeof(end)); end.kind = OP_END;
        opvec_push(a, ops, end);
        ops->v[for_idx].jump = ops->n;
        break;
      }
      case N_INCLUDE: { Op op; memset(&op, 0, sizeof(op)); op.kind = OP_INC; op.inc_name = nd->inc_name; opvec_push(a, ops, op); break; }
    }
  }
}
static void render_ops(Arena *a, Op *ops, size_t start, size_t end, Scope *scope, TplCtx *ctx, int depth, StrBuf *out) {
  size_t pc = start;
  while (pc < end) {
    Op *op = &ops[pc];
    switch (op->kind) {
      case OP_TEXT:
        buf_adds(out, op->text); pc++; break;
      case OP_VAR: case OP_RAW: {
        Val *v = lookup(scope, op->path, op->npath);
        bool raw = op->kind == OP_RAW;
        if (!v || v->t == VUNDEF || v->t == VNULL || v->t == VOBJ) { pc++; break; }
        if (v->t == VSTR) { if (raw) buf_adds(out, v->s); else esc_append(out, v->s); pc++; break; }
        if (v->t == VNUM) { StrBuf tmp; buf_init(&tmp); js_num_into(&tmp, v->num); if (raw) buf_adds(out, buf_view(&tmp)); else esc_append(out, buf_view(&tmp)); free(tmp.p); pc++; break; }
        if (v->t == VBOOL) { buf_addc(out, v->b ? "true" : "false"); pc++; break; }
        if (v->t == VARR) {
          StrBuf tmp; buf_init(&tmp);
          text_of(a, &tmp, v);
          if (raw) buf_adds(out, buf_view(&tmp)); else esc_append(out, buf_view(&tmp));
          free(tmp.p);
          pc++; break;
        }
        pc++; break;
      }
      case OP_IF: {
        Val *v = lookup(scope, op->path, op->npath);
        bool tv = truthy(v);
        if (tv != op->not) {
          size_t then_end = op->else_jump ? op->else_jump - 1 : op->jump - 1;
          render_ops(a, ops, pc + 1, then_end, scope, ctx, depth, out);
        } else if (op->else_jump) {
          render_ops(a, ops, op->else_jump, op->jump - 1, scope, ctx, depth, out);
        }
        pc = op->jump;
        break;
      }
      case OP_FOR: {
        Val *xs = lookup(scope, op->path, op->npath);
        if (!xs || xs->t != VARR) { pc = op->jump; break; }
        Val *vars = val_obj(a);
        Val *loop = val_obj(a);
        val_obj_set(a, loop, str_lit("length"), val_num(a, (double)xs->arr.n));
        Scope child; child.vars = vars; child.parent = scope;
        for (size_t k = 0; k < xs->arr.n; k++) {
          val_obj_set(a, vars, op->for_name, xs->arr.items[k]);
          val_obj_set(a, loop, str_lit("index"), val_num(a, (double)(k + 1)));
          val_obj_set(a, loop, str_lit("first"), val_bool(a, k == 0));
          val_obj_set(a, loop, str_lit("last"), val_bool(a, k == xs->arr.n - 1));
          val_obj_set(a, vars, str_lit("loop"), loop);
          render_ops(a, ops, pc + 1, op->jump - 1, &child, ctx, depth, out);
        }
        pc = op->jump;
        break;
      }
      case OP_INC: {
        void **tslot = smap_slot(ctx->tpl_ops, op->inc_name, false);
        if (!tslot || depth >= 8) { pc++; break; }
        OpVec *t = *(OpVec **)tslot;
        void **uslot = smap_slot(ctx->uses, op->inc_name, false);
        if (uslot && *uslot) {
          Uses *uses = (Uses *)*uslot;
          StrBuf key; buf_init(&key);
          buf_adds(&key, op->inc_name);
          buf_add(&key, 0);
          buf_add(&key, 'd');
          num_into(&key, depth);
          for (size_t k = 0; k < uses->n; k++) {
            Str root = uses->roots[k];
            buf_add(&key, 0);
            buf_adds(&key, root);
            Val *v = lookup(scope, &root, 1);
            if (v && (v->t == VOBJ || v->t == VARR)) {
              int id = 0;
              if (!pmap_get(ctx->obj_ids, v, &id)) { id = ++ctx->next_id; pmap_put(ctx->obj_ids, v, id); }
              buf_addc(&key, "#obj");
              num_into(&key, id);
            } else {
              buf_add(&key, '=');
              json_stringify_val(&key, v);
            }
          }
          Str keyview = buf_view(&key);
          void **hit = smap_slot(ctx->cache, keyview, false);
          if (hit) {
            buf_adds(out, **(Str **)hit);
            free(key.p);
            pc++; break;
          }
          size_t start2 = out->n;
          render_ops(a, t->v, 0, t->n, scope, ctx, depth + 1, out);
          Str s = buf_str_range(a, out, start2, out->n);
          Str keycopy = buf_str(a, &key);
          Str *sp = arena_alloc(a, sizeof(Str));
          *sp = s;
          smap_put(ctx->cache, keycopy, sp);
          free(key.p);
        } else {
          render_ops(a, t->v, 0, t->n, scope, ctx, depth + 1, out);
        }
        pc++; break;
      }
      case OP_JUMP: pc = op->jump; break;
      case OP_END: pc++; break;
    }
  }
}

// ---------------------------------------------------------------- links
static Str dir_of(Str path) {
  long idx = -1;
  for (size_t i = 0; i < path.n; i++) if (path.s[i] == '/') idx = (long)i;
  return idx == -1 ? str_empty() : str_slice(path, 0, (size_t)idx);
}
static bool resolve_path(Arena *a, Str dir, Str rel, Str *out) {
  Str parts[64];
  size_t np = 0;
  if (dir.n > 0) {
    size_t start = 0;
    for (size_t i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.s[i] == '/') { if (np < 64) parts[np++] = str_slice(dir, start, i); start = i + 1; }
    }
  }
  size_t start = 0;
  for (size_t i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.s[i] == '/') {
      Str seg = str_slice(rel, start, i);
      start = i + 1;
      if (seg.n == 0) continue;
      if (seg.n == 1 && seg.s[0] == '.') continue;
      if (seg.n == 2 && seg.s[0] == '.' && seg.s[1] == '.') {
        if (np == 0) return false;
        np--;
      } else {
        if (np >= 64) return false;
        parts[np++] = seg;
      }
    }
  }
  StrBuf b; buf_init(&b);
  for (size_t i = 0; i < np; i++) { if (i) buf_add(&b, '/'); buf_adds(&b, parts[i]); }
  *out = buf_str(a, &b);
  free(b.p);
  return true;
}
static Str link_resolve(Arena *a, LinkEnv *env, Str target) {
  if (target.n > 0 && (target.s[0] == '/' || target.s[0] == '#')) return target;
  if (target.n >= 2) { // /^[A-Za-z][A-Za-z0-9+.-]*:/
    uint32_t c0 = target.s[0];
    if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
      size_t i = 1;
      while (i < target.n) {
        uint32_t c = target.s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) break;
        i++;
      }
      if (i < target.n && target.s[i] == ':') return target;
    }
  }
  long hash = find_char(target, '#', 0);
  Str rel = hash == -1 ? target : str_slice(target, 0, (size_t)hash);
  Str frag = hash == -1 ? str_empty() : str_slice(target, (size_t)hash, target.n);
  Str path;
  bool ok = rel.n != 0 && resolve_path(a, env->dir, rel, &path);
  if (ok) {
    void **hit = smap_slot(env->by_source, path, false);
    if (hit) {
      Source *src = *(Source **)hit;
      StrBuf b; buf_init(&b);
      buf_adds(&b, src->url); buf_adds(&b, frag);
      Str r = buf_str(a, &b); free(b.p);
      return r;
    }
    if (smap_get(env->static_paths, path)) {
      StrBuf b; buf_init(&b);
      buf_adds(&b, env->base); buf_add(&b, '/'); buf_adds(&b, path); buf_adds(&b, frag);
      Str r = buf_str(a, &b); free(b.p);
      return r;
    }
  }
  StrBuf b; buf_init(&b);
  buf_addc(&b, "broken-link "); buf_adds(&b, env->src_path); buf_addc(&b, " -> "); buf_adds(&b, target);
  Str line = buf_str(a, &b); free(b.p);
  if (!smap_get(env->broken_set, line)) { smap_put(env->broken_set, line, (void *)1); strvec_push(env->broken, line); }
  return target;
}

// ---------------------------------------------------------------- build
typedef struct { Str path, content; } OutEntry;
typedef struct { OutEntry *v; size_t n, cap; } OutVec;
static void out_set(Arena *a, OutVec *o, Str path, Str content) {
  for (size_t i = 0; i < o->n; i++) if (str_eq(o->v[i].path, path)) { o->v[i].content = content; return; }
  if (o->n == o->cap) {
    size_t nc = o->cap ? o->cap * 2 : 64;
    OutEntry *nv = arena_alloc(a, nc * sizeof(OutEntry));
    if (o->n) memcpy(nv, o->v, o->n * sizeof(OutEntry));
    o->v = nv; o->cap = nc;
  }
  o->v[o->n].path = path; o->v[o->n].content = content; o->n++;
}
static bool out_has(OutVec *o, Str path) {
  for (size_t i = 0; i < o->n; i++) if (str_eq(o->v[i].path, path)) return true;
  return false;
}
static void report_push(Arena *a, StrVec *report, StrBuf *b) {
  Str line = buf_str(a, b);
  buf_clear(b);
  strvec_push(report, line);
}
static int cmp_str_qsort(const void *pa, const void *pb) {
  return str_cmp(*(const Str *)pa, *(const Str *)pb);
}
static int cmp_file_path(const void *pa, const void *pb) {
  Val *fa = *(Val **)pa, *fb = *(Val **)pb;
  Str xa = val_obj_get_lit(fa, "path")->s, xb = val_obj_get_lit(fb, "path")->s;
  return str_cmp(xa, xb);
}
static int cmp_post_qsort(const void *pa, const void *pb) {
  Source *x = *(Source **)pa, *y = *(Source **)pb;
  Str dx = val_obj_get_lit(x->data, "date")->s, dy = val_obj_get_lit(y->data, "date")->s;
  int c = str_cmp(dy, dx); // newest first
  return c != 0 ? c : str_cmp(x->slug, y->slug);
}
static int cmp_tag_qsort(const void *pa, const void *pb) {
  Tag *x = *(Tag **)pa, *y = *(Tag **)pb;
  if (x->nposts != y->nposts) return x->nposts > y->nposts ? -1 : 1;
  return str_cmp(x->slug, y->slug);
}
static const char *MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static Str long_date(Arena *a, SMap *cache, Str date) {
  void *hit = smap_get(cache, date);
  if (hit) return *(Str *)hit;
  StrBuf b; buf_init(&b);
  long d = (date.s[8] - '0') * 10 + (date.s[9] - '0');
  long m = (date.s[5] - '0') * 10 + (date.s[6] - '0');
  num_into(&b, d);
  buf_add(&b, ' ');
  buf_addc(&b, MONTHS[m - 1]);
  buf_add(&b, ' ');
  buf_addn(&b, date.s, 4);
  Str r = buf_str(a, &b);
  free(b.p);
  Str *slot = arena_alloc(a, sizeof(Str));
  *slot = r;
  smap_put(cache, date, slot);
  return r;
}
typedef struct { Str name, sl; } NameSlug;
static size_t own_tags(Arena *a, SMap *slug_cache, Source *s, NameSlug *out, size_t cap) {
  Val *tags_v = val_obj_get_lit(s->data, "tags");
  size_t n = 0;
  if (!tags_v || tags_v->t != VARR) return 0;
  for (size_t i = 0; i < tags_v->arr.n; i++) {
    Str name = tags_v->arr.items[i]->s;
    Str sl = slugify(slug_cache, a, name);
    bool seen = false;
    for (size_t k = 0; k < n; k++) if (str_eq(out[k].sl, sl)) seen = true;
    if (!seen && n < cap) { out[n].name = name; out[n].sl = sl; n++; }
  }
  return n;
}
static void tag_add_post(Arena *a, Tag *tag, Source *s) {
  if (tag->nposts == tag->cap) {
    size_t nc = tag->cap ? tag->cap * 2 : 8;
    Source **np = arena_alloc(a, nc * sizeof(Source *));
    if (tag->nposts) memcpy(np, tag->posts, tag->nposts * sizeof(Source *));
    tag->posts = np; tag->cap = nc;
  }
  tag->posts[tag->nposts++] = s;
}
static Val *card_of(Arena *a, SMap *slug_cache, SMap *date_cache, Str base, Source *s) {
  (void)slug_cache; (void)base;
  if (s->card) return s->card;
  Val *c = val_obj(a);
  val_obj_set(a, c, str_lit("title"), val_str(a, val_obj_get_lit(s->data, "title")->s));
  val_obj_set(a, c, str_lit("url"), val_str(a, s->url));
  Val *date_v = val_obj_get_lit(s->data, "date");
  val_obj_set(a, c, str_lit("date"), val_str(a, date_v ? date_v->s : str_empty()));
  val_obj_set(a, c, str_lit("date_long"), val_str(a, date_v ? long_date(a, date_cache, date_v->s) : str_empty()));
  Val *sum = val_obj_get_lit(s->data, "summary");
  val_obj_set(a, c, str_lit("summary"), val_str(a, sum ? sum->s : str_empty()));
  val_obj_set(a, c, str_lit("excerpt"), val_str(a, s->excerpt));
  long rm = (words_of(s->body) + 199) / 200;
  if (rm < 1) rm = 1;
  val_obj_set(a, c, str_lit("reading_minutes"), val_num(a, (double)rm));
  Val *tags_arr = val_arr(a);
  for (size_t i = 0; i < s->ntag_refs; i++) {
    Tag *t = s->tag_refs[i];
    Val *tv = val_obj(a);
    val_obj_set(a, tv, str_lit("name"), val_str(a, t->name));
    val_obj_set(a, tv, str_lit("slug"), val_str(a, t->slug));
    val_obj_set(a, tv, str_lit("url"), val_str(a, t->url));
    if (tags_arr->arr.n == tags_arr->arr.cap) {
      size_t nc = tags_arr->arr.cap ? tags_arr->arr.cap * 2 : 8;
      Val **ni = arena_alloc(a, nc * sizeof(Val *));
      if (tags_arr->arr.n) memcpy(ni, tags_arr->arr.items, tags_arr->arr.n * sizeof(Val *));
      tags_arr->arr.items = ni; tags_arr->arr.cap = nc;
    }
    tags_arr->arr.items[tags_arr->arr.n++] = tv;
  }
  val_obj_set(a, c, str_lit("tags"), tags_arr);
  s->card = c;
  return c;
}
static Val *link_of(Arena *a, SMap *date_cache, Source *s) {
  if (!s) return val_null(a);
  Val *o = val_obj(a);
  val_obj_set(a, o, str_lit("title"), val_str(a, val_obj_get_lit(s->data, "title")->s));
  val_obj_set(a, o, str_lit("url"), val_str(a, s->url));
  Val *date_v = val_obj_get_lit(s->data, "date");
  val_obj_set(a, o, str_lit("date"), val_str(a, date_v ? date_v->s : str_empty()));
  val_obj_set(a, o, str_lit("date_long"), val_str(a, date_v ? long_date(a, date_cache, date_v->s) : str_empty()));
  return o;
}
static Val *page_of(Arena *a, SMap *slug_cache, SMap *date_cache, Str base, Source *s) {
  Val *card = card_of(a, slug_cache, date_cache, base, s);
  Val *p = val_obj(a);
  for (size_t i = 0; i < card->obj.n; i++) val_obj_set(a, p, card->obj.keys[i], card->obj.vals[i]);
  val_obj_set(a, p, str_lit("slug"), val_str(a, s->slug));
  val_obj_set(a, p, str_lit("content"), val_str(a, s->html));
  Val *toc = val_arr(a);
  for (size_t i = 0; i < s->ntoc; i++) {
    Val *h = val_obj(a);
    val_obj_set(a, h, str_lit("level"), val_num(a, (double)s->toc[i].level));
    val_obj_set(a, h, str_lit("id"), val_str(a, s->toc[i].id));
    val_obj_set(a, h, str_lit("html"), val_str(a, s->toc[i].html));
    if (toc->arr.n == toc->arr.cap) {
      size_t nc = toc->arr.cap ? toc->arr.cap * 2 : 8;
      Val **ni = arena_alloc(a, nc * sizeof(Val *));
      if (toc->arr.n) memcpy(ni, toc->arr.items, toc->arr.n * sizeof(Val *));
      toc->arr.items = ni; toc->arr.cap = nc;
    }
    toc->arr.items[toc->arr.n++] = h;
  }
  val_obj_set(a, p, str_lit("toc"), toc);
  val_obj_set(a, p, str_lit("newer"), link_of(a, date_cache, s->newer));
  val_obj_set(a, p, str_lit("older"), link_of(a, date_cache, s->older));
  val_obj_set(a, p, str_lit("is_post"), val_bool(a, s->post));
  return p;
}
static bool render_tpl(Arena *a, TplCtx *ctx, Str name, Val *vars, Str *out) {
  void **slot = smap_slot(ctx->tpl_ops, name, false);
  if (!slot) return false;
  OpVec *ops = *(OpVec **)slot;
  Scope scope; scope.vars = vars; scope.parent = NULL;
  StrBuf b; buf_init(&b);
  render_ops(a, ops->v, 0, ops->n, &scope, ctx, 0, &b);
  *out = buf_str(a, &b);
  free(b.p);
  return true;
}
static void arr_push_val(Arena *a, Val *arr, Val *item) {
  if (arr->arr.n == arr->arr.cap) {
    size_t nc = arr->arr.cap ? arr->arr.cap * 2 : 8;
    Val **ni = arena_alloc(a, nc * sizeof(Val *));
    if (arr->arr.n) memcpy(ni, arr->arr.items, arr->arr.n * sizeof(Val *));
    arr->arr.items = ni; arr->arr.cap = nc;
  }
  arr->arr.items[arr->arr.n++] = item;
}

static OutVec *build_case(Arena *a, Val *input) {
  SMap slug_cache; smap_init(&slug_cache, 256);
  SMap date_cache; smap_init(&date_cache, 64);
  SMap templates; smap_init(&templates, 64);
  StrVec report; memset(&report, 0, sizeof(report));
  StrBuf lb; buf_init(&lb);
  Val *tpl_obj = val_obj_get_lit(input, "templates");
  Val *site = val_obj_get_lit(input, "site");
  Val *files_v = val_obj_get_lit(input, "files");
  if (!tpl_obj || !site || !files_v) die("input is missing site/templates/files");

  // Template names, sorted by code unit; each parsed or reported.
  size_t ntnames = tpl_obj->obj.n;
  Str *tnames = arena_alloc(a, (ntnames ? ntnames : 1) * sizeof(Str));
  for (size_t i = 0; i < ntnames; i++) tnames[i] = tpl_obj->obj.keys[i];
  qsort(tnames, ntnames, sizeof(Str), cmp_str_qsort);
  for (size_t i = 0; i < ntnames; i++) {
    Str tname = tnames[i];
    Val *src = val_obj_get(tpl_obj, tname);
    NodeVec body; TplErr e; e.error = false;
    if (!parse_template(a, tname, src->s, &body, &e)) {
      buf_adds(&lb, e.errmsg);
      report_push(a, &report, &lb);
      continue;
    }
    NodeVec *np = arena_alloc(a, sizeof(NodeVec));
    *np = body;
    smap_put(&templates, tname, np);
  }

  Str base = val_obj_get_lit(site, "base_url")->s;
  while (base.n > 0 && base.s[base.n - 1] == '/') base.n--;
  OutVec *output = arena_alloc(a, sizeof(OutVec));
  memset(output, 0, sizeof(OutVec));
  SMap by_source; smap_init(&by_source, 512);

  // 1. Sources, sorted by path; .md is a source, else static.
  size_t nf = files_v->arr.n;
  Val **farr = arena_alloc(a, (nf ? nf : 1) * sizeof(Val *));
  if (nf) memcpy(farr, files_v->arr.items, nf * sizeof(Val *));
  qsort(farr, nf, sizeof(Val *), cmp_file_path);
  Source *sources = arena_alloc(a, (nf ? nf : 1) * sizeof(Source));
  size_t nsrc = 0;
  typedef struct { Str path, content; } StaticEnt;
  StaticEnt *statics = arena_alloc(a, (nf ? nf : 1) * sizeof(StaticEnt));
  size_t nstat = 0;
  for (size_t i = 0; i < nf; i++) {
    Val *f = farr[i];
    Str path = val_obj_get_lit(f, "path")->s;
    Str content = val_obj_get_lit(f, "content")->s;
    if (str_ends(path, str_lit(".md"))) {
      Source *s = &sources[nsrc++];
      memset(s, 0, sizeof(Source));
      s->path = path;
      if (find_char(content, '\r', 0) == -1) s->text = content;
      else {
        StrBuf b; buf_init(&b);
        for (size_t k = 0; k < content.n; k++) {
          if (content.s[k] == '\r') { if (k + 1 < content.n && content.s[k + 1] == '\n') k++; buf_add(&b, '\n'); }
          else buf_add(&b, content.s[k]);
        }
        s->text = buf_str(a, &b);
        free(b.p);
      }
    } else {
      statics[nstat].path = path;
      statics[nstat].content = content;
      nstat++;
    }
  }

  // 2. Front matter, in the reference's check order.
  Source *kept = arena_alloc(a, (nsrc ? nsrc : 1) * sizeof(Source));
  size_t nkept = 0;
  for (size_t i = 0; i < nsrc; i++) {
    Source *s = &sources[i];
    FMResult fm = parse_front_matter(a, s->text);
    if (fm.error) {
      buf_addc(&lb, "refused "); buf_adds(&lb, s->path); buf_addc(&lb, ": "); buf_adds(&lb, fm.errmsg);
      report_push(a, &report, &lb);
      continue;
    }
    s->data = fm.data;
    s->body = fm.body;
    s->post = str_starts(s->path, str_lit("posts/"));
    Str own = str_empty();
    bool has_own = false;
    if (str_starts(s->path, str_lit("tags/"))) { own = str_lit("tags"); has_own = true; }
    else if (str_starts(s->path, str_lit("page/"))) { own = str_lit("page"); has_own = true; }
    if (has_own) {
      buf_addc(&lb, "refused "); buf_adds(&lb, s->path); buf_addc(&lb, ": "); buf_adds(&lb, own); buf_addc(&lb, "/ is the generator's own");
      report_push(a, &report, &lb);
      continue;
    }
    if (s->post && !val_has(s->data, str_lit("date"))) {
      buf_addc(&lb, "refused "); buf_adds(&lb, s->path); buf_addc(&lb, ": a post needs a date");
      report_push(a, &report, &lb);
      continue;
    }
    Val *layout_v = val_obj_get_lit(s->data, "layout");
    Str layout = layout_v ? layout_v->s : (s->post ? str_lit("post") : str_lit("page"));
    if (!smap_get(&templates, layout)) {
      buf_addc(&lb, "refused "); buf_adds(&lb, s->path); buf_addc(&lb, ": layout \""); buf_adds(&lb, layout); buf_addc(&lb, "\" is no template that parses");
      report_push(a, &report, &lb);
      continue;
    }
    s->layout = layout;
    Val *draft = val_obj_get_lit(s->data, "draft");
    if (draft && draft->t == VBOOL && draft->b) {
      buf_addc(&lb, "draft "); buf_adds(&lb, s->path);
      report_push(a, &report, &lb);
      continue;
    }
    kept[nkept++] = *s;
  }
  sources = kept;
  nsrc = nkept;

  // 3. Permalinks: slug unique in its directory, in source order.
  {
    SMap taken_dirs; smap_init(&taken_dirs, 64);
    for (size_t i = 0; i < nsrc; i++) {
      Source *s = &sources[i];
      Str dir = s->post ? str_lit("posts") : dir_of(s->path);
      void **slot = smap_slot(&taken_dirs, dir, false);
      SMap *set;
      if (!slot) {
        set = xmalloc(sizeof(SMap));
        smap_init(set, 32);
        if (dir.n == 0) {
          smap_put(set, str_lit("posts"), (void *)1);
          smap_put(set, str_lit("tags"), (void *)1);
          smap_put(set, str_lit("page"), (void *)1);
        }
        smap_put(&taken_dirs, dir, set);
      } else set = *(SMap **)slot;
      Val *slug_v = val_obj_get_lit(s->data, "slug");
      Str sl = slug_v ? slug_v->s : slugify(&slug_cache, a, val_obj_get_lit(s->data, "title")->s);
      s->slug = unique_slug(a, set, sl);
      StrBuf b; buf_init(&b);
      if (dir.n == 0) buf_adds(&b, s->slug);
      else { buf_adds(&b, dir); buf_add(&b, '/'); buf_adds(&b, s->slug); }
      s->out_dir = buf_str(a, &b);
      buf_clear(&b);
      buf_adds(&b, base); buf_add(&b, '/'); buf_adds(&b, s->out_dir); buf_add(&b, '/');
      s->url = buf_str(a, &b);
      free(b.p);
      smap_put(&by_source, s->path, s);
    }
  }

  // 4. Markdown, links resolved to pages and static files.
  StrVec broken; memset(&broken, 0, sizeof(broken));
  SMap broken_set; smap_init(&broken_set, 64);
  {
    SMap static_paths; smap_init(&static_paths, 32);
    for (size_t i = 0; i < nstat; i++) smap_put(&static_paths, statics[i].path, (void *)1);
    for (size_t i = 0; i < nsrc; i++) {
      Source *s = &sources[i];
      LinkEnv env;
      env.by_source = &by_source;
      env.static_paths = &static_paths;
      env.base = base;
      env.dir = dir_of(s->path);
      env.src_path = s->path;
      env.broken = &broken;
      env.broken_set = &broken_set;
      MdResult md = render_markdown(a, &slug_cache, s->body, &env);
      s->html = md.html;
      s->toc = md.toc;
      s->ntoc = md.ntoc;
      s->excerpt = md.excerpt;
    }
  }

  // 5. Collections: posts newest first, equal dates by slug.
  Source **posts = arena_alloc(a, (nsrc ? nsrc : 1) * sizeof(Source *));
  size_t nposts = 0;
  for (size_t i = 0; i < nsrc; i++) if (sources[i].post) posts[nposts++] = &sources[i];
  qsort(posts, nposts, sizeof(Source *), cmp_post_qsort);
  for (size_t k = 0; k < nposts; k++) {
    posts[k]->newer = k == 0 ? NULL : posts[k - 1];
    posts[k]->older = k == nposts - 1 ? NULL : posts[k + 1];
  }

  // 6. Tags, grouped by slug, ordered by count then slug.
  SMap by_slug; smap_init(&by_slug, 128);
  Tag **all_tags = arena_alloc(a, (nposts ? nposts : 1) * sizeof(Tag *));
  size_t nall_tags = 0;
  for (size_t k = 0; k < nposts; k++) {
    Source *p = posts[k];
    NameSlug ns[16];
    size_t nns = own_tags(a, &slug_cache, p, ns, 16);
    p->tag_refs = arena_alloc(a, (nns ? nns : 1) * sizeof(Tag *));
    p->ntag_refs = 0;
    for (size_t q = 0; q < nns; q++) {
      void **slot = smap_slot(&by_slug, ns[q].sl, false);
      Tag *tag;
      if (!slot) {
        tag = arena_alloc(a, sizeof(Tag));
        memset(tag, 0, sizeof(Tag));
        tag->name = ns[q].name;
        tag->slug = ns[q].sl;
        StrBuf b; buf_init(&b);
        buf_adds(&b, base); buf_addc(&b, "/tags/"); buf_adds(&b, ns[q].sl); buf_add(&b, '/');
        tag->url = buf_str(a, &b);
        free(b.p);
        smap_put(&by_slug, ns[q].sl, tag);
        all_tags[nall_tags++] = tag;
      } else tag = *(Tag **)slot;
      tag_add_post(a, tag, p);
      p->tag_refs[p->ntag_refs++] = tag;
    }
  }
  for (size_t i = 0; i < nsrc; i++) {
    Source *s = &sources[i];
    if (s->post) continue;
    NameSlug ns[16];
    size_t nns = own_tags(a, &slug_cache, s, ns, 16);
    s->tag_refs = arena_alloc(a, (nns ? nns : 1) * sizeof(Tag *));
    s->ntag_refs = 0;
    for (size_t q = 0; q < nns; q++) {
      void **slot = smap_slot(&by_slug, ns[q].sl, false);
      Tag *tag;
      if (slot) tag = *(Tag **)slot;
      else {
        tag = arena_alloc(a, sizeof(Tag));
        memset(tag, 0, sizeof(Tag));
        tag->name = ns[q].name;
        tag->slug = ns[q].sl;
        tag->url = str_empty();
      }
      s->tag_refs[s->ntag_refs++] = tag;
    }
  }
  qsort(all_tags, nall_tags, sizeof(Tag *), cmp_tag_qsort);

  // 7. Layouts: every page, the index pages, the tag pages, then statics.
  {
    SMap uses_map; smap_init(&uses_map, 64);
    UsesCtx uctx; uctx.templates = &templates; uctx.memo = &uses_map; uctx.sp = 0;
    for (size_t i = 0; i < ntnames; i++) uses_of(a, &uctx, tnames[i]);
    SMap inc_cache; smap_init(&inc_cache, 1024);
    PMap obj_ids; pmap_init(&obj_ids, 256);
    TplCtx ctx;
    ctx.templates = &templates;
    ctx.uses = &uses_map;
    ctx.cache = &inc_cache;
    ctx.obj_ids = &obj_ids;
    ctx.next_id = 0;
    ctx.a = a;
    // Compile every parsed template to flat ops once per case.
    SMap tpl_ops; smap_init(&tpl_ops, 64);
    for (size_t i = 0; i < ntnames; i++) {
      void **slot = smap_slot(&templates, tnames[i], false);
      if (!slot) continue;
      NodeVec *nv = *(NodeVec **)slot;
      OpVec *ov = arena_alloc(a, sizeof(OpVec));
      memset(ov, 0, sizeof(OpVec));
      compile_nodes(a, nv, ov);
      smap_put(&tpl_ops, tnames[i], ov);
    }
    ctx.tpl_ops = &tpl_ops;
    Val *site_v = val_obj(a);
    val_obj_set(a, site_v, str_lit("title"), val_str(a, val_obj_get_lit(site, "title")->s));
    val_obj_set(a, site_v, str_lit("base_url"), val_str(a, base));
    Val *all_tags_v = val_arr(a);
    for (size_t i = 0; i < nall_tags; i++) {
      Val *tv = val_obj(a);
      val_obj_set(a, tv, str_lit("name"), val_str(a, all_tags[i]->name));
      val_obj_set(a, tv, str_lit("slug"), val_str(a, all_tags[i]->slug));
      val_obj_set(a, tv, str_lit("url"), val_str(a, all_tags[i]->url));
      val_obj_set(a, tv, str_lit("count"), val_num(a, (double)all_tags[i]->nposts));
      arr_push_val(a, all_tags_v, tv);
    }
    Val *recent_v = val_arr(a);
    long nrecent = (long)val_obj_get_lit(site, "recent")->num;
    for (long k = 0; k < nrecent && k < (long)nposts; k++)
      arr_push_val(a, recent_v, card_of(a, &slug_cache, &date_cache, base, posts[k]));
    for (size_t i = 0; i < nsrc; i++) {
      Source *s = &sources[i];
      Val *vars = val_obj(a);
      val_obj_set(a, vars, str_lit("site"), site_v);
      val_obj_set(a, vars, str_lit("tags"), all_tags_v);
      val_obj_set(a, vars, str_lit("recent"), recent_v);
      val_obj_set(a, vars, str_lit("page"), page_of(a, &slug_cache, &date_cache, base, s));
      Str html;
      if (!render_tpl(a, &ctx, s->layout, vars, &html)) die("page layout vanished");
      StrBuf pb; buf_init(&pb);
      buf_adds(&pb, s->out_dir); buf_addc(&pb, "/index.html");
      out_set(a, output, buf_str(a, &pb), html);
      free(pb.p);
    }
    long per = (long)val_obj_get_lit(site, "per_page")->num;
    if (per < 1) per = 1;
    long npages = (long)nposts / per + ((long)nposts % per ? 1 : 0);
    if (npages < 1) npages = 1;
    for (long k = 1; k <= npages; k++) {
      Val *vars = val_obj(a);
      val_obj_set(a, vars, str_lit("site"), site_v);
      val_obj_set(a, vars, str_lit("tags"), all_tags_v);
      val_obj_set(a, vars, str_lit("recent"), recent_v);
      Val *plist = val_arr(a);
      long from = (k - 1) * per, to = k * per;
      if (to > (long)nposts) to = (long)nposts;
      for (long q = from; q < to; q++) arr_push_val(a, plist, card_of(a, &slug_cache, &date_cache, base, posts[q]));
      val_obj_set(a, vars, str_lit("posts"), plist);
      Val *pager = val_obj(a);
      val_obj_set(a, pager, str_lit("number"), val_num(a, (double)k));
      val_obj_set(a, pager, str_lit("total"), val_num(a, (double)npages));
      StrBuf ub; buf_init(&ub);
      buf_adds(&ub, base); buf_add(&ub, '/');
      if (k != 1) { buf_addc(&ub, "page/"); num_into(&ub, k); buf_add(&ub, '/'); }
      Str url_k = buf_str(a, &ub);
      buf_clear(&ub);
      Str newer_url = str_empty(), older_url = str_empty();
      if (k != 1) { buf_adds(&ub, base); buf_add(&ub, '/'); if (k - 1 != 1) { buf_addc(&ub, "page/"); num_into(&ub, k - 1); buf_add(&ub, '/'); } newer_url = buf_str(a, &ub); buf_clear(&ub); }
      if (k != npages) { buf_adds(&ub, base); buf_add(&ub, '/'); buf_addc(&ub, "page/"); num_into(&ub, k + 1); buf_add(&ub, '/'); older_url = buf_str(a, &ub); }
      free(ub.p);
      val_obj_set(a, pager, str_lit("url"), val_str(a, url_k));
      val_obj_set(a, pager, str_lit("newer_url"), val_str(a, newer_url));
      val_obj_set(a, pager, str_lit("older_url"), val_str(a, older_url));
      val_obj_set(a, vars, str_lit("pager"), pager);
      Str html;
      if (render_tpl(a, &ctx, str_lit("index"), vars, &html)) {
        Str path;
        if (k == 1) path = str_lit("index.html");
        else {
          StrBuf pb; buf_init(&pb);
          buf_addc(&pb, "page/"); num_into(&pb, k); buf_addc(&pb, "/index.html");
          path = buf_str(a, &pb);
          free(pb.p);
        }
        out_set(a, output, path, html);
      }
    }
    for (size_t i = 0; i < nall_tags; i++) {
      Tag *t = all_tags[i];
      Val *vars = val_obj(a);
      val_obj_set(a, vars, str_lit("site"), site_v);
      val_obj_set(a, vars, str_lit("tags"), all_tags_v);
      val_obj_set(a, vars, str_lit("recent"), recent_v);
      Val *tagv = val_obj(a);
      val_obj_set(a, tagv, str_lit("name"), val_str(a, t->name));
      val_obj_set(a, tagv, str_lit("slug"), val_str(a, t->slug));
      val_obj_set(a, tagv, str_lit("url"), val_str(a, t->url));
      val_obj_set(a, tagv, str_lit("count"), val_num(a, (double)t->nposts));
      Val *tposts = val_arr(a);
      for (size_t q = 0; q < t->nposts; q++) arr_push_val(a, tposts, card_of(a, &slug_cache, &date_cache, base, t->posts[q]));
      val_obj_set(a, tagv, str_lit("posts"), tposts);
      val_obj_set(a, vars, str_lit("tag"), tagv);
      Str html;
      if (render_tpl(a, &ctx, str_lit("tag"), vars, &html)) {
        StrBuf pb; buf_init(&pb);
        buf_addc(&pb, "tags/"); buf_adds(&pb, t->slug); buf_addc(&pb, "/index.html");
        out_set(a, output, buf_str(a, &pb), html);
        free(pb.p);
      }
    }
    {
      Val *vars = val_obj(a);
      val_obj_set(a, vars, str_lit("site"), site_v);
      val_obj_set(a, vars, str_lit("tags"), all_tags_v);
      val_obj_set(a, vars, str_lit("recent"), recent_v);
      Str html;
      if (render_tpl(a, &ctx, str_lit("tags"), vars, &html)) out_set(a, output, str_lit("tags/index.html"), html);
    }
    for (size_t i = 0; i < nstat; i++) {
      if (out_has(output, statics[i].path)) {
        buf_addc(&lb, "shadowed "); buf_adds(&lb, statics[i].path); buf_addc(&lb, ": a built page has its path");
        report_push(a, &report, &lb);
      } else out_set(a, output, statics[i].path, statics[i].content);
    }
  }

  // 8. Report: every line reported above and each broken link, sorted, then counts.
  {
    StrVec lines; memset(&lines, 0, sizeof(lines));
    for (size_t i = 0; i < report.n; i++) strvec_push(&lines, report.v[i]);
    for (size_t i = 0; i < broken.n; i++) strvec_push(&lines, broken.v[i]);
    qsort(lines.v, lines.n, sizeof(Str), cmp_str_qsort);
    StrBuf rb; buf_init(&rb);
    for (size_t i = 0; i < lines.n; i++) { buf_adds(&rb, lines.v[i]); buf_add(&rb, '\n'); }
    buf_addc(&rb, "built ");
    num_into(&rb, (long)(nsrc - nposts));
    buf_addc(&rb, " pages, ");
    num_into(&rb, (long)nposts);
    buf_addc(&rb, " posts, ");
    num_into(&rb, (long)nall_tags);
    buf_addc(&rb, " tags and ");
    num_into(&rb, (long)nstat);
    buf_addc(&rb, " static files\n");
    out_set(a, output, str_lit("build-report.txt"), buf_str(a, &rb));
    free(rb.p);
    free(lines.v);
    free(report.v);
    free(broken.v);
  }
  free(lb.p);
  return output;
}

// ---------------------------------------------------------------- main
static char *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) die("cannot open input file");
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = xmalloc(sz > 0 ? (size_t)sz : 1);
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) die("cannot read input file");
  fclose(f);
  *n = (size_t)sz;
  return buf;
}
int main(int argc, char **argv) {
  if (argc != 4) { fprintf(stderr, "usage: engine <cases.json> <answers.json> <nfkd.bin>\n"); return 1; }
  arena_init(&perm_arena);
  arena_init(&case_arena);
  nfkd_load(argv[3]);
  size_t n;
  char *raw = read_file(argv[1], &n);
  Val *cases = json_parse(&perm_arena, raw, n);
  if (!cases || cases->t != VARR) die("cases.json is not an array");
  ByteBuf out; bbuf_init(&out);
  bbuf_reserve(&out, n * 3); // answers run ~2.5x the input size; reserve once, no growth memmoves
  bbuf_addc(&out, '[');
  for (size_t ci = 0; ci < cases->arr.n; ci++) {
    arena_reset(&case_arena);
    Val *c = cases->arr.items[ci];
    Val *input = val_obj_get_lit(c, "input");
    if (!input) die("case has no input");
    OutVec *ans = build_case(&case_arena, input);
    if (ci) bbuf_addc(&out, ',');
    bbuf_addc(&out, '{');
    for (size_t i = 0; i < ans->n; i++) {
      if (i) bbuf_addc(&out, ',');
      json_escape_into(&out, ans->v[i].path);
      bbuf_addc(&out, ':');
      json_escape_into(&out, ans->v[i].content);
    }
    bbuf_addc(&out, '}');
  }
  bbuf_addc(&out, ']');
  FILE *f = fopen(argv[2], "wb");
  if (!f) die("cannot open output file");
  if (out.n && fwrite(out.p, 1, out.n, f) != out.n) die("cannot write output file");
  fclose(f);
  return 0;
}

// ---------------------------------------------------------------- front matter
static bool str_is_ascii(Str s, const char *c) { // exact match against an ASCII literal
  size_t n = strlen(c);
  if (s.n != n) return false;
  for (size_t i = 0; i < n; i++) if (s.s[i] != (unsigned char)c[i]) return false;
  return true;
}
static const char *FM_KEYS[] = { "title", "date", "tags", "draft", "layout", "slug", "summary" };
static bool fm_key_ok(Str k) {
  for (int i = 0; i < 7; i++) if (str_is_ascii(k, FM_KEYS[i])) return true;
  return false;
}
static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}
// stringOf(raw): {error} or {value}. Errors are bare (the caller prefixes).
typedef struct { bool error; Str errmsg; Str value; } StrResult;
static StrResult fm_string_of(Arena *a, Str raw) {
  StrResult r; r.error = false;
  if (raw.n > 0 && raw.s[0] == '"') {
    if (raw.n < 2 || raw.s[raw.n - 1] != '"') { r.error = true; r.errmsg = str_c(a, "a quoted string is not closed"); return r; }
    StrBuf out; buf_init(&out);
    for (size_t i = 1; i < raw.n - 1; i++) {
      uint32_t c = raw.s[i];
      if (c == '\\') {
        uint32_t n = raw.s[i + 1];
        if ((n != '"' && n != '\\') || i + 1 == raw.n - 1) {
          r.error = true; r.errmsg = str_c(a, "a quoted string holds an escape other than \\\" or \\\\");
          free(out.p); return r;
        }
        buf_add(&out, n);
        i++;
      } else if (c == '"') {
        r.error = true; r.errmsg = str_c(a, "a quoted string holds a stray quote");
        free(out.p); return r;
      } else buf_add(&out, c);
    }
    r.value = buf_str(a, &out);
    free(out.p);
    return r;
  }
  if (raw.n > 0 && raw.s[0] == '[') { r.error = true; r.errmsg = str_c(a, "a bare string opens on ["); return r; }
  r.value = raw;
  return r;
}
typedef struct { bool error; Str errmsg; Val *value; } ValResult;
static ValResult fm_value_of(Arena *a, Str key, Str raw) {
  ValResult r; r.error = false; r.value = NULL;
  char kb[16];
  size_t kn = key.n < 15 ? key.n : 15;
  for (size_t i = 0; i < kn; i++) kb[i] = (char)key.s[i];
  kb[kn] = 0;
  if (strcmp(kb, "title") == 0 || strcmp(kb, "summary") == 0) {
    StrResult s = fm_string_of(a, raw);
    if (s.error) {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "key \""); buf_adds(&b, key); buf_addc(&b, "\": "); buf_adds(&b, s.errmsg);
      r.error = true; r.errmsg = err_fmt(a, &b); return r;
    }
    r.value = val_str(a, s.value);
    return r;
  }
  if (strcmp(kb, "date") == 0) {
    bool ok = raw.n == 10 && raw.s[4] == '-' && raw.s[7] == '-';
    int y = 0, mo = 0, d = 0;
    if (ok) {
      for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && (raw.s[i] < '0' || raw.s[i] > '9')) ok = false;
      if (ok) { y = (raw.s[0]-'0')*1000 + (raw.s[1]-'0')*100 + (raw.s[2]-'0')*10 + (raw.s[3]-'0'); mo = (raw.s[5]-'0')*10 + (raw.s[6]-'0'); d = (raw.s[8]-'0')*10 + (raw.s[9]-'0'); }
    }
    if (!ok || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in(y, mo)) {
      r.error = true; r.errmsg = str_c(a, "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return r;
    }
    r.value = val_str(a, raw);
    return r;
  }
  if (strcmp(kb, "tags") == 0) {
    if (!(raw.n >= 2 && raw.s[0] == '[' && raw.s[raw.n - 1] == ']')) {
      r.error = true; r.errmsg = str_c(a, "key \"tags\": not a list [a, b]");
      return r;
    }
    Str inner = str_slice(raw, 1, raw.n - 1);
    Val *arr = val_arr(a);
    if (trim_st(inner).n != 0) {
      size_t start = 0;
      bool bad = false;
      for (size_t i = 0; i <= inner.n; i++) {
        if (i == inner.n || inner.s[i] == ',') {
          Str t = trim_st(str_slice(inner, start, i));
          if (t.n == 0) { bad = true; break; }
          bool tok = true;
          for (size_t k = 0; k < t.n; k++) if (t.s[k] == '"' || t.s[k] == '[' || t.s[k] == ']') tok = false;
          if (!tok) { bad = true; break; }
          if (arr->arr.n == arr->arr.cap) {
            size_t nc = arr->arr.cap ? arr->arr.cap * 2 : 8;
            Val **ni = arena_alloc(a, nc * sizeof(Val *));
            if (arr->arr.n) memcpy(ni, arr->arr.items, arr->arr.n * sizeof(Val *));
            arr->arr.items = ni; arr->arr.cap = nc;
          }
          arr->arr.items[arr->arr.n++] = val_str(a, t);
          start = i + 1;
        }
      }
      if (bad) { r.error = true; r.errmsg = str_c(a, "key \"tags\": an empty tag, or one holding \" [ or ]"); return r; }
    }
    r.value = arr;
    return r;
  }
  if (strcmp(kb, "draft") == 0) {
    if (str_is_ascii(raw, "true")) { r.value = val_bool(a, true); return r; }
    if (str_is_ascii(raw, "false")) { r.value = val_bool(a, false); return r; }
    r.error = true; r.errmsg = str_c(a, "key \"draft\": true or false");
    return r;
  }
  if (strcmp(kb, "layout") == 0) {
    bool ok = raw.n > 0;
    for (size_t i = 0; i < raw.n; i++) { uint32_t c = raw.s[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = false; }
    if (!ok) { r.error = true; r.errmsg = str_c(a, "key \"layout\": not a template name"); return r; }
    r.value = val_str(a, raw);
    return r;
  }
  if (strcmp(kb, "slug") == 0) {
    bool ok = raw.n > 0 && raw.n <= 64;
    // /^[a-z0-9]+(?:-[a-z0-9]+)*$/
    if (ok) {
      bool needAlnum = true;
      for (size_t i = 0; i < raw.n; i++) {
        uint32_t c = raw.s[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (needAlnum) { if (!alnum) { ok = false; break; } needAlnum = false; }
        else if (c == '-') needAlnum = true;
        else if (!alnum) { ok = false; break; }
      }
      if (needAlnum) ok = false; // ends with '-'
    }
    if (!ok) { r.error = true; r.errmsg = str_c(a, "key \"slug\": not a slug"); return r; }
    r.value = val_str(a, raw);
    return r;
  }
  { // unknown key (unreachable: the caller checks KEYS first)
    StrBuf b; buf_init(&b);
    buf_addc(&b, "unknown key \""); buf_adds(&b, key); buf_addc(&b, "\"");
    r.error = true; r.errmsg = err_fmt(a, &b); return r;
  }
}
static FMResult parse_front_matter(Arena *a, Str text) {
  FMResult r; r.error = false;
  long nl = find_char(text, '\n', 0);
  Str first = nl == -1 ? text : str_slice(text, 0, (size_t)nl);
  Str dash = str_lit("---");
  if (!str_eq(first, dash)) { r.error = true; r.errmsg = str_c(a, "no front matter"); return r; }
  long closeIdx = -1, closeEnd = -1;
  size_t search = nl == -1 ? 0 : (size_t)nl;
  Str needle = str_lit("\n---");
  while (1) {
    long idx = find_str(text, needle, search);
    if (idx == -1) break;
    long after = idx + 4;
    if (after == (long)text.n || text.s[after] == '\n') { closeIdx = idx; closeEnd = after; break; }
    search = (size_t)idx + 1;
  }
  if (closeIdx == -1) { r.error = true; r.errmsg = str_c(a, "the front matter is not closed"); return r; }
  Str fmText = str_slice(text, nl == -1 ? 0 : (size_t)nl + 1, (size_t)closeIdx);
  StrList fmLines;
  if (fmText.n == 0) { fmLines.items = NULL; fmLines.n = 0; }
  else fmLines = split_lines(a, fmText);
  Val *data = val_obj(a);
  for (size_t j = 0; j < fmLines.n; j++) {
    Str line = fmLines.items[j];
    if (is_blank_line(line) || (line.n > 0 && line.s[0] == '#')) continue;
    // LINE = /^([a-z_]+):(?: (.*))?$/
    size_t i = 0;
    while (i < line.n && ((line.s[i] >= 'a' && line.s[i] <= 'z') || line.s[i] == '_')) i++;
    if (i == 0 || i >= line.n || line.s[i] != ':') {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "line "); num_into(&b, (long)j + 2); buf_addc(&b, " is not \"key: value\"");
      r.error = true; r.errmsg = err_fmt(a, &b); return r;
    }
    Str key = str_slice(line, 0, i);
    Str raw;
    if (i + 1 == line.n) raw = str_empty();
    else if (line.s[i + 1] == ' ') raw = str_slice(line, i + 2, line.n);
    else {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "line "); num_into(&b, (long)j + 2); buf_addc(&b, " is not \"key: value\"");
      r.error = true; r.errmsg = err_fmt(a, &b); return r;
    }
    raw = trim_end_st(raw);
    if (!fm_key_ok(key)) {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "unknown key \""); buf_adds(&b, key); buf_addc(&b, "\"");
      r.error = true; r.errmsg = err_fmt(a, &b); return r;
    }
    if (val_has(data, key)) {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "key \""); buf_adds(&b, key); buf_addc(&b, "\" is given twice");
      r.error = true; r.errmsg = err_fmt(a, &b); return r;
    }
    if (raw.n == 0) {
      StrBuf b; buf_init(&b);
      buf_addc(&b, "key \""); buf_adds(&b, key); buf_addc(&b, "\" has no value");
      r.error = true; r.errmsg = err_fmt(a, &b); return r;
    }
    ValResult v = fm_value_of(a, key, raw);
    if (v.error) { r.error = true; r.errmsg = v.errmsg; return r; }
    val_obj_set(a, data, key, v.value);
  }
  if (!val_has(data, str_lit("title"))) { r.error = true; r.errmsg = str_c(a, "no title"); return r; }
  Str body = (closeEnd < (long)text.n && text.s[closeEnd] == '\n') ? str_slice(text, (size_t)closeEnd + 1, text.n) : str_empty();
  r.data = data;
  r.body = body;
  return r;
}
