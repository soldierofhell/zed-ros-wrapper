#include "sl_tools.h"
#include "zed_terrain_mapping.hpp"

#include <string>

#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <geometry_msgs/PoseStamped.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#ifdef TERRAIN_MAPPING

using namespace  std;

namespace zed_wrapper {

    ZEDTerrainMapping::ZEDTerrainMapping(ros::NodeHandle nh, ros::NodeHandle nhNs, sl::Camera* zed)
        : mZed(zed) {
        mNh = nh;
        mNhNs = nhNs;

        mMappingReady = false;
        mInitialized = false;
        mLocalTerrainPubRate = 5.0;
        mGlobalTerrainPubRate = 1.0;
    }

    bool ZEDTerrainMapping::init() {

        // Frame names
        mNhNs.param<std::string>("pose_frame", mMapFrameId, "map");
        mNhNs.param<std::string>("odometry_frame", mOdometryFrameId, "odom");
        mNhNs.param<std::string>("base_frame", mBaseFrameId, "base_link");
        mNhNs.param<std::string>("camera_frame", mCameraFrameId, "zed_camera_center");

        // Initialization transformation listener
        mTfBuffer.reset(new tf2_ros::Buffer);
        mTfListener.reset(new tf2_ros::TransformListener(*mTfBuffer));

        // Topic names
        string loc_height_map_topic     = "map/loc_map_heightmap";
        string loc_height_cloud_topic   = "map/loc_map_height_cloud";
        string loc_height_marker_topic  = "map/loc_map_height_cubes";
        string loc_height_markers_topic = "map/loc_map_height_boxes";
        string loc_cost_map_topic       = "map/loc_map_costmap";
        string glob_height_map_topic    = "map/glob_map_heightmap";
        string glob_height_cloud_topic  = "map/glob_map_height_cloud";
        string glob_height_marker_topic = "map/glob_map_height_cubes";
        string glob_cost_map_topic      = "map/glob_map_costmap";
        string height_map_image_topic   = "map/height_map_image";
        string color_map_image_topic    = "map/color_map_image";
        string travers_map_image_topic  = "map/travers_map_image";

        // Terrain Mapping publishers
        mPubLocalHeightMap = mNh.advertise<nav_msgs::OccupancyGrid>(loc_height_map_topic, 1); // local height map
        ROS_INFO_STREAM("Advertised on topic " << loc_height_map_topic);
        mPubLocalHeightCloud = mNh.advertise<sensor_msgs::PointCloud2>(loc_height_cloud_topic, 1); // local height cloud
        ROS_INFO_STREAM("Advertised on topic " << loc_height_cloud_topic);
        mPubLocalHeightMrk = mNh.advertise<visualization_msgs::Marker>(loc_height_marker_topic, 1); // local height cubes
        ROS_INFO_STREAM("Advertised on topic " << loc_height_marker_topic);
        mPubLocalHeightMrks = mNh.advertise<visualization_msgs::MarkerArray>(loc_height_markers_topic, 1); // local height boxes
        ROS_INFO_STREAM("Advertised on topic " << loc_height_markers_topic);
        mPubLocalCostMap = mNh.advertise<nav_msgs::OccupancyGrid>(loc_cost_map_topic, 1); // local cost map
        ROS_INFO_STREAM("Advertised on topic " << loc_cost_map_topic);

        mPubGlobalHeightMap = mNh.advertise<nav_msgs::OccupancyGrid>(glob_height_map_topic, 1,
                              boost::bind(&ZEDTerrainMapping::globalMapSubscribeCallback, this, _1),
                              ros::SubscriberStatusCallback(), ros::VoidConstPtr(), true); // global height map latched
        ROS_INFO_STREAM("Advertised on topic " << glob_height_map_topic);
        mPubGlobalHeightCloud = mNh.advertise<sensor_msgs::PointCloud2>(glob_height_cloud_topic, 1,
                                boost::bind(&ZEDTerrainMapping::globalMapSubscribeCallback, this, _1)); // global height cloud
        ROS_INFO_STREAM("Advertised on topic " << glob_height_cloud_topic);
        mPubGlobalHeightMrk = mNh.advertise<visualization_msgs::Marker>(glob_height_marker_topic, 1,
                              boost::bind(&ZEDTerrainMapping::globalMapSubscribeCallback, this, _1)); // global height cubes
        ROS_INFO_STREAM("Advertised on topic " << glob_height_marker_topic);
        mPubGlobalCostMap = mNh.advertise<nav_msgs::OccupancyGrid>(glob_cost_map_topic, 1,
                            boost::bind(&ZEDTerrainMapping::globalMapSubscribeCallback, this, _1), // global cost map latched
                            ros::SubscriberStatusCallback(), ros::VoidConstPtr(), true);
        ROS_INFO_STREAM("Advertised on topic " << glob_cost_map_topic);

        mPubGlobalHeightMapImg = mNh.advertise<sensor_msgs::Image>(height_map_image_topic, 1);
        ROS_INFO_STREAM("Advertised on topic " << height_map_image_topic);
        mPubGlobalColorMapImg = mNh.advertise<sensor_msgs::Image>(color_map_image_topic, 1);
        ROS_INFO_STREAM("Advertised on topic " << color_map_image_topic);
        mPubGlobalCostMapImg = mNh.advertise<sensor_msgs::Image>(travers_map_image_topic, 1);
        ROS_INFO_STREAM("Advertised on topic " << travers_map_image_topic);

        // Mapping services
        mSrvGetStaticMap = mNh.advertiseService("static_map", &ZEDTerrainMapping::on_get_static_map, this);
        mSrvGetLocHeightMap = mNh.advertiseService("local_height_map", &ZEDTerrainMapping::on_get_loc_height_map, this);
        mSrvGetLocCostMap = mNh.advertiseService("local_cost_map", &ZEDTerrainMapping::on_get_loc_height_map, this);
        mSrvGetGlobHeightMap = mNh.advertiseService("global_height_map", &ZEDTerrainMapping::on_get_glob_height_map, this);
        mSrvGetGlobCostMap = mNh.advertiseService("global_cost_map", &ZEDTerrainMapping::on_get_glob_cost_map, this);

        mInitialized = true;
        return true;
    }

