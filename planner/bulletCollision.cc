#include "collision.hh"
#ifndef USE_FCL
#include <algorithm>
#include <fstream>

#include <fmt/ostream.h>

// clang-format off
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
// clang-format on

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "fplus/fplus.hpp"

#include <ompl/base/spaces/RealVectorStateSpace.h>
#include <ompl/base/spaces/SE2StateSpace.h>
#include <ompl/base/spaces/SE3StateSpace.h>
#include <ompl/base/spaces/SO2StateSpace.h>

#include <nlohmann/json.hpp>

namespace planner::collisions {
using json = nlohmann::json;
namespace {
  struct HackyDrawer : public btIDebugDraw {
    void drawLine(const btVector3& from, const btVector3& to, const btVector3& color) override {
      json jFrom = {{"x", from.x()}, {"y", from.y()}, {"z", from.z()}};
      json jTo   = {{"x", to.x()}, {"y", to.y()}, {"z", to.z()}};
      output.push_back({{"type", "CollisionLine"},
                        {"from", jFrom},
                        {"to", jTo},
                        {"r", color.x()},
                        {"g", color.y()},
                        {"b", color.z()}});
    }

    void drawContactPoint(const btVector3& PointOnB,
                          const btVector3& normalOnB,
                          btScalar distance,
                          int lifeTime,
                          const btVector3& color) override {
      output.push_back({{"type", "CollisionPoint"},
                        {"at", {{"x", PointOnB.x()}, {"y", PointOnB.y()}, {"z", PointOnB.z()}}},
                        {"r", color.x()},
                        {"g", color.y()},
                        {"b", color.z()}});
    }

    void reportErrorWarning(const char* warningString) override { spdlog::error(warningString); }

    void draw3dText(const btVector3& location, const char* textString) override {
      output.push_back({{"type", "CollisionText"},
                        {"msg", textString},
                        {"at", {{"x", location.x()}, {"y", location.y()}, {"z", location.z()}}}});
    }

    void setDebugMode(int debugMode) override { this->debugMode = debugMode; }

    int getDebugMode() const override { return debugMode; }

    void flushTo(const Str& filename) {
      std::ofstream out_file(filename);
      out_file << output;
      out_file.close();
      output.clear();
    }

    json output;
    int debugMode;
  };

