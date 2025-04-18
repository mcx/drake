/// @file
///
/// Demo of moving the jaco's end effector in cartesian space.  This program
/// creates a plan to move the end effector from the current position to the
/// location specified on the command line.  The current calculated position
/// of the end effector is printed before, during, and after the commanded
/// motion.

#include <iostream>

#include <gflags/gflags.h>

#include "drake/lcm/drake_lcm.h"
#include "drake/lcmt_jaco_status.hpp"
#include "drake/manipulation/kinova_jaco/jaco_constants.h"
#include "drake/manipulation/util/move_ik_demo_base.h"
#include "drake/math/rigid_transform.h"
#include "drake/math/roll_pitch_yaw.h"
#include "drake/multibody/parsing/package_map.h"

DEFINE_string(urdf, "", "Name of urdf to load");
DEFINE_string(lcm_status_channel, "KINOVA_JACO_STATUS",
              "Channel on which to listen for lcmt_jaco_status messages.");
DEFINE_string(lcm_plan_channel, "COMMITTED_ROBOT_PLAN",
              "Channel on which to send lcmt_robot_plan messages.");
DEFINE_double(x, 0.3, "x coordinate (meters) to move to");
DEFINE_double(y, -0.26, "y coordinate (meters) to move to");
DEFINE_double(z, 0.5, "z coordinate (meters) to move to");
DEFINE_double(roll, -1.7,
              "target roll (radians) about world x axis for end effector");
DEFINE_double(pitch, -1.3,
              "target pitch (radians) about world y axis for end effector");
DEFINE_double(yaw, -1.8,
              "target yaw (radians) about world z axis for end effector");
DEFINE_string(ee_name, "j2s7s300_end_effector",
              "Name of the end effector link");

namespace drake {
namespace examples {
namespace kinova_jaco_arm {
namespace {

using lcm::DrakeLcm;
using lcm::Subscriber;
using manipulation::kinova_jaco::kFingerSdkToUrdf;
using manipulation::util::MoveIkDemoBase;
using multibody::PackageMap;

const char kUrdfUrl[] =
    "package://drake_models/jaco_description/urdf/"
    "j2s7s300_sphere_collision.urdf";

int DoMain() {
  math::RigidTransformd pose(
      math::RollPitchYawd(FLAGS_roll, FLAGS_pitch, FLAGS_yaw),
      Eigen::Vector3d(FLAGS_x, FLAGS_y, FLAGS_z));

  MoveIkDemoBase demo(
      !FLAGS_urdf.empty() ? FLAGS_urdf : PackageMap{}.ResolveUrl(kUrdfUrl),
      "base", FLAGS_ee_name, 100);

  // Wait for a status message.
  DrakeLcm lcm;
  Subscriber<lcmt_jaco_status> subscriber(&lcm, FLAGS_lcm_status_channel);
  while (subscriber.count() == 0) {
    lcm.HandleSubscriptions(10);
  }

  // Read the positions.
  const lcmt_jaco_status& status = subscriber.message();
  Eigen::VectorXd jaco_q(status.num_joints + status.num_fingers);
  for (int i = 0; i < status.num_joints; i++) {
    jaco_q[i] = status.joint_position[i];
  }
  for (int i = 0; i < status.num_fingers; i++) {
    jaco_q[status.num_joints + i] =
        status.finger_position[i] * kFingerSdkToUrdf;
  }

  // Make a plan.
  demo.HandleStatus(jaco_q);
  std::optional<lcmt_robot_plan> plan = demo.Plan(pose);
  if (!plan.has_value()) {
    std::cerr << "Plan failed; exiting!\n";
    return 0;
  }

  // Run the plan.
  Publish(&lcm, FLAGS_lcm_plan_channel, plan.value());
  return 0;
}

}  // namespace
}  // namespace kinova_jaco_arm
}  // namespace examples
}  // namespace drake

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return drake::examples::kinova_jaco_arm::DoMain();
}
