/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

    bool touches_index(const IndexMeta &index) const {
        for (auto &set_clause : set_clauses_) {
            for (auto &col : index.cols) {
                if (set_clause.lhs.col_name == col.name) {
                    return true;
                }
            }
        }
        return false;
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        // 写路径只需要表级 IX；具体冲突由扫描阶段获取的行级 X 锁处理。
        if (context_ != nullptr && context_->lock_mgr_ != nullptr && context_->txn_ != nullptr) {
            context_->lock_mgr_->lock_IX_on_table(context_->txn_, fh_->GetFd());
        }
        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);

            // 保存旧记录到 write_set，供 abort 回滚
            if (context_ != nullptr && context_->txn_ != nullptr) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *rec));
            }
            // 从索引中删除旧的key
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                if (!touches_index(index)) {
                    continue;
                }
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                char stack_key[512];
                char *key = stack_key;
                std::unique_ptr<char[]> heap_key;
                if (index.col_tot_len > 512) {
                    heap_key = std::make_unique<char[]>(index.col_tot_len);
                    key = heap_key.get();
                }
                int offset = 0;
                for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                    memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->delete_entry(key, context_->txn_);
            }
            // 更新记录
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                auto &val = set_clause.rhs;
                if (col->type != val.type) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
                }
                if (!val.raw) {
                    val.init_raw(col->len);
                }
                char *dst = rec->data + col->offset;
                if (set_clause.op == SetOpType::ASSIGN) {
                    memcpy(dst, val.raw->data, col->len);
                } else if (col->type == TYPE_INT) {
                    int cur = *(int *)dst;
                    int delta = *(int *)val.raw->data;
                    int next = set_clause.op == SetOpType::PLUS ? cur + delta : cur - delta;
                    memcpy(dst, &next, sizeof(int));
                } else if (col->type == TYPE_FLOAT) {
                    float cur = *(float *)dst;
                    float delta = *(float *)val.raw->data;
                    float next = set_clause.op == SetOpType::PLUS ? cur + delta : cur - delta;
                    memcpy(dst, &next, sizeof(float));
                } else {
                    throw IncompatibleTypeError(coltype2str(col->type), "incremental update");
                }
            }
            fh_->update_record(rid, rec->data, context_);
            // 向索引中插入新的key
            for (size_t i = 0; i < tab_.indexes.size(); ++i) {
                auto &index = tab_.indexes[i];
                if (!touches_index(index)) {
                    continue;
                }
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                char stack_key[512];
                char *key = stack_key;
                std::unique_ptr<char[]> heap_key;
                if (index.col_tot_len > 512) {
                    heap_key = std::make_unique<char[]>(index.col_tot_len);
                    key = heap_key.get();
                }
                int offset = 0;
                for (size_t j = 0; j < (size_t)index.col_num; ++j) {
                    memcpy(key + offset, rec->data + index.cols[j].offset, index.cols[j].len);
                    offset += index.cols[j].len;
                }
                ih->insert_entry(key, rid, context_->txn_);
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
