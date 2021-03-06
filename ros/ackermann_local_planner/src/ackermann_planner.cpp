/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
* Author: Austin Hendrix
*********************************************************************/
#include <ackermann_local_planner/ackermann_planner.h>
#include <angles/angles.h>

namespace ackermann_local_planner {
  void AckermannPlanner::reconfigureCB(AckermannPlannerConfig &config, uint32_t level)
  {
    if(setup_ && config.restore_defaults) {
      config = default_config_;
      config.restore_defaults = false;
    }

    if(!setup_) {
      default_config_ = config;
      setup_ = true;
    }
    boost::mutex::scoped_lock l(configuration_mutex_);
 
    max_vel_x_ = config.max_vel_x;
    min_vel_x_ = config.min_vel_x;
 
    min_radius_ = config.min_radius;
 
    sim_time_ = config.sim_time;
    sim_granularity_ = config.sim_granularity;
    pdist_scale_ = config.path_distance_bias;
    gdist_scale_ = config.goal_distance_bias;
    occdist_scale_ = config.occdist_scale;
 
    stop_time_buffer_ = config.stop_time_buffer;
    oscillation_reset_dist_ = config.oscillation_reset_dist;
    forward_point_distance_ = config.forward_point_distance;
 
    scaling_speed_ = config.scaling_speed;
    max_scaling_factor_ = config.max_scaling_factor;
 
    int vx_samp, radius_samp;
    vx_samp = config.vx_samples;
    radius_samp = config.radius_samples;
 
    if(vx_samp <= 0){
      ROS_WARN("You've specified that you don't want any samples in the x dimension. We'll at least assume that you want to sample one value... so we're going to set vx_samples to 1 instead");
      vx_samp = 1;
      config.vx_samples = vx_samp;
    }
 
    if(radius_samp <= 0){
      ROS_WARN("You've specified that you don't want any samples in the th dimension. We'll at least assume that you want to sample one value... so we're going to set vth_samples to 1 instead");
      radius_samp = 1;
      config.radius_samples = radius_samp;
    }
 
    vsamples_[0] = vx_samp;
    //vsamples_[1] = vy_samp;
    vsamples_[2] = radius_samp;
 
    penalize_negative_x_ = config.penalize_negative_x;
  }

  AckermannPlanner::AckermannPlanner(std::string name, 
      costmap_2d::Costmap2DROS* costmap_ros) : 
        costmap_ros_(NULL), world_model_(NULL), 
        dsrv_(ros::NodeHandle("~/" + name)), 
        setup_(false), 
        penalize_negative_x_(true) {
    costmap_ros_ = costmap_ros;
    costmap_ros_->getCostmapCopy(costmap_);

    map_ = base_local_planner::MapGrid(costmap_.getSizeInCellsX(),
        costmap_.getSizeInCellsY(), costmap_.getResolution(), 
        costmap_.getOriginX(), costmap_.getOriginY());

    front_map_ = base_local_planner::MapGrid(costmap_.getSizeInCellsX(),
        costmap_.getSizeInCellsY(), costmap_.getResolution(), 
        costmap_.getOriginX(), costmap_.getOriginY());

    ros::NodeHandle pn("~/" + name);

    double acc_lim_x;
    pn.param("acc_lim_x", acc_lim_x, 2.5);

    //Assuming this planner is being run within the navigation stack, we can
    //just do an upward search for the frequency at which its being run. This
    //also allows the frequency to be overwritten locally.
    std::string controller_frequency_param_name;
    if(!pn.searchParam("controller_frequency", controller_frequency_param_name))
      sim_period_ = 0.05;
    else
    {
      double controller_frequency = 0;
      pn.param(controller_frequency_param_name, controller_frequency, 20.0);
      if(controller_frequency > 0)
        sim_period_ = 1.0 / controller_frequency;
      else
      {
        ROS_WARN("A controller_frequency less than 0 has been set. Ignoring the parameter, assuming a rate of 20Hz");
        sim_period_ = 0.05;
      }
    }
    ROS_INFO("Sim period is set to %.2f", sim_period_);

    acc_lim_[0] = acc_lim_x;

    dynamic_reconfigure::Server<AckermannPlannerConfig>::CallbackType cb = boost::bind(&AckermannPlanner::reconfigureCB, this, _1, _2);
    dsrv_.setCallback(cb);

    footprint_spec_ = costmap_ros_->getRobotFootprint();

    world_model_ = new base_local_planner::CostmapModel(costmap_);

    prev_stationary_pos_ = Eigen::Vector3f::Zero();
    resetOscillationFlags();

    map_viz_.initialize(name, &costmap_, boost::bind(&AckermannPlanner::getCellCosts, this, _1, _2, _3, _4, _5, _6));
  }

