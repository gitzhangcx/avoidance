#include "local_planner/planner_functions.h"

#include "avoidance/common.h"

#include <ros/console.h>

#include <numeric>

namespace avoidance {

// trim the point cloud so that only points inside the bounding box are
// considered
void processPointcloud(pcl::PointCloud<pcl::PointXYZI>& final_cloud,
                       const std::vector<pcl::PointCloud<pcl::PointXYZ>>& complete_cloud, const Box& histogram_box,
                       const std::vector<FOV>& fov, float yaw_fcu_frame_deg, float pitch_fcu_frame_deg,
                       const Eigen::Vector3f& position, float min_realsense_dist, float max_age, float elapsed_s,
                       int min_num_points_per_cell) {
  pcl::PointCloud<pcl::PointXYZI> old_cloud;
  std::swap(final_cloud, old_cloud);
  final_cloud.points.clear();
  final_cloud.width = 0;
  final_cloud.points.reserve((2 * GRID_LENGTH_Z) * (2 * GRID_LENGTH_E));

  float distance;

  // counter to keep track of how many points lie in a given cell
  Eigen::MatrixXi histogram_points_counter(180 / (ALPHA_RES / 2), 360 / (ALPHA_RES / 2));
  histogram_points_counter.fill(0);

  for (const auto& cloud : complete_cloud) {
    for (const pcl::PointXYZ& xyz : cloud) {
      // Check if the point is invalid
      if (!std::isnan(xyz.x) && !std::isnan(xyz.y) && !std::isnan(xyz.z)) {
        if (histogram_box.isPointWithinBox(xyz.x, xyz.y, xyz.z)) {
          distance = (position - toEigen(xyz)).norm();
          if (distance > min_realsense_dist && distance < histogram_box.radius_) {
            // subsampling the cloud
            PolarPoint p_pol = cartesianToPolarHistogram(toEigen(xyz), position);
            Eigen::Vector2i p_ind = polarToHistogramIndex(p_pol, ALPHA_RES / 2);
            histogram_points_counter(p_ind.y(), p_ind.x())++;
            if (histogram_points_counter(p_ind.y(), p_ind.x()) == min_num_points_per_cell) {
              final_cloud.points.push_back(toXYZI(toEigen(xyz), 0.0f));
            }
          }
        }
      }
    }
  }

  // combine with old cloud
  for (const pcl::PointXYZI& xyzi : old_cloud) {
    // adding older points if not expired and space is free according to new
    // cloud
    if (histogram_box.isPointWithinBox(xyzi.x, xyzi.y, xyzi.z)) {
      distance = (position - toEigen(xyzi)).norm();
      if (distance < histogram_box.radius_) {
        PolarPoint p_pol = cartesianToPolarHistogram(toEigen(xyzi), position);
        PolarPoint p_pol_fcu = cartesianToPolarFCU(toEigen(xyzi), position);
        p_pol_fcu.e -= pitch_fcu_frame_deg;
        p_pol_fcu.z -= yaw_fcu_frame_deg;
        wrapPolar(p_pol_fcu);
        Eigen::Vector2i p_ind = polarToHistogramIndex(p_pol, ALPHA_RES / 2);

        // only remember point if it's in a cell not previously populated by
        // complete_cloud, as well as outside FOV and 'young' enough
        if (xyzi.intensity < max_age && !pointInsideFOV(fov, p_pol_fcu)) {
          final_cloud.points.push_back(toXYZI(toEigen(xyzi), xyzi.intensity + elapsed_s));

          // to indicate that this cell now has a point
          histogram_points_counter(p_ind.y(), p_ind.x()) = min_num_points_per_cell;
        }
      }
    }
  }

  final_cloud.header.stamp = complete_cloud[0].header.stamp;
  final_cloud.header.frame_id = complete_cloud[0].header.frame_id;
  final_cloud.height = 1;
  final_cloud.width = final_cloud.points.size();
}

// Generate new histogram from pointcloud
void generateNewHistogram(Histogram& polar_histogram, const pcl::PointCloud<pcl::PointXYZI>& cropped_cloud,
                          const Eigen::Vector3f& position) {
  Eigen::MatrixXi counter(GRID_LENGTH_E, GRID_LENGTH_Z);
  counter.fill(0);
  for (auto xyz : cropped_cloud) {
    Eigen::Vector3f p = toEigen(xyz);
    PolarPoint p_pol = cartesianToPolarHistogram(p, position);
    float dist = p_pol.r;
    Eigen::Vector2i p_ind = polarToHistogramIndex(p_pol, ALPHA_RES);

    counter(p_ind.y(), p_ind.x()) += 1;
    polar_histogram.set_dist(p_ind.y(), p_ind.x(), polar_histogram.get_dist(p_ind.y(), p_ind.x()) + dist);
  }

  // Normalize and get mean in distance bins
  for (int e = 0; e < GRID_LENGTH_E; e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      if (counter(e, z) > 0) {
        polar_histogram.set_dist(e, z, polar_histogram.get_dist(e, z) / counter(e, z));
      } else {
        polar_histogram.set_dist(e, z, 0.f);
      }
    }
  }
}

