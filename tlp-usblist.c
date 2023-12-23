#define _POSIX_C_SOURCE 200809L
#define POSIXLY_CORRECT 1
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <libudev.h>
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  size_t alloc;
  size_t len;
  char **data;
} StringVec;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define xfree(v)                                                               \
  do {                                                                         \
    if (v) {                                                                   \
      free(v);                                                                 \
      (v) = NULL;                                                              \
    }                                                                          \
  } while (0)
#if defined(__GNUC__) && __GNUC__ > 2
#define __nonnull__(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define __nonnull__(...)
#endif

#define USB_MAX_DEPTH 7
#define DECIMAL_DIGITS_BOUND(t) (241 * sizeof(t) / 100 + 1)
#define SYSFS_USB_PATH "/sys/bus/usb/devices/"

static int verbose = 0;

static int parse_opts(int argc, char *const *argv);
static void sort(libusb_device **, size_t);
static void dump_device(struct udev_hwdb *, libusb_device *);

int main(int argc, char *const *argv) {
  libusb_context *ctx = NULL;
  libusb_device **devices = NULL;
  struct udev *udev = NULL;
  struct udev_hwdb *hwdb = NULL;
  int ret = 0;

  if (parse_opts(argc, argv) == -1) {
    goto error;
  }

  if ((ret = libusb_init(&ctx))) {
    eprintf("%s\n", libusb_strerror(ret));
    goto error;
  }

  size_t devices_len;
  {
    ssize_t len = libusb_get_device_list(ctx, &devices);
    if (len < 0) {
      goto error;
    }
    devices_len = len;
  }
  sort(devices, devices_len);

  udev = udev_new();
  if (!udev) {
    goto error;
  }

  hwdb = udev_hwdb_new(udev);
  if (!hwdb) {
    goto error;
  }

  for (libusb_device **dev = devices, **end = devices + devices_len; dev < end;
       ++dev) {
    dump_device(hwdb, *dev);
  }

  int res = 0;
  goto cleanup;
error:
  res = -1;
cleanup:
  if (hwdb) {
    udev_hwdb_unref(hwdb);
  }
  if (udev) {
    udev_unref(udev);
  }
  if (devices) {
    libusb_free_device_list(devices, 1);
  }
  if (ctx) {
    libusb_exit(ctx);
  }
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

static int compare(libusb_device **a, libusb_device **b) {
  {
    uint8_t a_bus = libusb_get_bus_number(*a);
    uint8_t b_bus = libusb_get_bus_number(*b);
    if (a_bus < b_bus) {
      return -1;
    }
    if (a_bus > b_bus) {
      return 1;
    }
  }

  uint8_t a_addr = libusb_get_device_address(*a);
  uint8_t b_addr = libusb_get_device_address(*b);
  if (a_addr < b_addr) {
    return -1;
  }
  if (a_addr > b_addr) {
    return 1;
  }
  return 0;
}

static void sort(libusb_device **devices, size_t len) {
  qsort(devices, len, sizeof(libusb_device *), (__compar_fn_t)compare);
}

static int string_vec_reserve(StringVec *vec, size_t n) {
  size_t needed = vec->len + n;
  size_t new_alloc = vec->alloc;
  while (new_alloc < needed) {
    if (new_alloc) {
      new_alloc *= 2;
    } else {
      new_alloc = 10;
    }
  }

  if (vec->alloc < new_alloc) {
    if (vec->alloc) {
      vec->data = realloc(vec->data, new_alloc * sizeof(char *));
    } else {
      vec->data = malloc(new_alloc * sizeof(char *));
    }
    if (!vec->data) {
      vec->len = vec->alloc = 0;
      return -1;
    }
    vec->alloc = new_alloc;
  }
  return 0;
}

static size_t string_vec_find(const StringVec *vec, const char *str) {
  char **end = vec->data + vec->len;
  size_t i = 0;
  for (char **ptr = vec->data; ptr < end; ++ptr, ++i) {
    if (!strcmp(*ptr, str)) {
      return i;
    }
  }
  return SIZE_MAX;
}

static void string_vec_free(StringVec *vec) {
  if (vec->data) {
    char **end = vec->data + vec->len;
    for (char **ptr = vec->data; ptr < end; ++ptr) {
      free(*ptr);
    }
    free(vec->data);
    vec->alloc = vec->len = 0;
  }
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

static char *cat_relative(const char *base, const char *rel) {
  errno = 0;
  char *buf = NULL, *res = NULL;

  buf = malloc(strlen(base) + strlen(rel) + 2);
  if (!buf) {
    goto cleanup;
  }
  sprintf(buf, "%s/%s", base, rel);
  res = cat(buf);

cleanup:
  xfree(buf);
  return res;
}

static char *extract_driver(const char *uevent_path) {
  char *content = cat(uevent_path);
  char *buf = content;
  char *driver = NULL;

  while (*buf) {
    char *line = buf;
    while (1) {
      if (!*buf) {
        break;
      }
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

static int __nonnull__(1, 2)
    udev_device_get_children_drivers(const char *syspath, StringVec *drivers) {
  drivers->alloc = 0;
  drivers->len = 0;
  drivers->data = NULL;

  size_t path_len = strlen(syspath);
  char *driver = NULL;
  char *child_syspath = NULL;
  DIR *dir = opendir(syspath);
  if (!dir) {
    goto error;
  }

  while (1) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno) {
        goto error;
      }
      break;
    }

    char *col = strchr(entry->d_name, ':');
    if (!col || !*(col + 1)) {
      continue;
    }

    size_t len = strlen(entry->d_name);
    child_syspath = malloc(path_len + len + 9);
    if (!child_syspath) {
      goto error;
    }
    sprintf(child_syspath, "%s/%s/uevent", syspath, entry->d_name);
    driver = extract_driver(child_syspath);
    xfree(child_syspath);

    if (driver && string_vec_find(drivers, driver) == SIZE_MAX) {
      if (string_vec_reserve(drivers, 1) == -1) {
        goto error;
      }
      drivers->data[drivers->len++] = driver;
      driver = NULL;
    }
    xfree(driver);
  }

  int res = 0;
  goto cleanup;
error:
  res = -1;
  string_vec_free(drivers);
cleanup:
  xfree(driver);
  xfree(child_syspath);
  if (dir) {
    closedir(dir);
  }
  return res;
}

static const char *hwdb_get(struct udev_hwdb *hwdb, const char *modalias,
                            const char *key) {
  struct udev_list_entry *entry;

  udev_list_entry_foreach(
      entry, udev_hwdb_get_properties_list_entry(hwdb, modalias, 0)) {
    if (!strcmp(udev_list_entry_get_name(entry), key)) {
      return udev_list_entry_get_value(entry);
    }
  }
  return NULL;
}

static const char *hwdb_get_vendor(struct udev_hwdb *hwdb, uint16_t vendor_id) {
  char modalias[64];
  sprintf(modalias, "usb:v%04X*", vendor_id);
  return hwdb_get(hwdb, modalias, "ID_VENDOR_FROM_DATABASE");
}

static const char *hwdb_get_model(struct udev_hwdb *hwdb, uint16_t vendor_id,
                                  uint16_t model_id) {
  char modalias[64];
  sprintf(modalias, "usb:v%04Xp%04X*", vendor_id, model_id);
  return hwdb_get(hwdb, modalias, "ID_MODEL_FROM_DATABASE");
}

static size_t get_sysfs_name(libusb_device *dev, char **buf) {
  uint8_t bnum = libusb_get_bus_number(dev);
  uint8_t pnums[USB_MAX_DEPTH];
  size_t pnums_len = 0;

  {
    int ret = libusb_get_port_numbers(dev, pnums, sizeof(pnums));
    if (ret == LIBUSB_ERROR_OVERFLOW) {
      return SIZE_MAX;
    } else if (ret == 0) {
      size_t alloc = sizeof(SYSFS_USB_PATH) + DECIMAL_DIGITS_BOUND(uint8_t) + 3;
      *buf = malloc(alloc + 1);
      if (buf) {
        size_t len = sprintf(*buf, SYSFS_USB_PATH "usb%" PRIu8, bnum);
        if (alloc != len + 1) {
          *buf = realloc(*buf, len + 1);
        }
        return len;
      }
      return SIZE_MAX;
    }
    pnums_len = ret;
  }

  size_t alloc = sizeof(SYSFS_USB_PATH) + DECIMAL_DIGITS_BOUND(uint8_t) +
                 ((1 + DECIMAL_DIGITS_BOUND(uint8_t)) * pnums_len);
  *buf = malloc(alloc + 1);
  if (!*buf) {
    return SIZE_MAX;
  }
  size_t len = sprintf(*buf, SYSFS_USB_PATH "%" PRIu8, bnum);
  for (uint8_t *p = pnums, *end = pnums + pnums_len; p < end; ++p) {
    len += sprintf(*buf + len, (p == pnums) ? "-%" PRIu8 : ".%" PRIu8, *p);
  }
  if (alloc != len + 1) {
    *buf = realloc(*buf, len + 1);
  }
  return len;
}

static void dump_device(struct udev_hwdb *hwdb, libusb_device *dev) {
  char *syspath = NULL, *vendor_alloc = NULL, *model_alloc = NULL;
  char *ptimeout = NULL, *pmode = NULL, *pstatus = NULL;
  uint8_t bnum = libusb_get_bus_number(dev);
  uint8_t dnum = libusb_get_device_address(dev);
  struct libusb_device_descriptor desc;

  libusb_get_device_descriptor(dev, &desc);

  if (get_sysfs_name(dev, &syspath) == SIZE_MAX) {
    syspath = NULL;
  }

  printf("Bus %03" PRIu8 " Device %03" PRIu8 ": ID %04x:%04x ", bnum, dnum,
         desc.idVendor, desc.idProduct);

  if (syspath) {
    ptimeout = cat_relative(syspath, "power/autosuspend_delay_ms");
    if (ptimeout) {
      pmode = cat_relative(syspath, "power/control");
    }
  }

  if (ptimeout && pmode) {
    printf("control = %s,", pmode);
    size_t padding = strlen(pmode);
    if (padding < 4) {
      padding = 4 - padding;
      for (size_t i = 0; i < padding; ++i) {
        putchar(' ');
      }
    }
    printf(" autosuspend_delay_ms = %4s", ptimeout);

    if (verbose) {
      pstatus = cat_relative(syspath, "power/runtime_status");
      printf(", runtime_status = %-9s", pstatus ? pstatus : "");
    }
  } else {
    printf("(autosuspend not available)");
  }

  xfree(ptimeout);
  xfree(pmode);
  xfree(pstatus);

  const char *vendor = "[unknown]";
  {
    const char *hwdb_vendor = hwdb_get_vendor(hwdb, desc.idVendor);
    if (hwdb_vendor) {
      vendor = hwdb_vendor;
    } else {
      if (syspath) {
        vendor_alloc = cat_relative(syspath, "manufacturer");
      }

      if (vendor_alloc) {
        vendor = vendor_alloc;
      }
    }
  }
  printf(" -- %s ", vendor);
  xfree(vendor_alloc);

  const char *model = "[unknown]";
  {
    const char *hwdb_model =
        hwdb_get_model(hwdb, desc.idVendor, desc.idProduct);
    if (hwdb_model) {
      model = hwdb_model;
    } else {
      if (syspath) {
        model_alloc = cat_relative(syspath, "product");
      }

      if (model_alloc) {
        model = model_alloc;
      }
    }
  }
  printf("%s ", model);
  xfree(model_alloc);

  StringVec drivers;
  if (!syspath || udev_device_get_children_drivers(syspath, &drivers) == -1) {
    printf("(no driver)\n");
  } else {
    putchar('(');
    for (char **p = drivers.data, **end = drivers.data + drivers.len; p < end;
         ++p) {
      if (p != drivers.data) {
        printf(", ");
      }
      printf("%s", *p);
    }
    string_vec_free(&drivers);
    printf(")\n");
  }

  xfree(syspath);
}
