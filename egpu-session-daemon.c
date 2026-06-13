/*
 * egpu-session-daemon.c
 * Detects NVIDIA eGPU via sysfs and switches sessions in AccountsService
 * atomically. Ready for systemd (Type=notify).
 *
 * Build: make (see Makefile)
 * Systemd service: Type=notify, ExecStart=/usr/local/bin/egpu-session-daemon %I
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <stdbool.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <signal.h>
#include <systemd/sd-daemon.h>

#define NVIDIA_VENDOR     "0x10de"
#define PCI_DIR           "/sys/bus/pci/devices"
#define CONF_DIR          "/var/lib/AccountsService/users"

#define MAX_RETRIES       20          /* 10 seconds total to make up for Thunderbolt latency */
#define SLEEP_NSEC        500000000L  /* 0.5 seconds in nanoseconds */
#define USERNAME_MAX      32
#define CONF_LINE_MAX     512
#define PATH_MAX_LEN      512

static volatile sig_atomic_t g_terminate = 0;
static char g_tmp_path[PATH_MAX_LEN] = {0};

/* ---------- Security Utilities ---------- */

/* Locale-independent validation: avoids ctype.h to ensure consistent
 * behavior regardless of system locale (critical for a root daemon). */
static bool valid_user(const char *u)
{
    if (!u || *u == '\0' || *u == '-' || strlen(u) > USERNAME_MAX)
        return false;

    /* The first character must be alphabetic (POSIX convention) */
    if (!((*u >= 'a' && *u <= 'z') || (*u >= 'A' && *u <= 'Z')))
        return false;

    for (const char *p = u + 1; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '.' || *p == '-'))
            return false;
    }
    return true;
}

static void cleanup_tmp(void)
{
    if (g_tmp_path[0] != '\0')
        unlink(g_tmp_path);
}

static void signal_handler(int sig)
{
    (void)sig;
    g_terminate = 1;
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

static void nsleep(long nsec)
{
    struct timespec req = {0, nsec};
    struct timespec rem;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        if (g_terminate) return;
        req = rem;
    }
}

/* ---------- eGPU Detection ---------- */

static bool is_display_class(const char *class_id)
{
    unsigned int val = 0;
    if (sscanf(class_id, "0x%x", &val) != 1)
        return false;
    /* Display controller major class = 0x03 */
    return ((val >> 16) & 0xFF) == 0x03;
}

static bool is_thunderbolt_device(const char *devname)
{
    /*
     * Verify if the device is behind a Thunderbolt bridge.
     * Read the bus branch: /sys/bus/pci/devices/XXXX:YY:ZZ.A/
     * If there is a Thunderbolt bridge (vendor 0x8086 class 0x060400) in any ancestor,
     * it is very likely to be an external eGPU.
     */
    char path[PATH_MAX_LEN];
    char real_path[PATH_MAX_LEN];

    snprintf(path, sizeof(path), "%s/%s", PCI_DIR, devname);
    if (realpath(path, real_path) == NULL)
        return false;

    /* go up the sysfs hierarchy looking for bridges */
    char *slash = strrchr(real_path, '/');
    while (slash) {
        *slash = '\0';

        char vendor_path[PATH_MAX_LEN + 8];
        char class_path[PATH_MAX_LEN + 8];
        int vn = snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", real_path);
        int cn = snprintf(class_path,  sizeof(class_path),  "%s/class",  real_path);
        if (vn < 0 || (size_t)vn >= sizeof(vendor_path) ||
            cn < 0 || (size_t)cn >= sizeof(class_path)) {
            slash = strrchr(real_path, '/');
            continue;
        }

        FILE *vf = fopen(vendor_path, "r");
        FILE *cf = fopen(class_path,  "r");
        if (vf && cf) {
            char vbuf[16] = {0};
            char cbuf[16] = {0};
            if (fgets(vbuf, sizeof(vbuf), vf) && fgets(cbuf, sizeof(cbuf), cf)) {
                vbuf[strcspn(vbuf, "\n")] = '\0';
                cbuf[strcspn(cbuf, "\n")] = '\0';

                unsigned int cls = 0;
                sscanf(cbuf, "0x%x", &cls);

                /* Intel vendor + PCI-to-PCI bridge = probable Thunderbolt */
                if (strcmp(vbuf, "0x8086") == 0 && ((cls >> 8) == 0x0604)) {
                    fclose(vf);
                    fclose(cf);
                    return true;
                }
            }
        }
        if (vf) fclose(vf);
        if (cf) fclose(cf);

        slash = strrchr(real_path, '/');
    }

    return false;
}