void compressHistogramElevation(Histogram& new_hist, const Histogram& input_hist) {
  float vertical_FOV_range_sensor = 20.0;
  PolarPoint p_pol_lower(-1.0f * vertical_FOV_range_sensor / 2.0f, 0.0f, 0.0f);
  PolarPoint p_pol_upper(vertical_FOV_range_sensor / 2.0f, 0.0f, 0.0f);
  Eigen::Vector2i p_ind_lower = polarToHistogramIndex(p_pol_lower, ALPHA_RES);
  Eigen::Vector2i p_ind_upper = polarToHistogramIndex(p_pol_upper, ALPHA_RES);

  for (int e = p_ind_lower.y(); e <= p_ind_upper.y(); e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      if (input_hist.get_dist(e, z) > 0) {
        if (input_hist.get_dist(e, z) < new_hist.get_dist(0, z) || (new_hist.get_dist(0, z) == 0.f))
          new_hist.set_dist(0, z, input_hist.get_dist(e, z));
      }
    }
  }
}

void getCostMatrix(const Histogram& histogram, const Eigen::Vector3f& goal, const Eigen::Vector3f& position,
                   const Eigen::Vector3f& velocity, const costParameters& cost_params, float smoothing_margin_degrees,
                   Eigen::MatrixXf& cost_matrix, Eigen::MatrixXf& distance_matrix,
                   Eigen::MatrixXf& velocity_cost_matrix, Eigen::MatrixXf& yaw_cost_matrix,
                   Eigen::MatrixXf& pitch_cost_matrix, std::vector<uint8_t>& image_data) {
  distance_matrix.resize(GRID_LENGTH_E, GRID_LENGTH_Z);
  velocity_cost_matrix.resize(GRID_LENGTH_E, GRID_LENGTH_Z);
  yaw_cost_matrix.resize(GRID_LENGTH_E, GRID_LENGTH_Z);
  pitch_cost_matrix.resize(GRID_LENGTH_E, GRID_LENGTH_Z);

  distance_matrix.fill(NAN);
  velocity_cost_matrix.fill(NAN);
  yaw_cost_matrix.fill(NAN);
  pitch_cost_matrix.fill(NAN);

  // reset cost matrix to zero
  cost_matrix.resize(GRID_LENGTH_E, GRID_LENGTH_Z);
  cost_matrix.fill(NAN);

  // fill in cost matrix
  for (int e_index = 0; e_index < GRID_LENGTH_E; e_index++) {
    // determine how many bins at this elevation angle would be equivalent to
    // a single bin at horizontal, then work in steps of that size
    const float bin_width = std::cos(histogramIndexToPolar(e_index, 0, ALPHA_RES, 1).e * DEG_TO_RAD);
    const int step_size = static_cast<int>(std::round(1 / bin_width));

    for (int z_index = 0; z_index < GRID_LENGTH_Z; z_index += step_size) {
      float obstacle_distance = histogram.get_dist(e_index, z_index);
      PolarPoint p_pol = histogramIndexToPolar(e_index, z_index, ALPHA_RES, 1.0f);  // unit vector of current direction

      std::vector<float> costs = costFunction(p_pol, obstacle_distance, goal, position, velocity, cost_params);
      cost_matrix(e_index, z_index) = costs[1];
      distance_matrix(e_index, z_index) = costs[0];
      velocity_cost_matrix(e_index, z_index) = costs[2];
      yaw_cost_matrix(e_index, z_index) = costs[3];
      pitch_cost_matrix(e_index, z_index) = costs[4];
    }
    if (step_size > 1) {
      // horizontally interpolate all of the un-calculated values
      int last_index = 0;
      for (int z_index = step_size; z_index < GRID_LENGTH_Z; z_index += step_size) {
        float other_costs_gradient = (cost_matrix(e_index, z_index) - cost_matrix(e_index, last_index)) / step_size;
        float distance_cost_gradient =
            (distance_matrix(e_index, z_index) - distance_matrix(e_index, last_index)) / step_size;
        float velocity_cost_gradient =
            (velocity_cost_matrix(e_index, z_index) - velocity_cost_matrix(e_index, last_index)) / step_size;
        float yaw_cost_gradient =
            (yaw_cost_matrix(e_index, z_index) - yaw_cost_matrix(e_index, last_index)) / step_size;
        float pitch_cost_gradient =
            (pitch_cost_matrix(e_index, z_index) - pitch_cost_matrix(e_index, last_index)) / step_size;
        for (int i = 1; i < step_size; i++) {
          cost_matrix(e_index, last_index + i) = cost_matrix(e_index, last_index) + other_costs_gradient * i;
          distance_matrix(e_index, last_index + i) = distance_matrix(e_index, last_index) + distance_cost_gradient * i;
          velocity_cost_matrix(e_index, last_index + i) =
              velocity_cost_matrix(e_index, last_index) + velocity_cost_gradient * i;
          yaw_cost_matrix(e_index, last_index + i) = yaw_cost_matrix(e_index, last_index) + yaw_cost_gradient * i;
          pitch_cost_matrix(e_index, last_index + i) = pitch_cost_matrix(e_index, last_index) + pitch_cost_gradient * i;
        }
        last_index = z_index;
      }

      // special case the last columns wrapping around back to 0
      int clamped_z_scale = GRID_LENGTH_Z - last_index;
      float other_costs_gradient = (cost_matrix(e_index, 0) - cost_matrix(e_index, last_index)) / clamped_z_scale;
      float distance_cost_gradient =
          (distance_matrix(e_index, 0) - distance_matrix(e_index, last_index)) / clamped_z_scale;
      float velocity_cost_gradient =
          (velocity_cost_matrix(e_index, 0) - velocity_cost_matrix(e_index, last_index)) / clamped_z_scale;
      float yaw_cost_gradient = (yaw_cost_matrix(e_index, 0) - yaw_cost_matrix(e_index, last_index)) / clamped_z_scale;
      float pitch_cost_gradient =
          (pitch_cost_matrix(e_index, 0) - pitch_cost_matrix(e_index, last_index)) / clamped_z_scale;

      for (int i = 1; i < clamped_z_scale; i++) {
        cost_matrix(e_index, last_index + i) = cost_matrix(e_index, last_index) + other_costs_gradient * i;
        distance_matrix(e_index, last_index + i) = distance_matrix(e_index, last_index) + distance_cost_gradient * i;
        velocity_cost_matrix(e_index, last_index + i) =
            velocity_cost_matrix(e_index, last_index) + velocity_cost_gradient * i;
        yaw_cost_matrix(e_index, last_index + i) = yaw_cost_matrix(e_index, last_index) + yaw_cost_gradient * i;
        pitch_cost_matrix(e_index, last_index + i) = pitch_cost_matrix(e_index, last_index) + pitch_cost_gradient * i;
      }
    }
  }

  unsigned int smooth_radius = ceil(smoothing_margin_degrees / ALPHA_RES);
  smoothPolarMatrix(distance_matrix, smooth_radius);

  generateCostImage(cost_matrix, distance_matrix, image_data);
  cost_matrix = cost_matrix + distance_matrix;
}

