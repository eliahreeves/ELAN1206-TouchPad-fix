/*
 * touchpad-monitor: keep the ELAN1206 I2C touchpad usable on Linux >= 6.9.
 *
 * Background (ASUS ZenBook UX564EH, Intel Tiger Lake LPSS):
 *   The interrupt line shared by the touchpad's I2C controller
 *   (i2c_designware.N) and its DMA engine (idma64.N) is permanently asserted
 *   on this hardware.  Since kernel 6.9 (commit 9140ce47872b, "dmaengine:
 *   idma64: Don't try to serve interrupts when device is powered off") the
 *   idma64 handler no longer claims those interrupts, so the kernel logs
 *   "irq N: nobody cared", disables the line, and from then on the I2C
 *   controller is only serviced by the 10 Hz spurious-IRQ poller.  The
 *   touchpad still "works" but reports at 10 Hz, which libinput cannot use.
 *
 * What this program does:
 *   Re-binding idma64.N re-enables the disabled interrupt (the kernel's
 *   shared-IRQ setup path clears the spurious-disabled state).  A device can
 *   only be bound after being unbound, so the cycle is: unbind idma64.N once
 *   the interrupt line has gone quiet, re-bind it on the next touch event.
 *   While the touchpad is in use enough real I2C interrupts are handled that
 *   the kernel keeps the line enabled; once it goes idle the kernel disables
 *   the storming line again within a few seconds, bound or not.
 *
 *   Unbinding while the line is still storming is expensive: freeing a
 *   shared IRQ busy-spins in the kernel until no handler is in progress,
 *   which under a continuous storm means spinning for seconds on a full CPU.
 *   So we watch the interrupt count in /proc/interrupts and only unbind after
 *   it has stopped changing.  If the IRQ number cannot be determined we fall
 *   back to unbinding after a fixed idle period (-t).
 *
 * Runs as root (needs /dev/input/event* and /sys/bus/platform/drivers/idma64).
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TOUCHPAD_NAME "ELAN1206"
#define DEFAULT_DMA_DEVICE "idma64.1"
#define DEFAULT_IDLE_SECONDS 1.0
#define IDMA_DRIVER_DIR "/sys/bus/platform/drivers/idma64"
#define RETRY_SECONDS 2
#define POLL_MS 250    /* how often to check the interrupt count while bound */
#define QUIET_POLLS 2  /* unchanged polls before the line counts as quiet */

static volatile sig_atomic_t stop_requested = 0;
static int verbose = 0;
static int dry_run = 0;

static void on_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

static void logmsg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("touchpad-monitor: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

/*
 * Find /dev/input/eventN for the touchpad.  Returns N, or -1 if not found.
 * From the device's sysfs path, e.g.
 *   /sys/devices/pci0000:00/0000:00:15.1/i2c_designware.1/i2c-1/i2c-ELAN1206:00/...
 * derive the DMA engine paired with the I2C controller (idma64.1) and the
 * controller's IRQ number (the "irq" attribute of its parent, 0000:00:15.1).
 * Either output may be NULL to leave it alone.
 */
static int find_touchpad(const char *name, char *dma_dev, size_t dma_len,
                         int *irq_out) {
  DIR *dir = opendir("/sys/class/input");
  if (dir == NULL) {
    logmsg("opendir /sys/class/input: %s", strerror(errno));
    return -1;
  }

  int found = -1;
  struct dirent *entry;
  while (found < 0 && (entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "event", 5) != 0)
      continue;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "/sys/class/input/%s/device/name",
             entry->d_name);
    FILE *f = fopen(path, "r");
    if (f == NULL)
      continue;
    char devname[256] = "";
    if (fgets(devname, sizeof devname, f) == NULL)
      devname[0] = '\0';
    fclose(f);
    devname[strcspn(devname, "\n")] = '\0';

    if (strstr(devname, name) == NULL || strstr(devname, "Touchpad") == NULL)
      continue;

    int num;
    if (sscanf(entry->d_name, "event%d", &num) != 1)
      continue;
    found = num;

    char real[PATH_MAX];
    snprintf(path, sizeof path, "/sys/class/input/%s/device", entry->d_name);
    const char *dw = realpath(path, real) ? strstr(real, "i2c_designware.")
                                          : NULL;
    if (dw == NULL)
      continue;
    int ctrl;
    if (dma_dev != NULL && sscanf(dw, "i2c_designware.%d", &ctrl) == 1)
      snprintf(dma_dev, dma_len, "idma64.%d", ctrl);
    if (irq_out != NULL && dw > real + 1) {
      snprintf(path, sizeof path, "%.*s/irq", (int)(dw - real - 1), real);
      f = fopen(path, "r");
      if (f != NULL) {
        int irq;
        if (fscanf(f, "%d", &irq) == 1 && irq > 0)
          *irq_out = irq;
        fclose(f);
      }
    }
  }
  closedir(dir);
  return found;
}

