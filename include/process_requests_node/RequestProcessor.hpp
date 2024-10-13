#ifndef REQUEST_PROCESSOR_HPP
#define REQUEST_PROCESSOR_HPP

#include "process_requests_node/ThreadPool.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <thread>
#include <unordered_map>

class RequestProcessor {
public:
  RequestProcessor(size_t thread_pool_size = 4)
      : thread_pool_(thread_pool_size) {}

  // 注册服务，避免重复插入
  template <typename ServiceT>
  void add_service(const std::string &service_name) {
    // 检查服务是否已经存在
    auto node = std::make_shared<rclcpp::Node>(service_name + "_node");    
    if (services_.find(service_name) != services_.end()) {
      RCLCPP_WARN(node->get_logger(),
                  "Service '%s' is already registered.", service_name.c_str());
      node.reset(); 
      return;
    }
    // 如果不存在，则创建服务节点和客户端
    auto client = node->create_client<ServiceT>(service_name);
    services_[service_name] = {node, client};
  }

  // 移除服务
  void remove_service(const std::string &service_name) {
    services_.erase(service_name);
  }

  // 处理服务请求
  template <typename ServiceT, typename RequestT, typename ResponseT>
  void process_request(
      const std::string &service_name, std::shared_ptr<RequestT> request,
      std::function<void(std::shared_ptr<ResponseT>)> callback,
      const std::chrono::seconds &timeout = std::chrono::seconds(3)) {

    thread_pool_.enqueue([this, service_name, request, callback, timeout]() {
      auto it = services_.find(service_name);
      if (it == services_.end()) {
        RCLCPP_ERROR(rclcpp::get_logger("RequestProcessor"),
                     "Service '%s' is not registered.", service_name.c_str());
        return;
      }

      auto node = it->second.node;
      auto client =
          std::static_pointer_cast<rclcpp::Client<ServiceT>>(it->second.client);

      // 检查服务是否可用
      while (!client->wait_for_service(timeout)) {
        if (!rclcpp::ok()) {
          RCLCPP_ERROR(
              node->get_logger(),
              "client interrupted while waiting for service to appear.");
          return;
        }
        RCLCPP_INFO(node->get_logger(), "waiting for service to appear...");
      }

      // 异步发送请求
      auto future_result = client->async_send_request(request);

      // 等待服务结果
      auto result =
          rclcpp::spin_until_future_complete(node, future_result, timeout);
      if (result == rclcpp::FutureReturnCode::SUCCESS) {
        try {
          callback(future_result.get());
        } catch (const std::exception &e) {
          RCLCPP_ERROR(node->get_logger(), "Exception in callback: %s",
                       e.what());
        }
      } else if (result == rclcpp::FutureReturnCode::TIMEOUT) {
        RCLCPP_ERROR(node->get_logger(), "Service call to '%s' timed out.",
                     service_name.c_str());
      } else {
        RCLCPP_ERROR(node->get_logger(), "Service call to '%s' failed.",
                     service_name.c_str());
      }
    });
  }

private:
  struct RequestInfo {
    rclcpp::Node::SharedPtr node;         // 每个服务独立的节点
    rclcpp::ClientBase::SharedPtr client; // 服务客户端
  };

  ThreadPool thread_pool_; // 线程池处理任务
  std::unordered_map<std::string, RequestInfo> services_; // 服务的映射表
};

#endif // REQUEST_PROCESSOR_HPP
