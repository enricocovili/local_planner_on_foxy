#ifndef SKIDPAD_H
#define SKIDPAD_H

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/point.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <mmr_base/msg/marker_array.hpp>
#include "mmr_base/msg/race_status.hpp"
#include <unistd.h>

struct SkidpadCircle
{
	double x;
	double y;
    double r;
};

class SkidpadPlanner
{
    public:
        SkidpadPlanner(const rclcpp::Node::SharedPtr &nh,
					   const rclcpp::Publisher<mmr_base::msg::MarkerArray>::SharedPtr &bordersPub,
					   const rclcpp::Publisher<mmr_base::msg::Marker>::SharedPtr &centerLinePub);

        void loadParameters();

		void raceStatusCallBack(mmr_base::msg::RaceStatus::SharedPtr raceStatus);

		void odometryCallback(nav_msgs::msg::Odometry::SharedPtr odometry);

		void slamConesCallback(mmr_base::msg::Marker::SharedPtr slamCones);

        std::vector<geometry_msgs::msg::Point> generateDiscretizedLine(const geometry_msgs::msg::Point &pointStart,
										           const geometry_msgs::msg::Point &pointEnd);

        std::vector<geometry_msgs::msg::Point> generateDiscretizedCircle(const SkidpadCircle &circle,
                                                     const bool &borderType);

        void generateCircleCenter(const std::vector<geometry_msgs::msg::Point> &slamCones,
                                  const bool &borderType,
                                  const double &centerError,
                                  const double &radiusError);

		void generateBorder(const std::vector<geometry_msgs::msg::Point> &slamCones,
                            const bool &borderType);

		void generateCenterLine();

    private:
        rclcpp::Node::SharedPtr nh;
		rclcpp::Publisher<mmr_base::msg::MarkerArray>::SharedPtr bordersPub;
		rclcpp::Publisher<mmr_base::msg::Marker>::SharedPtr centerLinePub;

        mmr_base::msg::MarkerArray borders;
        mmr_base::msg::RaceStatus raceStatus;
		geometry_msgs::msg::Point odometry;

       SkidpadCircle circleL;
       SkidpadCircle circleR;

        bool firstAlign;
        int lastOdometry;

        double param_centerX;
        double param_centerY;
        double param_innerRadius;
        double param_centerRadius;
        double param_outerRadius;
        double param_firstAlignCenterError;
        double param_firstAlignRadiusError;
        double param_errorIncrement;
        double param_centerError;
        double param_radiusError;
        double param_radiusOffsetL;
        double param_radiusOffsetR;
        int param_circlePoints;
        int param_linePoints;

};

#endif // SKIDPAD_H