    bool ZEDTerrainMapping::startTerrainMapping() {

        if (!mZed) {
            ROS_WARN("ZED Camera not initialized");
            return false;
        }

        if (!mInitialized) {
            if (!init()) {
                return false;
            }
        }

        mNhNs.getParam("loc_terrain_pub_rate",  mLocalTerrainPubRate);
        mNhNs.getParam("glob_terrain_pub_rate", mGlobalTerrainPubRate);

        mNhNs.getParam("default_map", mDefaultMap);

        mNhNs.getParam("mapping_agent_step", mMapAgentStep);
        mNhNs.getParam("mapping_agent_slope", mMapAgentSlope);
        mNhNs.getParam("mapping_agent_radius", mMapAgentRadius);
        mNhNs.getParam("mapping_agent_height", mMapAgentHeight);
        mNhNs.getParam("mapping_agent_roughness", mMapAgentRoughness);

        mNhNs.getParam("mapping_max_depth", mMapMaxDepth);
        mNhNs.getParam("mapping_max_height", mMapMaxHeight);
        mNhNs.getParam("mapping_height_resol", mMapHeightResol);
        mNhNs.getParam("mapping_cell_resol", mMapResolIdx);
        mNhNs.getParam("mapping_local_radius", mMapLocalRadius);


        sl::TerrainMappingParameters terrainParams;

        terrainParams.setAgentParameters(sl::UNIT_METER,
                                         mMapAgentStep, mMapAgentSlope, mMapAgentRadius,
                                         mMapAgentHeight, mMapAgentRoughness);

        sl::TerrainMappingParameters::GRID_RESOLUTION grid_resolution = static_cast<sl::TerrainMappingParameters::GRID_RESOLUTION>(mMapResolIdx);
        mTerrainMapRes = terrainParams.setGridResolution(grid_resolution);

        ROS_INFO_STREAM("Terrain Grid Resolution " << mTerrainMapRes << "m");
        ROS_INFO_STREAM("Terrain Cutting height " << terrainParams.setHeightThreshold(sl::UNIT_METER, mMapMaxHeight) << "m");
        ROS_INFO_STREAM("Terrain Z Resolution " << terrainParams.setZResolution(sl::UNIT_METER, mMapHeightResol) << "m");
        ROS_INFO_STREAM("Terrain Max range " << terrainParams.setRange(sl::UNIT_METER, mMapMaxDepth) << "m");

        terrainParams.enable_traversability_cost_computation = true;
        terrainParams.enable_dynamic_extraction = true;
        terrainParams.enable_color_extraction = true;

        if (mZed->enableTerrainMapping(terrainParams) != sl::SUCCESS) {
            ROS_WARN_STREAM("Terrain Mapping: NOT ENABLED");
            mMappingReady = false;
            return false;
        }

        mGlobMapMutex.lock();
        initGlobalMapMsgs(1, 1);
        mGlobMapMutex.unlock();

        mMappingReady = true;

        // Start Local Terrain Mapping Timer
        mLocalTerrainTimer = mNhNs.createTimer(ros::Duration(1.0 / mLocalTerrainPubRate),
                                               &ZEDTerrainMapping::localTerrainCallback, this);
        ROS_INFO_STREAM("Local Terrain Mapping: ENABLED @ " << mLocalTerrainPubRate << "Hz");

        // Start Global Terrain Mapping Timer
        mGlobalTerrainTimer = mNhNs.createTimer(ros::Duration(1.0 / mGlobalTerrainPubRate),
                                                &ZEDTerrainMapping::globalTerrainCallback, this);
        ROS_INFO_STREAM("Global Terrain Mapping: ENABLED @ " << mGlobalTerrainPubRate << "Hz");

        return true;
    }

    void ZEDTerrainMapping::dynamicReconfCallback(zed_wrapper::TerrainMappingConfig& config, uint32_t level) {
        switch (level) {
        case 0:
            mMapLocalRadius = config.loc_map_radius;
            ROS_INFO("Reconfigure local map radius : %g", mMapLocalRadius);
            break;
        }
    }

