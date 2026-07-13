/**
 * @file collect_main.cpp
 * @author Omar Shrit
 *
 * Small data-collection app that records GY-89 sensor data to a CSV file.  It
 * depends only on the drivers in ../driver; argument parsing is hand-rolled and
 * all I/O is C stdio, so the binary stays tiny and exception-free.
 *
 * - choose which sensors to record (`--sensors accel,gyro,mag,baro` or `all`);
 * - timestamp every row with the Unix clock in microseconds;
 * - name the output file `<label>_<date>.csv`, so the label is the file name
 *   (there is no label column).
 *
 * Examples:
 *   collect --label walking --sensors all --duration 30
 *   collect --label stairs_up --sensors accel,gyro --out-dir data
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
#include <sstream>
#include <string>
#include <thread>

#include "../driver/sensor_board.hpp"

namespace {

std::atomic<bool> g_stop{false};

void HandleSigint(int) { g_stop = true; }

// Return the next command-line value after argv[i], advancing i.  Exits with an
// error if the option was given without a value.
std::string NextArg(int& i, int argc, char** argv, const std::string& name)
{
  if (i + 1 >= argc)
  {
    std::fprintf(stderr, "error: %s requires a value\n", name.c_str());
    std::exit(2);
  }
  return argv[++i];
}

// Parse "all" or a comma list like "accel,gyro,mag,baro".  Returns false if the
// spec is empty or contains an unknown name.
bool ParseSensors(const std::string& spec, Sensors& out)
{
  if (spec == "all")
  {
    out = { true, true, true, true };
    return true;
  }

  std::string token;
  std::stringstream ss(spec);

  while (std::getline(ss, token, ','))
  {
    if (token == "accel")
      out.accel = true;
    else if (token == "gyro")
      out.gyro = true;
    else if (token == "mag")
      out.mag = true;
    else if (token == "baro")
      out.baro = true;
    else
      return false;
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
std::string MakeFilename(const std::string& dir, const std::string& label)
{
  const time_t now = std::time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
  return dir + "/" + label + "_" + stamp + ".csv";
}

void WriteHeader(std::FILE* f, const Sensors& s)
{
  std::fprintf(f, "timestamp_unix_us");

  if (s.accel)
    std::fprintf(f, ",ax,ay,az");
  if (s.gyro)
    std::fprintf(f, ",gx,gy,gz");
  if (s.mag)
    std::fprintf(f, ",mx,my,mz");
  if (s.baro)
    std::fprintf(f, ",pressure_pa,temp_c");

  std::fprintf(f, "\n");
}

void WriteRow(std::FILE* f, const Sensors& s, uint64_t ts, const Reading& r)
{
  const ImuSample& m = r.motion;
  std::fprintf(f, "%llu", static_cast<unsigned long long>(ts));
  if (s.accel)
    std::fprintf(f, ",%.6g,%.6g,%.6g", m.ax, m.ay, m.az);
  if (s.gyro)
    std::fprintf(f, ",%.6g,%.6g,%.6g", m.gx, m.gy, m.gz);
  if (s.mag)
    std::fprintf(f, ",%.6g,%.6g,%.6g", m.mx, m.my, m.mz);
  if (s.baro)
    std::fprintf(f, ",%.6g,%.6g", r.pressurePa, r.tempC);

  std::fprintf(f, "\n");
}

void Usage(const char* prog)
{
  std::printf(
      "Record GY-89 sensor data to a CSV file (one file per recording, named\n"
      "<label>_<date>.csv).\n\n"
      "Usage: %s --label NAME [options]\n"
      "  -l, --label NAME    label for this recording (used as the file name)\n"
      "  -o, --out-dir DIR   directory for the output file (default .)\n"
      "  -s, --sensors LIST  comma list of accel,gyro,mag,baro -- or all "
      "(default all)\n"
      "  -d, --device PATH   I2C device (default /dev/i2c-0)\n"
      "  -r, --rate HZ       sampling rate (default 100)\n"
      "  -t, --duration SEC  seconds to record, 0 = until Ctrl-C (default 0)\n"
      "      --mag-cal FILE  apply a magnetometer calibration file\n"
      "  -h, --help          show this help\n",
      prog);
}

// Sample the board at `rateHz`, writing one CSV row per sample until the
// duration elapses or Ctrl-C is pressed.
int Record(const SensorBoard& board, std::FILE* csv,
           double rateHz, double durationSec)
{
  using namespace std::chrono;
  const steady_clock::duration period =
      duration_cast<steady_clock::duration>(duration<double>(1.0 / rateHz));
  const uint64_t durationMicros =
      static_cast<uint64_t>(durationSec * 1e6);

  // Report progress to the terminal every this many samples.
  constexpr uint64_t kProgressEvery = 50;

  uint64_t count = 0, firstTs = 0, lastTs = 0;
  steady_clock::time_point nextTick = steady_clock::now();

  while (!g_stop.load())
  {
    const uint64_t ts = UnixMicros();

    Reading reading;

    if (!board.Read(reading))
    {
      std::fprintf(stderr, "\nerror: I2C read failed during sampling: %s\n",
                   std::strerror(errno));
      return 1;
    }

    WriteRow(csv, board.Selected(), ts, reading);

    if (count == 0)
      firstTs = ts;

    lastTs = ts;

    if (++count % kProgressEvery == 0)
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

}  // namespace

int main(int argc, char** argv)
{
  std::string device = "/dev/i2c-0";
  std::string label;
  std::string outDir = ".";
  std::string sensorSpec = "all";
  std::string magCal;
  double rateHz = 100.0;
  double durationSec = 0.0;

  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];

    if (a == "-l" || a == "--label")
      label = NextArg(i, argc, argv, a);
    else if (a == "-o" || a == "--out-dir")
      outDir = NextArg(i, argc, argv, a);
    else if (a == "-s" || a == "--sensors")
      sensorSpec = NextArg(i, argc, argv, a);
    else if (a == "-d" || a == "--device")
      device = NextArg(i, argc, argv, a);
    else if (a == "-r" || a == "--rate")
      rateHz = std::atof(NextArg(i, argc, argv, a).c_str());
    else if (a == "-t" || a == "--duration")
      durationSec = std::atof(NextArg(i, argc, argv, a).c_str());
    else if (a == "--mag-cal")
      magCal = NextArg(i, argc, argv, a);
    else if (a == "-h" || a == "--help")
    {
      Usage(argv[0]);
      return 0;
    }
    else
    {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      Usage(argv[0]);
      return 2;
    }
  }

  if (label.empty())
  {
    std::fprintf(stderr, "error: --label is required\n");
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

  // The whole GY-89 board behind one object: open the bus and bring up only the
  // sensors that --sensors selected.
  SensorBoard board(device, sensors);

  if (!board.Begin(magCal))
    return 1;

  std::signal(SIGINT, HandleSigint);

  // Open one dated file named after the label.
  const std::string path = MakeFilename(outDir, label);

  std::FILE* csv = std::fopen(path.c_str(), "w");

  if (!csv)
  {
    std::fprintf(stderr, "error: cannot open '%s' for writing: %s\n",
                 path.c_str(), std::strerror(errno));
    return 1;
  }

  WriteHeader(csv, sensors);
  std::printf("Recording '%s' (%s) at %.0f Hz to %s. Press Ctrl-C to stop.\n",
              label.c_str(), sensorSpec.c_str(), rateHz, path.c_str());

  const int rc = Record(board, csv, rateHz, durationSec);

  std::fflush(csv);
  std::fclose(csv);

  return rc;
}
