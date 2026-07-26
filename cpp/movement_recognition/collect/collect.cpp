/**
 * @file collect.cpp
 * @author Omar Shrit
 *
 * Small data-collection app that records GY-89 sensor data to a CSV file.  It
 * depends only on the drivers in ../driver and argument parsing is hand-rolled,
 * so the binary stays tiny.
 *
 * - choose which sensors to record (`--sensors accel,gyro,mag,baro` or `all`);
 * - timestamp every row with the Unix clock in microseconds;
 * - name the output file `<label>_<date>.csv`, so the label is the file name
 *   (there is no label column).
 *
 * Examples:
 *   collect walking                       # all sensors, cwd, until Ctrl-C
 *   collect stairs_up accel,gyro data      # accel+gyro into data/
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
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "../driver/sensor_board.hpp"

namespace {

std::atomic<bool> g_stop{false};

void HandleSigint(int) { g_stop = true; }

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

void WriteHeader(std::ostream& f, const Sensors& s)
{
  f << "timestamp_unix_us";

  if (s.accel)
    f << ",ax,ay,az";
  if (s.gyro)
    f << ",gx,gy,gz";
  if (s.mag)
    f << ",mx,my,mz";
  if (s.baro)
    f << ",pressure_pa,temp_c";

  f << "\n";
}

void WriteRow(std::ostream& f, const Sensors& s, uint64_t ts, const Reading& r)
{
  // Six significant digits per value, matching the sensors' precision (the
  // stream keeps this setting between calls, so set it once at the start).
  const ImuSample& m = r.motion;
  f << ts;
  if (s.accel)
    f << "," << m.ax << "," << m.ay << "," << m.az;
  if (s.gyro)
    f << "," << m.gx << "," << m.gy << "," << m.gz;
  if (s.mag)
    f << "," << m.mx << "," << m.my << "," << m.mz;
  if (s.baro)
    f << "," << r.pressurePa << "," << r.tempC;

  f << "\n";
}

// Sample the board at `rateHz`, writing one CSV row per sample until the
// duration elapses or Ctrl-C is pressed.
int Record(const SensorBoard& board, std::ostream& csv,
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
      std::cerr << "\nerror: I2C read failed during sampling: "
                << std::strerror(errno) << "\n";
      return 1;
    }

    WriteRow(csv, board.Selected(), ts, reading);

    if (count == 0)
      firstTs = ts;

    lastTs = ts;

    if (++count % kProgressEvery == 0)
      std::cout << "\r  " << count << " samples..." << std::flush;

    if (durationMicros > 0 && ts - firstTs >= durationMicros)
      break;

    nextTick += period;
    std::this_thread::sleep_until(nextTick);
  }

  const double elapsed = (count > 1) ? (lastTs - firstTs) / 1e6 : 0.0;
  const double effHz = (elapsed > 0.0) ? (count - 1) / elapsed : 0.0;
  std::cout << "\rWrote " << count << " samples in "
            << std::fixed << std::setprecision(2) << elapsed << " s (effective "
            << std::setprecision(1) << effHz << " Hz).\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  // Arguments are positional: the label is required (it becomes the CSV file
  // name); the rest are optional and fall back to sensible defaults if omitted.
  // For device pass "/dev/i2c-0" and for mag-cal pass "-" to skip it.
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <label> [sensors] [out-dir] [device]"
                 " [rate-hz] [duration-sec] [mag-cal]\n";
    return 1;
  }

  const std::string label      = argv[1];
  const std::string sensorSpec = argc > 2 ? argv[2] : "all";
  const std::string outDir     = argc > 3 ? argv[3] : ".";
  const std::string device     = argc > 4 ? argv[4] : "/dev/i2c-0";
  const double rateHz          = argc > 5 ? std::stod(argv[5]) : 100.0;
  const double durationSec     = argc > 6 ? std::stod(argv[6]) : 0.0;
  const std::string magCal     = (argc > 7 && std::string(argv[7]) != "-")
                                     ? argv[7] : "";

  if (rateHz <= 0.0)
  {
    std::cerr << "error: rate-hz must be > 0\n";
    return 2;
  }

  Sensors sensors;

  if (!ParseSensors(sensorSpec, sensors))
  {
    std::cerr << "error: sensors must be 'all' or a comma list of "
                 "accel,gyro,mag,baro\n";
    return 2;
  }

  // The whole GY-89 board behind one object: open the bus and bring up only the
  // selected sensors.
  SensorBoard board(device, sensors);

  if (!board.Begin(magCal))
    return 1;

  std::signal(SIGINT, HandleSigint);

  // Open one dated file named after the label.
  const std::string path = MakeFilename(outDir, label);

  std::ofstream csv(path);

  if (!csv)
  {
    std::cerr << "error: cannot open '" << path << "' for writing: "
              << std::strerror(errno) << "\n";
    return 1;
  }

  // Six significant digits per value (see WriteRow); set once for the file.
  csv << std::setprecision(6);

  WriteHeader(csv, sensors);
  std::cout << "Recording '" << label << "' (" << sensorSpec << ") at "
            << std::fixed << std::setprecision(0) << rateHz << " Hz to " << path
            << ". Press Ctrl-C to stop.\n";

  const int rc = Record(board, csv, rateHz, durationSec);

  csv.close();

  return rc;
}