    void ZEDTerrainMapping::localTerrainCallback(const ros::TimerEvent& e) {
        if (!mMappingReady) {
            mMappingReady = startTerrainMapping();
        }

        mMappingReady = true;

        // Timer synchronization with Global mapping
        sl::ERROR_CODE res;
        do {
            mTerrainMutex.lock();
            res = mZed->getTerrainRequestStatusAsync();
            if (res != sl::SUCCESS) {
                mZed->requestTerrainAsync(); // if an elaboration is in progress the request is ignored
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                mTerrainMutex.unlock();
            }
        } while (res != sl::SUCCESS);

        uint32_t heightSub = mPubLocalHeightMap.getNumSubscribers();
        uint32_t costSub = mPubLocalCostMap.getNumSubscribers();
        uint32_t cloudSub = mPubLocalHeightCloud.getNumSubscribers();
        uint32_t mrkSub = mPubLocalHeightMrk.getNumSubscribers();
        uint32_t mrksSub = mPubLocalHeightMrks.getNumSubscribers();
        uint32_t run = heightSub + costSub + cloudSub + mrkSub + mrksSub;

        if (run > 0) {
            if (mZed->retrieveTerrainAsync(mTerrain) == sl::SUCCESS) {

                ROS_DEBUG("Local Terrain available");
                sl::timeStamp t = mTerrain.getReferenceTS();

                // Request New Terrain calculation while elaborating data
                mZed->requestTerrainAsync();
                mTerrainMutex.unlock();

                // Local chunks list
                std::vector<sl::HashKey> chunks;

                // Process only Updated Terrain Chuncks
                //chunks = mTerrain.getUpdatedChunks();

                // Camera position in map frame
                // Look up the transformation from base frame to map link
                tf2::Transform cam_to_map;
                try {
                    // Save the transformation from base to frame
                    geometry_msgs::TransformStamped c2m =
                        mTfBuffer->lookupTransform(mMapFrameId, mCameraFrameId,  ros::Time(0));
                    // Get the TF2 transformation
                    tf2::fromMsg(c2m.transform, cam_to_map);
                } catch (tf2::TransformException& ex) {
                    ROS_WARN_THROTTLE(
                        10.0, "The tf from '%s' to '%s' does not seem to be available. "
                        "IMU TF not published!",
                        mCameraFrameId.c_str(), mMapFrameId.c_str());
                    ROS_DEBUG_THROTTLE(1.0, "Transform error: %s", ex.what());
                    return;
                }

                //ROS_DEBUG_STREAM("TF POSE: " << base_to_map.getOrigin().x() << "," << base_to_map.getOrigin().y());
                //ROS_DEBUG_STREAM("ZED POSE: " << mLastZedPose.getTranslation().x << "," << mLastZedPose.getTranslation().y);

                // Process the robot surrounding chunks
                float camX = cam_to_map.getOrigin().x();
                float camY = cam_to_map.getOrigin().y();
                chunks = mTerrain.getSurroundingValidChunks(-camY, camX, mMapLocalRadius); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL

                ROS_DEBUG_STREAM(" ********************** Camera Position: " << camX << "," << camY);

                ROS_DEBUG_STREAM("Terrain chunks updated (local map): " << chunks.size());

                if (chunks.size() > 0) {

                    std::vector<sl::HashKey>::iterator it;

                    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;

                    // Local map limits
                    for (it = chunks.begin(); it != chunks.end(); it++) {
                        sl::HashKey key = *it;
                        sl::TerrainChunk& chunk = mTerrain.getChunk(key);
                        sl::Dimension dim = chunk.getDimension();

                        if (dim.getXmin() < minX) {
                            minX = dim.getXmin();
                        }

                        if (dim.getYmin() < minY) {
                            minY = dim.getYmin();
                        }

                        if (dim.getXmax() > maxX) {
                            maxX = dim.getXmax();
                        }

                        if (dim.getYmax() > maxY) {
                            maxY = dim.getYmax();
                        }
                    }

                    mLocMapMutex.lock();
                    publishLocalMaps(camX, camY, minX, minY, maxX, maxY, chunks, heightSub, costSub, cloudSub, mrkSub, mrksSub, sl_tools::slTime2Ros(t));
                    mLocMapMutex.unlock();
                }
            } else {
                mTerrainMutex.unlock();
                ROS_DEBUG_STREAM("Local terrain not available");
            }
        } else {
            mTerrainMutex.unlock();
        }
    }

