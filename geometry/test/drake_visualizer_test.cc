#include "drake/geometry/drake_visualizer.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <common_robotics_utilities/base64_helpers.hpp>
#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "drake/common/find_resource.h"
#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/common/test_utilities/expect_throws_message.h"
#include "drake/common/unused.h"
#include "drake/geometry/geometry_frame.h"
#include "drake/geometry/geometry_instance.h"
#include "drake/geometry/kinematics_vector.h"
#include "drake/geometry/proximity_properties.h"
#include "drake/geometry/read_gltf_to_memory.h"
#include "drake/geometry/rgba.h"
#include "drake/geometry/scene_graph.h"
#include "drake/geometry/shape_specification.h"
#include "drake/lcm/drake_lcm.h"
#include "drake/lcm/lcm_messages.h"
#include "drake/lcmt_viewer_draw.hpp"
#include "drake/lcmt_viewer_geometry_data.hpp"
#include "drake/lcmt_viewer_load_robot.hpp"
#include "drake/math/rigid_transform.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/framework/context.h"
#include "drake/systems/framework/diagram_builder.h"

namespace drake {
namespace geometry {

using Eigen::Vector3d;
using internal::MakeLcmChannelNameForRole;
using lcm::DrakeLcmInterface;
using math::RigidTransform;
using math::RigidTransformd;
using math::RotationMatrixd;
using std::make_unique;
using std::map;
using std::string;
using std::unique_ptr;
using std::vector;
using systems::Context;
using systems::Diagram;
using systems::DiagramBuilder;
using systems::Simulator;

class DrakeVisualizerTester {
 public:
  template <typename T>
  static std::string lcm_url(const DrakeVisualizer<T>& visualizer) {
    return visualizer.lcm_->get_lcm_url();
  }

  template <typename T>
  static const DrakeLcmInterface* owned_lcm_interface(
      const DrakeVisualizer<T>& visualizer) {
    return visualizer.owned_lcm_.get();
  }

  template <typename T>
  static const DrakeLcmInterface* active_lcm_interface(
      const DrakeVisualizer<T>& visualizer) {
    return visualizer.lcm_;
  }

  template <typename T>
  static const DrakeVisualizerParams& get_params(
      const DrakeVisualizer<T>& visualizer) {
    return visualizer.params_;
  }
};

namespace {

namespace fs = std::filesystem;

/* Collection of data for reporting what is waiting in the LCM queue.  */
struct MessageResults {
  int num_load{};
  // Zeroed out unless num_load > 0.
  lcmt_viewer_load_robot load_message{};
  int num_draw{};
  // Zeroed out unless num_draw > 0.
  lcmt_viewer_draw draw_message{};
  int num_deformable{};
  // Zeroed out unless num_deformable > 0.
  lcmt_viewer_link_data deformable_message{};
};

/* Serves as a source of pose values for SceneGraph input ports. */
template <typename T>
class PoseSource : public systems::LeafSystem<T> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(PoseSource);
  PoseSource() {
    this->DeclareAbstractOutputPort(systems::kUseDefaultName,
                                    FramePoseVector<T>(),
                                    &PoseSource<T>::ReadPoses);
  }

  void SetPoses(FramePoseVector<T> poses) { poses_ = std::move(poses); }

 private:
  void ReadPoses(const Context<T>&, FramePoseVector<T>* poses) const {
    *poses = poses_;
  }

  FramePoseVector<T> poses_;
};

/* Serves as a source of configuration values for SceneGraph input ports. */
template <typename T>
class ConfigurationSource : public systems::LeafSystem<T> {
 public:
  DRAKE_NO_COPY_NO_MOVE_NO_ASSIGN(ConfigurationSource);
  ConfigurationSource() {
    this->DeclareAbstractOutputPort(
        systems::kUseDefaultName, GeometryConfigurationVector<T>(),
        &ConfigurationSource<T>::ReadConfigurations);
  }

  void SetConfigurations(GeometryConfigurationVector<T> configurations) {
    configurations_ = std::move(configurations);
  }

 private:
  void ReadConfigurations(
      const Context<T>&, GeometryConfigurationVector<T>* configurations) const {
    *configurations = configurations_;
  }

  GeometryConfigurationVector<T> configurations_;
};

// TODO(SeanCurtis-TRI): These unit tests aren't complete. Much of the DUT has
//  code from the old `geometry_visualizer.{h|cc}. That code wasn't particularly
//  tested either, but has been run thousands of times. That code lives on in
//  DrakeVisualizer and the tests still leave out the following aspects:
//
//    1. Mapping from shape specification to LCM equivalent (i.e., spheres are
//       visualized as spheres, boxes as boxes, etc.)
//    2. The poses of geometries (both relative to their parent frame and the
//       frame in the world).
//
// The assumption is that if these were wrong, it would immediately apparent to
// every user. If this ever changes, it might be worthwhile to test these
// things.

/* Infrastructure for testing the DrakeVisualizer. */
template <typename T>
class DrakeVisualizerTest : public ::testing::Test {
 protected:
  void setUp() { ASSERT_EQ(lcm_.get_lcm_url(), "memq://"); }

  /* Configures the diagram (and raw pointers) with a DrakeVisualizer configured
   by the given parameters.  */
  void ConfigureDiagram(DrakeVisualizerParams params = {}) {
    DiagramBuilder<T> builder;
    scene_graph_ = builder.template AddSystem<SceneGraph<T>>();
    visualizer_ = builder.template AddSystem<DrakeVisualizer<T>>(
        &lcm_, std::move(params));
    builder.Connect(scene_graph_->get_query_output_port(),
                    visualizer_->query_object_input_port());
    pose_source_id_ = scene_graph_->RegisterSource(kPoseSourceName);
    configuration_source_id_ =
        scene_graph_->RegisterSource(kConfigurationSourceName);
    pose_source_ = builder.template AddSystem<PoseSource<T>>();
    builder.Connect(pose_source_->get_output_port(0),
                    scene_graph_->get_source_pose_port(pose_source_id_));
    configuration_source_ =
        builder.template AddSystem<ConfigurationSource<T>>();
    builder.Connect(
        configuration_source_->get_output_port(0),
        scene_graph_->get_source_configuration_port(configuration_source_id_));
    diagram_ = builder.Build();
  }

  /* Populates SceneGraph with various objects. This function's defining
   characteristic is that there are three different geometries, each with a
   different role, such that there will always be one geometry included,
   regardless of role.  */
  void PopulateScene() {
    // A box has perception properties with a green color.
    const FrameId f0_id = scene_graph_->RegisterFrame(
        pose_source_id_, GeometryFrame("perception", 0));
    const GeometryId g0_id = scene_graph_->RegisterGeometry(
        pose_source_id_, f0_id,
        make_unique<GeometryInstance>(RigidTransformd{},
                                      make_unique<Box>(1, 1, 1), "perception"));
    PerceptionProperties percep_prop;
    percep_prop.AddProperty("phong", "diffuse", Rgba(0, 1, 0, 1));
    scene_graph_->AssignRole(pose_source_id_, g0_id, percep_prop);

    // A cylinder has illustration properties with a blue color.
    const FrameId f1_id = scene_graph_->RegisterFrame(
        pose_source_id_, GeometryFrame("illustration", 1));
    const GeometryId g1_id = scene_graph_->RegisterGeometry(
        pose_source_id_, f1_id,
        make_unique<GeometryInstance>(
            RigidTransformd{}, make_unique<Cylinder>(1, 1), "illustration"));
    IllustrationProperties illus_prop;
    illus_prop.AddProperty("phong", "diffuse", Rgba(0, 1, 0, 1));
    scene_graph_->AssignRole(pose_source_id_, g1_id, illus_prop);

    // A sphere has proximity properties with no color.
    proximity_frame_id_ = scene_graph_->RegisterFrame(
        pose_source_id_, GeometryFrame("proximity", 2));
    const GeometryId g2_id = scene_graph_->RegisterGeometry(
        pose_source_id_, proximity_frame_id_,
        make_unique<GeometryInstance>(RigidTransformd{}, make_unique<Sphere>(1),
                                      "proximity"));
    scene_graph_->AssignRole(pose_source_id_, g2_id, ProximityProperties());

    pose_source_->SetPoses({{f0_id, RigidTransform<T>{}},
                            {f1_id, RigidTransform<T>{}},
                            {proximity_frame_id_, RigidTransform<T>{}}});
  }

