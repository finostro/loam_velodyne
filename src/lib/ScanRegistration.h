// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is an implementation of the algorithm described in the following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

#ifndef LOAM_SCANREGISTRATION_H
#define LOAM_SCANREGISTRATION_H


#include "CircularBuffer.h"
#include "math_utils.h"

#include <loam_velodyne/common.h>

#include <stdint.h>
#include <vector>
#include <sensor_msgs/PointCloud2.h>
#include <ros/node_handle.h>
#include <pcl_conversions/pcl_conversions.h>
#include <sensor_msgs/Imu.h>


namespace loam {

/** Point label options. */
enum PointLabel {
  CORNER_SHARP = 2,       ///< sharp corner point
  CORNER_LESS_SHARP = 1,  ///< less sharp corner point
  SURFACE_LESS_FLAT = 0,  ///< less flat surface point
  SURFACE_FLAT = -1       ///< flat surface point
};



/** Scan Registration configuration parameters. */
typedef struct RegistrationParams {
  RegistrationParams(int nFeatureRegions_ = 6,
                     int curvatureRegion_ = 5,
                     int maxCornerSharp_ = 2,
                     int maxSurfaceFlat_ = 4,
                     float lessFlatFilterSize_ = 0.2,
                     float surfaceCurvatureThreshold_ = 0.1)
        : nFeatureRegions(nFeatureRegions_),
          curvatureRegion(curvatureRegion_),
          maxCornerSharp(maxCornerSharp_),
          maxCornerLessSharp(10 * maxCornerSharp_),
          maxSurfaceFlat(maxSurfaceFlat_),
          lessFlatFilterSize(lessFlatFilterSize_),
          surfaceCurvatureThreshold(surfaceCurvatureThreshold_) {};

  ~RegistrationParams() {};

  /** The number of (equally sized) regions used to distribute the feature extraction within a scan. */
  int nFeatureRegions;

  /** The number of surrounding points (+/- region around a point) used to calculate a point curvature. */
  int curvatureRegion;

  /** The maximum number of sharp corner points per feature region. */
  int maxCornerSharp;

  /** The maximum number of less sharp corner points per feature region. */
  int maxCornerLessSharp;

  /** The maximum number of flat surface points per feature region. */
  int maxSurfaceFlat;

  /** The voxel size used for down sizing the remaining less flat surface points. */
  float lessFlatFilterSize;

  /** The curvature threshold below / above a point is considered a flat / corner point. */
  float surfaceCurvatureThreshold;


  /** \brief Parse node parameter.
   *
   * @param nh the ROS node handle
   * @return true, if all specified parameters are valid, false if at least one specified parameter is invalid
   */
  bool parseParams(const ros::NodeHandle& nh) {
    bool success = true;
    int iParam = 0;
    float fParam = 0;

    if (nh.getParam("featureRegions", iParam)) {
      if (iParam < 1) {
        ROS_ERROR("Invalid featureRegions parameter: %d (expected >= 1)", iParam);
        success = false;
      } else {
        nFeatureRegions = iParam;
      }
    }

    if (nh.getParam("curvatureRegion", iParam)) {
      if (iParam < 1) {
        ROS_ERROR("Invalid curvatureRegion parameter: %d (expected >= 1)", iParam);
        success = false;
      } else {
        curvatureRegion = iParam;
      }
    }

    if (nh.getParam("maxCornerSharp", iParam)) {
      if (iParam < 1) {
        ROS_ERROR("Invalid maxCornerSharp parameter: %d (expected >= 1)", iParam);
        success = false;
      } else {
        maxCornerSharp = iParam;
        maxCornerLessSharp = 10 * iParam;
      }
    }

    if (nh.getParam("maxCornerLessSharp", iParam)) {
      if (iParam < maxCornerSharp) {
        ROS_ERROR("Invalid maxCornerLessSharp parameter: %d (expected >= %d)", iParam, maxCornerSharp);
        success = false;
      } else {
        maxCornerLessSharp = iParam;
      }
    }

    if (nh.getParam("maxSurfaceFlat", iParam)) {
      if (iParam < 1) {
        ROS_ERROR("Invalid maxSurfaceFlat parameter: %d (expected >= 1)", iParam);
        success = false;
      } else {
        maxSurfaceFlat = iParam;
      }
    }

    if (nh.getParam("surfaceCurvatureThreshold", fParam)) {
      if (fParam < 0.001) {
        ROS_ERROR("Invalid surfaceCurvatureThreshold parameter: %f (expected >= 0.001)", fParam);
        success = false;
      } else {
        surfaceCurvatureThreshold = fParam;
      }
    }

    if (nh.getParam("lessFlatFilterSize", fParam)) {
      if (fParam < 0.001) {
        ROS_ERROR("Invalid lessFlatFilterSize parameter: %f (expected >= 0.001)", fParam);
        success = false;
      } else {
        lessFlatFilterSize = fParam;
      }
    }

    return success;
  };

  /** \brief Print parameters to ROS_INFO. */
  void print()
  {
    ROS_INFO_STREAM(" ===== scan registration parameters =====" << std::endl
        << "  - Using  " << nFeatureRegions << "  feature regions per scan." << std::endl
        << "  - Using  +/- " << curvatureRegion << "  points for curvature calculation." << std::endl
        << "  - Using at most  " << maxCornerSharp << "  sharp" << std::endl
        << "              and  " << maxCornerLessSharp << "  less sharp corner points per feature region." << std::endl
        << "  - Using at most  " << maxSurfaceFlat << "  flat surface points per feature region." << std::endl
        << "  - Using  " << surfaceCurvatureThreshold << "  as surface curvature threshold." << std::endl
        << "  - Using  " << lessFlatFilterSize << "  as less flat surface points voxel filter size.");
  };
} RegistrationParams;



/** IMU state data. */
typedef struct IMUState {
  /** The time of the measurement leading to this state (in seconds). */
  ros::Time stamp;

  /** The current roll angle. */
  Angle roll;

  /** The current pitch angle. */
  Angle pitch;

  /** The current yaw angle. */
  Angle yaw;

  /** The accumulated global IMU position in 3D space. */
  Vector3 position;

  /** The accumulated global IMU velocity in 3D space. */
  Vector3 velocity;

  /** The current (local) IMU acceleration in 3D space. */
  Vector3 acceleration;

  /** \brief Interpolate between two IMU states.
   *
   * @param start the first IMUState
   * @param end the second IMUState
   * @param ratio the interpolation ratio
   * @param result the target IMUState for storing the interpolation result
   */
  static void interpolate(const IMUState& start,
                          const IMUState& end,
                          const float& ratio,
                          IMUState& result)
  {
    float invRatio = 1 - ratio;

    result.roll = start.roll.rad() * invRatio + end.roll.rad() * ratio;
    result.pitch = start.pitch.rad() * invRatio + end.pitch.rad() * ratio;
    if (start.yaw.rad() - end.yaw.rad() > M_PI) {
      result.yaw = start.yaw.rad() * invRatio + (end.yaw.rad() + 2 * M_PI) * ratio;
    } else if (start.yaw.rad() - end.yaw.rad() < -M_PI) {
      result.yaw = start.yaw.rad() * invRatio + (end.yaw.rad() - 2 * M_PI) * ratio;
    } else {
      result.yaw = start.yaw.rad() * invRatio + end.yaw.rad() * ratio;
    }

    result.velocity = start.velocity * invRatio + end.velocity * ratio;
    result.position = start.position * invRatio + end.position * ratio;
  };
} IMUState;



/** \brief Base class for LOAM scan registration implementations.
 *
 * As there exist various sensor devices, producing differently formatted point clouds,
 * specific implementations are needed for each group of sensor devices to achieve an accurate registration.
 * This class provides common configurations, buffering and processing logic.
 */
class ScanRegistration {
public:
  explicit ScanRegistration(const float& scanPeriod,
                            const uint16_t& nScans = 0,
                            const size_t& imuHistorySize = 200,
                            const RegistrationParams& config = RegistrationParams());

