// file included in sequential_SfM, for dev

SequentialSfMReconstructionEngine::SequentialSfMReconstructionEngine(
  const SfM_Data & sfm_data,
  const std::string & soutDirectory,
  const std::string & sloggingFile)
  : ReconstructionEngine(sfm_data, soutDirectory),
    sLogging_file_(sloggingFile),
    initial_pair_(0,0),
    initial_triplet_(0,0,0),
    cam_type_(EINTRINSIC(PINHOLE_CAMERA_RADIAL3))
{
  if (!sLogging_file_.empty())
  {
    // setup HTML logger
    html_doc_stream_ = std::make_shared<htmlDocument::htmlDocumentStream>("SequentialReconstructionEngine SFM report.");
    html_doc_stream_->pushInfo(
      htmlDocument::htmlMarkup("h1", std::string("SequentialSfMReconstructionEngine")));
    html_doc_stream_->pushInfo("<hr>");

    html_doc_stream_->pushInfo( "Dataset info:");
    html_doc_stream_->pushInfo( "Views count: " +
      htmlDocument::toString( sfm_data.GetViews().size()) + "<br>");
  }
  // Init remaining image list
  for (Views::const_iterator itV = sfm_data.GetViews().begin();
    itV != sfm_data.GetViews().end(); ++itV)
  {
    set_remaining_view_id_.insert(itV->second->id_view);
  }
}

SequentialSfMReconstructionEngine::~SequentialSfMReconstructionEngine()
{
  if (!sLogging_file_.empty())
  {
    // Save the reconstruction Log
    std::ofstream htmlFileStream(sLogging_file_);
    htmlFileStream << html_doc_stream_->getDoc();
  }
}

void SequentialSfMReconstructionEngine::SetFeaturesProvider(Features_Provider * provider)
{
  features_provider_ = provider;
}

void SequentialSfMReconstructionEngine::SetMatchesProvider(Matches_Provider * provider)
{
  matches_provider_ = provider;
}

// TODO(trifocal future) GetTripletWithMostMatches
// Get the PairWiseMatches that have the most support point
std::vector<openMVG::matching::PairWiseMatches::const_iterator>
GetPairWithMostMatches(const SfM_Data& sfm_data, const PairWiseMatches& matches, int clamp_count = 10) {

  std::vector<openMVG::matching::PairWiseMatches::const_iterator> sorted_pairwise_matches_iterators;
  // List Views that supports valid intrinsic
  std::set<IndexT> valid_views;
  for (const auto & view : sfm_data.GetViews())
  {
    const View * v = view.second.get();
    if (sfm_data.GetIntrinsics().find(v->id_intrinsic) != sfm_data.GetIntrinsics().end())
      valid_views.insert(v->id_view);
  }

  if (sfm_data.GetIntrinsics().empty() || valid_views.empty())
  {
    OPENMVG_LOG_ERROR
      << "Unable to choose an initial pair, since there is no defined intrinsic data.";
    return {};
  }

  // Try to list the clamp_count top pairs that have valid intrinsics
  std::vector<uint32_t > vec_NbMatchesPerPair;
  std::vector<openMVG::matching::PairWiseMatches::const_iterator> vec_MatchesIterator;
  const openMVG::matching::PairWiseMatches & map_Matches = matches;
  for (openMVG::matching::PairWiseMatches::const_iterator
    iter = map_Matches.begin();
    iter != map_Matches.end(); ++iter)
  {
    const Pair current_pair = iter->first;
    if (valid_views.count(current_pair.first) &&
      valid_views.count(current_pair.second) )
    {
      vec_NbMatchesPerPair.push_back(iter->second.size());
      vec_MatchesIterator.push_back(iter);
    }
  }
  // sort the Pairs in descending order according their correspondences count
  using namespace stl::indexed_sort;
  std::vector<sort_index_packet_descend<uint32_t, uint32_t>> packet_vec(vec_NbMatchesPerPair.size());
  sort_index_helper(packet_vec, &vec_NbMatchesPerPair[0], std::min((size_t)clamp_count, vec_NbMatchesPerPair.size()));

  for (size_t i = 0; i < std::min((size_t)clamp_count, vec_NbMatchesPerPair.size()); ++i) {
    const uint32_t index = packet_vec[i].index;
    sorted_pairwise_matches_iterators.emplace_back(vec_MatchesIterator[index]);
  }
  return sorted_pairwise_matches_iterators;
}


