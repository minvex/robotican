


#include <ros/ros.h>
#include <std_msgs/String.h>
#include <geometry_msgs/PointStamped.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <std_msgs/Empty.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <actionlib/client/simple_action_client.h>
#include <control_msgs/GripperCommandAction.h>
#include <moveit/move_group_interface/move_group.h>

#include <moveit/robot_model_loader/robot_model_loader.h>
#include <moveit/robot_model/robot_model.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/kinematics_metrics/kinematics_metrics.h>
#include <moveit_msgs/WorkspaceParameters.h>
#include <moveit/planning_scene/planning_scene.h>

typedef actionlib::SimpleActionClient<control_msgs::GripperCommandAction> GripperClient;

robot_state::GroupStateValidityCallbackFn state_validity_callback_fn_;
const robot_model::JointModelGroup* joint_model_group;

bool isIKSolutionCollisionFree(robot_state::RobotState *joint_state,
                               const robot_model::JointModelGroup *joint_model_group,
                               const double *ik_solution);

planning_scene::PlanningScenePtr *planning_scene_ptr;
robot_state::RobotStatePtr *robot_state_ptr;

bool checkIK(geometry_msgs::PoseStamped pose);
void go(tf::Transform  dest);
bool gripper_cmd(double gap,double effort);
bool arm_cmd(geometry_msgs::PoseStamped target_pose1);
geometry_msgs::PoseStamped move_to_object();


geometry_msgs::PoseStamped lift_arm();


void pick_go_cb(std_msgs::Empty);
void button_go_cb(std_msgs::Empty);
void look_down();

GripperClient *gripperClient_ptr;

moveit::planning_interface::MoveGroup *moveit_ptr;
tf::TransformListener *listener_ptr;

tf::StampedTransform base_obj_transform;
std::string object_frame;

geometry_msgs::PoseStamped pick_pose;
double pick_yaw=0;
bool have_goal=false;
bool moving=false;



ros::Publisher planning_scene_diff_publisher;
ros::Publisher goal_pub;
ros::Publisher pub_controller_command;

double wrist_distance_from_object=0.10;
geometry_msgs::PoseStamped moveit_goal;



geometry_msgs::PoseStamped lift_arm(){
    geometry_msgs::PoseStamped target_pose1;
    target_pose1.header.frame_id="map";
    target_pose1.header.stamp=ros::Time::now();
    target_pose1.pose.position.x = 0.5;
    target_pose1.pose.position.y =  0.0;
    target_pose1.pose.position.z =  0.9;
    target_pose1.pose.orientation= tf::createQuaternionMsgFromRollPitchYaw(0.0,0.0,0.0);
    return target_pose1;
}


void look_down() {

    trajectory_msgs::JointTrajectory traj;
    traj.header.stamp = ros::Time::now();
    traj.joint_names.push_back("head_pan_joint");
    traj.joint_names.push_back("head_tilt_joint");
    traj.points.resize(1);
    traj.points[0].time_from_start = ros::Duration(1.0);
    std::vector<double> q_goal(2);
    q_goal[0]=0.0;
    q_goal[1]=0.4;
    traj.points[0].positions=q_goal;
    traj.points[0].velocities.push_back(0);
    traj.points[0].velocities.push_back(0);
    pub_controller_command.publish(traj);

}


geometry_msgs::PoseStamped move_to_object(){
    geometry_msgs::PoseStamped target_pose1;

    tf::StampedTransform transform_base_obj=base_obj_transform;


    tf::Vector3 v= transform_base_obj.getOrigin();

    pick_yaw=atan2(v.y(),v.x());

    target_pose1.header.frame_id="base_footprint";
    target_pose1.header.stamp=ros::Time::now();

    float away=wrist_distance_from_object/sqrt(v.x()*v.x()+v.y()*v.y());
    tf::Vector3 dest=v*(1-away);

    target_pose1.pose.position.x = dest.x();
    target_pose1.pose.position.y =  dest.y();
    target_pose1.pose.position.z =  v.z();
    target_pose1.pose.orientation= tf::createQuaternionMsgFromRollPitchYaw(0,0,-pick_yaw );

    return target_pose1;

}

