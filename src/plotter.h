#pragma once

#include <vector>
#include <memory>
#include <regex>
#include <chrono>
#include <boost/compute.hpp>

#include <OptionParser.h>
#include <spdlog/spdlog.h>

#include "common/paged_block.h"
#include "common/queue.h"
#include "common/timer.h"
#include "common/signal.h"
#include "common/utils.h"
#include "poc/cpu_plotter.h"
#include "poc/gpu_plotter.h"
#include "task_hasher.h"
#include "task_writer.h"
#include "worker_hasher.h"
#include "worker_writer.h"
#include "report.h"

namespace compute = boost::compute;

class plotter {
  plotter() = delete;
  plotter(gpu_plotter&) = delete;
  plotter(gpu_plotter&&) = delete;
  plotter& operator=(gpu_plotter&) = delete;
  plotter& operator=(gpu_plotter&&) = delete;

public:
  plotter(optparse::Values& args) : args_{args} {}

  void run() {
    if ((int)args_.get("plot")) {
      run_plotter();
    } else if ((int)args_.get("test")) {
      run_test();
    }
  }

private:
  void run_test() {
    auto gpu = compute::system::default_device();
    auto plot_id = std::stoull(args_["id"]);
    auto start_nonce = std::stoull(args_["sn"]);
    auto nonces = (int32_t)std::stoull(args_["num"]);

    spdlog::info("do test cpu plot: {}_{}_{}", plot_id, start_nonce, nonces);
    util::timer timer1;
    cpu_plotter cplot;
    cplot.plot(plot_id, start_nonce);
    auto&& chash = cplot.to_string();
    spdlog::info("cpu plot hash: 0x{}", chash.substr(0, 64));
    spdlog::info("cpu plot time cost: {} ms.", timer1.elapsed());

    spdlog::info("do test gpu plot: {}_{}_{}", plot_id, start_nonce, nonces);
    auto plot_args = gpu_plotter::args_t{std::stoull(args_["lws"])
                                        ,std::stoull(args_["gws"])
                                        ,(int32_t)std::stoull(args_["step"])
                                        };
    gpu_plotter gplot(gpu, plot_args);
    auto res = gplot.init("./kernel/kernel.cl", "ploting");
    if (!res)
      spdlog::error("init gpu plotter failed. kernel build log: {}", gplot.program().build_log());
    std::string buff;
    buff.resize(gplot.global_work_size() * gpu_plotter::PLOT_SIZE);
    util::timer timer2;
    for (size_t i=0; i<nonces; i+=gplot.global_work_size())
      gplot.plot( plot_id
                , start_nonce
                , nonces
                , (uint8_t*)buff.data()
                );
    spdlog::info("gpu plot time cost: {} ms.", timer2.elapsed());
    auto ghash = gplot.to_string((uint8_t*)buff.data(), 32);
    spdlog::info("gpu plot hash: 0x{}", ghash);
  }

  void run_plotter() {
    signal::get().install_signal();
    auto plot_id = std::stoull(args_["id"]);
    auto start_nonce = std::stoull(args_["sn"]);
    auto total_nonces = std::stoull(args_["num"]);
    auto max_mem_to_use = uint64_t((double)std::stod(args_["mem"]) * 1024) * 1024 * 1024;
    auto max_weight_per_file = uint64_t((double)std::stod(args_["weight"]) * 1024) * 1024 * 1024;
    util::block_allocator page_block_allocator{max_mem_to_use};
    
    auto patharg = args_["drivers"];
    std::regex re{", "};
    auto drivers = std::vector<std::string> {
      std::sregex_token_iterator(patharg.begin(), patharg.end(), re, -1),
      std::sregex_token_iterator()
    };
    if (patharg.empty() || drivers.empty()) {
      spdlog::warn("No dirver(directory) specified. exit!!!");
      return;
    }

    // TODO: calc by free space
    // TODO: padding
    auto max_nonces_per_file = max_weight_per_file / plotter_base::PLOT_SIZE;
    auto total_files = std::ceil(total_nonces * 1. / max_nonces_per_file);
    auto max_files_per_driver = std::ceil(total_files * 1. / drivers.size());

    // init writer worker and task
    auto sn_to_gen = start_nonce;
    auto nonces_to_gen = total_nonces;
    for (auto& driver : drivers) {
      auto worker = std::make_shared<writer_worker>(*this, driver);
      workers_.push_back(worker);
      for (int i=0; i<max_files_per_driver && nonces_to_gen>0; ++i) {
        int32_t nonces = (int32_t)std::min(nonces_to_gen, max_nonces_per_file);
        auto task = std::make_shared<writer_task>(plot_id, sn_to_gen, nonces, driver);
        sn_to_gen += nonces;
        nonces_to_gen -= nonces;
        worker->push_task(std::move(task));
      }
    }

    // init hasher worker
    auto plot_args = gpu_plotter::args_t{std::stoull(args_["lws"])
                                        ,std::stoull(args_["gws"])
                                        ,(int32_t)std::stoull(args_["step"])
                                        };
    auto device = compute::system::default_device();
    auto ploter = std::make_shared<gpu_plotter>(device, plot_args);
    auto res = ploter->init("./kernel/kernel.cl", "ploting");
    if (!res)
      spdlog::error("init gpu plotter failed. kernel build log: {}", ploter->program().build_log());
    auto hashing = std::make_shared<hasher_worker>(*this, ploter);
    workers_.push_back(hashing);

    spdlog::info("Plotting {} - [{} {}) ...", plot_id, start_nonce, start_nonce+total_nonces);
    for (auto& w : workers_) {
      spdlog::info(w->info());
    }

    std::vector<std::thread> pools;
    for (auto& worker : workers_) {
      pools.emplace_back([=](){ worker->run(); });
    }

    // dispatcher
    int cur_worker_pos{0}, max_worker_pos{(int)workers_.size()-1};
    while (! signal::get().stopped()) {
      auto& report = reporter_.pop_for(std::chrono::milliseconds(100));
      if (workers_.size() == 0)
        continue;

      auto& nb = page_block_allocator.allocate(ploter->global_work_size());
      if (! nb)
        continue;

      if (cur_worker_pos >= max_worker_pos)
        cur_worker_pos = 0;
      auto wr_worker = std::dynamic_pointer_cast<writer_worker>(workers_[cur_worker_pos++]);
      auto ht = wr_worker->next_hasher_task((int)(ploter->global_work_size()), nb);
      if (! ht) {
        page_block_allocator.retain(nb);
      }
      hashing->push_task(std::move(ht));
    }

    spdlog::info("dispatcher thread stopped!!!");
    for (auto& t : pools)
      t.join();
    spdlog::info("all worker thread stopped!!!");
  }

private:
  optparse::Values& args_;

  std::vector<std::shared_ptr<worker>> workers_;
  std::vector<std::shared_ptr<util::paged_block>> blocks_;
  util::queue<report> reporter_;
};