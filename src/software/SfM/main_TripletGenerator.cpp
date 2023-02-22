// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.
//
//:\file
//\author Ricardo Fabbri Rio de Janeiro State U. (rfabbri.github.io) 
//\author Pierre MOULON
//\author Gabriel ANDRADE Rio de Janeiro State U.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/matching_image_collection/Triplet_Builder.hpp"
#include "openMVG/sfm/sfm_data.hpp"
#include "openMVG/sfm/sfm_data_io.hpp"

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"

#include <iostream>

/**
 * @brief Current list of available triplet mode
 *
 */
enum ETripletMode
{
  TRIPLET_EXHAUSTIVE = 0, // Build every combination of image triplets
  TRIPLET_CONTIGUOUS = 1  // Only consecutive image triplets (useful for video mode)
};

using namespace openMVG;
using namespace openMVG::sfm;

void usage( const char* argv0 )
{
  std::cerr << "Usage: " << argv0 << '\n'
            << "[-i|--input_file]         A SfM_Data file\n"
            << "[-o|--output_file]        Output file where triplets are stored\n"
            << "\n[Optional]\n"
            << "[-m|--triplet_mode] mode     Triplet generation mode\n"
            << "       EXHAUSTIVE:        Build all possible triplets. [default]\n"
            << "       CONTIGUOUS:        Build triplets for contiguous images (use it with --contiguous_count parameter)\n"
            << "[-c|--contiguous_count] X Number of contiguous links\n"
            << "       X: will match 0 with (1->X), ...]\n"
            << "       2: will match 0 with (1,2), 1 with (2,3), ...\n"
            << "       3: will match 0 with (1,2,3), 1 with (2,3,4), ...\n"
            << std::endl;
}

// This executable computes triplets of images to be matched
int main( int argc, char** argv )
{
  CmdLine cmd;

  std::string sSfMDataFilename;
  std::string sOutputTripletsFilename;
  std::string sTripletMode        = "EXHAUSTIVE";
  int         iContiguousCount = -1;

  // Mandatory elements:
  cmd.add( make_option( 'i', sSfMDataFilename, "input_file" ) );
  cmd.add( make_option( 'o', sOutputTripletsFilename, "output_file" ) );
  // Optional elements:
  cmd.add( make_option( 'm', sTripletMode, "triplet_mode" ) );
  cmd.add( make_option( 'c', iContiguousCount, "contiguous_count" ) );

  try
  {
    if ( argc == 1 )
      throw std::string( "Invalid command line parameter." );
    cmd.process( argc, argv );
  }
  catch ( const std::string& s )
  {
    usage( argv[ 0 ] );
    std::cerr << "[Error] " << s << std::endl;

    return EXIT_FAILURE;
  }

  // 0. Parse parameters
  std::cout << " You called:\n"
            << argv[ 0 ] << "\n"
            << "--input_file       : " << sSfMDataFilename << "\n"
            << "--output_file      : " << sOutputTripletsFilename << "\n"
            << "Optional parameters\n"
            << "--triplet_mode        : " << sTripletMode << "\n"
            << "--contiguous_count : " << iContiguousCount << "\n"
            << std::endl;

  if ( sSfMDataFilename.empty() )
  {
    usage( argv[ 0 ] );
    std::cerr << "[Error] Input file not set." << std::endl;
    exit( EXIT_FAILURE );
  }
  if ( sOutputTripletsFilename.empty() )
  {
    usage( argv[ 0 ] );
    std::cerr << "[Error] Output file not set." << std::endl;
    exit( EXIT_FAILURE );
  }

  ETripletMode tripletMode;
  if ( sTripletMode == "EXHAUSTIVE" )
  {
    tripletMode = TRIPLET_EXHAUSTIVE;
  }
  else if ( sTripletMode == "CONTIGUOUS" )
  {
    if ( iContiguousCount == -1 )
    {
      usage( argv[ 0 ] );
      std::cerr << "[Error] Contiguous pair mode selected but contiguous_count not set." << std::endl;
      exit( EXIT_FAILURE );
    }

    tripletMode = TRIPLET_CONTIGUOUS;
  }

  // 1. Load SfM data scene
  std::cout << "Loading scene.";
  SfM_Data sfm_data;
  if ( !Load( sfm_data, sSfMDataFilename, ESfM_Data( VIEWS | INTRINSICS ) ) )
  {
    std::cerr << std::endl
              << "The input SfM_Data file \"" << sSfMDataFilename << "\" cannot be read." << std::endl;
    exit( EXIT_FAILURE );
  }
  const size_t NImage = sfm_data.GetViews().size();

  // 2. Compute triplets
  std::cout << "Computing triplets." << std::endl;
  Triplet_Set triplets;
  switch ( tripletMode )
  {
    case TRIPLET_EXHAUSTIVE:
    {
      triplets = exhaustiveTriplets( NImage );
      break;
    }
    case TRIPLET_CONTIGUOUS:
    {
      triplets = contiguousWithOverlap( NImage, iContiguousCount );
      break;
    }
    default:
    {
      std::cerr << "Unknown triplet mode" << std::endl;
      exit( EXIT_FAILURE );
    }
  }

  // 3. Save triplets
  std::cout << "Saving triplets." << std::endl;
  if ( !saveTriplets( sOutputTripletsFilename, triplets ) ){
  
    std::cerr << "Failed to save triplets to file: \"" << sOutputTripletsFilename << "\"" << std::endl;
    exit( EXIT_FAILURE );
  }

  return EXIT_SUCCESS;
}
