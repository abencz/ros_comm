/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2008, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
********************************************************************/
#ifndef ROSBAG_SNAPSHOTER_H
#define ROSBAG_SNAPSHOTER_H

#include <queue>
#include <map>
#include <string>
#include <boost/atomic.hpp>
#include <boost/thread/mutex.hpp>
#include <ros/ros.h>
#include <ros/time.h>
#include <rosbag/TriggerSnapshot.h>
#include <std_srvs/SetBool.h>
#include <topic_tools/shape_shifter.h>
#include "rosbag/bag.h"
#include "rosbag/macros.h"

namespace rosbag {
class ROSBAG_DECL Snapshoter;

/* Configuration for a single topic in the Snapshoter node. Holds
 * the buffer limits for a topic by duration (time difference between newest and oldest message)
 * and memory usage, in bytes.
 */
struct ROSBAG_DECL SnapshoterTopicOptions
{
    // When the value of duration_limit_, do not truncate the buffer no matter how large the duration is
    static const ros::Duration NO_DURATION_LIMIT;
    // When the value of memory_limit_, do not trunctate the buffer no matter how much memory it consumes (DANGROUS)
    static const int32_t NO_MEMORY_LIMIT;
    // When the value of duration_limit_, inherit the limit from the node's configured default
    static const ros::Duration INHERIT_DURATION_LIMIT;
    // When the value of memory_limit_, inherit the limit from the node's configured default
    static const int32_t INHERIT_MEMORY_LIMIT;

    // Maximum difference in time from newest and oldest message in buffer before older messages are removed
    ros::Duration  duration_limit_;
    // Maximum memory usage of the buffer before older messages ar eremoved
    int32_t       memory_limit_;

    SnapshoterTopicOptions(ros::Duration duration_limit=INHERIT_DURATION_LIMIT,
                           int32_t memory_limit=INHERIT_MEMORY_LIMIT);
};


/* Configuration for the Snapshoter node. Contains default limits for memory and duration
 * and a map of topics to their limits which may override the defaults.
 */
struct ROSBAG_DECL SnapshoterOptions
{
    // Duration limit to use for a topic's buffer if one is not specified
    ros::Duration   default_duration_limit_;
    // Memory limit to use for a topic's buffer if one is not specified
    int32_t        default_memory_limit_;

    typedef std::map<std::string, SnapshoterTopicOptions> topics_t;
    // Provides list of topics to snapshot and their limit configurations
    topics_t topics_;

    SnapshoterOptions(ros::Duration default_duration_limit = ros::Duration(), int32_t default_memory_limit=0);

    // Add a new topic to the configuration
    void addTopic(std::string const& topic,
                  ros::Duration duration_limit=SnapshoterTopicOptions::INHERIT_DURATION_LIMIT,
                  int32_t memory_limit=SnapshoterTopicOptions::INHERIT_MEMORY_LIMIT);
};


/* Stores a buffered message of an ambiguous type and it's associated metadata (time of arrival, connection data),
 * for later writing to disk
 */
struct ROSBAG_DECL SnapshotMessage
{
    SnapshotMessage(topic_tools::ShapeShifter::ConstPtr _msg,
                    boost::shared_ptr<ros::M_string> _connection_header, ros::Time _time);
    topic_tools::ShapeShifter::ConstPtr msg;
    boost::shared_ptr<ros::M_string>    connection_header;
    // ROS time when messaged arrived (does not use header stamp)
    ros::Time                           time;
};


/* Stores a queue of buffered messages for a single topic ensuring
 * that the duration and memory limits are respected by truncating
 * as needed on push() operations.
 */
class ROSBAG_DECL MessageQueue
{
friend Snapshoter;
public:
    MessageQueue(SnapshoterTopicOptions const& options);
    // Add a new message to the internal queue if possible, truncating the front of the queue as needed to enforce limits
    void push(SnapshotMessage const& msg);
    // Removes the message at the front of the queue (oldest) and returns it
    SnapshotMessage pop();
    // Returns the time difference between back and front of queue, or 0 if size <= 1
    ros::Duration duration() const;
    // Store the subscriber for this topic's queue internaly so it is not deleted
    void setSubscriber(boost::shared_ptr<ros::Subscriber> sub);
private:
    boost::mutex lock;
    SnapshoterTopicOptions options_;
    // Internal push whitch does not obtain lock
    void _push(SnapshotMessage const& msg);
    // Internal pop whitch does not obtain lock
    SnapshotMessage _pop();
    // Current total size of the queue
    int64_t size_;
    std::queue<SnapshotMessage> queue_;
    // Subscriber to the callback which uses this queue
    boost::shared_ptr<ros::Subscriber> sub_;
    // Truncate front of queue as needed to fit a new message of specified size and time. Returns False if this is impossible.
    bool prepare_push(int32_t size, ros::Time const& time);
};


/* Snapshoter node. Maintains a circular buffer of the most recent messages from configured topics
 * while enforcing limits on memory and duration. The node can be triggered to write some or all
 * of these buffers to a bag file via an action goal. Useful in live testing scenerios where interesting
 * data may be produced before a user has the oppurtunity to "rosbag record" the data.
 */
class ROSBAG_DECL Snapshoter
{
public:
    Snapshoter(SnapshoterOptions const& options);
    // Sets up callbacks and spins until node is killed
    int run();
private:
    // Subscribe queue size for each topic
    static const int QUEUE_SIZE;
    SnapshoterOptions options_;
    typedef std::map<std::string, boost::shared_ptr<MessageQueue> > buffers_t;
    buffers_t buffers_;
    // Locks recording_ and writing_ states.
    boost::upgrade_mutex state_lock_;
    // True if new messages are being written to the internal buffer
    bool recording_;
    // True if currently writing buffers to a bag file
    bool writing_;
    ros::NodeHandle nh_;
    ros::ServiceServer trigger_snapshot_server_;
    ros::ServiceServer enable_server_;

    // Replace individual topic limits with node defaults if they are flagged for it (see SnapshoterTopicOptions)
    void fixTopicOptions(SnapshoterTopicOptions &options);
    // Clean a requested filename, ensuring .bag is at the end and appending the date TODO make clearer, mention return bool
    bool postfixFilename(std::string& file);
    // TODO 
    std::string timeAsStr();
    // Subscribe to one of the topics, setting up the callback to add to the respective queue
    void subscribe(std::string const& topic, boost::shared_ptr<MessageQueue> queue);
    // Called on new message from any configured topic. Adds to queue for that topic
    void topicCB(const ros::MessageEvent<topic_tools::ShapeShifter const>& msg_event, boost::shared_ptr<MessageQueue> queue);
    // Service callback, write all of part of the internal buffers to a bag file according to request parameters
    bool triggerSnapshotCb(rosbag::TriggerSnapshot::Request &req, rosbag::TriggerSnapshot::Response& res);
    // Service callback, enable or disable recording (storing new messages into queue). Used to pause before writing
    bool recordCb(std_srvs::SetBool::Request& req, std_srvs::SetBool::Response& res);
};

} // namespace rosbag

#endif
