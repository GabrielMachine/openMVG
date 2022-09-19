//:\file
//\author Ricardo Fabbri, Brown & Rio de Janeiro State U. (rfabbri.github.io) 
//\date Tue Jun  1 09:04:21 -03 2021
//\author Gabriel ANDRADE Rio de Janeiro State U.
//\author Pierre MOULON
//
#include "openMVG/multiview/trifocal/three_view_kernel.hpp"
#include "openMVG/multiview/trifocal/solver_trifocal_metrics.hpp"
#include "openMVG/multiview/trifocal/solver_trifocal_three_point.hpp"
#include "trifocal_app.hpp"
#include <minus/chicago-default.h>

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"
#include "third_party/vectorGraphics/svgDrawer.hpp"

#include "openMVG/robust_estimation/robust_estimator_MaxConsensus.hpp"
#include "openMVG/robust_estimation/score_evaluator.hpp"
#include "software/SfM/SfMPlyHelper.hpp"
#include "openMVG/multiview/triangulation_nview.hpp"

static void
invert_intrinsics(
    const double K[/*3 or 2 ignoring last line*/][3], 
    const double px_coords[2], 
    double normalized_coords[2])
{
  const double *px = px_coords;
  double *nrm = normalized_coords;
  nrm[1] = (px[1] - K[1][2]) /K[1][1];
  nrm[0] = (px[0] - K[0][1]*nrm[1] - K[0][2])/K[0][0];
}