  /* Processes the message queue, reporting the number of load and draw messages
   and, if that number is non-zero, the last message of each type received.  */
  MessageResults ProcessMessages(std::optional<Role> role = std::nullopt) {
    lcm_.HandleSubscriptions(0);
    Subscribers& subs = GetSubscribers(role);
    const int num_draw = subs.draw.count();
    lcmt_viewer_draw draw_message = subs.draw.message();
    const int num_load = subs.load.count();
    lcmt_viewer_load_robot load_message = subs.load.message();
    const int num_deformable = subs.deformable.count();
    lcmt_viewer_link_data deformable_message = subs.deformable.message();
    subs.clear();
    // clang-format off
    return {num_load,       load_message,
            num_draw,       draw_message,
            num_deformable, deformable_message};
    // clang-format on
  }

  /* Handles all subscribers and confirms the number of load and draw messages
   are as expected. Clears the subscribers after looking. We assume in related
   tests that the messages for drawing non-rigid geometries and for deformable
   geometries are dispatched in lock step (even if the message is functionally
   empty), and thus the numbers of the respective messages are the same. */
  ::testing::AssertionResult ExpectedMessageCount(int num_load, int num_draw) {
    MessageResults results = ProcessMessages();
    if (results.num_draw != num_draw || results.num_load != num_load ||
        results.num_deformable != num_draw) {
      return ::testing::AssertionFailure()
             << "Expected " << num_load << " load messages, " << num_draw
             << " draw messages, and " << num_draw << " deformable messages"
             << "\nFound   " << results.num_load << " load messages, "
             << results.num_draw << " draw messages, and "
             << results.num_deformable << " deformable messages.";
    }
    return ::testing::AssertionSuccess();
  }

  /* Helper function to test the two variants of AddToBuilder. Both need to test
   the same behaviors, but they differ in whether a SceneGraph reference is
   provided directly vs providing a QueryObject-valued port. This function
   attempts to provide a test that can test both with a single implementation.

   We've templated it on the three parameters:

     1. AddFunctor - the AddToBuild with four parameters.
     2. AddFunctorNoParam - The AddToBuild with three parameters (rely on
        default DrakeVisualizerParameters values).
     3. SourceFunctor - a function takes the source *system* and determines
        the source parameter (system or port) for the function call.

  For both variants, we need to confirm:

     1. DrakeVisualizer is created.
     2. Connected to SceneGraph/port.
     3. LCM is passed through to the DrakeVisualizer, or it creates its own.
     4. The parameters propagate through appropriately.

  The only thing this test *doesn't* cover is that the two-parameter version
  doesn't default to lcm = nullptr.  */
  template <typename AddFunctor, typename AddFunctorNoParam,
            typename SourceFunctor>
  void TestAddToBuilder(const AddFunctor& add_to_builder,
                        const AddFunctorNoParam& add_to_builder_no_param,
                        const SourceFunctor& port_source) {
    using Tester = DrakeVisualizerTester;
    {
      /* Case: Confirm successful construction, connection, and that the passed
       lcm interface is used.  */
      DiagramBuilder<T> builder;
      const auto& scene_graph = *builder.template AddSystem<SceneGraph<T>>();
      /* The fact that a visualizer is returned is proof that a DrakeVisualizer
       was created.  */
      const auto& visualizer =
          add_to_builder_no_param(&builder, port_source(scene_graph), &lcm_);
      auto diagram = builder.Build();
      auto context = diagram->CreateDefaultContext();
      /* We'll show they are connected because scene_graph's output port reports
       the same GeometryVersion as visualizer's input port.  */
      const auto& sg_context = scene_graph.GetMyContextFromRoot(*context);
      const auto& viz_context = visualizer.GetMyContextFromRoot(*context);
      const auto& sg_query_object =
          scene_graph.get_query_output_port().template Eval<QueryObject<T>>(
              sg_context);
      /* Not throwing is evidence of *some* connection.  */
      const auto& viz_query_object =
          visualizer.query_object_input_port().template Eval<QueryObject<T>>(
              viz_context);

      /* Confirm correct name.  */
      EXPECT_EQ(visualizer.get_name(), "drake_visualizer(illustration)");

      /* Confirm correct connection.  */
      EXPECT_TRUE(sg_query_object.inspector().geometry_version().IsSameAs(
          viz_query_object.inspector().geometry_version(),
          Role::kIllustration));

      /* Confirm unowned active lcm interface.  */
      EXPECT_EQ(Tester::owned_lcm_interface(visualizer), nullptr);
      EXPECT_EQ(Tester::active_lcm_interface(visualizer), &lcm_);

      /* Confirm default parameters used when non specified.  */
      const DrakeVisualizerParams default_params;
      const DrakeVisualizerParams& vis_params = Tester::get_params(visualizer);
      EXPECT_EQ(vis_params.publish_period, default_params.publish_period);
      EXPECT_EQ(vis_params.role, default_params.role);
      EXPECT_EQ(vis_params.default_color, default_params.default_color);
    }

    {
      /* Case: No Lcm provided, confirm owned lcm.  */
      DiagramBuilder<T> builder;
      const auto& scene_graph = *builder.template AddSystem<SceneGraph<T>>();
      const auto& visualizer =
          add_to_builder_no_param(&builder, port_source(scene_graph), nullptr);
      EXPECT_NE(Tester::owned_lcm_interface(visualizer), nullptr);
      EXPECT_EQ(Tester::owned_lcm_interface(visualizer),
                Tester::active_lcm_interface(visualizer));
      /* NOTE: Don't do anything to broadcast here!  */
    }

    {
      /* Case: Parameters given, confirm custom parameters get passed along.  */
      DiagramBuilder<T> builder;
      const auto& scene_graph = *builder.template AddSystem<SceneGraph<T>>();
      const DrakeVisualizerParams params{1 / 13.0, Role::kPerception,
                                         Rgba{0.1, 0.2, 0.3, 0.4}};
      const auto& visualizer =
          add_to_builder(&builder, port_source(scene_graph), &lcm_, params);
      EXPECT_EQ(visualizer.get_name(), "drake_visualizer(perception)");
      const DrakeVisualizerParams& vis_params = Tester::get_params(visualizer);
      EXPECT_EQ(vis_params.publish_period, params.publish_period);
      EXPECT_EQ(vis_params.role, params.role);
      EXPECT_EQ(vis_params.default_color, params.default_color);
    }
  }

  /* Test helper to evaluate how mesh types appear in messages. It can
   distinguish between whether we expect to see an explicit mesh (e.g., a convex
   hull) or whether we simply forward the file path. See tests below for usage.

   Where role matters, the test assumes that the only distinction is kProximity
   and everything else. It uses Role::kIllustration to represent "everything
   else".

   @param role         The role to assign to the test geometry -- should be
                       either kIllustrated or kProximity.
   @param expect_hull  If true, we expect the MeshType with the given `role`
                       to be represented by its convex hull.
   @param in_memory    If true, the mesh will be specified using in-memory
                       resources. */
  template <typename MeshType>
  void ExpectMeshInMessage(Role role, bool expect_hull,
                           bool in_memory = false) {
    std::unique_ptr<MeshType> mesh;
    const std::string gltf_path = FindResourceOrThrow(
        "drake/geometry/render/test/meshes/fully_textured_pyramid.gltf");

    // The names of two supporting files. We'll poke the results by examining
    // each of these two files when the Mesh is defined in memory.
    const std::string kSupportingPngAsPath("fully_textured_pyramid_omr.png");
    const std::string kSupportingPngAsMemory(
        "fully_textured_pyramid_normal.png");

    // Apply a non-uniform scale, so we can observe it in the LCM message.
    const Vector3d scale(2, 3, 4);
    if (in_memory) {
      InMemoryMesh mesh_data = ReadGltfToMemory(fs::path(gltf_path));
      mesh_data.supporting_files[kSupportingPngAsMemory] =
          MemoryFile::Make(std::get<fs::path>(
              mesh_data.supporting_files[kSupportingPngAsMemory]));
      mesh = make_unique<MeshType>(std::move(mesh_data), scale);
    } else {
      mesh = make_unique<MeshType>(gltf_path, scale);
    }

    SCOPED_TRACE(fmt::format("{} {} shape with {} role",
                             in_memory ? "in-memory" : "on-disk",
                             mesh->type_name(), role));
    this->ConfigureDiagram({.role = role});

    const FrameId f_id = this->scene_graph_->RegisterFrame(
        this->pose_source_id_, GeometryFrame("test", 0));

    const RigidTransformd X_PC(Vector3d(-1, 2, -3));
    const GeometryId g_id = this->scene_graph_->RegisterGeometry(
        this->pose_source_id_, f_id,
        make_unique<GeometryInstance>(X_PC, mesh->Clone(), "test"));

    // We assign a diffuse color, because we expect it to come through when
    // `expect_hull` is true.
    const Rgba expected_rgba(0.25, 0.5, 0.75, 1.0);
    switch (role) {
      case Role::kProximity: {
        ProximityProperties props;
        props.AddProperty("phong", "diffuse", expected_rgba);
        this->scene_graph_->AssignRole(this->pose_source_id_, g_id, props);
      } break;
      case Role::kIllustration: {
        IllustrationProperties props;
        props.AddProperty("phong", "diffuse", expected_rgba);
        this->scene_graph_->AssignRole(this->pose_source_id_, g_id, props);
      } break;
      case Role::kPerception:
        DRAKE_UNREACHABLE();
        break;
      case Role::kUnassigned:
        DRAKE_UNREACHABLE();
        break;
    }

    FramePoseVector<T> poses;
    poses.set_value(f_id, RigidTransform<T>{});
    this->pose_source_->SetPoses(std::move(poses));

    /* Dispatch a load message. */
    auto context = this->diagram_->CreateDefaultContext();
    const auto& vis_context = this->visualizer_->GetMyContextFromRoot(*context);
    this->visualizer_->ForcedPublish(vis_context);

    /* Confirm that messages were sent.  */
    MessageResults results = this->ProcessMessages();
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.load_message.num_links, 1);

