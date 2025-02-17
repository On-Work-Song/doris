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

#pragma once

#include <butil/macros.h>
#include <fmt/format.h>
#include <gen_cpp/Descriptors_types.h>
#include <glog/logging.h>
#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <ostream>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/object_pool.h"
#include "common/status.h"
#include "olap/column_mapping.h"
#include "olap/olap_common.h"
#include "olap/rowset/pending_rowset_helper.h"
#include "olap/rowset/rowset.h"
#include "olap/rowset/rowset_reader.h"
#include "olap/rowset/rowset_writer.h"
#include "olap/rowset/segment_v2/inverted_index_writer.h"
#include "olap/storage_engine.h"
#include "olap/tablet.h"
#include "olap/tablet_fwd.h"
#include "olap/tablet_schema.h"
#include "runtime/descriptors.h"
#include "runtime/memory/mem_tracker.h"
#include "vec/data_types/data_type.h"

namespace doris {
class DeleteHandler;
class Field;
class TAlterInvertedIndexReq;
class TAlterTabletReqV2;
class TExpr;
enum AlterTabletType : int;
enum RowsetTypePB : int;
enum SegmentsOverlapPB : int;

namespace vectorized {
class Block;
class OlapBlockDataConvertor;
} // namespace vectorized

class BlockChanger {
public:
    BlockChanger(TabletSchemaSPtr tablet_schema, DescriptorTbl desc_tbl);

    ~BlockChanger();

    ColumnMapping* get_mutable_column_mapping(size_t column_index);

    Status change_block(vectorized::Block* ref_block, vectorized::Block* new_block) const;

    void set_where_expr(const std::shared_ptr<TExpr>& where_expr) { _where_expr = where_expr; }

    void set_type(AlterTabletType type) { _type = type; }

    void set_compatible_version(int32_t version) noexcept { _fe_compatible_version = version; }

    bool has_where() const { return _where_expr != nullptr; }

private:
    static Status _check_cast_valid(vectorized::ColumnPtr ref_column,
                                    vectorized::ColumnPtr new_column, AlterTabletType type);

    // @brief column-mapping specification of new schema
    SchemaMapping _schema_mapping;

    DescriptorTbl _desc_tbl;

    std::shared_ptr<TExpr> _where_expr;

    AlterTabletType _type;

    int32_t _fe_compatible_version = -1;
};

class SchemaChange {
public:
    SchemaChange() = default;
    virtual ~SchemaChange() = default;

    virtual Status process(RowsetReaderSharedPtr rowset_reader, RowsetWriter* rowset_writer,
                           BaseTabletSPtr new_tablet, BaseTabletSPtr base_tablet,
                           TabletSchemaSPtr base_tablet_schema,
                           TabletSchemaSPtr new_tablet_schema) {
        if (rowset_reader->rowset()->empty() || rowset_reader->rowset()->num_rows() == 0) {
            RETURN_IF_ERROR(rowset_writer->flush());
            return Status::OK();
        }

        _filtered_rows = 0;
        _merged_rows = 0;
        RETURN_IF_ERROR(_inner_process(rowset_reader, rowset_writer, new_tablet, base_tablet_schema,
                                       new_tablet_schema));

        // Check row num changes
        if (!_check_row_nums(rowset_reader, *rowset_writer)) {
            return Status::Error<ErrorCode::ALTER_STATUS_ERR>("SchemaChange check row nums failed");
        }

        LOG(INFO) << "all row nums. source_rows=" << rowset_reader->rowset()->num_rows()
                  << ", merged_rows=" << merged_rows() << ", filtered_rows=" << filtered_rows()
                  << ", new_index_rows=" << rowset_writer->num_rows();
        return Status::OK();
    }

    uint64_t filtered_rows() const { return _filtered_rows; }

    uint64_t merged_rows() const { return _merged_rows; }

protected:
    void _add_filtered_rows(uint64_t filtered_rows) { _filtered_rows += filtered_rows; }

    void _add_merged_rows(uint64_t merged_rows) { _merged_rows += merged_rows; }