static void
invert_intrinsics_tgt(
    const double K[/*3 or 2 ignoring last line*/][3], 
    const double px_tgt_coords[2], 
    double normalized_tgt_coords[2])
{
  const double *tp = px_tgt_coords;
  double *t = normalized_tgt_coords;
  t[1] = tp[1]/K[1][1];
  t[0] = (tp[0] - K[0][1]*t[1])/K[0][0];
}
namespace trifocal3pt {
  
using namespace std;
using namespace openMVG;
using namespace openMVG::image;
using SIFT_Regions = openMVG::features::SIFT_Regions;
using namespace openMVG::robust;
using namespace openMVG::features;
using namespace openMVG::trifocal;


void TrifocalSampleApp::
ProcessCmdLine(int argc, char **argv)
{
  CmdLine cmd;
  cmd.add( make_option('a', image_filenames_[0], "image_a") );
  cmd.add( make_option('b', image_filenames_[1], "image_b") );
  cmd.add( make_option('c', image_filenames_[2], "image_c") );
  cmd.add( make_option('K', intrinsics_filename_, "K matrix") );
  //cmd.add( make_option('K', K_, "K matrix") );
  
  try{ 
    if (argc == 1) throw string("Invalid command line parameter.");
    cerr << "Vazio? " << image_filenames_.empty() << endl;
    
    cerr << "Tamanho: " << image_filenames_.size() << endl;
    cmd.process(argc, argv);
    cerr << "Image loaded:" << image_filenames_[0] << endl;
  } catch (const string& s) {
    cerr << "Usage: " << argv[0] << '\n' << endl;
    cerr << s << endl;
    exit(EXIT_FAILURE);
  }
}

void TrifocalSampleApp::
ExtractKeypoints() 
{
  // Call Keypoint extractor
  using namespace openMVG::features;
  unique_ptr<Image_describer> image_describer;
  image_describer.reset(new SIFT_Anatomy_Image_describer(SIFT_Anatomy_Image_describer::Params()));
 
  if (!image_describer) {
    cerr << "Invalid Image_describer type" << endl;
    exit(EXIT_FAILURE);
  }
  for (const int image_idx : {0,1,2})
  {
    if (ReadImage(image_filenames_[image_idx].c_str(), &images_[image_idx]))
      image_describer->Describe(images_[image_idx], regions_per_image_[image_idx]);
    else {
      cerr << "Problem reading image" << endl;
      exit(EXIT_FAILURE);
    }
  }
  
}

void TrifocalSampleApp::
MatchKeypoints() 
{
  //--
  // Compute corresponding points {{0,1}, {1,2}}
  //--
  //-- Perform matching -> find Nearest neighbor, filtered with Distance ratio
  //unique_ptr<Matcher> collectionMatcher(new Cascade_Hashing_Matcher_Regions(fDistRatio));
  //collectionMatcher->Match(regions_provider, {{0,1}, {1,2}}, pairwise_matches_, &progress);
  matching::DistanceRatioMatch(
    0.8, matching::BRUTE_FORCE_L2,
    * regions_per_image_.at(0).get(),
    * regions_per_image_.at(1).get(),
    pairwise_matches_[{0,1}]);
  matching::DistanceRatioMatch(
    0.8, matching::BRUTE_FORCE_L2,
    * regions_per_image_.at(1).get(),
    * regions_per_image_.at(2).get(),
    pairwise_matches_[{1,2}]);
}

void TrifocalSampleApp::
ComputeTracks() 
{
  // see bool SequentialSfMReconstructionEngine::InitLandmarkTracks()
  openMVG::tracks::TracksBuilder track_builder;
  track_builder.Build(pairwise_matches_);
  track_builder.Filter(3);
  track_builder.ExportToSTL(tracks_);
}

void TrifocalSampleApp::
Stats() 
{
  // Display some statistics
  cout
    <<  regions_per_image_.at(0)->RegionCount() << " #Features on image A" << endl
    <<  regions_per_image_.at(1)->RegionCount() << " #Features on image B" << endl
    <<  regions_per_image_.at(2)->RegionCount() << " #Features on image C" << endl
    << pairwise_matches_.at({0,1}).size() << " #matches with Distance Ratio filter" << endl
    << pairwise_matches_.at({1,2}).size() << " #matches with Distance Ratio filter" << endl
    << tracks_.size() << " #tracks" << endl;
}

void TrifocalSampleApp::
ExtractXYOrientation() 
{
  sio_regions_ = array<const SIFT_Regions*, 3> ({
    dynamic_cast<SIFT_Regions*>(regions_per_image_.at(0).get()),
    dynamic_cast<SIFT_Regions*>(regions_per_image_.at(1).get()),
    dynamic_cast<SIFT_Regions*>(regions_per_image_.at(2).get())
  });
  //
  // Build datum_ (corresponding {x,y,orientation})
  //
  datum_[0].resize(4, tracks_.size());
  datum_[1].resize(4, tracks_.size());
  datum_[2].resize(4, tracks_.size());
  int idx = 0;
  for (const auto &track_it: tracks_) {
    auto iter = track_it.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;

    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];
    datum_[0].col(idx) << 
      feature_i.x(), feature_i.y(), cos(feature_i.orientation()), sin(feature_i.orientation());
    datum_[1].col(idx) << 
      feature_j.x(), feature_j.y(), cos(feature_j.orientation()), sin(feature_j.orientation());
    datum_[2].col(idx) << 
      feature_k.x(), feature_k.y(), cos(feature_k.orientation()), sin(feature_k.orientation());
    pxdatum_ = datum_;
    
    for (unsigned v=0; v < 3; ++v) {
      invert_intrinsics(K_, datum_[v].col(idx).data(), datum_[v].col(idx).data()); 
      invert_intrinsics_tgt(K_, datum_[v].col(idx).data()+2, datum_[v].col(idx).data()+2);
    }
    ++idx;
  }
}

void TrifocalSampleApp::
Display() 
{
  //
  // Display demo
  //
  const int svg_w = images_[0].Width();
  const int svg_h = images_[0].Height() + images_[1].Height() + images_[2].Height();
  svg::svgDrawer svg_stream(svg_w, svg_h);

  // Draw image side by side
  svg_stream.drawImage(image_filenames_[0], images_[0].Width(), images_[0].Height());
  svg_stream.drawImage(image_filenames_[1], images_[1].Width(), images_[1].Height(), 0, images_[0].Height());
  svg_stream.drawImage(image_filenames_[2], images_[2].Width(), images_[2].Height(), 0, images_[0].Height() + images_[1].Height());

  unsigned track_id=0;
  for (const auto &track_it: tracks_) {
  //TODO: find examples of features: point in curve(3), edge(33) 
    auto iter = track_it.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];

    svg_stream.drawCircle(
      feature_i.x(), feature_i.y(), feature_i.scale(),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawCircle(
      feature_j.x(), feature_j.y() + images_[0].Height(), feature_j.scale(),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawCircle(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_k.scale(),
      svg::svgStyle().stroke("yellow", 1));
    //TODO: Tangent line segments in yellow and if inlier -> in green
    svg_stream.drawText(
      feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_id));
   
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
      svg::svgStyle().stroke("yellow", 1)); 
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawLine(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()), //it seems that this last tangent is wrong!!
      svg::svgStyle().stroke("yellow", 1));

    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      svg::svgStyle().stroke("blue", 1));
    track_id++;
  }
  ofstream svg_file( "trifocal_track_demo.svg" );
  if (svg_file.is_open()) {
    svg_file << svg_stream.closeSvgFile().str();
  }
}

