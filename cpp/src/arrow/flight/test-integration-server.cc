#include <random>
#include <vector>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include "arrow/table.h"
#include "api.h"
#include "arrow/util/logging.h"
#include <terrier/common/macros.h>
#include <terrier/common/scoped_timer.h>
#include <terrier/common/worker_pool.h>
#include <terrier/storage/garbage_collector.h>
#include <terrier/storage/garbage_collector.h>
#include <terrier/storage/storage_defs.h>
#include <terrier/tpcc/builder.h>
#include <terrier/tpcc/database.h>
#include <terrier/tpcc/delivery.h>
#include <terrier/tpcc/loader.h>
#include <terrier/tpcc/new_order.h>
#include <terrier/tpcc/order_status.h>
#include <terrier/tpcc/payment.h>
#include <terrier/tpcc/stock_level.h>
#include <terrier/tpcc/worker.h>
#include <terrier/tpcc/workload.h>
#include <terrier/storage/block_compactor.h>
#include <terrier/transaction/transaction_manager.h>
#include <terrier/storage/dirty_globals.h>
#include <terrier/storage/arrow_util.h>

namespace terrier {
void NoOp(void *) {}

class TerrierServer : public arrow::flight::FlightServerBase {
 public:
  TerrierServer(storage::DataTable *order_line, transaction::TransactionManager *manager)
      : order_line_(order_line), manager_(manager) {}
  arrow::Status DoAction(const arrow::flight::ServerCallContext &,
                         const arrow::flight::Action &action,
                         std::unique_ptr<arrow::flight::ResultStream> *out) override {
    double hot_ratio = std::stod(action.type);
    printf("Connection dispatched, hot ratio:%f\n", hot_ratio);
    *out = std::unique_ptr<arrow::flight::ResultStream>(new arrow::flight::SimpleResultStream({}));
    return arrow::Status::OK();
  }

  arrow::Status DoGet(const arrow::flight::ServerCallContext &,
                      const arrow::flight::Ticket &,
                      std::unique_ptr<arrow::flight::FlightDataStream> *stream) override {

    std::bernoulli_distribution treat_as_hot{hot_ratio};
    std::list<storage::RawBlock *> blocks = order_line_->blocks_;
    uint32_t blocks_accessed = 0;
    std::vector<std::shared_ptr<arrow::Table>> table_chunks;
    for (storage::RawBlock *block : blocks) {
      blocks_accessed++;
      if (block->controller_.CurrentBlockState() != storage::BlockState::FROZEN || treat_as_hot(generator_)) {
        table_chunks.push_back(MaterializeHotBlock(block));
      } else {
        table_chunks.push_back(storage::ArrowUtil::AssembleToArrowTable(order_line_->accessor_, block));
      }
      if (blocks_accessed % 500 == 0) printf("%u blocks have been processed\n", blocks_accessed);
    }
    ARROW_CHECK_OK(arrow::ConcatenateTables(table_chunks, &logical_table));
    printf("data preparation complete, sending...\n");
    uint64_t size = 0;
    int32_t num_cols = logical_table->num_columns();
    for (int32_t ci = 0; ci < num_cols; ci++) {
      auto col = logical_table->column(ci);
      for (int32_t foo = 0; foo < col->data()->num_chunks(); foo++) {
        auto array_data = col->data()->chunk(foo)->data();
        for (uint64_t bi = 0; bi < array_data->buffers.size(); bi++) {
          auto buffer = array_data->buffers[bi];
          if (buffer.get() == nullptr) continue;
          size += buffer->size();
        }
      }
    }
    printf("server side has %lu bytes of data\n", size);

    *stream = std::unique_ptr<arrow::flight::FlightDataStream>(new arrow::flight::RecordBatchStream(
        std::shared_ptr<arrow::RecordBatchReader>(new arrow::TableBatchReader(*logical_table))));
    return arrow::Status::OK();
  }

