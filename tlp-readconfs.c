#define _POSIX_C_SOURCE 200809L
#define POSIXLY_CORRECT 1
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define EXIT_TLPCONF 5
#define EXIT_DEFCONF 6

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define xfree(v)                                                               \
  do {                                                                         \
    if (v) {                                                                   \
      free(v);                                                                 \
      (v) = NULL;                                                              \
    }                                                                          \
  } while (0)
#define DECIMAL_DIGITS_BOUND(t) (241 * sizeof(t) / 100 + 1)

char *outfile = NULL;
int notrace = false;
bool debug = false;
int cdiff = false;

typedef struct {
  char *first;
  char *second;
} StringPair;

typedef struct {
  size_t len;
  StringPair *data;
} StringPairSlice;

typedef struct {
  char *first;
  size_t second;
} StringSizePair;

typedef struct {
  size_t cap;
  size_t len;
  StringSizePair *data;
} StringSizeVec;

typedef struct {
  char *name, *value, *source, *def;
} VarEntry;

typedef struct {
  size_t cap;
  size_t len;
  VarEntry *data;
} VarEntryVec;

typedef struct {
  StringSizeVec names;
  VarEntryVec infos;
} Names;

StringPairSlice renames = {0, NULL};
StringPairSlice deprecated = {0, NULL};
Names names = {{0, 0, NULL}, {0, 0, NULL}};

static int parse_opts(int argc, char *const *argv);
static int parse_renfile(const char *);
static int parse_dprfile(const char *);
static int parse_configfile(int, const char *file, bool do_ren);
static int write_runconf(const char *file);

int main(int argc, char *const *argv) {
  if (parse_opts(argc, argv) == -1) {
    exit(1);
  }

  openlog("tlp", LOG_PID, LOG_USER);

  parse_renfile(CONF_REN);
  bool do_rename = renames.len != 0;
  parse_dprfile(CONF_DPR);
  if (parse_configfile(AT_FDCWD, CONF_DEF, false)) {
    exit(EXIT_DEFCONF);
  }

  {
    DIR *dir = opendir(CONF_DIR);
    if (dir) {
      struct dirent64 *entry;
      while ((entry = readdir64(dir))) {
        size_t len = strlen(entry->d_name);
        if (!strcmp(entry->d_name + len - 5, ".conf")) {
          parse_configfile(dirfd(dir), entry->d_name, do_rename);
        }
      }
      closedir(dir);
    }
  }

  if (parse_configfile(AT_FDCWD, CONF_USR, do_rename) &&
      parse_configfile(AT_FDCWD, CONF_OLD, do_rename)) {
    exit(EXIT_TLPCONF);
  }

  write_runconf(outfile);

  return 0;
}

static int parse_opts(int argc, char *const *argv) {
  static struct option lo[] = {
      {"cdiff", no_argument, &cdiff, 1},
      {"notrace", no_argument, &notrace, 1},
      {"outfile", required_argument, NULL, 0},
      {0, 0, 0, 0},
  };

  int c;

  while (1) {
    int option_index = 0;
    c = getopt_long(argc, argv, "cno:", lo, &option_index);

    if (c == -1) {
      break;
    }

    switch (c) {
    case 0:
      if (option_index == 2) {
        outfile = optarg;
      }
      break;
    case 'c':
      cdiff = 1;
      break;
    case 'n':
      notrace = 1;
      break;
    case 'o':
      outfile = optarg;
      break;
    case '?':
      return -1;
    default:
      eprintf("Invalid option -- '%c'\n", c);
      return -1;
    }
  }

  if (optind < argc) {
    eprintf("non-option ARGV-elements: ");
    while (optind < argc) {
      eprintf("%s ", argv[optind++]);
    }
    fputc('\n', stderr);
    fflush(stderr);
    return -1;
  }
  return 0;
}