bool TrifocalSampleApp::
FilterIds(int desired_ids[], int n_ids)
{
  // Checks if my given IDs are OK
  bool found = false;

  for (unsigned i = 0; i < n_ids; ++i) {
    found = false;
    unsigned track_id = 0;
    for (const auto &track_it: tracks_) {
      if (track_id == desired_ids[i]) {
        found = true;
      }
      track_id++;
    }
    if(!found){
      return false;
    }
  }
  return found;
}

void TrifocalSampleApp::
SeparateIds( unsigned desired_ids[], unsigned non_desired_ids[], unsigned n_ids )
{
  //
  // Separate my desired ids and non desired ids from the total
  //
  unsigned track_id = 0;
  bool found = false;
  unsigned j = 0;
  for (const auto &track_it: tracks_)
  {
    found = false;
    for (unsigned i = 0; i < n_ids; ++i) {
      if (track_id == desired_ids[i])
        found = true;
    }
    if (!found) {
      non_desired_ids[j] = track_id;
      track_id++;
      j++;
      continue;
    }
  }
}

void TrifocalSampleApp::
DisplayDesiredIds() 
{
  //
  // Display desired ids
  //
  const int svg_w = images_[0].Width();
  const int svg_h = images_[0].Height() + images_[1].Height() + images_[2].Height();
  svg::svgDrawer svg_stream(svg_w, svg_h);

  // Draw image side by side
  svg_stream.drawImage(image_filenames_[0], images_[0].Width(), images_[0].Height());
  svg_stream.drawImage(image_filenames_[1], images_[1].Width(), images_[1].Height(), 0, images_[0].Height());
  svg_stream.drawImage(image_filenames_[2], images_[2].Width(), images_[2].Height(), 0, images_[0].Height() + images_[1].Height());

  constexpr unsigned n_ids = 5;
  unsigned desired_ids[n_ids] = {13, 23, 33, 63, 53};
  unsigned track_id = 0;
  for (const auto &track_it: tracks_)
  {
    bool found=false;
    for (unsigned i = 0; i < n_ids; ++i)
      if (track_id == desired_ids[i])
        found = true;
        
    if (!found) {
      track_id++;
      continue;
    }
  //TODO: find examples of features: point in curve(3), edge(33) 
    auto iter = track_it.second.cbegin();
    
 uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];

    svg_stream.drawCircle(
      feature_i.x(), feature_i.y(), feature_i.scale(),
      svg::svgStyle().stroke("navy", 1));
    svg_stream.drawCircle(
      feature_j.x(), feature_j.y() + images_[0].Height(), feature_k.scale(),
      svg::svgStyle().stroke("navy", 1));
    svg_stream.drawCircle(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_j.scale(),
      svg::svgStyle().stroke("navy", 1));
    //TODO: Tangent line segments in yellow and if inlier -> in green
    svg_stream.drawText(
      feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_id));
   
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
      svg::svgStyle().stroke("yellow", 1)); 
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawLine(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()), //it seems that this last tangent is wrong!!
      svg::svgStyle().stroke("yellow", 1));

    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      svg::svgStyle().stroke("blue", 1));
    track_id++;
  }
  ofstream svg_file( "trifocal_track_desired_ids.svg" );
  if (svg_file.is_open())
  {
    svg_file << svg_stream.closeSvgFile().str();
  }
}

