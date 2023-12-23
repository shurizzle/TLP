#define _POSIX_C_SOURCE 200809L
#define POSIXLY_CORRECT 1
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <libkmod.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ACPI_CALL_DEV "/proc/acpi/call"
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define xfree(v)                                                               \
  do {                                                                         \
    if (v) {                                                                   \
      free(v);                                                                 \
      (v) = NULL;                                                              \
    }                                                                          \
  } while (0)
const char *progname = NULL;
#define USAGE                                                                  \
  "Usage:\n"                                                                   \
  "  Show this message:\n"                                                     \
  "    %s [-h|--help]\n"                                                       \
  "\n"                                                                         \
  "  Get charge thresholds / inhibit charge / force discharge:\n"              \
  "    %s [-v] -g ST <bat{1,2}>\n"                                             \
  "    %s [-v] -g SP <bat{1,2}>\n"                                             \
  "    %s [-v] -g IC <bat{1,2,0}>\n"                                           \
  "    %s [-v] -g FD <bat{1,2}>\n"                                             \
  "\n"                                                                         \
  "  Set charge thresholds / inhibit charge / force discharge:\n"              \
  "    %s [-v] -s ST <bat{1,2,0}> <percent{0,1-99}>\n"                         \
  "    %s [-v] -s SP <bat{1,2,0}> <percent{0,1-99}>\n"                         \
  "    %s [-v] -s IC <bat{1,2,0}> <inhibit{1,0}> "                             \
  "[<min{0,1-720,65535}>]\n"                                                   \
  "    %s [-v] -s FD <bat{1,2}> <discharge{1,0}> [<acbreak{1,0}>]\n"           \
  "\n"                                                                         \
  "  Set peak shift state, which is mysterious and inhibits charge:\n"         \
  "    %s [-v] -s PS <inhibit{1,0}> [<min{0,1-1440,65535}>]\n"                 \
  "\n"                                                                         \
  "  Synonyms:\n"                                                              \
  "    ST -> st|startThreshold|start|--st|--startThreshold|--start\n"          \
  "    SP -> sp|stopThreshold|stop|--sp|--stopThreshold|--stop\n"              \
  "    IC -> ic|inhibitCharge|inhibit|--ic|--inhibitCharge|--inhibit\n"        \
  "    FD -> fd|forceDischarge|--fd|--forceDischarge\n"                        \
  "    PS -> ps|peakShiftState|--ps|--peakShiftState\n"                        \
  "\n"                                                                         \
  "  Options:\n"                                                               \
  "    -v           show ASL call and response\n"                              \
  "    <bat>        1 for main, 2 for secondary, 0 for either/both\n"          \
  "    <min>        number of minutes, or 0 for never, or 65535 for forever\n" \
  "    <percent>    0 for default, 1-99 for percentage\n"                      \
  "    <inhibit>    1 for inhibit charge, 0 for stop inhibiting charge\n"      \
  "    <discharge>  1 for force discharge, 0 for stop forcing discharge\n"     \
  "    <acbreak>    1 for stop forcing when AC is detached, 0 for do not\n"    \
  "    [] means optional: sets value to 0\n"
#define print_usage(file)                                                      \
  fprintf((file), USAGE, progname, progname, progname, progname, progname,     \
          progname, progname, progname, progname, progname)
#if defined(__GNUC__) && __GNUC__ > 2
#define NO_RETURN __attribute__((noreturn))
#else
#define NO_RETURN
#endif

void NO_RETURN die(const char *fmt, ...) {
  va_list args;

  va_start(args, fmt);
  vfprintf(stderr, fmt, args); // NOLINT(clang-analyzer-valist.Uninitialized)
  va_end(args);
  exit(1);
}
#define die_usage()                                                            \
  die(USAGE, progname, progname, progname, progname, progname, progname,       \
      progname, progname, progname, progname)

static int verbose = 0;

typedef enum {
  METHOD_ST,
  METHOD_SP,
  METHOD_IC,
  METHOD_FD,
  METHOD_PS,
} Method;
static const char *METHODS[] = {"ST", "SP", "IC", "FD", "PS", NULL};

static Method parse_method(const char *);
static char *get_method(Method, uint8_t);
static void set_method(Method, uint8_t, int, char *const *);
static void load_acpi_call(void);

