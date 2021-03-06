/*
 * test_training.cpp
 *
 *  Created on: Mar 9, 2012
 *      Author: aitor
 */
#include <pcl/pcl_macros.h>
//#include <pcl/apps/3d_rec_framework/pipeline/global_nn_classifier.h>
//#include "cus_global_nn_classifier.h"
//#include <pcl/apps/3d_rec_framework/pipeline/global_nn_recognizer_cvfh.h>
#include "cus_global_nn_recognizer_ourcvfh.h"
#include <pcl/apps/3d_rec_framework/pc_source/mesh_source.h>
//#include <pcl/apps/3d_rec_framework/feature_wrapper/global/vfh_estimator.h>
//#include <pcl/apps/3d_rec_framework/feature_wrapper/global/esf_estimator.h>
//#include <pcl/apps/3d_rec_framework/feature_wrapper/global/cvfh_estimator.h>
//#include <pcl/apps/3d_rec_framework/feature_wrapper/global/ourcvfh_estimator.h>
#include "cus_ourcvfh_estimator.h"
//#include <pcl/apps/3d_rec_framework/tools/openni_frame_source.h>
#include <pcl/apps/3d_rec_framework/utils/metrics.h>
//#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/apps/dominant_plane_segmentation.h>
#include <pcl/console/parse.h>
#include <pcl/console/time.h>
#include "pcl_ros/point_cloud.h"
#include "geometries.hpp"
#include "ros/ros.h"
#include <dynamic_reconfigure/server.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pwd.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <geometry_msgs/Point.h>
#include <pcl/recognition/hv/hypotheses_verification.h>
#include <pcl/recognition/hv/hv_go.h>
#include "fast_segmentation.hpp"
#include <object_vision/recognitionConfig.h>

#define VISUALIZE_BB
#define VISUALIZE_MODEL
#define SAVE_PCD

typedef pcl::PointXYZ PointType;
typedef pcl::PointCloud<pcl::PointXYZ> CloudType;

// testing branch is working on our-cvfh
//static pcl::rec_3d_framework::GlobalNNCVFHPipeline<Metrics::HistIntersectionUnionDistance, PointType, pcl::VFHSignature308> global;
static cus_rec_3d_framework::GlobalNNCVFHRecognizer<Metrics::HistIntersectionUnionDistance, PointType, pcl::VFHSignature308> global;
ros::Publisher marker_pub;
ros::Publisher model_pub;
static size_t prev_marker_size;
static cus_obj_seg::FastObjectSegmentation cus_dps;
static pcl::apps::DominantPlaneSegmentation<PointType> dps;
static bool useCustSegmentation;

static object_vision::recognitionConfig last_config;

//static struct settings
//{
//    bool cus_seg;
//    float leaf_size;
//    float cluster_tolerance;
//    float min_angle;
//    float max_angle;
//    float cluster_tolerance;
//    float min_cluster_size;
//    float object_min_height;
//    float


//};


void cfg_cb(object_vision::recognitionConfig &config, uint32_t level) {
    ROS_INFO("Reconfigure Request:");
    //  ROS_INFO("Reconfigure Request: %d %f %s %s %d",
//            config.int_param, config.double_param,
//            config.str_param.c_str(),
//            config.bool_param?"True":"False",
//            config.size);
    useCustSegmentation = config.use_custom_dps;

    cus_dps.setLeafSize(config.downsample_size);
    dps.setDownsamplingSize(config.downsample_size);

    cus_dps.setMinAngle(config.min_angle);
    cus_dps.setMaxAngle(config.max_angle);

    cus_dps.setClusterTolerance(config.cluster_tolerance);
    dps.setDistanceBetweenClusters(config.cluster_tolerance);

    cus_dps.setMinClusterSize(config.min_cluster_size);
    dps.setMinClusterSize(config.min_cluster_size);

    cus_dps.setMinObjectDistance(config.object_min_height);
    dps.setObjectMinHeight(config.object_min_height);

    cus_dps.setMaxObjectDistance(config.object_max_height);
    dps.setObjectMaxHeight(config.object_max_height);

    global.setICPIterations(config.icp_iterations);

    last_config = config;

    // will force a retrain if new angle is different
    //global.setEpsAngle(config.epsilon_angle_thresh / (180 * M_PI)); // 5 degrees);
}