    const auto& link_message = results.load_message.link[0];
    ASSERT_EQ(link_message.num_geom, 1);
    const auto& geo_message = link_message.geom[0];

    EXPECT_EQ(geo_message.type, geo_message.MESH);
    if (expect_hull) {
      EXPECT_TRUE(geo_message.string_data.empty());
      // The float data contains:
      //   - 2 floats indicating triangle and vertex counts.
      //   - 3 floats for each triangle (we expect 4 for the pyramid).
      //   - 3 floats for each vertex and 3 vertices per triangle (faceted).
      const int num_tris = 4;
      const int num_vertices = num_tris * 3;
      EXPECT_GT(geo_message.num_float_data,
                2 + 3 * num_tris + 3 * num_vertices);

      const auto& color = geo_message.color;
      const Rgba test_rgba(color[0], color[1], color[2], color[3]);
      // The color values used in this test can all be perfectly represented by
      // 32-bit floats (e.g., 0.5, 0.75,  etc.). So, we can use exact equality.
      EXPECT_EQ(test_rgba, expected_rgba);

      const auto& p_PG = geo_message.position;
      const auto& q_PG = geo_message.quaternion;
      const RotationMatrixd R_PG(
          Eigen::Quaternion<double>(q_PG[0], q_PG[1], q_PG[2], q_PG[3]));
      const RigidTransformd X_PG_test(R_PG, {p_PG[0], p_PG[1], p_PG[2]});
      /* Tolerance due to conversion to float. */
      EXPECT_TRUE(CompareMatrices(X_PC.GetAsMatrix34(),
                                  X_PG_test.GetAsMatrix34(), 1e-7));
    } else {
      EXPECT_FALSE(geo_message.float_data.empty());
      EXPECT_EQ(geo_message.float_data[0], scale.x());
      EXPECT_EQ(geo_message.float_data[1], scale.y());
      EXPECT_EQ(geo_message.float_data[2], scale.z());
      if (in_memory) {
        EXPECT_FALSE(geo_message.string_data.empty());
        nlohmann::json json_root =
            nlohmann::json::parse(geo_message.string_data);
        ASSERT_TRUE(json_root.contains("in_memory_mesh"));
        const auto& json_mesh = json_root["in_memory_mesh"];
        ASSERT_TRUE(json_mesh.contains("mesh_file"));
        ASSERT_TRUE(json_mesh["mesh_file"].contains("contents"));
        ASSERT_TRUE(json_mesh["mesh_file"].contains("extension"));
        ASSERT_TRUE(json_mesh["mesh_file"].contains("filename_hint"));
        // Contents is a valid base64-encoded glTF file.
        auto gltf_base64 =
            json_mesh["mesh_file"]["contents"].get<std::string_view>();
        const std::vector<uint8_t> decoded_gltf =
            common_robotics_utilities::base64_helpers::Decode(
                std::string{gltf_base64});
        std::string gltf_content(
            reinterpret_cast<const char*>(decoded_gltf.data()),
            decoded_gltf.size());
        EXPECT_FALSE(gltf_content.empty());
        EXPECT_NO_THROW(unused(nlohmann::json::parse(gltf_content)));
        EXPECT_EQ(json_mesh["mesh_file"]["extension"].get<std::string_view>(),
                  mesh->source().extension());
        EXPECT_EQ(
            json_mesh["mesh_file"]["filename_hint"].get<std::string_view>(),
            mesh->source().in_memory().mesh_file.filename_hint());

        ASSERT_TRUE(json_mesh.contains("supporting_files"));
        const nlohmann::json& files = json_mesh["supporting_files"];

        // We won't explore *all* of the supporting files. We'll poke one that
        // should be encoded as a path, and the other as a MemoryFile.
        ASSERT_TRUE(files.contains(kSupportingPngAsPath));
        ASSERT_TRUE(files[kSupportingPngAsPath].contains("path"));
        EXPECT_EQ(
            files[kSupportingPngAsPath]["path"].get<std::string_view>(),
            std::get<fs::path>(mesh->source().in_memory().supporting_files.at(
                kSupportingPngAsPath)));

        ASSERT_TRUE(files.contains(kSupportingPngAsMemory));
        const nlohmann::json& mem_file = files[kSupportingPngAsMemory];
        EXPECT_EQ(mem_file["extension"].get<std::string_view>(), ".png");
        EXPECT_TRUE(mem_file["filename_hint"].get<std::string_view>().ends_with(
            kSupportingPngAsMemory));
        const auto emissive_64 = mem_file["contents"].get<std::string_view>();
        // We'll confirm that the contents indicate an encoded png.
        const std::vector<uint8_t> decoded_png =
            common_robotics_utilities::base64_helpers::Decode(
                std::string{emissive_64});
        // See http://www.libpng.org/pub/png/spec/1.2/PNG-Structure.html.
        uint8_t kPngHeader[] = {137, 80, 78, 71, 13, 10, 26, 10};
        for (int i = 0; i < 8; ++i) {
          ASSERT_EQ(decoded_png[i], kPngHeader[i]);
        }
      } else {
        EXPECT_THAT(geo_message.string_data,
                    ::testing::HasSubstr("fully_textured_pyramid.gltf"));
      }
    }
    /* We don't care about the draw message. */
  }

  struct Subscribers;
  Subscribers& GetSubscribers(std::optional<Role> role) {
    std::map<Role, Subscribers*> subscribers_{
        {Role::kIllustration, &illustration_subs_},
        {Role::kProximity, &proximity_subs_},
        {Role::kPerception, &perception_subs_},
    };
    if (role.has_value()) {
      DRAKE_DEMAND(*role != Role::kUnassigned);
      return *subscribers_[*role];
    }
    return default_subs_;
  }

  static constexpr double kPublishPeriod = 1 / 64.0;

  /* The names of the sources registered with the SceneGraph.  */
  static constexpr char kPoseSourceName[] = "DrakeVisualizerTestPoseSource";
  static constexpr char kConfigurationSourceName[] =
      "DrakeVisualizerTestConfigurationSource";

  /* The LCM visualizer broadcasts messages on.  */
  lcm::DrakeLcm lcm_;

  /* The subscribers for draw and load messages.  */
  struct Subscribers {
    /* The LCM channel names. We are implicitly confirming that DrakeVisualizer
       broadcasts on the right channels via the subscribers. The reception of
       expected messages on those subscribers is proof.  */
    static constexpr char kLoadChannel[] = "DRAKE_VIEWER_LOAD_ROBOT";
    static constexpr char kDrawChannel[] = "DRAKE_VIEWER_DRAW";
    static constexpr char kDeformableDrawChannel[] = "DRAKE_VIEWER_DEFORMABLE";