  bool AckermannPlanner::getCellCosts(int cx, int cy, float &path_cost, float &goal_cost, float &occ_cost, float &total_cost) {
    base_local_planner::MapCell cell = map_(cx, cy);
    if (cell.within_robot) {
        return false;
    }
    occ_cost = costmap_.getCost(cx, cy);
    if (cell.path_dist >= map_.map_.size() || cell.goal_dist >= map_.map_.size() || occ_cost >= costmap_2d::INSCRIBED_INFLATED_OBSTACLE) {
        return false;
    }
    path_cost = cell.path_dist;
    goal_cost = cell.goal_dist;

    double resolution = costmap_.getResolution();
    total_cost = pdist_scale_ * resolution * path_cost + gdist_scale_ * resolution * goal_cost + occdist_scale_ * occ_cost;
    return true;
  }

  Eigen::Vector3f AckermannPlanner::computeNewPositions(const Eigen::Vector3f& pos, 
      const Eigen::Vector3f& vel, double dt){
    Eigen::Vector3f new_pos = Eigen::Vector3f::Zero();
    new_pos[0] = pos[0] + (vel[0] * cos(pos[2])) * dt;
    new_pos[1] = pos[1] + (vel[0] * sin(pos[2])) * dt;
    new_pos[2] = pos[2] + vel[2] * dt;
    return new_pos;
  }
  
  void AckermannPlanner::selectBestTrajectory(base_local_planner::Trajectory* &best, base_local_planner::Trajectory* &comp){
    //check if the comp trajectory is better than the current best and, if so, swap them
    bool best_valid = best->cost_ >= 0.0;
    bool best_forward = best->xv_ >= 0.0;
    bool comp_valid = comp->cost_ >= 0.0;
    bool comp_forward = comp->xv_ >= 0.0;

    //if we don't have a valid trajecotry... then do nothing
    if(!comp_valid)
      return;

    ////check to see if we don't want to score a trajectory that is penalized bc it is negative
    if(penalize_negative_x_ && best_valid && best_forward && !comp_forward)
      return;


    if(comp_valid && ((comp->cost_ < best->cost_ || !best_valid) || (penalize_negative_x_ && comp_forward && !best_forward))){
      base_local_planner::Trajectory* swap = best;
      best = comp;
      comp = swap;
    }
  }

  bool AckermannPlanner::oscillationCheck(const Eigen::Vector3f& vel){
    if(forward_pos_only_ && vel[0] < 0.0)
      return true;

    if(forward_neg_only_ && vel[0] > 0.0)
      return true;

    if(rot_pos_only_ && vel[2] < 0.0)
      return true;

    if(rot_neg_only_ && vel[2] > 0.0)
      return true;

    return false;
  }

