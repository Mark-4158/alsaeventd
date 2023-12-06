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
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stddef.h>

#include <canberra.h>
#include <alsa/asoundlib.h>

static volatile sig_atomic_t signo;

static void ca_context_exit_cb(int, void *m_ctx)
{
    ca_context_destroy(m_ctx);
}

static void ca_context_play_cb(ca_context *, uint32_t, int, void *m_pid)
{
    pid_t register tgid = *(pid_t *)m_pid;

    syscall(SYS_tgkill, tgid, tgid, SIGINT);
}

static void ca_proplist_exit_cb(int, void *m_prop)
{
    ca_proplist_destroy(m_prop);
}

static void ctl_exit_cb(int, void *m_ctl)
{
    snd_ctl_close(m_ctl);
}

static inline void sa_handler_cb(int sig)
{
    signo = sig;
}

int main()
{
    const char *s = "service-login";
    size_t n = sizeof "service-login";

    ca_context *m_ctx = NULL;
    ca_proplist *m_prop = NULL;
    ca_finish_callback_t m_cb = NULL;

    snd_ctl_t *m_ctl = NULL;
    snd_ctl_event_t *m_event = NULL;

    sigset_t m_set[1];
    sigemptyset(m_set);
    for (ptrdiff_t i = 6; i;) {
        int register sig =
            (int const[]){
                SIGHUP,
                SIGINT,
                SIGUSR1,
                SIGPOLL,
                SIGTERM,
            }[--i];

        if (!sigaddset(m_set, sig)) {
            struct sigaction m_act[1];

            sigaction(sig, NULL, m_act);
            m_act->sa_handler = sa_handler_cb;
            sigfillset(&m_act->sa_mask);
            sigaction(sig, m_act, NULL);
        }
    }
    sigprocmask(SIG_BLOCK, m_set, NULL);

    fcntl(open(getenv("GRIM_DEFAULT_DIR"),
               O_ASYNC | O_CLOEXEC | O_DIRECTORY),
          0x402,
          0x80000004);

    ca_context_create(&m_ctx);
    on_exit(ca_context_exit_cb, m_ctx);
    ca_context_set_driver(m_ctx, "alsa");
    ca_proplist_create(&m_prop);
    on_exit(ca_proplist_exit_cb, m_prop);

    snd_ctl_open(&m_ctl, "default", SND_CTL_READONLY);
    on_exit(ctl_exit_cb, m_ctl);
    snd_ctl_subscribe_events(m_ctl, 1);
    snd_ctl_event_alloca(&m_event);

    ca_proplist_set(m_prop, CA_PROP_CANBERRA_CACHE_CONTROL, "volatile", 9);
MAIN_LOOP_INIT:
    ca_proplist_set(m_prop, CA_PROP_EVENT_ID, s, n);
    ca_context_play_full(m_ctx, 0, m_prop, m_cb,
                         m_cb ? (pid_t[]){ getpid() } : NULL);
    signo = 0;
    sigprocmask(SIG_UNBLOCK, m_set, NULL);
    if (signo)
        goto MAIN_LOOP_BODY;
    snd_ctl_wait(m_ctl, -1);
    sigprocmask(SIG_BLOCK, m_set, NULL);
    if (snd_ctl_read(m_ctl, m_event) > 0 &&
        snd_ctl_event_get_type(m_event) == SND_CTL_EVENT_ELEM)
        goto MAIN_LOOP_CASE1;

MAIN_LOOP_BODY:
    switch (signo) {
    case SIGINT:
        return EXIT_SUCCESS;
    case SIGUSR1:
MAIN_LOOP_CASE1:
        s = "audio-volume-change";
        n = sizeof "audio-volume-change";
        goto MAIN_LOOP_INIT;
    case SIGPOLL:
        s = "screen-capture";
        n = sizeof "audio-volume-change";
        goto MAIN_LOOP_INIT;
    default:
        s = "service-logout";
        n = sizeof "service-logout";
        m_cb = ca_context_play_cb;
        sigemptyset(m_set);
        sigaddset(m_set, SIGINT);
        goto MAIN_LOOP_INIT;
    }

    return EXIT_FAILURE;
}
