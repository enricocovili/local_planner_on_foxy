/**
 * @file WayComputer.cpp
 * @author Oriol Gorriz (origovi2000@gmail.com)
 * @brief Contains the WayComputer class member functions implementation
 * @version 1.0
 * @date 2022-10-31
 *
 * @copyright Copyright (c) 2022 BCN eMotorsport
 */

#include "modules/WayComputer.hpp"

/* ----------------------------- Private Methods ---------------------------- */

void WayComputer::filterTriangulation(TriangleSet &triangulation) const {
  auto it = triangulation.begin();
  while (it != triangulation.end()) {
    bool removeTriangle = false;
    // Look for edges longer than accepted
    for (const Edge &e : it->edges) {
      if (e.len > this->params_.max_triangle_edge_len) {
        removeTriangle = true;
        break;
      }
    }

    // Look for angles smaller than accepted
    if (!removeTriangle) {
      for (const double &angle : it->angles()) {
        if (angle < this->params_.min_triangle_angle) {
          removeTriangle = true;
          break;
        }
      }
    }

    if (removeTriangle)
      it = triangulation.erase(it);
    else
      it++;
  }
}

void WayComputer::filterMidpoints(EdgeSet &edges, const TriangleSet &triangulation) const {
  // Build a k-d tree of all circumcenters so finding matches is O(logn)
  std::vector<Point> circums(triangulation.size());
  std::transform(triangulation.begin(), triangulation.end(), circums.begin(),
                 [](const Triangle &t) -> const Point & { return t.circumCenter(); });
  KDTree circumKDTree(circums);

  // Perform the filtering
  auto it = edges.begin();
  while (it != edges.end()) {
    Point midPoint = it->midPoint();
    KDTData<size_t> nearestCC = circumKDTree.nearest_index(midPoint);
    if (bool(nearestCC) and Point::distSq(circums[*nearestCC], midPoint) > pow(this->params_.max_dist_circum_midPoint, 2)) {
      it = edges.erase(it);
    } else {
      it++;
    }
  }
}

double WayComputer::getHeuristic(const Point &actPos, const Point &nextPos, const Vector &dir, const Params::WayComputer::Search &params) const {
  double distHeur = Point::dist(actPos, nextPos);

  double angle = Vector(actPos, nextPos).angleWith(dir);
  double angleHeur = -log(std::max(0.0, ((M_PI_2 - abs(angle)) / M_PI_2) - 0.2));

  return params.heur_dist_ponderation * distHeur + (1 - params.heur_dist_ponderation) * angleHeur;
}

inline double WayComputer::avgEdgeLen(const Trace *trace) const {
  if (not trace or trace->empty())
    return this->way_.getAvgEdgeLen();
  else if (this->way_.empty())
    return trace->avgEdgeLen();
  else
    return ((trace->avgEdgeLen() * trace->size()) / (trace->size() + this->way_.size())) + ((this->way_.getAvgEdgeLen() * this->way_.size()) / (trace->size() + this->way_.size()));
}