static void printf_debug(const char *fmt, ...) {
  if (!notrace && debug) {
    va_list args;

    va_start(args, fmt);
    vsyslog(LOG_DEBUG, fmt, args);
    va_end(args);
  }
}

static void *xrealloc(void *orig, size_t size) {
  if (size == 0) {
    xfree(orig);
    return NULL;
  }
  if (orig) {
    orig = realloc(orig, size);
  } else {
    orig = malloc(size);
  }
  if (!orig) {
    perror(NULL);
    exit(1);
  }
  return orig;
}

static char *str_materialize(const char *str, size_t len) {
  if (!str) {
    char *res = xrealloc(NULL, 1);
    *res = 0;
    return res;
  }
  char *res = xrealloc(NULL, len + 1);
  res[len] = 0;
  memcpy(res, str, len);
  return res;
}

static char *xstrdup(const char *str) {
  char *res = strdup(str);
  if (!res) {
    perror(NULL);
    exit(1);
  }
  return res;
}

static char *rtrim(char *str, size_t *len) {
  for (char *p = str + *len - 1; *len; *len -= 1, --p) {
    if (!isspace(*p)) {
      return str;
    }
  }

  return NULL;
}

static char *ltrim(char *str, size_t *len) {
  for (char *p = str; *len; *len -= 1, ++p) {
    if (!isspace(*p)) {
      return p;
    }
  }
  return NULL;
}

static char *parse_name(char *str, size_t *len) {
  size_t l = 0;
  char *p = str;
  char *end = str + *len;
  while (p < end && ((*p >= 'A' && *p <= 'Z') || *p == '_')) {
    ++l;
    ++p;
  }
  if (!l) {
    *len = 0;
    return NULL;
  }
  while (p < end && (*p >= '0' && *p <= '9')) {
    ++l;
    ++p;
  }

  *len = l;
  return str;
}

static int string_pair_compare(StringPair *a, StringPair *b) {
  return strcmp(a->first, b->first);
}

static void string_pair_sort(StringPairSlice *slice) {
  if (slice->data) {
    qsort(slice->data, slice->len, sizeof(StringPair),
          (__compar_fn_t)string_pair_compare);
  }
}

static int parse_renfile(const char *file) {
  FILE *fp;
  char *line_buf = NULL;
  size_t cap = 0;
  ssize_t read;
  size_t r_cap = 0;

  fp = fopen(file, "r");
  if (fp == NULL) {
    return -1;
  }

  while ((read = getline(&line_buf, &cap, fp)) != -1) {
    size_t len = read;
    char *line = rtrim(line_buf, &len);

    size_t old_name_len = len;
    char *old_name = parse_name(line, &old_name_len);
    if (!old_name || old_name_len >= len) {
      continue;
    }
    line += old_name_len;
    len -= old_name_len;

    {
      size_t prev = len;
      line = ltrim(line, &len);
      if (prev == len || !line) {
        continue;
      }
    }

    size_t new_name_len = len;
    char *new_name = parse_name(line, &new_name_len);
    if (!new_name || new_name_len != len) {
      continue;
    }

    if (r_cap == renames.len) {
      r_cap = r_cap ? r_cap * 2 : 10;
      renames.data = xrealloc(renames.data, r_cap * sizeof(StringPair));
    }

    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    renames.data[renames.len].first = str_materialize(old_name, old_name_len);
    renames.data[renames.len++].second =
        str_materialize(new_name, new_name_len);
  }

  fclose(fp);
  xfree(line_buf);
  if (r_cap != renames.len) {
    renames.data = xrealloc(renames.data, r_cap * sizeof(StringPair));
  }
  string_pair_sort(&renames);
  return 0;
}