static bool detect_nvidia(void)
{
    DIR *dir = opendir(PCI_DIR);
    if (!dir) {
        syslog(LOG_ERR, "Failed to open %s: %s", PCI_DIR, strerror(errno));
        return false;
    }

    struct dirent *entry;
    char path[PATH_MAX_LEN];
    char vendor[32];
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        int n = snprintf(path, sizeof(path), "%s/%s/vendor",
                         PCI_DIR, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            syslog(LOG_WARNING, "path too long for %s, ignoring", entry->d_name);
            continue;
        }

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        if (fgets(vendor, sizeof(vendor), f)) {
            vendor[strcspn(vendor, "\n")] = '\0';

            if (strcmp(vendor, NVIDIA_VENDOR) == 0) {
                char class_path[PATH_MAX_LEN];
                snprintf(class_path, sizeof(class_path),
                         "%s/%s/class", PCI_DIR, entry->d_name);

                FILE *cf = fopen(class_path, "r");
                if (cf) {
                    char class_id[16];
                    if (fgets(class_id, sizeof(class_id), cf)) {
                        if (is_display_class(class_id)) {
                            /*
                             * If there are multiple NVIDIA devices, prioritize the one
                             * that is on Thunderbolt. If there is only one, use it.
                             */
                            if (is_thunderbolt_device(entry->d_name)) {
                                found = true;
                                syslog(LOG_INFO,
                                       "NVIDIA eGPU detected on Thunderbolt: %s",
                                       entry->d_name);
                                fclose(cf);
                                fclose(f);
                                break;
                            }
                            /* If it's not on Thunderbolt, we save it but keep looking */
                            found = true;
                            syslog(LOG_INFO,
                                   "NVIDIA GPU detected (non-TB): %s",
                                   entry->d_name);
                        }
                    }
                    fclose(cf);
                }

                if (found) {
                    fclose(f);
                    break;
                }
            }
        }
        fclose(f);
    }
    closedir(dir);
    return found;
}

/* ---------- Atomic Write Utilities ---------- */

