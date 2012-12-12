#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/math/tools/roots.hpp>

#include "track.hpp"
#include "track_list.hpp"
#include "multiview_track_list.hpp"
#include "camera.hpp"
#include "distortion.hpp"
#include "util.hpp"

#include "track_list_reader.hpp"
#include "image_point_reader.hpp"
#include "read_lines.hpp"
#include "camera_pose_reader.hpp"
#include "camera_properties_reader.hpp"
#include "multiview_track_list_writer.hpp"
#include "image_point_writer.hpp"

struct OtherView {
  int index;
  Camera camera;
};

std::pair<int, cv::Point2d> calibrateIndexedPoint(
    const std::pair<int, cv::Point2d>& point,
    const cv::Mat& K_inv) {
  cv::Point2d x = point.second;
  cv::Mat X = imagePointToHomogeneous(x);
  cv::Mat Y = K_inv * X;
  cv::Point2d y = imagePointFromHomogeneous(Y);
  return std::make_pair(point.first, y);
}

bool indexedPointIsNotUndistortable(const std::pair<int, cv::Point2d>& point,
                                    double w) {
  return !isUndistortable(point.second, w);
}

std::pair<int, cv::Point2d> undistortIndexedPoint(
    const std::pair<int, cv::Point2d>& point,
    double w) {
  return std::make_pair(point.first, undistort(point.second, w));
}

Track<cv::Point2d> calibrateAndUndistortTrack(
    const Track<cv::Point2d>& track,
    const CameraProperties& intrinsics) {
  // Calibrate each point, undo intrinsics.
  Track<cv::Point2d> calibrated;
  cv::Mat K_inv(intrinsics.matrix().inv());
  std::transform(track.begin(), track.end(),
      std::inserter(calibrated, calibrated.begin()),
      boost::bind(calibrateIndexedPoint, _1, K_inv));

  // Remove non-undistortable points.
  Track<cv::Point2d> valid;
  std::remove_copy_if(calibrated.begin(), calibrated.end(),
      std::inserter(valid, valid.begin()),
      boost::bind(indexedPointIsNotUndistortable, _1, intrinsics.distort_w));
  DLOG(INFO) << valid.size() << " / " << calibrated.size() <<
      " could be undistorted";

  // Undistort undistortable points.
  Track<cv::Point2d> undistorted;
  std::transform(valid.begin(), valid.end(),
    std::inserter(undistorted, undistorted.begin()),
    boost::bind(undistortIndexedPoint, _1, intrinsics.distort_w));

  return undistorted;
}

////////////////////////////////////////////////////////////////////////////////

Camera extractCameraFromOtherView(const OtherView& view) {
  return view.camera;
}

double errorInDistanceFromPoint(cv::Point2d y,
                                double lambda,
                                const cv::Mat A,
                                const cv::Mat B,
                                double delta,
                                const CameraProperties& intrinsics) {
  cv::Mat X = A + lambda * B;
  cv::Point2d x = imagePointFromHomogeneous(X);
  x = intrinsics.distortAndUncalibrate(x);
  return cv::norm(x - y) - delta;
}

bool toleranceIsOk(double x, double y, double epsilon) {
  return std::abs(x - y) < epsilon;
}