int main(int argc, char *const *argv) {
  progname = *argv++;
  argc--;
  if (argc == 1 && (!strcmp(*argv, "-h") || !strcmp(*argv, "--help"))) {
    print_usage(stdout);
    exit(0);
  }

  if (argc > 0 && !strcmp(*argv, "-v")) {
    argv++;
    argc--;
    verbose = 1;
  }

  char cmd;
  {
    const char *cmd_arg = NULL;
    if (argc > 0) {
      argc--;
      cmd_arg = *argv++;
    }
    if (!(cmd_arg && *cmd_arg == '-' &&
          (cmd_arg[1] == 's' || cmd_arg[1] == 'g') && !cmd_arg[2])) {
      die_usage();
    }
    cmd = cmd_arg[1];
  }
  Method method;
  {
    const char *method_arg = NULL;
    if (argc > 0) {
      argc--;
      method_arg = *argv++;
    }
    method = parse_method(method_arg);
  }

  uint8_t bat;
  if (method == METHOD_PS) {
    bat = 0;
  } else {
    if (argc <= 0) {
      die("<bat> missing or incorrect\n");
    }
    argc--;
    const char *bat_arg = *argv++;
    switch (*bat_arg) {
    case '0':
      bat = 0;
      break;
    case '1':
      bat = 1;
      break;
    case '2':
      bat = 2;
      break;
    default:
      die("<bat> missing or incorrect\n");
    }
    if (bat_arg[1]) {
      die("<bat> missing or incorrect\n");
    }
  }

  if (cmd == 'g' && argc == 0) {
    char *res = get_method(method, bat);
    if (res) {
      puts(res);
      free(res);
    }
  } else if (cmd == 's') {
    set_method(method, bat, argc, argv);
  } else {
    die_usage();
  }

  return 0;
}

static void *xrealloc(void *orig, size_t size) {
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

static char *cat(const char *path) {
  errno = 0;
  size_t alloc = 0, len = 0;
  char *res = NULL;
  int fd = -1;

  fd = open(path, O_RDONLY);
  if (fd == -1) {
    goto cleanup;
  }

  alloc = 4096;
  res = malloc(alloc);
  if (!res) {
    goto cleanup;
  }
  while (1) {
    if (alloc == len) {
      alloc += 4096;
      res = realloc(res, alloc);
      if (!res) {
        goto cleanup;
      }
    }
    int ret = read(fd, res + len, alloc - len);
    if (ret == -1) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      xfree(res);
      goto cleanup;
    } else if (ret == 0) {
      break;
    } else {
      len += (size_t)((unsigned int)ret);
    }
  }

  while (len && isspace(res[len - 1])) {
    len--;
  }
  if (len) {
    if (alloc++ == len) {
      res = realloc(res, alloc);
    }
    if (res) {
      res[len++] = 0;
    }
    if (alloc != len) {
      res = realloc(res, len);
    }
  } else {
    xfree(res);
  }

cleanup:
  if (fd != -1) {
    close(fd);
  }
  return res;
}

static char *run_acpi_call(const char *call, const char *asl_base) {
  {
    int fd = -1;
    fd = open(ACPI_CALL_DEV, O_RDONLY);
    if (fd == -1) {
      load_acpi_call();
    }
    fd = open(ACPI_CALL_DEV, O_RDONLY);
    if (fd == -1) {
      die("Could not find " ACPI_CALL_DEV ". Is module acpi_call loaded?\n");
    }

    {
      char *buf = NULL;
      size_t len = strlen(call) + strlen(asl_base) + 2;
      buf = xrealloc(NULL, len + 1);
      sprintf(buf, "%s.%s\n", call, asl_base);

      char *p = buf;
      size_t l = len;
      while (l > 0) {
        ssize_t ret = write(fd, p, l);
        if (ret == -1) {
          if (errno == EINTR || errno == EAGAIN) {
            continue;
          }
          perror(NULL);
          free(buf);
          close(fd);
          exit(1);
        }
        size_t nbytes = ret;
        if (nbytes > len) {
          p += l;
          l = 0;
        } else {
          p += nbytes;
          l -= nbytes;
        }
      }
      free(buf);
    }
  }

  char *res = cat(ACPI_CALL_DEV);
  if (!res && !errno) {
    perror(NULL);
    exit(1);
  }
  return res;
}