 private:
  storage::DataTable *order_line_;
  transaction::TransactionManager *manager_;
  std::default_random_engine generator_;
  bool first_call = true;
  std::shared_ptr<arrow::Table> logical_table;
  uint64_t buf[1024]{};
  double hot_ratio;

  template<class IntType, class T>
  void Append(T *builder, storage::ProjectedRow *row, uint16_t i) {
    auto *int_pointer = row->AccessWithNullCheck(i);
    if (int_pointer == nullptr)
      auto status
      UNUSED_ATTRIBUTE = builder->AppendNull();
    else
      auto status1
      UNUSED_ATTRIBUTE = builder->Append(*reinterpret_cast<IntType *>(int_pointer));
  }

  std::shared_ptr<arrow::Table> MaterializeHotBlock(storage::RawBlock *block) {
    storage::DataTable *table = order_line_;
    const storage::BlockLayout &layout = table->accessor_.GetBlockLayout();
    if (first_call) {
      auto initializer = storage::ProjectedRowInitializer::CreateProjectedRowInitializer(layout, layout.AllColumns());
      initializer.InitializeRow(&buf);
      first_call = false;
    }
    auto *row = reinterpret_cast<storage::ProjectedRow *>(&buf);
    transaction::TransactionContext *txn = manager_->BeginTransaction();
    arrow::Int32Builder o_id_builder;
    arrow::Int8Builder o_d_id_builder;
    arrow::Int8Builder o_w_id_builder;
    arrow::Int8Builder ol_number_builder;
    arrow::Int32Builder ol_i_id_builder;
    arrow::Int8Builder ol_supply_w_id_builder;
    arrow::Int64Builder ol_delivery_d_builder;
    arrow::Int8Builder ol_quantity_builder;
    arrow::Int64Builder ol_amount_builder;
    arrow::StringBuilder ol_dist_info_builder;
    for (uint32_t i = 0; i < layout.NumSlots(); i++) {
      storage::TupleSlot slot(block, i);
      bool visible = table->Select(txn, slot, row);
      if (!visible) continue;
      Append<uint32_t>(&o_id_builder, row, storage::DirtyGlobals::ol_o_id_insert_pr_offset);
      Append<uint8_t>(&o_d_id_builder, row, storage::DirtyGlobals::ol_d_id_insert_pr_offset);
      Append<uint8_t>(&o_w_id_builder, row, storage::DirtyGlobals::ol_w_id_insert_pr_offset);
      Append<uint8_t>(&ol_number_builder, row, storage::DirtyGlobals::ol_number_insert_pr_offset);
      Append<uint32_t>(&ol_i_id_builder, row, storage::DirtyGlobals::ol_i_id_insert_pr_offset);
      Append<uint8_t>(&ol_supply_w_id_builder, row, storage::DirtyGlobals::ol_supply_w_id_insert_pr_offset);
      Append<uint64_t>(&ol_delivery_d_builder, row, storage::DirtyGlobals::ol_delivery_d_insert_pr_offset);
      Append<uint8_t>(&ol_quantity_builder, row, storage::DirtyGlobals::ol_quantity_insert_pr_offset);
      Append<uint64_t>(&ol_amount_builder, row, storage::DirtyGlobals::ol_amount_insert_pr_offset);

      auto *varlen_pointer = row->AccessWithNullCheck(storage::DirtyGlobals::ol_dist_info_insert_pr_offset);
      if (varlen_pointer == nullptr) {
        auto status
        UNUSED_ATTRIBUTE = ol_dist_info_builder.AppendNull();
      } else {
        auto *entry = reinterpret_cast<storage::VarlenEntry *>(varlen_pointer);
        auto status2
        UNUSED_ATTRIBUTE =
            ol_dist_info_builder.Append(reinterpret_cast<const uint8_t *>(entry->Content()), entry->Size());
      }
    }

    manager_->Commit(txn, NoOp, nullptr);

    std::shared_ptr<arrow::Array> o_id, o_d_id, o_w_id, ol_number,
        ol_i_id, ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info;
    auto status
    UNUSED_ATTRIBUTE = o_id_builder.Finish(&o_id);
    auto status1
    UNUSED_ATTRIBUTE = o_d_id_builder.Finish(&o_d_id);
    auto status2
    UNUSED_ATTRIBUTE = o_w_id_builder.Finish(&o_w_id);
    auto status3
    UNUSED_ATTRIBUTE = ol_number_builder.Finish(&ol_number);
    auto status4
    UNUSED_ATTRIBUTE = ol_i_id_builder.Finish(&ol_i_id);
    auto status5
    UNUSED_ATTRIBUTE = ol_supply_w_id_builder.Finish(&ol_supply_w_id);
    auto status6
    UNUSED_ATTRIBUTE = ol_delivery_d_builder.Finish(&ol_delivery_d);
    auto status7
    UNUSED_ATTRIBUTE = ol_quantity_builder.Finish(&ol_quantity);
    auto status8
    UNUSED_ATTRIBUTE = ol_amount_builder.Finish(&ol_amount);
    auto status9
    UNUSED_ATTRIBUTE = ol_dist_info_builder.Finish(&ol_dist_info);

    std::vector<std::shared_ptr<arrow::Field>> schema_vector{
        arrow::field("", arrow::utf8()),
        arrow::field("", arrow::uint64()),
        arrow::field("", arrow::uint64()),
        arrow::field("", arrow::uint32()),
        arrow::field("", arrow::uint32()),
        arrow::field("", arrow::uint8()),
        arrow::field("", arrow::uint8()),
        arrow::field("", arrow::uint8()),
        arrow::field("", arrow::uint8()),
        arrow::field("", arrow::uint8())};

    std::vector<std::shared_ptr<arrow::Array>> table_vector
        {ol_dist_info, ol_delivery_d, ol_amount, o_id, ol_i_id, o_d_id, o_w_id, ol_number, ol_supply_w_id, ol_quantity};
    return arrow::Table::Make(std::make_shared<arrow::Schema>(schema_vector), table_vector);
  }
};

class TpccLoader {
 public:
  void StartGC(transaction::TransactionManager *const) {

    gc_ = new storage::GarbageCollector(&txn_manager, &access_observer_);
//    gc_ = new storage::GarbageCollector(txn_manager);
    run_gc_ = true;
    gc_thread_ = std::thread([this] { GCThreadLoop(); });
  }

