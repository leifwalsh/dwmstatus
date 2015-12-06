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

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <err.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>

#include "hostap/src/common/wpa_ctrl.h"

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
protected:
  template<class stream>
  class ColorScope {
    stream &_s;
    Color _c;
  public:
    ColorScope(stream &s, enum Color c) : _s(s), _c(c) {
      if (_c != NORMAL) {
        _s << (char) _c;
      }
    }
    ~ColorScope() {
      if (_c != NORMAL) {
        _s << '\x01';
      }
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

class Bar : public Metric {
  const int _x, _y, _w, _h, _skip;
  const bool _filled;
  const Color _c;
public:
  Bar(int x, int y, int w, int h, int skip, bool filled, Color c)
    : _x(x),
      _y(y),
      _w(w),
      _h(h),
      _skip(skip),
      _filled(filled),
      _c(c)
  {}
  Color color() const { return NORMAL; }
  operator std::string() const {
    char buf[6];
    buf[0] = (char) _c;
    buf[0] |= 1<<7;
    if (_filled) {
      buf[0] |= 1<<6;
    }
    buf[1] = (char) _x + 1;
    buf[2] = (char) _y + 1;
    buf[3] = (char) _w + 1;
    buf[4] = (char) _h + 1;
    buf[5] = (char) _skip + 1;
    return std::string(buf, sizeof buf);
  }
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

class Cpuinfo : public Metric {
  static int nelts;
  static std::unique_ptr<size_t[]> total_last, user_last, sys_last, io_last;

  std::unique_ptr<size_t[]> total_cur, user_cur, sys_cur, io_cur;

public:
  Cpuinfo() {
    if (!nelts) {
      nelts = getncpu() + 1;
      total_last = std::make_unique<size_t[]>(nelts);
      user_last = std::make_unique<size_t[]>(nelts);
      sys_last = std::make_unique<size_t[]>(nelts);
      io_last = std::make_unique<size_t[]>(nelts);
      std::fill(&total_last[0], &total_last[nelts], 0);
      std::fill(&user_last[0], &user_last[nelts], 0);
      std::fill(&sys_last[0], &sys_last[nelts], 0);
      std::fill(&io_last[0], &io_last[nelts], 0);
    }

    total_cur = std::make_unique<size_t[]>(nelts);
    user_cur = std::make_unique<size_t[]>(nelts);
    sys_cur = std::make_unique<size_t[]>(nelts);
    io_cur = std::make_unique<size_t[]>(nelts);
    std::fill(&total_cur[0], &total_cur[nelts], 0);
    std::fill(&user_cur[0], &user_cur[nelts], 0);
    std::fill(&sys_cur[0], &sys_cur[nelts], 0);
    std::fill(&io_cur[0], &io_cur[nelts], 0);
    
    std::ifstream f("/proc/stat");
    for (int i = 0; i < nelts; ++i) {
      f.ignore(std::numeric_limits<std::streamsize>::max(), ' ');
      for (int j = 0; f.good(); ++j) {
        size_t jiffies;
        f >> jiffies;
        if (f.fail()) {
          f.clear();
          break;
        }
        if (j < 2) {
          user_cur[i] += jiffies;
        } else if (j == 2) {
          sys_cur[i] += jiffies;
        } else if (j == 4) {
          io_cur[i] += jiffies;
        }
        total_cur[i] += jiffies;
      }
    }
  }
  ~Cpuinfo() {
    std::copy(&total_cur[0], &total_cur[nelts], &total_last[0]);
    std::copy(&user_cur[0], &user_cur[nelts], &user_last[0]);
    std::copy(&sys_cur[0], &sys_cur[nelts], &sys_last[0]);
    std::copy(&io_cur[0], &io_cur[nelts], &io_last[0]);
  }
  int pct(int i) const {
    return int(100 * double(user_cur[i] - user_last[i] + sys_cur[i] - sys_last[i]) / double(total_cur[i] - total_last[i]));
  }
  int user(int i) const {
    return int(100 * double(user_cur[i] - user_last[i]) / double(total_cur[i] - total_last[i]));
  }
  int sys(int i) const {
    return int(100 * double(sys_cur[i] - sys_last[i]) / double(total_cur[i] - total_last[i]));
  }
  int io(int i) const {
    return int(100 * double(io_cur[i] - io_last[i]) / double(total_cur[i] - total_last[i]));
  }
  enum Color color() const {
    return NORMAL;
  }
  enum Color color_for(int i) const {
    const int p = pct(i);
    return ((p > 90)
            ? RED
            : ((p > 75)
               ? ORANGE
               : ((p > 50)
                  ? YELLOW
                  : ((p > 10)
                     ? GREEN
                     : BLUE))));
  }
  operator std::string() const {
    std::stringstream ss;
    {
      ColorScope<std::stringstream> cs(ss, color_for(0));
      ss << user(0) << "% "
         << sys(0) << "% "
         << io(0) << "%";
    }
    for (int i = 1; i < nelts; ++i) {
      ss << Bar(0,
                2 + (i - 1) * 3,
                40 * pct(i) / 100,
                2,
                (i == (nelts - 1)) ? 41 : 0,
                true,
                color_for(i));
    }
    return ss.str();
  }
};
int Cpuinfo::nelts = 0;
std::unique_ptr<size_t[]> Cpuinfo::total_last = nullptr;
std::unique_ptr<size_t[]> Cpuinfo::user_last = nullptr;
std::unique_ptr<size_t[]> Cpuinfo::sys_last = nullptr;
std::unique_ptr<size_t[]> Cpuinfo::io_last = nullptr;

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
    const size_t used = (total - buff - cach - mfree);
    std::stringstream ss;
    ss << std::setw(1) << std::setfill('0') << std::setprecision(1) << std::fixed;
    ss << "u " << (used > (1<<20) ? (used / 1024.0 / 1024.0) : (used / 1024.0)) << (used > (1<<20) ? "G " : "M ")
       << "b " << (buff > (1<<20) ? (buff / 1024.0 / 1024.0) : (buff / 1024.0)) << (buff > (1<<20) ? "G " : "M ")
       << "c " << (cach > (1<<20) ? (cach / 1024.0 / 1024.0) : (cach / 1024.0)) << (cach > (1<<20) ? "G " : "M ");
    int x = 0;
    ss << Bar(x, 1, 100 * used / total, 12, 0, true, GREEN); x += 100 * used / total;
    ss << Bar(x, 1, 100 * buff / total, 12, 0, true, BLUE);  x += 100 * buff / total;
    ss << Bar(x, 1, 100 * cach / total, 12, 0, true, ORANGE);
    ss << Bar(0, 1, 100, 12, 101, false, NORMAL);
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

static bool dir_exists(const std::string &filename) {
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
      if (dir_exists(batdir)) {
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

    double dpercent = 100.0 * energy_now / energy_full;
    if (100.0 - dpercent < 0.5) {
      _percent = 100;
    } else {
      _percent = int(dpercent);
    }
    if (ac_present == 1) {
      if (_percent == 100) {
        _direction = '=';
        _minutes = 0;
      } else {
        _direction = '+';
        _minutes = int(60.0 * (energy_full - energy_now) / power);
      }
    } else {
      _direction = '-';
      _minutes = int(60.0 * energy_now / power);
    }
    if (_minutes < 0) {
      _minutes = 0;
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
    ss << _direction << _percent << "%";
    if (_percent != 100 || _direction == '-') {
      ss << " "
         << std::setw(1) << std::setfill('0') << _minutes / 60 << ":"
         << std::setw(2) << std::setfill('0') << _minutes % 60;
    }
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

class Dir {
  DIR *_dir;

  const char *_next() const {
    struct dirent *ent = readdir(_dir);
    if (ent == NULL) {
      return NULL;
    }
    return ent->d_name;
  }

public:
  Dir(const char *name) : _dir(opendir(name)) {
    if (_dir == NULL) {
      err(1, "opendir");
    }
  }
  ~Dir() {
    if (_dir) {
      closedir(_dir);
    }
  }

  const char *next() const {
    const char *ret = _next();
    while (std::string(".") == ret || std::string("..") == ret) {
      ret = _next();
    }
    return ret;
  }
};

class Wifi : public Metric {
  enum State {
    DISCONNECTED = 0,
    SEARCHING,
    CONNECTING,
    CONNECTED,
    WIFI_OFF
  };

  class WpaCtrl {
    struct wpa_ctrl *_c;
  public:
    WpaCtrl(const char *sock) : _c(wpa_ctrl_open(sock)) {}
    ~WpaCtrl() {
      if (_c != NULL) {
        wpa_ctrl_close(_c);
      }
    }

    bool ok() const {
      return _c != NULL;
    }

    void status(std::string &ssid, enum State &state) {
      char buf[1<<12];
      size_t bufsz = sizeof buf;
      int ret = wpa_ctrl_request(_c, "STATUS", (sizeof "STATUS") - 1, buf, &bufsz, NULL);
      if (ret != 0) {
        err(1, "wpa_ctrl_request");
      }

      std::string statusstr(buf, bufsz);
      std::istringstream iss(statusstr);
      while (iss.good()) {
        std::string key;
        std::getline(iss, key, '=');
        if (key == "ssid") {
          std::getline(iss, ssid);
        } else if (key == "wpa_state") {
          std::string statestr;
          std::getline(iss, statestr);
          if (statestr == "COMPLETED") {
            state = CONNECTED;
          } else if (statestr == "DISCONNECTED" || statestr == "INACTIVE") {
            state = DISCONNECTED;
          } else if (statestr == "SCANNING") {
            state = SEARCHING;
          } else if (statestr == "INTERFACE_DISABLED") {
            state = WIFI_OFF;
          } else {
            state = CONNECTING;
          }
        }
        std::string discard;
        std::getline(iss, discard);
      }
    }
  };

  std::string _ssid;
  enum State _state;
  bool _present;

public:
  Wifi() : _ssid(""), _state(WIFI_OFF), _present(false) {
    if (dir_exists("/run/wpa_supplicant")) {
      Dir dir("/run/wpa_supplicant");
      while (const char *name = dir.next()) {
        std::stringstream ss;
        ss << "/run/wpa_supplicant/" << name;
        WpaCtrl c(ss.str().c_str());
        if (c.ok()) {
          _present = true;
          c.status(_ssid, _state);
          break;
        }
      }
    }
  }

  enum Color color() const {
    switch (_state) {
    case WIFI_OFF:
      return RED;
    case DISCONNECTED:
      return ORANGE;
    case SEARCHING:
      return YELLOW;
    case CONNECTING:
      return GREEN;
    case CONNECTED:
      return BLUE;
    }
    return NORMAL;
  }

  operator std::string() const {
    if (_state == WIFI_OFF) {
      return "wifi off";
    } else if (_ssid.empty()) {
      return "???";
    } else {
      return _ssid;
    }
  }

  bool present() const { return _present; }
};

class Net : public Metric {
  const size_t N;
  const std::string iftok;
  mutable size_t rx[60], tx[60];
  mutable std::chrono::time_point<std::chrono::system_clock> t[60];
  mutable size_t i;

  void next() const {
    t[i % N] = std::chrono::system_clock::now();
    std::ifstream f("/proc/net/dev");
    std::string s;
    while (f.good() && s != iftok) {
      f >> s;
    }
    f >> rx[i % N];
    for (int j = 0; j < 7; ++j) {
      int ignore;
      f >> ignore;
    }
    f >> tx[i % N];
    i++;
  }

public:
  Net(const std::string &ifname) : N(60), iftok(ifname + ":"), i(0) {
    std::fill(&rx[0], &rx[N], 0);
    std::fill(&tx[0], &tx[N], 0);
  }
  Color color() const { return NORMAL; }
  operator std::string() const {
    next();
    if (i < 3) {
      return "";
    }

    std::stringstream ss;
    {
      size_t cur = (i - 1) % N;
      size_t prev = (i - 2) % N;
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(t[cur] - t[prev]);
      const double rx_rate = ((rx[cur] - rx[prev]) / 1024.0) / secs.count();
      const double tx_rate = ((tx[cur] - tx[prev]) / 1024.0) / secs.count();
      {
        Metric::ColorScope<std::stringstream> cs(ss, (rx_rate > 4500
                                                      ? RED
                                                      : (rx_rate > 2000
                                                         ? ORANGE
                                                         : (rx_rate > 1000
                                                            ? YELLOW
                                                            : (rx_rate > 100
                                                               ? GREEN
                                                               : BLUE)))));
        ss << std::setw(1) << std::setfill('0') << std::setprecision(1) << std::fixed;
        if (rx_rate > (1<<10)) {
          ss << (rx_rate / (1<<10)) << "M";
        } else {
          ss << rx_rate << "k";
        }
      }
      {
        Metric::ColorScope<std::stringstream> cs(ss, (tx_rate > 1000
                                                      ? RED
                                                      : (tx_rate > 500
                                                         ? ORANGE
                                                         : (tx_rate > 100
                                                            ? YELLOW
                                                            : (tx_rate > 50
                                                               ? GREEN
                                                               : BLUE)))));
        ss << std::setw(1) << std::setfill('0') << std::setprecision(1) << std::fixed;
        if (tx_rate > (1<<10)) {
          ss << (tx_rate / (1<<10)) << "M";
        } else {
          ss << tx_rate << "k";
        }
      }
    }
    for (size_t j = (i >= N) ? (i - N + 3) : 3; j < i; ++j) {
      static const size_t max_rx = (50<<20) / 8; // 50 megabits
      static const size_t max_tx = (5<<20) / 8;  // 5 megabits
      const int cur = j % N;
      const int prev = (j - 1) % N;
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(t[cur] - t[prev]);
      if (!secs.count()) {
        continue;
      }
      const double rx_rate = (rx[cur] - rx[prev]) / double(secs.count());
      const double tx_rate = (tx[cur] - tx[prev]) / double(secs.count());
      const int rh = std::min(8, int(rx_rate < (100<<10)
                                     ? (3 * rx_rate / (100<<10))
                                     : (rx_rate < (1<<20)
                                        ? (3 + (3 * rx_rate / (1<<20)))
                                        : (6 + (2 * rx_rate / max_rx)))));
      const int th = std::min(4, int(tx_rate < (10<<10)
                                     ? (2 * tx_rate / (10<<10))
                                     : (2 + (2 * tx_rate / max_tx))));
      ss << Bar(0, 8 - rh, 1, rh, 0, true, GREEN)
         << Bar(0, 9, 1, th, 1, true, RED);
    }
    return ss.str();
  }
};

int main(void) {
  Display *dpy;
  if (!(dpy = XOpenDisplay(NULL))) {
    err(1, "Cannot open display.");
  }

  Net n("wlp3s0");
  for (;;sleep(5)) {
    std::stringstream ss;
    Battery b;
    Wifi w;
    ss << Cpuinfo()
       << Meminfo()
       << n << Separator()
       << Temp() << Separator();
    if (w.present()) {
      ss << w << Separator();
    }
    if (b.present()) {
      ss << b << Separator();
    }
    ss << ' ' << Datetime();
    std::string s = ss.str();
    XStoreName(dpy, DefaultRootWindow(dpy), s.c_str());
    XSync(dpy, False);
  }

  XCloseDisplay(dpy);

  return 0;
}
