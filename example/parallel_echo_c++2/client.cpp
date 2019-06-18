// Copyright (c) 2014 Baidu, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// A client sending requests to server in parallel by multiple threads.

#include <gflags/gflags.h>
#include <bthread/bthread.h>
#include <butil/logging.h>
#include <butil/string_printf.h>
#include <butil/time.h>
#include <butil/macros.h>
#include <brpc/parallel_channel.h>
#include <brpc/server.h>
#include <chrono>
#include "echo.pb.h"

DEFINE_int32(thread_num, 1, "Number of threads to send requests");
DEFINE_int32(channel_num, 3, "Number of sub channels");
DEFINE_bool(same_channel, false, "Add the same sub channel multiple times");
DEFINE_bool(use_bthread, false, "Use bthread to send requests");
DEFINE_int32(attachment_size, 0, "Carry so many byte attachment along with requests");
DEFINE_int32(request_size, 16, "Bytes of each request");
DEFINE_string(connection_type, "", "Connection type. Available values: single, pooled, short");
DEFINE_string(protocol, "baidu_std", "Protocol type. Defined in src/brpc/options.proto");
DEFINE_string(server, "0.0.0.0:8002", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 10000, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)"); 
DEFINE_bool(dont_fail, false, "Print fatal when some call failed");
DEFINE_int32(dummy_port, -1, "Launch dummy server at this port");

std::string g_request;
std::string g_attachment;
bvar::LatencyRecorder g_latency_recorder("client");
bvar::Adder<int> g_error_count("client_error_count");
bvar::LatencyRecorder* g_sub_channel_latency = NULL;

class GetReqAndAddRes: public brpc::CallMapper {
    brpc::SubCall Map(int channel_index,
            const google::protobuf::MethodDescriptor* method,
            const google::protobuf::Message* req_base,
            google::protobuf::Message* res_base) {
        const example::ComboRequest* req =
                dynamic_cast<const example::ComboRequest*>(req_base);
        example::ComboResponse* res = dynamic_cast<example::ComboResponse*>(res_base);
        if (method->name() != "ComboEcho" || res == NULL || req == NULL
                || req->requests_size() <= channel_index) {
            return brpc::SubCall::Bad();
        }
        return brpc::SubCall(::example::EchoService::descriptor()->method(0),
                &req->requests(channel_index), res->add_responses(), 0);
    }
};

class MergeNothing: public brpc::ResponseMerger {
    Result Merge(google::protobuf::Message* /*response*/,
            const google::protobuf::Message* /*sub_response*/) {
        return brpc::ResponseMerger::MERGED;
    }
};

void CallMethod(brpc::ChannelBase* channel,
                brpc::Controller* cntl,
                example::ComboRequest* req, example::ComboResponse* res,
                bool async, bool destroy = false) {
    google::protobuf::Closure* done = NULL;
    brpc::CallId sync_id = { 0 };
    if (async) {
        sync_id = cntl->call_id();
        done = brpc::DoNothing();
    }
    ::example::EchoService::Stub(channel).ComboEcho(cntl, req, res, done);
    if (async) {
        if (destroy) {
            delete channel;
        }
        // Callback MUST be called for once and only once
        bthread_id_join(sync_id);
    }
}

static void* sender(void* arg) {
    // Normally, you should not call a Channel directly, but instead construct
    // a stub Service wrapping it. stub can be shared by all threads as well.
    brpc::ChannelBase* channel = static_cast<brpc::ChannelBase*>(arg);

    int log_id = 0;
    int count = 4;
    do {
        count--;
        std::chrono::system_clock::time_point start = std::chrono::system_clock::now();

        brpc::Controller cntl;
        example::ComboRequest req;
        example::ComboResponse res;

        //  init request
        for (size_t i = 0; i < FLAGS_channel_num; ++i) {
            ::example::EchoRequest* sub_req = req.add_requests();
            sub_req->set_value(i + 1);
        }


        CallMethod(channel, &cntl, &req, &res, false, false);


        if (!cntl.Failed()) {
            LOG(ERROR) << "send success, latency=" << cntl.latency_us();

            LOG(ERROR) << "response size =" << res.responses_size();
            for (int i = 0; i < res.responses_size(); ++i) {
                LOG(ERROR) << "            response[" << i << "] = " << res.responses(i).value();
            }



            for (int i = 0; i < cntl.sub_count(); ++i) {
                if (cntl.sub(i) && !cntl.sub(i)->Failed()) {
                    LOG(ERROR) << "       subChannel[" << i << "] latency = " <<
                            cntl.sub(i)->latency_us();
                }
            }
        } else {
            LOG(ERROR) << "curl failed, errCode=" << cntl.ErrorCode();
        }
        std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        LOG(ERROR) << "zml: latency = " << (elapsed).count();
    } while(count > 0);
    return NULL;
}