bool SequentialSfMReconstructionEngine::InitLandmarkTracks() 
{
  // Compute tracks from matches
  tracks::TracksBuilder tracksBuilder;

  {
    // List of features matches for each pair of images
    const openMVG::matching::PairWiseMatches & map_Matches = matches_provider_->pairWise_matches_;
    OPENMVG_LOG_INFO << "Track building";

    tracksBuilder.Build(map_Matches);
    OPENMVG_LOG_INFO << "Track filtering";
    tracksBuilder.Filter();
    OPENMVG_LOG_INFO << "Track export to internal struct";
    //-- Build tracks with STL compliant type :
    tracksBuilder.ExportToSTL(map_tracks_);

    {
      std::ostringstream osTrack;
      //-- Display stats :
      //    - number of images
      //    - number of tracks
      std::set<uint32_t> set_imagesId;
      tracks::TracksUtilsMap::ImageIdInTracks(map_tracks_, set_imagesId);
      osTrack << "\n------------------\n"
        << "-- Tracks Stats --" << "\n"
        << " Number of tracks: " << tracksBuilder.NbTracks() << "\n"
        << " Images Id: " << "\n";
      std::copy(set_imagesId.begin(),
        set_imagesId.end(),
        std::ostream_iterator<uint32_t>(osTrack, ", "));
      osTrack << "\n------------------\n";

      std::map<uint32_t, uint32_t> map_Occurrence_TrackLength;
      tracks::TracksUtilsMap::TracksLength(map_tracks_, map_Occurrence_TrackLength);
      osTrack << "TrackLength, Occurrence" << "\n";
      for (const auto & it : map_Occurrence_TrackLength)  {
        osTrack << "\t" << it.first << "\t" << it.second << "\n";
      }
      OPENMVG_LOG_INFO << osTrack.str();
    }
  }
  // Initialize the shared track visibility helper
  shared_track_visibility_helper_.reset(new openMVG::tracks::SharedTrackVisibilityHelper(map_tracks_));
  return map_tracks_.size() > 0;
}

