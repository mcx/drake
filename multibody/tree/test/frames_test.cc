/* clang-format off to disable clang-format-includes */
#include "drake/multibody/tree/multibody_tree.h"
/* clang-format on */

#include <memory>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "drake/common/eigen_types.h"
#include "drake/common/test_utilities/expect_throws_message.h"
#include "drake/multibody/tree/fixed_offset_frame.h"
#include "drake/multibody/tree/multibody_tree-inl.h"
#include "drake/multibody/tree/multibody_tree_system.h"
#include "drake/multibody/tree/revolute_joint.h"
#include "drake/multibody/tree/rigid_body.h"
#include "drake/systems/framework/context.h"

namespace drake {
namespace multibody {
namespace internal {
namespace {

using Eigen::AngleAxisd;
using Eigen::Translation3d;
using Eigen::Vector3d;
using math::RigidTransformd;
using std::unique_ptr;
using systems::Context;

const double kEpsilon = std::numeric_limits<double>::epsilon();

// This unit test fixture sets up a simple MultibodyTree model containing a
// number frames to verify the correctness of the frame methods.
// The model consists of a body B (with its corresponding BodyFrame) connected
// to the world via a revolute mobilizer, a frame P at pose X_BP on B, and a
// frame Q at pose X_PQ attached to P.
class FrameTests : public ::testing::Test {
 public:
  void SetUp() override {
    // Creates a NaN SpatialInertia to instantiate a body for the test.
    // Using a NaN spatial inertia is ok in this unit test since all
    // computations only relate to frame kinematics (and therefore mass
    // properties do not play any role.)
    const auto M_Bo_B = SpatialInertia<double>::NaN();

    // Create an empty model.
    auto model = std::make_unique<internal::MultibodyTree<double>>();

    bodyB_ = &model->AddRigidBody("B", M_Bo_B);
    frameB_ = &bodyB_->body_frame();

    // Joint connecting bodyB to the world.
    model->AddJoint<RevoluteJoint>("joint0", model->world_body(), {}, *bodyB_,
                                   {}, Vector3d::UnitZ() /*revolute axis*/);

    // Some arbitrary pose of frame P in the body frame B.
    X_BP_ = RigidTransformd(AngleAxisd(M_PI / 6.0, Vector3d::UnitZ()) *
                            AngleAxisd(M_PI / 5.0, Vector3d::UnitX()) *
                            Translation3d(0.0, -1.0, 0.0));
    // Frame P is rigidly attached to B with pose X_BP.
    frameP_ =
        &model->AddFrame<FixedOffsetFrame>("P", bodyB_->body_frame(), X_BP_);

    // Some arbitrary pose of frame Q in frame P.
    X_PQ_ = RigidTransformd(AngleAxisd(-M_PI / 3.0, Vector3d::UnitZ()) *
                            AngleAxisd(M_PI / 7.0, Vector3d::UnitX()) *
                            Translation3d(0.5, 1.0, -2.0));
    // Frame Q is rigidly attached to P with pose X_PQ.
    frameQ_ = &model->AddFrame<FixedOffsetFrame>("Q", *frameP_, X_PQ_);

    // Frame R is arbitrary, but named and not ephemeral.
    frameR_ = &model->AddFrame<FixedOffsetFrame>(
        "R", *frameP_, math::RigidTransformd::Identity());
    EXPECT_FALSE(frameR_->is_ephemeral());

    // Frame S is arbitrary, but named, with a specific model instance, and
    // ephemeral.
    X_WS_ = X_BP_;  // Any non-identity pose will do.
    extra_instance_ = model->AddModelInstance("extra_instance");
    frameS_ =
        &model->AddEphemeralFrame(std::make_unique<FixedOffsetFrame<double>>(
            "S", model->world_frame(), X_WS_, extra_instance_));
    EXPECT_EQ(frameS_->scoped_name().get_full(), "extra_instance::S");
    EXPECT_TRUE(frameS_->is_ephemeral());

    // Ensure that the model instance propagates implicitly.
    frameSChild_ = &model->AddFrame<FixedOffsetFrame>(
        "SChild", *frameS_, math::RigidTransformd::Identity());
    EXPECT_EQ(frameSChild_->scoped_name().get_full(), "extra_instance::SChild");

    // We are done adding modeling elements. Transfer tree to system and get
    // a Context.
    tree_ptr_ = model.get();
    system_ = std::make_unique<internal::MultibodyTreeSystem<double>>(
        std::move(model));
    context_ = system_->CreateDefaultContext();

    // An arbitrary pose of an arbitrary frame G in an arbitrary frame F.
    X_FG_ = RigidTransformd(AngleAxisd(M_PI / 6.0, Vector3d::UnitY()) *
                            AngleAxisd(M_PI / 8.0, Vector3d::UnitZ()) *
                            AngleAxisd(M_PI / 5.0, Vector3d::UnitX()) *
                            Translation3d(2.0, -1.0, 0.5));

    // An arbitrary pose of an arbitrary frame F in an arbitrary frame Q.
    X_QF_ = X_FG_;

    // An arbitrary pose of an arbitrary frame G in an arbitrary frame Q.
    X_QG_ = X_FG_;
  }