    Subscribers(lcm::DrakeLcm* lcm, std::optional<Role> role)
        : draw(lcm, MakeRoleChannelName(kDrawChannel, role)),
          load(lcm, MakeRoleChannelName(kLoadChannel, role)),
          deformable(lcm, MakeRoleChannelName(kDeformableDrawChannel, role)) {}

    void clear() {
      draw.clear();
      load.clear();
      deformable.clear();
    }

    static std::string MakeRoleChannelName(const std::string& channel,
                                           std::optional<Role> role) {
      DrakeVisualizerParams params;
      if (role.has_value()) {
        params.role = *role;
        params.use_role_channel_suffix = true;
      }
      return MakeLcmChannelNameForRole(channel, params);
    }

    lcm::Subscriber<lcmt_viewer_draw> draw;
    lcm::Subscriber<lcmt_viewer_load_robot> load;
    lcm::Subscriber<lcmt_viewer_link_data> deformable;
  };
  Subscribers default_subs_{&lcm_, std::nullopt};
  Subscribers illustration_subs_{&lcm_, Role::kIllustration};
  Subscribers proximity_subs_{&lcm_, Role::kProximity};
  Subscribers perception_subs_{&lcm_, Role::kPerception};

  /* Raw pointer into the diagram's scene graph.  */
  SceneGraph<T>* scene_graph_{};
  /* Raw pointer into the diagram's visualizer.  */
  DrakeVisualizer<T>* visualizer_{};
  /* Raw pointer to the pose data.  */
  PoseSource<T>* pose_source_{};
  ConfigurationSource<T>* configuration_source_{};
  SourceId pose_source_id_;
  SourceId configuration_source_id_;
  /* A diagram containing scene graph and connected visualizer.  */
  unique_ptr<Diagram<T>> diagram_;
  /* The id of the frame registered by PopulateScene() with the proximity
   geometry.  */
  FrameId proximity_frame_id_;
};

using ScalarTypes = ::testing::Types<double, AutoDiffXd>;
TYPED_TEST_SUITE(DrakeVisualizerTest, ScalarTypes);
/* Confirms that the visualizer publishes at the specified period.  */

TYPED_TEST(DrakeVisualizerTest, PublishPeriod) {
  using T = TypeParam;
  for (double period : {1 / 32.0, 1 / 10.0}) {
    this->ConfigureDiagram({.publish_period = period});
    Simulator<T> simulator(*(this->diagram_));
    ASSERT_EQ(simulator.get_context().get_time(), 0.0);

    SCOPED_TRACE(fmt::format("Period = {:.4}", period));

    // When we start up, we should get a load and a draw at time 0.
    simulator.AdvanceTo(0.0);
    EXPECT_TRUE(this->ExpectedMessageCount(1, 1));

    // Advancing past five period boundaries --> five draw messages.
    simulator.AdvanceTo(period * 5.1);
    EXPECT_TRUE(this->ExpectedMessageCount(0, 5));
  }
}

/* Confirms messages are sent, even if there is nothing. This matters because
 a no-op would *not* clear drake_visualizer leading to confusing results (if
 the visualizer already contained geometry from a previous session).  */
TYPED_TEST(DrakeVisualizerTest, EmptyScene) {
  using T = TypeParam;
  this->ConfigureDiagram();
  Simulator<T> simulator(*(this->diagram_));
  simulator.AdvanceTo(0.0);
  MessageResults results = this->ProcessMessages();
  ASSERT_EQ(results.num_load, 1);
  EXPECT_EQ(results.load_message.num_links, 0);

  ASSERT_EQ(results.num_draw, 1);
  EXPECT_EQ(results.draw_message.num_links, 0);
  EXPECT_EQ(results.draw_message.timestamp, 0);

  ASSERT_EQ(results.num_deformable, 1);
  EXPECT_EQ(results.deformable_message.num_geom, 0);
}

/* DrakeVisualizer can accept an lcm interface from the user or instantiate its
 own. This tests that logic. However, it does *not* do any work on the owned
 lcm interface because we don't want this unit test to spew network traffic.  */
TYPED_TEST(DrakeVisualizerTest, OwnedLcm) {
  using Tester = DrakeVisualizerTester;
  using T = TypeParam;
  DrakeVisualizer<T> visualizer_external(&(this->lcm_));
  EXPECT_EQ(Tester::owned_lcm_interface(visualizer_external), nullptr);
  EXPECT_EQ(Tester::active_lcm_interface(visualizer_external), &(this->lcm_));

  /* Note: do not do any work with this instance that would cause LCM messages
   to be spewed!  */
  DrakeVisualizer<T> visualizer_owning;
  EXPECT_NE(Tester::owned_lcm_interface(visualizer_owning), nullptr);
  EXPECT_NE(Tester::owned_lcm_interface(visualizer_owning), &(this->lcm_));
  EXPECT_EQ(Tester::active_lcm_interface(visualizer_owning),
            Tester::owned_lcm_interface(visualizer_owning));
}

/* DrakeVisualizer uses the cache to facilitate coordination between what got
 broadcast in the load message and what needs to be broadcast in the draw
 message. This confirms we get the same results with cache enabled and disabled.
 In this case, we use the number and names of the dynamic frames and the draw
 messages as evidence. */
TYPED_TEST(DrakeVisualizerTest, CacheInsensitive) {
  using T = TypeParam;
  lcmt_viewer_draw messages[2];

  for (bool cache_enabled : {true, false}) {
    this->ConfigureDiagram();
    this->PopulateScene();

    Simulator<T> simulator(*(this->diagram_));
    if (cache_enabled) {
      simulator.get_mutable_context().EnableCaching();
    } else {
      simulator.get_mutable_context().DisableCaching();
    }
    simulator.AdvanceTo(0.0);
    MessageResults results = this->ProcessMessages();
    /* Confirm that messages were sent.  */
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.num_draw, 1);

    /* Stash the draw message for comparison.  */
    messages[cache_enabled ? 0 : 1] = results.draw_message;
  }
  /* Confirm that *something* was visualized.  */
  ASSERT_GT(messages[0].num_links, 0);

  /* Confirm the two draw messages are identical.  */
  const vector<uint8_t> encoding0 = lcm::EncodeLcmMessage(messages[0]);
  const vector<uint8_t> encoding1 = lcm::EncodeLcmMessage(messages[1]);
  EXPECT_EQ(encoding0, encoding1);
}

/* Confirm that configuring the default diffuse color affects the resulting
 color for geometries with no specified diffuse values.  */
TYPED_TEST(DrakeVisualizerTest, ConfigureDefaultDiffuse) {
  using T = TypeParam;
  /* Apply to arbitrary default diffuse and confirm the geometry (with no)
   diffuse color defined, inherits it.  */
  for (const auto& expected_rgba :
       {Rgba{0.75, 0.25, 0.125, 1.0}, Rgba{0.5, 0.75, 0.875, 0.5}}) {
    this->ConfigureDiagram({.publish_period = this->kPublishPeriod,
                            .role = Role::kProximity,
                            .default_color = expected_rgba,
                            .use_role_channel_suffix = true});
    const FrameId f_id = this->scene_graph_->RegisterFrame(
        this->pose_source_id_, GeometryFrame("frame", 0));
    const GeometryId g_id = this->scene_graph_->RegisterGeometry(
        this->pose_source_id_, f_id,
        make_unique<GeometryInstance>(RigidTransformd{}, make_unique<Sphere>(1),
                                      "proximity_sphere"));
    // ProximityProperties will make use of the default diffuse value.
    this->scene_graph_->AssignRole(this->pose_source_id_, g_id,
                                   ProximityProperties());
    this->pose_source_->SetPoses({{f_id, RigidTransform<T>{}}});
    Simulator<T> simulator(*(this->diagram_));
    simulator.AdvanceTo(0.0);
    MessageResults results = this->ProcessMessages(Role::kProximity);
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.load_message.num_links, 1);
    ASSERT_EQ(results.load_message.link[0].num_geom, 1);
    const auto& color = results.load_message.link[0].geom[0].color;
    const Rgba rgba{color[0], color[1], color[2], color[3]};
    EXPECT_EQ(rgba, expected_rgba);
  }
}

