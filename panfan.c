#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define ACPI_CALL "/proc/acpi/call"
#define EC_PATH "\\_SB.PC00.LPCB.EC0"
#define DEFAULT_CONFIG "/etc/panfan.conf"
#define MODE_LOCK "/run/lock/panfan-mode.lock"
#define DAEMON_LOCK "/run/lock/panfan-daemon.lock"
#define RUNTIME_DIR "/run/panfan"
#define ACTIVE_MARKER RUNTIME_DIR "/active"
#define SLEEP_MARKER RUNTIME_DIR "/sleep"

struct policy {
    int poll_ms;
    int start_mc;
    int stop_mc;
    int emergency_mc;
    int min_state;
    int max_state;
    int step_up;
    int step_down;
};

struct hardware {
    char vendor[128];
    char product[128];
    char sku[128];
    char fan[PATH_MAX];
    char sensor[PATH_MAX];
    int fan_max;
};

static volatile sig_atomic_t stopping;
static bool use_syslog;

static void message(int priority, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    if (use_syslog)
        vsyslog(priority, format, args);
    else {
        vfprintf(stderr, format, args);
        fputc('\n', stderr);
    }
    va_end(args);
}

static int write_all(int fd, const void *data, size_t length)
{
    const char *cursor = data;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}

static int read_text(const char *path, char *buffer, size_t size)
{
    int fd;
    ssize_t length;
    char *start;

    if (size < 2) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    do {
        length = read(fd, buffer, size - 1);
    } while (length < 0 && errno == EINTR);
    if (close(fd) < 0 && length >= 0)
        return -1;
    if (length < 0)
        return -1;
    buffer[length] = '\0';

    start = buffer;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != buffer)
        memmove(buffer, start, strlen(start) + 1);
    length = (ssize_t)strlen(buffer);
    while (length > 0 && isspace((unsigned char)buffer[length - 1]))
        buffer[--length] = '\0';
    return 0;
}