static char *acpi_call(const char *msg) {
  static char *PROBES[] = {"/sys/class/power_supply/BAT0/device/path",
                           "/sys/class/power_supply/BAT1/device/path",
                           "/sys/class/power_supply/AC/device/path",
                           "/sys/class/power_supply/ADP0/device/path",
                           "/sys/class/power_supply/ADP1/device/path"};
  char *asl_base_alloc = NULL;
  char *asl_base = NULL;

  char *dmi_version = cat("/sys/class/dmi/id/product_version");
  if (dmi_version && !strncmp(dmi_version, "Thinkpad ", 9)) {
    char *tmp = dmi_version + 9;
    if (!strcmp(tmp, "S2")) {
      asl_base = "\\_SB.PCI0.LPCB.EC.HKEY";
    } else if (!strcmp(tmp, "13")) {
      asl_base = "\\_SB.PCI0.LPCB.EC.HKEY";
    } else if (!strcmp(tmp, "13 2nd Gen")) {
      asl_base = "\\_SB.PCI0.LPCB.EC.HKEY";
    } else if (!strcmp(tmp, "Edge E130")) {
      asl_base = "\\_SB.PCI0.LPCB.EC.HKEY";
    }
  }
  xfree(dmi_version);

  if (!asl_base) {
    for (char **p = PROBES, **end = PROBES + sizeof(PROBES) / sizeof(*PROBES);
         p < end; ++p) {
      asl_base_alloc = cat(*p);
      if (asl_base_alloc) {
        break;
      }
    }
    if (!asl_base_alloc) {
      die("No power supply device path to read ASL base from: "
          "/sys/class/power_supply/{BAT0,BAT1,AC,ADP0,ADP1}/device/path\n");
    }
    size_t len = strlen(asl_base_alloc);
    size_t alloc = len + 1;
    while (1) {
      char *dst = strstr(asl_base_alloc, "_.");
      if (!dst) {
        break;
      }
      char *src = dst + 1;
      for (--dst; dst >= asl_base_alloc && *dst == '_'; --dst)
        ;
      dst++;
      for (char *end = asl_base_alloc + len; src < end; ++dst, ++src) {
        *dst = *src;
      }
      len -= src - dst;
    }

    {
      char *p = asl_base_alloc + len - 1;
      for (; p >= asl_base_alloc && *p == '_'; --p)
        ;
      len = ++p - asl_base_alloc;
    }

    {
      char *last = asl_base_alloc + len - 1;
      char *p = last;
      for (; p >= asl_base_alloc && (*p == ',' || (*p >= 'A' && *p <= 'Z') ||
                                     (*p >= '0' && *p <= '9'));
           --p)
        ;
      if (p >= asl_base_alloc && p != last && *p++ == '.') {
        len = p - asl_base_alloc;
        if (alloc - len != 5) {
          asl_base_alloc = xrealloc(asl_base_alloc, len + 5);
        }
        memcpy(p, "HKEY", 5);
      }
    }
    asl_base = asl_base_alloc;
  }

  printf("%s\n", asl_base);
  char *val = run_acpi_call(msg, asl_base);

  if (val && strstr(val, "Error: AE_NOT_FOUND")) {
    die("Error: AE_NOT_FOUND for ASL base: %s\n%s\n", asl_base, val);
  }

  if (verbose) {
    printf("Call    : %s.%s\n", asl_base, msg);
    printf("Response: %s\n", val);
  }

  xfree(asl_base_alloc);
  return val;
}

static char *acpi_call_get(const char *method, uint32_t bits) {
  char *buf = xrealloc(NULL, strlen(method) + 12);
  sprintf(buf, "%s 0x%x", method, bits);
  char *val = acpi_call(buf);
  free(buf);

  if (val && !strcmp(val, "0x80000000")) {
    die("Call failure status returned: 0x80000000\n");
  }
  return val;
}