  base_local_planner::Trajectory AckermannPlanner::computeTrajectories(const Eigen::Vector3f& pos, const Eigen::Vector3f& vel){
    tf::Stamped<tf::Pose> robot_pose_tf;
    geometry_msgs::PoseStamped robot_pose;

    //compute the distance between the robot and the last point on the
    // global_plan
    costmap_ros_->getRobotPose(robot_pose_tf);
    tf::poseStampedTFToMsg(robot_pose_tf, robot_pose);

    double sq_dist = squareDist(robot_pose, global_plan_.back());

    bool two_point_scoring = true;
    if(sq_dist < forward_point_distance_ * forward_point_distance_)
      two_point_scoring = false;

    //compute the feasible velocity space based on the rate at which we run
    // absolute maximum velocity limits
    double max_vel = std::min(max_vel_x_, vel[0] + acc_lim_[0] * sim_period_);
    double min_vel = std::max(-max_vel_x_, vel[0] - acc_lim_[0] * sim_period_);

    // ensure minimum velocity is met
    if( max_vel < 0 && max_vel > -min_vel_x_ ) {
      max_vel = -min_vel_x_;
    }
    if( max_vel > 0 && max_vel < min_vel_x_ ) {
      max_vel = min_vel_x_;
    }

    if( min_vel > 0 && min_vel < min_vel_x_ ) {
      min_vel = min_vel_x_;
    }
    if( min_vel < 0 && min_vel > -min_vel_x_ ) {
      min_vel = -min_vel_x_;
    }

    //we want to sample the velocity space regularly
    double dv = (max_vel - min_vel) / (std::max(1.0, double(vsamples_[0]) - 1));

    //keep track of the best trajectory seen so far... we'll re-use two 
    // memeber vars for efficiency
    base_local_planner::Trajectory* best_traj = &traj_one_;
    best_traj->cost_ = -1.0;

    base_local_planner::Trajectory* comp_traj = &traj_two_;
    comp_traj->cost_ = -1.0;

    Eigen::Vector3f vel_samp = Eigen::Vector3f::Zero();

    // try the trajectory with velocity 0
    generateTrajectory(pos, vel_samp, *comp_traj, two_point_scoring);
    selectBestTrajectory(best_traj, comp_traj);

    // try trajectories with nonzero velocities
    for(VelocityIterator x_it(min_vel, max_vel, dv); !x_it.isFinished(); x_it++){
      vel_samp[0] = x_it.getVelocity();
      // ensure that minimum velocity limit is met
      if( vel_samp[0] > 0 && vel_samp[0] <  min_vel_x_ ) 
        vel_samp[0] =  min_vel_x_;
      if( vel_samp[0] < 0 && vel_samp[0] > -min_vel_x_ ) 
        vel_samp[0] = -min_vel_x_;

      vel_samp[1] = 0;
      // compute min and max radial velocity based on minimum turning radius 
      // and speed
      double max_theta = vel_samp[0] / min_radius_; // theta = d / r
      double dt = (max_theta*2) / (std::max(1.0, double(vsamples_[2]) - 1));
      for(VelocityIterator th_it(-max_theta, max_theta, dt); !th_it.isFinished(); th_it++){
        vel_samp[2] = th_it.getVelocity();
        generateTrajectory(pos, vel_samp, *comp_traj, two_point_scoring);
        selectBestTrajectory(best_traj, comp_traj);
      }
    }

    ROS_DEBUG_NAMED("oscillation_flags", "forward_pos_only: %d, forward_neg_only: %d, strafe_pos_only: %d, strafe_neg_only: %d, rot_pos_only: %d, rot_neg_only: %d",
        forward_pos_only_, forward_neg_only_, strafe_pos_only_, strafe_neg_only_, rot_pos_only_, rot_neg_only_);

    //ok... now we have our best trajectory
    if(best_traj->cost_ >= 0){
      //we want to check if we need to set any oscillation flags
      if(setOscillationFlags(best_traj)){
        prev_stationary_pos_ = pos;
      }

      //if we've got restrictions... check if we can reset any oscillation flags
      if(forward_pos_only_ || forward_neg_only_ 
          || strafe_pos_only_ || strafe_neg_only_
          || rot_pos_only_ || rot_neg_only_){
        resetOscillationFlagsIfPossible(pos, prev_stationary_pos_);
      }
    }

    //TODO: Think about whether we want to try to do things like back up when a valid trajectory is not found

    return *best_traj;

  }

  void AckermannPlanner::resetOscillationFlagsIfPossible(const Eigen::Vector3f& pos, const Eigen::Vector3f& prev){
    double x_diff = pos[0] - prev[0];
    double y_diff = pos[1] - prev[1];
    double sq_dist = x_diff * x_diff + y_diff * y_diff;

    //if we've moved far enough... we can reset our flags
    if(sq_dist > oscillation_reset_dist_ * oscillation_reset_dist_){
      resetOscillationFlags();
    }
  }

  void AckermannPlanner::resetOscillationFlags(){
    strafe_pos_only_ = false;
    strafe_neg_only_ = false;
    strafing_pos_ = false;
    strafing_neg_ = false;

    rot_pos_only_ = false;
    rot_neg_only_ = false;
    rotating_pos_ = false;
    rotating_neg_ = false;

    forward_pos_only_ = false;
    forward_neg_only_ = false;
    forward_pos_ = false;
    forward_neg_ = false;
  }
  
  bool AckermannPlanner::setOscillationFlags(base_local_planner::Trajectory* t){
    bool flag_set = false;
    //set oscillation flags for moving forward and backward
    if(t->xv_ < 0.0){
      if(forward_pos_){
        forward_neg_only_ = true;
        flag_set = true;
      }
      forward_pos_ = false;
      forward_neg_ = true;
    }
    if(t->xv_ > 0.0){
      if(forward_neg_){
        forward_pos_only_ = true;
        flag_set = true;
      }
      forward_neg_ = false;
      forward_pos_ = true;
    }
    return flag_set;
  }