void TrifocalSampleApp::
DisplayNonDesiredIds() 
{
  //
  // Display rest of ids
  //
  const int svg_w = images_[0].Width();
  const int svg_h = images_[0].Height() + images_[1].Height() + images_[2].Height();
  svg::svgDrawer svg_stream(svg_w, svg_h);

  // Draw image side by side
  svg_stream.drawImage(image_filenames_[0], images_[0].Width(), images_[0].Height());
  svg_stream.drawImage(image_filenames_[1], images_[1].Width(), images_[1].Height(), 0, images_[0].Height());
  svg_stream.drawImage(image_filenames_[2], images_[2].Width(), images_[2].Height(), 0, images_[0].Height() + images_[1].Height());

  constexpr unsigned n_ids = 5;
  unsigned desired_ids[n_ids] = {13, 23, 33, 63, 53};
  unsigned non_desired_ids[tracks_.size()-n_ids];
  SeparateIds(desired_ids, non_desired_ids, n_ids);
  unsigned track_id=0;
  for (const auto &track_it: tracks_)
  {
    bool found = false;
    for (unsigned i=0; i < tracks_.size()-n_ids; ++i)
      if (track_id == non_desired_ids[i])
        found = true;
        
    if (!found) {
      track_id++;
      continue;
    }
  //TODO: find examples of features: point in curve(3), edge(33) 
    auto iter = track_it.second.cbegin();
    
 uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];

    svg_stream.drawCircle(
      feature_i.x(), feature_i.y(), feature_i.scale(),
      svg::svgStyle().stroke("red", 1));
    svg_stream.drawCircle(
      feature_j.x(), feature_j.y() + images_[0].Height(), feature_k.scale(),
      svg::svgStyle().stroke("red", 1));
    svg_stream.drawCircle(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_j.scale(),
      svg::svgStyle().stroke("red", 1));
    //TODO: Tangent line segments in yellow and if inlier -> in green
    svg_stream.drawText(
      feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_id));
   
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
      svg::svgStyle().stroke("yellow", 1)); 
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawLine(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()), //it seems that this last tangent is wrong!!
      svg::svgStyle().stroke("yellow", 1));

    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      svg::svgStyle().stroke("blue", 1));
    track_id++;
  }
  ofstream svg_file( "trifocal_track_non_desired_ids.svg" );
  if (svg_file.is_open())
  {
    svg_file << svg_stream.closeSvgFile().str();
  }
}
// 3 files trifocal_track,trifocal_inlier,track_inlier, return the correct matrices, 
// pass to solver datum desired i,print feature sca scale
void TrifocalSampleApp::
RobustSolve() 
{
  using TrifocalKernel = ThreeViewKernel<Trifocal3PointPositionTangentialSolver,
                         NormalizedSquaredPointReprojectionOntoOneViewError>;

  const TrifocalKernel trifocal_kernel(datum_[0], datum_[1], datum_[2]);

  double threshold =
    NormalizedSquaredPointReprojectionOntoOneViewError::threshold_pixel_to_normalized(1, K_);
  threshold *= threshold; // squared error
  unsigned constexpr max_iteration = 5; // testing
  // Vector of inliers for the best fit found
  model = MaxConsensus(trifocal_kernel,
      ScorerEvaluator<TrifocalKernel>(threshold), &vec_inliers_, max_iteration);
  cerr << "vec_inliers size: "<< vec_inliers_.size() << "\n";
  cerr << "model: " << model[2] << "\n";
  // TODO(gabriel) recontruct from inliers and best models to show as PLY
}