  Map<Str, const structures::scenegraph::Node*> node_map;
}  // namespace

unsigned int oob_count        = 0;
unsigned int self_coll_count  = 0;
unsigned int world_coll_count = 0;
// std::unordered_map<std::pair<Str, Str>, unsigned int, boost::hash<std::pair<Str, Str>>>
// collision_counters;
// std::ofstream* collisions_file  = nullptr;
// std::ofstream* sample_data_file = nullptr;

void to_json(json& j, const Str& name, const btTransform& tf) {
  const auto& translation = tf.getOrigin();
  const auto& rotation    = tf.getRotation();
  j["name"]               = name;
  j["translation"]        = {translation.x(), translation.y(), translation.z()};
  j["rotation"]           = {rotation.x(), rotation.y(), rotation.z(), rotation.w()};
}

void BulletCollisionChecker::output_json() const {
  std::ofstream json_file("collision_state.json");
  json output;
  for (const auto& [name, obj] : object_collisions) {
    json pose;
    to_json(pose, name, obj->getWorldTransform());
    output.push_back(pose);
  }

  for (const auto& [name, obj] : robot_collisions) {
    json pose;
    to_json(pose, name, obj->getWorldTransform());
    output.push_back(pose);
  }

  json_file << output;
  json_file.close();
}

inline void write_sample_data(const Transform3r& base_tf,
                              const Map<Str, double>& joint_data,
                              const Map<Str, Transform3r>& pose_data,
                              const Vec<std::pair<Str, Str>>& colliding_links) {
  json output;
  const auto& base_translation = base_tf.translation();
  const Eigen::Quaterniond base_rotation(base_tf.linear());
  output["base_tf"] =
  json{{"translation", {base_translation.x(), base_translation.y(), base_translation.z()}},
       {"rotation", {base_rotation.x(), base_rotation.y(), base_rotation.z(), base_rotation.w()}}};
  output["joints"]     = joint_data;
  output["collisions"] = colliding_links;
  Map<Str, json> poses;
  for (const auto& [name, tf] : pose_data) {
    const auto& translation = tf.translation();
    const Eigen::Quaterniond rotation(tf.linear());
    poses.emplace(name,
                  json{{"translation", {translation.x(), translation.y(), translation.z()}},
                       {"rotation", {rotation.x(), rotation.y(), rotation.z(), rotation.w()}}});
  }

  output["poses"] = poses;
  // *sample_data_file << output << ",";
}

NeighborLinksFilter::NeighborLinksFilter(const std::optional<Str>& blacklist_path,
                                         std::size_t num_items,
                                         Graph* sg)
: sg(sg) {
  // Build the blacklist
  if (blacklist_path) {
    std::size_t idx = 0;
    std::ifstream blacklist_file(blacklist_path.value());
    Str link_a;
    Str link_b;
    Str line;
    while (blacklist_file >> line) {
      const auto links                        = fplus::split(',', true, line);
      link_a                                  = links[0];
      link_b                                  = links[1];
      const auto& [link_a_idx_it, link_a_new] = index_map.try_emplace(link_a, idx);
      if (link_a_new) {
        ++idx;
        blacklist.emplace_back(num_items, 0);
      }

      const auto& [link_b_idx_it, link_b_new] = index_map.try_emplace(link_b, idx);
      if (link_b_new) {
        ++idx;
        blacklist.emplace_back(num_items, 0);
      }

      auto& link_a_mask = blacklist.at(link_a_idx_it->second);
      auto& link_b_mask = blacklist.at(link_b_idx_it->second);

      link_a_mask.set(link_b_idx_it->second, true);
      link_b_mask.set(link_a_idx_it->second, true);
    }
  }
}

bool NeighborLinksFilter::needBroadphaseCollision(btBroadphaseProxy* proxy0,
                                                  btBroadphaseProxy* proxy1) const {
  bool collides    = (proxy0->m_collisionFilterGroup & proxy1->m_collisionFilterMask) != 0;
  collides         = collides && (proxy1->m_collisionFilterGroup & proxy0->m_collisionFilterMask);
  const auto* obj1 = static_cast<btCollisionObject*>(proxy0->m_clientObject);
  const auto* obj2 = static_cast<btCollisionObject*>(proxy1->m_clientObject);

  const auto idx1 = obj1->getUserIndex();
  const auto idx2 = obj2->getUserIndex();
  collides =
  collides && ((idx1 < 0) || (idx2 < 0) || !blacklist[idx1][idx2]);
  // collides = collides && blacklist.find({std::min(*name1, *name2), std::max(*name1, *name2)}) ==
  //                        blacklist.end();

  const auto* name1 = static_cast<Str*>(obj1->getUserPointer());
  const auto* name2 = static_cast<Str*>(obj2->getUserPointer());
  const auto& node1 = sg->find(*name1);
  const auto& node2 = sg->find(*name2);
  collides          = collides && !(node1.has_child(node2) || node2.has_child(node1));

  return collides;
}

// void near_callback(btBroadphasePair& collisionPair,
//                    btCollisionDispatcher& dispatcher,
//                    const btDispatcherInfo& dispatchInfo) {
//   const auto* proxy0   = collisionPair.m_pProxy0;
//   const auto* proxy1   = collisionPair.m_pProxy1;
//   const auto* obj1     = static_cast<btCollisionObject*>(proxy0->m_clientObject);
//   const auto* obj2     = static_cast<btCollisionObject*>(proxy1->m_clientObject);
//   const auto* name1    = static_cast<Str*>(obj1->getUserPointer());
//   const auto* name2    = static_cast<Str*>(obj2->getUserPointer());
//   const auto& node1_it = node_map.find(*name1);
//   const auto& node2_it = node_map.find(*name2);
//   bool valid_collision = (node1_it == node_map.end() || node2_it == node_map.end() ||
//                           (!node1_it->second->has_child(node2_it->second) &&
//                            !node2_it->second->has_child(node1_it->second)));
//   if (valid_collision) {
//     dispatcher.defaultNearCallback(collisionPair, dispatcher, dispatchInfo);
//   }
// }

int counter = 0;
bool BulletCollisionChecker::isValid(const ob::State* state) const {
  // Check bounds
  if (!si->satisfiesBounds(state)) {
    ++oob_count;
    // spdlog::error("Invalid bounds!");
    return false;
  }

  // return true;
  // Get the state into the type we want
  const auto cstate      = state->as<cspace::CompositeSpace::StateType>();
  const auto robot_state = cstate->as<ob::CompoundState>(robot_index);

  // Check that no robot links are colliding with: Each other, any non-held objects
  // Do FK to find robot pose for proposed state
  const auto& joint_state = robot_state->as<ob::RealVectorStateSpace::StateType>(joints_index);
  double cont_vals[cspace::cont_joint_idxs.size()];
  double* joint_vals  = nullptr;
  Transform3r base_tf = *robot->base_pose;
  util::state_to_pose_data(robot_state,
                           joint_state,
                           cspace::cont_joint_idxs,
                           space->base_space_idx,
                           cont_vals,
                           &joint_vals,
                           &base_tf);
  // Map<Str, double> joint_data;
  // for (size_t i = 0; i < cspace::cont_joint_idxs.size(); ++i) {
  //   const auto idx          = cspace::cont_joint_idxs[i];
  //   const auto& joint_space = robot_space->getSubspace(idx);
  //   joint_data.emplace(joint_space->getName(), cont_vals[i]);
  // }
  //
  // const auto& joint_space =
  // robot_space->getSubspace(joints_index)->as<cspace::RobotJointSpace>(); for (size_t i = 0; i <
  // joint_space->getDimension(); ++i) {
  //   const auto& dim_name = joint_space->getDimensionName(i);
  //   joint_data.emplace(dim_name, joint_vals[i]);
  // }

  // Compute pose for movable objects
  const auto* objects_state = cstate->as<ob::CompoundStateSpace::StateType>(objects_index);
  Map<Str, Transform3r> pose_map;
  pose_map.reserve(objects_space->getSubspaceCount());
  util::state_to_pose_map(objects_state, objects_space, pose_map);
  cstate->sg->pose_objects(pose_map);
  node_map.reserve(object_collisions.size() + robot_collisions.size());

  // Map<Str, Transform3r> link_poses;
  const auto pose_helper =
  [&](const auto* const node, const auto robot_ancestor, const auto& tf, const auto& coll_tf) {
    if (node->geom != nullptr && !node->is_obstacle) {
      const auto& translation = coll_tf.translation();
      const Eigen::Quaterniond rotation(coll_tf.linear());

      btCollisionObject* collision_obj = nullptr;
      if (node->is_object) {
        // collision_obj = object_collisions.at(node->name).get();
      } else {
        collision_obj = robot_collisions.at(node->name).get();
        // link_poses.insert_or_assign(node->name, coll_tf);
      }

      // collision_world->removeCollisionObject(collision_obj);
      if (collision_obj != nullptr) {
        auto trans = collision_obj->getWorldTransform();
        trans.setOrigin(btVector3(translation.x(), translation.y(), translation.z()));
        trans.setRotation(btQuaternion(rotation.x(), rotation.y(), rotation.z(), rotation.w()));
        collision_obj->setWorldTransform(trans);
        // collision_world->addCollisionObject(collision_obj);
      }
      node_map.emplace(node->name, node);
    }
  };

  cstate->sg->update_transforms<double>(cont_vals, joint_vals, base_tf, pose_helper);
  broadphase_filter->sg = cstate->sg;

  // Check collisions
  collision_world->updateAabbs();
  // const auto filter_callback = std::make_unique<NeighborLinksFilter>(&node_map);
  // collision_world->getPairCache()->setOverlapFilterCallback(filter_callback.get());
  // HackyDrawer dbDraw;
  // dbDraw.setDebugMode(1 | 8 | 2 | 64);
  // collision_world->setDebugDrawer(&dbDraw);
  collision_world->performDiscreteCollisionDetection();
  // collision_world->debugDrawWorld();
  // dbDraw.flushTo("coll_debug.json");
  const auto num_manifolds = collision_world->getDispatcher()->getNumManifolds();
  // int sum_contacts         = 0;
  // int not_close_enough     = 0;

  // Vec<std::pair<Str, Str>> collision_pairs;
  for (int i = 0; i < num_manifolds; ++i) {
    auto* manifold          = collision_world->getDispatcher()->getManifoldByIndexInternal(i);
    const auto num_contacts = manifold->getNumContacts();
    // sum_contacts += num_contacts;
    const auto* obA = static_cast<const btCollisionObject*>(manifold->getBody0());
    const auto* obB = static_cast<const btCollisionObject*>(manifold->getBody1());
    for (int j = 0; j < num_contacts; ++j) {
      const auto& point = manifold->getContactPoint(j);
      if (point.getDistance() <= -PENETRATION_EPSILON) {
        // We have a collision!
        auto* name1 = static_cast<Str*>(obA->getUserPointer());
        auto* name2 = static_cast<Str*>(obB->getUserPointer());
        // collision_pairs.emplace_back(std::make_pair(*name1, *name2));
        const auto* node1 = node_map[*name1];
        const auto* node2 = node_map[*name2];
        // const auto& [counter_elem, _] =
        // collision_counters.emplace(std::make_pair(*name1, *name2), 0);
        // ++counter_elem->second;

        if ((node1 != nullptr && node2 != nullptr) && (!node1->is_object && !node1->is_obstacle) &&
            (!node2->is_object && !node2->is_obstacle)) {
          ++self_coll_count;
        } else {
          ++world_coll_count;
        }

        // else {
        //   *collisions_file
        //   << base_tf.translation().x() << "," << base_tf.translation().y() << ","
        //   << ((node1 == nullptr || node1->is_object || node1->is_obstacle) ? *name1 : *name2)
        //   << std::endl;
        //   ++world_coll_count;
        // }

        // spdlog::warn("{} collides with {}: {}", *name1, *name2, point.getDistance());
        // const auto& tfA = obA->getWorldTransform();
        // const auto& tfB = obB->getWorldTransform();
        // spdlog::warn("{}, {}, {} -- {}, {}, {}, {}",
        //              tfA.getOrigin().x(),
        //              tfA.getOrigin().y(),
        //              tfA.getOrigin().z(),
        //              tfA.getRotation().x(),
        //              tfA.getRotation().y(),
        //              tfA.getRotation().z(),
        //              tfA.getRotation().w());
        // spdlog::warn("{}, {}, {} -- {}, {}, {}, {}",
        //              tfB.getOrigin().x(),
        //              tfB.getOrigin().y(),
        //              tfB.getOrigin().z(),
        //              tfB.getRotation().x(),
        //              tfB.getRotation().y(),
        //              tfB.getRotation().z(),
        //              tfB.getRotation().w());
        // output_json();
        // collision_world->debugDrawWorld();
        // dbDraw.flushTo(fmt::format("coll_debug{}.json", counter));

        ++counter;
        // if (counter > 20) {
        //   throw std::runtime_error("Enough!");
        // }
        collision_dispatch->clearManifold(manifold);
        return false;
      }
      // ++not_close_enough;
    }

    collision_dispatch->clearManifold(manifold);
  }

  // if (any_colls) {
  //   write_sample_data(base_tf, joint_data, link_poses, collision_pairs);
  //   return false;
  // }
  // if (world_coll_count > 0) {
  //   collision_world->debugDrawWorld();
  //   dbDraw.flushTo(fmt::format("coll_debug{}.json", counter));
  //   spdlog::error("Counter is {}", counter);
  //   throw std::runtime_error("Fuckitall");
  //   return false;
  // }

  // spdlog::warn("{} manifolds\t{} contacts\t{} not close enough\tno collisions",
  //              num_manifolds,
  //              sum_contacts,
  //              not_close_enough);
  // if (num_manifolds > 0 && sum_contacts == 0) {
  //   collision_world->debugDrawWorld();
  //   dbDraw.flushTo(fmt::format("coll_debug{}.json", counter));
  //   ++counter;
  //   if (counter > 5) {
  //     throw std::runtime_error("Fucky no-contact scenario");
  //   }
  // }

  return true;
}

BulletCollisionChecker::BulletCollisionChecker(const ob::SpaceInformationPtr& si,
                                               const scene::ObjectSet& objects,
                                               const scene::ObjectSet& obstacles,
                                               const Robot* robot,
                                               const std::optional<Str>& blacklist_path,
                                               Graph* sg)
: CollisionChecker(si, robot)
, collision_config(std::make_unique<btDefaultCollisionConfiguration>())
, collision_dispatch(std::make_unique<btCollisionDispatcher>(collision_config.get())) {
  // Initialize Bullet machinery
  broadphase_interface = std::make_unique<btDbvtBroadphase>();
  collision_world      = std::make_unique<btCollisionWorld>(collision_dispatch.get(),
                                                       broadphase_interface.get(),
                                                       collision_config.get());

  // This is a little gross (double-loop through the robot nodes) but (a) it's a ctor that runs
  // once and (b) it lets us get an accurate link count for the masks
  std::size_t num_robot_nodes = 0;
  for (const auto& robot_elem : robot->tree_nodes) {
    const auto& link = robot_elem.second;
    if (link->geom == nullptr) {
      continue;
    }

    const auto& [_, is_new] =
    robot_collisions.emplace(link->name, std::make_unique<btCollisionObject>());
    if (!is_new) {
      continue;
    }

    ++num_robot_nodes;
  }

  // NOTE: This uses the assumption that the filter will only ever exclude robot links
  broadphase_filter = std::make_unique<NeighborLinksFilter>(blacklist_path, num_robot_nodes, sg);
  collision_world->getPairCache()->setOverlapFilterCallback(broadphase_filter.get());
  for (const auto& obstacle_elem : obstacles) {
    const auto& obstacle = obstacle_elem.second;
    auto& obstacle_collision =
    obstacle_collisions.emplace_back(std::make_unique<btCollisionObject>());
    obstacle_collision->setUserPointer((void*)&(obstacle->name));
    obstacle_collision->setUserIndex(-1);
    obstacle_collision->setCollisionShape(obstacle->geom.get());
    obstacle_collision->setWorldTransform(obstacle->initial_pose);
    collision_world->addCollisionObject(obstacle_collision.get(),
                                        OBJECTS_COLLISION_GROUP,
                                        OBJECTS_COLLISION_MASK);
  }

  for (const auto& object_elem : objects) {
    const auto& object = object_elem.second;
    const auto& [object_collision_elem, _inserted] =
    object_collisions.emplace(object->name, std::make_unique<btCollisionObject>());
    const auto& [_name, object_collision] = *object_collision_elem;
    object_collision->setUserPointer((void*)&(object->name));
    object_collision->setUserIndex(-1);
    object_collision->setCollisionShape(object->geom.get());
    object_collision->setWorldTransform(object->initial_pose);
    collision_world->addCollisionObject(object_collision.get(),
                                        OBJECTS_COLLISION_GROUP,
                                        OBJECTS_COLLISION_MASK);
  }

  for (const auto& [name, link_collision] : robot_collisions) {
    const auto& link = robot->tree_nodes.at(name);
    link_collision->setUserPointer((void*)&(link->name));
    const auto& idx_it = broadphase_filter->index_map.find(link->name);
    if (idx_it != broadphase_filter->index_map.end()) {
      link_collision->setUserIndex(idx_it->second);
    } else {
      link_collision->setUserIndex(-1);
    }

    link_collision->setCollisionShape(link->geom.get());
    btTransform link_tf;
    auto& tf_origin              = link_tf.getOrigin();
    const auto& link_translation = link->collision_transform.translation();
    tf_origin.setX(link_translation.x());
    tf_origin.setY(link_translation.y());
    tf_origin.setZ(link_translation.z());
    const Eigen::Quaterniond link_rotation(link->collision_transform.linear());
    btQuaternion tf_rotation(
    link_rotation.x(), link_rotation.y(), link_rotation.z(), link_rotation.w());
    link_tf.setRotation(tf_rotation);
    link_collision->setWorldTransform(link_tf);
    collision_world->addCollisionObject(link_collision.get(),
                                        ROBOT_COLLISION_GROUP,
                                        ROBOT_COLLISION_MASK);
  }
}
}  // namespace planner::collisions
#endif