/* Confirm that the anchored and dynamic geometries are handled correctly. All
 geometries are loaded (with the "world" frame holding the anchored geometries
 and all dynamic geometries on other frames). The draw message should only
 provide data for the dynamic frames (i.e., "world" will not be included).  */
TYPED_TEST(DrakeVisualizerTest, AnchoredAndDynamicGeometry) {
  using T = TypeParam;
  this->ConfigureDiagram({.publish_period = this->kPublishPeriod,
                          .role = Role::kProximity,
                          .use_role_channel_suffix = true});
  const FrameId f_id = this->scene_graph_->RegisterFrame(
      this->pose_source_id_, GeometryFrame("frame", 0));
  const GeometryId g0_id = this->scene_graph_->RegisterGeometry(
      this->pose_source_id_, f_id,
      make_unique<GeometryInstance>(RigidTransformd{}, make_unique<Sphere>(1),
                                    "sphere0"));
  this->scene_graph_->AssignRole(this->pose_source_id_, g0_id,
                                 ProximityProperties());
  const GeometryId g1_id = this->scene_graph_->RegisterGeometry(
      this->pose_source_id_, f_id,
      make_unique<GeometryInstance>(RigidTransformd{Vector3d{10, 0, 0}},
                                    make_unique<Sphere>(1), "sphere1"));
  this->scene_graph_->AssignRole(this->pose_source_id_, g1_id,
                                 ProximityProperties());

  const GeometryId g2_id = this->scene_graph_->RegisterAnchoredGeometry(
      this->pose_source_id_,
      make_unique<GeometryInstance>(RigidTransformd{Vector3d{5, 0, 0}},
                                    make_unique<Sphere>(1), "sphere3"));
  this->scene_graph_->AssignRole(this->pose_source_id_, g2_id,
                                 ProximityProperties());

  this->pose_source_->SetPoses({{f_id, RigidTransform<T>{}}});

  Simulator<T> simulator(*(this->diagram_));
  simulator.AdvanceTo(0.0);
  MessageResults results = this->ProcessMessages(Role::kProximity);

  // Confirm load message; three geometries on two links.
  ASSERT_EQ(results.num_load, 1);
  ASSERT_EQ(results.load_message.num_links, 2);
  // We exploit the fact that we always know the world is first.
  ASSERT_EQ(results.load_message.link[0].name, "world");
  ASSERT_EQ(results.load_message.link[0].num_geom, 1);

  // Now test for the dynamic frame (with its two geometries).
  ASSERT_EQ(results.load_message.link[1].name,
            string(this->kPoseSourceName) + "::frame");
  ASSERT_EQ(results.load_message.link[1].num_geom, 2);

  // Confirm draw message; a single link.
  ASSERT_EQ(results.num_draw, 1);
  ASSERT_EQ(results.draw_message.num_links, 1);
  ASSERT_EQ(results.draw_message.link_name[0],
            string(this->kPoseSourceName) + "::frame");

  // Confirm deformable messages; no deformable geometry registered.
  ASSERT_EQ(results.num_draw, 1);
  ASSERT_EQ(results.deformable_message.num_geom, 0);
}

/* Confirms that the role parameter leads to the correct geometry being
 selected, and that all roles can be directed to both suffixed channels and the
 legacy/default channel.  */
TYPED_TEST(DrakeVisualizerTest, TargetRole) {
  using T = TypeParam;
  const string source_name(this->kPoseSourceName);
  /* For a given role, the name of the *unique* frame we expect to load. The
   frame should have a single geometry affixed to it.  */
  const map<Role, string> expected{
      {Role::kIllustration, source_name + "::illustration"},
      {Role::kPerception, source_name + "::perception"},
      {Role::kProximity, source_name + "::proximity"}};
  for (const auto& [role, name] : expected) {
    for (const bool use_suffix : {false, true}) {
      SCOPED_TRACE(fmt::format("role '{}', name '{}', use_suffix '{}'", role,
                               name, use_suffix));
      this->ConfigureDiagram({.publish_period = this->kPublishPeriod,
                              .role = role,
                              .use_role_channel_suffix = use_suffix});
      this->PopulateScene();
      Simulator<T> simulator(*(this->diagram_));
      simulator.AdvanceTo(0.0);
      std::optional<Role> maybe_role;
      if (use_suffix) {
        maybe_role = role;
      }
      MessageResults results = this->ProcessMessages(maybe_role);

      /* Confirm that messages were sent.  */
      ASSERT_EQ(results.num_load, 1);
      ASSERT_EQ(results.load_message.num_links, 1);
      EXPECT_EQ(results.load_message.link[0].name, name);
      EXPECT_EQ(results.load_message.link[0].num_geom, 1);

      ASSERT_EQ(results.num_draw, 1);
      ASSERT_EQ(results.draw_message.num_links, 1);
      EXPECT_EQ(results.draw_message.link_name[0], name);

      ASSERT_EQ(results.num_deformable, 1);
      ASSERT_EQ(results.deformable_message.num_geom, 0);
    }
  }
}

/* Confirms that a force publish is sufficient.  */
TYPED_TEST(DrakeVisualizerTest, ForcePublish) {
  this->ConfigureDiagram(
      {.publish_period = this->kPublishPeriod, .role = Role::kIllustration});
  this->PopulateScene();
  auto context = this->diagram_->CreateDefaultContext();
  const auto& vis_context = this->visualizer_->GetMyContextFromRoot(*context);
  this->visualizer_->ForcedPublish(vis_context);

  /* Confirm that messages were sent.  */
  MessageResults results = this->ProcessMessages();
  ASSERT_EQ(results.num_load, 1);
  ASSERT_EQ(results.num_draw, 1);
  ASSERT_EQ(results.num_deformable, 1);
}

/* When targeting a non-illustration role, if that same geometry *has* an
 illustration role with color, that value is used instead of the default.  */
TYPED_TEST(DrakeVisualizerTest, GeometryWithIllustrationFallback) {
  using T = TypeParam;
  this->ConfigureDiagram({.publish_period = this->kPublishPeriod,
                          .role = Role::kProximity,
                          .use_role_channel_suffix = true});
  const GeometryId g_id = this->scene_graph_->RegisterAnchoredGeometry(
      this->pose_source_id_,
      make_unique<GeometryInstance>(RigidTransformd{}, make_unique<Sphere>(1),
                                    "sphere0"));
  const Rgba expected_rgba{0.25, 0.125, 0.75, 0.5};
  ASSERT_NE(expected_rgba, DrakeVisualizerParams().default_color);
  IllustrationProperties illus_props;
  illus_props.AddProperty("phong", "diffuse", expected_rgba);
  this->scene_graph_->AssignRole(this->pose_source_id_, g_id,
                                 ProximityProperties());
  this->scene_graph_->AssignRole(this->pose_source_id_, g_id, illus_props);

  Simulator<T> simulator(*(this->diagram_));
  simulator.AdvanceTo(0.0);

  // We're just checking the load message for the right color.
  MessageResults results = this->ProcessMessages(Role::kProximity);
  ASSERT_EQ(results.num_load, 1);
  ASSERT_EQ(results.load_message.num_links, 1);
  ASSERT_EQ(results.load_message.link[0].num_geom, 1);
  const auto& color = results.load_message.link[0].geom[0].color;
  const Rgba rgba(color[0], color[1], color[2], color[3]);
  EXPECT_EQ(rgba, expected_rgba);
}

/* For *any* role type, if the properties include ("phong", "diffuse"), *that*
 color will be used.  */
