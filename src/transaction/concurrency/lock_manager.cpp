/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"
#include "transaction/txn_defs.h"

#include <chrono>

namespace {
constexpr auto LOCK_YOUNG_TXN_WAIT = std::chrono::milliseconds(2);
}

bool LockManager::lock_shared_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IS_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction* txn, const Rid& rid, int tab_fd) {
    lock_IX_on_table(txn, tab_fd);
    return lock(txn, LockDataId(tab_fd, rid, LockDataType::RECORD), LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction* txn, int tab_fd) {
    return lock_table(txn, tab_fd, LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction* txn, int tab_fd) {
    return lock_table(txn, tab_fd, LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::lock_shared_on_table(Transaction* txn, int tab_fd) {
    return lock_table(txn, tab_fd, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction* txn, int tab_fd) {
    return lock_table(txn, tab_fd, LockMode::EXLUCSIVE);
}

bool LockManager::lock_table(Transaction* txn, int tab_fd, LockMode mode) {
    return lock(txn, LockDataId(tab_fd, LockDataType::TABLE), mode);
}

bool LockManager::is_compatible(LockMode granted, LockMode requested) const {
    if (granted == LockMode::INTENTION_SHARED) {
        return requested != LockMode::EXLUCSIVE;
    }
    if (granted == LockMode::INTENTION_EXCLUSIVE) {
        return requested == LockMode::INTENTION_SHARED ||
               requested == LockMode::INTENTION_EXCLUSIVE;
    }
    if (granted == LockMode::SHARED) {
        return requested == LockMode::INTENTION_SHARED ||
               requested == LockMode::SHARED;
    }
    if (granted == LockMode::S_IX) {
        return requested == LockMode::INTENTION_SHARED;
    }
    return false;
}

bool LockManager::is_compatible_with_granted(const LockRequestQueue& queue, txn_id_t txn_id,
                                             LockMode requested) const {
    for (const auto &req : queue.request_queue_) {
        if (!req.granted_ || req.txn_id_ == txn_id) {
            continue;
        }
        if (!is_compatible(req.lock_mode_, requested)) {
            return false;
        }
    }
    return true;
}

bool LockManager::should_abort_by_wait_die(const LockRequestQueue& queue, Transaction* txn,
                                           LockMode requested) const {
    for (const auto &req : queue.request_queue_) {
        if (!req.granted_ || req.txn_id_ == txn->get_transaction_id()) {
            continue;
        }
        if (!is_compatible(req.lock_mode_, requested) &&
            txn->get_start_ts() >= req.start_ts_) {
            return true;
        }
    }
    return false;
}

LockManager::LockMode LockManager::upgrade_mode(LockMode held, LockMode requested) const {
    if (held == requested || held == LockMode::EXLUCSIVE) {
        return held;
    }
    if (requested == LockMode::EXLUCSIVE || held == LockMode::EXLUCSIVE) {
        return LockMode::EXLUCSIVE;
    }
    if ((held == LockMode::SHARED && requested == LockMode::INTENTION_EXCLUSIVE) ||
        (held == LockMode::INTENTION_EXCLUSIVE && requested == LockMode::SHARED) ||
        held == LockMode::S_IX || requested == LockMode::S_IX) {
        return LockMode::S_IX;
    }
    if (held == LockMode::INTENTION_SHARED && requested == LockMode::INTENTION_EXCLUSIVE) {
        return LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE && requested == LockMode::INTENTION_SHARED) {
        return LockMode::INTENTION_EXCLUSIVE;
    }
    if (held == LockMode::INTENTION_SHARED && requested == LockMode::SHARED) {
        return LockMode::SHARED;
    }
    if (held == LockMode::SHARED && requested == LockMode::INTENTION_SHARED) {
        return LockMode::SHARED;
    }
    return requested;
}

LockManager::GroupLockMode LockManager::recompute_group_lock_mode(const LockRequestQueue& queue) const {
    bool has_is = false, has_ix = false, has_s = false, has_x = false, has_six = false;
    for (const auto &req : queue.request_queue_) {
        if (!req.granted_) {
            continue;
        }
        if (req.lock_mode_ == LockMode::INTENTION_SHARED) has_is = true;
        else if (req.lock_mode_ == LockMode::INTENTION_EXCLUSIVE) has_ix = true;
        else if (req.lock_mode_ == LockMode::SHARED) has_s = true;
        else if (req.lock_mode_ == LockMode::EXLUCSIVE) has_x = true;
        else if (req.lock_mode_ == LockMode::S_IX) has_six = true;
    }
    if (has_x) return GroupLockMode::X;
    if (has_six || (has_s && has_ix)) return GroupLockMode::SIX;
    if (has_s) return GroupLockMode::S;
    if (has_ix) return GroupLockMode::IX;
    if (has_is) return GroupLockMode::IS;
    return GroupLockMode::NON_LOCK;
}

bool LockManager::lock(Transaction* txn, const LockDataId& lock_data_id, LockMode mode) {
    if (txn == nullptr) return true;
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(),
                                        AbortReason::LOCK_ON_SHIRINKING);
    }

    std::unique_lock<std::mutex> lk(latch_);
    auto &q = lock_table_[lock_data_id];

    LockRequest *self = nullptr;
    for (auto &req : q.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            self = &req;
            break;
        }
    }
    if (self != nullptr) {
        LockMode upgraded = upgrade_mode(self->lock_mode_, mode);
        if (upgraded == self->lock_mode_) {
            return true;
        }
        while (!is_compatible_with_granted(q, txn->get_transaction_id(), upgraded)) {
            if (should_abort_by_wait_die(q, txn, upgraded)) {
                q.cv_.wait_for(lk, LOCK_YOUNG_TXN_WAIT);
                if (!is_compatible_with_granted(q, txn->get_transaction_id(), upgraded) &&
                    should_abort_by_wait_die(q, txn, upgraded)) {
                    throw TransactionAbortException(txn->get_transaction_id(),
                                                    AbortReason::DEADLOCK_PREVENTION);
                }
            } else {
                q.cv_.wait(lk);
            }
        }
        self->lock_mode_ = upgraded;
        q.group_lock_mode_ = recompute_group_lock_mode(q);
        return true;
    }

    while (!is_compatible_with_granted(q, txn->get_transaction_id(), mode)) {
        if (should_abort_by_wait_die(q, txn, mode)) {
            q.cv_.wait_for(lk, LOCK_YOUNG_TXN_WAIT);
            if (!is_compatible_with_granted(q, txn->get_transaction_id(), mode) &&
                should_abort_by_wait_die(q, txn, mode)) {
                throw TransactionAbortException(txn->get_transaction_id(),
                                                AbortReason::DEADLOCK_PREVENTION);
            }
        } else {
            q.cv_.wait(lk);
        }
    }

    q.request_queue_.emplace_back(txn->get_transaction_id(), txn->get_start_ts(), mode);
    q.request_queue_.back().granted_ = true;
    q.group_lock_mode_ = recompute_group_lock_mode(q);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::unlock(Transaction* txn, LockDataId lock_data_id) {
    if (txn == nullptr) return true;

    std::unique_lock<std::mutex> lk(latch_);
    auto it = lock_table_.find(lock_data_id);
    if (it == lock_table_.end()) return true;
    auto &q = it->second;

    // 删除该事务的所有请求
    for (auto rit = q.request_queue_.begin(); rit != q.request_queue_.end();) {
        if (rit->txn_id_ == txn->get_transaction_id()) {
            rit = q.request_queue_.erase(rit);
        } else {
            ++rit;
        }
    }
    q.group_lock_mode_ = recompute_group_lock_mode(q);

    // 唤醒等待者
    q.cv_.notify_all();
    return true;
}

void LockManager::unlock_all(Transaction* txn) {
    if (txn == nullptr) return;

    std::unique_lock<std::mutex> lk(latch_);
    auto lock_set = txn->get_lock_set();
    for (const auto &lock_id : *lock_set) {
        auto it = lock_table_.find(lock_id);
        if (it == lock_table_.end()) continue;
        auto &q = it->second;

        // 删除该事务的所有请求
        for (auto rit = q.request_queue_.begin(); rit != q.request_queue_.end();) {
            if (rit->txn_id_ == txn->get_transaction_id()) {
                rit = q.request_queue_.erase(rit);
            } else {
                ++rit;
            }
        }
        q.group_lock_mode_ = recompute_group_lock_mode(q);

        // 唤醒等待者
        q.cv_.notify_all();
    }
}
