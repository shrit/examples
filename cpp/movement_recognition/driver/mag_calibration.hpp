/**
 * @file driver/mag_calibration.hpp
 * @author Omar Shrit
 *
 * Magnetometer calibration: a per-axis hard-iron offset plus a simple soft-iron
 * scale.  Raw magnetometer readings are distorted by nearby ferromagnetic
 * material and the device's own electronics; uncorrected, the field vector
 * traces an off-centre, stretched ellipsoid as the sensor is rotated.  Hard-iron
 * correction re-centres it and the scale equalizes the axes.  Lives in the
 * driver directory so the sensor code is self-contained for testing/adjustment.
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */
#ifndef MOVEMENT_RECOGNITION_DRIVER_MAG_CALIBRATION_HPP
#define MOVEMENT_RECOGNITION_DRIVER_MAG_CALIBRATION_HPP

#include <string>

/**
 * Correction applied as: corrected_i = (raw_i - offset_i) * scale_i.  The
 * default (zero offset, unit scale) is the identity transform.
 */
struct MagCalibration
{
  float offset[3] = {0.f, 0.f, 0.f};  //!< hard-iron offsets (gauss)
  float scale[3]  = {1.f, 1.f, 1.f};  //!< soft-iron per-axis scale

  //! Apply the correction in place.
  void Apply(float& mx, float& my, float& mz) const;

  /**
   * Compute calibration from the per-axis min/max observed while rotating the
   * sensor through all orientations.  Offsets are the range midpoints; scales
   * normalize each axis to the average half-range.
   */
  static MagCalibration FromMinMax(const float minVals[3],
                                   const float maxVals[3]);

  //! Save as a small, human-readable text file.  Returns false on I/O error.
  bool Save(const std::string& path) const;

  //! Load a calibration previously written by Save().  Returns false on error.
  bool Load(const std::string& path);
};

#endif