static bool set_session_atomic(const char *user, const char *session_type)
{
    char conf_path[PATH_MAX_LEN];
    char tmp_path[PATH_MAX_LEN];

    snprintf(conf_path, sizeof(conf_path), "%s/%s", CONF_DIR, user);
    snprintf(tmp_path,  sizeof(tmp_path),  "%s/.%s.XXXXXX", CONF_DIR, user);

    /*
     * Single lstat check: verify the file is regular (not a symlink) and
     * cache stat data for permission replication. No second lstat avoids TOCTOU.
     */
    bool orig_exists = false;
    struct stat st_orig;
    if (lstat(conf_path, &st_orig) == 0) {
        if (!S_ISREG(st_orig.st_mode)) {
            syslog(LOG_ERR, "%s is not a regular file (possible symlink attack)",
                   conf_path);
            return false;
        }
        orig_exists = true;
    } else if (errno == ENOENT) {
        syslog(LOG_WARNING, "%s does not exist, creating a new one", conf_path);
    } else {
        syslog(LOG_ERR, "lstat failed on %s: %s", conf_path, strerror(errno));
        return false;
    }

    /* Create temporary file with mkstemp (safe against race conditions) */
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        syslog(LOG_ERR, "mkstemp failed: %s", strerror(errno));
        return false;
    }

    /* Save for cleanup in case of signal */
    snprintf(g_tmp_path, sizeof(g_tmp_path), "%s", tmp_path);

    /* Replicate permissions and owner from original (cached, no TOCTOU) */
    if (orig_exists) {
        if (fchmod(fd, st_orig.st_mode) != 0)
            syslog(LOG_WARNING, "fchmod failed: %s", strerror(errno));
        if (fchown(fd, st_orig.st_uid, st_orig.st_gid) != 0)
            syslog(LOG_WARNING, "fchown failed: %s", strerror(errno));
    } else {
        /* Reasonable defaults for AccountsService */
        if (fchmod(fd, 0644) != 0)
            syslog(LOG_WARNING, "fchmod (default) failed: %s", strerror(errno));
        if (fchown(fd, 0, 0) != 0)
            syslog(LOG_WARNING, "fchown (default) failed: %s", strerror(errno));
    }

    FILE *dst = fdopen(fd, "w");
    if (!dst) {
        syslog(LOG_ERR, "fdopen failed: %s", strerror(errno));
        close(fd);
        unlink(tmp_path);
        g_tmp_path[0] = '\0';
        return false;
    }

    /* Copy and modify if file exists. If not, create from scratch. */
    bool session_set = false;
    bool has_user_section = false;
    FILE *src = fopen(conf_path, "r");

    if (src) {
        /* File locking to avoid race conditions with another instance */
        if (flock(fileno(src), LOCK_SH) != 0)
            syslog(LOG_WARNING, "Failed to obtain read lock: %s", strerror(errno));

        char line[CONF_LINE_MAX];
        while (fgets(line, sizeof(line), src)) {
            if (strncmp(line, "[User]", 6) == 0)
                has_user_section = true;
            if (strncmp(line, "Session=", 8) == 0) {
                fprintf(dst, "Session=%s\n", session_type);
                session_set = true;
            } else {
                fputs(line, dst);
            }
        }

        flock(fileno(src), LOCK_UN);
        fclose(src);
    }

    if (!session_set) {
        /* Only write [User] header if not already present in the copied content */
        if (!has_user_section)
            fprintf(dst, "[User]\n");
        fprintf(dst, "Session=%s\n", session_type);
    }

    /* Force flush to disk BEFORE the atomic rename */
    if (fflush(dst) != 0 || fsync(fd) != 0) {
        syslog(LOG_ERR, "fsync failed: %s", strerror(errno));
        fclose(dst);
        unlink(tmp_path);
        g_tmp_path[0] = '\0';
        return false;
    }

    fclose(dst);

    /* Atomic rename: this guarantees consistency even if power is lost */
    if (rename(tmp_path, conf_path) != 0) {
        syslog(LOG_ERR, "rename failed: %s", strerror(errno));
        unlink(tmp_path);
        g_tmp_path[0] = '\0';
        return false;
    }

    /* Sync the directory entry to ensure the rename is durable on disk */
    int dir_fd = open(CONF_DIR, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    g_tmp_path[0] = '\0';
    syslog(LOG_INFO, "Session changed to %s for user %s", session_type, user);
    return true;
}

/* ---------- Main ---------- */

int main(int argc, char *argv[])
{
    openlog("egpu-session-daemon", LOG_PID, LOG_DAEMON);
    setup_signals();
    atexit(cleanup_tmp);

    const char *user = getenv("TARGET_USER");
    if (!user && argc > 1)
        user = argv[1];

    if (!user) {
        syslog(LOG_ERR, "User not specified. Usage: %s <user> or set TARGET_USER",
               argv[0]);
        closelog();
        return EXIT_FAILURE;
    }

    if (!valid_user(user)) {
        syslog(LOG_ERR, "Invalid user name or potentially malicious: %s", user);
        closelog();
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO, "Initializing eGPU detection for user %s", user);

    bool has_egpu = false;
    for (int i = 0; i < MAX_RETRIES && !g_terminate; i++) {
        if (detect_nvidia()) {
            has_egpu = true;
            break;
        }
        syslog(LOG_DEBUG, "Attempt %d/%d: eGPU not detected, waiting...",
               i + 1, MAX_RETRIES);
        nsleep(SLEEP_NSEC);
    }

    if (g_terminate) {
        syslog(LOG_INFO, "Signal received, terminating without changes");
        closelog();
        return EXIT_SUCCESS;
    }

    /*
     * Personalize sessions that you want to use.
     * eGPU detected  -> use the heavy session (e.g., KDE Plasma with effects)
     * Only iGPU       -> use the light session (e.g., GNOME or Sway)
     */
    const char *session = has_egpu ? "plasma" : "gnome";

    if (!set_session_atomic(user, session)) {
        syslog(LOG_ERR, "Failed to change session, aborting");
        closelog();
        return EXIT_FAILURE;
    }

    sd_notify(0, "READY=1");
    syslog(LOG_INFO, "Daemon terminated correctly. Session: %s", session);
    closelog();

    return EXIT_SUCCESS;
}