  double AckermannPlanner::footprintCost(const Eigen::Vector3f& pos, double scale){
    double cos_th = cos(pos[2]);
    double sin_th = sin(pos[2]);

    std::vector<geometry_msgs::Point> scaled_oriented_footprint;
    for(unsigned int i  = 0; i < footprint_spec_.size(); ++i){
      geometry_msgs::Point new_pt;
      new_pt.x = pos[0] + (scale * footprint_spec_[i].x * cos_th - scale * footprint_spec_[i].y * sin_th);
      new_pt.y = pos[1] + (scale * footprint_spec_[i].x * sin_th + scale * footprint_spec_[i].y * cos_th);
      scaled_oriented_footprint.push_back(new_pt);
    }

    geometry_msgs::Point robot_position;
    robot_position.x = pos[0];
    robot_position.y = pos[1];

    //check if the footprint is legal
    double footprint_cost = world_model_->footprintCost(robot_position, scaled_oriented_footprint, costmap_.getInscribedRadius(), costmap_.getCircumscribedRadius());

    return footprint_cost;
  }

  void AckermannPlanner::generateTrajectory(Eigen::Vector3f pos, const Eigen::Vector3f& vel, base_local_planner::Trajectory& traj, bool two_point_scoring){
    //ROS_ERROR("%.2f, %.2f, %.2f - %.2f %.2f", vel[0], vel[1], vel[2], sim_time_, sim_granularity_);
    double impossible_cost = map_.map_.size();

    double vmag = vel[0];

    //compute the number of steps we must take along this trajectory to be "safe"
    int num_steps = ceil(std::max((vmag * sim_time_) / sim_granularity_, fabs(vel[2]) / sim_granularity_));

    //compute a timestep
    double dt = sim_time_ / num_steps;
    double time = 0.0;

    //initialize the costs for the trajectory
    double path_dist = 0.0;
    double goal_dist = 0.0;
    double occ_cost = 0.0;

    //we'll also be scoring a point infront of the robot
    double front_path_dist = 0.0;
    double front_goal_dist = 0.0;

    //create a potential trajectory... it might be reused so we'll make sure to reset it
    traj.resetPoints();
    traj.xv_ = vel[0];
    traj.yv_ = vel[1];
    traj.thetav_ = vel[2];
    traj.cost_ = -1.0;

    //if we're not actualy going to simulate... we may as well just return now
    if(num_steps == 0){
      traj.cost_ = -1.0;
      return;
    }

    //we want to check against the absolute value of the velocities for collisions later
    Eigen::Vector3f abs_vel = vel.array().abs();

    //simulate the trajectory and check for collisions, updating costs along the way
    for(int i = 0; i < num_steps; ++i){
      //get the mapp coordinates of a point
      unsigned int cell_x, cell_y;

      //we won't allow trajectories that go off the map... shouldn't happen that often anyways
      if(!costmap_.worldToMap(pos[0], pos[1], cell_x, cell_y)){
        //we're off the map
        traj.cost_ = -1.0;
        return;
      }

      double front_x = pos[0] + forward_point_distance_ * cos(pos[2]);
      double front_y = pos[1] + forward_point_distance_ * sin(pos[2]);

      unsigned int front_cell_x, front_cell_y;
      //we won't allow trajectories that go off the map... shouldn't happen that often anyways
      if(!costmap_.worldToMap(front_x, front_y, front_cell_x, front_cell_y)){
        //we're off the map
        traj.cost_ = -1.0;
        return;
      }


      //if we're over a certain speed threshold, we'll scale the robot's
      //footprint to make it either slow down or stay further from walls
      double scale = 1.0;
      if(vmag > scaling_speed_){
        //scale up to the max scaling factor linearly... this could be changed later
        double ratio = (vmag - scaling_speed_) / (max_vel_x_ - scaling_speed_);
        scale = max_scaling_factor_ * ratio + 1.0;
      }

      //we want to find the cost of the footprint
      double footprint_cost = footprintCost(pos, scale);

      //if the footprint hits an obstacle... we'll check if we can stop before we hit it... given the time to get there
      if(footprint_cost < 0){
        traj.cost_ = -1.0;
        return;
      }

      //compute the costs for this point on the trajectory
      occ_cost = std::max(std::max(occ_cost, footprint_cost), double(costmap_.getCost(cell_x, cell_y)));
      path_dist = map_(cell_x, cell_y).path_dist;
      goal_dist = map_(cell_x, cell_y).goal_dist;

      front_path_dist = front_map_(front_cell_x, front_cell_y).path_dist;
      front_goal_dist = front_map_(front_cell_x, front_cell_y).goal_dist;

      //if a point on this trajectory has no clear path to the goal... it is invalid
      if(impossible_cost <= goal_dist || impossible_cost <= path_dist){
        traj.cost_ = -2.0; //-2.0 means that we were blocked because propagation failed
        return;
      }

      //add the point to the trajectory so we can draw it later if we want
      traj.addPoint(pos[0], pos[1], pos[2]);

      //update the position of the robot using the velocities passed in
      pos = computeNewPositions(pos, vel, dt);
      time += dt;
    }

    double resolution = costmap_.getResolution();
    //if we're not at the last point in the plan, then we can just score 
    if(two_point_scoring)
      traj.cost_ = pdist_scale_ * resolution * ((front_path_dist + path_dist) / 2.0) + gdist_scale_ * resolution * ((front_goal_dist + goal_dist) / 2.0) + occdist_scale_ * occ_cost;
    else
      traj.cost_ = pdist_scale_ * resolution * path_dist + gdist_scale_ * resolution * goal_dist + occdist_scale_ * occ_cost;
    //ROS_ERROR("%.2f, %.2f, %.2f, %.2f", vel[0], vel[1], vel[2], traj.cost_);
  }