  void EndGC() {
    run_gc_ = false;
    gc_thread_.join();
    // Make sure all garbage is collected. This take 2 runs for unlink and deallocate
    gc_->PerformGarbageCollection();
    gc_->PerformGarbageCollection();
    delete gc_;
  }

  void StartCompactor(transaction::TransactionManager *const) {
    run_compactor_ = true;
    compactor_thread_ = std::thread([this] { CompactorThreadLoop(&txn_manager); });
  }

  void EndCompactor() {
    run_compactor_ = false;
    compactor_thread_.join();
  }

  const uint64_t blockstore_size_limit_ = 50000;
  const uint64_t blockstore_reuse_limit_ = 50000;
  const uint64_t buffersegment_size_limit_ = 5000000;
  const uint64_t buffersegment_reuse_limit_ = 5000000;
  storage::BlockStore block_store_{blockstore_size_limit_, blockstore_reuse_limit_};
  storage::RecordBufferSegmentPool buffer_pool_{buffersegment_size_limit_, buffersegment_reuse_limit_};
  std::default_random_engine generator_;
  storage::LogManager *log_manager_ = nullptr;
  storage::BlockCompactor compactor_;
  storage::AccessObserver access_observer_{&compactor_};

  const int8_t num_threads_ = 6;
  const uint32_t num_precomputed_txns_per_worker_ = 10000;
  const uint32_t w_payment = 43;
  const uint32_t w_delivery = 4;
  const uint32_t w_order_status = 4;
  const uint32_t w_stock_level = 4;