bool SequentialSfMReconstructionEngine::AutomaticInitialPairChoice(Pair & initial_pair) const
{
  // select a pair that have the largest baseline (mean angle between its bearing vectors).

  const unsigned iMin_inliers_count = 100;
  const float fRequired_min_angle = 3.0f;
  const float fLimit_max_angle = 60.0f; // More than 60 degree, we cannot rely on matches for initial pair seeding

  // List Views that support valid intrinsic (view that could be used for Essential matrix computation)
  std::set<IndexT> valid_views;
  for (Views::const_iterator it = sfm_data_.GetViews().begin();
    it != sfm_data_.GetViews().end(); ++it)
  {
    const View * v = it->second.get();
    if (sfm_data_.GetIntrinsics().count(v->id_intrinsic))
      valid_views.insert(v->id_view);
  }

  if (valid_views.size() < 2)
  {
    return false; // There is not view that support valid intrinsic data
  }

  std::vector<std::pair<double, Pair>> scoring_per_pair;

  // Compute the relative pose & the 'baseline score'
  system::LoggerProgress my_progress_bar( matches_provider_->pairWise_matches_.size(),
    "Selection of an initial pair");
#ifdef OPENMVG_USE_OPENMP
  #pragma omp parallel
#endif
  for (const std::pair<Pair, IndMatches> & match_pair : matches_provider_->pairWise_matches_)
  {
#ifdef OPENMVG_USE_OPENMP
  #pragma omp single nowait
#endif
    {
      ++my_progress_bar;

      const Pair current_pair = match_pair.first;

      const uint32_t I = std::min(current_pair.first, current_pair.second);
      const uint32_t J = std::max(current_pair.first, current_pair.second);
      if (valid_views.count(I) && valid_views.count(J))
      {
        const View
          * view_I = sfm_data_.GetViews().at(I).get(),
          * view_J = sfm_data_.GetViews().at(J).get();
        const Intrinsics::const_iterator
          iterIntrinsic_I = sfm_data_.GetIntrinsics().find(view_I->id_intrinsic),
          iterIntrinsic_J = sfm_data_.GetIntrinsics().find(view_J->id_intrinsic);

        const auto
          cam_I = iterIntrinsic_I->second.get(),
          cam_J = iterIntrinsic_J->second.get();
        if (cam_I && cam_J)
        {
          openMVG::tracks::STLMAPTracks map_tracksCommon;
          shared_track_visibility_helper_->GetTracksInImages({I, J}, map_tracksCommon);

          // Copy points correspondences to arrays for relative pose estimation
          const size_t n = map_tracksCommon.size();
          Mat xI(2,n), xJ(2,n);
          size_t cptIndex = 0;
          for (const auto & track_iter : map_tracksCommon)
          {
            auto iter = track_iter.second.cbegin();
            const uint32_t i = iter->second;
            const uint32_t j = (++iter)->second;

            Vec2 feat = features_provider_->feats_per_view[I][i].coords().cast<double>();
            xI.col(cptIndex) = cam_I->get_ud_pixel(feat);
            feat = features_provider_->feats_per_view[J][j].coords().cast<double>();
            xJ.col(cptIndex) = cam_J->get_ud_pixel(feat);
            ++cptIndex;
          }

          // Robust estimation of the relative pose
          RelativePose_Info relativePose_info;
          relativePose_info.initial_residual_tolerance = Square(4.0);

          if (robustRelativePose(
                cam_I, cam_J,
                xI, xJ, relativePose_info,
                {cam_I->w(), cam_I->h()}, {cam_J->w(), cam_J->h()},
                256)
              && relativePose_info.vec_inliers.size() > iMin_inliers_count)
          {
            // Triangulate inliers & compute angle between bearing vectors
            std::vector<float> vec_angles;
            vec_angles.reserve(relativePose_info.vec_inliers.size());
            const Pose3 pose_I = Pose3(Mat3::Identity(), Vec3::Zero());
            const Pose3 pose_J = relativePose_info.relativePose;
            for (const uint32_t & inlier_idx : relativePose_info.vec_inliers)
            {
              openMVG::tracks::STLMAPTracks::const_iterator iterT = map_tracksCommon.begin();
              std::advance(iterT, inlier_idx);
              tracks::submapTrack::const_iterator iter = iterT->second.begin();
              const Vec2 featI = features_provider_->feats_per_view[I][iter->second].coords().cast<double>();
              const Vec2 featJ = features_provider_->feats_per_view[J][(++iter)->second].coords().cast<double>();
              vec_angles.push_back(AngleBetweenRay(pose_I, cam_I, pose_J, cam_J,
                cam_I->get_ud_pixel(featI), cam_J->get_ud_pixel(featJ)));
            }
            // Compute the median triangulation angle
            const unsigned median_index = vec_angles.size() / 2;
            std::nth_element(
              vec_angles.begin(),
              vec_angles.begin() + median_index,
              vec_angles.end());
            const float scoring_angle = vec_angles[median_index];
            // Store the pair iff the pair is in the asked angle range [fRequired_min_angle;fLimit_max_angle]
            if (scoring_angle > fRequired_min_angle &&
                scoring_angle < fLimit_max_angle)
            {
  #ifdef OPENMVG_USE_OPENMP
              #pragma omp critical
  #endif
              scoring_per_pair.emplace_back(scoring_angle, current_pair);
            }
          }
        }
      }
    } // omp section
  }
  std::sort(scoring_per_pair.begin(), scoring_per_pair.end());
  // Since scoring is ordered in increasing order, reverse the order
  std::reverse(scoring_per_pair.begin(), scoring_per_pair.end());
  if (!scoring_per_pair.empty())
  {
    initial_pair = scoring_per_pair.begin()->second;
    return true;
  }
  return false;
}

