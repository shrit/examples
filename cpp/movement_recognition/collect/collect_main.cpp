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

  if (!label)
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

  I2CBus bus(device);
  if (!bus.IsOpen())
  {
    std::fprintf(stderr, "error: cannot open I2C device '%s': %s\n",
                 device, std::strerror(errno));
    return 1;
  }

  // Bring up only the sensors that --sensors selected.
  IMU imu(bus);
  if (sensors.AnyMotion())
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
  if (sensors.baro)
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

  // Open one dated file named after the label.
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
