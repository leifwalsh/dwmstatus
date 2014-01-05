// Copyright (c) 2014, Leif Walsh <leif.walsh@gmail.com>
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

#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <err.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>
#include <alsa/asoundlib.h>

static int getncpu(void) {
  int r;
  if ((r = sysconf(_SC_NPROCESSORS_ONLN)) < 0) {
    err(1, "sysconf(_SC_NPROCESSORS_ONLN)");
  }
  return r;
}

enum Color {
  NORMAL = 1,
  SELECTED,
  RED,
  ORANGE,
  YELLOW,
  BLUE,
  CYAN,
  GREEN
};

class Metric {
  template<class stream>
  class ColorScope {
    stream &_s;
  public:
    ColorScope(stream &s, enum Color c) : _s(s) {
      _s << (char) c;
    }
    ~ColorScope() {
      _s << '\x01';
    }
  };

public:
  virtual enum Color color() const = 0;
  virtual operator std::string() const = 0;
  template<class stream>
  friend stream &operator<<(stream &s, const Metric &metric) {
    if (metric.color() != NORMAL) {
      ColorScope<stream> cs(s, metric.color());
      s << (std::string) metric;
    } else {
      s << (std::string) metric;
    }
    return s;
  }
};

class Separator : public Metric {
public:
  enum Color color() const { return NORMAL; }
  operator std::string() const { return "::"; }
};

class Load : public Metric {
  double one, five, fifteen;
public:
  Load() {
    std::ifstream f("/proc/loadavg");
    f >> one >> five >> fifteen;
  }
  enum Color color() const {
    static int ncpu = getncpu();
    return ((one>2*ncpu)
            ? RED
            : ((one>3*ncpu/2)
               ? ORANGE
               : ((one>ncpu)
                  ? YELLOW
                  : BLUE)));
  }
  operator std::string() const {
    std::stringstream ss;
    ss << std::setw(4) << std::setfill('0') << std::setprecision(2) << std::fixed;
    ss << one << " " << five << " " << fifteen;
    return ss.str();
  }
};

class Meminfo : public Metric {
  size_t total, mfree, buff, cach;
public:
  Meminfo() {
    std::ifstream f("/proc/meminfo");
    int got = 0;
    while (!f.eof()) {
      char buf[256];
      f.getline(buf, 256);
      if (sscanf(buf, "MemTotal: %zu kB", &total) == 1 ||
          sscanf(buf, "MemFree: %zu kB", &mfree) == 1 ||
          sscanf(buf, "Buffers: %zu kB", &buff) == 1 ||
          sscanf(buf, "Cached: %zu kB", &cach) == 1) {
        if (++got == 4) {
          break;
        }
      }
    }
  }
  enum Color color() const {
    return (((mfree+cach)*10<total)
            ? RED
            : (((mfree+cach)*5<total)
               ? ORANGE
               : (((mfree+cach)*3<total)
                  ? YELLOW
                  : GREEN)));
  }
  operator std::string() const {
    std::stringstream ss;
    ss << std::setw(1) << std::setfill('0') << std::setprecision(1) << std::fixed;
    ss << "u " << (total - buff - cach - mfree) / 1024.0 << "M "
       << "f " << mfree / 1024.0 << "M "
       << "b " << buff / 1024.0 << "M "
       << "c " << cach / 1024.0 << "M";
    return ss.str();
  }
};

class Temp : public Metric {
  double temp;
public:
  Temp() : temp(0) {
    for (int i = 0; i < 2; ++i) {
      std::stringstream ss;
      ss << "/sys/class/thermal/thermal_zone" << i << "/temp";
      std::ifstream f(ss.str());
      double ti;
      f >> ti;
      temp += ti / 2000.0;
    }
  }
  enum Color color() const {
    return ((temp > 80)
            ? RED
            : ((temp > 65)
               ? ORANGE
               : ((temp > 50)
                  ? YELLOW
                  : GREEN)));
  }
  operator std::string() const {
    std::stringstream ss;
    ss << std::setw(1) << std::setfill('0') << std::setprecision(1) << std::fixed;
    ss << temp << 'C';
    return ss.str();
  }
};