// Displays inliers only
void TrifocalSampleApp::
  DisplayDesiredInliers() 
{
  const int svg_w = images_[0].Width();
  const int svg_h = images_[0].Height() + images_[1].Height() + images_[2].Height();
  svg::svgDrawer svg_stream(svg_w, svg_h);

  // Draw image side by side
  svg_stream.drawImage(image_filenames_[0], images_[0].Width(), images_[0].Height());
  svg_stream.drawImage(image_filenames_[1], images_[1].Width(), images_[1].Height(), 0, images_[0].Height());
  svg_stream.drawImage(image_filenames_[2], images_[2].Width(), images_[2].Height(), 0, images_[0].Height() + images_[1].Height());
  
  constexpr unsigned n_inlier_pp = 3;
  unsigned desired_inliers[n_inlier_pp] = {13, 23, 43};
  unsigned track_inlier=0;
  for (const auto &track_it: tracks_) {
    bool inlier=false;
    for (unsigned i=0; i < n_inlier_pp; ++i)
      if (track_inlier == desired_inliers[i])
        inlier = true;
        
    if (!inlier) {
      track_inlier++;
      continue;
    }
    
    auto iter = track_it.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];
    svg_stream.drawCircle(
      feature_i.x(), feature_i.y(), feature_i.scale(),
      svg::svgStyle().stroke("green", 1));
    svg_stream.drawCircle(
      feature_j.x(), feature_j.y() + images_[0].Height(), feature_j.scale(),
      svg::svgStyle().stroke("green", 1));
    svg_stream.drawCircle(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_k.scale(),
      svg::svgStyle().stroke("green", 1));
    //TODO: Tangent line segments in yellow and if inlier -> in green
    svg_stream.drawText(
      feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_inlier));
   
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
      svg::svgStyle().stroke("yellow", 1)); 
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawLine(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()),
      svg::svgStyle().stroke("yellow", 1));

    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("lightblue", 1));
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("lightblue", 1));
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      svg::svgStyle().stroke("lightblue", 1));
    track_inlier++;
  }
  ofstream svg_file( "trifocal_track_inliers.svg" );
  if (svg_file.is_open())
    svg_file << svg_stream.closeSvgFile().str();
}

// Displays inliers and and tracks
void TrifocalSampleApp::
DisplayInliersCamerasAndPoints() 
{
  // TODO We can then display the inlier and the 3D camera configuration as PLY

  const int svg_w = images_[0].Width();
  const int svg_h = images_[0].Height() + images_[1].Height() + images_[2].Height();
  svg::svgDrawer svg_stream(svg_w, svg_h);

  // Draw image side by side
  svg_stream.drawImage(image_filenames_[0], images_[0].Width(), images_[0].Height());
  svg_stream.drawImage(image_filenames_[1], images_[1].Width(), images_[1].Height(), 0, images_[0].Height());
  svg_stream.drawImage(image_filenames_[2], images_[2].Width(), images_[2].Height(), 0, images_[0].Height() + images_[1].Height());
  
  // these are the selected inliers from vec_inliers_. 
  // Its easier select the result got from robustsolve() than select in robustsolve()
  unsigned track_id=0;
  for (const auto &track_it: tracks_) {
  //TODO: find examples of features: point in curve(3), edge(33) 
    auto iter = track_it.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];

    svg_stream.drawCircle(
      feature_i.x(), feature_i.y(), feature_i.scale(),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawCircle(
    feature_j.x(), feature_j.y() + images_[0].Height(), feature_j.scale(),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawCircle(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_k.scale(),
      svg::svgStyle().stroke("yellow", 1));
    //TODO: Tangent line segments in yellow and if inlier -> in green
    svg_stream.drawText(
      feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_id));
   
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
      svg::svgStyle().stroke("yellow", 1)); 
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawLine(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()), //it seems that this last tangent is wrong!!
      svg::svgStyle().stroke("yellow", 1));

    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("blue", 1));
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      svg::svgStyle().stroke("blue", 1));
    track_id++;
  }

  unsigned track_inlier=0;
  for (const auto &track_it: tracks_) {
    bool inlier=false;
    for (unsigned i=0; i < vec_inliers_.size(); ++i)
      if (track_inlier == vec_inliers_.at(i))
        inlier = true;
        
    if (!inlier) {
      track_inlier++;
      continue;
    }
    
    auto iter = track_it.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];
    //cout<<"cyka"<<endl; 
    svg_stream.drawCircle(
      feature_i.x(), feature_i.y(), feature_i.scale(),
      svg::svgStyle().stroke("green", 1));
    svg_stream.drawCircle(
      feature_j.x(), feature_j.y() + images_[0].Height(), feature_j.scale(),
      svg::svgStyle().stroke("green", 1));
    svg_stream.drawCircle(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_k.scale(),
      svg::svgStyle().stroke("green", 1));
    //TODO: Tangent line segments in yellow and if inlier -> in green
    svg_stream.drawText(
      feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_inlier));
   
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
      svg::svgStyle().stroke("yellow", 1)); 
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
      svg::svgStyle().stroke("yellow", 1));
    svg_stream.drawLine(
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()),
      svg::svgStyle().stroke("yellow", 1));

    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("lightblue", 1));
    svg_stream.drawLine(
      feature_i.x(), feature_i.y(),
      feature_j.x(), feature_j.y() + images_[0].Height(),
      svg::svgStyle().stroke("lightblue", 1));
    svg_stream.drawLine(
      feature_j.x(), feature_j.y() + images_[0].Height(),
      feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
      svg::svgStyle().stroke("lightblue", 1));
    track_inlier++;
  }
  ofstream svg_file( "trifocal_track.svg" );
  if (svg_file.is_open()) {
    svg_file << svg_stream.closeSvgFile().str();
  }
}