    void ZEDTerrainMapping::publishLocalMaps(float camX, float camY, float minX, float minY, float maxX, float maxY,
            std::vector<sl::HashKey>& chunks,
            uint32_t heightSub, uint32_t costSub, uint32_t cloudSub, uint32_t mrkSub, uint32_t mrksSub,
            ros::Time t) {

        float mapMinX = (minY > (camX - mMapLocalRadius)) ? minY : (camX - mMapLocalRadius); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
        float mapMaxX = (maxY < (camX + mMapLocalRadius)) ? maxY : (camX + mMapLocalRadius); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
        //float mapMinX = (minX > (camX-mMapLocalRadius))?minX:(camX-mMapLocalRadius);
        //float mapMaxX = (maxX < (camX+mMapLocalRadius))?maxX:(camX+mMapLocalRadius);
        float mapMinY = (-maxX > (camY - mMapLocalRadius)) ? -maxX : (camY - mMapLocalRadius); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
        float mapMaxY = (-minX < (camY + mMapLocalRadius)) ? -minX : (camY + mMapLocalRadius); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
        //float mapMinY = (minY > (camY-mMapLocalRadius))?minY:(camY-mMapLocalRadius);
        //float mapMaxY = (maxY < (camY+mMapLocalRadius))?maxY:(camY+mMapLocalRadius);

        float mapW = fabs(mapMaxX - mapMinX);
        float mapH = fabs(mapMaxY - mapMinY);

        uint32_t mapRows = static_cast<uint32_t>(ceil(mapH / mTerrainMapRes)) + 1;
        uint32_t mapCols = static_cast<uint32_t>(ceil(mapW / mTerrainMapRes)) + 1;

        uint32_t totCell = mapRows * mapCols;

        ROS_DEBUG_STREAM("Local map origin: [" << mapMinX << "," << mapMinY << "]");
        ROS_DEBUG_STREAM("Local map dimensions: " << mapW << " x " << mapH << " m");
        ROS_DEBUG_STREAM("Local map cell dim: " << mapCols << " x " << mapRows);

        // Pointcloud
        int ptsCount = mapRows * mapCols;
        mLocalHeightPointcloudMsg.header.stamp = t;
        if (mLocalHeightPointcloudMsg.width != mapCols || mLocalHeightPointcloudMsg.height != mapRows) {
            mLocalHeightPointcloudMsg.header.frame_id = mMapFrameId; // Set the header values of the ROS message
            mLocalHeightPointcloudMsg.is_bigendian = false;
            mLocalHeightPointcloudMsg.is_dense = false;

            sensor_msgs::PointCloud2Modifier modifier(mLocalHeightPointcloudMsg);
            modifier.setPointCloud2Fields(4,
                                          "x", 1, sensor_msgs::PointField::FLOAT32,
                                          "y", 1, sensor_msgs::PointField::FLOAT32,
                                          "z", 1, sensor_msgs::PointField::FLOAT32,
                                          "rgb", 1, sensor_msgs::PointField::FLOAT32);

            modifier.resize(ptsCount);
        }

        // MetaData
        nav_msgs::MapMetaData mapInfo;
        mapInfo.resolution = mTerrainMapRes;
        mapInfo.height = mapRows;
        mapInfo.width = mapCols;
        mapInfo.origin.position.x = /*mInitialPoseSl.getTranslation().x + */ mapMinX; // TODO uncomment and test when the bug in SDK is fixed
        mapInfo.origin.position.y = /*mInitialPoseSl.getTranslation().x + */ mapMinY; // TODO uncomment and test when the bug in SDK is fixed
        mapInfo.origin.position.z = 0.0;
        mapInfo.origin.orientation.x = 0;
        mapInfo.origin.orientation.y = 0;
        mapInfo.origin.orientation.z = 0;
        mapInfo.origin.orientation.w = 1;
        mapInfo.map_load_time = t;

        // Height Map Message as OccupancyGrid
        mLocHeightMapMsg.info = mapInfo;
        mLocHeightMapMsg.header.frame_id = mMapFrameId;
        mLocHeightMapMsg.header.stamp = t;
        mLocHeightMapMsg.data = std::vector<int8_t>(totCell, -1);

        // Cost Map Message as OccupancyGrid
        mLocCostMapMsg.info = mapInfo;
        mLocCostMapMsg.header.frame_id = mMapFrameId;
        mLocCostMapMsg.header.stamp = t;
        mLocCostMapMsg.data = std::vector<int8_t>(totCell, -1);

        // Height Marker
        visualization_msgs::Marker marker;
        if (mrkSub) {
            marker.header.frame_id = mMapFrameId;
            marker.header.stamp = t;
            marker.ns = "height_cubes";
            marker.id = 0;
            marker.type = visualization_msgs::Marker::CUBE_LIST;
            marker.pose.position.x = 0;
            marker.pose.position.y = 0;
            marker.pose.position.z = 0;
            marker.pose.orientation.x = 0.0;
            marker.pose.orientation.y = 0.0;
            marker.pose.orientation.z = 0.0;
            marker.pose.orientation.w = 1.0;
            marker.scale.x = mTerrainMapRes;
            marker.scale.y = mTerrainMapRes;
            marker.scale.z = mMapHeightResol;
            marker.lifetime = ros::Duration(1.0);
            marker.action = visualization_msgs::Marker::MODIFY;
        }

        // Height MarkerArray
        visualization_msgs::MarkerArray markers;

        #pragma omp parallel for
        for (int k = 0; k < chunks.size(); k++) {
            //ROS_DEBUG("*** NEW CHUNK parsing ***");
            sl::HashKey key = chunks.at(k);
            sl::TerrainChunk chunk = mTerrain.getChunk(key);

            sl::Dimension dim = chunk.getDimension();
            unsigned int cellCount = dim.getFullSizeIdx();

            #pragma omp parallel for
            for (unsigned int i = 0; i < cellCount; i++) {
                if (!chunk.isCellValid(i)) { // Leave the value to its default: -1
                    continue;
                }

                float xm, ym;
                if (dim.index2x_y(i, xm, ym)) {
                    continue; // Index out of range
                }

                float dist = sqrt((-xm - camY) * (-xm - camY) + (ym - camX) * (ym - camX)); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                // float dist = sqrt((xm - camX) * (xm - camX) + (ym - camY) * (ym - camY));

                if (dist > mMapLocalRadius) {
                    continue;
                }

                // (xm,ym) to ROS map index
                uint32_t v = static_cast<uint32_t>(round((-xm - mapMinY) / mTerrainMapRes)); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                uint32_t u = static_cast<uint32_t>(round((ym - mapMinX) / mTerrainMapRes)); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                //uint32_t u = static_cast<uint32_t>(round((xm - mapMinY) / mTerrainMapRes));
                //uint32_t v = static_cast<uint32_t>(round((ym - mapMinX) / mTerrainMapRes));

                uint32_t mapIdx = u + v * mapCols;

                if (mapIdx >= totCell) {
                    ROS_DEBUG_STREAM("[Local map] Cell OUT OF RANGE: [" << u << "," << v << "] -> " << mapIdx);
                    continue;
                }

                // Cost Map
                if (costSub > 0) {
                    int8_t cost = static_cast<int8_t>(chunk.at(sl::TRAVERSABILITY_COST, i) * 100);
                    mLocCostMapMsg.data.at(mapIdx) = cost;
                }

                if (cloudSub > 0 || heightSub > 0 || mrkSub > 0 || mrksSub > 0) {
                    float height = chunk.at(sl::ELEVATION, i);
                    int8_t heightAbs = static_cast<int8_t>(fabs(round(height / mMapMaxHeight) * 100));

                    // Height Map
                    if (heightSub > 0) {
                        mLocHeightMapMsg.data.at(mapIdx) = heightAbs;
                    }

                    if (cloudSub > 0 || mrkSub > 0 || mrksSub > 0) {
                        float color_f = static_cast<float>(chunk.at(sl::COLOR, i));
                        sl::float3 color = sl_tools::depackColor3f(color_f);

                        //PointCloud
                        if (cloudSub > 0) {
                            float* ptCloudPtr = (float*)(&mLocalHeightPointcloudMsg.data[0]);
                            ptCloudPtr[mapIdx * 4 + 0] = ym + (mTerrainMapRes / 2);  // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                            ptCloudPtr[mapIdx * 4 + 1] = -xm  + (mTerrainMapRes / 2); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                            ptCloudPtr[mapIdx * 4 + 2] = height;
                            ptCloudPtr[mapIdx * 4 + 3] = color_f;
                        }

                        // Cube List
                        if (mrkSub > 0) {
                            int col_count = static_cast<int>(ceil(fabs(height) / mMapHeightResol));

                            #pragma omp critical
                            {
                                #pragma omp parallel for
                                for (int i = 1; i <= col_count; i++) {
                                    geometry_msgs::Point pt;
                                    pt.x = ym + (mTerrainMapRes / 2);  // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                                    pt.y = -xm  + (mTerrainMapRes / 2); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                                    pt.z = i * mMapHeightResol;

                                    if (height < 0) {
                                        pt.z *= - 1.0f;
                                    }

                                    //ROS_INFO("Height: %g -> Squares: %d -> i: %d -> Current: %g", height, col_count, i, pt.z);

                                    std_msgs::ColorRGBA col;
                                    col.a = 1.0f;
                                    col.r = color[0];
                                    col.g = color[1];
                                    col.b = color[2];

                                    marker.points.push_back(pt);
                                    marker.colors.push_back(col);
                                }
                            }
                        }

                        // Parallelepipeds
                        if (mrksSub > 0 && fabs(height) > 0) {
                            visualization_msgs::Marker heightBox;
                            heightBox.header.frame_id = mMapFrameId;
                            heightBox.header.stamp = t;
                            heightBox.ns = "height_boxes";
                            heightBox.id = mapIdx;
                            heightBox.type = visualization_msgs::Marker::CUBE;
                            heightBox.pose.position.x = ym + (mTerrainMapRes / 2);  // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                            heightBox.pose.position.y = -xm  + (mTerrainMapRes / 2); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                            heightBox.pose.position.z = height / 2;
                            heightBox.pose.orientation.x = 0.0;
                            heightBox.pose.orientation.y = 0.0;
                            heightBox.pose.orientation.z = 0.0;
                            heightBox.pose.orientation.w = 1.0;
                            heightBox.scale.x = mTerrainMapRes;
                            heightBox.scale.y = mTerrainMapRes;
                            heightBox.scale.z = height;
                            heightBox.color.a = 1.0;
                            heightBox.color.r = color.r;
                            heightBox.color.g = color.g;
                            heightBox.color.b = color.b;
                            heightBox.action = visualization_msgs::Marker::MODIFY;
                            heightBox.lifetime = ros::Duration(1.0 / mLocalTerrainPubRate);

                            #pragma omp critical
                            markers.markers.push_back(heightBox);
                        }
                    }
                }
            }
        }


        // Map publishing
        if (heightSub > 0) {
            mPubLocalHeightMap.publish(mLocHeightMapMsg);
        }

        if (costSub > 0) {
            mPubLocalCostMap.publish(mLocCostMapMsg);
        }

        if (cloudSub > 0) {
            mPubLocalHeightCloud.publish(mLocalHeightPointcloudMsg);
        }

        if (mrkSub > 0) {
            mPubLocalHeightMrk.publish(marker);
        }

        if (mrksSub > 0) {
            mPubLocalHeightMrks.publish(markers);
        }
    }