void generateCostImage(const Eigen::MatrixXf& cost_matrix, const Eigen::MatrixXf& distance_matrix,
                       std::vector<uint8_t>& image_data) {
  float max_val = std::max(cost_matrix.maxCoeff(), distance_matrix.maxCoeff());
  image_data.clear();
  image_data.reserve(3 * GRID_LENGTH_E * GRID_LENGTH_Z);

  for (int e = GRID_LENGTH_E - 1; e >= 0; e--) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      float distance_cost = 255.f * distance_matrix(e, z) / max_val;
      float other_cost = 255.f * cost_matrix(e, z) / max_val;
      image_data.push_back(static_cast<uint8_t>(std::max(0.0f, std::min(255.f, distance_cost))));
      image_data.push_back(static_cast<uint8_t>(std::max(0.0f, std::min(255.f, other_cost))));
      image_data.push_back(0);
    }
  }
}

int colorImageIndex(int e_ind, int z_ind, int color) {
  // color = 0 (red), color = 1 (green), color = 2 (blue)
  return ((GRID_LENGTH_E - e_ind - 1) * GRID_LENGTH_Z + z_ind) * 3 + color;
}

void getBestCandidatesFromCostMatrix(const Eigen::MatrixXf& total_cost_matrix,
                                     const Eigen::MatrixXf& distance_cost_matrix,
                                     const Eigen::MatrixXf& velocity_cost_matrix,
                                     const Eigen::MatrixXf& yaw_cost_matrix, const Eigen::MatrixXf& pitch_cost_matrix,
                                     unsigned int number_of_candidates,
                                     std::vector<candidateDirection>& candidate_vector) {
  std::priority_queue<candidateDirection, std::vector<candidateDirection>, std::less<candidateDirection>> queue;

  for (int row_index = 0; row_index < total_cost_matrix.rows(); row_index++) {
    for (int col_index = 0; col_index < total_cost_matrix.cols(); col_index++) {
      PolarPoint p_pol = histogramIndexToPolar(row_index, col_index, ALPHA_RES, 1.0);
      float cost = total_cost_matrix(row_index, col_index);
      candidateDirection candidate(cost, distance_cost_matrix(row_index, col_index),
                                   velocity_cost_matrix(row_index, col_index), yaw_cost_matrix(row_index, col_index),
                                   pitch_cost_matrix(row_index, col_index), p_pol.e, p_pol.z);

      if (queue.size() < number_of_candidates) {
        queue.push(candidate);
      } else if (candidate < queue.top()) {
        queue.push(candidate);
        queue.pop();
      }
    }
  }
  // copy queue to vector and change order such that lowest cost is at the
  // front
  candidate_vector.clear();
  candidate_vector.reserve(queue.size());
  while (!queue.empty()) {
    candidate_vector.push_back(queue.top());
    queue.pop();
  }
  std::reverse(candidate_vector.begin(), candidate_vector.end());
}