static bool exists(const std::string &filename) {
  struct stat st;
  int r = stat(filename.c_str(), &st);
  if (r == 0) {
    return S_ISDIR(st.st_mode);
  } else if (errno == ENOENT) {
    return false;
  } else {
    err(1, "stat");
  }
}

class Battery : public Metric {
  class SingleBattery {
  public:
    ssize_t energy_now, energy_full, power_now;
    std::string status;
    bool present;

    SingleBattery(const std::string &batdir)
      : energy_now(0),
        energy_full(0),
        power_now(0),
        status(""),
        present(false) {
      if (exists(batdir)) {
        {
          std::stringstream ss;
          ss << batdir << "/present";
          int p;
          std::ifstream f(ss.str());
          f >> p;
          present = p != 0;
        }
        if (present) {
          {
            std::stringstream ss;
            ss << batdir << "/energy_now";
            std::ifstream f(ss.str());
            f >> energy_now;
          }
          {
            std::stringstream ss;
            ss << batdir << "/energy_full";
            std::ifstream f(ss.str());
            f >> energy_full;
          }
          {
            std::stringstream ss;
            ss << batdir << "/power_now";
            std::ifstream f(ss.str());
            f >> power_now;
          }
          {
            std::stringstream ss;
            ss << batdir << "/status";
            std::ifstream f(ss.str());
            f >> status;
          }
        }
      }
    }
  };

  int _percent, _minutes;
  bool _present;
  char _direction;

public:
  Battery() : _percent(0), _minutes(0), _present(false), _direction('!') {
    std::vector<SingleBattery> batteries;
    for (int i = 0; i < 2; ++i) {
      std::stringstream ss;
      ss << "/sys/class/power_supply/BAT" << i;
      SingleBattery sb(ss.str());
      if (sb.present) {
        _present = true;
        batteries.push_back(sb);
      }
    }

    ssize_t power = std::accumulate(batteries.begin(), batteries.end(), 0,
                                    [](ssize_t acc, const SingleBattery &b) {
                                      if (b.present) {
                                        return acc + b.power_now;
                                      } else {
                                        return acc;
                                      }
                                    });
    ssize_t energy_full = std::accumulate(batteries.begin(), batteries.end(), 0,
                                          [](ssize_t acc, const SingleBattery &b) {
                                            if (b.present) {
                                              return acc + b.energy_full;
                                            } else {
                                              return acc;
                                            }
                                          });
    ssize_t energy_now = std::accumulate(batteries.begin(), batteries.end(), 0,
                                         [](ssize_t acc, const SingleBattery &b) {
                                           if (b.present) {
                                             return acc + b.energy_now;
                                           } else {
                                             return acc;
                                           }
                                         });

    std::ifstream acf("/sys/class/power_supply/AC/online");
    int ac_present;
    acf >> ac_present;

    _percent = 100.0 * energy_now / energy_full;
    if (ac_present == 1) {
      _direction = '+';
      _minutes = int(60.0 * (energy_full - energy_now) / power);
    } else {
      _direction = '-';
      _minutes = int(60.0 * energy_now / power);
    }
  }

  bool present() const { return _present; }

  enum Color color() const {
    return ((_percent < 10
             ? RED
             : ((_percent < 20)
                ? ORANGE
                : ((_percent < 30)
                   ? YELLOW
                   : CYAN))));
  }

  operator std::string() const {
    std::stringstream ss;
    ss << _direction << _percent << "% ";
    ss << std::setw(1) << std::setfill('0') << _minutes / 60 << ":";
    ss << std::setw(2) << std::setfill('0') << _minutes % 60;
    return ss.str();
  }
};

class Datetime : public Metric {
public:
  enum Color color() const { return NORMAL; }
  operator std::string() const {
    char buf[65];
    time_t result = time(NULL);
    struct tm *resulttm;
    if (!(resulttm = localtime(&result))) {
      err(1, "localtime");
    }
    if(!strftime(buf, 64, "%a %b %d %H:%M", resulttm)) {
      err(1, "strftime");
    }
    return buf;
  }
};

