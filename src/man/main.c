#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <notcurses/notcurses.h>
#include "builddef.h"

static void
usage(const char* argv0, FILE* o){
  fprintf(o, "usage: %s [ -hV ] files\n", argv0);
  fprintf(o, " -h: print help and return success\n");
  fprintf(o, " -v: print version and return success\n");
}

static int
parse_args(int argc, char** argv){
  const char* argv0 = *argv;
  int longindex;
  int c;
  struct option longopts[] = {
    { .name = "help", .has_arg = 0, .flag = NULL, .val = 'h', },
    { .name = NULL, .has_arg = 0, .flag = NULL, .val = 0, }
  };
  while((c = getopt_long(argc, argv, "hV", longopts, &longindex)) != -1){
    switch(c){
      case 'h': usage(argv0, stdout);
                exit(EXIT_SUCCESS);
                break;
      case 'V': fprintf(stderr, "ncman version %s\n", notcurses_version());
                exit(EXIT_SUCCESS);
                break;
      default: usage(argv0, stderr);
               return -1;
               break;
    }
  }
  if(argv[optind] == NULL){
    usage(argv0, stderr);
    return -1;
  }
  return optind;
}

#ifdef USE_DEFLATE // libdeflate implementation
#include <libdeflate.h>
// assume that |buf| is |*len| bytes of deflated data, and try to inflate
// it. if successful, the inflated map will be returned. either way, the
// input map will be unmapped (we take ownership). |*len| will be updated
// if an inflated map is successfully returned.
static unsigned char*
map_gzipped_data(unsigned char* buf, size_t* len, unsigned char* ubuf, uint32_t ulen){
  struct libdeflate_decompressor* inflate = libdeflate_alloc_decompressor();
  if(inflate == NULL){
    munmap(buf, *len);
    return NULL;
  }
  size_t outbytes;
  enum libdeflate_result r;
  r = libdeflate_gzip_decompress(inflate, buf, *len, ubuf, ulen, &outbytes);
  munmap(buf, *len);
  libdeflate_free_decompressor(inflate);
  if(r != LIBDEFLATE_SUCCESS){
    return NULL;
  }
  *len = ulen;
  return ubuf;
}
#else // libz implementation
#include <zlib.h>
static unsigned char*
map_gzipped_data(unsigned char* buf, size_t* len, unsigned char* ubuf, uint32_t ulen){
  z_stream z = {
    .zalloc = Z_NULL,
    .zfree = Z_NULL,
    .opaque = Z_NULL,
    .next_in = buf,
    .avail_in = *len,
    .next_out = ubuf,
    .avail_out = ulen,
  };
  int r = inflateInit(&z);
  if(r != Z_OK){
    munmap(buf, *len);
    return NULL;
  }
  r = inflate(&z, Z_FINISH);
  munmap(buf, *len);
  if(r != Z_STREAM_END){
    inflateEnd(&z);
    return NULL;
  }
  inflateEnd(&z);
  munmap(buf, *len);
  return NULL;
}
#endif