    virtual Status _inner_process(RowsetReaderSharedPtr rowset_reader, RowsetWriter* rowset_writer,
                                  BaseTabletSPtr new_tablet, TabletSchemaSPtr base_tablet_schema,
                                  TabletSchemaSPtr new_tablet_schema) {
        return Status::NotSupported("inner process unsupported.");
    }

    virtual bool _check_row_nums(RowsetReaderSharedPtr reader, const RowsetWriter& writer) const {
        if (reader->rowset()->num_rows() - reader->filtered_rows() !=
            writer.num_rows() + writer.num_rows_filtered() + _merged_rows + _filtered_rows) {
            LOG(WARNING) << "fail to check row num! "
                         << "source_rows=" << reader->rowset()->num_rows()
                         << ", source_filtered_rows=" << reader->filtered_rows()
                         << ", written_rows=" << writer.num_rows()
                         << ", writer_filtered_rows=" << writer.num_rows_filtered()
                         << ", merged_rows=" << merged_rows()
                         << ", filtered_rows=" << filtered_rows();
            return false;
        }
        return true;
    }

private:
    uint64_t _filtered_rows {};
    uint64_t _merged_rows {};
};

class LinkedSchemaChange : public SchemaChange {
public:
    LinkedSchemaChange() = default;
    ~LinkedSchemaChange() override = default;

    Status process(RowsetReaderSharedPtr rowset_reader, RowsetWriter* rowset_writer,
                   BaseTabletSPtr new_tablet, BaseTabletSPtr base_tablet,
                   TabletSchemaSPtr base_tablet_schema,
                   TabletSchemaSPtr new_tablet_schema) override;

private:
    DISALLOW_COPY_AND_ASSIGN(LinkedSchemaChange);
};

class VSchemaChangeDirectly : public SchemaChange {
public:
    VSchemaChangeDirectly(const BlockChanger& changer) : _changer(changer) {}

private:
    Status _inner_process(RowsetReaderSharedPtr rowset_reader, RowsetWriter* rowset_writer,
                          BaseTabletSPtr new_tablet, TabletSchemaSPtr base_tablet_schema,
                          TabletSchemaSPtr new_tablet_schema) override;

    bool _check_row_nums(RowsetReaderSharedPtr reader, const RowsetWriter& writer) const override {
        return _changer.has_where() || SchemaChange::_check_row_nums(reader, writer);
    }

    const BlockChanger& _changer;
};

class VBaseSchemaChangeWithSorting : public SchemaChange {
public:
    VBaseSchemaChangeWithSorting(const BlockChanger& changer, size_t memory_limitation);
    ~VBaseSchemaChangeWithSorting() override = default;

    Status _inner_process(RowsetReaderSharedPtr rowset_reader, RowsetWriter* rowset_writer,
                          BaseTabletSPtr new_tablet, TabletSchemaSPtr base_tablet_schema,
                          TabletSchemaSPtr new_tablet_schema) override;

    virtual Result<RowsetSharedPtr> _internal_sorting(
            const std::vector<std::unique_ptr<vectorized::Block>>& blocks,
            const Version& temp_delta_versions, int64_t newest_write_timestamp,
            BaseTabletSPtr new_tablet, RowsetTypePB new_rowset_type,
            SegmentsOverlapPB segments_overlap, TabletSchemaSPtr new_tablet_schema);

    Status _external_sorting(std::vector<RowsetSharedPtr>& src_rowsets, RowsetWriter* rowset_writer,
                             BaseTabletSPtr new_tablet, TabletSchemaSPtr new_tablet_schema);

protected:
    // for external sorting
    // src_rowsets to store the rowset generated by internal sorting
    std::vector<RowsetSharedPtr> _src_rowsets;

private:
    bool _check_row_nums(RowsetReaderSharedPtr reader, const RowsetWriter& writer) const override {
        return _changer.has_where() || SchemaChange::_check_row_nums(reader, writer);
    }

