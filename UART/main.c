// main.c - UART CLI sender + read-until-end-marker
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/termios.h>
#include <time.h>
#include <unistd.h>

struct Config {
    int debug_mode;
    const char *device_path;
    long baud_rate;
} conf;

#define ERROR 1
#define WARNING 2
#define INFO 3
#define TRACE 4

#define _newline fprintf(stdout, "\n")

void logit(const char *fmt, va_list args, int log_lvl) {
    char message[1024];
    vsnprintf(message, sizeof(message), fmt, args);

    time_t now = time(NULL);

    const char *lvlStr[] = {"ERROR", "WARNING", "INFO", "TRACE"};
    const char *lvl = "UNKNOWN";
    if (log_lvl >= 1 && log_lvl <= 4) lvl = lvlStr[log_lvl - 1];

    fprintf(stdout, "[%ld] [%s] %s", (long)now, lvl, message);
    if (errno != 0) {
        fprintf(stdout, " (errno=%d: %s)", errno, strerror(errno));
    }
    fprintf(stdout, "\n");

    if (conf.debug_mode) {
        FILE *log_file = fopen("/tmp/error.log", "a");
        if (log_file) {
            fprintf(log_file, "[%ld] [%s] %s", (long)now, lvl, message);
            if (errno != 0) {
                fprintf(log_file, " (errno=%d: %s)", errno, strerror(errno));
            }
            fprintf(log_file, "\n");
            fclose(log_file);
        }
    }
}
void log_trace(const char *fmt, ...) { va_list args; va_start(args, fmt); logit(fmt, args, TRACE); va_end(args); }
void log_info(const char *fmt, ...)  { va_list args; va_start(args, fmt); logit(fmt, args, INFO);  va_end(args); }
void log_warning(const char *fmt, ...) { va_list args; va_start(args, fmt); logit(fmt, args, WARNING); va_end(args); }
void log_error(const char *fmt, ...) { va_list args; va_start(args, fmt); logit(fmt, args, ERROR); va_end(args); }

/* set blocking or non-blocking on fd */
static int set_blocking(int fd, int blocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (!blocking)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

/* open serial */
int serial_port_open(const char *path, long baud_rate) {
    log_trace("serial_port_open: Opening Serial Port {%s} at %ld baud", path, baud_rate);

    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        log_error("Failed to open device serial path: %s", path);
        perror("open");
        return -1;
    }

    if (!isatty(fd)) {
        close(fd);
        log_error("The given device path is not a TTY: %s", path);
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        log_error("tcgetattr failed");
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfmakeraw(&tty); /* raw mode */

    /* set baud */
    speed_t speed;
    switch (baud_rate) {
        case 9600:   speed = B9600; break;
        case 19200:  speed = B19200; break;
        case 38400:  speed = B38400; break;
        case 57600:  speed = B57600; break;
        case 115200: speed = B115200; break;
        default:     speed = B115200; break;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    /* 8N1 */
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= (CLOCAL | CREAD);

#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
#ifdef IXON
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
#endif
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    /* read behaviour */
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 10; /* 1.0s */

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        log_error("tcsetattr failed");
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    log_info("Serial port %s opened (fd=%d)", path, fd);
    return fd;
}

/* write data util */
static ssize_t write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = buf;
    size_t remaining = count;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        remaining -= (size_t)n;
        p += n;
    }
    return (ssize_t)count;
}

void send_data_to_device(int dev_handle, const char *message, int length) {
    if (dev_handle < 0) {
        log_error("Invalid device handle in send_data_to_device");
        return;
    }

    const char *header_start_message = "[UART_COM][START]";
    const char *header_end_msg = "[UART_COM][END]";

    size_t start_len = strlen(header_start_message);
    size_t end_len = strlen(header_end_msg);
    size_t msg_len = (length > 0) ? (size_t)length : strlen(message);

    size_t total = start_len + msg_len + end_len;
    char *buf = malloc(total);
    if (!buf) {
        log_error("malloc failed in send_data_to_device");
        return;
    }

    memcpy(buf, header_start_message, start_len);
    memcpy(buf + start_len, message, msg_len);
    memcpy(buf + start_len + msg_len, header_end_msg, end_len);

    if (write_all(dev_handle, buf, total) != (ssize_t)total) {
        log_error("Failed to write full message to device");
    } else {
        if (tcdrain(dev_handle) != 0) {
            log_warning("tcdrain returned error (errno=%d)", errno);
        } else {
            log_info("Message sent and drained successfully (%zu bytes)", total);
        }
    }

    free(buf);
}