static unsigned char*
map_troff_data(int fd, size_t* len){
  struct stat sbuf;
  if(fstat(fd, &sbuf)){
    return NULL;
  }
  // gzip has a 10-byte mandatory header and an 8-byte mandatory footer
  if(sbuf.st_size < 18){
    return NULL;
  }
  *len = sbuf.st_size;
  unsigned char* buf = mmap(NULL, *len, PROT_READ,
#ifdef MAP_POPULATE
                            MAP_POPULATE |
#endif
                            MAP_PRIVATE, fd, 0);
  if(buf == MAP_FAILED){
    return NULL;
  }
  if(buf[0] == 0x1f && buf[1] == 0x8b && buf[2] == 0x08){
    // the last four bytes have the uncompressed length
    uint32_t ulen;
    memcpy(&ulen, buf + *len - 4, 4);
    size_t pgsize = 4096; // FIXME
    void* ubuf = mmap(NULL, (ulen + pgsize - 1) / pgsize * pgsize,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if(ubuf == MAP_FAILED){
      munmap(buf, *len);
      return NULL;
    }
    if(map_gzipped_data(buf, len, ubuf, ulen) == NULL){
      munmap(ubuf, ulen);
      return NULL;
    }
    return ubuf;
  }
  return buf;
}

// find the man page, and inflate it if deflated
static unsigned char*
get_troff_data(const char *arg, size_t* len){
  // FIXME we'll want to use the mandb. for now, require a full path.
  int fd = open(arg, O_RDONLY | O_CLOEXEC);
  if(fd < 0){
    return NULL;
  }
  unsigned char* buf = map_troff_data(fd, len);
  close(fd);
  return buf;
}

typedef enum {
  LINE_UNKNOWN,
  LINE_COMMENT,
  LINE_B, LINE_BI, LINE_BR, LINE_I, LINE_IB, LINE_IR,
  LINE_RB, LINE_RI, LINE_SB, LINE_SM,
  LINE_EE, LINE_EX, LINE_RE, LINE_RS,
  LINE_SH, LINE_SS, LINE_TH,
  LINE_IP, LINE_LP, LINE_P, LINE_PP,
  LINE_TP, LINE_TQ,
  LINE_ME, LINE_MT, LINE_UE, LINE_UR,
  LINE_OP, LINE_SY, LINE_YS,
} ltypes;

typedef enum {
  TROFF_UNKNOWN,
  TROFF_COMMENT,
  TROFF_FONT,
  TROFF_STRUCTURE,
  TROFF_PARAGRAPH,
  TROFF_HYPERLINK,
  TROFF_SYNOPSIS
} ttypes;

typedef struct {
  ltypes ltype;
  const char* symbol;
  ttypes ttype;
} trofftype;

// all troff types start with a period, followed by one or two ASCII
// characters.
static const trofftype trofftypes[] = {
  { .ltype = LINE_UNKNOWN, .symbol = "", .ttype = TROFF_UNKNOWN, },
  { .ltype = LINE_COMMENT, .symbol = "\\\"", .ttype = TROFF_COMMENT, },
#define TROFF_FONT(x) { .ltype = LINE_##x, .symbol = #x, .ttype = TROFF_FONT, },
  TROFF_FONT(B) TROFF_FONT(BI) TROFF_FONT(BR)
  TROFF_FONT(I) TROFF_FONT(IB) TROFF_FONT(IR)
#undef TROFF_FONT
#define TROFF_STRUCTURE(x) { .ltype = LINE_##x, .symbol = #x, .ttype = TROFF_STRUCTURE, },
  TROFF_STRUCTURE(EE) TROFF_STRUCTURE(EX) TROFF_STRUCTURE(RE) TROFF_STRUCTURE(RS)
  TROFF_STRUCTURE(SH) TROFF_STRUCTURE(SS) TROFF_STRUCTURE(TH)
#undef TROFF_STRUCTURE
#define TROFF_PARA(x) { .ltype = LINE_##x, .symbol = #x, .ttype = TROFF_PARAGRAPH, },
  TROFF_PARA(IP) TROFF_PARA(LP) TROFF_PARA(P)
  TROFF_PARA(PP) TROFF_PARA(TP) TROFF_PARA(TQ)
#undef TROFF_PARA
#define TROFF_HLINK(x) { .ltype = LINE_##x, .symbol = #x, .ttype = TROFF_HYPERLINK, },
  TROFF_HLINK(ME) TROFF_HLINK(MT) TROFF_HLINK(UE) TROFF_HLINK(UR)
#undef TROFF_HLINK
#define TROFF_SYNOPSIS(x) { .ltype = LINE_##x, .symbol = #x, .ttype = TROFF_SYNOPSIS, },
  TROFF_SYNOPSIS(OP) TROFF_SYNOPSIS(SY) TROFF_SYNOPSIS(YS)
#undef TROFF_SYNOPSIS
};

// the troff trie is only defined on the 128 ascii values.
struct troffnode {
  struct troffnode* next[0x80];
  const trofftype *ttype;
};

static void
destroy_trofftrie(struct troffnode* root){
  if(root){
    for(unsigned i = 0 ; i < sizeof(root->next) / sizeof(*root->next) ; ++i){
      destroy_trofftrie(root->next[i]);
    }
    free(root);
  }
}

// build a trie rooted at an implicit leading period.
static struct troffnode*
trofftrie(void){
  struct troffnode* root = malloc(sizeof(*root));
  if(root == NULL){
    return NULL;
  }
  memset(root, 0, sizeof(*root));
  for(size_t toff = 0 ; toff < sizeof(trofftypes) / sizeof(*trofftypes) ; ++toff){
    const trofftype* t = &trofftypes[toff];
    if(strlen(t->symbol) == 0){
      continue;
    }
    struct troffnode* n = root;
    for(const char* s = t->symbol ; *s ; ++s){
      if(*s < 0){ // illegal symbol
        fprintf(stderr, "illegal symbol: %s\n", t->symbol);
        goto err;
      }
      unsigned char us = *s;
      if(us > sizeof(root->next) / sizeof(*root->next)){ // illegal symbol
        fprintf(stderr, "illegal symbol: %s\n", t->symbol);
        goto err;
      }
      if(n->next[us] == NULL){
        if((n->next[us] = malloc(sizeof(*root))) == NULL){
          goto err;
        }
        memset(n->next[us], 0, sizeof(*root));
      }
      n = n->next[us];
    }
    if(n->ttype){ // duplicate command
      fprintf(stderr, "duplicate command: %s %s\n", t->symbol, n->ttype->symbol);
      goto err;
    }
    n->ttype = t;
  }
  return root;

err:
  destroy_trofftrie(root);
  return NULL;
}

// lex the troffnode out from |ws|, where the troffnode is all text prior to
// whitespace or a NUL. the byte following the troffnode is written back to
// |ws|. if it is a valid troff command sequence, the node is returned;
// NULL is otherwise returned. |len| ought be non-negative.
static const trofftype*
get_type(const struct troffnode* trie, const unsigned char** ws, size_t len){
  if(**ws != '.'){
    return NULL;
  }
  ++*ws;
  --len;
  while(len && !isspace(**ws) && **ws){
    if(**ws > sizeof(trie->next) / sizeof(*trie->next)){ // illegal command
      return NULL;
    }
    if((trie = trie->next[**ws]) == NULL){
      return NULL;
    }
    ++*ws;
    --len;
  }
  return trie->ttype;
}

typedef struct pagenode {
  char* text;
  enum {
    NODE_SECTION,
    NODE_SUBSECTION,
    NODE_PARAGRAPH,
  } level;
  struct pagenode* subs;
  unsigned subcount;
} pagenode;

typedef struct pagedom {
  struct pagenode* root;
  struct troffnode* trie;
  char* title;
  char* section;
  char* version;
} pagedom;

static const char*
dom_get_title(const pagedom* dom){
  return dom->title;
}

// take the newly-added title section, and extract the title, section, and
// version (technically footer-middle, footer-inside, and header-middle).
// they ought be quoted, but might not be.
static int
lex_title(pagedom* dom){
  const char* tok = dom->root->text;
  while(isspace(*tok)){
    ++tok;
  }
  bool quoted = false;
  if(*tok == '"'){
    quoted = true;
    ++tok;
  }
  if(!*tok){
    fprintf(stderr, "couldn't extract title [%s]\n", dom->root->text);
    return -1;
  }
  const char* endtok = tok + 1;
  while(*endtok){
    if(!quoted){
      if(isspace(*endtok)){
        break;
      }else if(*endtok == '"'){
        quoted = true;
        break;
      }
    }else{
      if(*endtok == '"'){
        quoted = false;
        break;
      }
    }
    ++endtok;
  }
  if(!*endtok){
    fprintf(stderr, "couldn't extract title [%s]\n", dom->root->text);
    return -1;
  }
  dom->title = strndup(tok, endtok - tok);
  tok = endtok + 1;
  if(!*tok){
    fprintf(stderr, "couldn't extract section [%s]\n", dom->root->text);
    return -1;
  }
  if(!quoted){
    while(isspace(*tok)){
      ++tok;
    }
    quoted = false;
    if(*tok == '"'){
      quoted = true;
      ++tok;
    }
    if(!*tok){
      fprintf(stderr, "couldn't extract section [%s]\n", dom->root->text);
      return -1;
    }
  }
  endtok = tok + 1;
  while(*endtok){
    if(!quoted){
      if(isspace(*endtok)){
        break;
      }else if(*endtok == '"'){
        quoted = true;
        break;
      }
    }else{
      if(*endtok == '"'){
        quoted = false;
        break;
      }
    }
    ++endtok;
  }
  if(!*endtok){
    fprintf(stderr, "couldn't extract section [%s]\n", dom->root->text);
    return -1;
  }
  dom->section = strndup(tok, endtok - tok);
  return 0;
}

// extract the page structure.
static int
troff_parse(const unsigned char* map, size_t mlen, pagedom* dom){
  const struct troffnode* trie = dom->trie;
  const unsigned char* line = map;
  for(size_t off = 0 ; off < mlen ; ++off){
    const unsigned char* ws = line;
    size_t left = mlen - off;
    const trofftype* node = get_type(trie, &ws, left);
    // find the end of this line
    const unsigned char* eol = ws;
    left -= (ws - line);
    while(left && *eol != '\n' && *eol){
      ++eol;
      --left;
    }
    // functional end of line--doesn't include possible newline
    const unsigned char* feol = eol;
    if(left && *eol == '\n'){
      --feol;
    }
    if(node){
      if(node->ltype == LINE_TH){
        if(dom_get_title(dom)){
          fprintf(stderr, "found a second title (was %s)\n", dom_get_title(dom));
          return -1;
        }
        if(ws == feol || ws + 1 == feol){
          fprintf(stderr, "bogus empty title\n");
          return -1;
        }
        if((dom->root = malloc(sizeof(*dom->root))) == NULL){
          return -1;
        }
        if((dom->root->text = strndup((const char*)ws + 1, feol - (ws + 1))) == NULL){
          return -1;
        }
        if(lex_title(dom)){
          return -1;
        }
      }
    }
    off += eol - line;
    line = eol + 1;
  }
  if(dom_get_title(dom) == NULL){
    fprintf(stderr, "no title found\n");
    return -1;
  }
  return 0;
}

static int
draw_content(struct ncplane* p){
  const pagedom* dom = ncplane_userptr(p);
  ncplane_printf_aligned(p, 0, NCALIGN_LEFT, "%s(%s)", dom->title, dom->section);
  ncplane_printf_aligned(p, 0, NCALIGN_RIGHT, "%s(%s)", dom->title, dom->section);
  return 0;
}

static int
resize_pman(struct ncplane* pman){
  unsigned dimy, dimx;
  ncplane_dim_yx(ncplane_parent_const(pman), &dimy, &dimx);
  ncplane_resize_simple(pman, dimy - 1, dimx);
  int r = draw_content(pman);
  ncplane_move_yx(pman, 0, 0);
  return r;
}

// we create a plane sized appropriately for the troff data. all we do
// after that is move the plane up and down.
static struct ncplane*
render_troff(struct notcurses* nc, const unsigned char* map, size_t mlen,
             pagedom* dom){
  unsigned dimy, dimx;
  struct ncplane* stdn = notcurses_stddim_yx(nc, &dimy, &dimx);
  // this is just an estimate
  if(troff_parse(map, mlen, dom)){
    return NULL;
  }
  // this is just an estimate
  struct ncplane_options popts = {
    .rows = dimy - 1,
    .cols = dimx,
    .userptr = dom,
    .resizecb = resize_pman,
  };
  struct ncplane* pman = ncplane_create(stdn, &popts);
  if(pman == NULL){
    return NULL;
  }
  if(draw_content(pman)){
    ncplane_destroy(pman);
    return NULL;
  }
  return pman;
}

static const char USAGE_TEXT[] = "(q)uit";

static int
draw_bar(struct ncplane* bar, pagedom* dom){
  ncplane_cursor_move_yx(bar, 0, 0);
  ncplane_set_styles(bar, NCSTYLE_BOLD);
  ncplane_putstr(bar, dom_get_title(dom));
  ncplane_set_styles(bar, NCSTYLE_NONE);
  ncplane_putchar(bar, '(');
  ncplane_set_styles(bar, NCSTYLE_BOLD);
  ncplane_putstr(bar, dom->section);
  ncplane_set_styles(bar, NCSTYLE_NONE);
  ncplane_putchar(bar, ')');
  ncplane_set_styles(bar, NCSTYLE_ITALIC);
  ncplane_putstr_aligned(bar, 0, NCALIGN_RIGHT, USAGE_TEXT);
  return 0;
}

static int
resize_bar(struct ncplane* bar){
  unsigned dimy, dimx;
  ncplane_dim_yx(ncplane_parent_const(bar), &dimy, &dimx);
  ncplane_resize_simple(bar, 1, dimx);
  int r = draw_bar(bar, ncplane_userptr(bar));
  ncplane_move_yx(bar, dimy - 1, 0);
  return r;
}

static void
domnode_destroy(pagenode* node){
  if(node){
    free(node->text);
    for(unsigned z = 0 ; z < node->subcount ; ++z){
      domnode_destroy(&node->subs[z]);
    }
    free(node->subs);
  }
}

static void
pagedom_destroy(pagedom* dom){
  destroy_trofftrie(dom->trie);
  domnode_destroy(dom->root);
  free(dom->root);
  free(dom->title);
  free(dom->version);
}

static struct ncplane*
create_bar(struct notcurses* nc, pagedom* dom){
  unsigned dimy, dimx;
  struct ncplane* stdn = notcurses_stddim_yx(nc, &dimy, &dimx);
  struct ncplane_options nopts = {
    .y = dimy - 1,
    .x = 0,
    .rows = 1,
    .cols = dimx,
    .resizecb = resize_bar,
    .userptr = dom,
  };
  struct ncplane* bar = ncplane_create(stdn, &nopts);
  if(bar == NULL){
    return NULL;
  }
  uint64_t barchan = NCCHANNELS_INITIALIZER(0, 0, 0, 0x26, 0x62, 0x41);
  ncplane_set_fg_rgb(bar, 0xffffff);
  if(ncplane_set_base(bar, " ", 0, barchan) != 1){
    ncplane_destroy(bar);
    return NULL;
  }
  if(draw_bar(bar, dom)){
    ncplane_destroy(bar);
    return NULL;
  }
  if(notcurses_render(nc)){
    ncplane_destroy(bar);
    return NULL;
  }
  return bar;
}

static int
manloop(struct notcurses* nc, const char* arg){
  int ret = -1;
  struct ncplane* page = NULL;
  struct ncplane* bar = NULL;
  pagedom dom = {};
  size_t len;
  unsigned char* buf = get_troff_data(arg, &len);
  if(buf == NULL){
    goto done;
  }
  dom.trie = trofftrie();
  if(dom.trie == NULL){
    goto done;
  }
  page = render_troff(nc, buf, len, &dom);
  if(page == NULL){
    goto done;
  }
  bar = create_bar(nc, &dom);
  if(bar == NULL){
    goto done;
  }
  uint32_t key;
  do{
    if(notcurses_render(nc)){
      goto done;
    }
    ncinput ni;
    key = notcurses_get(nc, NULL, &ni);
    switch(key){
      case 'L':
        if(ni.ctrl && !ni.alt){
          notcurses_refresh(nc, NULL, NULL);
        }
        break;
      case 'q':
        ret = 0;
        goto done;
    }
  }while(key != (uint32_t)-1);

done:
  if(page){
    ncplane_destroy(page);
  }
  ncplane_destroy(bar);
  if(buf){
    munmap(buf, len);
  }
  pagedom_destroy(&dom);
  return ret;
}

static int
ncman(struct notcurses* nc, const char* arg){
  int r = manloop(nc, arg);
  return r;
}

int main(int argc, char** argv){
  int nonopt = parse_args(argc, argv);
  if(nonopt <= 0){
    return EXIT_FAILURE;
  }
  struct notcurses_options nopts = {
  };
  struct notcurses* nc = notcurses_core_init(&nopts, NULL);
  if(nc == NULL){
    return EXIT_FAILURE;
  }
  bool success;
  for(int i = 0 ; i < argc - nonopt ; ++i){
    success = false;
    if(ncman(nc, argv[nonopt + i])){
      break;
    }
    success = true;
  }
  return notcurses_stop(nc) || !success ? EXIT_FAILURE : EXIT_SUCCESS;
}
