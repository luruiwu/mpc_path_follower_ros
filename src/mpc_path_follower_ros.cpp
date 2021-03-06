/*********************************************************************
 * @file:   mpc_path_follower_ros.cpp
 * @author: Lianchuan Zhang
 * @date:   2019.01.16
 * @version:1.0.1
 * @brief:  interface between mpc and ros navigation
 *********************************************************************/
#include <mpc_path_follower/mpc_path_follower_ros.h>
Mpc_Path_Follower_Ros::Mpc_Path_Follower_Ros():initialized_(false),
    odom_helper_("odom"), setup_(false){

}

void Mpc_Path_Follower_Ros::initialize(std::__cxx11::string name,
                                       tf::TransformListener *tf,
                                       costmap_2d::Costmap2DROS *costmap_ros)
{
    if (!isInitialized()) {

        ros::NodeHandle private_nh("~/" + name);
        ros::NodeHandle nh;
        g_plan_sub = nh.subscribe("global_plan", 1, &Mpc_Path_Follower_Ros::global_path_CB, this);
        //g_plan_sub = nh.subscribe("/control/speed", 1, &Mpc_Path_Follower_Ros::global_path_CB, this);
        l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);
        tf_ = tf;
        costmap_ros_ = costmap_ros;
        costmap_ros_->getRobotPose(current_pose_);

        // make sure to update the costmap we'll use for this cycle
        costmap_2d::Costmap2D* costmap = costmap_ros_->getCostmap();

        if( private_nh.getParam( "odom_topic", odom_topic_ ))
        {
            odom_helper_.setOdomTopic( odom_topic_ );
        }

        initialized_ = true;
    }
    else{
        ROS_WARN("This planner has already been initialized, doing nothing.");
    }
}

bool Mpc_Path_Follower_Ros::computeVelocityCommands(geometry_msgs::Twist &cmd_vel){

    if ( ! costmap_ros_->getRobotPose(current_pose_)) {
        ROS_ERROR("Could not get robot pose");
        return false;
    }
    std::vector<geometry_msgs::PoseStamped> transformed_plan;
    if ( ! planner_util_.getLocalPlan(current_pose_, transformed_plan)) {
        ROS_ERROR("Could not get local plan");
        return false;
    }

    //if the global plan passed in is empty... we won't do anything
    if(transformed_plan.empty()) {
        ROS_WARN_NAMED("mpc_local_planner", "Received an empty transformed plan.");
        return false;
    }
    ROS_DEBUG_NAMED("mpc_local_planner", "Received a transformed plan with %zu points.", transformed_plan.size());

    if (isGoalReached())
    {
        //publish an empty plan because we've reached our goal position
        publishZeroVelocity();
    }
    else
    {
        //转到这里计算速度，增加了当前机器人的位置
        //bool isOk = dwaComputeVelocityCommands(current_pose_, cmd_vel);
        bool isOk = false;
        if (isOk)
        {
            publishGlobalPlan(transformed_plan);
        }
        else
        {
            ROS_WARN_NAMED("mpc_local_planner", "mpc planner failed to produce path.");
            std::vector<geometry_msgs::PoseStamped> empty_plan;
            publishGlobalPlan(empty_plan);
        }
        return isOk;
    }
}

