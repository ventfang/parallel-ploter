#include <vector>
#include <string>
#include <sstream>
#include <mutex>
#include <spdlog/spdlog.h>

#include "worker.h"
#include "task_writer.h"
#include "task_hasher.h"
#include "common/signal.h"
#include "plotter.h"

#define NONCES_VECTOR           16
#define NONCES_VECTOR_MASK      15
#define NONCES_VECTOR_ALIGN     (~15)
#define MESSAGE_CAP             64
#define NUM_HASHES              8192
#define HASH_SIZE_WORDS         8
#define NONCE_SIZE_WORDS        HASH_SIZE_WORDS * NUM_HASHES
#define Address(nonce,hash,word) \
              ((nonce) & NONCES_VECTOR_ALIGN) * NONCE_SIZE_WORDS \
            + (hash) * NONCES_VECTOR * HASH_SIZE_WORDS \
            + (word) * NONCES_VECTOR \
            + ((nonce) & NONCES_VECTOR_MASK)

void transposition(uint8_t* data, uint8_t* write_buff, int cur_scoop, int nstart, int nsize) {
  uint32_t* src = (uint32_t*)data;
  uint32_t* des = (uint32_t*)write_buff;

  for (; nsize-->0; ++nstart,des+=16) {
    des[0x00] = src[Address(nstart, cur_scoop * 2, 0)];
    des[0x01] = src[Address(nstart, cur_scoop * 2, 1)];
    des[0x02] = src[Address(nstart, cur_scoop * 2, 2)];
    des[0x03] = src[Address(nstart, cur_scoop * 2, 3)];
    des[0x04] = src[Address(nstart, cur_scoop * 2, 4)];
    des[0x05] = src[Address(nstart, cur_scoop * 2, 5)];
    des[0x06] = src[Address(nstart, cur_scoop * 2, 6)];
    des[0x07] = src[Address(nstart, cur_scoop * 2, 7)];
    des[0x08] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 0)];
    des[0x09] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 1)];
    des[0x0A] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 2)];
    des[0x0B] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 3)];
    des[0x0C] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 4)];
    des[0x0D] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 5)];
    des[0x0E] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 6)];
    des[0x0F] = src[Address(nstart, 8192 - (cur_scoop * 2 + 1), 7)];
  }
}

bool writer_worker::perform_write_plot(std::shared_ptr<writer_task>& write_task, std::shared_ptr<hasher_task>& hash_task) {
  size_t size = (size_t)hash_task->nonces * plotter_base::PLOT_SIZE;
  uint8_t* buff = hash_task->block->data();

  for (int cur_scoop=0; !signal::get().stopped() && cur_scoop<4096; ++cur_scoop) {
    auto offset = ((hash_task->sn - write_task->init_sn) + cur_scoop * write_task->init_nonces) * 64;
    osfile_.seek(offset);
    int nonces = hash_task->nonces;
    for (int nstart=0; !signal::get().stopped() && nonces>0; nonces-=SCOOPS_PER_WRITE, nstart+=SCOOPS_PER_WRITE) {
      transposition(hash_task->block->data(), write_buffer_, cur_scoop, nstart, std::min(nonces, SCOOPS_PER_WRITE));
      osfile_.write(write_buffer_, std::min(nonces, SCOOPS_PER_WRITE) * 64);
    }
  }
  return true;
}

void writer_worker::run() {
  spdlog::info("thread writer worker [{}] starting.", driver_);
  auto bench_mode = ctx_.bench_mode();
  while (! signal::get().stopped()) {
    auto task = fin_hasher_tasks_.pop();
    if (!task)
      continue;
    if (task->current_write_task == -1 || task->current_write_task >= writer_tasks_.size())
      break;
    if (!task->block || !task->writer)
      break;

    // write plot
    auto& wr_task = writer_tasks_[task->current_write_task];
    if ((bench_mode & 0x01) == 0) {
      util::timer timer;
      auto& file_path = wr_task->plot_file();
      if (! util::file::exists(file_path)) {
        if (osfile_.is_open())
          osfile_.close();
        osfile_.open(file_path, true, true);
        osfile_.allocate(wr_task->init_nonces * plotter_base::PLOT_SIZE);
      } else {
        if (! osfile_.is_open()) {
          osfile_.open(file_path, false, true);
          osfile_.allocate(wr_task->init_nonces * plotter_base::PLOT_SIZE);
        } else {
          if (osfile_.filename() != file_path) {
            osfile_.close();
            osfile_.open(file_path, false, true);
            osfile_.allocate(wr_task->init_nonces * plotter_base::PLOT_SIZE);
          }
        }
      }
      perform_write_plot(wr_task, task);
      task->mbps = task->nonces * 1000ull * plotter_base::PLOT_SIZE / 1024 / 1024 / timer.elapsed();
    }
    spdlog::debug("write nonce [{}][{}, {}) ({}) to `{}`"
                  , task->current_write_task
                  , task->sn
                  , task->sn+task->nonces
                  , plotter_base::btoh(task->block->data(), 32)
                  , wr_task->plot_file());
    ctx_.report(std::move(task));
  }
  spdlog::info("waiting for file released...");
  osfile_.close();
  spdlog::error("thread writer worker [{}] stopped.", driver_);
}