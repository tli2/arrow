// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Client implementation for Flight integration testing. Loads
// RecordBatches from the given JSON file and uploads them to the
// Flight server, which stores the data and schema in memory. The
// client then requests the data from the server and compares it to
// the data originally uploaded.

#include <iostream>
#include <memory>
#include <string>
#include <chrono>

#include <gflags/gflags.h>

#include "arrow/io/test-common.h"
#include "arrow/ipc/json-integration.h"
#include "arrow/ipc/writer.h"
#include "arrow/record_batch.h"
#include "arrow/table.h"
#include "arrow/util/logging.h"

#include "arrow/flight/api.h"
#include "arrow/flight/test-util.h"

int main(int argc, char **argv) {
  std::unique_ptr<arrow::flight::FlightClient> read_client;
  ARROW_CHECK_OK(arrow::flight::FlightClient::Connect("snode", 15712, &read_client));
  printf("Connection Request Sent\n");
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  arrow::flight::Action action{};
  action.type = std::string(argv[1]);
  std::unique_ptr<arrow::flight::ResultStream> res;
  ARROW_CHECK_OK(read_client->DoAction(action, &res));
  std::unique_ptr<arrow::RecordBatchReader> stream;
  ARROW_CHECK_OK(read_client->DoGet(arrow::flight::Ticket{}, &stream));
  printf("Connection Established, receiving data...\n");
  std::shared_ptr<arrow::Table> table;
  std::vector<std::shared_ptr<arrow::RecordBatch>> retrieved_chunks;
  std::shared_ptr<arrow::RecordBatch> chunk;
  while (true) {
    ARROW_CHECK_OK(stream->ReadNext(&chunk));
    if (chunk == nullptr) break;
    retrieved_chunks.push_back(chunk);
  }
  printf("Data all received, converting...\n");
  std::shared_ptr<arrow::Schema> schema = stream->schema();
  ARROW_CHECK_OK(arrow::Table::FromRecordBatches(schema, retrieved_chunks, &table));
  printf("Transmission complete\n");
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  fprintf(stdout, "Client side TOTAL duration: %ld\n", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

  uint64_t size = 0;
  int32_t num_cols = table->num_columns();
  for (int32_t ci = 0; ci < num_cols; ci++) {
    auto col = table->column(ci);
    for (int32_t foo = 0; foo < col->data()->num_chunks(); foo++) {
      auto array_data = col->data()->chunk(foo)->data();
      for (uint64_t bi = 0; bi < array_data->buffers.size(); bi++) {
        auto buffer = array_data->buffers[bi];
        if (buffer.get() == nullptr) continue;
        size += buffer->size();
      }
    }
  }
  printf("total received bytes:%lu\n", size);
  return 0;
}