static int parse_dprfile(const char *file) {
  FILE *fp;
  char *line_buf = NULL;
  size_t cap = 0;
  ssize_t read;
  size_t d_cap = 0;

  fp = fopen(file, "r");
  if (fp == NULL) {
    return -1;
  }

  while ((read = getline(&line_buf, &cap, fp)) != -1) {
    size_t len = read;
    char *line = rtrim(line_buf, &len);

    size_t dpr_name_len = len;
    char *dpr_name = parse_name(line, &dpr_name_len);
    if (!dpr_name || dpr_name_len >= len) {
      continue;
    }
    line += dpr_name_len;
    len -= dpr_name_len;

    {
      size_t prev = len;
      line = ltrim(line, &len);
      if (prev == len || !line) {
        continue;
      }
    }

    if (!len || *line != '#') {
      continue;
    }
    ++line;
    --len;

    {
      size_t prev = len;
      line = ltrim(line, &len);
      if (prev == len || !line) {
        continue;
      }
    }

    if (!len) {
      line = NULL;
    }

    if (d_cap == deprecated.len) {
      if (d_cap) {
        d_cap *= 2;
      } else {
        d_cap = 10;
      }
      deprecated.data = xrealloc(deprecated.data, d_cap * sizeof(StringPair));
    }

    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    deprecated.data[deprecated.len].first =
        str_materialize(dpr_name, dpr_name_len);
    deprecated.data[deprecated.len++].second = str_materialize(line, len);
  }

  fclose(fp);
  xfree(line_buf);
  if (d_cap != renames.len) {
    deprecated.data = xrealloc(deprecated.data, d_cap * sizeof(StringPair));
  }
  string_pair_sort(&deprecated);
  return 0;
}

struct string_slice {
  size_t len;
  const char *str;
};

static int string_slice_cmp(const struct string_slice *k, const char *str) {
  int ret = strncmp(k->str, str, k->len);
  if (ret != 0) {
    return ret;
  }
  size_t len = strlen(str);
  return (len == k->len) ? 0 : -1;
}

static int string_pair_slice_search_cmp(struct string_slice *k,
                                        StringPair *pair) {
  return string_slice_cmp(k, pair->first);
}

static StringPair *string_pair_slice_search(StringPairSlice *slice, char *key,
                                            size_t len) {
  if (!key || !len || !slice->data) {
    return NULL;
  }
  struct string_slice s = {len, key};
  return bsearch(&s, slice->data, slice->len, sizeof(StringPair),
                 (__compar_fn_t)string_pair_slice_search_cmp);
}

static void key_rename(char **key, size_t *len) {
  if (!*key || !*len) {
    return;
  }

  StringPair *renamed = string_pair_slice_search(&renames, *key, *len);
  if (renamed) {
    *key = renamed->second;
    *len = strlen(*key);
  }
}

static char *parse_value(char *str, size_t *len) {
  size_t l = 0;
  char *p = str;
  char *end = str + *len;
  bool escaped = false;

  if (*len > 1 && *p == '"') {
    l++;
    p++;
    escaped = true;
  }

  while (p < end && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                     (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == ' ' ||
                     *p == '_' || *p == '.' || *p == ':')) {
    ++l;
    ++p;
  }

  if (escaped) {
    if (p >= end || *p != '"') {
      l = SIZE_MAX;
    } else {
      l++;
    }
  } else {
    str = rtrim(str, &l);
  }

  *len = l;
  return l ? str : NULL;
}

static bool binary_search(const void *key, const void *base, size_t nmemb,
                          size_t size, __compar_fn_t compar, size_t *index) {
  size_t len = nmemb;
  size_t left = 0;
  size_t right = len;
  while (left < right) {
    size_t mid = left + len / 2;
    int cmp = (*compar)(key, ((const char *)base) + (mid * size));
    if (cmp > 0) {
      left = mid + 1;
    } else if (cmp < 0) {
      right = mid;
    } else {
      *index = mid;
      return true;
    }
    len = right - left;
  }
  *index = left;
  return false;
}

static int string_size_pair_slice_search_cmp(const struct string_slice *k,
                                             const StringSizePair *pair) {
  return string_slice_cmp(k, pair->first);
}