class AlsaManager {
  class MixerHandle {
    snd_mixer_t *_h;
  public:
    MixerHandle() : _h(nullptr) {
      if (snd_mixer_open(&_h, 0) != 0) {
        err(1, "snd_mixer_open");
      }
    }
    ~MixerHandle() {
      snd_mixer_close(_h);
    }
    snd_mixer_t *get() const { return _h; }
  };

  class MixerSelemId {
    snd_mixer_selem_id_t *_s;
  public:
    MixerSelemId(const char *mix_name, int mix_index) : _s(nullptr) {
      snd_mixer_selem_id_malloc(&_s);
      snd_mixer_selem_id_set_index(_s, mix_index);
      snd_mixer_selem_id_set_name(_s, mix_name);
    }
    ~MixerSelemId() {
      snd_mixer_selem_id_free(_s);
    }
    snd_mixer_selem_id_t *get() const { return _s; }
  };

  class MixerLoader {
    snd_mixer_t *_handle;
  public:
    MixerLoader(snd_mixer_t *handle) : _handle(handle) {
      if (snd_mixer_load(handle) != 0) {
        err(1, "snd_mixer_load");
      }
    }
    ~MixerLoader() {
      snd_mixer_free(_handle);
    }
  };

  MixerHandle _handle;

public:
  AlsaManager() : _handle() {
    static const char *card = "default";
    if (snd_mixer_attach(_handle.get(), card) != 0) {
      err(1, "snd_mixer_attach");
    }
    if (snd_mixer_selem_register(_handle.get(), NULL, NULL) != 0) {
      err(1, "snd_mixer_selem_register");
    }
  }

  class AlsaMetric : public Metric {
    long _volume;
    bool _muted;
  public:
    AlsaMetric(long volume, bool muted) : _volume(volume), _muted(muted) {}

    enum Color color() const { return NORMAL; }
    operator std::string() const {
      std::stringstream ss;
      ss << "vol ";
      if (_muted) {
        ss << "mute";
      } else {
        ss << _volume;
      }
      return ss.str();
    }
  };

  AlsaMetric get_volume() const {
    MixerLoader loader(_handle.get());

    static const char *mix_name = "Master";
    static int mix_index = 0;
    MixerSelemId sid(mix_name, mix_index);

    snd_mixer_elem_t *elem;
    if (!(elem = snd_mixer_find_selem(_handle.get(), sid.get()))) {
      err(1, "snd_mixer_find_selem");
    }

    long minv, maxv;
    if (snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv) != 0) {
      err(1, "snd_mixer_selem_get_playback_volume_range");
    }
    long lvol, rvol;
    if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &lvol) != 0) {
      err(1, "snd_mixer_selem_get_playback_volume");
    }
    if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &rvol) != 0) {
      err(1, "snd_mixer_selem_get_playback_volume");
    }
    long vol = (lvol + rvol) / 2;
    int lunmuted, runmuted;
    if (snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &lunmuted) != 0) {
      err(1, "snd_mixer_selem_get_playback_switch");
    }
    if (snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_RIGHT, &runmuted) != 0) {
      err(1, "snd_mixer_selem_get_playback_switch");
    }

    return AlsaMetric(100.0 * (vol - minv) / (maxv - minv), lunmuted == 0 && runmuted == 0);
  }
};

int main(void) {
  Display *dpy;
  if (!(dpy = XOpenDisplay(NULL))) {
    err(1, "Cannot open display.");
  }

  AlsaManager alsa_manager;

  for (;;sleep(5)) {
    std::stringstream ss;
    Battery b;
    ss << Load() << Separator()
       << Meminfo() << Separator()
       << Temp() << Separator();
    if (b.present()) {
      ss << b << Separator();
    }
    ss << ' ' << alsa_manager.get_volume() << ' ' << Separator() << ' '
       << Datetime();
    std::string s = ss.str();
    XStoreName(dpy, DefaultRootWindow(dpy), s.c_str());
    XSync(dpy, False);
  }

  XCloseDisplay(dpy);

  return 0;
}