void Mpc_Path_Follower_Ros::mpcComputeVelocity(std::vector<geometry_msgs::PoseStamped> &path, geometry_msgs::Twist &cmd_vel){

    tf::Stamped<tf::Pose> robot_vel;
    odom_helper_.getRobotVel(robot_vel);
    Eigen::Vector3f vel(robot_vel.getOrigin().getX(),
                        robot_vel.getOrigin().getY(), tf::getYaw(robot_vel.getRotation()));


    std::vector<double> waypoints_x;
    std::vector<double> waypoints_y;
    std::vector<geometry_msgs::PoseStamped>::iterator it = path.begin();
    std::vector<geometry_msgs::PoseStamped>::iterator it_end = path.begin();
    if(path.size() >= 40){
        it_end = it + 40;
    } else {
        it_end = path.end();
    }
    while(it != it_end){
        const geometry_msgs::PoseStamped& w = *it;
        waypoints_x.push_back(w.pose.position.x);
        waypoints_y.push_back(w.pose.position.y);
        double* ptrx = &waypoints_x[0];
        double* ptry = &waypoints_y[0];
        Eigen::Map<Eigen::VectorXd> waypoints_x_eig(ptrx, 6);
        Eigen::Map<Eigen::VectorXd> waypoints_y_eig(ptry, 6);
        // calculate cte and epsi
        auto coeffs = polyfit(waypoints_x_eig, waypoints_y_eig, 3);
        // The cross track error is calculated by evaluating at polynomial at x, f(x)
        // and subtracting y.
        //double cte = polyeval(coeffs, x) - y;
        // Due to the sign starting at 0, the orientation error is -f'(x).
        // derivative of coeffs[0] + coeffs[1] * x -> coeffs[1]
        //double epsi = psi - atan(coeffs[1]);
        double cte = polyeval(coeffs, 0);  // px = 0, py = 0
        double epsi = -atan(coeffs[1]);  // p
        Eigen::VectorXd state(6);
        state << 0, 0, 0, vel[0], cte, epsi;
        std::vector<double> vars;
        mpc_solver.initialize();
        vars = mpc_solver.solve(state, coeffs);
        std::vector<double> mpc_x_vals;
        std::vector<double> mpc_y_vals;
        for (int i = 2; i < vars.size(); i ++) {
          if (i%2 == 0) {
            mpc_x_vals.push_back(vars[i]);
          }
          else {
            mpc_y_vals.push_back(vars[i]);
          }
        }
//        steer_value = vars[0];
//        throttle_value = vars[1];

    }
}

bool Mpc_Path_Follower_Ros::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan){
    if(!initialized_){
        ROS_ERROR("Planner utils have not been initialized, please call initialize() first");
        return false;
    }

    //reset the global plan
    global_plan_.clear();
    ROS_INFO("Got new plan");
    global_plan_ = orig_global_plan;

    return true;
}

void Mpc_Path_Follower_Ros::global_path_CB(const nav_msgs::Path& path){
    if(path.poses.empty())
        return;
    std::vector<geometry_msgs::PoseStamped> original_plan;
    // Extract the plan in world co-ordinates, we assume the path is all in the same frame
    for(unsigned int i=0; i < path.poses.size(); i++){
        original_plan[i] = path.poses[i];
    }
    setPlan(original_plan);
}

bool Mpc_Path_Follower_Ros::isGoalReached(){
    if (! isInitialized()) {
        ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
        return false;
    }
    if ( ! costmap_ros_->getRobotPose(current_pose_)) {
        ROS_ERROR("Could not get robot pose");
        return false;
    }

    if(latchedStopRotateController_.isGoalReached(&planner_util_, odom_helper_, current_pose_)) {
        ROS_INFO("Goal reached");
        return true;
    } else {
        return false;
    }
}

double Mpc_Path_Follower_Ros::polyeval(Eigen::VectorXd coeffs, double x){
    double result = 0.0;
    for (int i = 0; i < coeffs.size(); i++) {
        result += coeffs[i] * pow(x, i);
    }
    return result;
}

Eigen::VectorXd Mpc_Path_Follower_Ros::polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals,
                                               int order){
    assert(xvals.size() == yvals.size());
    assert(order >= 1 && order <= xvals.size() - 1);
    Eigen::MatrixXd A(xvals.size(), order + 1);

    for (int i = 0; i < xvals.size(); i++) {
        A(i, 0) = 1.0;
    }

    for (int j = 0; j < xvals.size(); j++) {
        for (int i = 0; i < order; i++) {
            A(j, i + 1) = A(j, i) * xvals(j);
        }
    }
    auto Q = A.householderQr();
    auto result = Q.solve(yvals);
    return result;
}

void Mpc_Path_Follower_Ros::publishLocalPlan(std::vector<geometry_msgs::PoseStamped>& path) {
    base_local_planner::publishPlan(path, l_plan_pub_);
}


void Mpc_Path_Follower_Ros::publishGlobalPlan(std::vector<geometry_msgs::PoseStamped>& path) {
    base_local_planner::publishPlan(path, g_plan_pub_);
}
//int main(int argc, char** argv)
//{
//  ros::init(argc, argv, "mpc_path_follower_node");
//}