TYPED_TEST(DrakeVisualizerTest, AllRolesCanDefineDiffuse) {
  using T = TypeParam;
  for (const Role role : {Role::kProximity, Role::kPerception}) {
    this->ConfigureDiagram({.publish_period = this->kPublishPeriod,
                            .role = role,
                            .use_role_channel_suffix = true});
    const GeometryId g_id = this->scene_graph_->RegisterAnchoredGeometry(
        this->pose_source_id_,
        make_unique<GeometryInstance>(RigidTransformd{}, make_unique<Sphere>(1),
                                      "sphere0"));
    const Rgba expected_rgba{0.25, 0.125, 0.75, 0.5};
    ASSERT_NE(expected_rgba, DrakeVisualizerParams().default_color);
    if (role == Role::kProximity) {
      ProximityProperties props;
      props.AddProperty("phong", "diffuse", expected_rgba);
      this->scene_graph_->AssignRole(this->pose_source_id_, g_id, props);
    } else if (role == Role::kPerception) {
      PerceptionProperties props;
      props.AddProperty("phong", "diffuse", expected_rgba);
      this->scene_graph_->AssignRole(this->pose_source_id_, g_id, props);
    }

    Simulator<T> simulator(*(this->diagram_));
    simulator.AdvanceTo(0.0);

    // We're just checking the load message for the right color.
    MessageResults results = this->ProcessMessages(role);
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.load_message.num_links, 1);
    ASSERT_EQ(results.load_message.link[0].num_geom, 1);
    const auto& color = results.load_message.link[0].geom[0].color;
    const Rgba rgba(color[0], color[1], color[2], color[3]);
    EXPECT_EQ(rgba, expected_rgba);
  }
}

/* Confirms that the documented prerequisites do bad things.  */
TYPED_TEST(DrakeVisualizerTest, BadParameters) {
  using T = TypeParam;
  // Zero publish period.
  EXPECT_THROW(DrakeVisualizer<T>(&(this->lcm_),
                                  DrakeVisualizerParams{0, Role::kIllustration,
                                                        Rgba{1, 1, 1, 1}}),
               std::exception);

  // Negative publish period.
  EXPECT_THROW(
      DrakeVisualizer<T>(
          &(this->lcm_),
          DrakeVisualizerParams{-0.1, Role::kIllustration, Rgba{1, 1, 1, 1}}),
      std::exception);

  // Unassigned role.
  EXPECT_THROW(DrakeVisualizer<T>(&(this->lcm_),
                                  DrakeVisualizerParams{0.1, Role::kUnassigned,
                                                        Rgba{1, 1, 1, 1}}),
               std::exception);
}

/* This confirms that DrakeVisualizer will dispatch a new load message when it
 recognizes a change in the visualized role's version. We'll confirm both the
 positive case (change to expected role triggers load) and the negative case
 (change to other roles does *not* trigger load).  */
TYPED_TEST(DrakeVisualizerTest, ChangesInVersion) {
  using T = TypeParam;
  for (const Role role :
       {Role::kProximity, Role::kPerception, Role::kIllustration}) {
    this->ConfigureDiagram({.publish_period = this->kPublishPeriod,
                            .role = role,
                            .use_role_channel_suffix = true});
    const GeometryId g_id = this->scene_graph_->RegisterAnchoredGeometry(
        this->pose_source_id_,
        make_unique<GeometryInstance>(RigidTransformd{}, make_unique<Sphere>(1),
                                      "sphere0"));

    Simulator<T> simulator(*(this->diagram_));
    Context<T>& diagram_context = simulator.get_mutable_context();
    Context<T>& sg_context = this->diagram_->GetMutableSubsystemContext(
        *(this->scene_graph_), &diagram_context);
    double t = 0.0;
    simulator.AdvanceTo(t);
    // Confirm the initial load/draw message pair.
    MessageResults results = this->ProcessMessages(role);
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.num_draw, 1);
    ASSERT_EQ(results.num_deformable, 1);

    t += this->kPublishPeriod;
    simulator.AdvanceTo(t);
    // Confirm draw only.
    results = this->ProcessMessages(role);
    ASSERT_EQ(results.num_load, 0);
    ASSERT_EQ(results.num_draw, 1);
    ASSERT_EQ(results.num_deformable, 1);

    // Confirm that modifying a role has the expected outcome. If the modified
    // role matches the visualized `role`, we expect a load message. Otherwise
    // expect no load message.
    // We don't exhaustively compare all three roles. The only way to get
    // perception version changes is to have an actual render engine that
    // accepts geometry. We'll avoid the overhead and assume that successful
    // interactions of other role-role matchups suggest that it's likewise true
    // for a modified perception role.
    for (const Role modified_role : {Role::kProximity, Role::kIllustration}) {
      if (modified_role == Role::kProximity) {
        this->scene_graph_->AssignRole(&sg_context, this->pose_source_id_, g_id,
                                       ProximityProperties());
      } else if (modified_role == Role::kIllustration) {
        this->scene_graph_->AssignRole(&sg_context, this->pose_source_id_, g_id,
                                       IllustrationProperties());
      }
      t += this->kPublishPeriod;
      simulator.AdvanceTo(t);
      results = this->ProcessMessages(role);
      EXPECT_EQ(results.num_load, modified_role == role ? 1 : 0)
          << "For visualized role '" << role << "' and modified role '" << role
          << "'\n";
      EXPECT_EQ(results.num_draw, 1);
      EXPECT_EQ(results.num_deformable, 1);
    }
  }
}

/* This tests DrakeVisualizer's ability to broadcast messages about deformable
 geometries. We'll test this by evaluating a single load message with a
 SceneGraph populated with a deformable sphere.

 In the case where deformable mesh geometry is sent, we also explicitly test
 the mesh name and color information (as DrakeVisualizer uses bespoke code in
 this regard). In particular, we configure the geometry to have different a
 particular diffuse color for proximity roles while not specifying any color for
 illustration roles. We expect that the specified color is respected for the
 former while the default color of the visualizer is used for the latter.

 We are testing for the presence of the expected *type* of message, but we are
 not evaluating the mesh data produced. We assume that incorrectness in the
 mesh data will be immediately visible in visualization. */
TYPED_TEST(DrakeVisualizerTest, VisualizeDeformableGeometry) {
  for (const auto role :
       std::vector<Role>{Role::kProximity, Role::kIllustration}) {
    DrakeVisualizerParams params;
    /* We'll expect the visualizer default color gets applied to the deformable
     meshes if a diffuse color isn't specifically requested via
     GeometryProperties. */
    params.default_color = Rgba{0.25, 0.5, 0.75, 0.5};
    params.role = role;
    this->ConfigureDiagram(params);

    const Rgba expected_illustration_rgba = params.default_color;
    const Rgba expected_proximity_rgba = Rgba(0.75, 0.25, 0.5, 0.75);

    constexpr double kRezHint = 0.5;
    constexpr double kRadius = 1.0;
    auto geometry_instance = make_unique<GeometryInstance>(
        RigidTransformd::Identity(), make_unique<Sphere>(kRadius), "sphere");
    geometry_instance->set_illustration_properties(IllustrationProperties{});
    ProximityProperties proximity_properties;
    proximity_properties.AddProperty("phong", "diffuse",
                                     expected_proximity_rgba);
    geometry_instance->set_proximity_properties(proximity_properties);
    GeometryId g_id = this->scene_graph_->RegisterDeformableGeometry(
        this->configuration_source_id_, this->scene_graph_->world_frame_id(),
        std::move(geometry_instance), kRezHint);
    const auto& inspector = this->scene_graph_->model_inspector();
    const VolumeMesh<double>* mesh = inspector.GetReferenceMesh(g_id);
    ASSERT_NE(mesh, nullptr);
    this->configuration_source_->SetConfigurations(
        {{g_id, VectorX<double>::Zero(3 * mesh->num_vertices())}});

    /* Dispatch a load message. */
    auto context = this->diagram_->CreateDefaultContext();
    const auto& vis_context = this->visualizer_->GetMyContextFromRoot(*context);
    this->visualizer_->ForcedPublish(vis_context);

    /* Confirm that messages were sent.  */
    MessageResults results = this->ProcessMessages();
    ASSERT_EQ(results.num_load, 1);
    /* No rigid geometries are added. */
    ASSERT_EQ(results.load_message.num_links, 0);
    ASSERT_EQ(results.num_deformable, 1);
    ASSERT_EQ(results.deformable_message.num_geom, 1);
    EXPECT_EQ(results.deformable_message.name, "deformable_geometries");
    EXPECT_EQ(results.deformable_message.robot_num, 0);

    auto is_visualizer_color = [](const lcmt_viewer_geometry_data& message,
                                  const Rgba expected) {
      const auto& color = message.color;
      const Rgba test_rgba(color[0], color[1], color[2], color[3]);
      // The color values used in this test can all be perfectly represented by
      // 32-bit floats (e.g., 0.5, 0.75,  etc.). So, we can use exact equality.
      return expected == test_rgba;
    };

    const auto& geo_message = results.deformable_message.geom[0];
    EXPECT_EQ(geo_message.type, geo_message.MESH);
    EXPECT_EQ(geo_message.string_data, "sphere");
    EXPECT_EQ(geo_message.num_float_data, geo_message.float_data.size());
    EXPECT_EQ(geo_message.float_data.size(), 2 + geo_message.float_data[0] * 3 +
                                                 geo_message.float_data[1] * 3);
    EXPECT_TRUE(is_visualizer_color(
        geo_message, role == Role::kProximity ? expected_proximity_rgba
                                              : expected_illustration_rgba));
  }
}