void smoothPolarMatrix(Eigen::MatrixXf& matrix, unsigned int smoothing_radius) {
  // pad matrix by smoothing radius respecting all wrapping rules
  Eigen::MatrixXf matrix_padded;
  padPolarMatrix(matrix, smoothing_radius, matrix_padded);
  Eigen::ArrayXf kernel1d = getConicKernel(smoothing_radius);

  Eigen::ArrayXf temp_col(matrix_padded.rows());
  for (int col_index = 0; col_index < matrix_padded.cols(); col_index++) {
    temp_col = matrix_padded.col(col_index);
    for (int row_index = 0; row_index < matrix.rows(); row_index++) {
      float smooth_val = (temp_col.segment(row_index, 2 * smoothing_radius + 1) * kernel1d).sum();
      matrix_padded(row_index + smoothing_radius, col_index) = smooth_val;
    }
  }

  Eigen::ArrayXf temp_row(matrix_padded.cols());
  for (int row_index = 0; row_index < matrix.rows(); row_index++) {
    temp_row = matrix_padded.row(row_index + smoothing_radius);
    for (int col_index = 0; col_index < matrix.cols(); col_index++) {
      float smooth_val = (temp_row.segment(col_index, 2 * smoothing_radius + 1) * kernel1d).sum();
      matrix(row_index, col_index) = smooth_val;
    }
  }
}

Eigen::ArrayXf getConicKernel(int radius) {
  Eigen::ArrayXf kernel(radius * 2 + 1);
  for (int row = 0; row < kernel.rows(); row++) {
    kernel(row) = std::max(0.f, 1.f + radius - std::abs(row - radius));
  }

  kernel *= 1.f / kernel.maxCoeff();
  return kernel;
}

