// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.
//
//:\file
//\author Ricardo Fabbri Rio de Janeiro State U. (rfabbri.github.io) 
//\author Pierre MOULON
//\author Gabriel ANDRADE Rio de Janeiro State U.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef OPENMVG_MATCHING_IMAGE_COLLECTION_TRIPLET_BUILDER_HPP
#define OPENMVG_MATCHING_IMAGE_COLLECTION_TRIPLET_BUILDER_HPP

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "openMVG/types.hpp"
#include "openMVG/stl/split.hpp"
#include "openMVG/system/logger.hpp"

namespace openMVG {

/// Generate all the (I,J,K) triplets of the upper diagonal of the NxN matrix
inline Triplet_Set exhaustiveTriplets(const size_t N)
{
  Triplet_Set triplets;
  for (IndexT I = 0; I < static_cast<IndexT>(N); ++I)
    for (IndexT J = I+1; J < static_cast<IndexT>(N); ++J)
      for (IndexT K = J+1; K < static_cast<IndexT>(N); ++K)
        triplets.insert({I,J,K});

  return triplets;
}

/// Generate the triplets that have a distance inferior to the overlapSize
/// Usable to match video sequence
inline Triplet_Set contiguousWithOverlap(const size_t N, const size_t overlapSize)
{
  Triplet_Set triplets;
  for (IndexT I = 0; I < static_cast<IndexT>(N); ++I)
    for (IndexT J = I+1; J < I+1+overlapSize && J < static_cast<IndexT>(N); ++J)
      for (IndexT K = J+1; J < J+1+overlapSize && K < static_cast<IndexT>(N); ++K)
        triplets.insert({I,J,K});
  return triplets;
}

/// Load a set of Triplet_Set from a file
/// I J K L (triplet that link I)
inline bool loadTriplets(
     const size_t N,  // number of image in the current project (to check index validity)
     const std::string &sFileName, // filename of the list file,
     Triplet_Set & triplets)  // output triplet read from the list file
{
  std::ifstream in(sFileName);
  if (!in)
  {
    OPENMVG_LOG_ERROR
      << "loadTriplets: Impossible to read the specified file: \"" << sFileName << "\".";
    return false;
  }
  std::string sValue;
  std::vector<std::string> vec_str;
  while (std::getline( in, sValue ) )
  {
    vec_str.clear();
    stl::split(sValue, ' ', vec_str);
    const IndexT str_size (vec_str.size());
    if (str_size < 2)
    {
      OPENMVG_LOG_ERROR << "loadTriplets: Invalid input file: \"" << sFileName << "\".";
    
      return false;
    }
    std::stringstream oss;
    oss.clear(); oss.str(vec_str[0]);
    IndexT I, J, K;
    oss >> I;
    for (IndexT i=1; i<str_size; ++i)
    {
      oss.clear(); oss.str(vec_str[i]);
      oss >> J >> K;
      if ( I > N-1 || J > N-1 || K > N-1) //I&J&K always > 0 since we use unsigned type
      {
        OPENMVG_LOG_ERROR
          << "loadTriplets: Invalid input file. Image out of range. "
          << "I: " << I << " J:" << J << "K:" << K << " N:" << N << "\n"
          << "File: \"" << sFileName << "\".";
        return false;
      }
      if ( I == J || I == K )
      {
        OPENMVG_LOG_ERROR << "loadTriplets: Invalid input file. Image " << I << " see itself. File: \"" << sFileName << "\".";
        return false;
      }
      if ( J == K )
      {
        OPENMVG_LOG_ERROR << "loadTriplets: Invalid input file. Image " << J << " see itself. File: \"" << sFileName << "\".";
        return false;
      }
      // Insert the triplet such that max, middle, min
      if(std::min({I, J, K}) == I && std::max({I, J, K}) != I)
        triplets.insert( {std::min({I, J, K}), I, std::max({I, J, K})} );
      if(std::min({I, J, K}) == J && std::max({I, J, K}) != J)
        triplets.insert( {std::min({I, J, K}), K, std::max({I, J, K})} );
      else
        triplets.insert( {std::min({I, J, K}), K, std::max({I, J, K})} );
    }
  }
  in.close();
  return true;
}

/// Save a set of Triplet_Set to a file (one triplet per line)
/// I J K
/// I K L
/// ...
inline bool saveTriplets(const std::string &sFileName, const Triplet_Set & triplets)
{
  std::ofstream outStream(sFileName);
  if (!outStream)  {
    OPENMVG_LOG_ERROR
      << "saveTriplets: Impossible to open the output specified file: \"" << sFileName << "\".";
    return false;
  }
  for ( const auto & cur_triplets : triplets )
  {
    outStream << std::get<0>(cur_triplets) << ' ' << std::get<1>(cur_triplets) << ' ' << std::get<2>(cur_triplets) <<'\n';
  }
  
  const bool bOk = !outStream.bad();
  outStream.close();
  return bOk;
}

} // namespace openMVG

#endif // OPENMVG_MATCHING_IMAGE_COLLECTION_TRIPLET_BUILDER_HPP