static int try_parse_status_hex(const char *resp, uint32_t *res) {
  *res = 0;
  if (!resp || *resp != '0' || (resp[1] != 'x' && resp[1] != 'X')) {
    return 0;
  }
  size_t len = 0;
  for (const char *p = resp + 2; *p; ++p) {
    if (len > 8) {
      return 0;
    }

    char c = *p;
    if (c >= '0' && c <= '9') {
      *res = *res * 16 + (c - '0');
    } else if (c >= 'A' && c <= 'F') {
      *res = *res * 16 + (c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      *res = *res * 16 + (c - 'a' + 10);
    } else {
      return 0;
    }

    len++;
  }
  if (len == 0) {
    return 0;
  }

  return 1;
}

static uint32_t parse_status_hex(const char *resp) {
  uint32_t res;
  if (!try_parse_status_hex(resp, &res)) {
    if (strstr(resp, "Error: AE_NOT_FOUND")) {
      die("ASL base not found for this machine\n"
          "  {perhaps it does not have the ThinkPad ACPI interface}\n"
          "ASL base parsed using: "
          "/sys/class/power_supply/{BAT0,BAT1,AC,ADP0,ADP1}/device/path\n");
    } else {
      die("Bad status returned: %s\n", resp);
    }
  }
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

static char *read_start_stop_charge_threshold(char *resp, const char *err) {
  uint32_t bits = parse_status_hex(resp);
  free(resp);
  uint32_t mask = 3 << 8;
  if ((bits & mask) != mask) {
    die(err);
  }
  uint8_t val = (uint8_t)(bits & ((1 << 8) - 1));
  char *suffix;
  if (val == 0) {
    return xstrdup("0 (default)");
  } else if (val < 100) {
    suffix = " (relative percent)";
  } else {
    suffix = " (unknown)";
  }

  char *res = xrealloc(NULL, strlen(suffix) + 4);
  res = xrealloc(res, sprintf(res, "%" PRIu8 "%s", val, suffix) + 1);
  return res;
}

static char *read_start_charge_threshold(char *resp) {
  return read_start_stop_charge_threshold(
      resp, "<start charge threshold unsupported>\n");
}

static char *read_stop_charge_threshold(char *resp) {
  return read_start_stop_charge_threshold(
      resp, "<stop charge threshold unsupported>\n");
}

static char *read_inhibit_charge(char *resp) {
  uint32_t bits = parse_status_hex(resp);
  free(resp);
  uint32_t mask = 1 << 5;
  if ((bits & mask) != mask) {
    die("<inhibit charge unsupported>\n");
  }
  if (bits & 1) {
    uint16_t min = (bits >> 8) & 0xffff;
    if (min == 0) {
      return xstrdup("yes (unspecified min)");
    } else if (min == 65535) {
      return xstrdup("yes (forever)");
    } else {
      char *res = xrealloc(NULL, 16);
      return xrealloc(res, sprintf(res, "yes (%" PRIu16 " min)", min) + 1);
    }
  } else {
    return xstrdup("no");
  }
}

char *read_force_discharge(char *resp) {
  uint32_t bits = parse_status_hex(resp);
  free(resp);
  uint32_t mask = 3 << 8;
  if ((bits & mask) != mask) {
    die("<force discharge unsupported>\n");
  }
  const char *val;
  if (bits & 1) {
    val = "yes";
  } else {
    val = "no";
  }
  if (bits & 2) {
    char *res = xrealloc(NULL, strlen(val) + 22);
    sprintf(res, "%s (break on AC detach)", val);
    return res;
  } else {
    return xstrdup(val);
  }
}

static char *get_method(Method method, uint8_t bat) {
  if (method == METHOD_PS) {
    die("Cannot read %s", METHODS[method]);
  }
  if (bat == 0 &&
      (method == METHOD_ST || method == METHOD_SP || method == METHOD_FD)) {
    die("Cannot specify 'either/both' for reading %s\n", METHODS[method]);
  }

  if (method == METHOD_ST) {
    return read_start_charge_threshold(acpi_call_get("GCTG", bat));
  } else if (method == METHOD_SP) {
    return read_stop_charge_threshold(acpi_call_get("BCSG", bat));
  } else if (method == METHOD_IC) {
    // this is actually reading peak shift state
    return read_inhibit_charge(acpi_call_get("PSSG", bat));
  } else if (method == METHOD_FD) {
    return read_force_discharge(acpi_call_get("BDSG", bat));
  } else {
    die_usage();
  }

  return NULL;
}

void acpi_call_set(const char *method, uint32_t value) {
  char *buf = xrealloc(NULL, strlen(method) + 12);
  sprintf(buf, "%s 0x%x", method, value);
  char *val = acpi_call(buf);
  free(buf);
  if (val && !strcmp(val, "0x80000000")) {
    die("Call failure status returned: %s\n", val);
  }
}

static void set_method(Method method, uint8_t bat_, int argc,
                       char *const *argv) {
  if (bat_ == 0 && method == METHOD_FD) {
    die("Cannot specify 'either/both' for writing %s\n", METHODS[method]);
  }
  uint32_t bat = bat_;
  const char *meth;
  uint32_t value;

  if (method == METHOD_IC || method == METHOD_PS) {
    uint32_t inhibit;
    {
      if (argc < 1) {
        die("missing value for <discharge>\n");
      }
      argc--;
      const char *i = *argv++;
      switch (*i) {
      case '0':
        inhibit = 0;
        break;
      case '1':
        inhibit = 1;
        break;
      default:
        die("invalid value for <discharge>\n");
      }
      if (i[1]) {
        die("invalid value for <discharge>\n");
      }
    }
    uint32_t min;
    {
      if (argc > 0) {
        argc--;
        const char *m = *argv++;
        char *end;
        errno = 0;
        unsigned long res = strtoul(m, &end, 10);
        if (*end) {
          die("invalid value for <min>\n");
        }
        if (res == ULONG_MAX && errno == ERANGE) {
          die("invalid value for <min>\n");
        }
        if (res > UINT16_MAX) {
          die("invalid value for <min>\n");
        }
        min = res;
      } else {
        min = 0;
      }
    }
    if (method == METHOD_IC && min != 65535) {
      min *= 2;
    }
    if (min > 1440 && min != 65535) {
      die("invalid value for <min>\n");
    }

    meth = method == METHOD_IC ? "BICS" : "PSSS";
    value = inhibit | (bat << 4) | (min << 8);
  } else if (method == METHOD_FD) {
    uint32_t discharge;
    {
      if (argc < 1) {
        die("missing value for <discharge>\n");
      }
      argc--;
      const char *i = *argv++;
      switch (*i) {
      case '0':
        discharge = 0;
        break;
      case '1':
        discharge = 1;
        break;
      default:
        die("invalid value for <discharge>\n");
      }
      if (i[1]) {
        die("invalid value for <discharge>\n");
      }
    }
    uint32_t acbreak;
    {
      if (argc >= 1) {
        argc--;
        const char *i = *argv++;
        switch (*i) {
        case '0':
          acbreak = 0;
          break;
        case '1':
          acbreak = 1;
          break;
        default:
          die("invalid value for <acbreak>\n");
        }
        if (i[1]) {
          die("invalid value for <acbreak>\n");
        }
      } else {
        acbreak = 0;
      }
    }
    meth = "BDSS";
    value = discharge | (acbreak << 1) | (bat << 8);
  } else if (method == METHOD_ST || method == METHOD_SP) {
    uint32_t percent;
    {
      if (argc > 0) {
        argc--;
        const char *m = *argv++;
        char *end;
        errno = 0;
        unsigned long res = strtoul(m, &end, 10);
        if (*end) {
          die("invalid value for <percent>\n");
        }
        if (res == ULONG_MAX && errno == ERANGE) {
          die("invalid value for <percent>\n");
        }
        if (res > 99) {
          die("invalid value for <percent>\n");
        }
        percent = res;
      } else {
        percent = 0;
      }
    }
    meth = method == METHOD_ST ? "BCCS" : "BCSS";
    value = percent | (bat << 8);
  } else {
    die_usage();
  }

  if (argc > 0) {
    die_usage();
  }

  acpi_call_set(meth, value);
}

static Method parse_method(const char *method) {
  if (!method) {
    die_usage();
  }
  size_t len = strlen(method);
  if (len < 2) {
    die_usage();
  }

  if (len == 2) {
    switch (*method) {
    case 's':
      if (method[1] == 't') {
        return METHOD_ST;
      } else if (method[1] == 'p') {
        return METHOD_SP;
      }
      break;
    case 'S':
      if (method[1] == 'T') {
        return METHOD_ST;
      } else if (method[1] == 'P') {
        return METHOD_SP;
      }
      break;
    case 'i':
      if (method[1] == 'c') {
        return METHOD_IC;
      }
      break;
    case 'I':
      if (method[1] == 'C') {
        return METHOD_IC;
      }
      break;
    case 'f':
      if (method[1] == 'd') {
        return METHOD_FD;
      }
      break;
    case 'F':
      if (method[1] == 'D') {
        return METHOD_FD;
      }
      break;
    case 'p':
      if (method[1] == 's') {
        return METHOD_PS;
      }
      break;
    case 'P':
      if (method[1] == 'S') {
        return METHOD_PS;
      }
    }
    die_usage();
  }

  if (*method == '-' && method[1] == '-') {
    method += 2;
    len -= 2;
  }

  switch (len) {
  case 2:
    switch (*method) {
    case 's':
      if (method[1] == 't') {
        return METHOD_ST;
      } else if (method[1] == 'p') {
        return METHOD_SP;
      }
      break;
    case 'i':
      if (method[1] == 'c') {
        return METHOD_IC;
      }
      break;
    case 'f':
      if (method[1] == 'd') {
        return METHOD_FD;
      }
      break;
    case 'p':
      if (method[1] == 's') {
        return METHOD_PS;
      }
    }
    break;
  case 4:
    if (!memcmp(method, "stop", 4)) {
      return METHOD_SP;
    }
    break;
  case 5:
    if (!memcmp(method, "start", 5)) {
      return METHOD_ST;
    }
    break;
  case 13:
    if (!memcmp(method, "stopThreshold", 13)) {
      return METHOD_SP;
    } else if (!memcmp(method, "inhibitCharge", 13)) {
      return METHOD_IC;
    }
    break;
  case 14:
    if (!memcmp(method, "startThreshold", 14)) {
      return METHOD_ST;
    } else if (!memcmp(method, "forceDischarge", 14)) {
      return METHOD_FD;
    } else if (!memcmp(method, "peakShiftState", 14)) {
      return METHOD_PS;
    }
    break;
  }

  die_usage();
  return 0;
}

static int insmod_insert(struct kmod_module *mod, int flags) {
  int err = 0;

  err = kmod_module_probe_insert_module(mod, flags, NULL, NULL, NULL, NULL);

  if (err >= 0) {
    err = 0;
  } else {
    switch (err) {
    case -EEXIST:
      err = 0;
      break;
    case -ENOENT:
      eprintf("could not insert '%s': Unknown symbol in module (see "
              "dmesg)\n",
              kmod_module_get_name(mod));
      break;
    default:
      eprintf("could not insert '%s': %s\n", kmod_module_get_name(mod),
              strerror(-err));
      break;
    }
  }

  return err;
}

static int insmod(struct kmod_ctx *ctx, const char *alias) {
  struct kmod_list *l, *list = NULL;
  struct kmod_module *mod = NULL;
  int err;

  if (strncmp(alias, "/", 1) == 0 || strncmp(alias, "./", 2) == 0) {
    err = kmod_module_new_from_path(ctx, alias, &mod);
    if (err < 0) {
      eprintf("Failed to get module from path: %s: %s\n", alias,
              strerror(-err));
      return -ENOENT;
    }
  } else {
    err = kmod_module_new_from_lookup(ctx, alias, &list);
    if (list == NULL || err < 0) {
      eprintf("Module %s not found in directory %s\n", alias,
              ctx ? kmod_get_dirname(ctx) : "(missing)");
      return -ENOENT;
    }
  }

  int flags = KMOD_PROBE_APPLY_BLACKLIST_ALIAS_ONLY;

  if (mod != NULL) {
    err = insmod_insert(mod, flags);
    kmod_module_unref(mod);
  } else {
    kmod_list_foreach(l, list) {
      mod = kmod_module_get_module(l);
      err = insmod_insert(mod, flags);
      kmod_module_unref(mod);
    }
    kmod_module_unref_list(list);
  }

  return err;
}

static void load_acpi_call(void) {
  struct kmod_ctx *ctx = kmod_new(NULL, NULL);
  if (!ctx) {
    die("kmod_new() failed!\n");
  }
  kmod_load_resources(ctx);
  int res = insmod(ctx, "acpi_call");
  kmod_unref(ctx);
  if (!res) {
    exit(1);
  }
}