void padPolarMatrix(const Eigen::MatrixXf& matrix, unsigned int n_lines_padding, Eigen::MatrixXf& matrix_padded) {
  matrix_padded.resize(matrix.rows() + 2 * n_lines_padding, matrix.cols() + 2 * n_lines_padding);

  matrix_padded.fill(0.0);
  // middle part
  matrix_padded.block(n_lines_padding, n_lines_padding, matrix.rows(), matrix.cols()) = matrix;

  if (matrix.cols() % 2 > 0) {
    ROS_ERROR("invalid resolution: 180 mod (2* resolution) must be zero");
  }
  int middle_index = floor(matrix.cols() / 2);

  // top border
  matrix_padded.block(0, n_lines_padding, n_lines_padding, middle_index) =
      matrix.block(0, middle_index, n_lines_padding, middle_index).colwise().reverse();
  matrix_padded.block(0, n_lines_padding + middle_index, n_lines_padding, middle_index) =
      matrix.block(0, 0, n_lines_padding, middle_index).colwise().reverse();

  // bottom border
  matrix_padded.block(matrix.rows() + n_lines_padding, n_lines_padding, n_lines_padding, middle_index) =
      matrix.block(matrix.rows() - n_lines_padding, middle_index, n_lines_padding, middle_index).colwise().reverse();
  matrix_padded.block(matrix.rows() + n_lines_padding, n_lines_padding + middle_index, n_lines_padding, middle_index) =
      matrix.block(matrix.rows() - n_lines_padding, 0, n_lines_padding, middle_index).colwise().reverse();

  // left border
  matrix_padded.block(0, 0, matrix_padded.rows(), n_lines_padding) =
      matrix_padded.block(0, matrix_padded.cols() - 2 * n_lines_padding, matrix_padded.rows(), n_lines_padding);
  // right border
  matrix_padded.block(0, n_lines_padding + matrix.cols(), matrix_padded.rows(), n_lines_padding) =
      matrix_padded.block(0, n_lines_padding, matrix_padded.rows(), n_lines_padding);
}

// costfunction for every free histogram cell
std::vector<float> costFunction(const PolarPoint& candidate_polar, float obstacle_distance, const Eigen::Vector3f& goal,
                                const Eigen::Vector3f& position, const Eigen::Vector3f& velocity,
                                const costParameters& cost_params) {
  // Compute  polar direction to goal and cartesian representation of current direction to evaluate
  const PolarPoint facing_goal = cartesianToPolarHistogram(goal, position);
  const Eigen::Vector3f candidate_velocity_cartesian =
      polarHistogramToCartesian(candidate_polar, Eigen::Vector3f(0.0f, 0.0f, 0.0f));

  const float velocity_cost =
      cost_params.velocity_cost_param * (velocity.norm() - candidate_velocity_cartesian.normalized().dot(velocity));
  const float yaw_cost =
      cost_params.yaw_cost_param * (candidate_polar.z - facing_goal.z) * (candidate_polar.z - facing_goal.z);
  const float pitch_cost =
      cost_params.pitch_cost_param * (candidate_polar.e - facing_goal.e) * (candidate_polar.e - facing_goal.e);
  const float d = cost_params.obstacle_cost_param - obstacle_distance;
  const float distance_cost = obstacle_distance > 0 ? 5000.0f * (1 + d / sqrt(1 + d * d)) : 0.0f;
  std::vector<float> ret{distance_cost, velocity_cost + yaw_cost + pitch_cost, velocity_cost, yaw_cost, pitch_cost};
  return ret;
}

bool getSetpointFromPath(const std::vector<Eigen::Vector3f>& path, const ros::Time& path_generation_time,
                         float velocity, Eigen::Vector3f& setpoint) {
  int i = path.size();
  // path contains nothing meaningful
  if (i < 2) {
    return false;
  }

  // path only has one segment: return end of that segment as setpoint
  if (i == 2) {
    setpoint = path[0];
    return true;
  }

  // step through the path until the point where we should be if we had traveled perfectly with velocity along it
  Eigen::Vector3f path_segment = path[i - 3] - path[i - 2];
  float distance_left = (ros::Time::now() - path_generation_time).toSec() * velocity;
  setpoint = path[i - 2] + (distance_left / path_segment.norm()) * path_segment;

  for (i = path.size() - 3; i > 0 && distance_left > path_segment.norm(); --i) {
    distance_left -= path_segment.norm();
    path_segment = path[i - 1] - path[i];
    setpoint = path[i] + (distance_left / path_segment.norm()) * path_segment;
  }

  // If we excited because we're past the last node of the path, the path is no longer valid!
  return distance_left < path_segment.norm();
}

void printHistogram(Histogram& histogram) {
  std::cout << "------------------------------------------Histogram------------------------------------------------\n";
  for (int e = 0; e < GRID_LENGTH_E; e++) {
    for (int z = 0; z < GRID_LENGTH_Z; z++) {
      int val = floor(histogram.get_dist(e, z));
      if (val > 99) {
        std::cout << val << " ";
      } else if (val > 9) {
        std::cout << val << "  ";
      } else {
        std::cout << val << "   ";
      }
    }
    std::cout << "\n";
  }
  std::cout << "___________________________________________________________________________________________________\n";
}
}