/* This tests DrakeVisualizer's ability to replace primitive shape declarations
 with discrete meshes for proximity geometries with a hydroelastic mesh
 representation. This test focuses on:

 - Rigid shapes are serialized as mesh with data (not path).
 - Soft shapes are serialized as mesh with data (not path).
 - Rigid HalfSpace does not get replaced.
 - Soft HalfSpace does not get replaced.
 - Geometry w/o hydroelastic representation is preserved.

 We'll test this by evaluating a single load message with a SceneGraph populated
 with:
  - a rigid cube
  - a soft sphere
  - rigid and soft half spaces
  - an ellipsoid with no hydro representation.

 In the case where hydroelastic mesh geometry is sent, we also explicitly test
 the mesh pose and color information (as DrakeVisualizer uses bespoke code in
 this regard).

 We are testing for the presence of the expected *type* of message, but we are
 not evaluating the mesh data produced. We assume that incorrectness in the
 mesh data will be immediately visible in visualization. */
TYPED_TEST(DrakeVisualizerTest, VisualizeHydroGeometry) {
  using T = TypeParam;

  DrakeVisualizerParams params;
  /* We'll expect the visualizer default color gets applied to the hydroelastic
   meshes -- we haven't defined any other color to the geometry. So, we'll pick
   an arbitrary value that *isn't* the default value. */
  params.default_color = Rgba{0.25, 0.5, 0.75, 0.5};
  params.role = Role::kProximity;
  params.show_hydroelastic = true;
  params.use_role_channel_suffix = true;
  this->ConfigureDiagram(params);

  FramePoseVector<T> poses;
  auto add_geometry = [this, &poses](auto shape_u_p, const std::string& name,
                                     const ProximityProperties& properties,
                                     const RigidTransformd& X_PG = {}) {
    const FrameId f_id = this->scene_graph_->RegisterFrame(
        this->pose_source_id_, GeometryFrame(name, 0));
    const GeometryId g_id = this->scene_graph_->RegisterGeometry(
        this->pose_source_id_, f_id,
        make_unique<GeometryInstance>(X_PG, std::move(shape_u_p), name));
    this->scene_graph_->AssignRole(this->pose_source_id_, g_id, properties);
    poses.set_value(f_id, RigidTransform<T>{});
  };

  ProximityProperties props;
  add_geometry(make_unique<Ellipsoid>(1, 2, 3), "ellipsoid", props);

  /* Populate with rigid hydroelastic properties and add rigid geometries. */
  using internal::HydroelasticType;
  props.AddProperty(internal::kHydroGroup, internal::kRezHint, 5.0);
  props.AddProperty(internal::kHydroGroup, internal::kComplianceType,
                    HydroelasticType::kRigid);
  const RigidTransformd X_PBox{RotationMatrixd::MakeYRotation(0.25),
                               Vector3d{1.25, 2.5, 3.75}};
  add_geometry(make_unique<Box>(1, 1, 1), "box", props, X_PBox);
  add_geometry(make_unique<HalfSpace>(), "rigid_half_space", props);

  /* Populate with compliant hydroelastic properties and add
     compliant geometries.
  */
  props.AddProperty(internal::kHydroGroup, internal::kSlabThickness, 5.0);
  props.AddProperty(internal::kHydroGroup, internal::kElastic, 5.0);
  props.UpdateProperty(internal::kHydroGroup, internal::kComplianceType,
                       HydroelasticType::kSoft);
  const RigidTransformd X_PSphere{RotationMatrixd::MakeZRotation(0.3),
                                  Vector3d{2.5, 1.25, -3.75}};
  add_geometry(make_unique<Sphere>(1), "sphere", props, X_PSphere);
  add_geometry(make_unique<HalfSpace>(), "soft_half_space", props);

  this->pose_source_->SetPoses(std::move(poses));

  /* Dispatch a load message. */
  auto context = this->diagram_->CreateDefaultContext();
  const auto& vis_context = this->visualizer_->GetMyContextFromRoot(*context);
  this->visualizer_->ForcedPublish(vis_context);

  /* Confirm that messages were sent.  */
  MessageResults results = this->ProcessMessages(params.role);
  ASSERT_EQ(results.num_load, 1);
  ASSERT_EQ(results.load_message.num_links, 5);

  auto is_visualizer_color = [expected = params.default_color](
                                 const lcmt_viewer_geometry_data& message) {
    const auto& color = message.color;
    const Rgba test_rgba(color[0], color[1], color[2], color[3]);
    // The color values used in this test can all be perfectly represented by
    // 32-bit floats (e.g., 0.5, 0.75,  etc.). So, we can use exact equality.
    return expected == test_rgba;
  };

  auto pose_matches = [](const RigidTransformd& X_PG_expected,
                         const lcmt_viewer_geometry_data& message) {
    const auto& p_PG = message.position;
    const auto& q_PG = message.quaternion;
    const RotationMatrixd R_PG(
        Eigen::Quaternion<double>(q_PG[0], q_PG[1], q_PG[2], q_PG[3]));
    const RigidTransformd X_PG_test(R_PG, {p_PG[0], p_PG[1], p_PG[2]});
    /* Tolerance due to conversion to float. */
    return CompareMatrices(X_PG_expected.GetAsMatrix34(),
                           X_PG_test.GetAsMatrix34(), 1e-7);
  };

  /* We're going to make sure that:
    - Where we've converted to a mesh, the lcm message reports mesh type,
      empty string_data, and non-empty float_data.
    - Again, we're *not* examining the details of the float data (except for
      coherent sizes reported); we'll rely on visual inspection to alert us as
      to whether the mesh is "correct" or not. */
  for (int i = 0; i < results.load_message.num_links; ++i) {
    const auto& link_message = results.load_message.link[i];
    EXPECT_EQ(link_message.num_geom, 1);
    const auto& geo_message = link_message.geom[0];
    if (link_message.name == fmt::format("{}::box", this->kPoseSourceName)) {
      EXPECT_EQ(geo_message.type, geo_message.MESH);
      EXPECT_TRUE(geo_message.string_data.empty());
      EXPECT_GT(geo_message.num_float_data, 2);
      EXPECT_EQ(
          geo_message.float_data.size(),
          2 + geo_message.float_data[0] * 3 + geo_message.float_data[1] * 3);
      EXPECT_TRUE(is_visualizer_color(geo_message));
      EXPECT_TRUE(pose_matches(X_PBox, geo_message));
    } else if (link_message.name ==
               fmt::format("{}::sphere", this->kPoseSourceName)) {
      EXPECT_EQ(geo_message.type, geo_message.MESH);
      EXPECT_TRUE(geo_message.string_data.empty());
      EXPECT_GT(geo_message.num_float_data, 2);
      EXPECT_EQ(
          geo_message.float_data.size(),
          2 + geo_message.float_data[0] * 3 + geo_message.float_data[1] * 3);
      EXPECT_TRUE(is_visualizer_color(geo_message));
      EXPECT_TRUE(pose_matches(X_PSphere, geo_message));
    } else if (link_message.name ==
               fmt::format("{}::soft_half_space", this->kPoseSourceName)) {
      /* In drake_visualizer, half spaces are big, flat boxes. */
      EXPECT_EQ(geo_message.type, geo_message.BOX);
    } else if (link_message.name ==
               fmt::format("{}::rigid_half_space", this->kPoseSourceName)) {
      EXPECT_EQ(geo_message.type, geo_message.BOX);
    } else if (link_message.name ==
               fmt::format("{}::ellipsoid", this->kPoseSourceName)) {
      EXPECT_EQ(geo_message.type, geo_message.ELLIPSOID);
    } else {
      GTEST_FAIL() << "Link encountered which wasn't consumed as part of the "
                      "expected links: "
                   << link_message.name;
    }
  }

  /* We don't care about the draw message. */
}