void WayComputer::findNextEdges(std::vector<HeurInd> &nextEdges, const Trace *actTrace, const KDTree &midpointsKDT, const std::vector<Edge> &edges, const Params::WayComputer::Search &params) const {
  nextEdges.clear();

  const Edge *actEdge = nullptr;  // Last valid Edge (starting point)
  Point actPos(0, 0);
  Point lastPos(0, 0);

  // Set actual and last position
  if (actTrace and not actTrace->empty()) {
    actEdge = &edges[actTrace->edgeInd()];
    actPos = edges[actTrace->edgeInd()].midPoint();
    if (actTrace->size() >= 2) {
      lastPos = edges[actTrace->before().edgeInd()].midPoint();
    }
  }
  if (not this->way_.empty()) {
    if (not actEdge) {
      actEdge = &this->way_.back();
      actPos = this->way_.back().midPoint();
      if (this->way_.size() >= 2) {
        lastPos = this->way_.beforeBack().midPoint();
      }
    } else if (actTrace->size() < 2) {
      lastPos = this->way_.back().midPoint();
    }
  }

  // Set dir vector
  Vector dir;
  if (actEdge and (this->way_.size() >= 2 or actTrace))  // This condition avoids setting a vector pointing backwards
    dir = Vector(lastPos, actPos);
  else
    dir = Vector(1, 0);

  // Find all possible edges in a specified radius
  std::unordered_set<size_t> nextPossibleEdges = midpointsKDT.neighborhood_indices_set(actPos, params.search_radius);

  // Discard edges by specifications
  auto it = nextPossibleEdges.begin();
  while (it != nextPossibleEdges.end()) {
    const Edge &nextPossibleEdge = edges[*it];
    // The currently-iterated Edge will be removed if any of the below conditions is true
    bool removeConditions = actEdge and (
      // 1. Remove itself from being the next one
      nextPossibleEdge == *actEdge or

      // 2. Remove any edge whose midpoint create an angle too closed with last one
      abs(dir.angleWith(Vector(actPos, nextPossibleEdge.midPoint()))) > params.max_angle_diff or

      // 3. [Only before appending the edge that closes the loop] Remove any edge that is already contained in the path but is not the one that closes the loop
      (not this->way_.closesLoopWith(nextPossibleEdge) and (not actTrace or not actTrace->isLoopClosed()) and this->way_.containsEdge(nextPossibleEdge)) or

      // 4. Remove any edge whose midpoint and lastPos are in the same side of actEdge (avoid bouncing on a track limit)
      ((this->way_.size() >= 2 or actTrace) and Vector::pointBehind(actEdge->midPoint(), lastPos, actEdge->normal()) == Vector::pointBehind(actEdge->midPoint(), nextPossibleEdge.midPoint(), actEdge->normal())) or

      // 5. Remove any edge whose length is too big or too small compared to the average edge length of the way
      (nextPossibleEdge.len < (1 - params.edge_len_diff_factor) * this->avgEdgeLen(actTrace) or nextPossibleEdge.len > (1 + params.edge_len_diff_factor) * this->avgEdgeLen(actTrace)) or

      // 6. [If not allow_intersection, only before closing the loop] Remove any Edge which appended would create an intersection
      (not params.allow_intersection and (not actTrace or not actTrace->isLoopClosed()) and not this->way_.closesLoopWith(nextPossibleEdge) and this->way_.intersectsWith(nextPossibleEdge)));

    if (removeConditions)
      it = nextPossibleEdges.erase(it);
    else
      it++;
  }

  // Get the heuristics for all possible next edges and filter them (only the
  // ones having a heuristic small enough will prevail)
  std::vector<HeurInd> privilege_runner;
  privilege_runner.reserve(nextPossibleEdges.size());
  for (const size_t &nextPossibleEdgeInd : nextPossibleEdges) {
    double heuristic = this->getHeuristic(actPos, edges[nextPossibleEdgeInd].midPoint(), dir, params);
    if (heuristic <= params.max_next_heuristic) privilege_runner.emplace_back(heuristic, nextPossibleEdgeInd);
  }

  // Copy the n best HeurInd(s) into the nextEdges vector, according to
  // cppreference.com, this is O(nlogn)
  nextEdges.resize(std::min(privilege_runner.size(), (size_t)params.max_search_options));
  std::partial_sort_copy(privilege_runner.begin(), privilege_runner.end(), nextEdges.begin(), nextEdges.end());
}

Trace WayComputer::computeBestTraceWithFinishedT(const Trace &best, const Trace &t) const {
  // The method of choosing the best trace is as follows:
  // 1. The longest trace wins.
  // 2. If the size is equal, then the trace with smallest accum heuristic wins.
  // Note that here, no trace is added to the queue.
  if (t.size() > best.size() or (t.size() == best.size() and t.sumHeur() < best.sumHeur())) {
    return t;
  } else
    return best;
}

size_t WayComputer::treeSearch(std::vector<HeurInd> &nextEdges, const KDTree &midpointsKDT, const std::vector<Edge> &edges, const Params::WayComputer::Search &params) const {
  std::queue<Trace> cua;
  for (const HeurInd &nextEdge : nextEdges) {
    bool closesLoop = this->way_.closesLoopWith(edges[nextEdge.second]);
    cua.emplace(nextEdge.second, nextEdge.first, edges[nextEdge.second].len, closesLoop);
  }
  // The provisional best is the Trace with smaller heuristic, i.e. the front one
  Trace best = cua.front();

  // This loop will realize the tree search and get the longest (& best) path.
  // The loop will stop if the tree search is completed or a time limit has
  // been exceeded.
  auto searchBeginTime = rclcpp::Clock().now();
  while (not cua.empty()) {
    if (rclcpp::Clock().now() - searchBeginTime > rclcpp::Duration::from_seconds(params.max_treeSearch_time)) {
      RCLCPP_WARN(rclcpp::get_logger("local_planner"), "Tree search time limit exceeded.");
      break;
    }
    Trace t = cua.front();
    cua.pop();

    bool trace_at_max_height = false;

    if (t.size() >= params.max_search_tree_height)
      trace_at_max_height = true;
    else
      this->findNextEdges(nextEdges, &t, midpointsKDT, edges, params);

    if (trace_at_max_height or nextEdges.empty()) {
      // Means that this trace is finished, should be considered as the "best"
      // trace.
      best = this->computeBestTraceWithFinishedT(best, t);
    } else {
      // Add new possible traces to the queue
      for (const HeurInd &nextEdge : nextEdges) {
        Point actPos = edges[t.edgeInd()].midPoint();
        bool closesLoop = this->way_.closesLoopWith(edges[nextEdge.second], &actPos);
        Trace aux = t;
        aux.addEdge(nextEdge.second, nextEdge.first, edges[nextEdge.second].len, closesLoop);
        cua.push(aux);
      }
    }
  }
  // The next point will be the FIRST point of the best path
  return best.first().edgeInd();
}