void clearOldMarkers(std::string frame_id, size_t num_markers, std::string ns1, std::string ns2)
{

pcl::ScopeTime cluster_process_time ("Deleting old markers ------------------ ");
  visualization_msgs::MarkerArray del_marker_array;
  for (int id=0; id < num_markers; id++)
    {
      visualization_msgs::Marker delete_obj_marker;
      delete_obj_marker.header.stamp = ros::Time::now();
      delete_obj_marker.header.frame_id = frame_id;
      delete_obj_marker.id = id;
      delete_obj_marker.action = visualization_msgs::Marker::DELETE;
      delete_obj_marker.ns = ns1;
      del_marker_array.markers.push_back(delete_obj_marker);

      visualization_msgs::Marker delete_text_marker;
      delete_text_marker.header.stamp = ros::Time::now();
      delete_text_marker.header.frame_id = frame_id;
      delete_text_marker.id = id;
      delete_text_marker.action = visualization_msgs::Marker::DELETE;
      delete_text_marker.ns = ns2;
      del_marker_array.markers.push_back(delete_text_marker);

    }
  marker_pub.publish(del_marker_array);
}

void
segmentAndClassify_cb (const sensor_msgs::PointCloud2ConstPtr& cloud_msg)
{
    //bool useCustSegmentation = false;


    // Set our initial shape type to be a cube
    uint32_t shape = visualization_msgs::Marker::CUBE;

    visualization_msgs::MarkerArray marker_array;
    std::string marker_ns_obj = "object";
    std::string marker_ns_text = "object_desc";
    std::string frame_id = global.getFrameID();

    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_scene (new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::fromROSMsg(*cloud_msg, *cloud_scene);
    cloud_scene->is_dense = false;

    float Z_DIST_ = 1.5f;
    float text_scale = 0.02f;
    float distance_thresh = 0.43f;

    pcl::ScopeTime frame_process ("Global frame processing ------------- ");
    //frame = camera.snap ();
    CloudType::Ptr xyz_points (new CloudType);
    pcl::copyPointCloud (*cloud_scene, *xyz_points);

//std::cout << "Removing color from scene, width: " << xyz_points->width << "height: " << xyz_points->height << " IsDense: " << cloud_scene->is_dense << std::endl;

    pcl::console::TicToc tt_seg;
    tt_seg.tic();


    //Step 1 -> Segment

    std::vector<CloudType::Ptr> clusters;
    std::vector<pcl::PointIndices> indices;
    if(useCustSegmentation){
        std::cout << " ---- using custom object segmentation pipeline ----" << std::endl;
        cus_dps.setInputCloud(xyz_points);
        cus_dps.compute(clusters);
        cus_dps.getClusterIndices(indices);

    }
    else
    {

        pcl::apps::DominantPlaneSegmentation<PointType> l_dps;
        std::cout << " ---- using pcl app object segmentation pipeline ----" << std::endl;
        l_dps.setInputCloud (xyz_points);
        l_dps.setMaxZBounds (Z_DIST_);
        l_dps.setObjectMinHeight (0.02);
        l_dps.setMinClusterSize (50);
        l_dps.setWSize (9);
        l_dps.setDistanceBetweenClusters (0.25f);
        l_dps.setObjectMaxHeight(0.5);
        l_dps.setDownsamplingSize (0.02f);

        std::cout << "object_min_height: " << last_config.object_min_height << std::endl;
        l_dps.setObjectMinHeight (last_config.object_min_height);
        std::cout << "min_cluster_size" << last_config.min_cluster_size << std::endl;
        l_dps.setMinClusterSize (last_config.min_cluster_size);
        l_dps.setWSize (9);
        std::cout << "cluster_tolerance" << last_config.cluster_tolerance << std::endl;
        l_dps.setDistanceBetweenClusters (last_config.cluster_tolerance);
        std::cout << "object_max_height" << last_config.object_max_height << std::endl;
        l_dps.setObjectMaxHeight(last_config.object_max_height);
        std::cout << "downsample_size" << last_config.downsample_size << std::endl;
        l_dps.setDownsamplingSize (last_config.downsample_size);
        l_dps.compute_fast (clusters);
        l_dps.getIndicesClusters (indices);
        //std::cout << "Indicies recieved: " << indices.empty() << std::endl;
        //Eigen::Vector4f table_plane_;
        //dps.getTableCoefficients (table_plane_);
        //Eigen::Vector3f normal_plane_ = Eigen::Vector3f (table_plane_[0], table_plane_[1], table_plane_[2]);

    }

    std::cout << "[Segmentation done, " << tt_seg.toc() << " ms] \n";

    pcl::console::TicToc tt;
    tt.tic();

    size_t cur_marker_size = 0;
    CloudType::Ptr models_out (new CloudType);
    std::cout << "Indicies size " << indices.size() << " Clusters size " << clusters.size() << std::endl;
    for (size_t i = 0; i < clusters.size (); i++)
    {

#ifdef SAVE_PCD
        pcl::PCDWriter writer;
        std::stringstream obj_file_name;
        obj_file_name << "cloud_objects_" << i << "_cus_seg_" << useCustSegmentation << ".pcd";
        std::cout << "Saving cluster cloud as " << obj_file_name.str() << std::endl;
//        CloudType::Ptr cluster_to_save (new CloudType);
//        pcl::copyPointCloud(*clusters[i], *cluster_to_save);
        writer.write<PointType>(obj_file_name.str(), *clusters[i], false);
#endif

        std::ostringstream prob_str;
        prob_str.precision (4);

        std::stringstream clus_proc_time_str;
        clus_proc_time_str << "Catagorized cluster " << i << " --------";
        pcl::ScopeTime cluster_process_time (clus_proc_time_str.str().c_str());
//std::cout << "working on " << i << "th cluster" << std::endl;
        std::stringstream cluster_name;
        cluster_name << "cluster_" << i;
        global.setInputCloud (xyz_points);
        global.setIndices (indices[i].indices);
        //global.classify ();
        global.recognize();

        //std::vector < std::string > categories;
        std::vector<float> dists;
        boost::shared_ptr<std::vector<pcl::rec_3d_framework::Model<PointType> > > models;
        boost::shared_ptr<std::vector<Eigen::Matrix4f, Eigen::aligned_allocator<Eigen::Matrix4f> > > transforms;

        global.getDescriptorDistances(dists);
std::cout << "Total number of distances: " << dists.size() << std::endl;
        models = global.getModels();
std::cout << "Total number of models: " << models->size() << std::endl;
        transforms = global.getTransforms();
std::cout << "Total number of transforms: " << transforms->size() << std::endl;

        CloudType::Ptr transform_cloud(new CloudType);
        if(models->size() > 0)
        {
            //pcl::ScopeTime model_grab_process ("Grabbing model ------------- ");
            pcl::rec_3d_framework::Model<PointType> best_model = (*models).at(0);
            prob_str << "Best match: Model ID: " << best_model.id_ << ", in class: " << best_model.class_<< ", at distance " << dists[0] << std::endl;
            if(dists[0] < distance_thresh)
            {
                pcl::ScopeTime transforming_process ("Found Match -> Transforming model ------------- ");
                pcl::transformPointCloud(*best_model.getAssembled(0.01f), *transform_cloud, (*transforms)[0]);
                *models_out += *transform_cloud;
#ifdef VISUALIZE_BB
                if(global.getCreateModelBoxes())
                {
                    PointType centroid;
                    pcl::computeCentroid(*transform_cloud, centroid);
                    boost::shared_ptr<c44::BoundingBox> obj_box = global.getBoundingBoxOfModel(best_model.id_); //(clusters[i], frame_id, prob_str.str(), i, marker_ns_obj, marker_ns_text);
                    // std::cout << "Inside loop, Marker " << i << " type: " << obj_box.text_marker.type << std::endl;
                    obj_box->text_marker.text = prob_str.str();
                    obj_box->transform_Markers((*transforms)[0], centroid);
                    marker_array.markers.push_back(obj_box->obj_marker);
                    marker_array.markers.push_back(obj_box->text_marker);
                    cur_marker_size += 2;
                }
#endif
            }
        }
    }

#ifdef VISUALIZE_MODEL
    if(models_out->size() > 0)
    {
        sensor_msgs::PointCloud2::Ptr model_msg (new sensor_msgs::PointCloud2);
        pcl::toROSMsg(*models_out, *model_msg);
        model_msg->header.frame_id = frame_id;
        model_msg->header.stamp = ros::Time::now();
        model_pub.publish(*model_msg);

#ifdef VISUALIZE_BB
std::cout << "Previous Marker size: " << prev_marker_size << ", Current Marker size: " << cur_marker_size << std::endl;

        clearOldMarkers(frame_id, prev_marker_size, marker_ns_obj, marker_ns_text);
std::cout << "Number of markers to publish" << marker_array.markers.size() << endl;
        marker_pub.publish(marker_array);

        prev_marker_size = marker_array.markers.size();
#endif
    }
#endif


    std::cout << "[Classification done, " << tt.toc() << " ms] \n";
std::cout << "Objects segmented: " << clusters.size() << std::endl;
}

int
main (int argc, char ** argv)
{
    prev_marker_size = 0;
    const char* home = getenv("HOME");

    if(!home){
        std::cerr << "Can't find home directory";
    }
    std::string home_path(home);

    //std::string scene_path = "scene.pcd";
    std::string model_dir = home_path + "/model_db/models"; //home_path + "/catkin_ws/src/object_vision_3D/classification/models";
    std::string desc_name = "our-cvfh";
    std::string training_dir = home_path + "/model_db/trained_models"; //home_path + "/catkin_ws/src/object_vision_3D/classification/trained_models";
    std::string frame_id = "camera_depth_optical_frame";
    int NN = 10;



    // set defaults for object segmentation
    useCustSegmentation = false;
    cus_dps.setLeafSize(0.02);
    cus_dps.setNeighbors_to_analyze(20);
    cus_dps.setStdDeviation(1.0);
    cus_dps.setMinAngle(0);
    cus_dps.setMaxAngle(1.57);
    cus_dps.setRansacIterations(1000);
    cus_dps.setThresholdDistance(0.01);
    cus_dps.setMinObjectDistance(0.02);
    cus_dps.setMaxObjectDistance(0.5);
    cus_dps.setClusterTolerance(0.05);
    cus_dps.setMinClusterSize(20);
    cus_dps.setMaxClusterSize(1000);

    ros::init(argc, argv, "quanta_obj_seg");
    ros::NodeHandle nh;
    ros::NodeHandle priv_nh;


    //pcl::console::parse_argument (argc, argv, "-scene_path", scene_path);
    //pcl::console::parse_argument (argc, argv, "-models_dir", path);
    //pcl::console::parse_argument (argc, argv, "-training_dir", training_dir);
    //pcl::console::parse_argument (argc, argv, "-descriptor_name", desc_name);
    //pcl::console::parse_argument (argc, argv, "-nn", NN);

    //pcl::console::parse_argument (argc, argv, "-z_dist", chop_at_z_);
    //pcl::console::parse_argument (argc, argv, "-tesselation_level", views_level_);

    boost::shared_ptr<pcl::rec_3d_framework::MeshSource<PointType> > mesh_source (new pcl::rec_3d_framework::MeshSource<PointType>);
    mesh_source->setPath (model_dir);
    mesh_source->setResolution (150);
    mesh_source->setTesselationLevel (1);
    mesh_source->setViewAngle (57.f);
    mesh_source->setRadiusSphere (1.5f);
    mesh_source->setModelScale (1.f);
    mesh_source->generate (training_dir);

    boost::shared_ptr<pcl::rec_3d_framework::Source<PointType> > cast_source;
    cast_source = boost::static_pointer_cast<pcl::rec_3d_framework::MeshSource<PointType> > (mesh_source);

    boost::shared_ptr<pcl::rec_3d_framework::PreProcessorAndNormalEstimator<PointType, pcl::Normal> > normal_estimator;
    normal_estimator.reset (new pcl::rec_3d_framework::PreProcessorAndNormalEstimator<PointType, pcl::Normal>);
    //normal_estimator->normal_radius_ = 0.03;
    normal_estimator->setCMR (true);
    normal_estimator->setDoVoxelGrid (true);
    normal_estimator->setRemoveOutliers (true);
    normal_estimator->setFactorsForCMR (3, 7);
    //normal_estimator->setCMR(false);
    //normal_estimator->setValuesForCMRFalse(0.005f, 0.03f);

    boost::shared_ptr<cus_rec_3d_framework::OURCVFHEstimator<PointType, pcl::VFHSignature308> > ourcvfh_estimator;
    ourcvfh_estimator.reset (new cus_rec_3d_framework::OURCVFHEstimator<PointType, pcl::VFHSignature308>);
    ourcvfh_estimator->setNormalEstimator (normal_estimator);
    float eps_angle_thresh = 20 / (180 * M_PI); // 5 degrees
    ourcvfh_estimator->setCVFHParams(eps_angle_thresh, 1.0f, 6.f);
    ourcvfh_estimator->setMinPoints(10);
    //ourcvfh_estimator->setRefineClustersParam();
    //ourcvfh_estimator->setAdaptativeMLS(true);

//    boost::shared_ptr<pcl::GlobalHypothesesVerification<pcl::PointXYZ, pcl::PointXYZ> > global_hv (
//                                                                                            new pcl::GlobalHypothesesVerification<pcl::PointXYZ,
//                                                                                            pcl::PointXYZ>);
//    //boost::shared_ptr<pcl::HypothesisVerification<PointType, PointType> > global_hv;
//    global_hv->setResolution(0.01f);
//    global_hv->setDetectClutter(true);
//    global_hv->setRadiusClutter(0.10f);
//    global_hv->setInlierThreshold(0.01f);
//    global_hv->setRegularizer(3.0f);
//    global_hv->setClutterRegularizer(7.5f);
//    global_hv->setOcclusionThreshold(0.01f);
//    global_hv->setMaxIterations(5000);

//    boost::shared_ptr<pcl::HypothesisVerification<pcl::PointXYZ, pcl::PointXYZ> > cast_hv_alg;
//    cast_hv_alg = boost::static_pointer_cast<pcl::HypothesisVerification<pcl::PointXYZ, pcl::PointXYZ> > (global_hv);

    //boost::shared_ptr<pcl::rec_3d_framework::GlobalEstimator<PointType, pcl::VFHSignature308> > cast_estimator;
    //cast_estimator = boost::dynamic_pointer_cast<pcl::rec_3d_framework::OURCVFHEstimator<PointType, pcl::VFHSignature308> > (ourcvfh_estimator);
    global.setFrameID(frame_id);
    global.setICPIterations(0);
    global.setCreateModelBoxes(true);
    global.setDataSource (cast_source);
    global.setTrainingDir (training_dir);
    global.setModelDir(model_dir);
    global.setDescriptorName (desc_name);
    global.setFeatureEstimator (ourcvfh_estimator);
    global.setNN (NN);
//    global.setHVAlgorithm(cast_hv_alg);


    dynamic_reconfigure::Server<object_vision::recognitionConfig> server;
    dynamic_reconfigure::Server<object_vision::recognitionConfig>::CallbackType f;

    f = boost::bind(&cfg_cb, _1, _2);
    server.setCallback(f);

    global.initialize (false);

    std::string sub_topic = nh.resolveName("camera/depth/points");
    std::string pub_topic = nh.resolveName("obj_marker");
    uint32_t queue_size = 1;

    // to create a subscriber, you can do this (as above):
    ros::Subscriber sub = nh.subscribe<sensor_msgs::PointCloud2> (sub_topic, queue_size, segmentAndClassify_cb);

    marker_pub = nh.advertise<visualization_msgs::MarkerArray>("visualization_marker", 1);
    model_pub = nh.advertise<sensor_msgs::PointCloud2>("visualization_model", 1);

    ros::spin();

    return 0;
}