static int read_integer(const char *path, int *value)
{
    char buffer[64];
    char *end;
    long parsed;

    if (read_text(path, buffer, sizeof(buffer)) < 0)
        return -1;
    errno = 0;
    parsed = strtol(buffer, &end, 10);
    if (errno || end == buffer || *end || parsed < INT_MIN || parsed > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int write_integer(const char *path, int value)
{
    char buffer[32];
    int fd;
    int length;

    length = snprintf(buffer, sizeof(buffer), "%d\n", value);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        errno = EOVERFLOW;
        return -1;
    }
    fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (write_all(fd, buffer, (size_t)length) < 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    return close(fd);
}

static int child_path(char *output, size_t size, const char *base, const char *child)
{
    int length = snprintf(output, size, "%s/%s", base, child);

    if (length < 0 || (size_t)length >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int find_thermal_device(const char *prefix, const char *wanted, char *output, size_t size)
{
    const char *root = "/sys/class/thermal";
    struct dirent *entry;
    DIR *directory;
    int matches = 0;

    directory = opendir(root);
    if (!directory)
        return -1;
    for (;;) {
        char base[PATH_MAX];
        char type_path[PATH_MAX];
        char type[128];

        errno = 0;
        entry = readdir(directory);
        if (!entry)
            break;
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
            continue;
        if (snprintf(base, sizeof(base), "%s/%s", root, entry->d_name) < 0 ||
            child_path(type_path, sizeof(type_path), base, "type") < 0)
            continue;
        if (read_text(type_path, type, sizeof(type)) < 0)
            continue;
        if (strcmp(type, wanted) != 0)
            continue;
        matches++;
        if (strlen(base) >= size) {
            closedir(directory);
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(output, base);
    }
    if (errno) {
        int saved = errno;

        closedir(directory);
        errno = saved;
        return -1;
    }
    closedir(directory);
    if (matches != 1) {
        message(LOG_ERR, "expected one %s device, found %d", wanted, matches);
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static int hardware_open(struct hardware *hardware)
{
    char path[PATH_MAX];

    memset(hardware, 0, sizeof(*hardware));
    if (read_text("/sys/class/dmi/id/sys_vendor", hardware->vendor, sizeof(hardware->vendor)) < 0 ||
        read_text("/sys/class/dmi/id/product_name", hardware->product, sizeof(hardware->product)) <
            0) {
        message(LOG_ERR, "cannot read DMI identity: %s", strerror(errno));
        return -1;
    }
    if (strncmp(hardware->vendor, "Panasonic", 9) != 0 ||
        strcmp(hardware->product, "CFSC-2") != 0) {
        message(LOG_ERR, "unsupported system: %s %s", hardware->vendor, hardware->product);
        errno = ENODEV;
        return -1;
    }
    if (read_text("/sys/class/dmi/id/product_sku", hardware->sku, sizeof(hardware->sku)) < 0)
        strcpy(hardware->sku, "unknown");
    if (find_thermal_device("cooling_device", "TFN1", hardware->fan, sizeof(hardware->fan)) < 0 ||
        find_thermal_device("thermal_zone", "x86_pkg_temp", hardware->sensor,
                            sizeof(hardware->sensor)) < 0)
        return -1;
    if (child_path(path, sizeof(path), hardware->fan, "max_state") < 0 ||
        read_integer(path, &hardware->fan_max) < 0) {
        message(LOG_ERR, "cannot read TFN1 maximum state: %s", strerror(errno));
        return -1;
    }
    if (hardware->fan_max != 50) {
        message(LOG_ERR, "unexpected TFN1 maximum state: %d", hardware->fan_max);
        errno = ERANGE;
        return -1;
    }
    if (access(ACPI_CALL, R_OK | W_OK) < 0) {
        message(LOG_ERR, "%s is unavailable: %s", ACPI_CALL, strerror(errno));
        return -1;
    }
    return 0;
}

static int fan_read(const struct hardware *hardware, int *state)
{
    char path[PATH_MAX];

    return child_path(path, sizeof(path), hardware->fan, "cur_state") < 0
               ? -1
               : read_integer(path, state);
}

static int fan_write(const struct hardware *hardware, int state)
{
    char path[PATH_MAX];
    int readback;

    if (state < 0 || state > hardware->fan_max) {
        errno = ERANGE;
        return -1;
    }
    if (child_path(path, sizeof(path), hardware->fan, "cur_state") < 0 ||
        write_integer(path, state) < 0 || fan_read(hardware, &readback) < 0)
        return -1;
    if (readback != state) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int temperature_read(const struct hardware *hardware, int *temperature)
{
    char path[PATH_MAX];

    return child_path(path, sizeof(path), hardware->sensor, "temp") < 0
               ? -1
               : read_integer(path, temperature);
}

static int lock_file(const char *path, bool nonblocking)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    int operation = LOCK_EX | (nonblocking ? LOCK_NB : 0);

    if (fd < 0)
        return -1;
    while (flock(fd, operation) < 0) {
        if (errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    return fd;
}

static int acpi_evaluate(const char *expression, char *result, size_t size)
{
    char command[256];
    int fd;
    int length;

    length = snprintf(command, sizeof(command), "%s\n", expression);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        errno = EOVERFLOW;
        return -1;
    }

    fd = open(ACPI_CALL, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (write_all(fd, command, (size_t)length) < 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0 || read_text(ACPI_CALL, result, size) < 0)
        return -1;
    if (strncmp(result, "Error", 5) == 0) {
        message(LOG_ERR, "ACPI evaluation failed: %s", result);
        errno = EIO;
        return -1;
    }
    return 0;
}

static int mode_read_locked(bool *passive)
{
    char result[128];

    if (acpi_evaluate(EC_PATH ".CEFM", result, sizeof(result)) < 0)
        return -1;
    if (strcmp(result, "0x0") == 0)
        *passive = false;
    else if (strcmp(result, "0x1") == 0)
        *passive = true;
    else {
        message(LOG_ERR, "unexpected CEFM result: %s", result);
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int mode_set(const struct hardware *hardware, bool passive)
{
    char result[128];
    bool current;
    int lock = lock_file(MODE_LOCK, false);
    int status = -1;

    if (lock < 0)
        return -1;
    if (!passive && fan_write(hardware, hardware->fan_max) < 0)
        goto done;
    if (mode_read_locked(&current) < 0)
        goto done;
    if (current != passive) {
        if (passive && fan_write(hardware, hardware->fan_max) < 0)
            goto done;
        if (acpi_evaluate(passive ? EC_PATH ".SEFM 1" : EC_PATH ".SEFM 0", result, sizeof(result)) <
            0)
            goto done;
    }
    if (mode_read_locked(&current) < 0 || current != passive) {
        errno = EIO;
        goto done;
    }
    if (!passive && fan_write(hardware, hardware->fan_max) < 0)
        goto done;
    status = 0;

done:
    close(lock);
    return status;
}

static void policy_defaults(struct policy *policy)
{
    *policy = (struct policy){
        .poll_ms = 1000,
        .start_mc = 45000,
        .stop_mc = 42000,
        .emergency_mc = 80000,
        .min_state = 15,
        .max_state = 50,
        .step_up = 5,
        .step_down = 5,
    };
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

static int parse_long(const char *text, long *value)
{
    char *end;

    errno = 0;
    *value = strtol(text, &end, 10);
    if (errno || end == text || *end)
        return -1;
    return 0;
}

static int policy_validate(const struct policy *policy)
{
    if (policy->poll_ms < 250 || policy->poll_ms > 2000 || policy->stop_mc < 0 ||
        policy->stop_mc >= policy->start_mc || policy->start_mc >= policy->emergency_mc ||
        policy->emergency_mc > 110000 || policy->min_state < 1 ||
        policy->min_state > policy->max_state || policy->max_state > 50 || policy->step_up < 1 ||
        policy->step_up > 50 || policy->step_down < 1 || policy->step_down > 50) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int policy_load(struct policy *policy, const char *path, bool required)
{
    enum {
        SEEN_POLL = 1 << 0,
        SEEN_START = 1 << 1,
        SEEN_STOP = 1 << 2,
        SEEN_EMERGENCY = 1 << 3,
        SEEN_MIN = 1 << 4,
        SEEN_MAX = 1 << 5,
        SEEN_UP = 1 << 6,
        SEEN_DOWN = 1 << 7
    };
    char line[512];
    unsigned int seen = 0;
    unsigned int number = 0;
    FILE *file;

    policy_defaults(policy);
    file = fopen(path, "re");
    if (!file) {
        if (!required && errno == ENOENT)
            return 0;
        message(LOG_ERR, "cannot open policy %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        char *comment;
        char *equals;
        char *key;
        char *value;
        long parsed;
        unsigned int flag;
        int *target;
        bool temperature = false;

        number++;
        if (!strchr(line, '\n') && !feof(file)) {
            message(LOG_ERR, "%s:%u: line is too long", path, number);
            goto invalid;
        }
        comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        key = trim(line);
        if (!*key)
            continue;
        equals = strchr(key, '=');
        if (!equals || strchr(equals + 1, '=')) {
            message(LOG_ERR, "%s:%u: expected key=value", path, number);
            goto invalid;
        }
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);
        if (parse_long(value, &parsed) < 0 || parsed < INT_MIN || parsed > INT_MAX) {
            message(LOG_ERR, "%s:%u: invalid integer", path, number);
            goto invalid;
        }
        if (strcmp(key, "poll_interval_ms") == 0) {
            flag = SEEN_POLL;
            target = &policy->poll_ms;
        } else if (strcmp(key, "start_temp_c") == 0) {
            flag = SEEN_START;
            target = &policy->start_mc;
            temperature = true;
        } else if (strcmp(key, "stop_temp_c") == 0) {
            flag = SEEN_STOP;
            target = &policy->stop_mc;
            temperature = true;
        } else if (strcmp(key, "emergency_temp_c") == 0) {
            flag = SEEN_EMERGENCY;
            target = &policy->emergency_mc;
            temperature = true;
        } else if (strcmp(key, "min_state") == 0) {
            flag = SEEN_MIN;
            target = &policy->min_state;
        } else if (strcmp(key, "max_state") == 0) {
            flag = SEEN_MAX;
            target = &policy->max_state;
        } else if (strcmp(key, "step_up") == 0) {
            flag = SEEN_UP;
            target = &policy->step_up;
        } else if (strcmp(key, "step_down") == 0) {
            flag = SEEN_DOWN;
            target = &policy->step_down;
        } else {
            message(LOG_ERR, "%s:%u: unknown key %s", path, number, key);
            goto invalid;
        }
        if (seen & flag) {
            message(LOG_ERR, "%s:%u: duplicate key %s", path, number, key);
            goto invalid;
        }
        if (temperature && (parsed < 0 || parsed > 110)) {
            message(LOG_ERR, "%s:%u: temperature is out of range", path, number);
            goto invalid;
        }
        *target = (int)(temperature ? parsed * 1000 : parsed);
        seen |= flag;
    }
    if (ferror(file)) {
        message(LOG_ERR, "cannot read policy %s: %s", path, strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);
    if (policy_validate(policy) < 0) {
        message(LOG_ERR, "%s: inconsistent policy", path);
        return -1;
    }
    return 0;

invalid:
    fclose(file);
    errno = EINVAL;
    return -1;
}

static void policy_print(const struct policy *policy)
{
    printf("poll_interval_ms=%d\n", policy->poll_ms);
    printf("start_temp_c=%d\n", policy->start_mc / 1000);
    printf("stop_temp_c=%d\n", policy->stop_mc / 1000);
    printf("emergency_temp_c=%d\n", policy->emergency_mc / 1000);
    printf("min_state=%d\n", policy->min_state);
    printf("max_state=%d\n", policy->max_state);
    printf("step_up=%d\n", policy->step_up);
    printf("step_down=%d\n", policy->step_down);
}

static int notify_systemd(const char *state)
{
    const char *name = getenv("NOTIFY_SOCKET");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    socklen_t address_length;
    size_t name_length;
    int socket_fd;
    ssize_t sent;

    if (!name || !*name)
        return 0;
    name_length = strlen(name);
    if (name_length >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (name[0] == '@') {
        address.sun_path[0] = '\0';
        memcpy(address.sun_path + 1, name + 1, name_length - 1);
        address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length);
    } else {
        memcpy(address.sun_path, name, name_length + 1);
        address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name_length + 1);
    }
    socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
        return -1;
    do {
        sent = sendto(socket_fd, state, strlen(state), MSG_NOSIGNAL, (struct sockaddr *)&address,
                      address_length);
    } while (sent < 0 && errno == EINTR);
    if (close(socket_fd) < 0 && sent >= 0)
        return -1;
    return sent < 0 ? -1 : 0;
}

static int ensure_runtime_directory(void)
{
    struct stat status;

    if (mkdir(RUNTIME_DIR, 0755) < 0 && errno != EEXIST)
        return -1;
    if (lstat(RUNTIME_DIR, &status) < 0 || !S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static bool marker_exists(const char *path)
{
    struct stat status;

    return lstat(path, &status) == 0 && S_ISREG(status.st_mode);
}

static int marker_create(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);

    if (fd < 0)
        return -1;
    return close(fd);
}

static void marker_remove(const char *path)
{
    if (unlink(path) < 0 && errno != ENOENT)
        message(LOG_WARNING, "cannot remove %s: %s", path, strerror(errno));
}

static void signal_handler(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int install_signal_handlers(void)
{
    const struct sigaction action = {.sa_handler = signal_handler};

    if (sigaction(SIGTERM, &action, NULL) < 0 || sigaction(SIGINT, &action, NULL) < 0)
        return -1;
    return 0;
}

static void timespec_add_ms(struct timespec *time, int milliseconds)
{
    time->tv_sec += milliseconds / 1000;
    time->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (time->tv_nsec >= 1000000000L) {
        time->tv_sec++;
        time->tv_nsec -= 1000000000L;
    }
}

static int sleep_until(const struct timespec *deadline)
{
    int status;

    do {
        status = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (status == EINTR && !stopping);
    if (status && !(status == EINTR && stopping)) {
        errno = status;
        return -1;
    }
    return 0;
}

static int control_once(const struct hardware *hardware, const struct policy *policy,
                        int *temperature, int *state)
{
    bool passive;
    int next;
    int lock;

    if (temperature_read(hardware, temperature) < 0 || fan_read(hardware, state) < 0)
        return -1;
    if (*temperature < -20000 || *temperature > 150000 || *state < 0 ||
        *state > hardware->fan_max) {
        errno = ERANGE;
        return -1;
    }
    next = *state;
    if (*temperature >= policy->emergency_mc)
        next = policy->max_state;
    else if (*temperature >= policy->start_mc) {
        if (next < policy->min_state)
            next = policy->min_state;
        else {
            next += policy->step_up;
            if (next > policy->max_state)
                next = policy->max_state;
        }
    } else if (*temperature <= policy->stop_mc) {
        next -= policy->step_down;
        if (next < 0)
            next = 0;
    }
    if (next == *state || marker_exists(SLEEP_MARKER))
        return 0;

    lock = lock_file(MODE_LOCK, false);
    if (lock < 0)
        return -1;
    if (marker_exists(SLEEP_MARKER)) {
        close(lock);
        return 0;
    }
    if (mode_read_locked(&passive) < 0) {
        int saved = errno;

        close(lock);
        errno = saved;
        return -1;
    }
    if (!passive) {
        close(lock);
        errno = EIO;
        return -1;
    }
    if (fan_write(hardware, next) < 0) {
        int saved = errno;

        close(lock);
        errno = saved;
        return -1;
    }
    close(lock);
    message(LOG_INFO, "temperature=%.1fC fan=%d/%d", *temperature / 1000.0, next,
            hardware->fan_max);
    *state = next;
    return 0;
}

static int run_daemon(const char *config, bool required)
{
    struct hardware hardware;
    struct policy policy;
    struct timespec deadline;
    int daemon_lock = -1;
    int exit_status = 1;
    bool control_session = false;

    use_syslog = true;
    openlog("panfan", LOG_PID, LOG_DAEMON);
    if (policy_load(&policy, config, required) < 0 || hardware_open(&hardware) < 0)
        goto done;
    if (ensure_runtime_directory() < 0) {
        message(LOG_ERR, "cannot create runtime directory: %s", strerror(errno));
        goto done;
    }
    daemon_lock = lock_file(DAEMON_LOCK, true);
    if (daemon_lock < 0) {
        message(LOG_ERR, "another panfan daemon is already running");
        goto done;
    }
    control_session = true;
    if (install_signal_handlers() < 0 || mode_set(&hardware, true) < 0 ||
        marker_create(ACTIVE_MARKER) < 0) {
        message(LOG_ERR, "cannot enter passive control: %s", strerror(errno));
        goto done;
    }
    marker_remove(SLEEP_MARKER);
    if (notify_systemd("READY=1\nSTATUS=Controlling TFN1") < 0) {
        message(LOG_ERR, "cannot notify systemd: %s", strerror(errno));
        goto done;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
        goto done;

    while (!stopping) {
        char status[192];
        int temperature;
        int state;

        if (control_once(&hardware, &policy, &temperature, &state) < 0) {
            message(LOG_ERR, "control cycle failed: %s", strerror(errno));
            goto done;
        }
        snprintf(status, sizeof(status), "WATCHDOG=1\nSTATUS=temperature %.1fC, fan %d/%d",
                 temperature / 1000.0, state, hardware.fan_max);
        if (notify_systemd(status) < 0) {
            message(LOG_ERR, "watchdog notification failed: %s", strerror(errno));
            goto done;
        }
        timespec_add_ms(&deadline, policy.poll_ms);
        if (!stopping && sleep_until(&deadline) < 0) {
            message(LOG_ERR, "control timer failed: %s", strerror(errno));
            goto done;
        }
    }
    exit_status = 0;

done:
    notify_systemd("STOPPING=1");
    marker_remove(ACTIVE_MARKER);
    marker_remove(SLEEP_MARKER);
    if (control_session && mode_set(&hardware, false) < 0) {
        message(LOG_CRIT, "cannot restore firmware control: %s", strerror(errno));
        exit_status = 1;
    }
    if (daemon_lock >= 0)
        close(daemon_lock);
    closelog();
    use_syslog = false;
    return exit_status;
}

static int require_root(void)
{
    if (geteuid() == 0)
        return 0;
    message(LOG_ERR, "this command must run as root");
    errno = EPERM;
    return -1;
}

static int mode_command(bool passive)
{
    struct hardware hardware;
    int daemon_guard;

    if (require_root() < 0 || hardware_open(&hardware) < 0)
        return 1;
    daemon_guard = lock_file(DAEMON_LOCK, true);
    if (daemon_guard < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            message(LOG_ERR, "panfan is running; stop it before changing mode");
        else
            message(LOG_ERR, "cannot lock fan control: %s", strerror(errno));
        return 1;
    }
    if (!passive) {
        marker_remove(ACTIVE_MARKER);
        marker_remove(SLEEP_MARKER);
    }
    if (mode_set(&hardware, passive) < 0) {
        message(LOG_ERR, "cannot enter %s mode: %s", passive ? "passive" : "fail-safe",
                strerror(errno));
        close(daemon_guard);
        return 1;
    }
    close(daemon_guard);
    return 0;
}

static int status_command(bool verbose, const char *config, bool required)
{
    struct hardware hardware;
    struct policy policy;
    bool passive;
    int temperature;
    int state;
    int lock;

    if (require_root() < 0 || policy_load(&policy, config, required) < 0 ||
        hardware_open(&hardware) < 0)
        return 1;
    lock = lock_file(MODE_LOCK, false);
    if (lock < 0 || mode_read_locked(&passive) < 0) {
        if (lock >= 0)
            close(lock);
        message(LOG_ERR, "cannot read fan mode: %s", strerror(errno));
        return 1;
    }
    close(lock);
    if (temperature_read(&hardware, &temperature) < 0 || fan_read(&hardware, &state) < 0) {
        message(LOG_ERR, "cannot read thermal state: %s", strerror(errno));
        return 1;
    }
    if (verbose) {
        printf("vendor=%s\nproduct=%s\nsku=%s\n", hardware.vendor, hardware.product, hardware.sku);
        printf("sensor=%s\nfan_device=%s\n", hardware.sensor, hardware.fan);
    }
    printf("daemon=%s\nmode=%s\nfan_state=%d\nfan_max=%d\ncpu_temp=%.1fC\n",
           marker_exists(ACTIVE_MARKER) ? "active" : "inactive", passive ? "passive" : "active",
           state, hardware.fan_max, temperature / 1000.0);
    if (verbose)
        policy_print(&policy);
    return 0;
}

static bool suspend_action(const char *action)
{
    return strcmp(action, "suspend") == 0 || strcmp(action, "suspend-after-failed-hibernate") == 0;
}

static int sleep_command(bool before, const char *argument)
{
    const char *environment = getenv("SYSTEMD_SLEEP_ACTION");
    const char *action = environment && *environment ? environment : argument;
    struct hardware hardware;
    int status = 0;

    use_syslog = true;
    openlog("panfan", LOG_PID, LOG_DAEMON);
    if (require_root() < 0) {
        status = 1;
        goto done;
    }
    if (!marker_exists(before ? ACTIVE_MARKER : SLEEP_MARKER))
        goto done;
    if (before) {
        if (marker_create(SLEEP_MARKER) < 0) {
            message(LOG_ERR, "cannot record sleep state: %s", strerror(errno));
            status = 1;
            goto done;
        }
        if (suspend_action(action))
            message(LOG_INFO, "preserving passive control for %s", action);
        else if (hardware_open(&hardware) < 0 || mode_set(&hardware, false) < 0) {
            message(LOG_CRIT, "cannot enter sleep fail-safe: %s", strerror(errno));
            status = 1;
        }
    } else if (hardware_open(&hardware) < 0 || mode_set(&hardware, true) < 0) {
        message(LOG_CRIT, "cannot restore passive control after %s: %s", action, strerror(errno));
        if (hardware.fan[0])
            mode_set(&hardware, false);
        status = 1;
    } else {
        marker_remove(SLEEP_MARKER);
        message(LOG_INFO, "restored passive control after %s", action);
    }

done:
    closelog();
    use_syslog = false;
    return status;
}

static void usage(FILE *stream)
{
    fprintf(stream, "usage: panfan COMMAND [CONFIG|ACTION]\n"
                    "commands:\n"
                    "  run [CONFIG]       run the controller\n"
                    "  status             show current state\n"
                    "  check [CONFIG]     validate hardware and policy\n"
                    "  policy [CONFIG]    print the effective policy\n"
                    "  passive            enter OS-controlled mode\n"
                    "  failsafe           enter firmware control at maximum fan\n"
                    "  pre ACTION         system-sleep pre-hook\n"
                    "  post ACTION        system-sleep post-hook\n");
}

int main(int argc, char **argv)
{
    const char *command;
    const char *argument = NULL;
    const char *config = DEFAULT_CONFIG;
    bool required = false;
    struct policy policy;

    if (argc < 2 || argc > 3) {
        usage(stderr);
        return 2;
    }
    command = argv[1];
    if (argc == 3)
        argument = argv[2];
    if (strcmp(command, "--version") == 0) {
        if (argument) {
            usage(stderr);
            return 2;
        }
        puts("panfan " PANFAN_VERSION);
        return 0;
    }
    if ((strcmp(command, "run") == 0 || strcmp(command, "check") == 0 ||
         strcmp(command, "policy") == 0) &&
        argument) {
        config = argument;
        required = true;
    }
    if (strcmp(command, "run") == 0) {
        if (require_root() < 0)
            return 1;
        return run_daemon(config, required);
    }
    if (strcmp(command, "status") == 0 && !argument)
        return status_command(false, config, false);
    if (strcmp(command, "check") == 0)
        return status_command(true, config, required);
    if (strcmp(command, "policy") == 0) {
        if (policy_load(&policy, config, required) < 0)
            return 1;
        policy_print(&policy);
        return 0;
    }
    if (strcmp(command, "passive") == 0 && !argument)
        return mode_command(true);
    if (strcmp(command, "failsafe") == 0 && !argument)
        return mode_command(false);
    if (strcmp(command, "pre") == 0 && argument)
        return sleep_command(true, argument);
    if (strcmp(command, "post") == 0 && argument)
        return sleep_command(false, argument);
    usage(stderr);
    return 2;
}