void WayComputer::computeWay(const std::vector<Edge> &edges, const Params::WayComputer::Search &params) {
  // Get rid of all edges from closest to car (included) to last
  this->way_.trimByLocal();

  // Build a k-d tree of all midpoints so is cheaper to find closest points
  std::vector<Point> midpoints(edges.size());
  std::transform(edges.begin(), edges.end(), midpoints.begin(),
                 [](const Edge &e) -> Point { return e.midPoint(); });
  KDTree midpointsKDT(midpoints);

  // Find the longest and with lower heuristic Trace. Search will be conducted
  // through a tree. The tree height will be limited, at that point the first
  // element of the longest with lower heuristic Trace will be added to the
  // way. The search finishes when no element can be added to the way.
  std::vector<HeurInd> nextEdges;

  // Get first set of possible Edges
  this->findNextEdges(nextEdges, nullptr, midpointsKDT, edges, params);

  // Main outer loop, every iteration of this loop will involve adding one
  // midpoint to the path.
  while (rclcpp::ok() and not nextEdges.empty() and (!params.max_way_horizon_size or this->way_.sizeAheadOfCar() <= params.max_way_horizon_size)) {
    size_t nextEdgeInd = this->treeSearch(nextEdges, midpointsKDT, edges, params);

    // Append the new Edge
    this->way_.addEdge(edges[nextEdgeInd]);

    // Check for loop closure
    if (this->way_.closesLoop()) {
      this->wayToPublish_ = this->way_.restructureClosure();
      this->isLoopClosed_ = true;
      return;
    }

    // Get next set of possible edges
    this->findNextEdges(nextEdges, nullptr, midpointsKDT, edges, params);
  }
  this->isLoopClosed_ = false;
  this->wayToPublish_ = this->way_;
}

/* ----------------------------- Public Methods ----------------------------- */

WayComputer::WayComputer(const Params::WayComputer &params) : params_(params) {
  Way::init(params.way);
  this->generalFailsafe_.initGeneral(this->params_.search, this->params_.general_failsafe_safetyFactor, this->params_.failsafe_max_way_horizon_size);
}
/*
void WayComputer::stateCallback(const nav_msgs::msg::Odometry::SharedPtr &data) {
  geometry_msgs::msg::Pose pose;
  pose.position = data->pose.pose.position;
  tf2::Quaternion qAux;
  qAux.setRPY(0.0, 0.0, tf2::getYaw(data->pose.pose.orientation));
  pose.orientation = tf2::toMsg(qAux);
  this->localTf_ = tf2::transformToEigen(pose);

  this->localTf_ = this->localTf_.inverse();

  this->localTfValid_ = true;
}
*/

void WayComputer::stateCallback(const nav_msgs::msg::Odometry::SharedPtr odom) {
    geometry_msgs::msg::Pose pose;
    pose.position = odom->pose.pose.position;

    // Extract yaw from the existing quaternion orientation
    tf2::Quaternion q_orig;
    tf2::fromMsg(odom->pose.pose.orientation, q_orig);

    double roll, pitch, yaw;
    tf2::Matrix3x3(q_orig).getRPY(roll, pitch, yaw);

    // Recreate the quaternion with zero roll and pitch,
    // preserving only the yaw to mimic the original logic
    tf2::Quaternion qAux;
    qAux.setRPY(0.0, 0.0, yaw);
    pose.orientation = tf2::toMsg(qAux);

    Eigen::Isometry3d eigen_pose;
    tf2::fromMsg(pose, eigen_pose);

    // Invert the transform
    this->localTf_ = eigen_pose.inverse();

    // Mark as valid
    this->localTfValid_ = true;
}


