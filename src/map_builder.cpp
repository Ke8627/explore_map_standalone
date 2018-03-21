//
// Created by kevin on 3/19/18.
//

#include "explore_large_map/map_builder.hpp"

//#define OPENCV_SHOW

namespace explore_global_map {

    explore_global_map::MapBuilder::MapBuilder(int width, int height, double resolution)
    : start_flag_(false),
      tailored_submap_width_(60),
      tailored_submap_height_(40),
      tailored_submap_x2base_(10){
        map_.header.frame_id = "/odom";
        map_.info.width = width;
        map_.info.height = height;
        map_.info.resolution = resolution;
        map_.info.origin.position.x = -static_cast<double>(width) / 2 * resolution;
        map_.info.origin.position.y = -static_cast<double>(height) / 2 * resolution;
        map_.info.origin.orientation.w = 1.0;
        map_.data.assign(width * height, -1);  // Fill with "unknown" occupancy.

    }

    void MapBuilder::grow(nav_msgs::Odometry &global_vehicle_pose,
                          iv_slam_ros_msgs::TraversibleArea &traversible_map) {

        // get abosolute x,y
        geographic_to_grid(global_vehicle_pose.pose.pose.position.x, global_vehicle_pose.pose.pose.position.y);
        geographic_to_grid(traversible_map.triD_submap_pose.position.x, traversible_map.triD_submap_pose.position.y);


        if(!start_flag_) {
            geometry_msgs::Quaternion msg;
            msg = reverse_yaw_roll(global_vehicle_pose.pose.pose.orientation);
            global_vehicle_pose.pose.pose.orientation = msg;

            tf::Quaternion q;
            tf::quaternionMsgToTF(msg, q);
            tf::Matrix3x3 m(q);
            double roll, pitch, yaw;
            m.getRPY(roll, pitch, yaw);
            std::cout << "Roll: " << roll << ", Pitch: " << pitch << ", Yaw: " << yaw << std::endl;
            // todo
            if (fabs(pitch) < 0.1 && fabs(roll) < 0.1) {
                initial_global_vehicle_pos_ = global_vehicle_pose.pose.pose;
                initial_global_vehicle_pos_.position.x = 0;  // global map center position
                initial_global_vehicle_pos_.position.y = 0;
                // remember initial x and y
                initial_x_ = global_vehicle_pose.pose.pose.position.x;
                initial_y_ = global_vehicle_pose.pose.pose.position.y;
                start_flag_ = true;
            } else {
                ROS_WARN("current roll : %f, pitvh : %f ,Waiting for more flat position !", roll, pitch);
            }
        } else {

            // calc diff
            global_vehicle_pose.pose.pose.position.x = global_vehicle_pose.pose.pose.position.x - initial_x_;
            global_vehicle_pose.pose.pose.position.y = global_vehicle_pose.pose.pose.position.y - initial_y_;
            traversible_map.triD_submap_pose.position.x = traversible_map.triD_submap_pose.position.x - initial_x_;
            traversible_map.triD_submap_pose.position.y = traversible_map.triD_submap_pose.position.y - initial_y_;

            // tailor submap, small filter
            iv_slam_ros_msgs::TraversibleArea tailored_submap;
            tailorSubmap(traversible_map, tailored_submap);

            // reverse yaw and roll sequence
            geometry_msgs::Quaternion msg;
            msg = reverse_yaw_roll(tailored_submap.triD_submap_pose.orientation);
            tailored_submap.triD_submap_pose.orientation = msg;
            msg = reverse_yaw_roll(global_vehicle_pose.pose.pose.orientation);
            global_vehicle_pose.pose.pose.orientation = msg;

            // get submap rpy
            tf::Quaternion q;
            tf::quaternionMsgToTF(tailored_submap.triD_submap_pose.orientation, q);
            tf::Matrix3x3 m(q);
            double submap_roll, submap_pitch, submap_yaw;
            m.getRPY(submap_roll, submap_pitch, submap_yaw);
            // get vehicle rpy
            tf::quaternionMsgToTF(global_vehicle_pose.pose.pose.orientation, q);
            tf::Matrix3x3 m0(q);
            double vehicle_roll, vehicle_pitch, vehilce_yaw;
            m0.getRPY(vehicle_roll, vehicle_pitch, vehilce_yaw);

            // reserve current submap roll and pitch
            vehicle_pitch = submap_pitch;
            vehicle_roll = submap_roll;
            tf::Quaternion q_new = tf::createQuaternionFromRPY(vehicle_roll, vehicle_pitch, vehilce_yaw);
            tf::quaternionTFToMsg(q_new, global_vehicle_pose.pose.pose.orientation);
            // -->global_odom
            tf::Pose ps;
            tf::poseMsgToTF(global_vehicle_pose.pose.pose, ps);
            tf::poseTFToMsg(worldToMap(initial_global_vehicle_pos_) * ps, current_odom_vehicle_pos_);
            ROS_INFO("vehicle position in odom frame (%f[m], %f[m])", current_odom_vehicle_pos_.position.x,
                     current_odom_vehicle_pos_.position.y);
            // broadcast tf tree
            broadcastTransformBetweenVehicleAndOdom();


#ifdef OPENCV_SHOW
            int map_width = traversible_map.width;
            int map_height = traversible_map.height;
            cv::Mat mat_src(map_height, map_width, CV_8UC1, cv::Scalar(127));
            for(int i = 0; i < map_height; i++) {
                for(int j = 0; j < map_width; j++) {
                    int index = i * map_width + j;
                    if(traversible_map.cells[index] == 1) {
                        mat_src.at<uchar>(i, j) = 255;
                    } else if(traversible_map.cells[index] == 2) {
                        mat_src.at<uchar>(i,j) = 0;
                    } else {
                        mat_src.at<uchar>(i,j) = 127;
                    }
                }
            }

            cv::Mat mat_src_bgr;
            cv::cvtColor(mat_src, mat_src_bgr, CV_GRAY2BGR);

            int re_index_x = traversible_map.triD_submap_pose_image_index_x;
            int re_index_y = traversible_map.triD_submap_pose_image_index_y;
            cv::circle(mat_src_bgr, cv::Point(re_index_x, re_index_y), 5, cv::Scalar(255, 0, 0), -1);  // blue

            // project vehicle to submap
            tf::Pose ps0;
            geometry_msgs::Pose pose_temp;
            tf::poseMsgToTF(global_vehicle_pose.pose.pose, ps0);
            tf::poseTFToMsg(worldToMap(traversible_map.triD_submap_pose) * ps0, pose_temp);
            int ve_index_x = re_index_x + pose_temp.position.x / traversible_map.resolution;
            int ve_index_y = re_index_y + pose_temp.position.y / traversible_map.resolution;
            cv::circle(mat_src_bgr, cv::Point(ve_index_x, ve_index_y), 5, cv::Scalar(0, 0, 255), -1);  // red


            int top_left_x = traversible_map.triD_submap_pose_image_index_x - tailored_submap_width_ / 2
                                                                              / traversible_map.resolution;
            if(top_left_x < 0) top_left_x = 0;
            int top_left_y = traversible_map.triD_submap_pose_image_index_y + (tailored_submap_height_ - tailored_submap_x2base_)
                                                                              /traversible_map.resolution;
            if(top_left_y > traversible_map.height) top_left_y = traversible_map.height;
            int bottom_right_x = traversible_map.triD_submap_pose_image_index_x + tailored_submap_width_ / 2
                                                                                  / traversible_map.resolution;
            if(bottom_right_x > traversible_map.width) bottom_right_x = traversible_map.width;
            int bottom_right_y = traversible_map.triD_submap_pose_image_index_y - tailored_submap_x2base_ /traversible_map.resolution;
            if(bottom_right_y < 0) bottom_right_y = 0;

            cv::Point2i top_left(top_left_x, top_left_y), bottom_right(bottom_right_x, bottom_right_y);
            cv::Rect roi(top_left, bottom_right);
            cv::Mat image_roi = mat_src_bgr(roi);

            cv::Mat src_r, roi_r;
            cv::flip(mat_src_bgr, src_r, 0);
            cv::flip(image_roi, roi_r, 0);
            cv::imshow("source", src_r);
//            cv::imshow("roi", roi_r);
            cv::waitKey(1);

#endif


#ifdef OPENCV_SHOW
            int tmap_width = tailored_submap.width;
            int tmap_height = tailored_submap.height;
            cv::Mat tmat_src(tmap_height, tmap_width, CV_8UC1, cv::Scalar(127));
            for(int i = 0; i < tmap_height; i++) {
                for(int j = 0; j < tmap_width; j++) {
                    int index = i * tmap_width + j;
                    if(tailored_submap.cells[index] == 1) {
                        tmat_src.at<uchar>(i, j) = 255;
                    } else if(tailored_submap.cells[index] == 2) {
                        tmat_src.at<uchar>(i,j) = 0;
                    } else {
                        tmat_src.at<uchar>(i,j) = 127;
                    }
                }
            }

            cv::Mat tmat_src_bgr;
            cv::cvtColor(tmat_src, tmat_src_bgr, CV_GRAY2BGR);

            re_index_x = tailored_submap.triD_submap_pose_image_index_x;
            re_index_y = tailored_submap.triD_submap_pose_image_index_y;
            cv::circle(tmat_src_bgr, cv::Point(re_index_x, re_index_y), 5, cv::Scalar(255, 0, 0), -1);  // blue

            ve_index_x = re_index_x + pose_temp.position.x / tailored_submap.resolution;
            ve_index_y = re_index_y + pose_temp.position.y / tailored_submap.resolution;
            cv::circle(tmat_src_bgr, cv::Point(ve_index_x, ve_index_y), 5, cv::Scalar(0, 0, 255), -1);  // red

            cv::Mat t_src_r;
            cv::flip(tmat_src_bgr, t_src_r, 0);
            cv::imshow("tailored_map", t_src_r);
            cv::waitKey(1);
#endif

            // fill tailored submap cell value into odom global map
            geometry_msgs::Point pt_local_submap;
            geometry_msgs::Pose ps_local_submap;
            geometry_msgs::Point pt_global_submap;
            geometry_msgs::Pose ps_global_submap;
            geometry_msgs::Pose ps_global_odom;
            int global_map_x, global_map_y;
            int ref_in_odom_x, ref_in_odom_y;

            auto start = std::chrono::system_clock::now();

            tf::Transform all_trans;
            all_trans = worldToMap(initial_global_vehicle_pos_) * mapToWorld(tailored_submap.triD_submap_pose);
            for(int i = 0; i < tailored_submap.height; i++) {
                for(int j = 0; j < tailored_submap.width; j++) {
                    int index_in_tailored_map = i * tailored_submap.width + j;
                    // -->local_submap
                    pt_local_submap.x = (j - tailored_submap.triD_submap_pose_image_index_x) * tailored_submap.resolution;
                    pt_local_submap.y = (i - tailored_submap.triD_submap_pose_image_index_y) * tailored_submap.resolution;
                    pt_local_submap.z = 0;
                    ps_local_submap.position = pt_local_submap;
                    ps_local_submap.orientation = tailored_submap.triD_submap_pose.orientation;

                    // -->global_submap
                    {
                        tf::Pose ps;
                        tf::poseMsgToTF(ps_local_submap, ps);
                        tf::poseTFToMsg(all_trans * ps, ps_global_odom);
                    }
//                    if(0) {
//                        // todo first multiply matrix, then .. faster!
//                        tf::Point pt(pt_local_submap.x, pt_local_submap.y, 0);
//                        tf::pointTFToMsg(mapToWorld(tailored_submap.triD_submap_pose) * pt, pt_global_submap);
//                        ps_global_submap.position = pt_global_submap;
//                        ps_global_submap.orientation = tailored_submap.triD_submap_pose.orientation;
//                        // -->global_odom
//                        tf::Pose ps;
//                        tf::poseMsgToTF(ps_global_submap, ps);
//                        // todo ps : yaw use own , roll and pitch use initial !
//                        tf::poseTFToMsg(worldToMap(initial_global_vehicle_pos_) * ps, ps_global_odom);
//                    }

                    global_map_x = floor((ps_global_odom.position.x  - map_.info.origin.position.x) / map_.info.resolution);
                    global_map_y = floor((ps_global_odom.position.y  - map_.info.origin.position.y) / map_.info.resolution);
                    int index_in_global_map = global_map_y * map_.info.width + global_map_x;
                    if(global_map_x > 0 && global_map_x < map_.info.width &&
                            global_map_y > 0 && global_map_y < map_.info.height) {
                        if(i == tailored_submap.triD_submap_pose_image_index_y
                                && j == tailored_submap.triD_submap_pose_image_index_x)
                        {
                            ref_in_odom_x = global_map_x;
                            ref_in_odom_y = global_map_y;
                        }
                        if(tailored_submap.cells[index_in_tailored_map] == 1) {
                            map_.data[index_in_global_map] = 0;
                        } else if(tailored_submap.cells[index_in_tailored_map] == 2) {
                            map_.data[index_in_global_map] = 100;
                        } else {
                            // todo only consider obs and free
//                            map_.data[index_in_global_map] = -1;
                        }
                    }
                }
            }
            auto end = std::chrono::system_clock::now();
            auto msec = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
            std::cout << "all project ...  cost time msec :" << msec << "\n";
#ifdef OPENCV_SHOW
            int occ_map_width = map_.info.width;
            int occ_map_height = map_.info.height;
            cv::Mat occ_mat_src(occ_map_width, occ_map_height, CV_8UC1, cv::Scalar(127));
            for(int i = 0; i < occ_map_height; i++) {
                for(int j = 0; j < occ_map_width; j++) {
                    int index = i * occ_map_width + j;
                    if(map_.data[index] == 0) {
                        occ_mat_src.at<uchar>(i, j) = 255;
                    } else if(map_.data[index] == 100) {
                        occ_mat_src.at<uchar>(i, j) = 0;
                    } else {
                        occ_mat_src.at<uchar>(i, j) = 127;
                    }
                }
            }

            cv::Mat occ_mat_bgr;
            cv::cvtColor(occ_mat_src, occ_mat_bgr, CV_GRAY2BGR);
            cv::Point2d current_pos;
            current_pos.x = (current_odom_vehicle_pos_.position.x - map_.info.origin.position.x) / map_.info.resolution;
            current_pos.y = (current_odom_vehicle_pos_.position.y - map_.info.origin.position.y) / map_.info.resolution;
            cv::circle(occ_mat_bgr, current_pos, 5, cv::Scalar(0, 0, 255), -1); // red
            cv::circle(occ_mat_bgr, cv::Point(ref_in_odom_x, ref_in_odom_y), 5, cv::Scalar(255, 0, 0), -1); // blue

            cv::Mat occ_mat_bgr_r;
            cv::flip(occ_mat_bgr, occ_mat_bgr_r, 0);
            cv::imshow("global_map", occ_mat_bgr_r);
            cv::waitKey(1);
#endif
        }


    }

