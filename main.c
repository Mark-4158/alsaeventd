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

#include <canberra.h>
#include <alsa/asoundlib.h>

#include <sys/syscall.h>
#include <paths.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t sigfd[2];

static inline void sa_handler_cb(int sig)
{
    write(sigfd[1], &sig, sizeof sig);
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

static inline void ca_context_play_cb(ca_context *, uint32_t, int, void *tgid)
{
    syscall(SYS_tgkill, (intptr_t)tgid, (intptr_t)tgid, SIGINT);
}

int main(int argc, char *argv[])
{
    void *s = "Yaru",
         *p = getenv("GRIM_DEFAULT_DIR"),
         *tgid = "alsa";
    size_t n;

    snd_ctl_t *ctlp = NULL;
    snd_ctl_event_t *evp = NULL;

    ca_proplist *propp = NULL;
    ca_context *ctxp = NULL;

    sigset_t ssetp[1];
    sigemptyset(ssetp);

    syscall(SYS_pipe2, sigfd, O_CLOEXEC | O_NONBLOCK);
    {
        const int *sigp = (const int[8]){
            SIGHUP,
            SIGINT,
            SIGUSR1,
            SIGUSR2,
            SIGALRM,
            SIGTERM,
            SIGPOLL,
        };

        do {
            if (!sigaddset(ssetp, *sigp)) {
                struct sigaction actp[1];

                sigaction(*sigp, NULL, actp);
                actp->sa_handler = sa_handler_cb;
                sigfillset(&actp->sa_mask);
                sigdelset(&actp->sa_mask, *sigp);
                sigaction(*sigp, actp, NULL);
            }
        } while (*++sigp);
    }
    sigprocmask(SIG_BLOCK, ssetp, NULL);

    for (;;) {
        switch (getopt(argc, argv, "t:d:b:h")) {
        case -1:
            break;
        case 't':
            s = optarg;
            continue;
        case 'd':
            p = optarg;
            continue;
        case 'b':
            tgid = optarg;
            continue;
        case 'h':
            printf("\n"
                   "Usage:\n"
                   " alsaeventd [options]\n"
                   "\n"
                   "Options:\n"
                   " -b <name>  pick libcanberra audio backend (default: '%s')\n"
                   " -d <dir>   pick screenshots path to watch (default: '%s')\n"
                   " -t <name>  pick XDG sound theme (default: '%s')\n"
                   " \n"
                   " -h  display this help\n",
                   tgid, p, s);
            return EXIT_SUCCESS;
        default:
            return EXIT_FAILURE;
        }

        break;
    }

    {
        register int i = 2;

        do {
            register const int fd = open(i ? _PATH_DEV "disk/by-uuid" : p,
                                         O_ASYNC | O_CLOEXEC | O_DIRECTORY);
            register int j = 1;

            do {
                fcntl(fd,
                      (const int[]){ 0x402, 0xA }[j],
                      (const int[][3]){
                          { 0x80000004, 0x80000008, 0x80000004 },
                          { SIGPOLL, SIGUSR2, SIGUSR1 },
                      }[j][i]);
            } while (j--);
        } while (i--);
    }

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
    n = sizeof "service-login";
    sigprocmask(SIG_UNBLOCK, ssetp, NULL);

    for (;;) {
        if (snd_ctl_read(ctlp, evp) <= 0)
            goto CA_PLAY_EVENT;
    }

    for (;;) {
        switch (read(sigfd[0], s = (int[1]){ }, sizeof(int)), *(const int *)s) {
        case SIGUSR1:
            s = "device-added";
            n = sizeof "device-added";
            break;
        case SIGUSR2:
            s = "device-removed";
            n = sizeof "device-removed";
            break;
        case SIGPOLL:
            s = "screen-capture";
            n = sizeof "screen-capture";
            break;
        case SIGALRM:
            s = "alarm-clock-elapsed";
            n = sizeof "alarm-clock-elapsed";
            break;
        case SIGHUP:
        case SIGTERM:
            s = "service-logout";
            p = ca_context_play_cb;
            tgid = (void *)(intptr_t)getpid();
            n = sizeof "service-logout";
            sigdelset(ssetp, SIGINT);
            sigprocmask(SIG_BLOCK, ssetp, NULL);
            lseek(sigfd[0], 0, SEEK_END);
            break;
        case SIGINT:
            return EXIT_SUCCESS;
        case 0:
            snd_ctl_wait(ctlp, -1);
            if (snd_ctl_read(ctlp, evp) > 0 &&
                snd_ctl_event_get_type(evp) == SND_CTL_EVENT_ELEM &&
                snd_ctl_event_elem_get_interface(evp)
                    == SND_CTL_ELEM_IFACE_MIXER &&
                snd_ctl_event_elem_get_mask(evp) == SND_CTL_EVENT_MASK_VALUE) {
                s = "audio-volume-change";
                n = sizeof "audio-volume-change";
                break;
            }
            [[fallthrough]];
        default:
            continue;
        }

CA_PLAY_EVENT:
        ca_proplist_set(propp, CA_PROP_EVENT_ID, s, n);
        ca_context_play_full(ctxp, 0, propp, p, tgid);
    }

    return EXIT_FAILURE;
}
