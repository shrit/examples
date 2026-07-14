/**
 * @file train/train.cpp
 * @author Omar Shrit
 *
 * Train a movement-recognition model, on the device, from the CSV files written
 * by the collect tool. It loads each * <label>_<date>.csv (the label is the file
 * name), cuts it into windows, runs an FFT per channel (one arma::fft call per
 * window) to get features, and trains a small f32 neural network.  No shared
 * "common" library -- everything is here.
 *
 *   train DIR 64 model         # data dir, window 64, output prefix "model"
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */
#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

// Enable mlpack's neural-network serialization so we can save/load the trained
// network with data::Save/data::Load (see the mnist_simple_f32 example).
#define MLPACK_ENABLE_ANN_SERIALIZATION

#include <mlpack.hpp>

using namespace mlpack;

// Register mlpack's layers for serialization, using f32 (arma::fmat) to match
// the network below.
CEREAL_REGISTER_MLPACK_LAYERS(arma::fmat);

namespace fs = std::filesystem;

constexpr size_t kHidden = 64;

namespace {

// Label = file name without the trailing "_<date>" (the date has no underscore,
// so the label is everything before the last '_'): stairs_up_20260619-...csv ->
// "stairs_up".
std::string LabelOf(const fs::path& p)
{
  const std::string stem = p.stem().string();
  const size_t u = stem.rfind('_');

  if (u == std::string::npos)
    return stem;

  return stem.substr(0, u);
}

// Step 1 of feature building: load one recording from disk.
//
// Loads <label>_<date>.csv into `raw` with the sensor channels as rows and the
// time samples as columns (mlpack loads text column-major and transposes by
// default, so each CSV column becomes a row).  The first row -- the Unix
// timestamp -- is dropped, leaving only sensor channels.  Returns false (and
// prints why) when the file is missing, malformed, or shorter than one window.
bool LoadRecording(const fs::path& path, size_t window, arma::fmat& raw)
{
  data::TextOptions opts;
  opts.HasHeaders() = true;  // skip the "timestamp_unix_us,ax,ay,..." header row

  std::cerr << "  load " << path.filename().string() << " ... ";

  if (!data::Load(path.string(), raw, opts))
  {
    std::cerr << "FAILED to load (skipped)\n";
    return false;
  }

  std::cerr << "loaded " << raw.n_rows << " channels(+ts) x " << raw.n_cols
            << " samples";

  if (raw.n_rows < 2)
  {
    std::cerr << " -- < 2 rows, skipped\n";
    return false;
  }

  if (raw.n_cols < window)
  {
    std::cerr << " -- fewer than --window (" << window << ") samples, skipped\n";
    return false;
  }

  // mlpack is column-major, so right now the sensor channels are the rows.
  raw.shed_row(0);  // drop the timestamp row; the remaining rows are channels

  return true;
}

// Step 2 of feature building: FFT pre-processing.
//
// Cut one recording (`raw`, channels x samples) into non-overlapping windows of
// `window` samples.  For each window we run a single arma::fft over all channels
// at once, keep the one-sided magnitude spectrum (window / 2 + 1 bins per
// channel), stack the channels into one feature column, and tag it with
// `classIdx`.  The columns/labels are appended to `cols`/`labels`.
void ExtractWindows(const arma::fmat& raw, size_t window, size_t classIdx,
                    std::vector<arma::fvec>& cols,
                    std::vector<size_t>& labels)
{
  size_t numWindows = 0;

  for (size_t s = 0; s + window <= raw.n_cols; s += window)
  {
    // raw.cols(s, s + window - 1) is (channels x window).  Transpose it so each
    // channel becomes a column, run the FFT down each column, then keep the
    // lower (one-sided) half of the magnitude spectrum.
    const arma::mat windowSamples =
        arma::conv_to<arma::mat>::from(raw.cols(s, s + window - 1).t());
    const arma::cx_mat spectrum = arma::fft(windowSamples);
    const arma::mat magnitude = arma::abs(spectrum.rows(0, window / 2));

    cols.push_back(arma::conv_to<arma::fvec>::from(arma::vectorise(magnitude)));
    labels.push_back(classIdx);
    ++numWindows;
  }

  const size_t numFeatures = cols.empty() ? 0 : cols.back().n_elem;
  std::cerr << " -> " << numWindows << " windows of " << numFeatures
            << " features\n";
}

double Accuracy(const arma::Row<size_t>& pred, const arma::Row<size_t>& truth)
{
  if (truth.n_elem == 0)
    return 0.0;

  return (double) arma::accu(pred == truth) / truth.n_elem;
}

// --- the classifier: a small f32 neural network --------------------------
//
// TrainNN takes the training split (trainData / trainLabels) and the held-out
// test split (testData / testLabels).  trainData / testData are feature matrices
// with one FFT-feature column per window; trainLabels / testLabels are the
// matching class indices.  `patience` is the early-stopping patience (below).

void TrainNN(const arma::fmat& trainData, const arma::Row<size_t>& trainLabels,
             const arma::fmat& testData, const arma::Row<size_t>& testLabels,
             size_t numClasses, size_t patience, const std::string& out)
{
  // A small feed-forward network, all in f32 to stay light on the device:
  //   Linear(kHidden) -> ReLU -> Linear(numClasses) -> LogSoftMax
  // LogSoftMax paired with NegativeLogLikelihood is the standard classification
  // head (same shape as the mnist_simple_f32 example).
  FFN<NegativeLogLikelihoodType<arma::fmat>, GlorotInitialization, arma::fmat> net;
  net.Add<Linear<arma::fmat>>(kHidden);
  net.Add<ReLU<arma::fmat>>();
  net.Add<Linear<arma::fmat>>(numClasses);
  net.Add<LogSoftMax<arma::fmat>>();

  // mlpack expects the responses as a 1 x N row of class indices.
  const arma::fmat responses = arma::conv_to<arma::fmat>::from(trainLabels);
  // The held-out split, used by early stopping below as a validation set.
  const arma::fmat testResponses = arma::conv_to<arma::fmat>::from(testLabels);

  // Adam optimizer (same arguments as the mnist_simple_f32 example).  The max
  // iterations is left at 0 (no fixed limit): how long we train is decided by the
  // early-stopping callback below, not by a fixed epoch count.
  ens::Adam optimizer(
      1e-2,    // step size (learning rate)
      32,      // batch size: points used per optimizer step
      0.9,     // exp. decay for the 1st moment estimate
      0.999,   // exp. decay for the 2nd moment estimate
      1e-8,    // epsilon for numerical stability
      0,       // max iterations: 0 = no limit; the early-stop callback decides
      1e-8,    // tolerance: stop if the loss barely moves
      true);   // shuffle the data between epochs

  // Early stopping on the *validation* loss (the held-out split).  The callback
  // is evaluated each epoch on testData and returns the validation loss;
  // EarlyStopAtMinLoss stops once that loss has not improved for `patience`
  // epochs past its lowest value.  Watching the validation -- not the training --
  // loss is the whole point: the training loss keeps creeping down as the net
  // memorises the data, so a training-loss early stop would run for thousands of
  // epochs without ever triggering.
  //
  // We also remember the weights at the validation minimum (bestParams) and
  // restore them afterwards, so we save the best-generalising model rather than
  // the slightly over-fit weights from the extra `patience` epochs.  PrintLoss
  // and ProgressBar just let the user watch training.
  double bestValLoss = std::numeric_limits<double>::infinity();
  arma::fmat bestParams;
  ens::EarlyStopAtMinLossType<arma::fmat> earlyStop(
      [&](const arma::fmat& /* param */)
      {
        const double valLoss = net.Evaluate(testData, testResponses);
        if (valLoss < bestValLoss)
        {
          bestValLoss = valLoss;
          bestParams = net.Parameters();
        }
        return valLoss;
      },
      patience);

  net.Train(trainData, responses, optimizer,
            ens::PrintLoss(),
            ens::ProgressBar(),
            earlyStop);

  if (!bestParams.is_empty())
    net.Parameters() = bestParams;

  // Test-set accuracy: predict log-probabilities, take the arg-max class per
  // column (one column == one window).
  arma::fmat scores;
  net.Predict(testData, scores);

  arma::Row<size_t> pred(scores.n_cols);
  for (size_t i = 0; i < scores.n_cols; ++i)
  {
    pred[i] = scores.col(i).index_max();
  }
  std::cout << "neural net test accuracy: " << Accuracy(pred, testLabels)
            << "\n";

  // Save the whole network with mlpack's serialization: architecture, weights,
  // and input size all go into one file, so `infer` loads it with a single
  // data::Load (same as the mnist_simple_f32 example).
  data::Save(out + ".bin", "model", net, false);
}

}  // namespace