void TrifocalSampleApp::
DisplayInliersCamerasAndPointsSIFT()
{
    // TODO We can then display the inlier and the 3D camera configuration as PLY
    const int svg_w = images_[0].Width();
    const int svg_h = images_[0].Height() + images_[1].Height() + images_[2].Height();
    svg::svgDrawer svg_stream(svg_w, svg_h);

    // Draw image side by side
    svg_stream.drawImage(image_filenames_[0], images_[0].Width(), images_[0].Height());
    svg_stream.drawImage(image_filenames_[1], images_[1].Width(), images_[1].Height(), 0, images_[0].Height());
    svg_stream.drawImage(image_filenames_[2], images_[2].Width(), images_[2].Height(), 0, images_[0].Height() + images_[1].Height());
    
    unsigned track_id=0;
    constexpr unsigned n_ids = 5;
    unsigned desired_ids[n_ids] = {13, 23, 33, 63, 53};
    constexpr unsigned n_inlier_pp = 3;
    unsigned desired_inliers[n_inlier_pp] = {13, 23, 43};
    unsigned track_inlier=0;
    for (const auto &track_it: tracks_) {
      bool found=false;
      bool inlier=false;
      
      auto iter = track_it.second.cbegin();
      const uint32_t
        i = iter->second,
        j = (++iter)->second,
        k = (++iter)->second;
      //
      const auto feature_i = sio_regions_[0]->Features()[i];
      const auto feature_j = sio_regions_[1]->Features()[j];
      const auto feature_k = sio_regions_[2]->Features()[k];
      for (unsigned i=0; i < n_ids; ++i)
        if (track_id == desired_ids[i]) { //this part is literaly overwriting the inliers
          found = true;
      svg_stream.drawCircle(
        feature_i.x(), feature_i.y(), 2*feature_i.scale(),
        svg::svgStyle().stroke("yellow", 1));
      svg_stream.drawCircle(
        feature_j.x(), feature_j.y() + images_[0].Height(), feature_k.scale(),
        svg::svgStyle().stroke("yellow", 1));
      svg_stream.drawCircle(
        feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_k.scale(),
        svg::svgStyle().stroke("yellow", 1));
      //TODO: Tangent line segments in yellow and if inlier -> in green
      svg_stream.drawText(
        feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_id));
     
      svg_stream.drawLine(
        feature_i.x(), feature_i.y(),
        feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
        svg::svgStyle().stroke("yellow", 1)); 
      svg_stream.drawLine(
        feature_j.x(), feature_j.y() + images_[0].Height(),
        feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
        svg::svgStyle().stroke("yellow", 1));
      svg_stream.drawLine(
        feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
        feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()), //it seems that this last tangent is wrong!!
        svg::svgStyle().stroke("yellow", 1));

      svg_stream.drawLine(
        feature_i.x(), feature_i.y(),
        feature_j.x(), feature_j.y() + images_[0].Height(),
        svg::svgStyle().stroke("blue", 1));
      svg_stream.drawLine(
        feature_i.x(), feature_i.y(),
        feature_j.x(), feature_j.y() + images_[0].Height(),
        svg::svgStyle().stroke("blue", 1));
      svg_stream.drawLine(
        feature_j.x(), feature_j.y() + images_[0].Height(),
        feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
        svg::svgStyle().stroke("blue", 1));
      track_id++;
          }
      if (!found) {
        track_id++;
        continue;
      }
     track_id++;
    }
    for (const auto &track_it: tracks_) {
      bool inlier=false;
      
      auto iter = track_it.second.cbegin();
      const uint32_t
        i = iter->second,
        j = (++iter)->second,
        k = (++iter)->second;
      //
      const auto feature_i = sio_regions_[0]->Features()[i];
      const auto feature_j = sio_regions_[1]->Features()[j];
      const auto feature_k = sio_regions_[2]->Features()[k];
      for (unsigned i=0; i < n_inlier_pp; ++i)
        if (track_inlier == desired_inliers[i]){
         svg_stream.drawCircle(
            feature_i.x(), feature_i.y(), feature_i.scale(),
            svg::svgStyle().stroke("green", 1));
          svg_stream.drawCircle(
            feature_j.x(), feature_j.y() + images_[0].Height(), feature_j.scale(),
            svg::svgStyle().stroke("green", 1));
          svg_stream.drawCircle(
            feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(), feature_k.scale(),
            svg::svgStyle().stroke("green", 1));
          //TODO: Tangent line segments in yellow and if inlier -> in green
          svg_stream.drawText(
            feature_i.x()+20, feature_i.y()-20, 6.0f, std::to_string(track_inlier));
     
      svg_stream.drawLine(
        feature_i.x(), feature_i.y(),
        feature_i.x()+20*cos(feature_i.orientation()), feature_i.y() + 20*sin(feature_i.orientation()) ,
        svg::svgStyle().stroke("yellow", 1)); 
      svg_stream.drawLine(
        feature_j.x(), feature_j.y() + images_[0].Height(),
        feature_j.x()+20*cos(feature_j.orientation()), feature_j.y() + images_[0].Height()+ 20*sin(feature_j.orientation()),
        svg::svgStyle().stroke("yellow", 1));
      svg_stream.drawLine(
        feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
        feature_k.x()+ 20*sin(feature_k.orientation()), feature_k.y() + images_[0].Height() + images_[1].Height()+ 20*sin(feature_k.orientation()),
        svg::svgStyle().stroke("yellow", 1));

      svg_stream.drawLine(
        feature_i.x(), feature_i.y(),
        feature_j.x(), feature_j.y() + images_[0].Height(),
        svg::svgStyle().stroke("lightblue", 1));
      svg_stream.drawLine(
        feature_i.x(), feature_i.y(),
        feature_j.x(), feature_j.y() + images_[0].Height(),
        svg::svgStyle().stroke("lightblue", 1));
      svg_stream.drawLine(
        feature_j.x(), feature_j.y() + images_[0].Height(),
        feature_k.x(), feature_k.y() + images_[0].Height() + images_[1].Height(),
        svg::svgStyle().stroke("lightblue", 1));
          inlier = true;
        }  
      if (!inlier) {
        track_inlier++;
        continue;
      }
     track_inlier++;
    }
    ofstream svg_file( "trifocal_track_SIFT.svg" );
    if (svg_file.is_open()) {
      svg_file << svg_stream.closeSvgFile().str();
    }
}
void TrifocalSampleApp::
ExportBaseReconstructiontoPLY(){

  std::vector<Mat3X> inlier_coordinates_; 
  unsigned track_inlier=0;
  for (const auto &track_it: tracks_) {
    bool inlier=false;
    for (unsigned i=0; i < vec_inliers_.size(); ++i)
      if (track_inlier == vec_inliers_.at(i))
        inlier = true;
        
    if (!inlier) {
      track_inlier++;
      continue;
    }
    
    auto iter = track_it.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second,
      k = (++iter)->second;
    //
    const auto feature_i = sio_regions_[0]->Features()[i];
    const auto feature_j = sio_regions_[1]->Features()[j];
    const auto feature_k = sio_regions_[2]->Features()[k];
     
    Mat3 bearing;
    bearing[0] = feature_i;
    bearing[1] = feature_j; 
    bearing[2] = feature_k;
    inlier_coordinates_.push_back(bearing);
      track_inlier++;
  }
  Vec4 triangulated_ply;
  TriangulateNView(inlier_coordinates_, model, &triangulated_ply);
  cerr << triangulated_ply << "\n";
}
} // namespace trifocal3pt