// projection -- Already calibrated and undistorted.
void findExtentOfRay(const cv::Point2d& projection,
                     const CameraPose& camera,
                     const std::vector<Camera>& others) {
  cv::Point2d w = projection;

  // Camera center.
  cv::Point3d c(camera.center);

  // Find vector in nullspace of linear projection system, A = R_xy - w R_z.
  cv::Mat R(camera.rotation);
  cv::Mat A = R.rowRange(0, 2) - cv::Mat(w) * R.rowRange(2, 3);
  // 1D nullspace found trivially by cross-product.
  // Take negative i x j because z < 0 is in front of camera.
  cv::Mat V = -A.row(0).t().cross(A.row(1).t());
  cv::Point3d v(V.at<double>(0, 0), V.at<double>(0, 1), V.at<double>(0, 2));

  // Space of solutions parametrized by 3D line c + lambda v, with lambda >= 0.

  // For each image, find intersections of the projected line with the outer
  // radius of the lens distortion.
  std::vector<Camera>::const_iterator other;
  for (other = others.begin(); other != others.end(); ++other) {
    cv::Mat P(other->extrinsics().matrix());

    cv::Mat C = worldPointToHomogeneous(c);
    cv::Mat V = worldPointToHomogeneous(v, 0);
    cv::Mat A = P * C;
    cv::Mat B = P * V;

    // Assume that line has a vanishing point (b is not at infinity).
    // TODO: Cope with non-vanishing-point case (b at infinity).
    //cv::Point2d a = imagePointFromHomogeneous(A);
    //CHECK(B.at<double>(2, 0) != 0);
    //cv::Point2d b = imagePointFromHomogeneous(B);

    double a3 = A.at<double>(2, 0);
    double b3 = B.at<double>(2, 0);

    if (a3 > 0 && b3 > 0) {
      // Entire ray is behind camera.
      DLOG(INFO) << "Ray is not observed";
      continue;
    }

    double delta = 1;

    double lambda;
    cv::Point2d x;
    double lambda_min;

    if (a3 < 0) {
      // Ray starts in front of camera. Line starts at a finite coordinate.
      DLOG(INFO) << "Ray starts in front of camera";
      lambda_min = 0;
    } else {
      // Ray starts behind camera. Line starts at infinity.
      DLOG(INFO) << "Ray starts behind camera";
      lambda_min = -a3 / b3;
    }

    if (b3 < 0) {
      // Ray goes to infinity in front of camera. There is a vanishing point.
      DLOG(INFO) << "Ray ends in front of camera";
      CHECK(B.at<double>(2, 0) != 0);
      x = imagePointFromHomogeneous(B);
      x = other->intrinsics().distortAndUncalibrate(x);

      // We can't use lambda = infinity for bisection.
      // Find a lambda which is big enough.
      lambda = 1.;
      bool found = false;
      while (!found) {
        double error = errorInDistanceFromPoint(x, lambda, A, B, delta,
            other->intrinsics());
        if (error < 0) {
          found = true;
        }
        lambda *= 2;
      }
    } else {
      // Ray goes to infinity behind camera, crossing image plane.
      // There is no vanishing point. However, under distortion, a 2D point at
      // infinity will still have a finite position.
      DLOG(INFO) << "Ray ends behind camera";
      lambda = -a3 / b3;
      cv::Mat X = A + lambda * B;
      x = cv::Point2d(X.at<double>(0, 0), X.at<double>(1, 0));
      x = distortPointAtInfinity(x, other->intrinsics().distort_w);
      x = other->intrinsics().uncalibrate(x);
    }

    std::vector<double> lambdas;
    bool converged = false;

    while (!converged) {
      // Check there is a point on the line at least delta pixels away from x.
      double f_max = errorInDistanceFromPoint(x, lambda_min, A, B, delta,
            other->intrinsics());

      if (f_max < 0) {
        // No solution is possible.
        converged = true;
      } else {
        double old_lambda = lambda;

        // Find lambda which gives point delta pixels away from x.
        std::pair<double, double> interval = boost::math::tools::bisect(
              boost::bind(errorInDistanceFromPoint, x, _1, A, B, delta,
                other->intrinsics()),
              lambda_min, lambda,
              boost::math::tools::eps_tolerance<double>(16));
        lambda = interval.second;

        // Guard against limit cycles.
        CHECK(lambda != old_lambda) << "Entered limit cycle";

        // Add lambda to the list.
        lambdas.push_back(lambda);

        // Update position.
        cv::Mat X = A + lambda * B;
        x = imagePointFromHomogeneous(X);
        x = other->intrinsics().distortAndUncalibrate(x);

        DLOG(INFO) << "x(" << lambda << ") => " << x;
      }
    }

    LOG(INFO) << "Quantized ray into " << lambdas.size() << " positions";
  }
}

void findMultiviewTrack(const Track<cv::Point2d>& track,
                        const CameraPose& pose,
                        const std::vector<OtherView>& other_views,
                        MultiviewTrack<cv::Point2d>& multiview_track) {
  int num_other_views = other_views.size();
  int num_views = num_other_views + 1;

  multiview_track = MultiviewTrack<cv::Point2d>(num_views);

  std::vector<Camera> other_cameras;
  std::transform(other_views.begin(), other_views.end(),
      std::back_inserter(other_cameras), extractCameraFromOtherView);

  // For dynamic program, need to find the extent of the 3D ray in each frame.
  Track<cv::Point2d>::const_iterator point;
  for (point = track.begin(); point != track.end(); ++point) {
    findExtentOfRay(point->second, pose, other_cameras);
  }
}

