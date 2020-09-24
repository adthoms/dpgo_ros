/* ----------------------------------------------------------------------------
 * Copyright 2020, Massachusetts Institute of Technology, * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Yulun Tian, et al. (see README for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

#include <dpgo_ros/PGOAgentROS.h>
#include <dpgo_ros/utils.h>
#include <geometry_msgs/PoseArray.h>
#include <nav_msgs/Path.h>
#include <tf/tf.h>

#include <map>

using namespace DPGO;

namespace dpgo_ros {

PGOAgentROS::PGOAgentROS(ros::NodeHandle nh_, unsigned ID,
                         const PGOAgentParameters& params)
    : PGOAgent(ID, params), nh(nh_) {
  // ROS subscriber
  commandSubscriber =
      nh.subscribe("/dpgo_command", 100, &PGOAgentROS::commandCallback, this);

  poseGraphSubscriber =
      nh.subscribe("pose_graph", 10, &PGOAgentROS::poseGraphCallback, this);

  // ROS service
  queryLiftingMatrixServer = nh.advertiseService(
      "query_lifting_matrix", &PGOAgentROS::queryLiftingMatrixCallback, this);

  queryPoseServer = nh.advertiseService("query_poses",
                                        &PGOAgentROS::queryPosesCallback, this);

  // ROS publisher
  commandPublisher = nh.advertise<Command>("/dpgo_command", 100);
  poseArrayPublisher = nh.advertise<geometry_msgs::PoseArray>("trajectory", 1);
  pathPublisher = nh.advertise<nav_msgs::Path>("path", 1);

  // Query robot 0 for lifting matrix
  if (getID() != 0) {
    std::string service_name = "/dpgo_agent_0/query_lifting_matrix";
    QueryLiftingMatrix query;
    query.request.robot_id = 0;
    if (!ros::service::waitForService(service_name, ros::Duration(5.0))) {
      ROS_ERROR("Service to query lifting matrix does not exist!");
      ros::shutdown();
    }
    if (!ros::service::call(service_name, query)) {
      ROS_ERROR("Failed to query lifting matrix!");
      ros::shutdown();
    }
    Matrix YLift = MatrixFromMsg(query.response.matrix);
    setLiftingMatrix(YLift);
  }
}

PGOAgentROS::~PGOAgentROS() {}

void PGOAgentROS::update() {
  ROS_INFO_STREAM("Agent " << getID() << " udpating...");

  // Query neighbors for their public poses
  std::set<unsigned> neighborAgents = getNeighbors();
  for (unsigned neighborID : neighborAgents) {
    if (!requestPublicPosesFromAgent(neighborID)) {
      ROS_WARN_STREAM("Public poses from neighbor " << neighborID
                                                    << " are not available.");
    }
  }

  // Optimize!
  ROPTResult OptResult = optimize();
  if (!OptResult.success) {
    ROS_WARN("Skipped optimization!");
  } else {
    ROS_INFO_STREAM("Objective decrease: " << OptResult.fInit - OptResult.fOpt);
  }

  // Publish trajectory
  if (!publishTrajectory()) {
    ROS_ERROR("Failed to publish trajectory in global frame!");
  }

  // Randomly select a neighbor to update next
  ros::Duration(0.05).sleep();
  unsigned neighborID;
  if (!getRandomNeighbor(neighborID)) {
    ROS_ERROR("This agent has no neighbor!");
  }
  Command msg;
  msg.command = Command::UPDATE;
  msg.executing_robot = neighborID;
  commandPublisher.publish(msg);
}

bool PGOAgentROS::requestPublicPosesFromAgent(const unsigned& neighborID) {
  std::vector<unsigned> poseIndices = getNeighborPublicPoses(neighborID);

  // Call ROS service
  QueryPoses srv;
  srv.request.robot_id = neighborID;
  for (size_t i = 0; i < poseIndices.size(); ++i) {
    srv.request.pose_ids.push_back(poseIndices[i]);
  }
  std::string service_name =
      "/dpgo_agent_" + std::to_string(neighborID) + "/query_poses";

  if (!ros::service::waitForService(service_name, ros::Duration(5.0))) {
    ROS_ERROR_STREAM("ROS service " << service_name << " does not exist!");
    return false;
  }
  if (!ros::service::call(service_name, srv)) {
    ROS_ERROR_STREAM("Failed to call ROS service " << service_name);
    return false;
  }
  if (srv.response.poses.size() != srv.request.pose_ids.size()) {
    ROS_ERROR(
        "Number of replied poses does not match number of requested pose!");
    return false;
  }

  if (srv.response.poses[0].cluster_id != 0) {
    ROS_WARN("Received poses are not merged in active cluster yet.");
    return false;
  }

  for (size_t i = 0; i < srv.response.poses.size(); ++i) {
    LiftedPose poseNbr = srv.response.poses[i];
    Matrix Xnbr = MatrixFromMsg(poseNbr.pose);
    updateNeighborPose(poseNbr.cluster_id, poseNbr.robot_id, poseNbr.pose_id,
                       Xnbr);
  }

  return true;
}

bool PGOAgentROS::publishTrajectory() {
  Matrix globalAnchor;
  if (getID() == 0) {
    getXComponent(0, globalAnchor);
  } else {
    // Request global anchor from robot 0
    QueryPoses srv;
    srv.request.robot_id = 0;
    srv.request.pose_ids.push_back(0);
    std::string service_name = "/dpgo_agent_0/query_poses";
    if (!ros::service::waitForService(service_name, ros::Duration(5.0))) {
      ROS_ERROR_STREAM("ROS service " << service_name << " does not exist!");
      return false;
    }
    if (!ros::service::call(service_name, srv)) {
      ROS_ERROR_STREAM("Failed to call ROS service " << service_name);
      return false;
    }

    globalAnchor = MatrixFromMsg(srv.response.poses[0].pose);
  }

  Matrix T = getTrajectoryInGlobalFrame(globalAnchor);

  // Publish as pose array
  geometry_msgs::PoseArray pose_array =
      TrajectoryToPoseArray(dimension(), num_poses(), T);
  poseArrayPublisher.publish(pose_array);

  // Publish as path
  nav_msgs::Path path = TrajectoryToPath(dimension(), num_poses(), T);
  pathPublisher.publish(path);

  return true;
}

void PGOAgentROS::commandCallback(const CommandConstPtr& msg) {
  switch (msg->command) {
    case Command::INITIALIZE:
      ROS_ERROR("INITIALIZE not implemented!");
      break;

    case Command::TERMINATE:
      ROS_ERROR("TERMINATE not implemented!");
      break;

    case Command::UPDATE:
      if (msg->executing_robot == getID()) {
        // My turn to update!
        update();
      }
      break;

    default:
      ROS_ERROR("Invalid command!");
  }
}

void PGOAgentROS::poseGraphCallback(
    const pose_graph_tools::PoseGraphConstPtr& msg) {
  ROS_INFO_STREAM("Agent " << getID() << " receives " << msg->edges.size()
                           << " edges!");
  vector<RelativeSEMeasurement> odometry;
  vector<RelativeSEMeasurement> privateLoopClosures;
  vector<RelativeSEMeasurement> sharedLoopClosures;
  for (size_t i = 0; i < msg->edges.size(); ++i) {
    pose_graph_tools::PoseGraphEdge edge = msg->edges[i];
    RelativeSEMeasurement m = RelativeMeasurementFromMsg(edge);
    if (m.r1 != getID() && m.r2 != getID()) {
      ROS_ERROR_STREAM("Agent " << getID()
                                << " receives irrelevant measurements!");
    }
    if (m.r1 == m.r2) {
      if (m.p1 + 1 == m.p2) {
        odometry.push_back(m);
      } else {
        privateLoopClosures.push_back(m);
      }
    } else {
      sharedLoopClosures.push_back(m);
    }
  }
  setPoseGraph(odometry, privateLoopClosures, sharedLoopClosures);

  ROS_INFO_STREAM("Agent " << getID() << " created local pose graph with "
                           << num_poses() << " poses.");

  // First robot initiates update sequence
  if (getID() == 0) {
    ros::Duration(3).sleep();
    update();
  }
}

bool PGOAgentROS::queryLiftingMatrixCallback(
    QueryLiftingMatrixRequest& request, QueryLiftingMatrixResponse& response) {
  if (getID() != 0) {
    ROS_ERROR_STREAM("Agent "
                     << getID()
                     << " should not receive request for lifting matrix!");
    return false;
  }
  if (request.robot_id != 0) {
    ROS_ERROR_STREAM("Requested robot ID is not zero! ");
    return false;
  }
  Matrix YLift = getLiftingMatrix();
  response.matrix = MatrixToMsg(YLift);
  return true;
}

bool PGOAgentROS::queryPosesCallback(QueryPosesRequest& request,
                                     QueryPosesResponse& response) {
  if (request.robot_id != getID()) {
    ROS_ERROR("Pose query addressed to wrong agent!");
    return false;
  }

  for (size_t i = 0; i < request.pose_ids.size(); ++i) {
    unsigned poseIndex = request.pose_ids[i];
    Matrix Xi;
    if (!getXComponent(poseIndex, Xi)) {
      ROS_ERROR("Requested pose index does not exist!");
      return false;
    }
    LiftedPose pose = constructLiftedPoseMsg(
        dimension(), relaxation_rank(), getCluster(), getID(), poseIndex, Xi);
    response.poses.push_back(pose);
  }

  return true;
}

}  // namespace dpgo_ros