void pick_go_cb(std_msgs::Empty) {

    if (!moving) moving=true;
    ROS_INFO("Openning gripper...");
    if(gripper_cmd(0.14,0.0)) {
        ROS_INFO("Gripper is oppend, planning for pre-grasping..");
        ros::Duration w2(5);
        w2.sleep(); //wait for re-detection
        pick_pose=move_to_object();
        if (arm_cmd(pick_pose)) {
            ROS_INFO("Arm planning is done, moving arm..");
            if(moveit_ptr->move()) {
                ROS_INFO("Ready to grasp");
                if(gripper_cmd(0.01,0.2)) {
                    ROS_INFO("Grasping is done");
                    ros::Duration w(8);
                    w.sleep(); //wait for attach
                    ROS_INFO("Lifting object...");
                    if (arm_cmd(lift_arm())) {
                        ROS_INFO("Arm planning is done, moving arm up..");
                        if (moveit_ptr->move()) {
                            ROS_INFO("Arm is up, placing on table...");
                            pick_pose.pose.position.z=pick_pose.pose.position.z+0.01;
                            pick_pose.pose.position.y=pick_pose.pose.position.y+0.1;
                            if (arm_cmd(pick_pose)) {
                                ROS_INFO("Arm planning is done, moving arm..");
                                if(moveit_ptr->move()) {
                                    ROS_INFO("Openning gripper...");
                                    if(gripper_cmd(0.14,0.0)) {
                                        ros::Duration w(5);
                                        w.sleep(); //wait for deattach
                                        ROS_INFO("Lifting arm up...");
                                        if (arm_cmd(lift_arm())) {
                                            ROS_INFO("Arm planning is done, moving arm up..");
                                            if (moveit_ptr->move()) {
                                                ROS_INFO("Arm is up");
                                                ROS_INFO("Done!");
                                            }

                                        }

                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}



bool isIKSolutionCollisionFree(robot_state::RobotState *joint_state,
                               const robot_model::JointModelGroup *joint_model_group_,
                               const double *ik_solution)
{
    joint_state->setJointGroupPositions(joint_model_group_, ik_solution);
    bool result = !(*planning_scene_ptr)->isStateColliding(*joint_state, joint_model_group_->getName());

    return result;
}

bool checkIK(geometry_msgs::PoseStamped pose) {

    bool found_ik = (*robot_state_ptr)->setFromIK(joint_model_group, pose.pose, 5, 0.005, state_validity_callback_fn_);
    std::printf("IK %d: [%f , %f , %f] [%f , %f , %f , %f]\n",found_ik,pose.pose.position.x,pose.pose.position.y,pose.pose.position.z,pose.pose.orientation.x,pose.pose.orientation.y,pose.pose.orientation.z,pose.pose.orientation.w);
    return found_ik;
}

bool arm_cmd( geometry_msgs::PoseStamped target_pose1) {




    moveit_ptr->setStartStateToCurrentState();

    if (!have_goal) have_goal=true;

    moveit::planning_interface::MoveGroup::Plan my_plan;
    double dz[]={0, 0.01, -0.01 ,0.02, -0.02,0.03, -0.03};
    double dy[]={0, 0.01, -0.01 ,0.02, -0.02,0.03, -0.03};
    double dx[]={0, 0.01, -0.01 ,0.02, -0.02,0.03, -0.03};
    double dY[]={0, 0.04, -0.04 ,0.08, -0.08};
    double z=target_pose1.pose.position.z;
    double x=target_pose1.pose.position.x;
    double y=target_pose1.pose.position.y;
    for (int n=0;n<sizeof(dx)/sizeof(double);n++) {
        for (int m=0;m<sizeof(dy)/sizeof(double);m++) {

            for (int i=0;i<sizeof(dz)/sizeof(double);i++) {
                for (int j=0;j<sizeof(dY)/sizeof(double);j++) {
                    target_pose1.pose.position.z=z+dz[i];
                    target_pose1.pose.position.x=x+dx[n];
                    target_pose1.pose.position.y=y+dy[m];
                    target_pose1.pose.orientation= tf::createQuaternionMsgFromRollPitchYaw(0,0,pick_yaw+dY[j] );

                    if (checkIK(target_pose1)) {
                        goal_pub.publish(target_pose1);
                        moveit_goal=target_pose1;

                        moveit_ptr->setPoseTarget(target_pose1);
                        bool success = moveit_ptr->plan(my_plan);
                        ROS_INFO("Moveit plan %s",success?"SUCCESS":"FAILED");
                        if (success) return true;
                    }
                }
            }
        }
    }
    return false;
}

bool gripper_cmd(double gap,double effort) {

    control_msgs::GripperCommandGoal openGoal;

    openGoal.command.position = gap;
    openGoal.command.max_effort = effort;
    gripperClient_ptr->sendGoal(openGoal);
    ROS_INFO("Sent gripper goal");
    gripperClient_ptr->waitForResult();

    if(gripperClient_ptr->getState() == actionlib::SimpleClientGoalState::SUCCEEDED) {
        ROS_INFO("Gripper done");
        return true;
    }
    else {
        ROS_ERROR("Gripper fault");
        // return false;
    }
    return false;
}


int main(int argc, char **argv) {

    ros::init(argc, argv, "pick_and_plce_node");
    ros::AsyncSpinner spinner(2);
    spinner.start();
    ros::NodeHandle n;

    tf::TransformBroadcaster br;
    ROS_INFO("Hello");

    n.param<double>("wrist_distance_from_object", wrist_distance_from_object, 0.03);
    n.param<std::string>("object_frame", object_frame, "object_frame");


    GripperClient gripperClient("/gripper_controller/gripper_cmd", true);
    //wait for the gripper action server to come up
    while (!gripperClient.waitForServer(ros::Duration(5.0))){
        ROS_INFO("Waiting for the /gripper_controller/gripper_cmd action server to come up");
    }

    gripperClient_ptr=&gripperClient;


    ROS_INFO("Waiting for the moveit action server to come up");
    moveit::planning_interface::MoveGroup group("arm");

    // moveit::planning_interface::PlanningSceneInterface planning_scene_interface;

    // group.allowReplanning(true);
    // group.setPlanningTime(5.0);
    //group.setNumPlanningAttempts(5);
    group.setPlannerId("RRTConnectkConfigDefault");
    //group.setPlannerId("LBKPIECEkConfigDefault");
    //group.setMaxAccelerationScalingFactor(0.1);
   // group.setMaxVelocityScalingFactor(0.1);
  //  group.setGoalPositionTolerance(0.02);
    group.setPoseReferenceFrame("base_footprint");
    moveit_ptr=&group;

    ros::Subscriber pick_sub = n.subscribe("pick_go", 1, pick_go_cb);

    goal_pub=n.advertise<geometry_msgs::PoseStamped>("pick_moveit_goal", 2, true);
    pub_controller_command = n.advertise<trajectory_msgs::JointTrajectory>("pan_tilt_trajectory_controller/command", 2);


    tf::TransformListener listener;
    listener_ptr=&listener;

    ros::Rate r(50); // 50 hz



    tf::Quaternion q;
    q.setRPY(0.0, 0, 0);

    ros::Duration(3.0).sleep();


    state_validity_callback_fn_ = boost::bind(&isIKSolutionCollisionFree, _1, _2, _3);

    std::string group_name="arm";

    /* Load the robot model */
    //robot_model_loader::RobotModelLoader robot_model_loader("robot_description");

    /* Get a shared pointer to the model */
    robot_model::RobotModelConstPtr robot_model = group.getRobotModel();// robot_model_loader.getModel();

    /* Create a robot state*/
    robot_state::RobotStatePtr robot_state(new robot_state::RobotState(robot_model));

    robot_state_ptr=&robot_state;

    if(!robot_model->hasJointModelGroup(group_name))
        ROS_FATAL("Invalid group name: %s", group_name.c_str());

    // const robot_model::JointModelGroup* joint_model_group;
    joint_model_group = robot_model->getJointModelGroup(group_name);
    //joint_model_group_ptr=&joint_model_group;
    /* Construct a planning scene - NOTE: this is for illustration purposes only.
      The recommended way to construct a planning scene is to use the planning_scene_monitor
      to construct it for you.*/
    planning_scene::PlanningScenePtr planning_scene(new planning_scene::PlanningScene(robot_model));
    planning_scene_ptr=&planning_scene;



    // std::string robot_name_ = robot_state->getRobotModel()->getName();
    std::string frame_id_ =  robot_state->getRobotModel()->getModelFrame();

    ROS_INFO_STREAM("Root frame ID: " << frame_id_);

    ROS_INFO("Looking down...");
    look_down();
    if (arm_cmd(lift_arm())) {
        ROS_INFO("Arm planning is done, moving arm up..");
        if (moveit_ptr->move()) {
            ROS_INFO("Arm is up");
        }
    }

    ROS_INFO("Ready!");
    while (ros::ok())
    {


        try{
            listener_ptr->lookupTransform("base_footprint", object_frame, ros::Time(0), base_obj_transform);
        }
        catch (tf::TransformException ex){
            ROS_ERROR("PNP NODE: %s",ex.what());
        }
        // }
        if (have_goal) {
            tf::StampedTransform wr_goal;
            tf::Quaternion q;
            tf::quaternionMsgToTF(moveit_goal.pose.orientation,q);
            wr_goal.setOrigin(tf::Vector3(moveit_goal.pose.position.x,moveit_goal.pose.position.y,moveit_goal.pose.position.z));
            wr_goal.setRotation((q));
            wr_goal.stamp_=ros::Time::now();
            wr_goal.child_frame_id_="moveit_goal";
            wr_goal.frame_id_="base_footprint";
            br.sendTransform(wr_goal);

        }


        r.sleep();
    }

    return 0;
}