/* Total number of times irq has fired, summed over all CPUs; -1 on error. */
static long irq_count(int irq) {
  FILE *f = fopen("/proc/interrupts", "r");
  if (f == NULL)
    return -1;
  char key[16];
  snprintf(key, sizeof key, "%d:", irq);
  size_t klen = strlen(key);
  long total = -1;
  char line[4096];
  while (fgets(line, sizeof line, f) != NULL) {
    char *p = line;
    while (*p == ' ')
      p++;
    if (strncmp(p, key, klen) != 0)
      continue;
    p += klen;
    total = 0;
    for (;;) {
      while (*p == ' ')
        p++;
      if (*p < '0' || *p > '9')
        break;
      total += strtol(p, &p, 10);
    }
    break;
  }
  fclose(f);
  return total;
}

static int dma_bound(const char *dma_dev) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, IDMA_DRIVER_DIR "/%s", dma_dev);
  return access(path, F_OK) == 0;
}

/* Write dma_dev to the idma64 driver's bind or unbind file.  0 on success. */
static int dma_set_bound(const char *dma_dev, int bind, const char *why) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, IDMA_DRIVER_DIR "/%s", bind ? "bind" : "unbind");
  if (verbose)
    logmsg("%s %s (%s)", bind ? "bind" : "unbind", dma_dev, why);
  if (dry_run)
    return 0;

  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    logmsg("open %s: %s", path, strerror(errno));
    return -1;
  }
  ssize_t n = write(fd, dma_dev, strlen(dma_dev));
  int saved = errno;
  close(fd);
  if (n < 0) {
    logmsg("write \"%s\" to %s: %s", dma_dev, path, strerror(saved));
    return -1;
  }
  return 0;
}

/*
 * Open the touchpad's event device, waiting for it to appear (the service may
 * start before the touchpad is probed, or the device may be re-created after
 * suspend).  Returns an fd, or -1 if a stop was requested while waiting.
 */
static int open_touchpad(const char *name, const char *event_path,
                         char *dma_dev, size_t dma_len, int *irq,
                         int derive_irq) {
  int announced = 0;
  while (!stop_requested) {
    char path[PATH_MAX];
    int num = -1;
    if (event_path != NULL) {
      snprintf(path, sizeof path, "%s", event_path);
      num = 0;
    } else {
      num = find_touchpad(name, dma_dev, dma_len, derive_irq ? irq : NULL);
      if (num >= 0)
        snprintf(path, sizeof path, "/dev/input/event%d", num);
    }
    if (num >= 0) {
      int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd >= 0) {
        logmsg("using %s (%s), DMA device %s, irq %d", path, name, dma_dev,
               *irq);
        return fd;
      }
      if (!announced)
        logmsg("open %s: %s (retrying every %d s)", path, strerror(errno),
               RETRY_SECONDS);
    } else if (!announced) {
      logmsg("touchpad \"%s\" not found (retrying every %d s)", name,
             RETRY_SECONDS);
    }
    announced = 1;
    sleep(RETRY_SECONDS);
  }
  return -1;
}

static int epoll_add(int epfd, int fd) {
  struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    logmsg("epoll_ctl: %s", strerror(errno));
    return -1;
  }
  return 0;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [-t idle_seconds] [-n touchpad_name] [-d dma_device] "
          "[-i irq] [-e event_device] [-v] [-D] [-l]\n"
          "  -t  fallback when the IRQ is unknown: seconds without touch "
          "events before unbinding (default %.1f)\n"
          "  -n  substring of the touchpad's input device name (default %s)\n"
          "  -d  DMA platform device to toggle (default: derived from the "
          "touchpad's I2C controller, else %s)\n"
          "  -i  IRQ number of the I2C controller (default: derived; 0 "
          "disables the interrupt-count check)\n"
          "  -e  path of the touchpad's event device (skips detection)\n"
          "  -v  log every bind/unbind\n"
          "  -D  dry run: never write to sysfs\n"
          "  -l  only print the detected touchpad, DMA device and IRQ, then "
          "exit\n",
          argv0, DEFAULT_IDLE_SECONDS, DEFAULT_TOUCHPAD_NAME,
          DEFAULT_DMA_DEVICE);
}