  /** \brief Setup component.
   *
   * @param node the ROS node handle
   * @param privateNode the private ROS node handle
   */
  virtual bool setup(ros::NodeHandle& node,
                     ros::NodeHandle& privateNode);

  /** \brief Handler method for IMU messages.
   *
   * @param imuIn the new IMU message
   */
  virtual void handleIMUMessage(const sensor_msgs::Imu::ConstPtr& imuIn);

  /** \brief Retrieve the current full resolution input cloud.
   *
   * @return the current full resolution input cloud
   */
  pcl::PointCloud<pcl::PointXYZI>::Ptr getFullResCloud() { return _laserCloud; };

  /** \brief Retrieve the current sharp corner input sub cloud.
   *
   * @return the current sharp corner input sub cloud
   */
  pcl::PointCloud<pcl::PointXYZI>::Ptr getCornerPointsSharp() { return _cornerPointsSharp; };

  /** \brief Retrieve the current less sharp corner input sub cloud.
   *
   * @return the current less sharp corner input sub cloud
   */
  pcl::PointCloud<pcl::PointXYZI>::Ptr getCornerPointsLessSharp() { return _cornerPointsLessSharp; };

  /** \brief Retrieve the current flat surface input sub cloud.
   *
   * @return the current flat surface input sub cloud
   */
  pcl::PointCloud<pcl::PointXYZI>::Ptr getSurfacePointsFlat() { return _surfacePointsFlat; };

