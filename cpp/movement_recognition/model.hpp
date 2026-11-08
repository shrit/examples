/**
 * @file model.hpp
 * @author Omar Shrit
 *
 * The trained movement-recognition model: the network, the feature scaler, and
 * the class names, bundled so they serialize into a single `.bin` file.
 */
#ifndef MOVEMENT_RECOGNITION_MODEL_HPP
#define MOVEMENT_RECOGNITION_MODEL_HPP

#include <mlpack.hpp>

struct MovementModel
{
  mlpack::FFN<mlpack::NegativeLogLikelihoodType<arma::fmat>,
              mlpack::GlorotInitialization,
              arma::fmat> net;
  mlpack::data::StandardScaler<arma::fmat> scaler;
  std::vector<std::string> classes;

  template<typename Archive>
  void serialize(Archive& ar, const unsigned int /* version */)
  {
    ar(CEREAL_NVP(net));
    ar(CEREAL_NVP(scaler));
    ar(CEREAL_NVP(classes));
  }
};

#endif