/* read until the end_marker is seen or timeout elapsed.
   Returns 0 on success (found marker), 1 on timeout (partial data in out_buf),
   -1 on error. out_buf is malloc'd inside and must be free()'d by caller.
*/
int read_until_marker(int fd, const char *end_marker, int timeout_seconds, char **out_buf, size_t *out_len) {
    const size_t CHUNK = 512;
    size_t cap = CHUNK;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t len = 0;
    int marker_len = (int)strlen(end_marker);
    time_t start = time(NULL);

    while (1) {
        /* compute remaining timeout for select */
        time_t elapsed = time(NULL) - start;
        int remaining = timeout_seconds - (int)elapsed;
        if (remaining <= 0) break; /* timeout */

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = remaining;
        tv.tv_usec = 0;

        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        } else if (sel == 0) {
            break; /* timeout */
        }

        if (FD_ISSET(fd, &rfds)) {
            /* read available data */
            if (len + CHUNK > cap) {
                cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) { free(buf); return -1; }
                buf = nb;
            }
            ssize_t r = read(fd, buf + len, cap - len);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                free(buf);
                return -1;
            } else if (r == 0) {
                /* EOF? break and return what we have */
                break;
            } else {
                len += (size_t)r;
                /* check for marker in buffer */
                if (len >= (size_t)marker_len) {
                    if (memmem(buf, len, end_marker, marker_len) != NULL) {
                        *out_buf = buf;
                        *out_len = len;
                        return 0; /* found */
                    }
                }
                /* continue reading until timeout or marker */
            }
        }
    }

    /* timeout: return partial data if any */
    *out_buf = buf;
    *out_len = len;
    return 1;
}

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s -p <device_path> -b <baud_rate> -c <command> [-T timeout_seconds] [-x] [-h]\n", prog);
  fprintf(stderr, "Example: %s -p /dev/ttyUSB0 -b 115200 -c \"STATUS\\r\\n\" -T 5 -x\n", prog);
  fprintf(stderr, "  -p <device_path> : Path to Serial Device (e.g., /dev/ttyUSB0)\n");
  fprintf(stderr, "  -b <baud_rate>   : Baud Rate (e.g., 9600, 115200)\n");
  fprintf(stderr, "  -c <command>     : Command to send (required)\n");
  fprintf(stderr, "  -T <seconds>     : Timeout seconds to wait for response (default 5)\n");
  fprintf(stderr, "  -x               : Enable Debug Mode (optional)\n");
  fprintf(stderr, "  -h               : Show this help message\n");
}

int main(int argc, char *argv[]) {
  const char *dev_path = NULL;
  long baud_rate = 0;
  int debug = 0;
  int opt;
  const char *command = NULL;
  int timeout_seconds = 5;

  while ((opt = getopt(argc, argv, ":p:b:c:T:xh")) != -1) {
    switch (opt) {
    case 'p':
      dev_path = optarg;
      break;
    case 'b': {
      char *end = NULL;
      errno = 0;
      long v = strtol(optarg, &end, 10);
      if (errno || end == optarg || *end != '\0' || v <= 0) {
        fprintf(stderr, "Invalid baud rate: %s\n", optarg);
        usage(argv[0]);
        return EXIT_FAILURE;
      }
      baud_rate = v;
      break;
    }
    case 'c':
      command = optarg;
      break;
    case 'T': {
      char *end = NULL;
      errno = 0;
      long v = strtol(optarg, &end, 10);
      if (errno || end == optarg || *end != '\0' || v < 0) {
        fprintf(stderr, "Invalid timeout: %s\n", optarg);
        usage(argv[0]);
        return EXIT_FAILURE;
      }
      timeout_seconds = (int)v;
      break;
    }
    case 'x':
      debug = 1;
      break;
    case ':':
      fprintf(stderr, "Option -%c requires an argument\n", optopt);
      usage(argv[0]);
      return EXIT_FAILURE;
    case 'h':
      usage(argv[0]);
      return EXIT_SUCCESS;
    case '?':
      fprintf(stderr, "Unknown option: -%c\n", optopt);
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (!dev_path || baud_rate == 0 || !command) {
    fprintf(stderr, "Missing required -p and/or -b and/or -c\n");
    usage(argv[0]);
    return 2;
  }

  fprintf(stdout, "Info Used: \n");
  fprintf(stdout, "Device: %s\n", dev_path);
  fprintf(stdout, "Baud: %ld bauds\n", baud_rate);
  fprintf(stdout, "Command: %s\n", command);
  fprintf(stdout, "Timeout: %d seconds\n", timeout_seconds);
  fprintf(stdout, "Debug: %s\n", debug ? "on" : "off");
  _newline; _newline;

  conf.device_path = dev_path;
  conf.baud_rate = baud_rate;
  conf.debug_mode = debug;

  log_info("Opening Serial Port...");
  int dev_handle = serial_port_open(dev_path, baud_rate);
  if (dev_handle < 0) {
    fprintf(stderr, "Failed to open serial port %s\n", dev_path);
    return EXIT_FAILURE;
  }

  /* send command */
  send_data_to_device(dev_handle, command, (int)strlen(command));


  // READING SENT DATA
const char *end_marker = "[UART_COM][END]";
  char *resp = NULL;
  size_t resp_len = 0;
  int r = read_until_marker(dev_handle, end_marker, timeout_seconds, &resp, &resp_len);
  if (r == -1) {
    log_error("Error while reading response");
    close(dev_handle);
    return EXIT_FAILURE;
  } else if (r == 1) {
    log_warning("Timeout waiting for end marker; partial data (%zu bytes) received", resp_len);
  } else {
    log_info("End marker seen; total bytes received: %zu", resp_len);
  }
    
    
    // RESPONSE
  if (resp_len > 0) {
      char *printbuf = malloc(resp_len + 1);
      if (printbuf) {
          memcpy(printbuf, resp, resp_len);
          printbuf[resp_len] = '\0';
          printf("---- DEVICE RESPONSE START ----\n%s\n---- DEVICE RESPONSE END ----\n", printbuf);
          free(printbuf);
      } else {
          write_all(STDOUT_FILENO, resp, resp_len);
      }
  } else {
      printf("No response received.\n");
  }

  free(resp);
  close(dev_handle);
  return EXIT_SUCCESS;
}