// Sketching automatic triplet generation!
// TODO XXX: MAKE IT WORK AND INTEGRATE INTO MAIN!!!
//
//bool SequentialSfMReconstructionEngine::AutomaticInitialTripletChoice(Triplet & initial_triplet) const
//{
//  // select a triplet that have the largest baseline (mean angle between its bearing vectors).
//  // if two of the views are close enough, discard it
//
//  const unsigned iMin_inliers_count = 100;
//  const float fRequired_min_angle = 3.0f;
//  const float fLimit_max_angle = 60.0f; // More than 60 degree, we cannot rely on matches for initial pair seeding
//
//  // List Views that support valid intrinsic (view that could be used for Essential matrix computation)
//  std::set<IndexT> valid_views;
//  for (Views::const_iterator it = sfm_data_.GetViews().begin();
//    it != sfm_data_.GetViews().end(); ++it)
//  {
//    const View * v = it->second.get();
//    if (sfm_data_.GetIntrinsics().count(v->id_intrinsic))
//      valid_views.insert(v->id_view);
//  }
//
//  if (valid_views.size() < 3)
//  {
//    return false; // There is not view that support valid intrinsic data
//  }
//
//  std::vector<std::pair<double, Pair>> scoring_per_pair;
//
//  // Compute the relative pose & the 'baseline score'
//  system::LoggerProgress my_progress_bar( matches_provider_->pairWise_matches_.size(),
//    "Selection of an initial pair");
//#ifdef OPENMVG_USE_OPENMP
//  #pragma omp parallel
//#endif
//  for (const std::pair<Pair, IndMatches> & match_pair : matches_provider_->pairWise_matches_)
//  {
//#ifdef OPENMVG_USE_OPENMP
//  #pragma omp single nowait
//#endif
//    {
//      ++my_progress_bar;
//
//      const Pair current_pair = match_pair.first;
//
//      const uint32_t I = std::min(current_pair.first, current_pair.second);
//      const uint32_t J = std::max(current_pair.first, current_pair.second);
//      if (valid_views.count(I) && valid_views.count(J))
//      {
//        const View
//          * view_I = sfm_data_.GetViews().at(I).get(),
//          * view_J = sfm_data_.GetViews().at(J).get();
//        const Intrinsics::const_iterator
//          iterIntrinsic_I = sfm_data_.GetIntrinsics().find(view_I->id_intrinsic),
//          iterIntrinsic_J = sfm_data_.GetIntrinsics().find(view_J->id_intrinsic);
//
//        const auto
//          cam_I = iterIntrinsic_I->second.get(),
//          cam_J = iterIntrinsic_J->second.get();
//        if (cam_I && cam_J)
//        {
//          openMVG::tracks::STLMAPTracks map_tracksCommon;
//          shared_track_visibility_helper_->GetTracksInImages({I, J}, map_tracksCommon);
//
//          // Copy points correspondences to arrays for relative pose estimation
//          const size_t n = map_tracksCommon.size();
//          Mat xI(2,n), xJ(2,n);
//          size_t cptIndex = 0;
//          for (const auto & track_iter : map_tracksCommon)
//          {
//            auto iter = track_iter.second.cbegin();
//            const uint32_t i = iter->second;
//            const uint32_t j = (++iter)->second;
//
//            Vec2 feat = features_provider_->feats_per_view[I][i].coords().cast<double>();
//            xI.col(cptIndex) = cam_I->get_ud_pixel(feat);
//            feat = features_provider_->feats_per_view[J][j].coords().cast<double>();
//            xJ.col(cptIndex) = cam_J->get_ud_pixel(feat);
//            ++cptIndex;
//          }
//
//          // Robust estimation of the relative pose
//          RelativePose_Info relativePose_info;
//          relativePose_info.initial_residual_tolerance = Square(4.0);
//
//          if (robustRelativePose(
//                cam_I, cam_J,
//                xI, xJ, relativePose_info,
//                {cam_I->w(), cam_I->h()}, {cam_J->w(), cam_J->h()},
//                256)
//              && relativePose_info.vec_inliers.size() > iMin_inliers_count)
//          {
//            // Triangulate inliers & compute angle between bearing vectors
//            std::vector<float> vec_angles;
//            vec_angles.reserve(relativePose_info.vec_inliers.size());
//            const Pose3 pose_I = Pose3(Mat3::Identity(), Vec3::Zero());
//            const Pose3 pose_J = relativePose_info.relativePose;
//            for (const uint32_t & inlier_idx : relativePose_info.vec_inliers)
//            {
//              openMVG::tracks::STLMAPTracks::const_iterator iterT = map_tracksCommon.begin();
//              std::advance(iterT, inlier_idx);
//              tracks::submapTrack::const_iterator iter = iterT->second.begin();
//              const Vec2 featI = features_provider_->feats_per_view[I][iter->second].coords().cast<double>();
//              const Vec2 featJ = features_provider_->feats_per_view[J][(++iter)->second].coords().cast<double>();
//              vec_angles.push_back(AngleBetweenRay(pose_I, cam_I, pose_J, cam_J,
//                cam_I->get_ud_pixel(featI), cam_J->get_ud_pixel(featJ)));
//            }
//            // Compute the median triangulation angle
//            const unsigned median_index = vec_angles.size() / 2;
//            std::nth_element(
//              vec_angles.begin(),
//              vec_angles.begin() + median_index,
//              vec_angles.end());
//            const float scoring_angle = vec_angles[median_index];
//            // Store the pair iff the pair is in the asked angle range [fRequired_min_angle;fLimit_max_angle]
//            if (scoring_angle > fRequired_min_angle &&
//                scoring_angle < fLimit_max_angle)
//            {
//  #ifdef OPENMVG_USE_OPENMP
//              #pragma omp critical
//  #endif
//              scoring_per_pair.emplace_back(scoring_angle, current_pair);
//            }
//          }
//        }
//      }
//    } // omp section
//  }
//  std::sort(scoring_per_pair.begin(), scoring_per_pair.end());
//  // Since scoring is ordered in increasing order, reverse the order
//  std::reverse(scoring_per_pair.begin(), scoring_per_pair.end());
//  if (!scoring_per_pair.empty())
//  {
//    initial_pair = scoring_per_pair.begin()->second;
//    return true;
//  }
//  return false;
//}

