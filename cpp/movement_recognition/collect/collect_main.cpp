/**
 * @file collect_main.cpp
 * @author Omar Shrit
 *
 * Small, Data collector app for sensor data from a GY-89 into a CSV
 * file.  It depends only on the drivers in ../driver.
 * Argument parsing is hand-rolled and all I/O is C stdio.
 *
 * - choose which sensors to record (`--sensors accel,gyro,mag,baro` or `all`);
 * - timestamp every row with the Unix clock in microseconds;
 * - name the output file `<label>_<date>.csv`, so the label is the file name
 *   (there is no label column).
 *
 * Examples:
 *   collect --label walking --sensors all --duration 30
 *   collect --label stairs_up --sensors accel,gyro --out-dir data
 *   collect --tui --out-dir data            # interactive picker (if built in)
 *
 * The optional ncurses TUI is compiled in only with `make NCURSES=1`.
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef USE_NCURSES
#include <ncurses.h>
#endif

#include "../driver/bmp180.hpp"
#include "../driver/imu.hpp"
#include "../driver/mag_calibration.hpp"

namespace {

std::atomic<bool> g_stop{false};

void HandleSigint(int) { g_stop = true; }

// Return the next command-line value after argv[i], advancing i.  Exits with an
// error if the option was given without a value.
const char* NextArg(int& i, int argc, char** argv, const char* name)
{
  if (i + 1 >= argc)
  {
    std::fprintf(stderr, "error: %s requires a value\n", name);
    std::exit(2);
  }
  return argv[++i];
}

const char* const kLabels[] = {"sitting", "walking", "walking_fast",
                                "stairs_up", "stairs_down", "squat" };

constexpr int kNumLabels = sizeof(kLabels) / sizeof(kLabels[0]);

// Which sensors to record.  accel/gyro/mag come from the IMU; baro is the BMP180.
struct Sensors
{
  bool accel = false, gyro = false, mag = false, baro = false;
  bool AnyMotion() const { return accel || gyro || mag; }
};

// Parse "all" or a comma list like "accel,gyro,mag,baro".  Returns false if the
// spec is empty or contains an unknown name.
bool ParseSensors(const char* spec, Sensors& out)
{
  if (!std::strcmp(spec, "all"))
  {
    out = { true, true, true, true };
    return true;
  }
  char buf[128];
  std::strncpy(buf, spec, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  for (char* tok = std::strtok(buf, ","); tok; tok = std::strtok(nullptr, ","))
  {
    if (!std::strcmp(tok, "accel")) out.accel = true;
    else if (!std::strcmp(tok, "gyro")) out.gyro = true;
    else if (!std::strcmp(tok, "mag")) out.mag = true;
    else if (!std::strcmp(tok, "baro")) out.baro = true;
    else return false;
  }
  return out.accel || out.gyro || out.mag || out.baro;
}

//! Microseconds since the Unix epoch (the device's real-time clock).
uint64_t UnixMicros()
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000;
}

//! Build "<dir>/<label>_<YYYYmmdd-HHMMSS>.csv" from the current date.
void MakeFilename(char* out, size_t n, const char* dir, const char* label)
{
  const time_t now = std::time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
  std::snprintf(out, n, "%s/%s_%s.csv", dir, label, stamp);
}

void WriteHeader(std::FILE* f, const Sensors& s)
{
  std::fprintf(f, "timestamp_unix_us");
  if (s.accel) std::fprintf(f, ",ax,ay,az");
  if (s.gyro)  std::fprintf(f, ",gx,gy,gz");
  if (s.mag)   std::fprintf(f, ",mx,my,mz");
  if (s.baro)  std::fprintf(f, ",pressure_pa,temp_c");
  std::fprintf(f, "\n");
}

void WriteRow(std::FILE* f, const Sensors& s, uint64_t ts,
              const ImuSample& m, float pressurePa, float tempC)
{
  std::fprintf(f, "%llu", static_cast<unsigned long long>(ts));
  if (s.accel) std::fprintf(f, ",%.6g,%.6g,%.6g", m.ax, m.ay, m.az);
  if (s.gyro)  std::fprintf(f, ",%.6g,%.6g,%.6g", m.gx, m.gy, m.gz);
  if (s.mag)   std::fprintf(f, ",%.6g,%.6g,%.6g", m.mx, m.my, m.mz);
  if (s.baro)  std::fprintf(f, ",%.6g,%.6g", pressurePa, tempC);
  std::fprintf(f, "\n");
}

// Read one row's worth of the selected sensors.  Returns false on I2C error.
bool ReadSelected(IMU& imu, BMP180& baro, const Sensors& s,
                  ImuSample& m, float& pressurePa, float& tempC)
{
  if (s.AnyMotion() && !imu.Sample(m))
    return false;
  if (s.baro && !baro.Read(tempC, pressurePa))
    return false;
  return true;
}

void Usage(const char* prog)
{
  std::printf(
      "Record GY-89 sensor data to a CSV file (one file per recording, named\n"
      "<label>_<date>.csv).  The interactive ncurses TUI runs by default;\n"
      "use --no-tui for plain command-line recording.\n\n"
      "Usage: %s [options]            (interactive TUI)\n"
      "       %s --no-tui --label NAME [options]   (command line)\n"
      "      --no-tui        plain command-line recording (needs --label)\n"
      "      --tui           force the interactive TUI (the default)\n"
      "  -l, --label NAME    label for this recording (used as the file name)\n"
      "  -o, --out-dir DIR   directory for the output file (default .)\n"
      "  -s, --sensors LIST  comma list of accel,gyro,mag,baro -- or all "
      "(default all)\n"
      "  -d, --device PATH   I2C device (default /dev/i2c-0)\n"
      "  -r, --rate HZ       sampling rate (default 100)\n"
      "  -t, --duration SEC  seconds to record, 0 = until Ctrl-C (default 0)\n"
      "      --mag-cal FILE  apply a magnetometer calibration file\n"
      "  -h, --help          show this help\n",
      prog, prog);
}

// Command-line recording loop: sample the selected sensors at `rateHz`, writing
// rows until the duration elapses or Ctrl-C.
int RunCommandLine(IMU& imu, BMP180& baro, const Sensors& sensors,
                   std::FILE* csv, double rateHz, double durationSec)
{
  using namespace std::chrono;
  const steady_clock::duration period =
      duration_cast<steady_clock::duration>(duration<double>(1.0 / rateHz));
  const uint64_t durationMicros =
      static_cast<uint64_t>(durationSec * 1e6);

  uint64_t count = 0, firstTs = 0, lastTs = 0;
  steady_clock::time_point nextTick = steady_clock::now();

  while (!g_stop.load())
  {
    const uint64_t ts = UnixMicros();
    ImuSample m;
    float pressurePa = 0.f, tempC = 0.f;
    if (!ReadSelected(imu, baro, sensors, m, pressurePa, tempC))
    {
      std::fprintf(stderr, "\nerror: I2C read failed during sampling: %s\n",
                   std::strerror(errno));
      return 1;
    }
    WriteRow(csv, sensors, ts, m, pressurePa, tempC);

    if (count == 0) firstTs = ts;
    lastTs = ts;
    if (++count % 50 == 0)
    {
      std::printf("\r  %llu samples...", static_cast<unsigned long long>(count));
      std::fflush(stdout);
    }
    if (durationMicros > 0 && ts - firstTs >= durationMicros)
      break;
    nextTick += period;
    std::this_thread::sleep_until(nextTick);
  }

  const double elapsed = (count > 1) ? (lastTs - firstTs) / 1e6 : 0.0;
  const double effHz = (elapsed > 0.0) ? (count - 1) / elapsed : 0.0;
  std::printf("\rWrote %llu samples in %.2f s (effective %.1f Hz).\n",
              static_cast<unsigned long long>(count), elapsed, effHz);
  return 0;
}

#ifdef USE_NCURSES

// Interactive picker: choose a label and which sensors to record, start/stop
// recording (each recording opens a new <out-dir>/<label>_<date>.csv), and watch
// the live readings.  `initial` seeds the sensor selection (from --sensors).
int RunTUI(IMU& imu, BMP180& baro, const Sensors& initial,
           const char* outDir, double rateHz)
{
  using namespace std::chrono;
  const steady_clock::duration period =
      duration_cast<steady_clock::duration>(duration<double>(1.0 / rateHz));

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  nodelay(stdscr, TRUE);

  int selected = 0;
  Sensors sel = initial;      // editable while idle
  std::FILE* csv = nullptr;   // open while recording
  unsigned long count = 0;
  steady_clock::time_point nextTick = steady_clock::now();
  int rc = 0;

  while (true)
  {
    // Always read the IMU for the live display; read the (slow) barometer only
    // when it is selected.
    const uint64_t ts = UnixMicros();
    ImuSample m;
    float pressurePa = 0.f, tempC = 0.f;
    if (!imu.Sample(m) || (sel.baro && !baro.Read(tempC, pressurePa)))
    {
      rc = 1;
      break;
    }
    if (csv)
    {
      WriteRow(csv, sel, ts, m, pressurePa, tempC);
      ++count;
    }

    bool quit = false;
    switch (getch())
    {
      case 'q': case 'Q':
        quit = true;
        break;
      case ' ': case 'r': case 'R':
        if (csv)  // stop
        {
          std::fclose(csv);
          csv = nullptr;
        }
        else if (sel.accel || sel.gyro || sel.mag || sel.baro)
        {
          // start a new dated file for the selected label + sensors
          char path[512];
          MakeFilename(path, sizeof(path), outDir, kLabels[selected]);
          csv = std::fopen(path, "w");
          if (csv) { WriteHeader(csv, sel); count = 0; }
        }
        break;
      // Sensor toggles -- only while idle, since a file's columns are fixed once
      // recording starts.
      case 'a': case 'A': if (!csv) sel.accel = !sel.accel; break;
      case 'g': case 'G': if (!csv) sel.gyro  = !sel.gyro;  break;
      case 'm': case 'M': if (!csv) sel.mag   = !sel.mag;   break;
      case 'b': case 'B': if (!csv) sel.baro  = !sel.baro;  break;
      case KEY_UP: case 'k':
        if (!csv && selected > 0) --selected;
        break;
      case KEY_DOWN: case 'j':
        if (!csv && selected + 1 < kNumLabels) ++selected;
        break;
      default:
        break;
    }
    if (quit || g_stop.load())
      break;

    erase();
    mvprintw(0, 2, "GY-89 data collection");
    mvprintw(1, 2, "keys: Up/Down label   a/g/m/b sensors   Space record   "
             "q quit");
    mvprintw(2, 2, "sensors: [%c] accel   [%c] gyro   [%c] mag   [%c] baro",
             sel.accel ? 'x' : ' ', sel.gyro ? 'x' : ' ',
             sel.mag ? 'x' : ' ', sel.baro ? 'x' : ' ');

    mvprintw(4, 2, "label:");
    for (int i = 0; i < kNumLabels; ++i)
    {
      if (i == selected) attron(A_REVERSE);
      mvprintw(5 + i, 4, "%-14s", kLabels[i]);
      if (i == selected) attroff(A_REVERSE);
    }

    const int base = 6 + kNumLabels;
    if (csv)
      mvprintw(base, 2, "[ RECORDING '%s' ]  %lu samples",
               kLabels[selected], count);
    else
      mvprintw(base, 2, "[ idle ]  toggle sensors, then Space to start a file");
    mvprintw(base + 2, 2, "accel(g)   % 7.3f % 7.3f % 7.3f", m.ax, m.ay, m.az);
    mvprintw(base + 3, 2, "gyro(dps)  % 7.1f % 7.1f % 7.1f", m.gx, m.gy, m.gz);
    mvprintw(base + 4, 2, "mag(gauss) % 7.3f % 7.3f % 7.3f", m.mx, m.my, m.mz);
    if (sel.baro)
      mvprintw(base + 5, 2, "baro       %8.2f hPa   %.1f C",
               pressurePa / 100.0f, tempC);
    refresh();

    nextTick += period;
    std::this_thread::sleep_until(nextTick);
  }

  if (csv)
    std::fclose(csv);
  endwin();
  if (rc != 0)
    std::fprintf(stderr, "error: I2C read failed during sampling.\n");
  return rc;
}
#endif  // USE_NCURSES

}  // namespace

int main(int argc, char** argv)
{
  const char* device = "/dev/i2c-0";
  const char* label = nullptr;
  const char* outDir = ".";
  const char* sensorSpec = "all";
  const char* magCal = nullptr;
  double rateHz = 100.0;
  double durationSec = 0.0;
#ifdef USE_NCURSES
  bool tui = true;   // the interactive TUI is the default; --no-tui disables it
#else
  bool tui = false;  // built without ncurses: command-line mode only
#endif

  for (int i = 1; i < argc; ++i)
  {
    const char* a = argv[i];

    if (!std::strcmp(a, "-l") || !std::strcmp(a, "--label"))
      label = NextArg(i, argc, argv, a);
    else if (!std::strcmp(a, "-o") || !std::strcmp(a, "--out-dir"))
      outDir = NextArg(i, argc, argv, a);
    else if (!std::strcmp(a, "-s") || !std::strcmp(a, "--sensors"))
      sensorSpec = NextArg(i, argc, argv, a);
    else if (!std::strcmp(a, "-d") || !std::strcmp(a, "--device"))
      device = NextArg(i, argc, argv, a);
    else if (!std::strcmp(a, "-r") || !std::strcmp(a, "--rate"))
      rateHz = std::atof(NextArg(i, argc, argv, a));
    else if (!std::strcmp(a, "-t") || !std::strcmp(a, "--duration"))
      durationSec = std::atof(NextArg(i, argc, argv, a));
    else if (!std::strcmp(a, "--mag-cal"))
      magCal = NextArg(i, argc, argv, a);
    else if (!std::strcmp(a, "--tui"))
      tui = true;
    else if (!std::strcmp(a, "--no-tui"))
      tui = false;
    else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help"))
    {
      Usage(argv[0]);
      return 0;
    }
    else
    {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a);
      Usage(argv[0]);
      return 2;
    }
  }

  if (!tui && !label)
  {
    std::fprintf(stderr, "error: --label is required in --no-tui mode\n");
    Usage(argv[0]);
    return 2;
  }
  if (rateHz <= 0.0)
  {
    std::fprintf(stderr, "error: --rate must be > 0\n");
    return 2;
  }

  Sensors sensors;
  if (!ParseSensors(sensorSpec, sensors))
  {
    std::fprintf(stderr, "error: --sensors must be 'all' or a comma list of "
                 "accel,gyro,mag,baro\n");
    return 2;
  }

#ifndef USE_NCURSES
  if (tui)
  {
    std::fprintf(stderr, "error: this build has no ncurses TUI; rebuild with "
                 "`make NCURSES=1`, or use --label for command-line mode.\n");
    return 2;
  }
#endif

  I2CBus bus(device);
  if (!bus.IsOpen())
  {
    std::fprintf(stderr, "error: cannot open I2C device '%s': %s\n",
                 device, std::strerror(errno));
    return 1;
  }

  // The TUI lets the user toggle any sensor, so bring everything up for it;
  // otherwise only what --sensors selected.
  IMU imu(bus);
  if (sensors.AnyMotion() || tui)
  {
    if (imu.Begin())
      std::printf("IMU detected (sensor identities OK).\n");
    else
      std::printf("Proceeding despite the IMU warning(s) above.\n");

    if (magCal)
    {
      MagCalibration cal;
      if (cal.Load(magCal))
      {
        imu.SetMagCalibration(cal);
        std::printf("Applied magnetometer calibration from %s.\n", magCal);
      }
      else
      {
        std::fprintf(stderr,
                     "warning: could not read mag calibration '%s'; using raw.\n",
                     magCal);
      }
    }
  }

  BMP180 baro(bus);
  if (sensors.baro || tui)
  {
    bool baroId = false;
    uint8_t baroChip = 0;
    if (baro.Begin(baroId, baroChip) && baroId)
      std::printf("BMP180 barometer detected.\n");
    else
      std::fprintf(stderr, "warning: BMP180 not detected (chip id 0x%02X); "
                   "pressure/temperature columns may be invalid.\n", baroChip);
  }

  std::signal(SIGINT, HandleSigint);

  if (tui)
  {
#ifdef USE_NCURSES
    return RunTUI(imu, baro, sensors, outDir, rateHz);
#endif
  }

  // Command-line mode: open one dated file named after the label.
  char path[512];
  MakeFilename(path, sizeof(path), outDir, label);
  std::FILE* csv = std::fopen(path, "w");
  if (!csv)
  {
    std::fprintf(stderr, "error: cannot open '%s' for writing: %s\n",
                 path, std::strerror(errno));
    return 1;
  }
  WriteHeader(csv, sensors);
  std::printf("Recording '%s' (%s) at %.0f Hz to %s. Press Ctrl-C to stop.\n",
              label, sensorSpec, rateHz, path);

  const int rc = RunCommandLine(imu, baro, sensors, csv, rateHz, durationSec);
  std::fflush(csv);
  std::fclose(csv);
  return rc;
}