    void ZEDTerrainMapping::publishGlobalMaps(std::vector<sl::HashKey>& chunks,
            uint32_t heightSub, uint32_t costSub, uint32_t cloudSub, uint32_t mrkSub,
            ros::Time t) {

        float mapWm = mGlobHeightMapMsg.info.width * mGlobHeightMapMsg.info.resolution;
        double mapMinX = mGlobHeightMapMsg.info.origin.position.x;

        float mapHm = mGlobHeightMapMsg.info.height * mGlobHeightMapMsg.info.resolution;
        double mapMinY = mGlobHeightMapMsg.info.origin.position.y;

        uint32_t mapRows = mGlobHeightMapMsg.info.height;
        uint32_t mapCols = mGlobHeightMapMsg.info.width;

        ROS_DEBUG_STREAM("Global map origin: [" << mapMinX << "," << mapMinY << "]");
        ROS_DEBUG_STREAM("Global map dimensions: " << mapWm << " x " << mapHm << " m");
        ROS_DEBUG_STREAM("Global map cell dim: " << mapCols << " x " << mapRows);

        // Height Pointcloud
        mGlobalHeightPointcloudMsg.header.stamp = t;

        // Height Marker
        visualization_msgs::Marker marker;
        if (mrkSub) {
            marker.header.frame_id = mMapFrameId;
            marker.header.stamp = t;
            marker.ns = "height_cubes";
            marker.id = 0;
            marker.type = visualization_msgs::Marker::CUBE_LIST;
            marker.pose.position.x = 0;
            marker.pose.position.y = 0;
            marker.pose.position.z = 0;
            marker.pose.orientation.x = 0.0;
            marker.pose.orientation.y = 0.0;
            marker.pose.orientation.z = 0.0;
            marker.pose.orientation.w = 1.0;
            marker.scale.x = mTerrainMapRes;
            marker.scale.y = mTerrainMapRes;
            marker.scale.z = mMapHeightResol;
            //marker.lifetime = ros::Duration(2.5 / mGlobalTerrainPubRate);
            marker.action = visualization_msgs::Marker::MODIFY;
        }

        // Height Map Message as OccupancyGrid
        mGlobHeightMapMsg.info.map_load_time = t;
        mGlobHeightMapMsg.header.stamp = t;

        // Cost Map Message as OccupancyGrid
        mGlobCostMapMsg.info.map_load_time = t;
        mGlobCostMapMsg.header.stamp = t;

        #pragma omp parallel for
        for (int k = 0; k < chunks.size(); k++) {
            //ROS_DEBUG("*** NEW CHUNK parsing ***");
            sl::HashKey key = chunks.at(k);
            sl::TerrainChunk chunk = mTerrain.getChunk(key);

            sl::Dimension dim = chunk.getDimension();
            unsigned int cellCount = dim.getFullSizeIdx();

            #pragma omp parallel for
            for (unsigned int i = 0; i < cellCount; i++) {

                float height = std::numeric_limits<float>::quiet_NaN();
                int heightNorm = -1, costNorm = -1; // If cell is not valid the current value must be replaced with -1


                if (chunk.isCellValid(i)) { // Leave the value to its default: -1
                    height = chunk.at(sl::ELEVATION, i);
                    heightNorm = static_cast<int8_t>(fabs(round(height / mMapMaxHeight) * 100));
                    costNorm = static_cast<int8_t>(chunk.at(sl::TRAVERSABILITY_COST, i) * 100);

                    if (!isfinite(heightNorm)) {
                        heightNorm = -1;
                        height = std::numeric_limits<float>::quiet_NaN();
                    }

                    if (!isfinite(heightNorm)) {
                        costNorm = -1;
                        height = std::numeric_limits<float>::quiet_NaN();
                    }
                }

                float xm, ym;
                if (dim.index2x_y(i, xm, ym)) {
                    continue; // Index out of range
                }

                // (xm,ym) to ROS map index
                int u = static_cast<uint32_t>(round((ym - mapMinX) / mTerrainMapRes)); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                int v = static_cast<uint32_t>(round((-xm - mapMinY) / mTerrainMapRes)); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL

                int mapIdx = u + v * mapCols;

                if (u < 0 || v < 0 ||
                    u > mGlobHeightMapMsg.info.width ||
                    v > mGlobHeightMapMsg.info.height ||
                    mapIdx < 0 || mapIdx >= mapCols * mapRows) {
                    //ROS_DEBUG_STREAM("[Global map] Cell OUT OF RANGE: [" << u << "," << v << "] -> " << mapIdx << "[max: " << mapCols * mapRows << "]");
                    //ROS_DEBUG_STREAM("ym: " << ym << " - xm: " << xm);
                    continue;
                }

                //ROS_DEBUG_STREAM("Cell: [" << u << "," << v << "] -> " << mapIdx);
                mGlobHeightMapMsg.data.at(mapIdx) = heightNorm;
                mGlobCostMapMsg.data.at(mapIdx) = costNorm;

                if (cloudSub > 0 || mrkSub > 0) {
                    float color_f = static_cast<float>(chunk.at(sl::COLOR, i));

                    //PointCloud
                    if (cloudSub > 0 || mrkSub > 0) {
                        float* ptCloudPtr = (float*)(&mGlobalHeightPointcloudMsg.data[0]);
                        ptCloudPtr[mapIdx * 4 + 0] = ym + (mTerrainMapRes / 2);   // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                        ptCloudPtr[mapIdx * 4 + 1] = -xm  + (mTerrainMapRes / 2); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                        ptCloudPtr[mapIdx * 4 + 2] = height;
                        ptCloudPtr[mapIdx * 4 + 3] = color_f;
                    }
                }
            }
        }

        // Cube List Update
        // Note: the cube list cannot be taken all in memory because it's dimension varies in Z direction at each step
        //       we must reconstruct it completely starting from the Height Point Cloud
        if (mrkSub > 0) {
            size_t ptCount = mGlobalHeightPointcloudMsg.data.size() / (4 * sizeof(float));
            float* ptCloudPtr = (float*)(&mGlobalHeightPointcloudMsg.data[0]);

            #pragma omp parallel for
            for (int p = 0; p < ptCount; p++) {
                // Current point
                float xm = ptCloudPtr[p * 4 + 0];
                float ym = ptCloudPtr[p * 4 + 1];
                float zm = ptCloudPtr[p * 4 + 2];
                float color_f = ptCloudPtr[p * 4 + 3];
                sl::float3 color = sl_tools::depackColor3f(color_f);

                // Number of vertical cubes for the current point
                int col_count = static_cast<int>(ceil(fabs(zm) / mMapHeightResol));

                #pragma omp critical
                {
                    #pragma omp parallel for
                    for (int i = 1; i <= col_count; i++) {
                        geometry_msgs::Point pt;
                        pt.x = xm + (mTerrainMapRes / 2);
                        pt.y = ym  + (mTerrainMapRes / 2);
                        pt.z = i * mMapHeightResol;

                        if (zm < 0) {
                            pt.z *= - 1.0f;
                        }

                        //ROS_INFO("Height: %g -> Squares: %d -> i: %d -> Current: %g", height, col_count, i, pt.z);

                        std_msgs::ColorRGBA col;
                        col.a = 1.0f;
                        col.r = color[0];
                        col.g = color[1];
                        col.b = color[2];

                        marker.points.push_back(pt);
                        marker.colors.push_back(col);
                    }
                }
            }
        }

        // Map publishing
        if (heightSub > 0) {
            mPubGlobalHeightMap.publish(mGlobHeightMapMsg);
        }

        if (costSub > 0) {
            mPubGlobalCostMap.publish(mGlobCostMapMsg);
        }

        if (cloudSub > 0) {
            mPubGlobalHeightCloud.publish(mGlobalHeightPointcloudMsg);
        }

        if (mrkSub > 0) {
            mPubGlobalHeightMrk.publish(marker);
        }

    }