/// Compute the initial 3D seed (First camera t=0; R=Id, second estimated by 5 point algorithm)
bool SequentialSfMReconstructionEngine::MakeInitialPair3D(const Pair & current_pair)
{
  // Compute robust Essential matrix for ImageId [I,J]
  // use min max to have I < J
  const uint32_t
    I = std::min(current_pair.first, current_pair.second),
    J = std::max(current_pair.first, current_pair.second);

  if (sfm_data_.GetViews().count(I) == 0)
  {
    OPENMVG_LOG_ERROR << "Cannot find the view corresponding to the view id: " << I;
    return false;
  }
  if (sfm_data_.GetViews().count(J) == 0)
  {
    OPENMVG_LOG_ERROR << "Cannot find the view corresponding to the view id: " << J;
    return false;
  }
  // a. Assert we have valid cameras
  const View
    * view_I = sfm_data_.GetViews().at(I).get(),
    * view_J = sfm_data_.GetViews().at(J).get();
  const Intrinsics::const_iterator
    iterIntrinsic_I = sfm_data_.GetIntrinsics().find(view_I->id_intrinsic),
    iterIntrinsic_J = sfm_data_.GetIntrinsics().find(view_J->id_intrinsic);

  if (iterIntrinsic_I == sfm_data_.GetIntrinsics().end() ||
      iterIntrinsic_J == sfm_data_.GetIntrinsics().end() )
  {
    OPENMVG_LOG_ERROR << "Views with valid intrinsic data are required.";
    return false;
  }

  const auto
    * cam_I = iterIntrinsic_I->second.get(),
    * cam_J = iterIntrinsic_J->second.get();
  if (!cam_I || !cam_J)
  {
    OPENMVG_LOG_ERROR << "Cannot get back the camera intrinsic model for the pair.";
    return false;
  }

  OPENMVG_LOG_INFO << "Putative starting pair info:"
    << "\nindex:(" << I << "," << J << ")"
    << "\nview basename:("
    << stlplus::basename_part(view_I->s_Img_path) << ","
    << stlplus::basename_part(view_J->s_Img_path) << ")";

  // b. Get common features between the two view
  // use the track to have a more dense match correspondence set
  openMVG::tracks::STLMAPTracks map_tracksCommon;
  shared_track_visibility_helper_->GetTracksInImages({I, J}, map_tracksCommon);

  //-- Copy point to arrays
  const size_t n = map_tracksCommon.size();
  Mat xI(2,n), xJ(2,n);
  uint32_t cptIndex = 0;
  for (const auto & track_iter : map_tracksCommon)
  {
    auto iter = track_iter.second.cbegin();
    const uint32_t
      i = iter->second,
      j = (++iter)->second;

    Vec2 feat = features_provider_->feats_per_view[I][i].coords().cast<double>();
    xI.col(cptIndex) = cam_I->get_ud_pixel(feat);
    feat = features_provider_->feats_per_view[J][j].coords().cast<double>();
    xJ.col(cptIndex) = cam_J->get_ud_pixel(feat);
    ++cptIndex;
  }

  // c. Robust estimation of the relative pose
  RelativePose_Info relativePose_info;

  const std::pair<size_t, size_t>
    imageSize_I(cam_I->w(), cam_I->h()),
    imageSize_J(cam_J->w(), cam_J->h());

  if (!robustRelativePose(
    cam_I, cam_J, xI, xJ, relativePose_info, imageSize_I, imageSize_J, 4096))
  {
    OPENMVG_LOG_ERROR << " /!\\ Robust estimation failed to compute E for this pair: "
      << "{"<< current_pair.first << "," << current_pair.second << "}";
    return false;
  }
  OPENMVG_LOG_INFO << "Relative pose a-contrario upper_bound residual is: "
    << relativePose_info.found_residual_precision;
  // Bound min precision at 1 pix.
  relativePose_info.found_residual_precision = std::max(relativePose_info.found_residual_precision, 1.0);

  const bool bRefine_using_BA = true;
  if (bRefine_using_BA)
  {
    // Refine the defined scene
    SfM_Data tiny_scene;
    tiny_scene.views.insert(*sfm_data_.GetViews().find(view_I->id_view));
    tiny_scene.views.insert(*sfm_data_.GetViews().find(view_J->id_view));
    tiny_scene.intrinsics.insert(*sfm_data_.GetIntrinsics().find(view_I->id_intrinsic));
    tiny_scene.intrinsics.insert(*sfm_data_.GetIntrinsics().find(view_J->id_intrinsic));

    // Init poses
    const Pose3 & Pose_I = tiny_scene.poses[view_I->id_pose] = Pose3(Mat3::Identity(), Vec3::Zero());
    const Pose3 & Pose_J = tiny_scene.poses[view_J->id_pose] = relativePose_info.relativePose;

    // Init structure
    Landmarks & landmarks = tiny_scene.structure;

    for (const auto & track_iterator : map_tracksCommon)
    {
      // Get corresponding points
      auto iter = track_iterator.second.cbegin();
      const uint32_t
        i = iter->second,
        j = (++iter)->second;

      const Vec2
        x1 = features_provider_->feats_per_view[I][i].coords().cast<double>(),
        x2 = features_provider_->feats_per_view[J][j].coords().cast<double>();

      Vec3 X;
      if (Triangulate2View(
            Pose_I.rotation(),
            Pose_I.translation(),
            (*cam_I)(cam_I->get_ud_pixel(x1)),
            Pose_J.rotation(),
            Pose_J.translation(),
            (*cam_J)(cam_J->get_ud_pixel(x2)),
            X,
            triangulation_method_))
      {
        Observations obs;
        obs[view_I->id_view] = Observation(x1, i);
        obs[view_J->id_view] = Observation(x2, j);
        landmarks[track_iterator.first].obs = std::move(obs);
        landmarks[track_iterator.first].X = X;
      }
    }
    Save(tiny_scene, stlplus::create_filespec(sOut_directory_, "initialPair.ply"), ESfM_Data(ALL));

    // - refine only Structure and Rotations & translations (keep intrinsic constant)
    Bundle_Adjustment_Ceres::BA_Ceres_options options(true, true);
    options.linear_solver_type_ = ceres::DENSE_SCHUR;
    Bundle_Adjustment_Ceres bundle_adjustment_obj(options);
    if (!bundle_adjustment_obj.Adjust(tiny_scene,
        Optimize_Options
        (
          Intrinsic_Parameter_Type::NONE, // Keep intrinsic constant
          Extrinsic_Parameter_Type::ADJUST_ALL, // Adjust camera motion
          Structure_Parameter_Type::ADJUST_ALL) // Adjust structure
        )
      )
    {
      return false;
    }

    // Save computed data
    const Pose3 pose_I = sfm_data_.poses[view_I->id_pose] = tiny_scene.poses[view_I->id_pose];
    const Pose3 pose_J = sfm_data_.poses[view_J->id_pose] = tiny_scene.poses[view_J->id_pose];
    map_ACThreshold_.insert({I, relativePose_info.found_residual_precision});
    map_ACThreshold_.insert({J, relativePose_info.found_residual_precision});
    set_remaining_view_id_.erase(view_I->id_view);
    set_remaining_view_id_.erase(view_J->id_view);

    // List inliers and save them
    for (const auto & landmark_entry : tiny_scene.GetLandmarks())
    {
      const IndexT trackId = landmark_entry.first;
      const Landmark & landmark = landmark_entry.second;
      const Observations & obs = landmark.obs;
      Observations::const_iterator
        iterObs_xI = obs.find(view_I->id_view),
        iterObs_xJ = obs.find(view_J->id_view);

      const Observation & ob_xI = iterObs_xI->second;
      const Observation & ob_xJ = iterObs_xJ->second;
      const Vec2
        ob_xI_ud = cam_I->get_ud_pixel(ob_xI.x),
        ob_xJ_ud = cam_J->get_ud_pixel(ob_xJ.x);

      const double angle = AngleBetweenRay(
        pose_I, cam_I, pose_J, cam_J, ob_xI_ud, ob_xJ_ud);
      const Vec2 residual_I = cam_I->residual(pose_I(landmark.X), ob_xI.x);
      const Vec2 residual_J = cam_J->residual(pose_J(landmark.X), ob_xJ.x);
      if (angle > 2.0 &&
          CheiralityTest((*cam_I)(ob_xI_ud), pose_I,
                         (*cam_J)(ob_xJ_ud), pose_J,
                         landmark.X) &&
          residual_I.norm() < relativePose_info.found_residual_precision &&
          residual_J.norm() < relativePose_info.found_residual_precision)
      {
        sfm_data_.structure[trackId] = landmarks[trackId];
      }
    }
    // Save outlier residual information
    Histogram<double> histoResiduals;
    OPENMVG_LOG_INFO
      << "\n=========================\n"
      << " MSE Residual InitialPair Inlier:\n";
    ComputeResidualsHistogram(&histoResiduals);
    std::cout << "passed Histogram\n";
    if (!sLogging_file_.empty())
    {
      using namespace htmlDocument;
      html_doc_stream_->pushInfo(htmlMarkup("h1","Essential Matrix."));
      std::ostringstream os;
      os
        << "-------------------------------" << "<br>"
        << "-- Robust Essential matrix: <"  << I << "," <<J << "> images: "
        << view_I->s_Img_path << ","
        << view_J->s_Img_path << "<br>"
        << "-- Threshold: " << relativePose_info.found_residual_precision << "<br>"
        << "-- Resection status: " << "OK" << "<br>"
        << "-- Nb points used for robust Essential matrix estimation: "
        << xI.cols() << "<br>"
        << "-- Nb points validated by robust estimation: "
        << sfm_data_.structure.size() << "<br>"
        << "-- % points validated: "
        << sfm_data_.structure.size()/static_cast<float>(xI.cols())
        << "<br>"
        << "-------------------------------" << "<br>";
      html_doc_stream_->pushInfo(os.str());

      html_doc_stream_->pushInfo(htmlMarkup("h2",
        "Residual of the robust estimation (Initial triangulation). Thresholded at: "
        + toString(relativePose_info.found_residual_precision)));

      html_doc_stream_->pushInfo(htmlMarkup("h2","Histogram of residuals"));

      const std::vector<double> xBin = histoResiduals.GetXbinsValue();
      const auto range = autoJSXGraphViewport<double>(xBin, histoResiduals.GetHist());

      htmlDocument::JSXGraphWrapper jsxGraph;
      jsxGraph.init("InitialPairTriangulationKeptInfo",600,300);
      jsxGraph.addXYChart(xBin, histoResiduals.GetHist(), "line,point");
      jsxGraph.addLine(relativePose_info.found_residual_precision, 0,
        relativePose_info.found_residual_precision, histoResiduals.GetHist().front());
      jsxGraph.UnsuspendUpdate();
      jsxGraph.setViewport(range);
      jsxGraph.close();
      html_doc_stream_->pushInfo(jsxGraph.toStr());

      html_doc_stream_->pushInfo("<hr>");

      std::ofstream htmlFileStream( std::string(stlplus::folder_append_separator(sOut_directory_) +
        "Reconstruction_Report.html"));
      htmlFileStream << html_doc_stream_->getDoc();
    }
  }
  return !sfm_data_.structure.empty();
}

