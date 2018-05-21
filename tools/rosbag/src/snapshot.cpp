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

#include "rosbag/snapshoter.h"
#include "rosbag/exceptions.h"

#include "boost/program_options.hpp"
#include <string>
#include <sstream>

namespace po = boost::program_options;

using rosbag::Snapshoter;
using rosbag::SnapshoterClient;
using rosbag::SnapshoterOptions;
using rosbag::SnapshoterTopicOptions;
using rosbag::SnapshoterClientOptions;


bool parseOptions(po::variables_map& vm, int argc, char** argv)
{
    po::options_description desc("Options");
    desc.add_options()
        ("help,h", "produce help message")
        ("trigger-write,t", "Write buffer of selected topcis to a bag file")
        ("pause,p", "Stop buffering new messages until resumed of write is triggered")
        ("resume,r", "Resume buffering new messages, writing over older messages as needed")
        ("size,s", po::value<double>()->default_value(-1), "Maximum memory per topic to use in buffering in MB. Default: no limit")
        ("duration,d", po::value<double>()->default_value(30.0), "Maximum difference between newest and oldest buffered message per topic in seconds. Default: 30")
        ("filename,o", po::value<std::string>()->default_value(""), "Name of output file when triggering a write. If it does NOT end in .bag, the current date/time and .bag will be appended.")
        ("topic", po::value<std::vector<std::string> >(), "Topic to buffer. If triggering write, write only these topics instead of all buffered topics.");
    po::positional_options_description p;
    p.add("topic", -1);

    try
    {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
        po::notify(vm); 
    } catch (boost::program_options::error const& e)
    {
        std::cout << "rosbag snapshot: " << e.what() << std::endl;
        return false;
    }

    if (vm.count("help"))
    {
        std::cout << "Usage: rosbag snapshot [options] [topic1 topic2 ...]" << std::endl << std::endl <<
                     "Buffer recent messages until triggered to write or trigger an already running instance."  << std::endl << std::endl;
        std::cout << desc << std::endl;
        return false;
    }
    return true;
}

bool parseVariablesMap(SnapshoterOptions& opts, po::variables_map const& vm)
{
    if (vm.count("topic"))
    {
        std::vector<std::string> topics = vm["topic"].as<std::vector<std::string> >();
        BOOST_FOREACH(std::string& str, topics)
            opts.addTopic(str);
    }
    opts.default_memory_limit_ = int(1E6 * vm["size"].as<double>());
    opts.default_duration_limit_ = ros::Duration(vm["duration"].as<double>());
    return true;
}

bool parseVariablesMapClient(SnapshoterClientOptions& opts, po::variables_map const& vm)
{
    if(vm.count("pause")) opts.action_ = SnapshoterClientOptions::PAUSE;
    else if(vm.count("resume")) opts.action_ = SnapshoterClientOptions::RESUME;
    else if(vm.count("trigger-write")) 
    {
        opts.action_ = SnapshoterClientOptions::TRIGGER_WRITE;
        if (vm.count("topic"))
            opts.topics_ = vm["topic"].as<std::vector<std::string> >();
        if (vm.count("filename"))
            opts.filename_ = vm["filename"].as<std::string>();
    }
    return true;
}

/* Read configured topics from by reading ~topics ROS param.
 * TODO: use exceptions instead of asserts to follow style conventions
 * This param should be set in the following (YAML represented) structure
 *   <rosparam>
 *       topics:                   # List of topics
 *           - /topic1             # Topic which will adopt default memory and duration limits
 *           - topic2:             # Topic with overriden memory and duration limit
 *               memory: 5000      # 5000 Byte limit on buffered data from this topic
 *               duration: 30      # 30 second duration limit between newest and oldest message from this topic
 *   </rosparam>
 */
void appendParamOptions(SnapshoterOptions& opts)
{
    using XmlRpc::XmlRpcValue;
    XmlRpcValue topics;
    if(!ros::param::get("~topics", topics))
    {
        return;
    }
    ROS_ASSERT_MSG(topics.getType() == XmlRpcValue::TypeArray, "topics param must be an array");
    // Iterator caused exception, hmmm...
    size_t size = topics.size();
    for (size_t i = 0; i < size; ++i)
    {
        XmlRpcValue topic_value = topics[i];
        // If it is just a string, add this topic
        if (topic_value.getType() == XmlRpcValue::TypeString)
        {
            opts.addTopic(topic_value);
        }
        else if (topic_value.getType() == XmlRpcValue::TypeStruct)
        {
            ROS_ASSERT_MSG(topic_value.size() == 1, "Paramater invalid for topic %lu", i);
            std::string const& topic = (*topic_value.begin()).first;
            XmlRpcValue& topic_config = (*topic_value.begin()).second;
            ROS_ASSERT_MSG(topic_config.getType() == XmlRpcValue::TypeStruct, "Topic limits invalid for: '%s'", topic.c_str());

            ros::Duration dur = SnapshoterTopicOptions::INHERIT_DURATION_LIMIT;
            int64_t mem = SnapshoterTopicOptions::INHERIT_MEMORY_LIMIT;
            std::string duration = "duration";
            std::string memory = "memory";
            if (topic_config.hasMember(duration))
            {
                XmlRpcValue& dur_limit = topic_config[duration];
                if (dur_limit.getType() == XmlRpcValue::TypeDouble)
                {
                    double seconds = dur_limit;
                    dur = ros::Duration(seconds);
                }
                else if (dur_limit.getType() == XmlRpcValue::TypeInt)
                {
                    int seconds = dur_limit;
                    dur = ros::Duration(seconds, 0);
                }
                else ROS_FATAL("err");
            }
            if (topic_config.hasMember("memory"))
            {
                XmlRpcValue& mem_limit = topic_config[memory];
                ROS_ASSERT_MSG(mem_limit.getType() == XmlRpcValue::TypeInt, "Memory limit is not an int for topic '%s'", topic.c_str());
                int tmp = mem_limit;
                mem = tmp;
            }
            opts.addTopic(topic, dur, mem);
        }
        else ROS_ASSERT_MSG(false, "Parameter invalid for topic %lu", i);
    }
}

int main(int argc, char** argv) {
    // TODO: strip ros arguments from argv so remaps can work
    po::variables_map vm;
    if (!parseOptions(vm, argc, argv)) return 1;

    // If any of the client flags are on, use the client
    if (vm.count("trigger-write") || vm.count("pause") || vm.count("resume"))
    {
        SnapshoterClientOptions opts;
        if(!parseVariablesMapClient(opts, vm)) return 1;
        ros::init(argc, argv, "snapshot_client", ros::init_options::AnonymousName);
        SnapshoterClient client;
        return client.run(opts);
    }


    // Parse the command-line options
    SnapshoterOptions opts;
    if (!parseVariablesMap(opts, vm)) return 1; 


    ros::init(argc, argv, "snapshot", ros::init_options::AnonymousName);
    // Get additional topic configurations if they're in ROS params
    appendParamOptions(opts);

    // Exit if not topics selected
    if (!opts.topics_.size()) 
    {
        ROS_FATAL("No topics selected. Exiting.");
        return 1;
    }

    // Run the snapshoter
    rosbag::Snapshoter snapshoter(opts);
    int result = snapshoter.run();
    return result;
}