    void ZEDTerrainMapping::globalTerrainCallback(const ros::TimerEvent& e) {
        if (!mMappingReady) {
            mMappingReady = startTerrainMapping();
        }

        mMappingReady = true;

        // Timer synchronization with Local mapping
        sl::ERROR_CODE res;
        do {
            mTerrainMutex.lock();
            res = mZed->getTerrainRequestStatusAsync();
            if (res != sl::SUCCESS) {
                mZed->requestTerrainAsync(); // if an elaboration is in progress the request is ignored
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                mTerrainMutex.unlock();
            }
        } while (res != sl::SUCCESS);

        uint32_t heightImgSub = mPubGlobalHeightMapImg.getNumSubscribers();
        uint32_t colorImgSub = mPubGlobalColorMapImg.getNumSubscribers();
        uint32_t costImgSub = mPubGlobalCostMapImg.getNumSubscribers();

        uint32_t heightMapSub = mPubGlobalHeightMap.getNumSubscribers();
        uint32_t costMapSub = mPubGlobalCostMap.getNumSubscribers();
        uint32_t cloudSub = mPubGlobalHeightCloud.getNumSubscribers();
        uint32_t mrkSub = mPubGlobalHeightMrk.getNumSubscribers();

        uint32_t run = /*gridSub + */heightImgSub + colorImgSub + costImgSub + heightMapSub + costMapSub + cloudSub + mrkSub;

        if (run > 0) {
            sl::Mat sl_heightMap, sl_colorMap, sl_traversMap;
            cv::Mat cv_heightMap, cv_colorMap, cv_traversMap;

            if (mZed->retrieveTerrainAsync(mTerrain) == sl::SUCCESS) {

                ROS_DEBUG("Global Terrain available");

                // Request New Terrain calculation while elaborating data
                mZed->requestTerrainAsync();
                mTerrainMutex.unlock();

                // Chunks list
                std::vector<sl::HashKey> chunks;

                if (mGlobMapWholeUpdate) {
                    chunks = mTerrain.getAllValidChunk();
                    mLastGlobMapTimestamp = mTerrain.getReferenceTS();
                    ROS_DEBUG("*************** ALL CHUNKS ***************");
                    mGlobMapWholeUpdate = false;
                } else {
                    chunks = mTerrain.getUpdatedChunks(mLastGlobMapTimestamp);
                    mLastGlobMapTimestamp = mTerrain.getReferenceTS();

                    ROS_DEBUG("+++++++++++++++ UPDATED CHUNKS +++++++++++++++");
                }

                ROS_DEBUG_STREAM("Terrain chunks (global map): " << chunks.size());

                if (chunks.size() > 0) {
                    // Get chunks map limits
                    double mapWm = mGlobHeightMapMsg.info.width * mGlobHeightMapMsg.info.resolution;
                    double mapMinX = mGlobHeightMapMsg.info.origin.position.x;
                    double mapMaxX = mapMinX + mapWm;

                    double mapHm = mGlobHeightMapMsg.info.height * mGlobHeightMapMsg.info.resolution;
                    double mapMinY = mGlobHeightMapMsg.info.origin.position.y;
                    double mapMaxY = mapMinY + mapHm;

                    std::vector<sl::HashKey>::iterator it;

                    float minX = -(mapMaxY), minY = mapMinX, maxX = -(mapMinY), maxY = mapMaxX; // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL

                    bool doResize = false;

                    // Chunk list limits
                    for (it = chunks.begin(); it != chunks.end(); it++) {
                        sl::HashKey key = *it;
                        sl::TerrainChunk& chunk = mTerrain.getChunk(key);
                        sl::Dimension dim = chunk.getDimension();

                        if (dim.getXmin() < minX) {
                            minX = dim.getXmin();
                            doResize = true;
                        }

                        if (dim.getYmin() < minY) {
                            minY = dim.getYmin();
                            doResize = true;
                        }

                        if (dim.getXmax() > maxX) {
                            maxX = dim.getXmax();
                            doResize = true;
                        }

                        if (dim.getYmax() > maxY) {
                            maxY = dim.getYmax();
                            doResize = true;
                        }
                    }

                    mGlobMapMutex.lock();

                    // Check if the map must be resized
                    if (doResize) {    // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL
                        float width = maxX - minX;
                        float height = maxY - minY;

                        initGlobalMapMsgs(height, width); // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL

                        // Map as been reinitialized to -1. We need all chunksm not only the updated
                        chunks = mTerrain.getAllValidChunk();
                        mGlobMapWholeUpdate = false;

                        mGlobHeightMapMsg.info.origin.position.x = /*mInitialPoseSl.getTranslation().x + */ minY; // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL - // TODO uncomment and test when the bug in SDK is fixed
                        mGlobHeightMapMsg.info.origin.position.y = /*mInitialPoseSl.getTranslation().y + */ -maxX; // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL - // TODO uncomment and test when the bug in SDK is fixed
                        mGlobCostMapMsg.info.origin.position.x = /*mInitialPoseSl.getTranslation().x + */   minY; // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL - // TODO uncomment and test when the bug in SDK is fixed
                        mGlobCostMapMsg.info.origin.position.y = /*mInitialPoseSl.getTranslation().y + */  -maxX; // REMEMBER X & Y ARE SWITCHED AT SDK LEVEL - // TODO uncomment and test when the bug in SDK is fixed

                        ROS_DEBUG("****************************************************************************************************************");
                    }

                    // Publish global map
                    publishGlobalMaps(chunks, heightMapSub, costMapSub, cloudSub, mrkSub, sl_tools::slTime2Ros(mLastGlobMapTimestamp));

                    mGlobMapMutex.unlock();
                } else {
                    ROS_DEBUG("Global map not available");
                    return;
                }

                // Height Map Image
                if (heightImgSub > 0 /*|| gridSub > 0*/) {
                    sl::float2 origin;
                    mTerrain.generateTerrainMap(sl_heightMap, origin, sl::MAT_TYPE_32F_C1, sl::LayerName::ELEVATION);

                    if (sl_heightMap.getResolution().area() > 0) {
                        cv_heightMap = sl_tools::toCVMat(sl_heightMap);
                        mPubGlobalHeightMapImg.publish(
                            sl_tools::imageToROSmsg(cv_heightMap, sensor_msgs::image_encodings::TYPE_32FC1,
                                                    mMapFrameId, sl_tools::slTime2Ros(mLastGlobMapTimestamp)));
                    }
                }

                // Color Map Image
                if (colorImgSub > 0 /*|| gridSub > 0*/) {
                    sl::float2 origin;
                    mTerrain.generateTerrainMap(sl_colorMap, origin, sl::MAT_TYPE_8U_C4, sl::LayerName::COLOR);

                    if (sl_colorMap.getResolution().area() > 0) {
                        cv_colorMap = sl_tools::toCVMat(sl_colorMap);
                        mPubGlobalColorMapImg.publish(sl_tools::imageToROSmsg(
                                                          cv_colorMap, sensor_msgs::image_encodings::TYPE_8UC4,
                                                          mMapFrameId, sl_tools::slTime2Ros(mLastGlobMapTimestamp)));
                    }
                }

                // Traversability Map Image
                if (costImgSub > 0 /*|| gridSub > 0*/) {
                    sl::float2 origin;
                    mTerrain.generateTerrainMap(sl_traversMap, origin, sl::MAT_TYPE_16U_C1, sl::LayerName::TRAVERSABILITY_COST);

                    if (sl_traversMap.getResolution().area() > 0) {
                        cv_traversMap = sl_tools::toCVMat(sl_traversMap);
                        mPubGlobalCostMapImg.publish(sl_tools::imageToROSmsg(
                                                         cv_traversMap, sensor_msgs::image_encodings::TYPE_16UC1,
                                                         mMapFrameId, sl_tools::slTime2Ros(mLastGlobMapTimestamp)));
                    }
                }

                mTerrainMutex.unlock();
                ROS_DEBUG_STREAM("Local terrain not available");
            }
        } else {
            mTerrainMutex.unlock();
        }
    }

