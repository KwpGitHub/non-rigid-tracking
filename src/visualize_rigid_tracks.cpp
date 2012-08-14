#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <iterator>
#include <algorithm>
#include <cstdlib>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <gflags/gflags.h>
#include "read_image.hpp"
#include "track_list.hpp"
#include "rigid_feature.hpp"
#include "rigid_warp.hpp"
#include "random_color.hpp"
#include "rigid_feature_reader.hpp"
#include "track_list_reader.hpp"

const int PATCH_SIZE = 9;
const double SATURATION = 0.99;
const double BRIGHTNESS = 0.99;

DEFINE_string(output_format, "%d.png", "Location to save image.");
DEFINE_bool(save, false, "Save to file?");
DEFINE_bool(display, true, "Show in window?");

std::string makeFilename(const std::string& format, int n) {
  return boost::str(boost::format(format) % (n + 1));
}

void drawFeatures(cv::Mat& image,
                  const std::map<int, RigidFeature>& features,
                  const std::vector<cv::Scalar>& colors) {
  typedef std::map<int, RigidFeature> FeatureSet;
  typedef std::vector<cv::Scalar> ColorList;

  RigidWarp warp(PATCH_SIZE);

  FeatureSet::const_iterator mapping;
  for (mapping = features.begin(); mapping != features.end(); ++mapping) {
    int index = mapping->first;
    const RigidFeature& feature = mapping->second;

    warp.draw(image, feature.data(), PATCH_SIZE, colors[index]);
  }
}

void init(int& argc, char**& argv) {
  std::ostringstream usage;
  usage << "Visualizes rigid-warp tracks." << std::endl;
  usage << std::endl;
  usage << argv[0] << " tracks image-format";

  google::SetUsageMessage(usage.str());
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
}

int main(int argc, char** argv) {
  init(argc, argv);

  if (argc != 3) {
    google::ShowUsageWithFlags(argv[0]);
    return 1;
  }

  std::string tracks_file = argv[1];
  std::string image_format = argv[2];
  std::string output_format = FLAGS_output_format;

  bool ok;

  // Load tracks.
  TrackList_<RigidFeature> tracks;
  RigidFeatureReader feature_reader;
  ok = loadTrackList(tracks_file, tracks, feature_reader);
  CHECK(ok) << "Could not load tracks";
  LOG(INFO) << "Loaded " << tracks.size() << " tracks";

  // Make a list of random colors.
  typedef std::vector<cv::Scalar> ColorList;
  ColorList colors;
  for (int i = 0; i < int(tracks.size()); i += 1) {
    colors.push_back(randomColor(BRIGHTNESS, SATURATION));
  }

  // Iterate through frames in which track was observed.
  FrameIterator_<RigidFeature> frame(tracks);
  frame.seekToStart();

  while (!frame.end()) {
    // Get the current time.
    int t = frame.t();

    // Load the image.
    cv::Mat color_image;
    cv::Mat gray_image;
    ok = readImage(makeFilename(image_format, t), color_image, gray_image);
    CHECK(ok) << "Could not read image";

    // Get the features.
    typedef std::map<int, RigidFeature> FeatureSet;
    FeatureSet features;
    frame.getPoints(features);

    // Draw each one with its color.
    drawFeatures(color_image, features, colors);

    if (FLAGS_save) {
      std::string output_file = makeFilename(output_format, t);
      cv::imwrite(output_file, color_image);
    }

    if (FLAGS_display) {
      cv::imshow("tracks", color_image);
      cv::waitKey(10);
    }

    ++frame;
  }

  return 0;
}
