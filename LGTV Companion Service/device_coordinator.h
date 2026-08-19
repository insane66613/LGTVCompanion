#pragma once

#include "../Common/external_tv_settings.h"
#include "../Common/log.h"
#include "device_coordinator_core.h"
#include "external_tv_transport.h"
#include "samsung_controller.h"

#include <boost/asio.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class DeviceCoordinator : public std::enable_shared_from_this<DeviceCoordinator> {
public:
    DeviceCoordinator(ExternalTvSettings settings, std::shared_ptr<Logging> log);
    ~DeviceCoordinator();

    void handleEvent(DeviceLifecycleEvent event);
    void shutdown();

private:
    void applyPlan(DevicePlan plan);
    void scheduleDeadline(const DevicePlan& plan);
    void executeSamsungActions(std::vector<DeviceAction> actions);
    void executeVizioActions(std::vector<DeviceAction> actions);
    bool executeSamsungSteps(const std::vector<SamsungCommandStep>& steps);
    void logFailure(const char* device, const char* action, const std::string& error);

    ExternalTvSettings settings_;
    std::shared_ptr<Logging> log_;
    DeviceCoordinatorCore core_;
    boost::asio::io_context control_ioc_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> control_work_;
    boost::asio::steady_timer deadline_timer_;
    boost::asio::thread_pool network_pool_{2};
    boost::asio::strand<boost::asio::thread_pool::executor_type> samsung_strand_{network_pool_.get_executor()};
    boost::asio::strand<boost::asio::thread_pool::executor_type> vizio_strand_{network_pool_.get_executor()};
    std::thread control_thread_;
    std::atomic<bool> stopped_{false};
    std::mutex admission_mutex_;
    SamsungController samsung_controller_;
    std::unique_ptr<SamsungTizenTransport> samsung_transport_;
    std::unique_ptr<VizioSmartCastTransport> vizio_transport_;
};