    void ZEDTerrainMapping::initGlobalMapMsgs(double map_W_m, double map_H_m) {
        // MetaData
        nav_msgs::MapMetaData mapInfo;
        mapInfo.resolution = mTerrainMapRes;

        uint32_t mapRows = static_cast<uint32_t>(ceil(map_H_m / mTerrainMapRes)) + 1;
        uint32_t mapCols = static_cast<uint32_t>(ceil(map_W_m / mTerrainMapRes)) + 1;

        mapInfo.resolution = mTerrainMapRes;
        mapInfo.height = mapRows;
        mapInfo.width = mapCols;
        mapInfo.origin.position.x = /*mInitialPoseSl.getTranslation().x + */ - (map_W_m / 2.0); // TODO uncomment and test when the bug in SDK is fixed
        mapInfo.origin.position.y = /*mInitialPoseSl.getTranslation().y + */ - (map_H_m / 2.0); // TODO uncomment and test when the bug in SDK is fixed
        mapInfo.origin.position.z = 0.0;
        mapInfo.origin.orientation.x = 0.0;
        mapInfo.origin.orientation.y = 0.0;
        mapInfo.origin.orientation.z = 0.0;
        mapInfo.origin.orientation.w = 1.0;

        // Maps
        mGlobHeightMapMsg.header.frame_id = mMapFrameId;
        mGlobCostMapMsg.header.frame_id = mMapFrameId;
        mGlobHeightMapMsg.info = mapInfo;
        mGlobCostMapMsg.info = mapInfo;

        mGlobHeightMapMsg.data = std::vector<int8_t>(mapRows * mapCols, -1);
        mGlobCostMapMsg.data = std::vector<int8_t>(mapRows * mapCols, -1);
        mGlobMapWholeUpdate = true;

        ROS_DEBUG_STREAM("Initialized Global map dimensions: " << map_W_m << " x " << map_H_m << " m");
        ROS_DEBUG_STREAM("Initialized Global map cell dim: " << mapInfo.width << " x " << mapInfo.height);

        // Height Pointcloud
        if (mGlobalHeightPointcloudMsg.width != mapCols || mGlobalHeightPointcloudMsg.height != mapRows) {
            mGlobalHeightPointcloudMsg.header.frame_id = mMapFrameId; // Set the header values of the ROS message
            mGlobalHeightPointcloudMsg.is_bigendian = false;
            mGlobalHeightPointcloudMsg.is_dense = false;

            sensor_msgs::PointCloud2Modifier modifier(mGlobalHeightPointcloudMsg);
            modifier.setPointCloud2Fields(4,
                                          "x", 1, sensor_msgs::PointField::FLOAT32,
                                          "y", 1, sensor_msgs::PointField::FLOAT32,
                                          "z", 1, sensor_msgs::PointField::FLOAT32,
                                          "rgb", 1, sensor_msgs::PointField::FLOAT32);

            modifier.resize(mapRows * mapCols);
        }
    }

