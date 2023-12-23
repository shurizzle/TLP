#define _POSIX_C_SOURCE 200809L
#define POSIXLY_CORRECT 1
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pci/pci.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define xfree(v)                                                               \
  do {                                                                         \
    if (v) {                                                                   \
      free(v);                                                                 \
      (v) = NULL;                                                              \
    }                                                                          \
  } while (0)

static int verbose = 0;

static int parse_opts(int argc, char *const *argv);
static void die(char *msg, ...) PCI_PRINTF(1, 2) PCI_NONRET;
static int sort(struct pci_access *, size_t);
static void print_device(struct pci_dev *, struct pci_access *);

int main(int argc, char *const *argv) {
  struct pci_access *pacc = NULL;

  if (parse_opts(argc, argv) == -1) {
    goto error;
  }

  pacc = pci_alloc();
  pacc->error = die;
  pci_init(pacc);

  pci_scan_bus(pacc);
  size_t len = 0;
  for (struct pci_dev *p = pacc->devices; p; p = p->next, ++len) {
    pci_fill_info(p, PCI_FILL_CLASS | PCI_FILL_CLASS_EXT);
  }
  if (sort(pacc, len) == -1) {
    goto error;
  }

  for (struct pci_dev *p = pacc->devices; p; p = p->next) {
    print_device(p, pacc);
  }

  int res = 0;
  goto cleanup;
error:
  res = 1;
cleanup:
  if (pacc) {
    pci_cleanup(pacc);
  }
  fflush(stdout);
  return res;
}

static int parse_opts(int argc, char *const *argv) {
  static struct option lo[] = {
      {"verbose", no_argument, &verbose, 1},
      {0, 0, 0, 0},
  };

  int c;

  while (1) {
    int option_index = 0;
    c = getopt_long(argc, argv, "v", lo, &option_index);

    if (c == -1) {
      break;
    }

    switch (c) {
    case 0:
      break;
    case 'v':
      verbose = 1;
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

static void PCI_NONRET die(char *msg, ...) {
  va_list args;

  va_start(args, msg);
  vfprintf(stderr, msg, args); // NOLINT(clang-analyzer-valist.Uninitialized)
  fputc('\n', stderr);
  exit(1);
}

static int compare(const struct pci_dev **a, const struct pci_dev **b) {
  if ((*a)->bus < (*b)->bus) {
    return -1;
  }
  if ((*a)->bus > (*b)->bus) {
    return 1;
  }
  if ((*a)->dev < (*b)->dev) {
    return -1;
  }
  if ((*a)->dev > (*b)->dev) {
    return 1;
  }
  if ((*a)->func < (*b)->func) {
    return -1;
  }
  if ((*a)->func > (*b)->func) {
    return 1;
  }
  return 0;
}

static int sort(struct pci_access *pacc, size_t len) {
  if (!len) {
    return 0;
  }

  struct pci_dev **index, **h, **last_dev;

  h = index = malloc(sizeof(struct pci_dev *) * len);
  if (!index) {
    perror(NULL);
    return -1;
  }
  size_t cnt = 0;
  for (struct pci_dev *d = pacc->devices; d; d = d->next, ++h, ++cnt) {
    *h = d;
  }
  qsort(index, len, sizeof(struct pci_dev *), (__compar_fn_t)compare);
  last_dev = &pacc->devices;
  h = index;
  while (len--) {
    *last_dev = *h;
    last_dev = &(*h)->next;
    h++;
  }
  *last_dev = NULL;
  xfree(index);
  return 0;
}

static char *cat_relative(const char *base, const char *rel) {
  errno = 0;
  size_t alloc = 0, len = 0;
  char *buf = NULL, *res = NULL;
  int fd = -1;

  buf = malloc(strlen(base) + strlen(rel) + 2);
  if (!buf) {
    goto cleanup;
  }
  sprintf(buf, "%s/%s", base, rel);
  fd = open(buf, O_RDONLY);
  xfree(buf);
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
  xfree(buf);
  if (fd != -1) {
    close(fd);
  }
  return res;
}

static char *get_driver(const char *syspath) {
  char *content = cat_relative(syspath, "uevent"), *driver = NULL;
  if (!content) {
    return NULL;
  }

  char *buf = content;
  while (*buf) { // NOLINT(clang-analyzer-core.uninitialized.Branch)
    char *line = buf;
    while (*buf) {
      if (*buf == '\n') {
        *buf = 0;
        buf++;
        break;
      }

      buf++;
    }

    if (!strncmp(line, "DRIVER=", 7)) {
      driver = strdup(line + 7);
      break;
    }
  }

  xfree(content);
  return driver;
}

static void print_device(struct pci_dev *p, struct pci_access *pacc) {
  char classbuf[256];
  char *power = NULL, *class = NULL, *pstatus = NULL, *driver = NULL;
  char *syspath = malloc(36);
  if (!syspath) {
    perror(NULL);
    return;
  }
  sprintf(syspath, "/sys/bus/pci/devices/0000:%02x:%02x.%d", p->bus, p->dev,
          p->func);

  printf("%s/power/control = ", syspath);

  power = cat_relative(syspath, "power/control");
  if (!power && errno == ENOENT) {
    printf("(not available)");
  } else {
    printf("%-4s", power ? power : "");
  }
  xfree(power);

  if (verbose) {
    pstatus = cat_relative(syspath, "power/runtime_status");
    printf(", runtime_status = %-9s", pstatus ? pstatus : "");
    xfree(pstatus);
  }

  class = cat_relative(syspath, "class");
  printf(" (%s, ", class ? class : "");
  xfree(class);

  printf("%s, ", pci_lookup_name(pacc, classbuf, sizeof(classbuf),
                                 PCI_LOOKUP_CLASS, p->device_class));

  driver = get_driver(syspath);
  printf("%s)\n", driver ? driver : "no driver");
  xfree(syspath);
  xfree(driver);
}