void WayComputer::update(TriangleSet &triangulation, const rclcpp::Time &stamp) {
  if (not this->localTfValid_) {
    RCLCPP_WARN(rclcpp::get_logger("local_planner"), "CarState not being received.");
    return;
  }

  // #0: Update last way (this will be used to calculate the raplan flag).
  //     And update stamp.
  this->lastWay_ = this->way_;
  this->lastStamp_ = stamp;

  // #1: Remove all triangles which we know will not be part of the track.
  this->filterTriangulation(triangulation);

  // #2: Extract all midpoints without repetitions, do that through an EdgeSet
  // so no midpoint is got twice.
  EdgeSet edgeSet;
  for (const Triangle &t : triangulation) {
    for (const Edge &e : t.edges) {
      edgeSet.insert(e);
    }
  }

  // #3: Filter the midpoints. Only the ones having a circumcenter near, will be
  // left.
  this->filterMidpoints(edgeSet, triangulation);

  // Convert this set to a vector
  std::vector<Edge> edgeVec;
  edgeVec.reserve(edgeSet.size());
  for (const Edge &e : edgeSet) {
    edgeVec.push_back(e);
  }

  // #4: Update all local positions (way and edges) with car tf
  this->way_.updateLocal(this->localTf_);
  for (const Edge &e : edgeVec) {
    e.updateLocal(this->localTf_);
  }

  // #5: Perform the search through the midpoints in order to obtain a way
  //     using normal parameters.
  this->computeWay(edgeVec, this->params_.search);

  // #6: Check failsafe(s)
  if (this->params_.general_failsafe and this->way_.sizeAheadOfCar() < MIN_FAILSAFE_WAY_SIZE and !this->isLoopClosed_) {
    RCLCPP_WARN(rclcpp::get_logger("local_planner"), "GENERAL FAILSAFE ACTIVATED!");
    this->computeWay(edgeVec, this->generalFailsafe_);
  }

  // #7: Visualize
  Visualization::getInstance().setTimestamp(stamp);
  Visualization::getInstance().visualize(edgeSet);
  Visualization::getInstance().visualize(triangulation);
  Visualization::getInstance().visualize(this->wayToPublish_);
}

const bool &WayComputer::isLoopClosed() const {
  return this->isLoopClosed_;
}

void WayComputer::writeWayToFile(const std::string &file_path) const {
  std::ofstream oStreamToWrite(file_path);
  oStreamToWrite << this->wayToPublish_;
  oStreamToWrite.close();
}

const bool &WayComputer::isLocalTfValid() const {
  return this->localTfValid_;
}

 const Eigen::Affine3d &WayComputer::getLocalTf() const {
  return this->localTf_;
}

std::vector<Point> WayComputer::getPath() const {
  return this->wayToPublish_.getPath();
}

Tracklimits WayComputer::getTracklimits() const {
  return this->wayToPublish_.getTracklimits();
}

mmr_base::msg::Marker WayComputer::getPathCenterLine() const {
  mmr_base::msg::Marker marker;

  // Get the centerline path as a vector of Points.
  std::vector<Point> path = this->wayToPublish_.getPath();

  // Set up header information.
  marker.header.frame_id = "track";
  marker.header.stamp = this->lastStamp_;
  marker.ns = "path_center_line";
  marker.id = 0;
  marker.type = mmr_base::msg::Marker::LINE_STRIP;
  marker.action = mmr_base::msg::Marker::ADD;

  // For a LINE_STRIP marker, only scale.x is used (as the width of the line).
  marker.scale.x = 0.3;

  // Set color to red.
  marker.color.r = 1.0;
  marker.color.g = 0.0;
  marker.color.b = 0.0;
  marker.color.a = 1.0;

  // Set the marker's pose to identity. The points are specified relative to the header frame.
  marker.pose.position.x = 0.0;
  marker.pose.position.y = 0.0;
  marker.pose.position.z = 0.0;
  marker.pose.orientation.w = 1.0;
  marker.pose.orientation.x = 0.0;
  marker.pose.orientation.y = 0.0;
  marker.pose.orientation.z = 0.0;

  // Add each point in the path to the marker's points vector.
  for (const Point &p : path) {
    marker.points.push_back(p.gmPoint());
  }

  return marker;
}


mmr_base::msg::MarkerArray WayComputer::getPathBorderLeft() const {

  mmr_base::msg::MarkerArray res;
  
  // Fill Tracklimits
  Tracklimits tracklimits = this->wayToPublish_.getTracklimits();
  // res.marker.stamp = res.stamp;
  // tracklimits.left.reserve(tracklimits.first.size());

  for (const Node &n : tracklimits.first) {
    res.markers.push_back(n.cone());
  }

  // res.tracklimits.replan indicates if the n midpoints in front of the car
  // have varied from last iteration
  // res.tracklimits.replan = this->way_.quinEhLobjetiuDeLaSevaDiresio(this->lastWay_);
 return res;
}
mmr_base::msg::MarkerArray WayComputer::getPathBorderRight() const {

  mmr_base::msg::MarkerArray res;
  
  // Fill Tracklimits
  Tracklimits tracklimits = this->wayToPublish_.getTracklimits();

  // res.tracklimits.stamp = res.stamp;
  // tracklimits.left.reserve(tracklimits.second.size());

  for (const Node &n : tracklimits.second) {
    res.markers.push_back(n.cone());
  }

  // res.tracklimits.replan indicates if the n midpoints in front of the car
  // have varied from last iteration
  // res.tracklimits.replan = this->way_.quinEhLobjetiuDeLaSevaDiresio(this->lastWay_);
 return res;
}