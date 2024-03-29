/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../mapping_2d/sparse_pose_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

#include "eigen3/Eigen/Eigenvalues"
#include "../common/make_unique.h"
#include "../common/math.h"
#include "../common/time.h"
#include "../sensor/compressed_point_cloud.h"
#include "../sensor/voxel_filter.h"
#include "cartographer/mapping/proto/scan_matching_progress.pb.h"
#include "cartographer/mapping/sparse_pose_graph/proto/constraint_builder_options.pb.h"

#include "glog/logging.h"

namespace cartographer {
namespace mapping_2d {

SparsePoseGraph::SparsePoseGraph(
    const mapping::proto::SparsePoseGraphOptions& options,
    common::ThreadPool* thread_pool,
    std::deque<mapping::TrajectoryNode::ConstantData>* constant_node_data)
    : options_(options),
      optimization_problem_(options_.optimization_problem_options()),
      constraint_builder_(options_.constraint_builder_options(), thread_pool),
      constant_node_data_(constant_node_data) {}

SparsePoseGraph::~SparsePoseGraph()
{
  WaitForAllComputations();
  common::MutexLocker locker(&mutex_);
  CHECK(scan_queue_ == nullptr);
}

void SparsePoseGraph::GrowSubmapTransformsAsNeeded(
    const std::vector<const mapping::Submap*>& submaps)
{cartographer
  CHECK(!submaps.empty());
  CHECK_LT(submap_transforms_.size(), std::numeric_limits<int>::max());
  const int next_transform_index = submap_transforms_.size();
  // Verify that we have an index for the first submap.
  const int first_transform_index = GetSubmapIndex(submaps[0]);
  if (submaps.size() == 1) {
    // If we don't already have an entry for this submap, add one.
    if (first_transform_index == next_transform_index) {
      submap_transforms_.push_back(transform::Rigid2d::Identity());
    }
    return;
  }
  CHECK_EQ(2, submaps.size());
  // CHECK that we have a index for the second submap.
  const int second_transform_index = GetSubmapIndex(submaps[1]);
  CHECK_LE(second_transform_index, next_transform_index);
  // Extrapolate if necessary.
  if (second_transform_index == next_transform_index) {
    const auto& first_submap_transform =
        submap_transforms_[first_transform_index];
    submap_transforms_.push_back(
        first_submap_transform *
        sparse_pose_graph::ComputeSubmapPose(*submaps[0]).inverse() *
        sparse_pose_graph::ComputeSubmapPose(*submaps[1]));
  }
}


/**
 * @brief SparsePoseGraph::AddScan
 * @param time                      激光数据对应的时间戳
 * @param tracking_to_pose          把激光数据转换到平面的转换矩阵 Marglization掉Yaw数据的转换矩阵
 * @param laser_fan_in_pose         平面的激光数据
 * @param pose                      滤波器估计的机器人位姿
 * @param covariance                滤波器估计的机器人位姿的协方差
 * @param submaps                   所有的submap
 * @param matching_submap           用来进行scan-matcha操作的submap
 * @param insertion_submaps         执行了插入操作的submap 就是submap(size-1) & submap(size-2)
 */
void SparsePoseGraph::AddScan(
    common::Time time,
    const transform::Rigid3d& tracking_to_pose,
    const sensor::LaserFan& laser_fan_in_pose,
    const transform::Rigid2d& pose,
    const kalman_filter::Pose2DCovariance& covariance,
    const mapping::Submaps* submaps,
    const mapping::Submap* const matching_submap,
    const std::vector<const mapping::Submap*>& insertion_submaps)
{
  //得到最优pose，  将trajectory_id下的TrajectoryNode中局部位姿转换到世界坐标系下
  const transform::Rigid3d optimized_pose(GetLocalToGlobalTransform(*submaps) *
                                          transform::Embed3D(pose));

  common::MutexLocker locker(&mutex_);
  const int j = trajectory_nodes_.size();     //  std::vector<mapping::TrajectoryNode> trajectory_nodes_ GUARDED_BY(mutex_);
  CHECK_LT(j, std::numeric_limits<int>::max());

  //生成一帧ConstanData 并且存储起来

  constant_node_data_->push_back(mapping::TrajectoryNode::ConstantData{
      time, laser_fan_in_pose,
      Compress(sensor::LaserFan3D{Eigen::Vector3f::Zero(), {}, {}, {}}),
      submaps, transform::Rigid3d(tracking_to_pose)});

  //在上面生成的ConstanData的基础上，生成一个路径节点，并把这个节点保存在轨迹trajectory_nodes_中。
  //用之前scan_match优化后的位姿添加一个新的轨迹点
  // mapping::NodeId包含一个轨迹ID和一个节点ID
  trajectory_nodes_.push_back(mapping::TrajectoryNode{
      &constant_node_data_->back(), optimized_pose,
  });

 // 增加一个没有任何连接的轨迹
  trajectory_connectivity_.Add(submaps);

  // 如果submap(size -1)没有分配下标，则为它分配下标
  // 如果刚刚被插入的那个submap是我们之前从未看过的，就更新submap_data_的数据
  // 也就是说submap_data_里面,包含了目前为止所有的submap
  if (submap_indices_.count(insertion_submaps.back()) == 0)
  {
    submap_indices_.emplace(insertion_submaps.back(),
                            static_cast<int>(submap_indices_.size()));
    submap_states_.emplace_back();

    submap_states_.back().submap = insertion_submaps.back();
    submap_states_.back().trajectory = submaps;
    CHECK_EQ(submap_states_.size(), submap_indices_.size());
  }

  //得到指向submap(size-2)的指针 ，如果submap(size-2)没有完成 则得到一个空指针
  const mapping::Submap* const finished_submap =
      insertion_submaps.front()->finished_probability_grid != nullptr
          ? insertion_submaps.front()
          : nullptr;

  //这下面进行pose-graph的构建　以及　回环检测操作
  // Make sure we have a sampler for this trajectory.
  if (!global_localization_samplers_[submaps])
  {
    global_localization_samplers_[submaps] =
        common::make_unique<common::FixedRatioSampler>(
            options_.global_sampling_ratio());
  }

  //为这帧新激光添加约束并开始在后台进行扫描匹配
  AddWorkItem(
      std::bind(std::mem_fn(&SparsePoseGraph::ComputeConstraintsForScan), this,
                j, submaps, matching_submap, insertion_submaps, finished_submap,
                pose, covariance));
}

//WorkItem传入的都是函数指针,这里实际上就是一个函数.
//这里只会在加入一个队列,在后台来运行
void SparsePoseGraph::AddWorkItem(std::function<void()> work_item)
{
  if (scan_queue_ == nullptr)
  {
    work_item();
  }
  else
  {
    scan_queue_->push_back(work_item);
  }
}

//为以前的scan，来计算和最近新完成的submap的约束．
void SparsePoseGraph::ComputeConstraintsForOldScans(
    const mapping::Submap* submap)
{
  const int submap_index = GetSubmapIndex(submap);

  CHECK_GT(point_cloud_poses_.size(), 0);
  CHECK_LT(point_cloud_poses_.size(), std::numeric_limits<int>::max());

  const int num_nodes = point_cloud_poses_.size();
  for (int scan_index = 0; scan_index < num_nodes; ++scan_index)
  {
    if (submap_states_[submap_index].scan_indices.count(scan_index) == 0)
    {
      const transform::Rigid2d relative_pose =
          submap_transforms_[submap_index].inverse() *
          point_cloud_poses_[scan_index];

      constraint_builder_.MaybeAddConstraint(
          submap_index, submap, scan_index,
          &trajectory_nodes_[scan_index].constant_data->laser_fan.point_cloud,
          relative_pose);
    }
  }
}

/**
 * @brief SparsePoseGraph::ComputeConstraintsForScan
 * 为激光数据帧scan计算约束
 * 同时也会进行回环检测
 *
// 为这帧新激光添加约束并开始在后台进行扫描匹配
// node_id新添加的轨迹点ID
// insertion_submaps插入激光帧的submap
// newly_finished_submap是否有新完成的submap
// 这是一个比较重要的函数,每次addScan()里面会调用这个函数来进行约束的计算.
// 约束的计算包括回环检测.
// 1.计算和insertion_submaps之间的约束
// 2.计算和每个submap之间的约束(可以认为这里是进行回环检测)
// 3.达成条件则进行优化
 *
 * @param scan_index            激光数据的下标 也就是代表了激光数据
 * @param scan_trajectory       所有的submap
 * @param matching_submap       用来进行scan-match操作的submap
 * @param insertion_submaps     执行了插入操作的submap 就是submap(size-1) & submap(size-2)。激光数据scan-index被插入到这些submap中
 * @param finished_submap       如果submap(size-2)已经finished，那就是submap(size-2) 否则就是null
 * @param pose
 * @param covariance
 **/
void SparsePoseGraph::ComputeConstraintsForScan(
    int scan_index, const mapping::Submaps* scan_trajectory,
    const mapping::Submap* matching_submap,
    std::vector<const mapping::Submap*> insertion_submaps,
    const mapping::Submap* finished_submap, const transform::Rigid2d& pose,
    const kalman_filter::Pose2DCovariance& covariance)
{

  GrowSubmapTransformsAsNeeded(insertion_submaps);

  const int matching_index = GetSubmapIndex(matching_submap);

  const transform::Rigid2d optimized_pose =
      submap_transforms_[matching_index] *
      sparse_pose_graph::ComputeSubmapPose(*matching_submap).inverse() * pose;

  CHECK_EQ(scan_index, point_cloud_poses_.size());
  initial_point_cloud_poses_.push_back(pose);
  point_cloud_poses_.push_back(optimized_pose);

  //枚举执行了插入操作的submap，即submap(size-1) & submap(size-2)
  //并为当前激光添加观测约束（因为当前激光帧刚刚匹配完成并插入到这两个submap中，所以是观测约束）
  //Constraint2D形式的约束都是观测约束
  for (const mapping::Submap* submap : insertion_submaps)
  {
    const int submap_index = GetSubmapIndex(submap);
    CHECK(!submap_states_[submap_index].finished);
    submap_states_[submap_index].scan_indices.emplace(scan_index);

    // Unchanged covariance as (submap <- map) is a translation.
    //更新submap_id里面的node
    const transform::Rigid2d constraint_transform =
        sparse_pose_graph::ComputeSubmapPose(*submap).inverse() * pose;

    //构建2D约束（所有的约束都是指观测值即观测约束） ****插入之后，先建立与submap(size-1) & submap(size-2)的约束关系
    constraints_.push_back(Constraint2D{
        submap_index,
        scan_index,
        {constraint_transform,
         common::ComputeSpdMatrixSqrtInverse(
             covariance, options_.constraint_builder_options()
                             .lower_covariance_eigenvalue_bound())},
        Constraint2D::INTRA_SUBMAP});
  }

  // Determine if this scan should be globally localized.

  //　接下来进行回环检测
  const bool run_global_localization =
      global_localization_samplers_[scan_trajectory]->Pulse();

  //枚举所有的submap　用当前的scan和所有finished的submap进行匹配来进行回环检测
  CHECK_LT(submap_states_.size(), std::numeric_limits<int>::max());
  const int num_submaps = submap_states_.size();
  for (int submap_index = 0; submap_index != num_submaps; ++submap_index)
  {
    //必须是已经finished　submap才用来进行匹配
    if (submap_states_[submap_index].finished)
    {
      CHECK_EQ(submap_states_[submap_index].scan_indices.count(scan_index), 0);
      const transform::Rigid2d relative_pose =
          submap_transforms_[submap_index].inverse() *
          point_cloud_poses_[scan_index];

      const auto* submap_trajectory = submap_states_[submap_index].trajectory;

      // Only globally match against submaps not in this trajectory.
      // 如果只跟不在这一条轨迹上的submap进行回环
      if (run_global_localization && scan_trajectory != submap_trajectory)
      {
        constraint_builder_.MaybeAddGlobalConstraint(
            submap_index, submap_states_[submap_index].submap, scan_index,
            scan_trajectory, submap_trajectory, &trajectory_connectivity_,
            &trajectory_nodes_[scan_index]
                 .constant_data->laser_fan.point_cloud);
      }
      else
      {
        const bool scan_and_submap_trajectories_connected =
            reverse_connected_components_.count(scan_trajectory) > 0 &&
            reverse_connected_components_.count(submap_trajectory) > 0 &&
            reverse_connected_components_.at(scan_trajectory) ==
            reverse_connected_components_.at(submap_trajectory);

        if (scan_trajectory == submap_trajectory ||
            scan_and_submap_trajectories_connected)
        {
          //进行约束计算,类似于计算回环检测
          constraint_builder_.MaybeAddConstraint(
              submap_index, submap_states_[submap_index].submap, scan_index,
              &trajectory_nodes_[scan_index]
                   .constant_data->laser_fan.point_cloud,
              relative_pose);
        }
      }
    }
  }

  //目前有了新的finished submap
  if (finished_submap != nullptr)
  {
    const int finished_submap_index = GetSubmapIndex(finished_submap);
    SubmapState& finished_submap_state = submap_states_[finished_submap_index];
    CHECK(!finished_submap_state.finished);
    // We have a new completed submap, so we look into adding constraints for
    // old scans.
    ComputeConstraintsForOldScans(finished_submap);
    finished_submap_state.finished = true;
  }


  constraint_builder_.NotifyEndOfScan(scan_index);
  ++num_scans_since_last_loop_closure_;

  //如果判断达成回环的条件，
  if (options_.optimize_every_n_scans() > 0 &&
      num_scans_since_last_loop_closure_ > options_.optimize_every_n_scans())
  {
    CHECK(!run_loop_closure_);
    run_loop_closure_ = true;
    // If there is a 'scan_queue_' already, some other thread will take care.
    if (scan_queue_ == nullptr)
    {
      scan_queue_ = common::make_unique<std::deque<std::function<void()>>>();
      HandleScanQueue();
    }
  }
}

//进行优化．最终优化的过程
void SparsePoseGraph::HandleScanQueue()
{
  constraint_builder_.WhenDone(
      [this](const sparse_pose_graph::ConstraintBuilder::Result& result)
    {
        constraints_.insert(constraints_.end(), result.begin(), result.end());
        
		RunOptimization();

        common::MutexLocker locker(&mutex_);
        num_scans_since_last_loop_closure_ = 0;
        run_loop_closure_ = false;
        while (!run_loop_closure_) 
		{
          if (scan_queue_->empty()) 
		  {
            LOG(INFO) << "We caught up. Hooray!";
            scan_queue_.reset();
            return;
          }
          scan_queue_->front()();
          scan_queue_->pop_front();
        }
        // We have to optimize again.
        HandleScanQueue();
      });
}

void SparsePoseGraph::WaitForAllComputations()
{
  bool notification = false;
  common::MutexLocker locker(&mutex_);
  const int num_finished_scans_at_start =
      constraint_builder_.GetNumFinishedScans();
  while (!locker.AwaitWithTimeout(
      [this]() REQUIRES(mutex_) {
        return static_cast<size_t>(constraint_builder_.GetNumFinishedScans()) ==
               trajectory_nodes_.size();
      },
      common::FromSeconds(1.)))
  {
    std::ostringstream progress_info;
    progress_info << "Optimizing: " << std::fixed << std::setprecision(1)
                  << 100. * (constraint_builder_.GetNumFinishedScans() -
                             num_finished_scans_at_start) /
                         (trajectory_nodes_.size() -
                          num_finished_scans_at_start)
                  << "%...";
    std::cout << "\r\x1b[K" << progress_info.str() << std::flush;
  }
  std::cout << "\r\x1b[KOptimizing: Done.     " << std::endl;
  constraint_builder_.WhenDone([this, &notification](
      const sparse_pose_graph::ConstraintBuilder::Result& result) {
    constraints_.insert(constraints_.end(), result.begin(), result.end());
    common::MutexLocker locker(&mutex_);
    notification = true;
  });

  locker.Await([&notification]() { return notification; });
}


void SparsePoseGraph::RunFinalOptimization()
{
  WaitForAllComputations();
  optimization_problem_.SetMaxNumIterations(
      options_.max_num_final_iterations());
  RunOptimization();
}

void SparsePoseGraph::RunOptimization()
{
  if (!submap_transforms_.empty())
  {
    std::vector<const mapping::Submaps*> trajectories;
    {
      common::MutexLocker locker(&mutex_);
      CHECK(!submap_states_.empty());
      for (const auto& trajectory_node : trajectory_nodes_)
      {
        trajectories.push_back(trajectory_node.constant_data->trajectory);
      }
    }

    optimization_problem_.Solve(constraints_, trajectories,
                                initial_point_cloud_poses_, &point_cloud_poses_,
                                &submap_transforms_);

    common::MutexLocker locker(&mutex_);
    has_new_optimized_poses_ = true;
    const size_t num_optimized_poses = point_cloud_poses_.size();

    for (size_t i = 0; i != num_optimized_poses; ++i)
    {
      trajectory_nodes_[i].pose =
          transform::Rigid3d(transform::Embed3D(point_cloud_poses_[i]));
    }

    // Extrapolate all point cloud poses that were added later.
    std::unordered_map<const mapping::Submaps*, transform::Rigid3d>
        extrapolation_transforms;

    for (size_t i = num_optimized_poses; i != trajectory_nodes_.size(); ++i)
    {
      const mapping::Submaps* trajectory =
          trajectory_nodes_[i].constant_data->trajectory;
      if (extrapolation_transforms.count(trajectory) == 0)
      {
        extrapolation_transforms[trajectory] = transform::Rigid3d(
            ExtrapolateSubmapTransforms(submap_transforms_, *trajectory)
                .back() *
            ExtrapolateSubmapTransforms(optimized_submap_transforms_,
                                        *trajectory)
                .back()
                .inverse());
      }
      trajectory_nodes_[i].pose =
          extrapolation_transforms[trajectory] * trajectory_nodes_[i].pose;
    }
    optimized_submap_transforms_ = submap_transforms_;
    connected_components_ = trajectory_connectivity_.ConnectedComponents();
    reverse_connected_components_.clear();
    for (size_t i = 0; i != connected_components_.size(); ++i)
    {
      for (const auto* trajectory : connected_components_[i])
      {
        reverse_connected_components_.emplace(trajectory, i);
      }
    }
  }
}

bool SparsePoseGraph::HasNewOptimizedPoses()
{
  common::MutexLocker locker(&mutex_);
  if (!has_new_optimized_poses_)
  {
    return false;
  }
  has_new_optimized_poses_ = false;
  return true;
}

mapping::proto::ScanMatchingProgress SparsePoseGraph::GetScanMatchingProgress()
{
  mapping::proto::ScanMatchingProgress progress;
  common::MutexLocker locker(&mutex_);
  progress.set_num_scans_total(trajectory_nodes_.size());
  progress.set_num_scans_finished(constraint_builder_.GetNumFinishedScans());
  return progress;
}

std::vector<mapping::TrajectoryNode> SparsePoseGraph::GetTrajectoryNodes()
{
  common::MutexLocker locker(&mutex_);
  return trajectory_nodes_;
}

std::vector<SparsePoseGraph::Constraint2D> SparsePoseGraph::constraints_2d()
{
  return constraints_;
}

std::vector<SparsePoseGraph::Constraint3D> SparsePoseGraph::constraints_3d()
{
  return {};
}

transform::Rigid3d SparsePoseGraph::GetLocalToGlobalTransform(
    const mapping::Submaps& submaps)
{
  return GetSubmapTransforms(submaps).back() *
         submaps.Get(submaps.size() - 1)->local_pose().inverse();
}

std::vector<std::vector<const mapping::Submaps*>>
SparsePoseGraph::GetConnectedTrajectories()
{
  common::MutexLocker locker(&mutex_);
  return connected_components_;
}

/**
* @brief SparsePoseGraph::GetSubmapTransforms
* @param submaps
* @return
*/
std::vector<transform::Rigid3d> SparsePoseGraph::GetSubmapTransforms(
    const mapping::Submaps& submaps)
{
  common::MutexLocker locker(&mutex_);
  return ExtrapolateSubmapTransforms(optimized_submap_transforms_, submaps);
}

/**
* @brief SparsePoseGraph::ExtrapolateSubmapTransforms
* 外推submap的转换
* @param submap_transforms
* @param submaps
* @return
*/
std::vector<transform::Rigid3d> SparsePoseGraph::ExtrapolateSubmapTransforms(   //推断
    const std::vector<transform::Rigid2d>& submap_transforms,
    const mapping::Submaps& submaps) const
{
  std::vector<transform::Rigid3d> result;
  int i = 0;
  // Submaps for which we have optimized poses.
  for (; i < submaps.size(); ++i)
  {
    const mapping::Submap* submap = submaps.Get(i);
    const auto submap_and_index = submap_indices_.find(submap);
    // Determine if we have a loop-closed transform for this submap.
    if (submap_and_index == submap_indices_.end() ||
        static_cast<size_t>(submap_and_index->second) >=
            submap_transforms.size())
    {
      break;
    }
    result.push_back(
        transform::Embed3D(submap_transforms[submap_and_index->second]));
  }

  // Extrapolate to the remaining submaps.
  for (; i < submaps.size(); ++i)
  {
    //第一个submap认为是原点 所以转换矩阵设置为I
    if (i == 0)
    {
      result.push_back(transform::Rigid3d::Identity());
      continue;
    }

    result.push_back(result.back() *
                     submaps.Get(result.size() - 1)->local_pose().inverse() *
                     submaps.Get(result.size())->local_pose());
  }
  return result;
}

}  // namespace mapping_2d
}  // namespace cartographer
