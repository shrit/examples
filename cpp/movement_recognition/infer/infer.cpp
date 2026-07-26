/**
 * @file infer/infer.cpp
 * @author Omar Shrit
 *
 * Live movement-recognition inference on the device.  It reads the GY-89 IMU
 * directly, slides a window over the stream, runs the SAME per-channel FFT used
 * for training (one arma::fft call) to get features, and classifies each window
 * with the f32 neural network trained by `train`.
 *
 * `train` writes a model as three files in one directory: model.bin (the
 * trained weights), scaler.bin (the feature scaler) and model.labels (a text
 * file listing the window size, step, and class names).  Point infer at that
 * directory.  ("-" for mag-cal skips it.)
 *
 *   infer accel /dev/i2c-0 - model                 # prints predictions to stdout
 *
 * Everything runs in f32 to stay light on the 64 MB device.  The feature
 * extraction here is deliberately identical to train/train.cpp -- keep the two
 * in sync if you change one.
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Enable mlpack's neural-network serialization so we can load the trained
// network with data::Load (see the mnist_simple_f32 example).
#define MLPACK_ENABLE_ANN_SERIALIZATION
#include <mlpack.hpp>

#include "../driver/sensor_board.hpp"

using namespace mlpack;

// Register mlpack's layers for serialization, using f32 (arma::fmat) to match
// the network we load.
CEREAL_REGISTER_MLPACK_LAYERS(arma::fmat);

namespace {

std::atomic<bool> g_stop{false};
void HandleSigint(int) { g_stop = true; }

// Sensors / ParseSensors come from the driver's sensor_board.hpp, shared with
// collect so the two agree on the canonical channel order: accel, gyro, mag,
// baro (the same order collect writes to CSV and train consumes).

// --- the two functions that "organize the data for the FFT" ----------------

// One board reading -> a column of the active channels, in the canonical order.
// This is how an arbitrary set of sensors becomes a fixed-height feature column.
arma::fvec SampleToColumn(const Sensors& s, const Reading& r)
{
  const ImuSample& m = r.motion;
  arma::fvec col(s.Channels());
  size_t i = 0;

  if (s.accel)
  {
    col[i++] = m.ax;
    col[i++] = m.ay;
    col[i++] = m.az;
  }

  if (s.gyro)
  {
    col[i++] = m.gx;
    col[i++] = m.gy;
    col[i++] = m.gz;
  }

  if (s.mag)
  {
    col[i++] = m.mx;
    col[i++] = m.my;
    col[i++] = m.mz;
  }

  if (s.baro)
  {
    col[i++] = r.pressurePa;
    col[i++] = r.tempC;
  }

  return col;
}

// ===========================================================================
// Turning one window into a feature vector (must match train's pipeline)
// ===========================================================================
//
// infer feeds each live window through the SAME transform train used, so the
// features line up with the model.  Here `win` is the current sliding window
// (the newest `window` samples the sensor loop has buffered), shape
// (channels x window).  The example below uses accel only (3 channels: ax, ay,
// az) with window = 256 (the training default); the code works for any
// channel count and window length.  arma::fft transforms each COLUMN
// independently, so the trick is to put each channel in a column:
//
//   1. the window                       2. transpose: win.t()
//      (channels x window)                  (window x channels), channel = column
//                                                  ax     ay     az
//          s0  s1 ... s255            t0   [  ax0    ay0    az0  ]
//    ax  [ ax0 ax1... ax255]          t1   [  ax1    ay1    az1  ]
//    ay  [ ay0 ay1... ay255]   ──►    ...  [  ...    ...    ...  ]
//    az  [ az0 az1... az255]          t255 [  ax255  ay255  az255]
//
//   3. arma::fft runs DOWN each column       4. one-sided half + power:
//      (one 1-D FFT per channel, batched          square(abs(rows(0,numBins-1)))
//       in a single call, no cross-mixing)         numBins = 256/2+1 = 129
//           FFT(ax) FFT(ay) FFT(az)                   pow(ax) pow(ay) pow(az)
//     bin0  [ AX0    AY0    AZ0  ]              bin0   [  .       .       .  ]
//     bin1  [ AX1    AY1    AZ1  ]      ──►      ...   (129 rows kept)
//     ...   [ ...    ...    ...  ]              bin128 [  .       .       .  ]
//     bin255[ AX255  AY255  AZ255]
//
//   5. vectorise() flattens the power matrix column by column, then the raw
//      per-channel time-domain stats (mean, stddev, median) are appended:
//
//      [ ax bin0..bin128 | ay bin0..bin128 | az bin0..bin128 | mean | std | median ]
//        └── 129 ──────┘ └── 129 ──────┘ └── 129 ──────┘ └ 3 ─┘└3 ─┘└─ 3 ──┘
//        └──────── FFT power: channels*(window/2+1) = 387 ────┘└ stats: 3*channels=9┘
//
//      feature length = channels*(window/2+1) + 3*channels = 387 + 9 = 396.
//
// See train.cpp for the full pipeline (recording -> sliding windows -> here).
// ===========================================================================

// A (channels x window) window -> one feature vector: the one-sided FFT power
// spectrum of each channel, followed by per-channel mean, stddev, and median.
// MUST match train's WindowToFeatures.
arma::fvec WindowToFeatures(const arma::fmat& win)
{
  const size_t numBins = win.n_cols / 2 + 1;

  const arma::cx_fmat spectrum = arma::fft(win.t());
  const arma::fmat power = arma::square(arma::abs(spectrum.rows(0, numBins - 1)));

  return arma::join_cols(arma::vectorise(power),
                         arma::mean(win, 1),
                         arma::stddev(win, 0, 1),
                         arma::median(win, 1));
}

using Network =
    FFN<NegativeLogLikelihoodType<arma::fmat>, GlorotInitialization, arma::fmat>;

// Read the model's metadata from the "key=value" text file model.labels that
// train writes: the window size, window step, and class names.  Returns false
// with a message if it is missing the essentials.
bool LoadModelMetadata(const std::string& path, size_t& window, size_t& step,
                       std::vector<std::string>& classes)
{
  std::ifstream f(path);
  if (!f)
  {
    std::cerr << "error: cannot read '" << path << "'\n";
    return false;
  }

  std::string line;
  while (std::getline(f, line))
  {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), val = line.substr(eq + 1);

    if (key == "window") window = std::stoul(val);
    else if (key == "step")   step = std::stoul(val);
    else if (key == "classes")
    {
      std::string c;
      std::stringstream ss(val);
      while (std::getline(ss, c, ','))
        classes.push_back(c);
    }
  }

  if (window == 0 || classes.empty())
  {
    std::cerr << "error: '" << path << "' is missing window= or classes=\n";
    return false;
  }
  return true;
}

// Classify one feature column -> class name (arg-max of the network output).
// Apply the SAME standardization train fitted (mlpack's scaler works in double)
// before predicting.
std::string Classify(Network& nn, data::StandardScaler& scaler,
                     const std::vector<std::string>& classes,
                     const arma::fmat& feat)
{

  // Note to future Omar, need to remove the conversion here after I merge the
  // scalar f32 PR.
  // The same thing needs to be done in the training code function
  arma::mat scaled;
  scaler.Transform(arma::conv_to<arma::mat>::from(feat), scaled);

  arma::fmat scores;
  nn.Predict(arma::conv_to<arma::fmat>::from(scaled), scores);
  const size_t idx = scores.col(0).index_max();
  return idx < classes.size() ? classes[idx] : "?";
}

// --- the live classification loop -------------------------------------------

// Sample the board at `rateHz`, and every time `window` samples have been
// buffered, turn them into features, classify, print the prediction, and slide
// the window forward by `step`.  Runs until Ctrl-C.
int RunInference(const SensorBoard& board, Network& nn,
                 data::StandardScaler& scaler,
                 const std::vector<std::string>& classes,
                 size_t window, size_t step, double rateHz)
{
  using namespace std::chrono;
  const Sensors& sensors = board.Selected();
  const steady_clock::duration period =
      duration_cast<steady_clock::duration>(duration<double>(1.0 / rateHz));
  std::deque<arma::fvec> buf;
  steady_clock::time_point nextTick = steady_clock::now();

  while (!g_stop.load())
  {
    Reading reading;
    if (!board.Read(reading))
    {
      std::cerr << "error: I2C read failed during sampling\n";
      return 1;
    }
    buf.push_back(SampleToColumn(sensors, reading));

    if (buf.size() >= window)
    {
      // Pack the last `window` samples into a (channels x window) matrix.
      arma::fmat win(sensors.Channels(), window);
      for (size_t j = 0; j < window; ++j)
        win.col(j) = buf[j];

      const std::string pred =
          Classify(nn, scaler, classes, WindowToFeatures(win));
      // std::endl flushes, so each prediction appears live.
      std::cout << "predict: " << pred << std::endl;

      // Slide the window forward by `step` samples.
      for (size_t j = 0; j < step && !buf.empty(); ++j)
        buf.pop_front();
    }

    nextTick += period;
    std::this_thread::sleep_until(nextTick);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  // Positional arguments: the sensors, the I2C device, the magnetometer
  // calibration file ("-" to skip), and the directory train wrote the model to
  // (model.bin, scaler.bin, model.labels).  The sample rate is 100 Hz; the
  // window and step come from model.labels.
  if (argc != 5)
  {
    std::cerr << "Usage: " << argv[0]
              << " <sensors> <device> <mag-cal> <model-dir>\n"
                 "  e.g. " << argv[0] << " accel /dev/i2c-0 - model\n";
    return 1;
  }

  const std::string sensorSpec = argv[1];
  const std::string device     = argv[2];
  const std::string magCal     = std::string(argv[3]) == "-" ? "" : argv[3];
  const std::string modelDir   = argv[4];
  const double rateHz = 100.0;

  Sensors sensors;
  if (!ParseSensors(sensorSpec, sensors))
  {
    std::cerr << "error: sensors must be 'all' or a comma list of "
                 "accel,gyro,mag,baro\n";
    return 2;
  }

  // The model's three files have fixed names inside the directory.
  namespace fs = std::filesystem;
  const std::string modelFile  = (fs::path(modelDir) / "model.bin").string();
  const std::string scalerFile = (fs::path(modelDir) / "scaler.bin").string();
  const std::string labelsFile = (fs::path(modelDir) / "model.labels").string();

  // Read the metadata (window/step/classes), then the network and its scaler.
  size_t window = 0, step = 0;
  std::vector<std::string> classes;
  if (!LoadModelMetadata(labelsFile, window, step, classes))
    return 1;
  // Older label files have no step= line; fall back to a 50% overlap.
  if (step == 0)
    step = std::max<size_t>(1, window / 2);

  Network nn;
  data::StandardScaler scaler;
  data::Load(modelFile, "model", nn, true);
  data::Load(scalerFile, "scaler", scaler, true);

  // Quick dimension check before running the inference loop, Our target here
  // is to verify that the channels from the sensors and the features numbers
  // after FFT matches the expected input by the neural network.
  const size_t featDim =
      sensors.Channels() * (window / 2 + 1) + sensors.Channels() * 3;
  const size_t expected =
      nn.InputDimensions().empty() ? 0 : nn.InputDimensions()[0];
  if (expected != featDim)
  {
    std::cerr << "error: '" << modelDir << "' expects " << expected
              << " features but the chosen sensors/window produce " << featDim
              << " -- they must match training.\n";
    return 1;
  }
  std::cerr << "loaded model from '" << modelDir << "' (window " << window
            << ", " << classes.size() << " classes)\n";

  // The whole GY-89 board behind one object: open the bus and bring up only the
  // sensors the model was trained on.
  SensorBoard board(device, sensors);
  if (!board.Begin(magCal))
    return 1;

  std::signal(SIGINT, HandleSigint);

  return RunInference(board, nn, scaler, classes, window, step, rateHz);
}
