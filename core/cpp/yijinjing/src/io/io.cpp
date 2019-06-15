/*****************************************************************************
 * Copyright [taurus.ai]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

#include <typeinfo>
#include <spdlog/spdlog.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <nanomsg/pubsub.h>
#include <nanomsg/pipeline.h>
#include <nlohmann/json.hpp>

#include <kungfu/yijinjing/util/util.h>
#include <kungfu/yijinjing/util/os.h>
#include <kungfu/yijinjing/io.h>

using namespace kungfu::yijinjing;
using namespace kungfu::yijinjing::data;
using namespace kungfu::yijinjing::journal;
using namespace kungfu::yijinjing::nanomsg;

namespace kungfu
{
    namespace yijinjing
    {
        class ipc_url_factory : public url_factory
        {
        public:

            const std::string make_url_bind(const data::location &location, protocol p) const override
            {
                std::string socket_dir = util::make_path({KF_DIR_SOCKET, get_category_name(location.category), location.group});
                return "ipc://" + socket_dir + "/" + location.name + "." + get_protocol_name(p);
            }

            const std::string make_url_connect(const data::location &location, protocol p) const override
            {
                std::string socket_dir = util::make_path({KF_DIR_SOCKET, get_category_name(location.category), location.group});
                return "ipc://" + socket_dir + "/" + location.name + "." + get_protocol_name(get_opposite_protol(p));
            }
        };

        class noop_publisher : public publisher
        {
        public:
            int notify() override
            {
                return 0;
            }

            int publish(const std::string &json_message) override
            {
                throw journal_error("noop publisher does not publish anything");
            }
        };

        class nanomsg_publisher : public publisher
        {
        public:
            nanomsg_publisher(url_factory_ptr factory, bool low_latency) : socket_(protocol::PUSH), low_latency_(low_latency)
            {
                auto url = factory->make_url_connect(data::location(data::mode::LIVE, data::category::SYSTEM, "master", "master"), socket_.get_protocol());
                socket_.connect(url);
                SPDLOG_DEBUG("ready to publish and notify to master [{}]", url);
            }

            ~nanomsg_publisher() override
            {
                SPDLOG_DEBUG("master publisher closing");
                socket_.close();
                SPDLOG_DEBUG("master publisher closed");
            }

            int notify() override
            {
                return low_latency_ ? 0 : publish("{}");
            }

            int publish(const std::string &json_message) override
            {
                return socket_.send(json_message);
            }

        private:
            const bool low_latency_;
            socket socket_;
        };

        class nanomsg_observer : public observer
        {
        public:
            nanomsg_observer(url_factory_ptr factory, bool low_latency) : socket_(protocol::SUBSCRIBE)
            {
                int timeout = low_latency ? 0 : DEFAULT_NOTICE_TIMEOUT;
                auto url = factory->make_url_connect(data::location(data::mode::LIVE, data::category::SYSTEM, "master", "master"), socket_.get_protocol());
                socket_.connect(url);
                socket_.setsockopt_int(NN_SOL_SOCKET, NN_RCVTIMEO, timeout);
                socket_.setsockopt_str(NN_SUB, NN_SUB_SUBSCRIBE, "");
                SPDLOG_DEBUG("observing master chanel with timeout {}ms [{}]", timeout, url);
            }

            ~nanomsg_observer() override
            {
                SPDLOG_DEBUG("master observer closing");
                socket_.close();
                SPDLOG_DEBUG("master observer closed");
            }

            bool wait() override
            {
                return socket_.recv() > 2;
            }

            const std::string &get_notice() override
            {
                return socket_.last_message();
            }

        private:
            socket socket_;
        };


        class nanomsg_master_service : public master_service
        {
        public:
            nanomsg_master_service(url_factory_ptr factory) : socket_(protocol::REQUEST)
            {
                auto url = factory->make_url_connect(data::location(data::mode::LIVE, data::category::SYSTEM, "master", "master"), socket_.get_protocol());
                SPDLOG_INFO("ready to use master service [{}]", url);
                socket_.connect(url);
            }

            ~nanomsg_master_service() override
            {
                socket_.close();
            }

            const std::string &request(const std::string &json_message) override
            {
                socket_.send(json_message);
                return socket_.recv_msg();
            }

        private:
            socket socket_;
        };


        io_device::io_device(const bool low_latency, const bool lazy) : low_latency_(low_latency), lazy_(lazy)
        {
            SPDLOG_DEBUG("creating io_device {}", low_latency);

            url_factory_ = std::make_shared<ipc_url_factory>();
        }

        io_device_ptr io_device::create_io_device(bool low_latency = false)
        {
            auto r = std::shared_ptr<io_device>(new io_device(low_latency));
            r->publisher_ = std::make_shared<noop_publisher>();
            return r;
        }

        reader_ptr io_device::open_reader_to_subscribe()
        {
            return std::make_shared<reader>(lazy_);
        }

        reader_ptr io_device::open_reader(const data::location &location)
        {
            auto r = std::make_shared<reader>(lazy_);
            r->subscribe(location, 0);
            return r;
        }

        writer_ptr io_device::open_writer(const data::location &location)
        {
            return std::make_shared<writer>(location, lazy_, publisher_);
        }

        socket_ptr io_device::connect_socket(const data::location &location, const protocol &p, int timeout)
        {
            socket_ptr s = std::make_shared<socket>(p);
            auto url = url_factory_->make_url_connect(location, p);
            s->connect(url);
            s->setsockopt_int(NN_SOL_SOCKET, NN_RCVTIMEO, timeout);
            SPDLOG_INFO("connected socket [{}] {} at {} with timeout {}", nanomsg::get_protocol_name(p), location.name, url, timeout);
            return s;
        }

        socket_ptr io_device::bind_socket(const data::location &location, const protocol &p, int timeout)
        {
            socket_ptr s = std::make_shared<socket>(p);
            auto url = url_factory_->make_url_bind(location, p);
            s->bind(url);
            s->setsockopt_int(NN_SOL_SOCKET, NN_RCVTIMEO, timeout);
            SPDLOG_INFO("bind to socket [{}] {} at {} with timeout {}", nanomsg::get_protocol_name(p), location.name, url, timeout);
            return s;
        }

        io_device_client::io_device_client(std::string name, bool low_latency) : io_device(low_latency, true), name_(std::move(name))
        {
            SPDLOG_DEBUG("creating io_device_client {}", name);

            observer_ = std::make_shared<nanomsg_observer>(get_url_factory(), is_low_latency());

            service_ = std::make_shared<nanomsg_master_service>(get_url_factory());
        }

        io_device_client_ptr io_device_client::create_io_device(std::string name, bool low_latency = false)
        {
            auto r = std::shared_ptr<io_device_client>(new io_device_client(name, low_latency));
            r->publisher_ = std::make_shared<nanomsg_publisher>(r->get_url_factory(), low_latency);
            return r;
        }
    }
}