    bool ZEDTerrainMapping::on_get_static_map(nav_msgs::GetMap::Request&  req,
            nav_msgs::GetMap::Response& res) {

        if (!mMappingReady) {
            return false;
        }

        mGlobMapMutex.lock();

        if (mDefaultMap == 0) {
            res.map = mGlobHeightMapMsg;
        } else {
            res.map = mGlobCostMapMsg;
        }

        mGlobMapMutex.unlock();

        return true;
    }

    bool ZEDTerrainMapping::on_get_loc_height_map(nav_msgs::GetMap::Request&  req,
            nav_msgs::GetMap::Response& res) {

        if (!mMappingReady) {
            return false;
        }

        mLocMapMutex.lock();

        res.map = mLocHeightMapMsg;

        mLocMapMutex.unlock();

        return true;
    }

    bool ZEDTerrainMapping::on_get_loc_cost_map(nav_msgs::GetMap::Request&  req,
            nav_msgs::GetMap::Response& res) {

        if (!mMappingReady) {
            return false;
        }

        mLocMapMutex.lock();

        res.map = mLocCostMapMsg;

        mLocMapMutex.unlock();

        return true;
    }

    bool ZEDTerrainMapping::on_get_glob_height_map(nav_msgs::GetMap::Request&  req,
            nav_msgs::GetMap::Response& res) {

        if (!mMappingReady) {
            return false;
        }

        mGlobMapMutex.lock();

        res.map = mGlobHeightMapMsg;

        mGlobMapMutex.unlock();

        return true;
    }

    bool ZEDTerrainMapping::on_get_glob_cost_map(nav_msgs::GetMap::Request&  req,
            nav_msgs::GetMap::Response& res) {

        if (!mMappingReady) {
            return false;
        }

        mGlobMapMutex.lock();

        res.map = mGlobCostMapMsg;

        mGlobMapMutex.unlock();

        return true;
    }

    void ZEDTerrainMapping::globalMapSubscribeCallback(const ros::SingleSubscriberPublisher& pub) {
        uint32_t heightMapSub = mPubGlobalHeightMap.getNumSubscribers();
        uint32_t costMapSub = mPubGlobalCostMap.getNumSubscribers();
        uint32_t cloudSub = mPubGlobalHeightCloud.getNumSubscribers();
        uint32_t mrkSub = mPubGlobalHeightMrk.getNumSubscribers();
        if (heightMapSub == 1 || costMapSub == 1 || cloudSub == 1 || mrkSub == 1) {
            mGlobMapWholeUpdate = true;
        }

        ROS_DEBUG_STREAM("New global map subscription by " << pub.getSubscriberName() << " to topic " << pub.getTopic());
    }


}

#endif // TERRAIN_MAPPING