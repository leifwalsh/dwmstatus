// Copyright (c) 2013, Leif Walsh <leif.walsh@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the <organization> nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <alloca.h>
#include <err.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>
#include <alsa/asoundlib.h>

static void setstatus(Display *dpy, const char *str) {
        XStoreName(dpy, DefaultRootWindow(dpy), str);
        XSync(dpy, False);
}

#define withfile(fvar, filename, block) do {                    \
                FILE *fvar;                                     \
                if ((fvar = fopen(filename, "r")) == NULL) {    \
                        err(1, "fopen(\"%s\")", filename);      \
                }                                               \
                do block while(0);                              \
                if (fclose(fvar) != 0) {                        \
                        err(1, "fclose(\"%s\")", filename);     \
                }                                               \
        } while (0)

#define scanfile(filename, nelts, fmt, args...) do {                    \
                withfile(scanfile_fvar, filename, {                     \
                                if (fscanf(scanfile_fvar, fmt, ##args) != nelts) { \
                                        err(1, "fscanf(\"%s\")", filename); \
                                }                                       \
                        });                                             \
        } while (0)

static void getload(double *a, double *b, double *c) {
        scanfile("/proc/loadavg", 3, "%lf %lf %lf", a, b, c);
}

static void getmem(int *total, int *free, int *buff, int *cach) {
        scanfile("/proc/meminfo", 4,
                 ("MemTotal: %d kB "
                  "MemFree: %d kB "
                  "Buffers: %d kB "
                  "Cached: %d kB"),
                 total, free, buff, cach);
}

#if TEMP
static double getdoubleval(const char *file) {
        double ret;
        scanfile(file, 1, "%lf", &ret);
        return ret;
}

static double gettemp(void) {
        double t1 = getdoubleval("/sys/class/thermal/thermal_zone0/temp");
        double t2 = getdoubleval("/sys/class/thermal/thermal_zone1/temp");
        return (t1 + t2) / 2000.0;
}
#endif

#if BAT
static int getintval(const char *file) {
        int ret;
        scanfile(file, 1, "%d", &ret);
        return ret;
}

static void getbattery(char *batchr, int *batpct, int *batmin) {
        *batpct = getintval("/sys/devices/platform/smapi/BAT0/remaining_percent");
        char state[32];
        withfile(fd, "/sys/devices/platform/smapi/BAT0/state", {
                        if (!fgets(state, 32, fd)) {
                                err(1, "fgets(\"/sys/devices/platform/smapi/BAT0/state\")");
                        }
                });
        if (!strncmp("charging", state, 8)) {
                *batchr = '+';
                *batmin = getintval("/sys/devices/platform/smapi/BAT0/remaining_charging_time");
        } else if (!strncmp("discharging", state, 11)) {
                *batchr = '-';
                *batmin = getintval("/sys/devices/platform/smapi/BAT0/remaining_running_time_now");
        } else if (!strncmp("idle", state, 4)) {
                *batchr = '=';
                *batmin = 0;
        } else {
                *batchr = '!';
                *batmin = 0;
        }
}
#endif

static void setup_alsa(snd_mixer_t **handlep) {
        snd_mixer_t *handle;

        static const char *card = "default";

        if (snd_mixer_open(&handle, 0) != 0) {
                err(1, "snd_mixer_open");
        }
        if (snd_mixer_attach(handle, card) != 0) {
                int errsv = errno;
                snd_mixer_close(handle);
                errno = errsv;
                err(1, "snd_mixer_attach");
        }
        if (snd_mixer_selem_register(handle, NULL, NULL) != 0) {
                int errsv = errno;
                snd_mixer_close(handle);
                errno = errsv;
                err(1, "snd_mixer_selem_register");
        }

        *handlep = handle;
}

static void teardown_alsa(snd_mixer_t **handlep) {
        snd_mixer_close(*handlep);
        *handlep = NULL;
}