int main(int argc, char** argv)
{
  // Arguments are positional: the data directory is required; the rest are
  // optional and fall back to sensible defaults if omitted.
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0]
              << " <data-dir> [window] [out-prefix] [patience] [test-split]\n";
    return 1;
  }

  const std::string dataDir = argv[1];
  const size_t window       = argc > 2 ? std::stoul(argv[2]) : 64;
  const std::string out     = argc > 3 ? argv[3] : "model";
  const size_t patience     = argc > 4 ? std::stoul(argv[4]) : 10;
  const double testSplit    = argc > 5 ? std::stod(argv[5]) : 0.2;

  if (!fs::is_directory(dataDir))
  {
    std::cerr << "error: '" << dataDir << "' is not a directory\n";
    return 1;
  }

  // Collect the CSV files in the data directory.
  std::vector<fs::path> files;
  for (const fs::directory_entry& entry : fs::directory_iterator(dataDir))
  {
    if (entry.path().extension() == ".csv")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  std::cerr << "data dir '" << dataDir << "': " << files.size()
            << " .csv file(s), window=" << window << "\n";
  if (files.empty())
  {
    std::cerr << "error: no .csv files in '" << dataDir << "'\n";
    return 1;
  }

  // Map each label (the file name) to a class index, then turn every file into
  // feature columns: load it (LoadRecording) and FFT-window it (ExtractWindows).
  std::map<std::string, size_t> classOf;
  std::vector<std::string> classNames;
  std::vector<arma::fvec> cols;
  std::vector<size_t> labels;

  for (size_t f = 0; f < files.size(); ++f)
  {
    const fs::path& path = files[f];
    const std::string label = LabelOf(path);

    if (classOf.count(label) == 0)
    {
      classOf[label] = classNames.size();
      classNames.push_back(label);
    }
    const size_t classIdx = classOf[label];

    std::cerr << "[" << label << "]\n";

    arma::fmat raw;
    if (LoadRecording(path, window, raw))
      ExtractWindows(raw, window, classIdx, cols, labels);
  }

  // Every file must yield the same feature length (same sensors + window);
  // mixing sensor sets would make the columns un-stackable.
  const size_t feat = cols.empty() ? 0 : cols[0].n_elem;
  for (size_t i = 0; i < cols.size(); ++i)
  {
    if (cols[i].n_elem != feat)
    {
      std::cerr << "error: window " << i << " has " << cols[i].n_elem
                << " features but the first has " << feat
                << " -- were the files recorded with different --sensors?\n";
      return 1;
    }
  }

  std::cerr << "totals: " << cols.size() << " windows, " << feat
            << " features, " << classNames.size() << " classes (";
  for (size_t i = 0; i < classNames.size(); ++i)
  {
    std::cerr << (i ? ", " : "") << classNames[i];
  }
  std::cerr << ")\n";

  if (cols.size() < 2 || classNames.size() < 2)
  {
    std::cerr << "error: need >=2 windows and >=2 labels. Collect more data, "
                 "or lower --window (currently " << window
              << ") so short recordings yield windows.\n";
    return 1;
  }

  // Assemble the feature matrix X (one column per window) and the label row y.
  arma::fmat X(feat, cols.size());
  arma::Row<size_t> y(cols.size());
  for (size_t i = 0; i < cols.size(); ++i)
  {
    X.col(i) = cols[i];
    y[i] = labels[i];
  }
  std::cout << X.n_cols << " windows, " << X.n_rows << " features, "
            << classNames.size() << " classes.\n";

  // Split into a training set and a held-out test set.  trainData / testData are
  // the feature matrices, trainLabels / testLabels the matching class indices.
  // mlpack's data::Split shuffles and splits in one call (testSplit = fraction
  // held out for testing).
  arma::fmat trainData, testData;
  arma::Row<size_t> trainLabels, testLabels;
  data::Split(X, y, trainData, testData, trainLabels, testLabels, testSplit);
  std::cerr << "split: " << trainData.n_cols << " train, " << testData.n_cols
            << " test\n";

  TrainNN(trainData, trainLabels, testData, testLabels, classNames.size(),
          patience, out);

  // Sidecar: model type, window size, and class names, so the inference tool can
  // load the model and reproduce the exact features.
  std::ofstream meta(out + ".labels");
  meta << "model=nn\nwindow=" << window << "\n";

  meta << "classes=";
  for (size_t i = 0; i < classNames.size(); ++i)
  {
    meta << (i ? "," : "") << classNames[i];
  }
  meta << "\n";

  std::cout << "saved " << out << ".bin (+ " << out << ".labels)\n";
  return 0;
}