  bool AckermannPlanner::checkTrajectory(const Eigen::Vector3f& pos, const Eigen::Vector3f& vel){
    resetOscillationFlags();
    base_local_planner::Trajectory t;
    generateTrajectory(pos, vel, t, false);

    //if the trajectory is a legal one... the check passes
    if(t.cost_ >= 0)
      return true;

    //otherwise the check fails
    return false;
  }

  void AckermannPlanner::updatePlan(const std::vector<geometry_msgs::PoseStamped>& new_plan){
    global_plan_.resize(new_plan.size());
    for(unsigned int i = 0; i < new_plan.size(); ++i){
      global_plan_[i] = new_plan[i];
    }
  }

  //given the current state of the robot, find a good trajectory
  base_local_planner::Trajectory AckermannPlanner::findBestPath(tf::Stamped<tf::Pose> global_pose, tf::Stamped<tf::Pose> global_vel, 
      tf::Stamped<tf::Pose>& drive_velocities){

    //make sure that our configuration doesn't change mid-run
    boost::mutex::scoped_lock l(configuration_mutex_);

    //make sure to get an updated copy of the costmap before computing trajectories
    costmap_ros_->getCostmapCopy(costmap_);

    Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
    Eigen::Vector3f vel(global_vel.getOrigin().getX(), global_vel.getOrigin().getY(), tf::getYaw(global_vel.getRotation()));

    //reset the map for new operations
    map_.resetPathDist();
    front_map_.resetPathDist();

    //make sure that we update our path based on the global plan and compute costs
    map_.setPathCells(costmap_, global_plan_);

    std::vector<geometry_msgs::PoseStamped> front_global_plan = global_plan_;
    front_global_plan.back().pose.position.x = front_global_plan.back().pose.position.x + forward_point_distance_ * cos(tf::getYaw(front_global_plan.back().pose.orientation));
    front_global_plan.back().pose.position.y = front_global_plan.back().pose.position.y + forward_point_distance_ * sin(tf::getYaw(front_global_plan.back().pose.orientation));
    front_map_.setPathCells(costmap_, front_global_plan);
    ROS_DEBUG_NAMED("ackermann_local_planner", "Path/Goal distance computed");

    //rollout trajectories and find the minimum cost one
    base_local_planner::Trajectory best = computeTrajectories(pos, vel);
    ROS_DEBUG_NAMED("ackermann_local_planner", "Trajectories created");

    //if we don't have a legal trajectory, we'll just command zero
    if(best.cost_ < 0){
      drive_velocities.setIdentity();
    }
    else{
      btVector3 start(best.xv_, best.yv_, 0);
      drive_velocities.setOrigin(start);
      btMatrix3x3 matrix;
      matrix.setRotation(tf::createQuaternionFromYaw(best.thetav_));
      drive_velocities.setBasis(matrix);
    }

    //we'll publish the visualization of the costs to rviz before returning our best trajectory
    map_viz_.publishCostCloud();

    return best;
  }
};