static void getvolume(snd_mixer_t *handle, char *str) {
        if (snd_mixer_load(handle) != 0) {
                int errsv = errno;
                snd_mixer_close(handle);
                errno = errsv;
                err(1, "snd_mixer_load");
        }

        long minv, maxv;
        long vol;
        int unmuted;
        {
                static const char *mix_name = "Master";
                static int mix_index = 0;

                snd_mixer_selem_id_t *sid;
                snd_mixer_selem_id_alloca(&sid);
                snd_mixer_selem_id_set_index(sid, mix_index);
                snd_mixer_selem_id_set_name(sid, mix_name);

                snd_mixer_elem_t *elem;
                if (!(elem = snd_mixer_find_selem(handle, sid))) {
                        int errsv = errno;
                        snd_mixer_close(handle);
                        errno = errsv;
                        err(1, "snd_mixer_find_selem");
                }

                if (snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv) != 0) {
                        int errsv = errno;
                        snd_mixer_close(handle);
                        errno = errsv;
                        err(1, "snd_mixer_selem_get_playback_volume_range");
                }

                if (snd_mixer_selem_get_playback_volume(elem, 0, &vol) != 0) {
                        int errsv = errno;
                        snd_mixer_close(handle);
                        errno = errsv;
                        err(1, "snd_mixer_selem_get_playback_volume");
                }

                if (snd_mixer_selem_get_playback_switch(elem, 0, &unmuted) != 0) {
                        int errsv = errno;
                        snd_mixer_close(handle);
                        errno = errsv;
                        err(1, "snd_mixer_selem_get_playback_volume");
                }
        }

        snd_mixer_free(handle);

        if (unmuted) {
                snprintf(str, 4, "%ld", (100 * (vol - minv)) / (maxv - minv));
        } else {
                strcpy(str, "mute");
        }
}

static char *getdatetime(void) {
        time_t result = time(NULL);
        struct tm *resulttm;
        if (!(resulttm = localtime(&result))) {
                err(1, "localtime");
        }
        char *buf;
        if (!(buf = malloc(65))) {
                err(1, "malloc");
        }
        if(!strftime(buf, 64, "%a %b %d %H:%M", resulttm)) {
                err(1, "strftime");
        }

        return buf;
}

int main(void) {
        Display *dpy;
        if (!(dpy = XOpenDisplay(NULL))) {
                err(1, "Cannot open display.");
        }

        snd_mixer_t *handle;
        setup_alsa(&handle);

        for (;;sleep(5)) {
                double a, b, c;
                getload(&a, &b, &c);
                int total, mfree, buff, cach;
                getmem(&total, &mfree, &buff, &cach);
#if TEMP
                double temp = gettemp();
#endif
#if BAT
                char batchr;
                int batpct, batmin;
                getbattery(&batchr, &batpct, &batmin);
#endif
                char vol[5];
                getvolume(handle, vol);
                char *datetime = getdatetime();
                char status[200];
                char loadcolor = ((a>4)
                                  ? 3
                                  : ((a>3)
                                     ? 4
                                     : ((a>2)
                                        ? 5
                                        : 6)));
                char memcolor = (((mfree+cach)*10<total)
                                 ? 3
                                 : (((mfree+cach)*5<total)
                                    ? 4
                                    : (((mfree+cach)*3<total)
                                       ? 5
                                       : 8)));
#if TEMP
                char tempcolor = ((temp > 80)
                                  ? 3
                                  : ((temp > 65)
                                     ? 4
                                     : ((temp > 50)
                                        ? 5
                                        : 8)));
#endif
#if BAT
                char batcolor = ((batpct < 10
                                  ? 3
                                  : ((batpct < 20)
                                     ? 4
                                     : ((batpct < 30)
                                        ? 5
                                        : 7))));
#endif
                snprintf(status, 200,
                         "%c%0.02lf %0.02lf %0.02lf\x01::"
                         "%cu %0.01lfM f %0.01lfM b %0.01lfM c %0.01lfM\x01::"
#if TEMP
                         "%c%0.01lfC\x01::"
#endif
#if BAT
                         "%c%c%d%% %0d:%02d\x01::"
#endif
                         " vol %s :: %s",
                         loadcolor, a, b, c,
                         memcolor, (total-buff-cach-mfree)/1024.0, mfree/1024.0, buff/1024.0, cach/1024.0,
#if TEMP
                         tempcolor, temp,
#endif
#if BAT
                         batcolor, batchr, batpct, batmin / 60, batmin % 60,
#endif
                         vol, datetime);
                free(datetime);
                setstatus(dpy, status);
        }

        teardown_alsa(&handle);

        XCloseDisplay(dpy);

        return 0;
}