  /** \brief Retrieve the current less flat surface input sub cloud.
   *
   * @return the current less flat surface input sub cloud
   */
  pcl::PointCloud<pcl::PointXYZI>::Ptr getSurfacePointsLessFlat() { return _surfacePointsLessFlat; };

  /** \brief Retrieve the current IMU transformation information.
   *
   * @return the current IMU transformation information
   */
  pcl::PointCloud<pcl::PointXYZ>::Ptr getIMUTrans() { return _imuTrans; };


protected:
  /** \brief Prepare for next sweep: Reset internal cloud buffers and re-initialize start IMU state.
   *
   * @param scanTime the current scan time
   */
  void reset(const ros::Time& scanTime);

  /** \brief Project the given point to the start of the sweep, using the current IMU state and relative time.
   *
   * @param point the point to project
   * @param relTime the relative point measurement time
   */
  void transformToStartIMU(pcl::PointXYZI& point,
                           const float& relTime);

  /** \brief Extract features from current laser cloud.
   *
   * @param beginIdx the index of the first scan to extract features from
   */
  void extractFeatures(const uint16_t& beginIdx = 0);

  /** \brief Set up region buffers for the specified point range.
   *
   * @param startIdx the region start index
   * @param endIdx the region end index
   */
  void setRegionBuffersFor(const size_t& startIdx,
                           const size_t& endIdx);

  /** \brief Set up scan buffers for the specified point range.
   *
   * @param startIdx the scan start index
   * @param endIdx the scan start index
   */
  void setScanBuffersFor(const size_t& startIdx,
                         const size_t& endIdx);

  /** \brief Set sweep end IMU transformation information.
   *
   * @param sweepDuration the total duration of the current sweep
   */
  void setIMUTrans(const double& sweepDuration);

  /** \brief Generate a point cloud message for the specified cloud. */
  void generateROSMsg(sensor_msgs::PointCloud2& msg,
                      const pcl::PointCloud<pcl::PointXYZI>::Ptr& cloud);

  /** \brief Publish the current result via the respective topics. */
  void publishResult();


protected:
  const uint16_t _nScans;     ///< number of scans per sweep
  const float _scanPeriod;    ///< time per scan
  ros::Time _sweepStamp;      ///< time stamp of the beginning of current sweep
  RegistrationParams _config; ///< registration parameter

  IMUState _imuStart;                     ///< the interpolated IMU state corresponding to the start time of the currently processed laser scan
  IMUState _imuCur;                       ///< the interpolated IMU state corresponding to the time of the currently processed laser scan point
  size_t _imuStartIdx;                    ///< the index in the IMU history of the first IMU state received after the current scan time
  CircularBuffer<IMUState> _imuHistory;   ///< history of IMU states for cloud registration

  pcl::PointCloud<pcl::PointXYZI>::Ptr _laserCloud;   ///< full resolution input cloud
  std::vector<size_t> _scanStartIndices;              ///< start indices of the individual scans
  std::vector<size_t> _scanEndIndices;                ///< end indices of the individual scans

  pcl::PointCloud<pcl::PointXYZI>::Ptr _cornerPointsSharp;      ///< sharp corner points cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr _cornerPointsLessSharp;  ///< less sharp corner points cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr _surfacePointsFlat;      ///< flat surface points cloud
  pcl::PointCloud<pcl::PointXYZI>::Ptr _surfacePointsLessFlat;  ///< less flat surface points cloud
  pcl::PointCloud<pcl::PointXYZ>::Ptr _imuTrans;                ///< IMU transformation information

  std::vector<float> _regionCurvature;      ///< point curvature buffer
  std::vector<PointLabel> _regionLabel;     ///< point label buffer
  std::vector<size_t> _regionSortIndices;   ///< sorted region indices based on point curvature
  std::vector<int> _scanNeighborPicked;     ///< flag if neighboring point was already picked

  ros::Subscriber _subImu;    ///< IMU message subscriber

  ros::Publisher _pubLaserCloud;              ///< full resolution cloud message publisher
  ros::Publisher _pubCornerPointsSharp;       ///< sharp corner cloud message publisher
  ros::Publisher _pubCornerPointsLessSharp;   ///< less sharp corner cloud message publisher
  ros::Publisher _pubSurfPointsFlat;          ///< flat surface cloud message publisher
  ros::Publisher _pubSurfPointsLessFlat;      ///< less flat surface cloud message publisher
  ros::Publisher _pubImuTrans;                ///< IMU transformation message publisher
};

} // end namespace loam


#endif //LOAM_SCANREGISTRATION_H
