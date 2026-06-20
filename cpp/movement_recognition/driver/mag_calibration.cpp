/**
 * @file driver/mag_calibration.cpp
 * @author Omar Shrit
 *
 * Implementation of the magnetometer calibration helpers.  File I/O uses C
 * stdio (not iostream) to keep the binaries small.
 *
 * mlpack is free software; you may redistribute it and/or modify it under the
 * terms of the 3-clause BSD license.  You should have received a copy of the
 * 3-clause BSD license along with mlpack.  If not, see
 * http://www.opensource.org/licenses/BSD-3-Clause for more information.
 */
#include "mag_calibration.hpp"

#include <cstdio>

void MagCalibration::Apply(float& mx, float& my, float& mz) const
{
  mx = (mx - offset[0]) * scale[0];
  my = (my - offset[1]) * scale[1];
  mz = (mz - offset[2]) * scale[2];
}

MagCalibration MagCalibration::FromMinMax(const float minVals[3],
                                          const float maxVals[3])
{
  MagCalibration cal;

  float halfRange[3];
  float sumHalfRange = 0.f;
  for (int i = 0; i < 3; ++i)
  {
    cal.offset[i] = 0.5f * (maxVals[i] + minVals[i]);
    halfRange[i] = 0.5f * (maxVals[i] - minVals[i]);
    sumHalfRange += halfRange[i];
  }

  const float avgHalfRange = sumHalfRange / 3.f;
  for (int i = 0; i < 3; ++i)
    cal.scale[i] = (halfRange[i] > 1e-9f) ? (avgHalfRange / halfRange[i]) : 1.f;

  return cal;
}

bool MagCalibration::Save(const std::string& path) const
{
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f)
    return false;

  std::fprintf(f, "# magnetometer calibration\n"
                  "# offset_x offset_y offset_z scale_x scale_y scale_z\n");
  std::fprintf(f, "%g %g %g %g %g %g\n",
               offset[0], offset[1], offset[2], scale[0], scale[1], scale[2]);
  std::fclose(f);
  return true;
}

bool MagCalibration::Load(const std::string& path)
{
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f)
    return false;

  char line[256];
  while (std::fgets(line, sizeof(line), f))
  {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
      continue;
    const int n = std::sscanf(line, "%f %f %f %f %f %f",
                              &offset[0], &offset[1], &offset[2],
                              &scale[0], &scale[1], &scale[2]);
    std::fclose(f);
    return n == 6;
  }
  std::fclose(f);
  return false;
}