double SequentialSfMReconstructionEngine::ComputeResidualsHistogram(Histogram<double> * histo)
{
  // Collect residuals for each observation
  std::vector<float> vec_residuals;
  vec_residuals.reserve(sfm_data_.structure.size());
  for (const auto & landmark_entry : sfm_data_.GetLandmarks())
  {
   OPENMVG_LOG_INFO << "3D point " << landmark_entry.second.X << std::endl;
    const Observations & obs = landmark_entry.second.obs;
    for (const auto & observation : obs)
    {
      const View * view = sfm_data_.GetViews().find(observation.first)->second.get();
      const Pose3 pose = sfm_data_.GetPoseOrDie(view);
      OPENMVG_LOG_INFO << "Pose rotation" << pose.rotation() << std::endl;
      OPENMVG_LOG_INFO << "Pose center " << pose.center() << std::endl;
      const auto intrinsic = sfm_data_.GetIntrinsics().find(view->id_intrinsic)->second;
      const Vec2 residual = intrinsic->residual(pose(landmark_entry.second.X), observation.second.x);
      OPENMVG_LOG_INFO << "Raw residual " << residual;
      vec_residuals.emplace_back( std::abs(residual(0)) );
      vec_residuals.emplace_back( std::abs(residual(1)) );
    }
  }
  // Display statistics
  if (vec_residuals.size() > 1)
  {
    float dMin, dMax, dMean, dMedian;
    minMaxMeanMedian<float>(vec_residuals.cbegin(), vec_residuals.cend(),
                            dMin, dMax, dMean, dMedian);
    if (histo)  {
      *histo = Histogram<double>(dMin, dMax, 10);
      histo->Add(vec_residuals.cbegin(), vec_residuals.cend());
    }

    OPENMVG_LOG_INFO
      << "\nSequentialSfMReconstructionEngine::ComputeResidualsMSE."
      << "\n\t-- #Tracks:\t" << sfm_data_.GetLandmarks().size()
      << "\n\t-- Residual min:\t" << dMin
      << "\n\t-- Residual median:\t" << dMedian
      << "\n\t-- Residual max:\t "  << dMax
      << "\n\t-- Residual mean:\t " << dMean;

    return dMean;
  }
  return -1.0;
}

/// Functor to sort a vector of pair given the pair's second value
template<class T1, class T2, class Pred = std::less<T2>>
struct sort_pair_second {
  bool operator()(const std::pair<T1,T2>&left,
                    const std::pair<T1,T2>&right)
  {
    Pred p;
    return p(left.second, right.second);
  }
};

/**
 * @brief Discard tracks with too large residual error
 *
 * Remove observation/tracks that have:
 *  - too large residual error
 *  - too small angular value
 *
 * @return True if more than 'count' outliers have been removed.
 */
bool SequentialSfMReconstructionEngine::badTrackRejector(double dPrecision, size_t count)
{
  const size_t nbOutliers_residualErr = RemoveOutliers_PixelResidualError(sfm_data_, dPrecision, 2);
  const size_t nbOutliers_angleErr = RemoveOutliers_AngleError(sfm_data_, 2.0);

  return (nbOutliers_residualErr + nbOutliers_angleErr) > count;
}