void findMultiviewTracks(
    const TrackList<cv::Point2d>& tracks,
    const CameraPose& camera,
    const std::vector<OtherView>& other_views,
    MultiviewTrackList<cv::Point2d>& multiview_tracks) {
  int num_views = other_views.size() + 1;
  multiview_tracks = MultiviewTrackList<cv::Point2d>(num_views);

  TrackList<cv::Point2d>::const_iterator track;
  for (track = tracks.begin(); track != tracks.end(); ++track) {
    // Find match for this track.
    MultiviewTrack<cv::Point2d> multiview_track;
    findMultiviewTrack(*track, camera, other_views, multiview_track);

    // Swap into end of list.
    multiview_tracks.push_back(MultiviewTrack<cv::Point2d>());
    multiview_tracks.back().swap(multiview_track);
  }
}

////////////////////////////////////////////////////////////////////////////////

std::string makeViewFilename(const std::string& format,
                             const std::string& name) {
  return boost::str(boost::format(format) % name);
}

void init(int& argc, char**& argv) {
  std::ostringstream usage;
  usage << "Finds an optimal multiview track given a track in one view" <<
      std::endl;
  usage << std::endl;
  usage << argv[0] << " view-index tracks image-format extrinsics-format "
      "intrinsics-format views num-frames multiview-tracks" << std::endl;
  usage << std::endl;
  usage << "Parameters:" << std::endl;
  usage << "view-index -- Zero-based index of view to which the original "
      "tracks belong" << std::endl;
  usage << "image-format -- e.g. images/%s/%07d.png" << std::endl;
  usage << "extrinsics-format -- e.g. extrinsics/%s.yaml" << std::endl;
  usage << "intrinsics-format -- e.g. intrinsics/%s.yaml" << std::endl;
  usage << "view -- Text file whose lines are the view names" << std::endl;
  google::SetUsageMessage(usage.str());

  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

  if (argc != 9) {
    google::ShowUsageWithFlags(argv[0]);
    std::exit(1);
  }
}

int main(int argc, char** argv) {
  bool ok;

  init(argc, argv);
  int main_view = boost::lexical_cast<int>(argv[1]);
  std::string input_tracks_file = argv[2];
  std::string image_format = argv[3];
  std::string extrinsics_format = argv[4];
  std::string intrinsics_format = argv[5];
  std::string views_file = argv[6];
  int num_frames = boost::lexical_cast<int>(argv[7]);
  std::string multiview_tracks_file = argv[8];

  // Load tracks.
  TrackList<cv::Point2d> input_tracks;
  ImagePointReader<double> point_reader;
  ok = loadTrackList(input_tracks_file, input_tracks, point_reader);
  CHECK(ok) << "Could not load tracks";
  LOG(INFO) << "Loaded " << input_tracks.size() << " single-view tracks";

  // Load names of views.
  std::vector<std::string> view_names;
  ok = readLines(views_file, view_names);
  CHECK(ok) << "Could not load view names";
  int num_views = view_names.size();
  LOG(INFO) << "Matching to " << num_views << " views";

  CHECK(main_view >= 0);
  CHECK(main_view < num_views);

  // Load properties of each view.
  std::vector<OtherView> other_views;
  Camera camera;

  CameraPoseReader extrinsics_reader;
  CameraPropertiesReader intrinsics_reader;

  for (int view = 0; view < num_views; view += 1) {
    const std::string& name = view_names[view];

    // Load cameras for all views.
    CameraProperties intrinsics;
    std::string intrinsics_file = makeViewFilename(intrinsics_format, name);
    ok = load(intrinsics_file, intrinsics, intrinsics_reader);
    CHECK(ok) << "Could not load intrinsics";

    CameraPose extrinsics;
    std::string extrinsics_file = makeViewFilename(extrinsics_format, name);
    ok = load(extrinsics_file, extrinsics, extrinsics_reader);
    CHECK(ok) << "Could not load extrinsics";

    Camera view_camera(intrinsics, extrinsics);

    if (view == main_view) {
      // Set intrinsics of main view.
      camera = view_camera;
    } else {
      OtherView other_view;
      other_view.index = view;
      other_view.camera = view_camera;
      other_views.push_back(other_view);
    }
  }

  // Undistort points in original view.
  TrackList<cv::Point2d> undistorted_tracks;
  std::transform(input_tracks.begin(), input_tracks.end(),
      std::back_inserter(undistorted_tracks),
      boost::bind(calibrateAndUndistortTrack, _1, camera.intrinsics()));

  // Find multiview tracks.
  MultiviewTrackList<cv::Point2d> multiview_tracks;
  findMultiviewTracks(undistorted_tracks, camera.extrinsics(), other_views,
      multiview_tracks);

  // Save points and tracks out.
  ImagePointWriter<double> point_writer;
  ok = saveMultiviewTrackList(multiview_tracks_file, multiview_tracks,
      point_writer);
  CHECK(ok) << "Could not save tracks";

  return 0;
}