int main(int argc, char **argv) {
  const char *name = DEFAULT_TOUCHPAD_NAME;
  const char *event_path = NULL;
  double idle = DEFAULT_IDLE_SECONDS;
  char dma_dev[32] = DEFAULT_DMA_DEVICE;
  int irq_num = 0;
  int dma_forced = 0, irq_forced = 0, list_only = 0, opt;

  while ((opt = getopt(argc, argv, "t:n:d:i:e:vDlh")) != -1) {
    switch (opt) {
    case 't':
      idle = atof(optarg);
      if (idle <= 0) {
        usage(argv[0]);
        return 2;
      }
      break;
    case 'n':
      name = optarg;
      break;
    case 'd':
      snprintf(dma_dev, sizeof dma_dev, "%s", optarg);
      dma_forced = 1;
      break;
    case 'i':
      irq_num = atoi(optarg);
      irq_forced = 1;
      break;
    case 'e':
      event_path = optarg;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'D':
      dry_run = 1;
      break;
    case 'l':
      list_only = 1;
      break;
    default:
      usage(argv[0]);
      return 2;
    }
  }

  if (list_only) {
    int num = find_touchpad(name, dma_forced ? NULL : dma_dev, sizeof dma_dev,
                            irq_forced ? NULL : &irq_num);
    if (num < 0) {
      logmsg("touchpad \"%s\" not found", name);
      return 1;
    }
    printf("touchpad: /dev/input/event%d\ndma device: %s (%s)\nirq: %d "
           "(count %ld)\n",
           num, dma_dev, dma_bound(dma_dev) ? "bound" : "unbound", irq_num,
           irq_num > 0 ? irq_count(irq_num) : -1L);
    return 0;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal; /* no SA_RESTART: blocking calls return EINTR */
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    logmsg("epoll_create1: %s", strerror(errno));
    return 1;
  }

  /*
   * Two states.  Awake (DMA bound): poll the interrupt count and unbind once
   * the line has gone quiet (or, without an IRQ number, after the idle
   * period).  Asleep (DMA unbound): wait in epoll for the first touch event,
   * then bind again.
   */
  int fd = -1;
  int sleeping = 0; /* 1 while the DMA device is unbound */
  long last_count = -1;
  int quiet_polls = 0;

  while (!stop_requested) {
    if (fd < 0) {
      fd = open_touchpad(name, event_path, dma_forced ? NULL : dma_dev,
                         sizeof dma_dev, &irq_num, !irq_forced);
      if (fd < 0)
        break;
      /* Resync with the real state, e.g. after a restart while unbound. */
      sleeping = !dma_bound(dma_dev);
      last_count = -1;
      quiet_polls = 0;
      if (sleeping && epoll_add(epfd, fd) < 0) {
        close(fd);
        fd = -1;
        sleep(RETRY_SECONDS);
        continue;
      }
    }

    if (sleeping) {
      struct epoll_event ev;
      if (epoll_wait(epfd, &ev, 1, -1) < 0) {
        if (errno == EINTR)
          continue;
        logmsg("epoll_wait: %s", strerror(errno));
        break;
      }
    } else {
      double period = irq_num > 0 ? POLL_MS / 1000.0 : idle;
      struct timespec ts;
      ts.tv_sec = (time_t)period;
      ts.tv_nsec = (long)((period - (double)ts.tv_sec) * 1e9);
      if (nanosleep(&ts, NULL) < 0)
        continue; /* EINTR: re-check stop_requested */
    }

    /* Drain everything queued; one read per wake-up would spin. */
    int got = 0, lost = 0, lost_errno = 0;
    for (;;) {
      struct input_event buf[64];
      ssize_t r = read(fd, buf, sizeof buf);
      if (r > 0) {
        got = 1;
        continue;
      }
      if (r < 0 && errno == EINTR)
        continue;
      if (r < 0 && errno == EAGAIN)
        break;
      lost = 1; /* EOF or ENODEV: the device went away */
      lost_errno = r < 0 ? errno : 0;
      break;
    }

    if (lost) {
      logmsg("touchpad device lost (%s), rediscovering",
             lost_errno ? strerror(lost_errno) : "hang-up");
      if (sleeping)
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
      close(fd);
      fd = -1;
      sleep(1);
      continue;
    }

    if (sleeping) {
      if (got) {
        sleeping = 0;
        last_count = -1;
        quiet_polls = 0;
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        dma_set_bound(dma_dev, 1, "touch");
      }
      continue;
    }

    /* Awake: decide whether it is time (and cheap) to unbind. */
    int unbind = 0;
    const char *why = "idle";
    long count = irq_num > 0 ? irq_count(irq_num) : -1;
    if (count >= 0) {
      quiet_polls = (count == last_count) ? quiet_polls + 1 : 0;
      last_count = count;
      if (quiet_polls >= QUIET_POLLS) {
        unbind = 1;
        why = "irq quiet";
      }
    } else if (!got) {
      unbind = 1; /* no usable IRQ count: fall back to the idle period */
    }
    if (unbind) {
      sleeping = 1;
      dma_set_bound(dma_dev, 0, why);
      if (epoll_add(epfd, fd) < 0) {
        close(fd);
        fd = -1;
      }
    }
  }

  /* Leave the system in its stock state (DMA device bound) on exit. */
  if (sleeping && !dry_run && !dma_bound(dma_dev))
    dma_set_bound(dma_dev, 1, "exit");
  if (fd >= 0)
    close(fd);
  close(epfd);
  return 0;
}