static bool string_size_vec_insert(StringSizeVec *vec, const char *key,
                                   size_t len, StringSizePair **ret) {
  bool found;
  size_t index;
  {
    struct string_slice k = {len, key};
    found =
        binary_search(&k, vec->data, vec->len, sizeof(StringSizePair),
                      (__compar_fn_t)string_size_pair_slice_search_cmp, &index);
  }

  if (found) {
    *ret = vec->data + index;
    return true;
  }

  if (vec->cap == vec->len) {
    vec->cap = vec->cap ? vec->cap * 2 : 10;
    vec->data = xrealloc(vec->data, vec->cap * sizeof(StringSizePair));
  }

  for (StringSizePair *p = vec->data + vec->len, *end = &vec->data[index];
       p > end; --p) {
    *p = *(p - 1);
  }

  *ret = vec->data + index;
  (*ret)->first = str_materialize(key, len);
  vec->len++;
  return false;
}

static char *strnstr(const char *haystack, const char *needle, size_t len) {
  if (!needle || !haystack || !len) {
    return NULL;
  }

  size_t needle_len = strlen(needle);
  if (!needle_len) {
    return (char *)haystack;
  }

  if (needle_len > len) {
    return NULL;
  }

  for (const char *p = haystack, *end = p + len - needle_len + 1; p < end;
       ++p) {
    if (!strncmp(p, needle, needle_len)) {
      return (char *)p;
    }
  }

  return NULL;
}

#define LINE_SIZE                                                              \
  (DECIMAL_DIGITS_BOUND(size_t) > 4 ? DECIMAL_DIGITS_BOUND(size_t) : 4)
static void store_name_value_source(const char *key, size_t key_len,
                                    const char *value, size_t value_len,
                                    const char *source, size_t line,
                                    bool append, bool is_def) {

  if (!debug && !strncmp(key, "TLP_DEBUG", key_len)) {
    const char *p = value;
    size_t l = value_len;
    char *f;
    while ((f = strnstr(p, "cfg", l))) {
      bool pre_boundary = false;
      if (f == p) {
        pre_boundary = true;
      } else {
        char c = *(f - 1);
        pre_boundary = !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '_');
      }

      l -= f - p;
      l -= 3;
      p = f + 3;

      if (pre_boundary) {
        if (!l) {
          debug = true;
          break;
        } else {
          char c = *p;
          if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_')) {
            debug = true;
            break;
          }
        }
      }
    }
  }

  StringSizePair *key_pair;
  size_t source_len = strlen(source);
  if (string_size_vec_insert(&names.names, key, key_len, &key_pair)) {
    VarEntry *data = &names.infos.data[key_pair->second];
    if (append) {
      {
        size_t old_value_len = strlen(data->value);
        size_t new_len = old_value_len + 1 + value_len;
        data->value = xrealloc(data->value, new_len + 1);
        data->value[old_value_len] = ' ';
        memcpy(data->value + old_value_len + 1, value, value_len);
        data->value[new_len] = 0;
      }
      {
        size_t old_source_len = strlen(data->source);
        size_t new_cap = old_source_len + 3 + source_len + 2 + LINE_SIZE + 1;
        data->source = xrealloc(data->source, new_cap);
        data->source =
            xrealloc(data->source, sprintf(data->source + old_source_len,
                                           " & %s L%04zu", source, line) +
                                       old_source_len + 1);
      }
    } else {
      xfree(data->value);
      xfree(data->source);
      data->value = str_materialize(value, value_len);
      char *location = xrealloc(NULL, source_len + 2 + LINE_SIZE + 1);
      data->source =
          xrealloc(location, sprintf(location, "%s L%04zu", source, line) + 1);
    }

    printf_debug("tlp-readconfs.replace [%zu]: %s=\"%s\" %s\n",
                 key_pair->second, data->name, data->value, data->source);
  } else {
    if (names.infos.len == names.infos.cap) {
      names.infos.cap = names.infos.cap ? names.infos.cap * 2 : 10;
      names.infos.data =
          xrealloc(names.infos.data, names.infos.cap * sizeof(VarEntry));
    }
    key_pair->second = names.infos.len;

    char *location = xrealloc(NULL, source_len + 2 + LINE_SIZE + 1);
    location =
        xrealloc(location, sprintf(location, "%s L%04zu", source, line) + 1);
    char *val = str_materialize(value, value_len);
    char *def = is_def ? xstrdup(val) : NULL;
    VarEntry *data = names.infos.data + names.infos.len++;
    data->source = location;
    data->value = val;
    data->def = def;
    data->name = key_pair->first;

    printf_debug("tlp-readconfs.insert  [%zu]: %s=\"%s\" %s\n",
                 key_pair->second, data->name, data->value, data->source);
  }
}