/* This confirms that DrakeVisualizer dispatches a faceted convex hull for
 Convex shapes. */
TYPED_TEST(DrakeVisualizerTest, ConvexIsHullAlways) {
  this->template ExpectMeshInMessage<Convex>(Role::kProximity,
                                             /* expect_hull = */ true);
  this->template ExpectMeshInMessage<Convex>(Role::kIllustration,
                                             /* expect_hull = */ true);
  this->template ExpectMeshInMessage<Convex>(Role::kProximity,
                                             /* expect_hull = */ true,
                                             /* in_memory = */ true);
  this->template ExpectMeshInMessage<Convex>(Role::kIllustration,
                                             /* expect_hull = */ true,
                                             /* in_memory = */ true);
}

/* This confirms that DrakeVisualizer dispatches a faceted convex hull for
 Mesh shapes with proximity role, but the mesh name for non-proximity role. */
TYPED_TEST(DrakeVisualizerTest, MeshIsHullForProximity) {
  this->template ExpectMeshInMessage<Mesh>(Role::kProximity,
                                           /* expect_hull = */ true);
  this->template ExpectMeshInMessage<Mesh>(Role::kIllustration,
                                           /* expect_hull = */ false);
}

/* When a Mesh contains an in-memory representations, confirm that it encodes
 as the expected json. */
TYPED_TEST(DrakeVisualizerTest, InMemoryMesh) {
  this->template ExpectMeshInMessage<Mesh>(Role::kProximity,
                                           /* expect_hull = */ true,
                                           /* in_memory = */ true);

  this->template ExpectMeshInMessage<Mesh>(Role::kIllustration,
                                           /* expect_hull = */ false,
                                           /* in_memory = */ true);
}

/* Tests the AddToBuilder method that connects directly to a provided SceneGraph
 instance -- see TestAddToBuilder for testing details.  */
template <typename T>
const DrakeVisualizer<T>& add_to_builder_sg(DiagramBuilder<T>* builder,
                                            const SceneGraph<T>& scene_graph,
                                            DrakeLcmInterface* lcm,
                                            DrakeVisualizerParams params) {
  return DrakeVisualizer<T>::AddToBuilder(builder, scene_graph, lcm, params);
}

template <typename T>
const DrakeVisualizer<T>& add_to_builder_sg_no_params(
    DiagramBuilder<T>* builder, const SceneGraph<T>& scene_graph,
    DrakeLcmInterface* lcm) {
  return DrakeVisualizer<T>::AddToBuilder(builder, scene_graph, lcm);
}

template <typename T>
const SceneGraph<T>& pose_source_sg(const SceneGraph<T>& sg) {
  return sg;
}

TYPED_TEST(DrakeVisualizerTest, AddToBuilderSceneGraph) {
  using T = TypeParam;
  this->TestAddToBuilder(add_to_builder_sg<T>, add_to_builder_sg_no_params<T>,
                         pose_source_sg<T>);
}

/* Tests the AddToBuilder method that connects directly to a provided
 QueryObject-valued port -- see TestAddToBuilder for testing details.  */
template <typename T>
const DrakeVisualizer<T>& add_to_builder_port(
    DiagramBuilder<T>* builder, const systems::OutputPort<T>& port,
    DrakeLcmInterface* lcm, DrakeVisualizerParams params) {
  return DrakeVisualizer<T>::AddToBuilder(builder, port, lcm, params);
}

template <typename T>
const DrakeVisualizer<T>& add_to_builder_port_no_params(
    DiagramBuilder<T>* builder, const systems::OutputPort<T>& port,
    DrakeLcmInterface* lcm) {
  return DrakeVisualizer<T>::AddToBuilder(builder, port, lcm);
}
template <typename T>
const systems::OutputPort<T>& pose_source_port(const SceneGraph<T>& sg) {
  return sg.get_query_output_port();
}

TYPED_TEST(DrakeVisualizerTest, AddToBuilderQueryObjectPort) {
  using T = TypeParam;
  this->TestAddToBuilder(add_to_builder_port<T>,
                         add_to_builder_port_no_params<T>, pose_source_port<T>);
}

/* Confirms that DispatchLoadMessage works on the scene graph model. We need to
 test the following:

   1. The contents of the SceneGraph model are broadcast.
   2. The provided parameters are used.
   3. The given lcm is used.

 Operating with the understanding that this API uses the same machinery that
 the system itself uses in its event handler, we just need evidence that the
 machinery is being meaningfully exercised.

 So, we'll populate a SceneGraph and broadcast twice, changing the role. We
 confirm 1 & 2 simultaneously by looking for the single frame with a name
 that depends on the role broadcast. We confirm 3 in that we get messages at
 all. */
TYPED_TEST(DrakeVisualizerTest, DispatchLoadMessageFromModel) {
  using T = TypeParam;
  this->ConfigureDiagram();
  this->PopulateScene();

  {
    // Case: role = illustration (via default) --> one frame labeled with
    // "illustration".
    DrakeVisualizer<T>::DispatchLoadMessage(*(this->scene_graph_),
                                            &(this->lcm_));
    MessageResults results = this->ProcessMessages();
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.load_message.num_links, 1);
    ASSERT_EQ(results.load_message.link[0].name,
              string(this->kPoseSourceName) + "::illustration");
    ASSERT_EQ(results.load_message.link[0].num_geom, 1);
    ASSERT_EQ(results.num_draw, 0);
    ASSERT_EQ(results.num_deformable, 0);
  }
  {
    // Case: role = proximity --> one frame labeled with "proximity".
    DrakeVisualizerParams params;
    params.role = Role::kProximity;
    params.use_role_channel_suffix = true;
    DrakeVisualizer<T>::DispatchLoadMessage(*(this->scene_graph_),
                                            &(this->lcm_), params);
    MessageResults results = this->ProcessMessages(params.role);
    ASSERT_EQ(results.num_load, 1);
    ASSERT_EQ(results.load_message.num_links, 1);
    ASSERT_EQ(results.load_message.link[0].name,
              string(this->kPoseSourceName) + "::proximity");
    ASSERT_EQ(results.load_message.link[0].num_geom, 1);
    ASSERT_EQ(results.num_draw, 0);
    ASSERT_EQ(results.num_deformable, 0);
  }
}

/* Confirm transmogrification logic. Specifically:
  - If the source owns its lcm (it must be a DrakeLcm), the system can be
    transmogrified AND
      - the result owns its own DrakeLcm.
      - It is a different instance than the source.
      - they have the same URL.
  - If the source does *not* own its lcm (whether it is an actual DrakeLcm or
    not) converting throws.  */
GTEST_TEST(DrakeVisualizerdTest, Transmogrify) {
  using Tester = DrakeVisualizerTester;

  // DrakeVisualizer which owns its lcm.
  DrakeVisualizerd vis_own_lcm_d;
  auto sys_own_lcm_ad = vis_own_lcm_d.ToAutoDiffXd();
  auto* vis_own_lcm_ad =
      dynamic_cast<DrakeVisualizer<AutoDiffXd>*>(sys_own_lcm_ad.get());
  ASSERT_NE(vis_own_lcm_ad, nullptr);
  ASSERT_NE(Tester::owned_lcm_interface(*vis_own_lcm_ad), nullptr);
  ASSERT_NE(Tester::active_lcm_interface(vis_own_lcm_d),
            Tester::owned_lcm_interface(*vis_own_lcm_ad));
  ASSERT_EQ(Tester::lcm_url(vis_own_lcm_d), Tester::lcm_url(*vis_own_lcm_ad));

  // DrakeVisualizer does not own DrakeLcm implementation.
  lcm::DrakeLcm lcm;
  DrakeVisualizerd vis_not_own_lcm_d(&lcm);
  DRAKE_EXPECT_THROWS_MESSAGE(
      vis_not_own_lcm_d.ToAutoDiffXd(),
      "DrakeVisualizer can only be scalar converted if it owns its "
      "DrakeLcmInterface instance.");
}

// The Graphviz should have an arrow pointing to the DrakeLcmInterface.
GTEST_TEST(DrakeVisualizerdTest, Graphviz) {
  DrakeVisualizerd dut;
  EXPECT_THAT(dut.GetGraphvizString(), testing::HasSubstr(" -> drakelcm"));
}

}  // namespace
}  // namespace geometry
}  // namespace drake