  common::WorkerPool thread_pool_{static_cast<uint32_t>(num_threads_), {}};

  void ServerLoop(tpcc::Database *tpcc_db) {
    storage::DataTable *order_line = tpcc_db->order_line_table_->table_.data_table;
    TerrierServer server(order_line, &txn_manager);
    ARROW_CHECK_OK(server.Init(std::unique_ptr<arrow::flight::NoOpAuthHandler>(), 15712));
//    ARROW_CHECK_OK(server.SetShutdownOnSignals({SIGTERM}));
    printf("Server listening on localhost:15712\n");
    ARROW_CHECK_OK(server.Serve());
  }

  void Run() {
    // one TPCC worker = one TPCC terminal = one thread
    std::vector<tpcc::Worker> workers;
    workers.reserve(num_threads_);

    thread_pool_.Shutdown();
    thread_pool_.SetNumWorkers(num_threads_);
    thread_pool_.Startup();

    // we need transactions, TPCC database, and GC
//  log_manager_ = new storage::LogManager(LOG_FILE_NAME, &buffer_pool_);
    auto tpcc_builder = tpcc::Builder(&block_store_);

    // random number generation is slow, so we precompute the args
    std::vector<std::vector<tpcc::TransactionArgs>> precomputed_args;
    precomputed_args.reserve(workers.size());

    tpcc::Deck deck(w_payment, w_order_status, w_delivery, w_stock_level);

    for (int8_t warehouse_id = 1; warehouse_id <= num_threads_; warehouse_id++) {
      std::vector<tpcc::TransactionArgs> txns;
      txns.reserve(num_precomputed_txns_per_worker_);
      for (uint32_t i = 0; i < num_precomputed_txns_per_worker_; i++) {
        switch (deck.NextCard()) {
          case tpcc::TransactionType::NewOrder:
            txns.emplace_back(tpcc::BuildNewOrderArgs(&generator_,
                                                      warehouse_id,
                                                      num_threads_));
            break;
          case tpcc::TransactionType::Payment:
            txns.emplace_back(tpcc::BuildPaymentArgs(&generator_,
                                                     warehouse_id,
                                                     num_threads_));
            break;
          case tpcc::TransactionType::OrderStatus:
            txns.emplace_back(tpcc::BuildOrderStatusArgs(&generator_,
                                                         warehouse_id,
                                                         num_threads_));
            break;
          case tpcc::TransactionType::Delivery:
            txns.emplace_back(tpcc::BuildDeliveryArgs(&generator_,
                                                      warehouse_id,
                                                      num_threads_));
            break;
          case tpcc::TransactionType::StockLevel:
            txns.emplace_back(tpcc::BuildStockLevelArgs(&generator_,
                                                        warehouse_id,
                                                        num_threads_));
            break;
          default:throw std::runtime_error("Unexpected transaction type.");
        }
      }
      precomputed_args.emplace_back(txns);
    }

    auto *const tpcc_db = tpcc_builder.Build();
    storage::DirtyGlobals::tpcc_db = tpcc_db;

    // prepare the workers
    workers.clear();
    for (int8_t i = 0; i < num_threads_; i++) {
      workers.emplace_back(tpcc_db);
    }
    printf("loading database...\n");
    compactor_ = storage::BlockCompactor();
    access_observer_ = storage::AccessObserver(&compactor_);

    tpcc::Loader::PopulateDatabase(&txn_manager, &generator_, tpcc_db, workers);
//    log_manager_->Process();  // log all of the Inserts from table creation
    StartGC(&txn_manager);

//    StartLogging();
    std::this_thread::sleep_for(std::chrono::seconds(1));  // Let GC clean up
    StartCompactor(&txn_manager);
    // define the TPCC workload
    auto tpcc_workload = [&](int8_t worker_id) {
      auto new_order = tpcc::NewOrder(tpcc_db);
      auto payment = tpcc::Payment(tpcc_db);
      auto order_status = tpcc::OrderStatus(tpcc_db);
      auto delivery = tpcc::Delivery(tpcc_db);
      auto stock_level = tpcc::StockLevel(tpcc_db);

      for (uint32_t i = 0; i < num_precomputed_txns_per_worker_; i++) {
        auto &txn_args = precomputed_args[worker_id][i];
        switch (txn_args.type) {
          case tpcc::TransactionType::NewOrder: {
            if (!new_order.Execute(&txn_manager,
                                   &generator_,
                                   tpcc_db,
                                   &workers[worker_id],
                                   txn_args))
              txn_args.aborted++;
            break;
          }
          case tpcc::TransactionType::Payment: {
            if (!payment.Execute(&txn_manager, &generator_, tpcc_db, &workers[worker_id], txn_args)) txn_args.aborted++;
            break;
          }
          case tpcc::TransactionType::OrderStatus: {
            if (!order_status.Execute(&txn_manager,
                                      &generator_,
                                      tpcc_db,
                                      &workers[worker_id],
                                      txn_args))
              txn_args.aborted++;
            break;
          }
          case tpcc::TransactionType::Delivery: {
            if (!delivery.Execute(&txn_manager,
                                  &generator_,
                                  tpcc_db,
                                  &workers[worker_id],
                                  txn_args))
              txn_args.aborted++;
            break;
          }
          case tpcc::TransactionType::StockLevel: {
            if (!stock_level.Execute(&txn_manager,
                                     &generator_,
                                     tpcc_db,
                                     &workers[worker_id],
                                     txn_args))
              txn_args.aborted++;
            break;
          }
          default:throw std::runtime_error("Unexpected transaction type.");
        }
      }
    };
    printf("starting workload\n");
    // run the TPCC workload to completion
    {
      for (int8_t i = 0; i < num_threads_; i++) {
        thread_pool_.SubmitTask([i, &tpcc_workload] { tpcc_workload(i); });
      }
      thread_pool_.WaitUntilAllFinished();
      printf("transactions all submitted\n");
//      EndLogging();
    }
    // cleanup
    std::this_thread::sleep_for(std::chrono::seconds(5));  // Let GC clean up
    EndCompactor();
    EndGC();
    printf("order_line table:\n");
    tpcc_db->order_line_table_->table_.data_table->InspectTable();
    printf("\n\n\n");

    ServerLoop(tpcc_db);
    // Clean up the buffers from any non-inlined VarlenEntrys in the precomputed args
    for (const auto &worker_id : precomputed_args) {
      for (const auto &args : worker_id) {
        if ((args.type == tpcc::TransactionType::Payment || args.type == tpcc::TransactionType::OrderStatus) &&
            args.use_c_last && !args.c_last.IsInlined()) {
          delete[] args.c_last.Content();
        }
      }
    }
    delete tpcc_db;
    delete log_manager_;
  }

 private:
  std::thread gc_thread_;
  storage::GarbageCollector *gc_ = nullptr;
  volatile bool run_gc_ = false;
  const std::chrono::milliseconds gc_period_{1};
  transaction::TransactionManager txn_manager{&buffer_pool_, true, LOGGING_DISABLED};

  void GCThreadLoop() {
    while (run_gc_) {
      std::this_thread::sleep_for(gc_period_);
      gc_->PerformGarbageCollection();
    }
  }

  std::thread compactor_thread_;
  volatile bool run_compactor_ = false;
  const std::chrono::milliseconds compaction_period_{10};

  void CompactorThreadLoop(transaction::TransactionManager *const) {
    while (run_compactor_) {
      std::this_thread::sleep_for(compaction_period_);
      compactor_.ProcessCompactionQueue(&txn_manager);
    }
  }
};
}

int main() {
  terrier::TpccLoader b;
  b.Run();
  return 0;
}