static int parse_configfile(int dirfd, const char *file, bool do_ren) {
  char *source_alloc = NULL;
  const char *source;
  bool is_def;
  if (!strcmp(file, CONF_DEF)) {
    source_alloc = xstrdup(basename(file));
    source = source_alloc;
    is_def = true;
  } else {
    source = file;
    is_def = false;
  }

  FILE *fp;
  char *line_buf = NULL;
  size_t cap = 0;
  ssize_t read;

  {
    int fd = openat(dirfd, file, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
      xfree(source_alloc);
      return -1;
    }
    fp = fdopen(fd, "r");
    if (!fp) {
      xfree(source_alloc);
      return -1;
    }
  }

  size_t ln = 0;
  while ((read = getline(&line_buf, &cap, fp)) != -1) {
    size_t len = read;
    char *line = rtrim(line_buf, &len);
    ln++;

    size_t name_len = len;
    char *name = parse_name(line, &name_len);
    if (!name || name_len >= len) {
      continue;
    }
    line += name_len;
    len -= name_len;

    bool append;
    if (len > 0 && *line == '=') {
      append = false;
      len--;
      line++;
    } else if (len > 1 && *line == '+' && line[1] == '=') {
      append = true;
      len -= 2;
      line += 2;
    } else {
      continue;
    }

    size_t value_len = len;
    char *value = parse_value(line, &value_len);
    if (value_len == SIZE_MAX || value_len > len) {
      continue;
    }
    line += value_len;
    len -= value_len;
    if (value_len > 1 && *value == '"') {
      value++;
      value_len -= 2;
    }

    line = ltrim(line, &len);
    if (len && *line != '#') {
      continue;
    }

    if (do_ren) {
      key_rename(&name, &name_len);
    }

    store_name_value_source(name, name_len, value, value_len, source, ln,
                            append, is_def);
  }

  fclose(fp);
  xfree(line_buf);
  xfree(source_alloc);
  return 0;
}

static void fwrite_runconf(FILE *fp, bool verbose) {
  for (VarEntry *e = names.infos.data, *end = e + names.infos.len; e < end;
       ++e) {
    if (verbose) {
      if (!cdiff || !e->def || strcmp(e->value, e->def)) {
        fprintf(fp, "%s: %s=\"%s\"", e->source, e->name, e->value);
        StringPair *warning =
            string_pair_slice_search(&deprecated, e->name, strlen(e->name));
        if (warning) {
          fprintf(fp, " # ");
          if (warning->second) {
            fprintf(fp, "%s", warning->second);
          }
        }
        putc('\n', fp);
      }
    } else {
      fprintf(fp, "%s=\"%s\"\n", e->name, e->value);
    }
  }
  fflush(fp);
}

static int write_runconf(const char *file) {
  bool std = false;
  FILE *fp = NULL;

  if (file) {
    fp = fopen(file, "wa");
    if (!fp) {
      return -1;
    }
  } else {
    fp = stderr;
    std = true;
  }

  fwrite_runconf(fp, std);

  if (!std) {
    fclose(fp);
  }
  return 0;
}