    void MapBuilder::tailorSubmap(const iv_slam_ros_msgs::TraversibleArea &traver_map,
                                  iv_slam_ros_msgs::TraversibleArea &tailored_submap) {

        auto submap_x = traver_map.triD_submap_pose_image_index_x;
        auto submap_y = traver_map.triD_submap_pose_image_index_y;
        int shift_cell_x = tailored_submap_width_ / traver_map.resolution / 2;
        int nagative_shift_cell_y = tailored_submap_x2base_ / traver_map.resolution;
        int positive_shift_cell_y = (tailored_submap_height_ - tailored_submap_x2base_) / traver_map.resolution;

        tailored_submap.header = traver_map.header;
        tailored_submap.resolution = traver_map.resolution;
        tailored_submap.triD_submap_pose = traver_map.triD_submap_pose;


        int start_index_x = submap_x - shift_cell_x < 0 ? 0 : submap_x - shift_cell_x;
        int end_index_x = submap_x + shift_cell_x > traver_map.width ? traver_map.width : submap_x + shift_cell_x ;
        int start_index_y = submap_y - nagative_shift_cell_y < 0 ? 0 : submap_y - nagative_shift_cell_y ;
        int end_index_y = submap_y + positive_shift_cell_y > traver_map.height ? traver_map.height : submap_y + positive_shift_cell_y;


        tailored_submap.width = end_index_x - start_index_x ;
        tailored_submap.height = end_index_y - start_index_y;
        tailored_submap.triD_submap_pose_image_index_x = submap_x - start_index_x;
        tailored_submap.triD_submap_pose_image_index_y = submap_y - start_index_y;
        tailored_submap.cells.assign(tailored_submap.width * tailored_submap.height, 0);

//        ROS_INFO("tailor_map: width --> %d  height --> %d , ref position :(%d, %d)", tailored_submap.width, tailored_submap.height,
//                 submap_x - start_index_x, submap_y - start_index_y);

        uint8 value;
        int y_tailor = 0;
        for(int y = start_index_y; y < end_index_y; ++y, ++y_tailor) {
            int x_tailor = 0;
            for(int x = start_index_x; x < end_index_x; ++x, ++x_tailor) {
                value = traver_map.cells[y * traver_map.width + x];
//                if(value == 2) {
//                  ROS_INFO("""""""");
//                }
                tailored_submap.cells[y_tailor * tailored_submap.width + x_tailor] = value;

            }
        }
        //todo check bug
//        for(int i = 0; i < tailored_submap.height && start_index_y < end_index_y; i++, start_index_y++) {
//            for(int j = 0; j < tailored_submap.width && start_index_x < end_index_x; j++, start_index_x++) {
//                int index_in_tailor = i * tailored_submap.width + j;
//                int index_in_submap = start_index_y * traver_map.width + start_index_x;
//                ROS_INFO("---------%d", traver_map.cells[index_in_submap]);
//                if(traver_map.cells[index_in_submap] == 2) {
//                    tailored_submap.cells[index_in_tailor] = traver_map.cells[index_in_submap];
//                }
//            }
//        }

    }

    void MapBuilder::broadcastTransformBetweenVehicleAndOdom() {
        tf::Transform transform;
        transform.setOrigin(tf::Vector3(current_odom_vehicle_pos_.position.x,
        current_odom_vehicle_pos_.position.y, 0));
        tf::Quaternion q;
        tf::quaternionMsgToTF(current_odom_vehicle_pos_.orientation, q);
        transform.setRotation(q);
        br_.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "/odom", "base_link"));

    }
}