  const internal::MultibodyTree<double>& tree() const {
    return internal::GetInternalTree(*system_);
  }

 protected:
  std::unique_ptr<internal::MultibodyTreeSystem<double>> system_;
  std::unique_ptr<Context<double>> context_;
  const internal::MultibodyTree<double>* tree_ptr_{};

  // Bodies:
  const RigidBody<double>* bodyB_;
  // Model instances.
  ModelInstanceIndex extra_instance_;
  // Frames:
  const Frame<double>* frameB_{};
  const Frame<double>* frameP_{};
  const Frame<double>* frameQ_{};
  const Frame<double>* frameR_{};
  const Frame<double>* frameS_{};
  const Frame<double>* frameSChild_{};
  // Poses:
  math::RigidTransformd X_BP_;
  math::RigidTransformd X_PQ_;
  math::RigidTransformd X_WS_;
  math::RigidTransformd X_FG_;
  math::RigidTransformd X_QF_;
  math::RigidTransformd X_QG_;
};

TEST_F(FrameTests, IsWorldFrameMethod) {
  EXPECT_FALSE(frameB_->is_world_frame());
  EXPECT_FALSE(frameP_->is_world_frame());
  EXPECT_FALSE(frameQ_->is_world_frame());
  EXPECT_FALSE(frameR_->is_world_frame());
  EXPECT_FALSE(frameS_->is_world_frame());
  EXPECT_FALSE(frameSChild_->is_world_frame());
  const Frame<double>& frame_W = tree().world_frame();
  EXPECT_TRUE(frame_W.is_world_frame());
}

TEST_F(FrameTests, IsBodyFrameMethod) {
  EXPECT_TRUE(frameB_->is_body_frame());
  EXPECT_FALSE(frameP_->is_body_frame());
  EXPECT_FALSE(frameQ_->is_body_frame());
  EXPECT_FALSE(frameR_->is_body_frame());
  EXPECT_FALSE(frameS_->is_body_frame());
  EXPECT_FALSE(frameSChild_->is_body_frame());
  const Frame<double>& frame_W = tree().world_frame();
  EXPECT_TRUE(frame_W.is_body_frame());
  const Frame<double>& bodyB_frame = bodyB_->body_frame();
  EXPECT_TRUE(bodyB_frame.is_body_frame());
}

// Create a frame which is not part of a plant. Ensure an exception is thrown if
// there is a query for information about this frame that needs the associated
// multibody tree. Check that exceptions are thrown for common public methods.
TEST_F(FrameTests, IsFrameMethodCalledWithNoMultibodyTree) {
  FixedOffsetFrame<double> frame_not_in_multibody_tree(
      "bad_frame", *frameB_, math::RigidTransform<double>{});

  DRAKE_EXPECT_THROWS_MESSAGE(
      frame_not_in_multibody_tree.CalcPoseInWorld(*context_),
      ".*has_parent_tree.*failed.*");

  const Frame<double>& frame_W = tree().world_frame();
  DRAKE_EXPECT_THROWS_MESSAGE(
      frame_not_in_multibody_tree.CalcPose(*context_, frame_W),
      ".*has_parent_tree.*failed.*");

  DRAKE_EXPECT_THROWS_MESSAGE(
      frame_not_in_multibody_tree.CalcRotationMatrixInWorld(*context_),
      ".*has_parent_tree.*failed.*");

  DRAKE_EXPECT_THROWS_MESSAGE(
      frame_not_in_multibody_tree.CalcRotationMatrix(*context_, frame_W),
      ".*has_parent_tree.*failed.*");
}

// Verifies the BodyFrame methods to compute poses in different frames.
TEST_F(FrameTests, BodyFrameCalcPoseMethods) {
  // Verify this method computes the pose X_BF of this frame F in the body
  // frame B to which this frame attaches to. Since in this case frame F IS the
  // body frame B, X_BF = Id and this method should return the identity
  // transformation. Next, verify the method CalcRotationMatrixInBodyFrame()
  // returns an identity rotation matrix for the rotation matrix R_BF.

  // Check the cached method for getting the body pose.
  EXPECT_TRUE(frameB_->EvalPoseInBodyFrame(*context_).IsExactlyIdentity());

  EXPECT_TRUE(frameB_->CalcPoseInBodyFrame(*context_).IsExactlyIdentity());
  EXPECT_TRUE(
      frameB_->CalcRotationMatrixInBodyFrame(*context_).IsExactlyIdentity());

  // Now verify the fixed pose version of the same method.
  // Similarly, verify the method GetFixedRotationMatrixInBodyFrame() for R_BF.
  EXPECT_TRUE(frameB_->GetFixedPoseInBodyFrame().IsExactlyIdentity());
  EXPECT_TRUE(frameB_->GetFixedRotationMatrixInBodyFrame().IsExactlyIdentity());

  // Verify this method computes the pose of a frame G measured in this
  // frame F given the pose of frame G in this frame F as: X_BG = X_BF * X_FG.
  // Since in this case frame F IS the body frame B, X_BF = Id and this method
  // simply returns X_FG.  Similarly, R_BG = R_BF * R_FG = Identity * R_FG.
  EXPECT_TRUE(frameB_->CalcOffsetPoseInBody(*context_, X_FG_)
                  .IsNearlyEqualTo(X_FG_, kEpsilon));
  const math::RotationMatrix<double>& R_FG = X_FG_.rotation();
  EXPECT_TRUE(frameB_->CalcOffsetRotationMatrixInBody(*context_, R_FG)
                  .IsNearlyEqualTo(R_FG, kEpsilon));

  // Now verify the fixed pose version of the same method.
  // As in the variant above, since in this case frame F IS the body frame B,
  // X_BF = Id and this method simply returns X_FG.
  // Similarly, R_BG = R_BF * R_FG = Identity * R_FG = R_FG.
  EXPECT_TRUE(frameB_->GetFixedOffsetPoseInBody(X_FG_).IsNearlyEqualTo(
      X_FG_, kEpsilon));
  EXPECT_TRUE(frameB_->GetFixedRotationMatrixInBody(R_FG).IsNearlyEqualTo(
      R_FG, kEpsilon));
}

// Verifies the FixedOffsetFrame methods to compute poses in different frames.
// In these tests frame P is attached to body frame B with a fixed posed X_BP.
// Schematically:
//
//         X_BP
//     B -------> P
TEST_F(FrameTests, FixedOffsetFrameCalcPoseMethods) {
  // Verify this method returns the pose X_BP of frame P in body frame B.
  // Similarly, verify the method CalcRotationMatrixInBodyFrame() returns R_BP.
  const math::RotationMatrix<double>& R_BP = X_BP_.rotation();

  // Check the cached method for getting the body pose.
  EXPECT_TRUE(
      frameP_->EvalPoseInBodyFrame(*context_).IsNearlyEqualTo(X_BP_, kEpsilon));

  EXPECT_TRUE(
      frameP_->CalcPoseInBodyFrame(*context_).IsNearlyEqualTo(X_BP_, kEpsilon));
  EXPECT_TRUE(frameP_->CalcRotationMatrixInBodyFrame(*context_).IsNearlyEqualTo(
      R_BP, kEpsilon));

  // Now verify the fixed pose and rotation matrix versions of those methods.
  EXPECT_TRUE(
      frameP_->GetFixedPoseInBodyFrame().IsNearlyEqualTo(X_BP_, kEpsilon));
  EXPECT_TRUE(frameP_->GetFixedRotationMatrixInBodyFrame().IsNearlyEqualTo(
      R_BP, kEpsilon));

  // Verify this method computes the pose X_BQ of a third frame Q measured in
  // the body frame B given we know the pose X_PQ of frame G in our frame P as:
  // X_BQ = X_BP * X_PQ.  Similarly for R_BQ = R_BP * R_PQ.
  const math::RigidTransform<double> X_BQ = X_BP_ * X_PQ_;
  EXPECT_TRUE(frameP_->CalcOffsetPoseInBody(*context_, X_PQ_)
                  .IsNearlyEqualTo(X_BQ, kEpsilon));
  const math::RotationMatrix<double>& R_PQ = X_PQ_.rotation();
  const math::RotationMatrix<double>& R_BQ = X_BQ.rotation();
  EXPECT_TRUE(frameP_->CalcOffsetRotationMatrixInBody(*context_, R_PQ)
                  .IsNearlyEqualTo(R_BQ, kEpsilon));

  // Now verify the fixed pose and rotation matrix versions of those methods.
  EXPECT_TRUE(
      frameP_->GetFixedOffsetPoseInBody(X_PQ_).IsNearlyEqualTo(X_BQ, kEpsilon));
  EXPECT_TRUE(frameP_->GetFixedRotationMatrixInBody(R_PQ).IsNearlyEqualTo(
      R_BQ, kEpsilon));
}

// Verifies FixedOffsetFrame methods to compute poses in different frames when
// several FixedOffsetFrame objects are chained in sequence.
// In these tests frame P is attached to body B with fixed offset X_BP while
// frame Q attaches to frame P with fixed offset X_PQ. Schematically:
//
//         X_BP       X_PQ
//     B -------> P -------> Q
TEST_F(FrameTests, ChainedFixedOffsetFrames) {
  // Verify this method computes the pose of frame Q in the body frame B as:
  // X_BQ = X_BP * X_PQ.
  // Similarly verify the method CalcRotationMatrixInBodyFrame() returns R_BQ.
  const math::RigidTransform<double> X_BQ = X_BP_ * X_PQ_;
  const math::RotationMatrix<double> R_BQ = X_BQ.rotation();

  // Check the cached method for getting the body pose.
  EXPECT_TRUE(
      frameQ_->EvalPoseInBodyFrame(*context_).IsNearlyEqualTo(X_BQ, kEpsilon));

  EXPECT_TRUE(
      frameQ_->CalcPoseInBodyFrame(*context_).IsNearlyEqualTo(X_BQ, kEpsilon));
  EXPECT_TRUE(frameQ_->CalcRotationMatrixInBodyFrame(*context_).IsNearlyEqualTo(
      R_BQ, kEpsilon));

  // Now verify the fixed pose and rotation matrix versions of those methods.
  EXPECT_TRUE(
      frameQ_->GetFixedPoseInBodyFrame().IsNearlyEqualTo(X_BQ, kEpsilon));
  EXPECT_TRUE(frameQ_->GetFixedRotationMatrixInBodyFrame().IsNearlyEqualTo(
      R_BQ, kEpsilon));

  // Verify this method computes the pose X_BG of a fourth frame G measured in
  // the body frame B given we know the pose X_QG of frame G in our frame Q as:
  // X_BG = X_BP * X_PQ * X_QG.  Similarly for R_BG = R_BP * R_PQ * R_QG.
  const math::RigidTransform<double> X_BG = X_BP_ * X_PQ_ * X_QG_;
  EXPECT_TRUE(frameQ_->CalcOffsetPoseInBody(*context_, X_QG_)
                  .IsNearlyEqualTo(X_BG, kEpsilon));
  const math::RotationMatrix<double>& R_QG = X_QG_.rotation();
  const math::RotationMatrix<double>& R_BG = X_BG.rotation();
  EXPECT_TRUE(frameQ_->CalcOffsetRotationMatrixInBody(*context_, R_QG)
                  .IsNearlyEqualTo(R_BG, kEpsilon));

  // Now verify the fixed pose and rotation matrix versions of those methods.
  EXPECT_TRUE(
      frameQ_->GetFixedOffsetPoseInBody(X_QG_).IsNearlyEqualTo(X_BG, kEpsilon));
  EXPECT_TRUE(frameQ_->GetFixedRotationMatrixInBody(R_QG).IsNearlyEqualTo(
      R_BG, kEpsilon));

  // Change the poses of the parent P and frame Q and make sure those are
  // noticed by the cached method. We're also going to do some digging to
  // show that we don't recalculate unnecessarily. For that we'll find the
  // cached value's serial number and show that it changes when we have
  // to recalculate and doesn't when we don't.

  const systems::CacheEntry& body_pose_cache_entry =
      system_->frame_body_poses_cache_entry();
  // This function just digs out the cached value; it doesn't compute anything.
  const systems::CacheEntryValue& body_poses =
      body_pose_cache_entry.get_cache_entry_value(*context_);
  const int64_t starting_serial_number = body_poses.serial_number();

  const math::RigidTransform<double> X_PQ2 = RigidTransformd(
      AngleAxisd(1.23, Vector3d::UnitZ()) *
      AngleAxisd(-1.45, Vector3d::UnitX()) * Translation3d(5.0, 6.0, -7.0));
  dynamic_cast<const FixedOffsetFrame<double>*>(frameQ_)->SetPoseInParentFrame(
      &*context_, X_PQ2);
  EXPECT_TRUE(frameQ_->EvalPoseInBodyFrame(*context_).IsNearlyEqualTo(
      X_BP_ * X_PQ2, kEpsilon));
  EXPECT_EQ(body_poses.serial_number(), starting_serial_number + 1);

  // Now change the parent P and make sure Q notices.
  dynamic_cast<const FixedOffsetFrame<double>*>(frameP_)->SetPoseInParentFrame(
      &*context_, X_PQ2);  // Re-using the same frame for convenience.
  EXPECT_TRUE(frameP_->EvalPoseInBodyFrame(*context_).IsExactlyEqualTo(X_PQ2));
  EXPECT_EQ(body_poses.serial_number(), starting_serial_number + 2);

  // Looking at frameQ's pose now shouldn't require recomputation.
  EXPECT_TRUE(frameQ_->EvalPoseInBodyFrame(*context_).IsNearlyEqualTo(
      X_PQ2 * X_PQ2, kEpsilon));
  EXPECT_EQ(body_poses.serial_number(), starting_serial_number + 2);
}

TEST_F(FrameTests, NamedFrame) {
  EXPECT_EQ(frameR_->name(), "R");
}

TEST_F(FrameTests, ModelInstanceOverride) {
  EXPECT_EQ(frameR_->model_instance(), default_model_instance());
  EXPECT_EQ(frameS_->model_instance(), extra_instance_);
  EXPECT_EQ(frameSChild_->model_instance(), extra_instance_);
}

// Verifies that `HasFrameNamed` has model instances correctly mapped for named
// frames.
TEST_F(FrameTests, HasFrameNamed) {
  for (FrameIndex i{0}; i < tree().num_frames(); ++i) {
    auto& frame = tree().get_frame(i);
    EXPECT_TRUE(tree().HasFrameNamed(frame.name(), frame.model_instance()))
        << frame.name();
  }
}

TEST_F(FrameTests, ShallowCloneTests) {
  // Make sure ShallowClone() preserves RigidBodyFrame frameB's properties.
  const std::unique_ptr<Frame<double>> clone_of_frameB =
      frameB_->ShallowClone();
  EXPECT_NE(clone_of_frameB.get(), frameB_);
  EXPECT_NE(dynamic_cast<const RigidBodyFrame<double>*>(frameB_), nullptr);
  EXPECT_EQ(clone_of_frameB->name(), frameB_->name());
  EXPECT_EQ(clone_of_frameB->model_instance(), frameB_->model_instance());
  EXPECT_FALSE(clone_of_frameB->is_ephemeral());

  // Make sure ShallowClone() preserves FixedOffsetFrame frameS's properties.
  const std::unique_ptr<Frame<double>> clone_of_frameS =
      frameS_->ShallowClone();
  EXPECT_NE(clone_of_frameS.get(), frameS_);
  EXPECT_EQ(clone_of_frameS->name(), frameS_->name());
  EXPECT_EQ(clone_of_frameS->model_instance(), frameS_->model_instance());
  EXPECT_TRUE(clone_of_frameS->is_ephemeral());
  const auto& downcast_clone =
      dynamic_cast<const FixedOffsetFrame<double>&>(*clone_of_frameS);
  EXPECT_TRUE(downcast_clone.GetFixedPoseInBodyFrame().IsExactlyEqualTo(X_WS_));
}

TEST_F(FrameTests, DeepCloneTests) {
  auto cloned_tree = tree_ptr_->CloneToScalar<AutoDiffXd>();

  // Make sure CloneToScalar() preserves RigidBodyFrame frameB's properties.
  const Frame<AutoDiffXd>& clone_of_frameB =
      cloned_tree->get_frame(frameB_->index());
  EXPECT_NE(dynamic_cast<const RigidBodyFrame<AutoDiffXd>*>(&clone_of_frameB),
            nullptr);
  EXPECT_EQ(clone_of_frameB.name(), frameB_->name());
  EXPECT_EQ(clone_of_frameB.model_instance(), frameB_->model_instance());
  EXPECT_FALSE(clone_of_frameB.is_ephemeral());

  // Make sure CloneToScalar() preserves FixedOffsetFrame frameS's properties.
  const Frame<AutoDiffXd>& clone_of_frameS =
      cloned_tree->get_frame(frameS_->index());
  EXPECT_EQ(clone_of_frameS.name(), frameS_->name());
  EXPECT_EQ(clone_of_frameS.model_instance(), frameS_->model_instance());
  EXPECT_TRUE(clone_of_frameS.is_ephemeral());
  const auto& downcast_clone =
      dynamic_cast<const FixedOffsetFrame<AutoDiffXd>&>(clone_of_frameS);
  EXPECT_TRUE(downcast_clone.GetFixedPoseInBodyFrame().IsExactlyEqualTo(
      X_WS_.cast<AutoDiffXd>()));
}

}  // namespace
}  // namespace internal
}  // namespace multibody
}  // namespace drake