int main(int argc, char* argv[]) {
    // Parse gflags. We recommend you to use gflags as well.
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);

    // A Channel represents a communication line to a Server. Notice that 
    // Channel is thread-safe and can be shared by all threads in your program.
    brpc::ParallelChannel channel;
    brpc::ParallelChannelOptions pchan_options;
    pchan_options.timeout_ms = FLAGS_timeout_ms;
    if (channel.Init(&pchan_options) != 0) {
        LOG(ERROR) << "Fail to init ParallelChannel";
        return -1;
    }

    brpc::ChannelOptions sub_options;
    sub_options.protocol = FLAGS_protocol;
    sub_options.connection_type = FLAGS_connection_type;
    sub_options.max_retry = FLAGS_max_retry;
    // Setting sub_options.timeout_ms does not work because timeout of sub 
    // channels are disabled in ParallelChannel.

    if (FLAGS_same_channel) {
        // For brpc >= 1.0.155.31351, a sub channel can be added into
        // a ParallelChannel more than once.
        brpc::Channel* sub_channel = new brpc::Channel;
        // Initialize the channel, NULL means using default options. 
        // options, see `brpc/channel.h'.
        if (sub_channel->Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &sub_options) != 0) {
            LOG(ERROR) << "Fail to initialize sub_channel";
            return -1;
        }
        for (int i = 0; i < FLAGS_channel_num; ++i) {
            if (channel.AddChannel(sub_channel, brpc::OWNS_CHANNEL,
                                   NULL, NULL) != 0) {
                LOG(ERROR) << "Fail to AddChannel, i=" << i;
                return -1;
            }
        }
    } else {
        std::string ip("0.0.0.0");
        int port = 8002;
        for (int i = 0; i < FLAGS_channel_num; ++i) {
            brpc::Channel* sub_channel = new brpc::Channel;
            // Initialize the channel, NULL means using default options. 
            // options, see `brpc/channel.h'.
            std::string server = ip + ":" + std::to_string(port + i);
            if (sub_channel->Init(server.c_str(), FLAGS_load_balancer.c_str(), &sub_options) != 0) {
                LOG(ERROR) << "Fail to initialize sub_channel[" << i << "]";
                return -1;
            }
            if (channel.AddChannel(sub_channel, brpc::OWNS_CHANNEL,
                    new GetReqAndAddRes, new MergeNothing) != 0) {
                LOG(ERROR) << "Fail to AddChannel, i=" << i;
                return -1;
            }
        }
    }

    // Initialize bvar for sub channel
    g_sub_channel_latency = new bvar::LatencyRecorder[FLAGS_channel_num];
    for (int i = 0; i < FLAGS_channel_num; ++i) {
        std::string name;
        butil::string_printf(&name, "client_sub_%d", i);
        g_sub_channel_latency[i].expose(name);
    }

    if (FLAGS_attachment_size > 0) {
        g_attachment.resize(FLAGS_attachment_size, 'a');
    }
    if (FLAGS_request_size <= 0) {
        LOG(ERROR) << "Bad request_size=" << FLAGS_request_size;
        return -1;
    }
    g_request.resize(FLAGS_request_size, 'r');

    if (FLAGS_dummy_port >= 0) {
        brpc::StartDummyServerAt(FLAGS_dummy_port);
    }

    std::vector<bthread_t> bids;
    std::vector<pthread_t> pids;
    if (!FLAGS_use_bthread) {
        pids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&pids[i], NULL, sender, &channel) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        bids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(
                    &bids[i], NULL, sender, &channel) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }

    /*
    while (!brpc::IsAskedToQuit()) {
        sleep(1);
        LOG(INFO) << "Sending EchoRequest at qps=" << g_latency_recorder.qps(1)
                  << " latency=" << g_latency_recorder.latency(1) << noflush;
        for (int i = 0; i < FLAGS_channel_num; ++i) {
            LOG(INFO) << " latency_" << i << "=" 
                      << g_sub_channel_latency[i].latency(1)
                      << noflush;
        }
        LOG(INFO);
    }
    */
    
    LOG(INFO) << "EchoClient is going to quit";
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(pids[i], NULL);
        } else {
            bthread_join(bids[i], NULL);
        }
    }

    return 0;
}
