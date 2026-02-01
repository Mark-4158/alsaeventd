/*
    ALSA Event Daemon
    Copyright (C) 2023-2024  Mark A. Williams, Jr.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>

#include <alsa/asoundlib.h>
#include <canberra.h>


static volatile sig_atomic_t sigfd;


static inline void sa_handler_cb(int sig)
{
    write(sigfd, &sig, sizeof sig);
}

static inline void ctl_exit_cb(int, void *ctlp)
{
    snd_ctl_close(ctlp);
}

static inline void ca_proplist_exit_cb(int, void *propp)
{
    ca_proplist_destroy(propp);
}

static inline void ca_context_exit_cb(int, void *ctxp)
{
    ca_context_destroy(ctxp);
}

static inline int sys_kill(pid_t pid, int sig)
{
    return syscall(SYS_tgkill, pid, pid, sig);
}

static inline void ca_context_play_cb(ca_context *, uint32_t, int, void *tgid)
{
    sys_kill((intptr_t)tgid, SIGINT);
}

static inline int sigfd_init(void)
{
    int pipefd[] = { -1, -1 };

    syscall(SYS_pipe2, pipefd, O_CLOEXEC | O_NONBLOCK);
    sigfd = pipefd[1];

    return pipefd[0];
}

static inline void sigset_init(sigset_t *const set)
{
    static const int sigv[] = {
        SIGHUP,
        SIGINT,
        SIGURG,
        SIGUSR1,
        SIGUSR2,
        SIGALRM,
        SIGTERM,
    };
    register int i = sizeof sigv / sizeof *sigv;

    struct sigaction sa[] = {
        { .sa_handler = sa_handler_cb },
    };

    sigfillset(&sa->sa_mask);

    sigemptyset(set);
    do {
        sigaddset(set, sigv[--i]);
    } while (i);

    i = sizeof sigv / sizeof *sigv;
    do {
        sigaction(sigv[--i], sa, NULL);
    } while (i);
}

static void async_init(const char* const path)
{
    register int i = 2;

    do {
        register const int fd = open(i ? _PATH_DEV "disk/by-uuid" : path,
                                     O_ASYNC | O_CLOEXEC | O_DIRECTORY);
        register int j = 1;

        do {
            fcntl(fd,
                  (static const int[]){ 0x402, 0xA }[j],
                  (static const int[][3]){
                      { 0x80000004, 0x80000008, 0x80000004 },
                      { SIGURG, SIGUSR1, SIGUSR2 },
                  }[j][i]);
        } while (j--);
    } while (i--);
}

int main(int argc, char *argv[])
{
    pid_t ppgid = -1;

    void *s = "Yaru",
         *p = getenv("GRIM_DEFAULT_DIR"),
         *tgid = "alsa";
    size_t n = sizeof "service-login";

    snd_ctl_t *ctlp = NULL;
    snd_ctl_event_t *evp = NULL;

    ca_proplist *propp = NULL;
    ca_context *ctxp = NULL;

    const int sigfd = sigfd_init();
    sigset_t setp[1];

    sigset_init(setp);
    sigprocmask(SIG_BLOCK, setp, NULL);

    for (register int opt; (opt = getopt(argc, argv, "+kht:d:b:")) != -1;) {
        if (opt == 'k') {
            ppgid = getpgid(getppid());
        } else if (opt == 't') {
            s = optarg;
        } else if (opt == 'd') {
            p = optarg;
        } else if (opt == 'b') {
            tgid = optarg;
        } else if (opt == 'h') {
            printf("\n"
                   "Usage:\n"
                   " alsaeventd [options]\n"
                   "\n"
                   "Options:\n"
                   " -b <name>  pick libcanberra audio backend (default: '%s')\n"
                   " -d <dir>   pick screenshots path to watch (default: '%s')\n"
                   " -t <name>  pick XDG sound theme (default: '%s')\n"
                   " \n"
                   " -k  kill the parent process group when killed\n"
                   " -h  display this help\n",
                   tgid, p, s);
            return EXIT_SUCCESS;
        } else {
            return EXIT_FAILURE;
        }
    }

    async_init(p);

    snd_ctl_open(&ctlp, "default", SND_CTL_READONLY);
    on_exit(ctl_exit_cb, ctlp);
    snd_ctl_subscribe_events(ctlp, 1);
    snd_ctl_event_alloca(&evp);

    ca_proplist_create(&propp);
    on_exit(ca_proplist_exit_cb, propp);
    ca_proplist_sets(propp, CA_PROP_CANBERRA_XDG_THEME_NAME, s);
    ca_proplist_set(propp, CA_PROP_CANBERRA_CACHE_CONTROL,
                    "volatile", sizeof "volatile");
    ca_context_create(&ctxp);
    on_exit(ca_context_exit_cb, ctxp);
    ca_context_set_driver(ctxp, tgid);

    s = "service-login";
    p = NULL;
    tgid = NULL;
    sigprocmask(SIG_UNBLOCK, setp, NULL);
    sigdelset(setp, SIGINT);

    snd_ctl_wait(ctlp, -1);
    while (snd_ctl_read(ctlp, evp) > 0) {
        snd_ctl_wait(ctlp, 250);
    }
    goto CA_PLAY_EVENT;

    for (;;) {
        int sig;

        s = &sig;
        n = sizeof sig;
        for (ssize_t sz; (sz = read(sigfd, s, n)) &&
                         (sz != -1 ? s += sz, n -= sz : errno == EINTR);) { }

        if (n) {
            if ((snd_ctl_wait(ctlp, -1), snd_ctl_read(ctlp, evp)) <= 0 ||
                snd_ctl_event_get_type(evp) != SND_CTL_EVENT_ELEM ||
                snd_ctl_event_elem_get_interface(evp) !=
                    SND_CTL_ELEM_IFACE_MIXER ||
                snd_ctl_event_elem_get_mask(evp) != SND_CTL_EVENT_MASK_VALUE) {
                continue;
            }
            s = "audio-volume-change";
            n = sizeof "audio-volume-change";
        } else if (sig == SIGHUP || sig == SIGTERM) {
            sigprocmask(SIG_BLOCK, setp, NULL);
            s = "service-logout";
            p = ca_context_play_cb;
            tgid = (void *)(intptr_t)getpid();
            n = sizeof "service-logout";
        } else if (sig == SIGURG) {
            s = "screen-capture";
            n = sizeof "screen-capture";
        } else if (sig == SIGUSR1) {
            s = "device-added";
            n = sizeof "device-added";
        } else if (sig == SIGUSR2) {
            s = "device-removed";
            n = sizeof "device-removed";
        } else if (sig == SIGALRM) {
            s = "alarm-clock-elapsed";
            n = sizeof "alarm-clock-elapsed";
        } else if (sig == SIGINT) {
            if (ppgid > 1) {
                kill(-ppgid, SIGHUP);
            }
            return EXIT_SUCCESS;
        } else {
            continue;
        }

CA_PLAY_EVENT:
        ca_proplist_set(propp, CA_PROP_EVENT_ID, s, n);
        ca_context_play_full(ctxp, 0, propp, p, tgid);
    }

    return EXIT_FAILURE;
}