    const BlockChanger& _changer;
    size_t _memory_limitation;
    Version _temp_delta_versions;
    std::unique_ptr<MemTracker> _mem_tracker;
};

// @breif schema change with sorting
// Mixin for local StorageEngine
class VLocalSchemaChangeWithSorting final : public VBaseSchemaChangeWithSorting {
public:
    VLocalSchemaChangeWithSorting(const BlockChanger& changer, size_t memory_limitation,
                                  StorageEngine& local_storage_engine)
            : VBaseSchemaChangeWithSorting(changer, memory_limitation),
              _local_storage_engine(local_storage_engine) {}
    ~VLocalSchemaChangeWithSorting() override = default;

    Status _inner_process(RowsetReaderSharedPtr rowset_reader, RowsetWriter* rowset_writer,
                          BaseTabletSPtr new_tablet, TabletSchemaSPtr base_tablet_schema,
                          TabletSchemaSPtr new_tablet_schema) override;

    Result<RowsetSharedPtr> _internal_sorting(
            const std::vector<std::unique_ptr<vectorized::Block>>& blocks,
            const Version& temp_delta_versions, int64_t newest_write_timestamp,
            BaseTabletSPtr new_tablet, RowsetTypePB new_rowset_type,
            SegmentsOverlapPB segments_overlap, TabletSchemaSPtr new_tablet_schema) override;

private:
    StorageEngine& _local_storage_engine;
    std::vector<PendingRowsetGuard> _pending_rs_guards;
};

struct AlterMaterializedViewParam {
    std::string column_name;
    std::string origin_column_name;
    std::shared_ptr<TExpr> expr;
};

struct SchemaChangeParams {
    AlterTabletType alter_tablet_type;
    bool enable_unique_key_merge_on_write;
    std::vector<RowsetReaderSharedPtr> ref_rowset_readers;
    DeleteHandler* delete_handler = nullptr;
    std::unordered_map<std::string, AlterMaterializedViewParam> materialized_params_map;
    DescriptorTbl* desc_tbl = nullptr;
    ObjectPool pool;
    int32_t be_exec_version;
};

class SchemaChangeJob {
public:
    SchemaChangeJob(StorageEngine& local_storage_engine, const TAlterTabletReqV2& request,
                    const std::string& job_id);
    Status process_alter_tablet(const TAlterTabletReqV2& request);

    bool tablet_in_converting(int64_t tablet_id);

    static Status parse_request(const SchemaChangeParams& sc_params,
                                TabletSchema* base_tablet_schema, TabletSchema* new_tablet_schema,
                                BlockChanger* changer, bool* sc_sorting, bool* sc_directly);

private:
    std::unique_ptr<SchemaChange> _get_sc_procedure(const BlockChanger& changer, bool sc_sorting,
                                                    bool sc_directly) {
        if (sc_sorting) {
            return std::make_unique<VLocalSchemaChangeWithSorting>(
                    changer, config::memory_limitation_per_thread_for_schema_change_bytes,
                    _local_storage_engine);
        }

        if (sc_directly) {
            return std::make_unique<VSchemaChangeDirectly>(changer);
        }

        return std::make_unique<LinkedSchemaChange>();
    }

    Status _get_versions_to_be_changed(std::vector<Version>* versions_to_be_changed,
                                       RowsetSharedPtr* max_rowset);

    Status _do_process_alter_tablet(const TAlterTabletReqV2& request);

    Status _validate_alter_result(const TAlterTabletReqV2& request);

    Status _convert_historical_rowsets(const SchemaChangeParams& sc_params,
                                       int64_t* real_alter_version);

    // Initialization Settings for creating a default value
    static Status _init_column_mapping(ColumnMapping* column_mapping,
                                       const TabletColumn& column_schema, const std::string& value);

    Status _calc_delete_bitmap_for_mow_table(int64_t alter_version);

    StorageEngine& _local_storage_engine;
    TabletSharedPtr _base_tablet;
    TabletSharedPtr _new_tablet;
    TabletSchemaSPtr _base_tablet_schema;
    TabletSchemaSPtr _new_tablet_schema;
    std::shared_mutex _mutex;
    std::unordered_set<int64_t> _tablet_ids_in_converting;
    std::set<std::string> _supported_functions;
    std::string _job_id;
};
} // namespace doris
