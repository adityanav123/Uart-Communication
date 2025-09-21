//
//  main.c
//  UART Communication

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/termios.h>
#include <time.h>
#include <unistd.h>

struct Config {
  int debug_mode;
  const char *device_path;
  int baud_rate;
} conf;

#define ERROR 1
#define WARNING 2
#define INFO 3
#define TRACE 4

void logit(const char *fmt, va_list args, int log_lvl);
void log_trace(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logit(fmt, args, TRACE);
  va_end(args);
}
void log_info(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logit(fmt, args, INFO);
  va_end(args);
}
void log_warning(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logit(fmt, args, WARNING);
  va_end(args);
}
void log_error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  logit(fmt, args, ERROR);
  va_end(args);
}

// LOGGING
// Var Message
void logit(const char *fmt, va_list args, int log_lvl) {
  char message[1024];
  vsnprintf(message, sizeof(message), fmt, args);

  // Log File
  if (conf.debug_mode) {
    FILE *log_file = fopen("/tmp/error.log", "a");
    if (log_file) {
      time_t seconds;
      time(&seconds);
      fprintf(log_file, "[%ld] [%d] Error: %s (errno=%d)\n", seconds, log_lvl,
              message, errno);
      perror("Detail");
      fclose(log_file);
    }
  }
  time_t now = time(NULL);

  char *lvlStr[] = {"TRACE", "INFO", "WARNING", "ERROR"};

  fprintf(stdout, "[%ld] [%s] %s (errno=%d)\n", now, lvlStr[log_lvl - 1],
          message, errno);
  perror("Detail");
}

int serial_port_open(const char *path, int baud_rate) {
  log_trace("serial_port_open: Opening Serial Port {%s} at %d baud", path,
            baud_rate);

  // dev handle
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    log_error("Failed to open device serial path. %s", path);
    perror("open");
    return -EXIT_FAILURE;
  }

  // First Checking the device is tty or not
  if (!isatty(fd)) {
    close(fd);
    log_error("The given device path is not /dev/tty. %s (err=%d)\n", path,
              errno);
    return -EXIT_FAILURE;
  }

  // Termios -- Terminal IOs for async communication ports
  struct termios terminalIos;

  // Fetching Current Configuration of serial interface
  if (tcgetattr(fd, &terminalIos) != 0) {
    log_error("Failure in getting configuration!\n");
    perror("tcgetattr");
    close(fd);
    return -EXIT_FAILURE;
  }

  // Raw Mode enabled
  cfmakeraw(&terminalIos);

  // Setting Up Baud Rate.
  log_trace("Setting Up Baud Rate to %d bauds\n", baud_rate);

  speed_t speed; // default 115200 bauds.
  switch (baud_rate) {
  case 9600:
    speed = B9600;
    break;
  case 19200:
    speed = B19200;
    break;
  case 38400:
    speed = B38400;
    break;
  case 57600:
    speed = B57600;
    break;
  case 115200:
    speed = B115200;
    break;
  default:
    speed = B115200;
    break;
  }
  cfsetispeed(&terminalIos, speed); // Input Speed
  cfsetospeed(&terminalIos, speed); // Output Speed

  // Turning off character Processing
  // Forcing 8bit input
  terminalIos.c_cflag &= ~PARENB; // no parity checking
  terminalIos.c_cflag &= ~CSTOPB; // no character processing
  terminalIos.c_cflag &= ~CSIZE;

  terminalIos.c_cflag |= CS8;            // force 8 bit input
  terminalIos.c_cflag |= CLOCAL | CREAD; // enable recvr

  // no software flow control
  terminalIos.c_cflag &= ~(IXON | IXOFF | IXANY);

  // One input byte is enough to return from read()
  // Inter-character timer off
  terminalIos.c_cc[VMIN] = 0;
  terminalIos.c_cc[VTIME] = 0;

  tcflush(fd, TCIFLUSH);
  // Setting Configuration
  if (tcsetattr(fd, TCSANOW, &terminalIos) != 0) {
    fprintf(stderr, "Failure in setting configuration!\n");
    perror("tcsetattr");
    close(fd);
    return -EXIT_FAILURE;
  }

  return fd; // device handle
}

static void usage(const char *prog) {
  fprintf(stderr, "Usage: %s -p <device_path> -b <baud_rate> [-x:Debug Flag]\n",
          prog);
  fprintf(stderr, "Example: %s -p /dev/ttyUSB0 -b 115200 -x\n", prog);
  fprintf(stderr,
          "  -p <device_path> : Path to Serial Device (e.g., /dev/ttyUSB0)\n");
  fprintf(stderr, "  -b <baud_rate>   : Baud Rate (e.g., 9600, 115200)\n");
  fprintf(stderr, "  -x               : Enable Debug Mode (optional)\n");
  fprintf(stderr, "  -h               : Show this help message\n");
}

void send_data_to_device(int dev_handle, const char *message, int length) {}

int main(int argc, char *argv[]) {
  const char *dev_path = NULL;
  long baud_rate = 0;
  int debug = 0;
  int opt;

  // optstring: p: and b: require arguments; x is a flag.
  while ((opt = getopt(argc, argv, ":p:b:xh")) != -1) {
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

  if (!dev_path || baud_rate == 0) {
    fprintf(stderr, "Missing required -p and/or -b\n");
    usage(argv[0]);
    return 2;
  }

  fprintf(stdout, "Info Used: \n");
  fprintf(stdout, "Device: %s\n", dev_path);
  fprintf(stdout, "Baud: %ld bauds\n", baud_rate);
  fprintf(stdout, "Debug: %s\n", debug ? "on" : "off");

  conf.device_path = dev_path;
  conf.baud_rate = baud_rate;
  conf.debug_mode = debug;

  fprintf(stdout, "Opening Serial Port...\n");
  int dev_handle = serial_port_open(dev_path, baud_rate);
  if (dev_handle < 0) {
    fprintf(stderr, "Failed to open serial port %s\n", dev_path);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
